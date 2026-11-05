// SPDX-License-Identifier: Apache-2.0
#include "https.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace Cluster {
namespace {
void copy_metrics(Host& target,const Host& source) {
	// Display zero offsets belong to this viewer, but cannot outlive a counter reset.
	const auto& old_interfaces=metric_items(metric_data(target,"network"));
	const auto& new_interfaces=metric_items(metric_data(source,"network"));
	for (const auto& item : new_interfaces) for (const auto& old : old_interfaces)
		if (item.value("name",std::string{})==old.value("name",std::string{})) {
			bool reset=item.value("ifindex",0)!=old.value("ifindex",0);
			for (const auto* key : {"rx_bytes_total","tx_bytes_total"})
				if (item.contains(key) and old.contains(key) and item[key].is_number() and old[key].is_number() and item[key]<old[key]) reset=true;
			if (reset) target.network_offset.erase(item.value("name",std::string{}));
		}
	target.cpu=source.cpu; target.cores=source.cores; target.cpu_valid=source.cpu_valid;
	target.memory_valid=source.memory_valid; target.memory_total=source.memory_total; target.memory_used=source.memory_used;
	target.load=source.load; target.history=source.history; target.groups=source.groups; target.series=source.series;
	target.network_rx=source.network_rx; target.network_tx=source.network_tx; target.network_peak=source.network_peak;
	target.received_ms=source.received_ms; target.sample_time=source.sample_time;
	target.status=source.status; target.error=source.error;
	std::erase_if(target.network_offset,[&](const auto& entry){return not source.network_rx.contains(entry.first);});
}
void restore_history(Host& host,const Net::Json& value) {
	auto check=[](const Net::Json& series) {
		if (not series.is_object() or series.size()>8192) throw std::runtime_error("Invalid history map");
		for (const auto& [name,points] : series.items()) {
			if (name.size()>256 or not points.is_array() or points.size()>300) throw std::runtime_error("Invalid history series");
			for (const auto& point : points) if (not point.is_number_integer() or point<0 or point>1000000000000000000LL) throw std::runtime_error("Invalid history point");
		}
	};
	for (const auto* key : {"series","network_rx","network_tx"}) check(value.at(key));
	check(Net::Json{{"history",value.at("history")}});
	host.history=value.at("history").get<std::deque<long long>>();
	host.series=value.at("series").get<decltype(host.series)>();
	host.network_rx=value.at("network_rx").get<decltype(host.network_rx)>();
	host.network_tx=value.at("network_tx").get<decltype(host.network_tx)>();
	host.network_peak=value.at("network_peak").get<decltype(host.network_peak)>();
}
}
struct HttpsFeed::Impl {
	Net::Client http;
	std::vector<std::string> ids;
	std::vector<Host> cached,pending;
	std::vector<bool> ready;
	std::vector<Protocol::Subscription> desired, pending_requests;
	std::mutex mutex;
	std::atomic<bool> reconnect{false};
	std::jthread worker;
	explicit Impl(const Net::Json& settings) : http(settings) {}
	void publish(std::size_t i,const Protocol::Subscription& request) { std::lock_guard guard(mutex); pending[i]=cached[i]; pending_requests[i]=request; ready[i]=true; }
	void run(std::stop_token stop) {
		struct State { std::string session,view,key,sent; std::uint64_t seq=0,renew=0; bool bootstrap=true; };
		std::vector<State> states(ids.size());
		while (not stop.stop_requested()) {
			std::vector<Protocol::Subscription> requests;
			{ std::lock_guard guard(mutex); requests=desired; }
			if (reconnect.exchange(false)) for (auto& state : states) { state.bootstrap=true; state.renew=0; }
			for (std::size_t i=0; i<ids.size() and not stop.stop_requested(); ++i) {
				auto& state=states[i]; auto& host=cached[i];
				try {
					const auto encoded=Protocol::encode(requests[i]); const auto serialized=encoded.dump();
					if (requests[i].processes) {
						if (serialized!=state.sent or Protocol::now_ms()-state.renew>4000) {
							const auto reply=http.request("POST","/api/v1/views",{{"id",state.view},{"host_id",ids[i]},{"subscription",encoded}});
							state.view=reply.at("id").get<std::string>(); state.key=reply.at("key").get<std::string>(); state.sent=serialized; state.renew=Protocol::now_ms();
						}
					} else if (not state.view.empty()) {
						http.request("DELETE","/api/v1/views/"+state.view); state.view.clear(); state.key.clear(); state.sent.clear(); host.groups.erase("processes");
					}
					const auto suffix=state.view.empty() ? "" : "?view="+state.view;
					const auto packet=http.request("GET","/api/v1/hosts/"+ids[i]+"/snapshot"+suffix);
					const auto session=packet.at("session_id").get<std::string>(); const auto seq=packet.at("seq").get<std::uint64_t>();
					bool changed=false;
					if (session!=state.session) { clear_histories(host); state.session=session; state.seq=0; state.bootstrap=true; changed=true; }
					if (packet.at("snapshot").contains("groups")) {
						if (seq!=state.seq) {
							if (state.seq and seq>state.seq+1) state.bootstrap=true;
							host.proc_detail=requests[i].selected_pid!=0; host.selected_pid=requests[i].selected_pid; host.selected_start=requests[i].selected_start;
							ingest(host,packet.at("snapshot")); state.seq=seq; changed=true;
						} else if (packet.at("snapshot").at("groups").contains("processes")) {
							const auto& group=packet.at("snapshot").at("groups").at("processes");
							if (not host.groups.contains("processes") or host.groups["processes"]!=group) {host.groups["processes"]=group; changed=true;}
						}
						if (state.bootstrap) {
							const auto backfill=http.request("GET","/api/v1/hosts/"+ids[i]+"/history"+suffix);
							if (backfill.at("session_id")==session and backfill.at("seq")==seq) { restore_history(host,backfill.at("history")); state.bootstrap=false; changed=true; }
						}
					}
					const auto status=packet.at("status").get<std::string>();
					const auto error=status=="ONLINE" ? "" : "last sample "+(packet.at("age_ms").is_number() ? std::to_string(packet.at("age_ms").get<std::uint64_t>()/1000)+"s ago" : "unavailable");
					if (host.status!=status or host.error!=error) {host.status=status; host.error=error; changed=true;}
					if (status!="ONLINE") { state.bootstrap=true; }
					if (changed) publish(i,requests[i]);
				} catch (const Net::HttpError& e) {
					if (e.status==404) {state.view.clear(); state.key.clear(); state.sent.clear(); state.renew=0;}
					host.status=e.status==401 or e.status==403 ? "AUTH_REQUIRED" : "DISCONNECTED";
					host.error=e.what(); state.bootstrap=true; publish(i,requests[i]);
				} catch (const std::exception& e) {
					host.status="DISCONNECTED"; host.error=e.what(); state.bootstrap=true; publish(i,requests[i]);
				}
			}
			for (int i=0; i<10 and not stop.stop_requested() and not reconnect.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}
};
HttpsFeed::HttpsFeed(const std::string& path,std::vector<Host>& hosts) {
	const auto settings=Net::config(path);
	Net::keys(settings,{"version","server","ca_file","token_file"});
	impl_=std::make_unique<Impl>(settings);
	const auto discovery=impl_->http.request("GET","/api/v1/hosts"); const auto& items=discovery.at("hosts");
	if (not items.is_array() or items.empty() or items.size()>4) throw std::runtime_error("HTTPS hub must expose 1..4 hosts");
	for (const auto& item : items) {
		impl_->ids.push_back(Net::identifier(item,"id"));
		Host host; host.name=item.at("label").get<std::string>(); host.simulated=false;
		host.cpu_valid=host.memory_valid=false; host.status="CONNECTING"; hosts.push_back(std::move(host));
	}
	impl_->cached=impl_->pending=hosts; impl_->ready.resize(hosts.size(),false); impl_->desired.resize(hosts.size()); impl_->pending_requests.resize(hosts.size());
	impl_->worker=std::jthread([&](std::stop_token token){impl_->run(token);});
}
HttpsFeed::~HttpsFeed() { if (impl_) {impl_->worker.request_stop(); impl_->http.stop();} }
bool HttpsFeed::tick(std::vector<Host>& hosts,const std::vector<Protocol::Subscription>& requests) {
	std::lock_guard guard(impl_->mutex); impl_->desired=requests; bool changed=false;
	for (std::size_t i=0; i<hosts.size(); ++i) if (impl_->ready[i]) {
		copy_metrics(hosts[i],impl_->pending[i]); impl_->ready[i]=false; changed=true;
		if (Protocol::encode(impl_->pending_requests[i])!=Protocol::encode(requests[i])) hosts[i].groups.erase("processes");
		if (not hosts[i].proc_detail) move_process(hosts[i],0);
	}
	return changed;
}
void HttpsFeed::reconnect() { impl_->reconnect=true; impl_->http.stop(); }
}
