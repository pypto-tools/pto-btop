// SPDX-License-Identifier: Apache-2.0
#include "https.hpp"
#include "net/common.hpp"
#include "collector/linux.hpp"
#include <iostream>
#include <thread>

int run_https_agent(const std::string& path,volatile std::sig_atomic_t& stopped) {
	const auto settings=Net::config(path);
	Net::keys(settings,{"version","server","ca_file","token_file","interval_ms","mounts","disk_devices","network_interfaces"});
	Net::Client http(settings); Collector::Linux collector;
	Protocol::Subscription options; options.detail=true;
	auto encoded=Protocol::encode(options);
	for (const auto* key : {"interval_ms","mounts","disk_devices","network_interfaces"}) if (settings.contains(key)) encoded[key]=settings[key];
	options=Protocol::subscription(encoded);
	const auto instance=Net::random_id(); const auto hello=collector.hello(instance);
	std::string session; std::uint64_t seq=0;
	std::map<std::string,Protocol::Subscription> requests;
	int failures=0;
	auto work=[&](const Net::Json& response) {
		const auto& views=response.at("requests");
		if (not views.is_object() or views.size()>8) throw std::runtime_error("Invalid process request batch");
		std::map<std::string,Protocol::Subscription> next;
		for (const auto& [id,view] : views.items()) {
			Net::identifier(Net::Json{{"id",id}},"id");
			next.emplace(id,Protocol::subscription(view));
		}
		requests=std::move(next);
	};
	while (not stopped) {
		const auto start=Protocol::now_ms(); int delay=options.interval_ms;
		try {
			if (session.empty()) {
				const auto reply=http.request("POST","/api/v1/agent/session",{{"instance_id",instance},{"hello",hello}});
				session=reply.at("session_id").get<std::string>(); work(reply);
			}
			auto body=collector.sample_batch(options,requests);
			body["session_id"]=session; body["seq"]=++seq;
			body["wall_time"]=Protocol::utc_now(); body["interval_ms"]=options.interval_ms;
			work(http.request("POST","/api/v1/agent/snapshots",body));
			if (failures) std::cerr<<"btop-agent: HTTPS connection restored\n";
			failures=0;
		} catch (const Net::HttpError& e) {
			if (e.status==401 or e.status==403 or e.status==400 or e.status==413) { std::cerr<<"btop-agent: "<<e.what()<<"; check configuration\n"; return 1; }
			if (e.status==409) session.clear();
			++failures; delay=std::min(30000,1000*(1<<std::min(failures-1,5)));
			std::cerr<<"btop-agent: "<<e.what()<<"; retrying\n";
		} catch (const std::exception& e) {
			++failures; delay=std::min(30000,1000*(1<<std::min(failures-1,5)));
			std::cerr<<"btop-agent: "<<e.what()<<"; retrying\n";
		}
		const auto deadline=start+delay;
		while (not stopped and Protocol::now_ms()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	return 0;
}
