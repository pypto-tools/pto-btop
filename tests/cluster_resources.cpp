// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "collector/resources.hpp"
#include "protocol/resources.hpp"
#include "protocol/process_filter.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <utmpx.h>

namespace {
using Protocol::Json;
struct ResourceFixture {
	std::filesystem::path root;
	ResourceFixture() { char path[]="/tmp/pto-resources-test-XXXXXX"; root=mkdtemp(path); }
	~ResourceFixture() { std::filesystem::remove_all(root); }
	void sessions() {
		std::ofstream file(root/"utmp",std::ios::binary);
		for (auto [name,type,stamp] : {std::tuple{"alice",USER_PROCESS,1000}, {"bob",USER_PROCESS,2000}, {"alice",USER_PROCESS,3000}, {"LOGIN",LOGIN_PROCESS,4000}}) {
			utmpx entry{}; entry.ut_type=type; entry.ut_tv.tv_sec=stamp;
			std::strncpy(entry.ut_user,name,sizeof(entry.ut_user)-1);
			file.write(reinterpret_cast<char*>(&entry),sizeof(entry));
		}
	}
	void script(const std::string& body) {
		std::ofstream(root/"monitor")<<"#!/bin/sh\n"<<body<<'\n';
		std::filesystem::permissions(root/"monitor",std::filesystem::perms::owner_all);
	}
};
Json snapshot() {
	Json held{{"task_id","task-a"},{"user","alice"},{"uid",1000},{"started_at","2026-01-01T00:00:00Z"},{"elapsed_seconds",100}};
	Json reservation{{"task_id","task-b"},{"user","bob"},{"uid",1001},{"requested_devices",2}};
	return {{"schema_version",1},{"producer",{{"name","pto-task"},{"version","test"}}},{"boot_id","test-boot"},{"snapshot_seq",42},
		{"observed_at","2026-01-01T00:01:40Z"},{"age_ms",100},{"status","ok"},{"reason",nullptr},{"inventory_complete",true},{"external_detection","unsupported"},
		{"scheduler",{{"state","running"},{"mode","pool_aware_reservation"},{"accepting_submissions",true},{"dispatching",true},{"running_tasks",1},{"pending_tasks",1},{"max_concurrent",8},{"reason",nullptr}}},
		{"devices",Json::array({
			{{"id","2"},{"group_id","g0"},{"pool_eligible",true},{"state","free"},{"reason_codes",Json::array()},{"allocation",nullptr},{"reservation",nullptr},{"health","unknown"}},
			{{"id","5"},{"group_id","g0"},{"pool_eligible",true},{"state","allocated"},{"reason_codes",Json::array({"allocated","reservation_pool"})},{"allocation",held},{"reservation",reservation},{"health","unknown"}},
			{{"id","9"},{"group_id","g0"},{"pool_eligible",true},{"state","reserved"},{"reason_codes",Json::array({"reservation_pool"})},{"allocation",nullptr},{"reservation",reservation},{"health","unknown"}}
		})},{"summary",{{"total",3},{"free",1},{"allocated",1},{"reserved",1},{"disabled",0},{"external",0},{"error",0},{"unknown",0},{"occupying_users",1}}}};
}
Json collected(Collector::Resources& collector) {
	const auto end=Protocol::now_ms()+4000;
	while (Protocol::now_ms()<end) {
		auto groups=collector.sample();
		if (groups["cards"]["status"]!="warming_up") return groups;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	throw std::runtime_error("Resource worker did not return");
}
}

TEST(resources, login_accounts_are_deduplicated_and_missing_is_not_zero) {
	ResourceFixture f; f.sessions();
	const auto group=Collector::login_sessions(f.root/"utmp");
	Protocol::validate_resource_group("sessions",group);
	EXPECT_EQ(group["status"],"ok"); EXPECT_EQ(group["data"]["users"],2); EXPECT_EQ(group["data"]["sessions"],3);
	EXPECT_EQ(group["data"]["items"][0]["sessions"],2); EXPECT_EQ(group["data"]["items"][0]["first_login_at"],1000);
	EXPECT_EQ(Collector::login_sessions(f.root/"missing")["status"],"unavailable");
	std::ofstream(f.root/"empty");
	EXPECT_EQ(Collector::login_sessions(f.root/"empty")["data"]["users"],0);
	std::ofstream(f.root/"utmp",std::ios::app)<<'x';
	EXPECT_EQ(Collector::login_sessions(f.root/"utmp")["status"],"partial");
}
TEST(resources, card_validation_preserves_reservations_and_rejects_false_availability) {
	auto value=snapshot(); value["private_extra"]="must not propagate";
	const auto normalized=Protocol::card_snapshot(value);
	EXPECT_FALSE(normalized.contains("private_extra"));
	EXPECT_EQ(normalized["devices"][1]["allocation"]["user"],"alice");
	EXPECT_EQ(normalized["devices"][1]["reservation"]["user"],"bob");
	EXPECT_EQ(normalized["devices"][2]["id"],"9");
	auto bad=value; bad["summary"]["free"]=2; EXPECT_THROW(Protocol::card_snapshot(bad),std::exception);
	bad=value; bad["devices"][0]["reservation"]=value["devices"][2]["reservation"]; EXPECT_THROW(Protocol::card_snapshot(bad),std::exception);
	bad=value; bad["devices"][1]["id"]="2"; EXPECT_THROW(Protocol::card_snapshot(bad),std::exception);
	bad=value; bad["inventory_complete"]=false; EXPECT_THROW(Protocol::card_snapshot(bad),std::exception);
	bad=value; bad["schema_version"]=2; EXPECT_THROW(Protocol::card_snapshot(bad),std::exception);
	bad=value; bad["devices"][0]["state"]="mystery"; EXPECT_THROW(Protocol::card_snapshot(bad),std::exception);
}
TEST(resources, monitor_worker_is_async_and_cache_age_does_not_reset) {
	ResourceFixture f; f.sessions();
	f.script("printf '%s' '"+snapshot().dump()+"'");
	Collector::Resources collector(f.root/"utmp",f.root/"monitor");
	const auto start=Protocol::now_ms(); auto first=collector.sample();
	EXPECT_LT(Protocol::now_ms()-start,500); EXPECT_EQ(first["cards"]["status"],"warming_up");
	const auto value=collected(collector);
	EXPECT_EQ(value["cards"]["status"],"ok"); EXPECT_EQ(value["sessions"]["data"]["sessions"],3);
	const auto age=value["cards"]["data"]["age_ms"].get<std::uint64_t>();
	const auto later=collector.sample(Protocol::now_ms()+1000);
	EXPECT_GE(later["cards"]["data"]["age_ms"].get<std::uint64_t>(),age+990);
	EXPECT_EQ(later["cards"]["data"]["snapshot_seq"],42);
}
TEST(resources, timeout_bad_json_and_nonzero_exit_do_not_break_login_samples) {
	ResourceFixture f; f.sessions();
	for (const auto& command : {std::string("sleep 3"),std::string("printf 'broken'"),"printf '%s' '"+snapshot().dump()+"'; exit 1"}) {
		f.script(command); Collector::Resources collector(f.root/"utmp",f.root/"monitor");
		const auto start=Protocol::now_ms(); const auto result=collected(collector);
		EXPECT_LT(Protocol::now_ms()-start,2500);
		EXPECT_NE(result["cards"]["status"],"ok"); EXPECT_EQ(result["sessions"]["status"],"ok");
		EXPECT_TRUE(result["cards"]["data"].is_null());
	}
}
TEST(resources, excessive_output_and_stale_snapshots_never_appear_free) {
	ResourceFixture f; f.sessions(); f.script("head -c 270000 /dev/zero");
	{ Collector::Resources collector(f.root/"utmp",f.root/"monitor"); EXPECT_EQ(collected(collector)["cards"]["status"],"unavailable"); }
	auto value=snapshot(); value["age_ms"]=16000;
	f.script("printf '%s' '"+value.dump()+"'");
	Collector::Resources collector(f.root/"utmp",f.root/"monitor");
	EXPECT_EQ(collected(collector)["cards"]["status"],"unavailable");
}
TEST(resources, uid_filter_is_exact_and_user_filter_remains_fuzzy) {
	const Json a{{"uid",1000},{"user","alice"},{"name","python"}}, b{{"uid",1001},{"user","alice-ci"},{"name","python"}};
	EXPECT_TRUE(Protocol::ProcessFilter("user:ALICE").matches(a)); EXPECT_TRUE(Protocol::ProcessFilter("user:ALICE").matches(b));
	EXPECT_TRUE(Protocol::ProcessFilter("uid:1000").matches(a)); EXPECT_FALSE(Protocol::ProcessFilter("uid:1000").matches(b));
	EXPECT_FALSE(Protocol::ProcessFilter("uid:100").matches(a)); EXPECT_FALSE(Protocol::ProcessFilter("uid:").matches(a));
	EXPECT_FALSE(Protocol::ProcessFilter("uid:-1").matches(a)); EXPECT_FALSE(Protocol::ProcessFilter("uid:4294967296").matches(a));
	EXPECT_FALSE(Protocol::ProcessFilter("uid:1000").matches(Json{{"uid",nullptr}}));
}
