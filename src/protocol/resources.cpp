// SPDX-License-Identifier: Apache-2.0
#include "resources.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace Protocol {
namespace {
void require(bool condition) { if (not condition) throw std::runtime_error("Invalid resource snapshot"); }
bool natural(const Json& v, std::uint64_t maximum=1000000000) {
	return v.is_number_integer() and v>=0 and v<=maximum;
}
bool string(const Json& v, std::size_t maximum=256) { return v.is_string() and v.get_ref<const std::string&>().size()<=maximum; }
bool nullable_number(const Json& v, std::uint64_t maximum=1000000000) { return v.is_null() or natural(v,maximum); }
bool nullable_string(const Json& v) { return v.is_null() or string(v); }
Json owner(const Json& source, bool reservation) {
	if (source.is_null()) return nullptr;
	require(source.is_object());
	Json result;
	for (const auto* name : {"task_id","user"}) {
		require(nullable_string(source.at(name))); result[name]=source.at(name);
	}
	require(nullable_number(source.at("uid"),4294967295ULL)); result["uid"]=source.at("uid");
	if (reservation) {
		require(nullable_number(source.at("requested_devices"),256)); result["requested_devices"]=source.at("requested_devices");
	} else {
		require(nullable_string(source.at("started_at")) and nullable_number(source.at("elapsed_seconds"),1000000000000ULL));
		result["started_at"]=source.at("started_at"); result["elapsed_seconds"]=source.at("elapsed_seconds");
	}
	return result;
}
}

Json card_snapshot(const Json& source) {
	require(source.is_object() and source.at("schema_version").is_number_integer() and source.at("schema_version")==1);
	const auto status=source.at("status").get<std::string>();
	require(status=="ok" or status=="partial" or status=="unavailable");
	require(nullable_string(source.at("reason")) and source.at("inventory_complete").is_boolean());
	require(nullable_number(source.at("age_ms"),1000000000000ULL));
	Json result{{"schema_version",1},{"status",status},{"reason",source.at("reason")},
		{"inventory_complete",source.at("inventory_complete")},{"age_ms",source.at("age_ms")}};
	for (const auto* key : {"boot_id","observed_at"}) { require(nullable_string(source.at(key))); result[key]=source.at(key); }
	require(nullable_number(source.at("snapshot_seq"),9007199254740991ULL)); result["snapshot_seq"]=source.at("snapshot_seq");
	const auto& producer=source.at("producer");
	require(string(producer.at("name")) and nullable_string(producer.at("version")));
	result["producer"]={{"name",producer.at("name")},{"version",producer.at("version")}};
	require(source.value("cached",Json(false)).is_boolean()); result["cached"]=source.value("cached",false);
	require(string(source.at("external_detection"))); result["external_detection"]=source.at("external_detection");
	const auto& scheduler=source.at("scheduler");
	const auto state=scheduler.at("state").get<std::string>();
	require(state=="running" or state=="maintenance" or state=="stopped" or state=="unknown");
	Json scheduling{{"state",state}};
	for (const auto* key : {"mode","reason"}) { require(nullable_string(scheduler.at(key))); scheduling[key]=scheduler.at(key); }
	for (const auto* key : {"accepting_submissions","dispatching"}) {
		require(scheduler.at(key).is_null() or scheduler.at(key).is_boolean()); scheduling[key]=scheduler.at(key);
	}
	for (const auto* key : {"running_tasks","pending_tasks","max_concurrent"}) {
		require(nullable_number(scheduler.at(key))); scheduling[key]=scheduler.at(key);
	}
	result["scheduler"]=scheduling;
	const auto& devices=source.at("devices"); require(devices.is_array() and devices.size()<=256);
	std::map<std::string,std::uint64_t> counts{{"free",0},{"allocated",0},{"reserved",0},{"disabled",0},{"external",0},{"error",0},{"unknown",0}};
	std::set<std::string> ids; std::set<std::uint64_t> owners; bool owners_complete=true;
	result["devices"]=Json::array();
	for (const auto& device : devices) {
		require(string(device.at("id"),32)); const auto id=device.at("id").get<std::string>();
		require(not id.empty() and std::ranges::all_of(id,[](unsigned char c){return c>='0' and c<='9';}) and ids.insert(id).second);
		const auto kind=device.at("state").get<std::string>(); require(counts.contains(kind)); ++counts[kind];
		require(nullable_string(device.at("group_id")) and (device.at("pool_eligible").is_null() or device.at("pool_eligible").is_boolean()));
		Json item{{"id",id},{"state",kind},{"group_id",device.at("group_id")},{"pool_eligible",device.at("pool_eligible")},
			{"allocation",owner(device.at("allocation"),false)},{"reservation",owner(device.at("reservation"),true)}};
		const auto& reasons=device.at("reason_codes"); require(reasons.is_array() and reasons.size()<=32);
		for (const auto& reason : reasons) require(string(reason,128));
		item["reason_codes"]=reasons;
		require(string(device.at("health"))); item["health"]=device.at("health");
		if (kind=="free") require(item["pool_eligible"]==true and item["allocation"].is_null() and item["reservation"].is_null());
		if (kind=="reserved") require(item["allocation"].is_null() and item["reservation"].is_object());
		if (kind=="allocated") require(item["allocation"].is_object());
		if (item["allocation"].is_object()) {
			if (item["allocation"]["uid"].is_null()) owners_complete=false;
			else owners.insert(item["allocation"]["uid"].get<std::uint64_t>());
		} else if (kind=="unknown" or kind=="external") owners_complete=false;
		result["devices"].push_back(std::move(item));
	}
	const auto& summary=source.at("summary"); Json totals;
	for (const auto* key : {"total","free","allocated","reserved","disabled","external","error","unknown","occupying_users"}) {
		const auto& n=summary.at(key); require(nullable_number(n,256)); totals[key]=n;
		if (not n.is_null()) {
			if (std::string(key)=="total") require(n==devices.size());
			else if (std::string(key)=="occupying_users") { if (owners_complete) require(n==owners.size()); }
			else require(n==counts.at(key));
		}
		if (not source.at("inventory_complete").get<bool>()) require(n.is_null());
		if (status=="ok") require(not n.is_null());
	}
	require(status!="ok" or source.at("inventory_complete")==true);
	result["summary"]=totals;
	return result;
}

void validate_resource_group(const std::string& name, const Json& group) {
	require(group.is_object() and group.at("status").is_string());
	const auto status=group.at("status").get<std::string>();
	require(status=="ok" or status=="partial" or status=="warming_up" or status=="unavailable" or status=="timeout");
	require(natural(group.at("observed_monotonic_ms"),9007199254740991ULL) and natural(group.at("interval_ms"),60000));
	const auto& data=group.at("data");
	if (data.is_null()) { require(status!="ok"); return; }
	if (name=="card_usage") { usage_snapshot(data); return; }
	if (name=="cards") { card_snapshot(data); return; }
	require(name=="sessions" and data.is_object() and data.at("source")=="utmpx");
	require(natural(data.at("users"),4096) and natural(data.at("sessions"),4096) and natural(data.at("age_ms"),1000000000000ULL));
	const auto& items=data.at("items"); require(items.is_array() and items.size()<=4096);
	std::set<std::string> users; std::uint64_t sessions=0;
	for (const auto& item : items) {
		require(string(item.at("user"),256) and not item.at("user").get<std::string>().empty());
		require(users.insert(item.at("user").get<std::string>()).second);
		require(natural(item.at("sessions"),4096) and item.at("sessions")>0 and nullable_number(item.at("first_login_at"),1000000000000ULL));
		sessions+=item.at("sessions").get<std::uint64_t>();
	}
	require(data.at("users")==items.size() and data.at("sessions")==sessions);
}
}
