// SPDX-License-Identifier: Apache-2.0
#include "view.hpp"
#include "protocol/resources.hpp"
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <functional>
#include <numeric>

namespace Cluster {
using Protocol::Json;
const Json& metric_data(const Host& host, std::string_view name) {
	static const Json empty = Json::object();
	const auto group = host.groups.find(std::string(name));
	if (group == host.groups.end() or not group->is_object()) return empty;
	const auto value = group->find("data");
	return value != group->end() and value->is_object() ? *value : empty;
}
const Json& metric_items(const Json& value, const char* key) {
	static const Json empty = Json::array();
	const auto found = value.find(key);
	return found != value.end() and found->is_array() ? *found : empty;
}
std::string process_key(const Json& value) {
	return std::to_string(value.value("pid",0)) + ":" + std::to_string(value.value("start_time_ticks",std::uint64_t{}));
}
namespace {
double number(const Json& value, const char* key, double fallback = -1) {
	const auto found = value.find(key);
	return found != value.end() and found->is_number() ? found->get<double>() : fallback;
}
void append(std::deque<long long>& history, double value, double maximum = 1e18) {
	if (not std::isfinite(value) or value < 0 or value > maximum) history.clear();
	else history.push_back(std::llround(value));
	while (history.size() > 300) history.pop_front();
}
}
namespace {
std::string count_text(const Json& value, const char* key) {
	const auto found=value.find(key);
	return found!=value.end() and found->is_number_integer() ? std::to_string(found->get<std::uint64_t>()) : "N/A";
}
}
std::string resource_status(const Host& host,const std::string& name) {
	if (host.status!="ONLINE") return host.status;
	const auto found=host.groups.find(name);
	if (found==host.groups.end()) return "N/A";
	const auto status=found->value("status",std::string{"unavailable"});
	const auto& data=metric_data(host,name);
	if (data.empty()) return status=="warming_up" ? "warming_up" : status;
	const auto age=number(data,"age_ms");
	const auto now=Protocol::now_ms();
	const auto elapsed=host.simulated or now<host.received_ms ? 0 : now-host.received_ms;
	if (age<0 or age+elapsed>15000 or data.value("cached",false)) return "STALE";
	if (status!="ok" and status!="partial") return status;
	if (name=="cards") {
		if (data.at("status")=="unavailable") return "unavailable";
		if (not data.at("inventory_complete").get<bool>()) return "UNKNOWN";
		const auto state=data.at("scheduler").at("state").get<std::string>();
		if (state!="running") return state=="maintenance" ? "MAINTENANCE" : state=="stopped" ? "STOPPED" : "UNKNOWN";
	}
	return status;
}
std::string card_summary(const Host& host,bool compact) {
	const auto state=resource_status(host,"cards");
	if (state!="ok" and state!="partial") return compact ? "--" : "NPU "+state;
	const auto& counts=metric_data(host,"cards").at("summary");
	const auto free=count_text(counts,"free"), total=count_text(counts,"total");
	if (compact) return free+"/"+total;
	return "NPU free "+free+"/"+total+" held "+count_text(counts,"allocated")+" res "+count_text(counts,"reserved")+" off "+count_text(counts,"disabled")+(state=="partial" ? " [partial]" : "");
}
std::string queue_summary(const Host& host) {
	const auto state=resource_status(host,"cards");
	if (state!="ok" and state!="partial") return "TASKS "+state;
	const auto& queue=metric_data(host,"cards").at("scheduler");
	return "TASKS run "+count_text(queue,"running_tasks")+" / limit "+count_text(queue,"max_concurrent")+" / wait "+count_text(queue,"pending_tasks")+" / owners "+count_text(metric_data(host,"cards").at("summary"),"occupying_users")+(queue.at("dispatching")==false ? " [paused]" : "");
}
std::string session_summary(const Host& host,bool compact) {
	const auto state=resource_status(host,"sessions");
	if (state!="ok" and state!="partial") return compact ? "--/--" : "users "+state;
	const auto& sessions=metric_data(host,"sessions");
	const auto suffix=state=="partial" ? " [partial]" : "";
	return compact ? count_text(sessions,"users")+"/"+count_text(sessions,"sessions") : "users "+count_text(sessions,"users")+" / sessions "+count_text(sessions,"sessions")+suffix;
}
int card_panel_height(const Host& host,int terminal_height) {
	const int cpu_height=std::max(7,((terminal_height-2)*32+50)/100);
	const int remaining=terminal_height-cpu_height-3;
	if (remaining<18 and host.cards_focus) return remaining;
	return host.cards_folded or remaining<18 ? 3 : std::clamp(remaining/2,8,15);
}
int card_visible_rows(const Host& host,int panel_height) {
	return std::max(1,panel_height-5-(host.card_detail and panel_height>=11 ? 3 : 0));
}
void move_card(Host& host,int delta) {
	const auto& items=metric_items(metric_data(host,"cards"),"devices");
	if (items.empty()) { host.selected_card.clear(); return; }
	int selected=0;
	for (std::size_t i=0; i<items.size(); ++i) if (items[i].at("id")==host.selected_card) selected=i;
	selected=std::clamp(selected+delta,0,int(items.size())-1);
	host.selected_card=items[selected].at("id").get<std::string>();
}
bool filter_card_owner(Host& host) {
	const auto state=resource_status(host,"cards");
	if (state!="ok" and state!="partial") return false;
	for (const auto& card : metric_items(metric_data(host,"cards"),"devices")) {
		if (card.at("id")!=host.selected_card or card.at("state")!="allocated") continue;
		const auto& allocation=card.at("allocation");
		if (not allocation.is_object() or not allocation.at("uid").is_number_integer()) return false;
		host.filter="uid:"+std::to_string(allocation.at("uid").get<std::uint32_t>());
		host.cards_focus=false; host.proc_detail=false; host.proc_page=0;
		return true;
	}
	return false;
}

std::vector<ProcessRow> process_rows(const Host& host) {
	const auto& items = metric_items(metric_data(host,"processes"));
	std::vector<std::size_t> order(items.size());
	std::iota(order.begin(),order.end(),0);
	std::ranges::sort(order,[&](auto left, auto right) {
		const auto& a = items[left]; const auto& b = items[right];
		const auto field = host.sort == "cpu" ? "cpu_pct" : host.sort == "rss" ? "rss_bytes" : host.sort.c_str();
		int comparison;
		if (host.sort == "name" or host.sort == "user") comparison = a.value(field,std::string{}).compare(b.value(field,std::string{}));
		else { const auto av=number(a,field), bv=number(b,field); comparison=av<bv ? -1 : av>bv ? 1 : 0; }
		if (comparison == 0) return number(a,"pid") < number(b,"pid");
		const bool ascending = (host.sort == "pid" or host.sort == "name" or host.sort == "user") != host.reverse;
		return ascending ? comparison < 0 : comparison > 0;
	});
	std::vector<ProcessRow> rows;
	if (not host.proc_tree) { for (auto index : order) rows.push_back({index}); return rows; }
	std::map<int,std::size_t> indices;
	for (auto index : order) indices[items[index].value("pid",0)] = index;
	std::map<std::size_t,std::vector<std::size_t>> children;
	std::vector<std::size_t> roots;
	for (auto index : order) {
		const int parent=items[index].value("ppid",0);
		if (indices.contains(parent) and indices[parent] != index) children[indices[parent]].push_back(index);
		else roots.push_back(index);
	}
	std::set<std::size_t> visited;
	std::function<void(std::size_t,int,bool)> visit = [&](auto index, int depth, bool hidden) {
		if (not visited.insert(index).second) return;
		if (not hidden) rows.push_back({index,std::min(depth,8),not children[index].empty()});
		const bool hide_children = hidden or host.collapsed.contains(process_key(items[index]));
		for (auto child : children[index]) visit(child,depth+1,hide_children);
	};
	for (auto root : roots) visit(root,0,false);
	// Malformed/cyclic or truncated parent graphs still produce each returned PID once.
	for (auto index : order) if (not visited.contains(index)) visit(index,0,false);
	return rows;
}
void move_process(Host& host, int delta) {
	const auto rows=process_rows(host);
	const auto& items=metric_items(metric_data(host,"processes"));
	if (rows.empty()) return;
	int selected=0;
	for (std::size_t i=0; i<rows.size(); ++i)
		if (items[rows[i].index].value("pid",0)==host.selected_pid and items[rows[i].index].value("start_time_ticks",std::uint64_t{})==host.selected_start) selected=i;
	selected=std::clamp(selected+delta,0,int(rows.size())-1);
	const auto& item=items[rows[selected].index];
	host.selected_pid=item.value("pid",0); host.selected_start=item.value("start_time_ticks",std::uint64_t{});
}
void toggle_process(Host& host) {
	const auto key=std::to_string(host.selected_pid)+":"+std::to_string(host.selected_start);
	if (not host.collapsed.erase(key)) host.collapsed.insert(key);
}
void clear_histories(Host& host) {
	host.history.clear(); host.series.clear(); host.network_rx.clear(); host.network_tx.clear();
	host.network_peak.clear(); host.network_offset.clear();
}
std::string safe_text(std::string_view input, int columns) {
	std::string out;
	std::mbstate_t state{};
	for (std::size_t i = 0; i < input.size() and columns > 0;) {
		wchar_t wc;
		auto n = std::mbrtowc(&wc, input.data()+i, input.size()-i, &state);
		if (n == std::size_t(-1) or n == std::size_t(-2) or n == 0) {
			out += '?'; ++i; --columns; state = {}; continue;
		}
		if (wc < 32 or (wc >= 0x7f and wc <= 0x9f) or (wc >= 0x202a and wc <= 0x202e) or (wc >= 0x2066 and wc <= 0x2069)) {
			out += '?'; --columns; i += n; continue;
		}
		int width = wcwidth(wc);
		if (width < 0) { out += '?'; --columns; }
		else if (width <= columns) { out.append(input.substr(i,n)); columns -= width; }
		else break;
		i += n;
	}
	return out;
}

void ingest(Host& host, const Json& snapshot) {
	const auto& groups=snapshot.at("groups");
	if (not groups.is_object()) throw std::runtime_error("Invalid metric groups");
	for (const auto& [name,group] : groups.items()) {
		if (name=="sessions" or name=="cards") Protocol::validate_resource_group(name,group);
		if (not group.is_object() or not group.contains("data") or not group.contains("status") or not group["status"].is_string()) throw std::runtime_error("Invalid metric group");
		if (metric_items(group["data"]).size()>2048) throw std::runtime_error("Too many metric items");
	}
	host.groups=groups;
	move_card(host,0);
	const auto& cpu=metric_data(host,"cpu"); const auto& mem=metric_data(host,"memory");
	const auto usage=number(cpu,"usage_pct");
	host.cpu_valid=usage>=0 and usage<=100; host.cpu=host.cpu_valid ? usage : 0;
	host.cores=int(std::clamp(number(cpu,"online_cores",0),0.0,1048576.0));
	const auto& load=metric_items(cpu,"load_avg");
	for (std::size_t i=0; i<3; ++i) host.load[i]=i<load.size() and load[i].is_number() ? load[i].get<double>() : 0;
	const auto total=number(mem,"total_bytes");
	host.memory_valid=total>0 and total<1e18;
	host.memory_total=host.memory_valid ? std::uint64_t(total) : 0;
	host.memory_used=std::uint64_t(std::clamp(number(mem,"used_bytes",0),0.0,double(host.memory_total)));
	append(host.history,usage,100);
	std::set<std::string> active;
	auto save=[&](std::string key,double value) { active.insert(key); append(host.series[key],value); };
	for (auto field : {"usage_pct","user_pct","system_pct","iowait_pct"}) save(field,number(cpu,field));
	for (const auto& core : metric_items(cpu,"cores")) save("core:"+std::to_string(core.value("id",0)),number(core,"usage_pct"));
	for (auto name : {"used","available","cached","free","swap_used"}) {
		const auto capacity=std::string(name)=="swap_used" ? number(mem,"swap_total_bytes") : total;
		save("mem:"+std::string(name),capacity>0 ? 100*number(mem,(std::string(name)+"_bytes").c_str())/capacity : 0);
	}
	for (const auto& disk : metric_items(metric_data(host,"disks")))
		for (auto field : {"read_bytes_per_sec","write_bytes_per_sec","activity_pct"}) save("disk:"+disk.value("id",std::string{})+":"+field,number(disk,field));
	std::set<std::string> active_interfaces;
	for (const auto& item : metric_items(metric_data(host,"network"))) {
		const auto name=item.value("name",std::string{}); active_interfaces.insert(name);
		const auto identity="netid:"+name;
		const auto index=number(item,"ifindex",0);
		if (host.series.contains(identity) and not host.series[identity].empty() and host.series[identity].back()!=index) {
			host.network_rx.erase(name); host.network_tx.erase(name); host.network_peak.erase(name); host.network_offset.erase(name);
		}
		save(identity,index);
		if (number(item,"rx_bytes_per_sec")<0 or number(item,"tx_bytes_per_sec")<0) {
			host.network_peak.erase(name); host.network_offset.erase(name);
		}
		append(host.network_rx[name],number(item,"rx_bytes_per_sec"),1e15);
		append(host.network_tx[name],number(item,"tx_bytes_per_sec"),1e15);
		for (int i=0; i<2; ++i) host.network_peak[name][i]=std::max(host.network_peak[name][i],number(item,i ? "tx_bytes_per_sec" : "rx_bytes_per_sec"));
	}
	std::erase_if(host.network_rx,[&](const auto& entry){return not active_interfaces.contains(entry.first);});
	std::erase_if(host.network_tx,[&](const auto& entry){return not active_interfaces.contains(entry.first);});
	std::erase_if(host.network_peak,[&](const auto& entry){return not active_interfaces.contains(entry.first);});
	std::erase_if(host.network_offset,[&](const auto& entry){return not active_interfaces.contains(entry.first);});
	std::set<std::string> process_keys;
	auto record_process=[&](const Json& process) {
		const auto key=process_key(process); process_keys.insert(key);
		save("proc:"+key,number(process,"cpu_pct")); save("rss:"+key,number(process,"rss_bytes"));
	};
	const auto& processes=metric_data(host,"processes");
	for (const auto& process : metric_items(processes)) record_process(process);
	const auto chosen=processes.find("selected");
	if (chosen!=processes.end() and chosen->is_object() and chosen->value("detail_status",std::string{})=="ok" and not process_keys.contains(process_key(*chosen))) record_process(*chosen);
	std::erase_if(host.series,[&](const auto& entry){return not active.contains(entry.first);});
	std::erase_if(host.collapsed,[&](const auto& key){return not process_keys.contains(key);});
	const auto selected_key=std::to_string(host.selected_pid)+":"+std::to_string(host.selected_start);
	if (not host.proc_detail and not process_keys.contains(selected_key)) move_process(host,0);
	const auto wall_time=snapshot.value("wall_time",std::string{});
	host.sample_time=wall_time.size()>=19 ? safe_text(wall_time.substr(11,8),8) : "";
	host.received_ms=Protocol::now_ms(); host.status="ONLINE"; host.error.clear();
}
}
