// SPDX-License-Identifier: Apache-2.0
#include "cluster_version.hpp"
#include "net/common.hpp"
#include "protocol/resources.hpp"
#include "cluster/view.hpp"
#include <httplib/httplib.h>
#include <csignal>
#include <arpa/inet.h>
#include <clocale>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using Net::Json;
volatile std::sig_atomic_t stopped=0;
void stop(int) { stopped=1; }
struct Machine {
	std::string id,label,credential,session,instance;
	Json hello=Json::object(),snapshot=Json::object();
	std::uint64_t seq=0,received=0;
	int interval=2000;
	Cluster::Host metrics;
};
struct Viewer { std::string id,credential; std::set<std::string> hosts; };
struct View {
	std::string owner,host,key;
	Json subscription,result=Json::object();
	std::uint64_t touched=0;
	Cluster::Host metrics;
};
std::string status(const Machine& machine) {
	if (not machine.received) return "CONNECTING";
	const auto age=Protocol::now_ms()-machine.received;
	return age>std::uint64_t(std::max(10000,machine.interval*5)) ? "DISCONNECTED" : age>std::uint64_t(std::max(6000,machine.interval*3)) ? "STALE" : "ONLINE";
}
Json history(const Cluster::Host& host) {
	return {{"history",host.history},{"series",host.series},{"network_rx",host.network_rx},{"network_tx",host.network_tx},{"network_peak",host.network_peak}};
}
class Hub {
	std::mutex mutex_;
	std::map<std::string,Machine> machines_;
	std::vector<Viewer> viewers_;
	std::map<std::string,View> views_;
	std::string epoch_=Net::random_id();
	std::string credential(const httplib::Request& request) {
		const auto header=request.get_header_value("Authorization");
		if (not header.starts_with("Bearer ") or header.size()>263) throw Net::HttpError(401,"Authentication required");
		return Net::digest(header.substr(7));
	}
	Machine& agent(const httplib::Request& request) {
		const auto hash=credential(request);
		for (auto& [id,machine] : machines_) if (Net::equal(hash,machine.credential)) return machine;
		throw Net::HttpError(403,"Agent credential required");
	}
	const Viewer& viewer(const httplib::Request& request) {
		const auto hash=credential(request);
		for (const auto& user : viewers_) if (Net::equal(hash,user.credential)) return user;
		throw Net::HttpError(403,"Viewer credential required");
	}
	void expire() { std::erase_if(views_,[](const auto& entry){return Protocol::now_ms()-entry.second.touched>15000;}); }
	Json demands(const std::string& host) {
		Json out=Json::object();
		for (const auto& [id,view] : views_) if (view.host==host) out[view.key]=view.subscription;
		return out;
	}
public:
	explicit Hub(const Json& settings) {
		Net::keys(settings,{"version","listen","port","cert_file","key_file","hosts","viewers"});
		const auto& hosts=settings.at("hosts"); const auto& users=settings.at("viewers");
		if (not hosts.is_array() or hosts.empty() or hosts.size()>4 or not users.is_array() or users.empty() or users.size()>16) throw std::runtime_error("Configure 1..4 hosts and 1..16 viewers");
		std::set<std::string> credentials,ids;
		for (auto item : hosts) {
			Net::keys(item,{"id","label","token_file"});
			Machine machine; machine.id=Net::identifier(item,"id"); machine.label=item.value("label",machine.id);
			if (machine.label.empty() or machine.label.size()>128 or machines_.contains(machine.id)) throw std::runtime_error("Invalid or duplicate host");
			item["_directory"]=settings["_directory"];
			machine.credential=Net::digest(Net::secret(Net::file_path(item,"token_file")));
			if (not credentials.insert(machine.credential).second) throw std::runtime_error("Credentials must be distinct");
			machine.metrics.simulated=false; machine.metrics.name=machine.label;
			machines_.emplace(machine.id,std::move(machine));
		}
		for (auto item : users) {
			Net::keys(item,{"id","hosts","token_file"});
			Viewer user; user.id=Net::identifier(item,"id");
			if (not ids.insert(user.id).second) throw std::runtime_error("Duplicate viewer");
			item["_directory"]=settings["_directory"];
			user.credential=Net::digest(Net::secret(Net::file_path(item,"token_file")));
			if (not credentials.insert(user.credential).second) throw std::runtime_error("Credentials must be distinct");
			if (not item.at("hosts").is_array() or item.at("hosts").size()>4) throw std::runtime_error("Invalid viewer host list");
			for (const auto& id : item.at("hosts")) {
				const auto host=id.get<std::string>();
				if (not machines_.contains(host)) throw std::runtime_error("Viewer references unknown host");
				user.hosts.insert(host);
			}
			if (user.hosts.empty()) throw std::runtime_error("Viewer must have a host");
			viewers_.push_back(std::move(user));
		}
	}
	Json dispatch(const httplib::Request& request) {
		std::lock_guard guard(mutex_); expire();
		const auto& path=request.path;
		if (request.method=="POST" and path=="/api/v1/agent/session") {
			auto& machine=agent(request); const auto body=Net::parse_json(request.body);
			const auto instance=Net::identifier(body,"instance_id");
			const auto& hello=body.at("hello");
			if (hello.at("version")!=1 or hello.at("type")!="hello" or hello.at("host").at("os")!="linux") throw Net::HttpError(400,"Unsupported agent hello");
			if (machine.instance!=instance) {
				if (not machine.instance.empty() and machine.received and Protocol::now_ms()-machine.received<std::uint64_t(machine.interval*3)) throw Net::HttpError(409,"Another agent is active for this host");
				machine.instance=instance; machine.session=Net::random_id(); machine.seq=machine.received=0;
				machine.snapshot=Json::object(); Cluster::clear_histories(machine.metrics);
				for (auto& [id,view] : views_) if (view.host==machine.id) { view.result=Json::object(); Cluster::clear_histories(view.metrics); }
			}
			machine.hello=hello;
			return {{"host_id",machine.id},{"session_id",machine.session},{"requests",demands(machine.id)},{"metric_groups",{"sessions","cards"}}};
		}
		if (request.method=="POST" and path=="/api/v1/agent/snapshots") {
			auto& machine=agent(request); const auto body=Net::parse_json(request.body);
			if (machine.session.empty() or body.at("session_id")!=machine.session) throw Net::HttpError(409,"Register agent session first");
			if (not body.at("seq").is_number_unsigned() or body.at("seq")<=machine.seq) throw Net::HttpError(409,"Sequence must increase");
			const auto& interval=body.at("interval_ms");
			if (not interval.is_number_integer() or interval<1000 or interval>10000) throw Net::HttpError(400,"Invalid sampling interval");
			if (not body.at("wall_time").is_string() or body.at("wall_time").get_ref<const std::string&>().size()>64) throw Net::HttpError(400,"Invalid sample timestamp");
			const auto& groups=body.at("groups");
			for (const auto* name : {"cpu","memory","filesystems","disks","network"}) if (not groups.contains(name)) throw Net::HttpError(400,"Missing metric group");
			if (groups.size()>7 or Cluster::metric_items(groups.at("cpu").at("data"),"cores").size()>2048) throw Net::HttpError(400,"Invalid metric groups");
			for (const auto& [name,group] : groups.items()) {
				if (name=="sessions" or name=="cards") Protocol::validate_resource_group(name,group);
				else if (name!="cpu" and name!="memory" and name!="filesystems" and name!="disks" and name!="network") throw Net::HttpError(400,"Unknown metric group");
			}
			auto next=machine.metrics;
			const auto gap=machine.received and Protocol::now_ms()-machine.received>std::uint64_t(std::max(6000,machine.interval*3));
			if (gap) Cluster::clear_histories(next);
			Cluster::ingest(next,body);
			const auto& results=body.at("process_views");
			if (not results.is_object() or results.size()>8) throw Net::HttpError(400,"Too many process views");
			std::map<std::string,Cluster::Host> prepared;
			for (auto& [id,view] : views_) if (view.host==machine.id and results.contains(view.key)) {
				const auto& group=results.at(view.key);
				if (Cluster::metric_items(group.at("data")).size()>500) throw Net::HttpError(400,"Too many processes");
				auto metrics=view.metrics;
				if (gap) Cluster::clear_histories(metrics);
				Cluster::ingest(metrics,{{"groups",{{"processes",group}}}});
				prepared.emplace(id,std::move(metrics));
			}
			machine.metrics=std::move(next); machine.interval=interval.get<int>();
			machine.seq=body.at("seq").get<std::uint64_t>(); machine.received=Protocol::now_ms();
			machine.snapshot={{"groups",groups},{"wall_time",body.at("wall_time")}};
			for (auto& [id,metrics] : prepared) { auto& view=views_.at(id); view.metrics=std::move(metrics); view.result=results.at(view.key); }
			return {{"accepted",machine.seq},{"requests",demands(machine.id)}};
		}
		const auto& user=viewer(request);
		if (request.method=="GET" and path=="/api/v1/hosts") {
			Json hosts=Json::array();
			for (const auto& [id,machine] : machines_) if (user.hosts.contains(id)) hosts.push_back({{"id",id},{"label",machine.label},{"status",status(machine)},{"hello",machine.hello}});
			return {{"epoch",epoch_},{"hosts",hosts}};
		}
		if (request.method=="POST" and path=="/api/v1/views") {
			const auto body=Net::parse_json(request.body); const auto host=Net::identifier(body,"host_id");
			if (not user.hosts.contains(host)) throw Net::HttpError(403,"Host not authorized");
			auto sub=Protocol::subscription(body.at("subscription")); sub.detail=sub.processes=true; sub.limit=500;
			sub.mounts.clear(); sub.disks.clear(); sub.interfaces.clear(); sub.request_id=1;
			const auto normalized=Protocol::encode(sub);
			auto id=body.value("id",std::string{});
			if (id.empty()) {
				std::size_t count=0; for (const auto& [key,view] : views_) if (view.host==host) ++count;
				if (views_.size()>=16 or count>=8) throw Net::HttpError(429,"View limit reached");
				id=Net::random_id(); views_[id].owner=user.id; views_[id].host=host;
			} else if (not views_.contains(id)) throw Net::HttpError(404,"View expired");
			auto& view=views_.at(id);
			if (view.owner!=user.id or view.host!=host) throw Net::HttpError(403,"View not authorized");
			if (view.subscription!=normalized) { view.subscription=normalized; view.key=Net::random_id(); view.result=Json::object(); Cluster::clear_histories(view.metrics); }
			view.touched=Protocol::now_ms();
			return {{"id",id},{"key",view.key},{"lease_ms",15000}};
		}
		if (request.method=="DELETE" and path.starts_with("/api/v1/views/")) {
			const auto id=path.substr(14);
			if (views_.contains(id)) { if (views_.at(id).owner!=user.id) throw Net::HttpError(403,"View not authorized"); views_.erase(id); }
			return {{"deleted",true}};
		}
		if (request.method=="GET" and path.starts_with("/api/v1/hosts/")) {
			const auto rest=path.substr(14); const auto slash=rest.find('/');
			if (slash==std::string::npos) throw Net::HttpError(404,"Unknown endpoint");
			const auto id=rest.substr(0,slash), action=rest.substr(slash+1);
			if (not user.hosts.contains(id)) throw Net::HttpError(403,"Host not authorized");
			const auto& machine=machines_.at(id);
			const View* view=nullptr;
			const auto view_id=request.get_param_value("view");
			if (not view_id.empty()) {
				if (not views_.contains(view_id)) throw Net::HttpError(404,"View expired");
				view=&views_.at(view_id);
				if (view->owner!=user.id or view->host!=id) throw Net::HttpError(403,"View not authorized");
			}
			if (action=="history") {
				auto result=history(machine.metrics);
				if (view) for (const auto& [key,values] : view->metrics.series) if (key.starts_with("proc:") or key.starts_with("rss:")) result["series"][key]=values;
				return {{"session_id",machine.session},{"seq",machine.seq},{"history",result}};
			}
			if (action=="snapshot") {
				auto snapshot=machine.snapshot;
				if (snapshot.contains("groups") and view) snapshot["groups"]["processes"]=view->result.empty() ? Protocol::group({{"items",Json::array()}},0,0,"warming_up","Awaiting requested view") : view->result;
				return {{"session_id",machine.session},{"seq",machine.seq},{"status",status(machine)},{"age_ms",machine.received ? Json(Protocol::now_ms()-machine.received) : Json(nullptr)},
					{"snapshot",snapshot},{"view_key",view ? view->key : ""}};
			}
		}
		throw Net::HttpError(404,"Unknown endpoint");
	}
};
}
int main(int argc,char** argv) {
	try {
		std::setlocale(LC_ALL,"");
		if (argc==2 and std::string_view(argv[1])=="--version") { std::cout<<"pto-btop-server " BTOP_CLUSTER_VERSION "\n"; return 0; }
		if (argc==2 and std::string_view(argv[1])=="--help") { std::cout<<"btop-server --config server.json\nHTTPS metrics hub; TLS certificate and private token files required.\n"; return 0; }
		if (argc!=3 or std::string_view(argv[1])!="--config") throw std::runtime_error("Use --config server.json");
		const auto settings=Net::config(argv[2]); Hub hub(settings);
		const auto address=settings.at("listen").get<std::string>();
		in_addr ipv4{}; in6_addr ipv6{};
		const bool specific=(inet_pton(AF_INET,address.c_str(),&ipv4)==1 and ipv4.s_addr!=INADDR_ANY) or
			(inet_pton(AF_INET6,address.c_str(),&ipv6)==1 and not IN6_IS_ADDR_UNSPECIFIED(&ipv6));
		if (not specific) throw std::runtime_error("Bind a specific NetBird or loopback IP address");
		const auto& port=settings.at("port");
		if (not port.is_number_integer() or port<1 or port>65535) throw std::runtime_error("Invalid listen port");
		httplib::SSLServer server(Net::file_path(settings,"cert_file").c_str(),Net::file_path(settings,"key_file").c_str());
		if (not server.is_valid()) throw std::runtime_error("Cannot load HTTPS certificate/private key");
		server.new_task_queue=[] {return new httplib::ThreadPool(4,4,16);};
		server.set_read_timeout(3); server.set_write_timeout(3); server.set_keep_alive_timeout(1); server.set_keep_alive_max_count(10); server.set_payload_max_length(Net::max_body);
		server.set_default_headers({{"Cache-Control","no-store"},{"X-Content-Type-Options","nosniff"}});
		auto handle=[&](const httplib::Request& request,httplib::Response& response) {
			try {
				const auto content=hub.dispatch(request).dump(-1,' ',false,Json::error_handler_t::replace);
				if (content.size()>Net::max_body) throw Net::HttpError(413,"Response exceeds limit");
				response.set_content(content,"application/json");
			} catch (const Net::HttpError& e) { response.status=e.status; response.set_content(Json{{"error",e.what()}}.dump(),"application/json"); }
			catch (const std::exception&) { response.status=400; response.set_content("{\"error\":\"Invalid request\"}","application/json"); }
		};
		server.Get("/api/v1/.*",handle); server.Post("/api/v1/.*",handle); server.Delete("/api/v1/.*",handle);
		std::signal(SIGINT,stop); std::signal(SIGTERM,stop); std::signal(SIGPIPE,SIG_IGN);
		if (not server.bind_to_port(address,port.get<int>())) throw std::runtime_error("Cannot bind HTTPS listener");
		std::jthread monitor([&](std::stop_token token) { while (not token.stop_requested() and not stopped) std::this_thread::sleep_for(std::chrono::milliseconds(50)); server.stop(); });
		std::cout<<"btop-server listening on https://"<<address<<":"<<port.get<int>()<<std::endl;
		const bool okay=server.listen_after_bind(); monitor.request_stop();
		return okay or stopped ? 0 : 1;
	} catch (const std::exception& e) { std::cerr<<"btop-server: "<<e.what()<<'\n'; return 1; }
}
