// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "collector/linux.hpp"
#include "protocol/protocol.hpp"
#include "protocol/process_filter.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

using Protocol::Json;
namespace {
struct Fixture {
	std::filesystem::path root;
	Fixture() {
		char name[]="/tmp/btop-collector-test-XXXXXX";
		root=mkdtemp(name);
		std::filesystem::create_directories(root/"sys/block/sda");
		std::filesystem::create_directories(root/"sys/class/hwmon");
		write("sys/class/net/eth0/ifindex","2\n");
		write("proc/loadavg","1.2 0.8 0.4 1/2 42\n");
		write("proc/meminfo","MemTotal: 1000 kB\nMemFree: 200 kB\nMemAvailable: 400 kB\nCached: 100 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
		write("proc/self/mountinfo","");
	}
	~Fixture() { std::filesystem::remove_all(root); }
	void write(const std::string& name, const std::string& value) {
		std::filesystem::create_directories((root/name).parent_path());
		std::ofstream(root/name)<<value;
	}
	std::string proc_stat(unsigned ticks, unsigned start=500) {
		std::vector<std::string> fields(22,"0");
		fields[0]="R"; fields[1]="1"; fields[11]=std::to_string(ticks); fields[17]="1";
		fields[19]=std::to_string(start); fields[21]="10";
		std::string out="42 (worker ) name)";
		for (const auto& field:fields) out+=' '+field;
		return out+'\n';
	}
};
}

TEST(protocol, user_filter_matches_owner_substrings_and_preserves_name_search) {
	const Json process{{"name","python3"},{"user","ci-runner"},{"uid",1001},{"command","python3 train.py"}};
	EXPECT_TRUE(Protocol::ProcessFilter("").matches(process));
	EXPECT_TRUE(Protocol::ProcessFilter("PyThOn").matches(process));
	EXPECT_TRUE(Protocol::ProcessFilter("user:ci-runner").matches(process));
	EXPECT_TRUE(Protocol::ProcessFilter("user:ci").matches(process));
	EXPECT_TRUE(Protocol::ProcessFilter("user:runner").matches(process));
	EXPECT_FALSE(Protocol::ProcessFilter("user:ci-runner-other").matches(process));
	EXPECT_TRUE(Protocol::ProcessFilter("user:CI-RUNNER").matches(process));
	EXPECT_FALSE(Protocol::ProcessFilter("user:").matches(process));
	EXPECT_FALSE(Protocol::ProcessFilter("ci-runner").matches(process));
	EXPECT_FALSE(Protocol::ProcessFilter("train.py").matches(process));
	EXPECT_FALSE(Protocol::ProcessFilter("user:ci-runner").matches(Json{{"name","ci-runner"}}));
	EXPECT_FALSE(Protocol::ProcessFilter("user:ci-runner").matches(Json{{"user",nullptr}}));
	EXPECT_TRUE(Protocol::ProcessFilter("user:1001").matches(Json{{"user","1001"}}));
	EXPECT_FALSE(Protocol::ProcessFilter("user:1001").matches(process));
}

TEST(collection, user_filter_runs_before_limit_and_preserves_other_views) {
	Fixture fixture;
	fixture.write("proc/stat","cpu 100 0 50 850 0 0 0 0\ncpu0 100 0 50 850 0 0 0 0\n");
	fixture.write("proc/42/stat",fixture.proc_stat(30));
	Collector::Linux collector(fixture.root/"proc",fixture.root/"sys",100,4096);
	Protocol::Subscription all;
	all.detail=all.processes=true;
	const auto first=collector.sample(all,1000);
	const auto owner=first["processes"]["data"]["items"][0]["user"].get<std::string>();
	// A second PID owned by the same account exercises the post-filter limit.
	auto other=fixture.proc_stat(20); other.replace(0,2,"43");
	fixture.write("proc/43/stat",other);
	Protocol::Subscription mine=all, nobody=all, named=all;
	mine.filter="user:"+owner; mine.limit=1; mine.sort="pid";
	nobody.filter="user:"+owner+"-not-this-account";
	named.filter="WoRkEr";
	const auto result=collector.sample_batch(all,{{"mine",mine},{"nobody",nobody},{"name",named}},3000);
	const auto& data=result["process_views"]["mine"]["data"];
	EXPECT_EQ(data["matched"],2);
	EXPECT_EQ(data["returned"],1);
	EXPECT_TRUE(data["truncated"].get<bool>());
	ASSERT_EQ(data["items"].size(),1);
	EXPECT_EQ(data["items"][0]["user"],owner);
	EXPECT_EQ(data["items"][0]["pid"],42);
	EXPECT_TRUE(result["process_views"]["nobody"]["data"]["items"].empty());
	EXPECT_EQ(result["process_views"]["name"]["data"]["matched"],2);
}

TEST(collection, cpu_guest_fields_reset_and_process_names) {
	const auto before=Collector::parse_cpu("100 0 50 850 0 0 0 0 500 0");
	const auto after=Collector::parse_cpu("110 0 60 930 0 0 0 0 900 0");
	EXPECT_EQ(before.total,1000);
	ASSERT_TRUE(Collector::cpu_usage(before,after));
	EXPECT_NEAR(*Collector::cpu_usage(before,after),20,1e-8);
	EXPECT_FALSE(Collector::cpu_usage(after,before));
	EXPECT_FALSE(Collector::cpu_usage(before,before));
	Fixture fixture;
	const auto proc=Collector::parse_process(fixture.proc_stat(30),4096);
	EXPECT_EQ(proc["name"],"worker ) name");
	EXPECT_EQ(proc["rss_bytes"],40960);
	EXPECT_EQ(proc["start_time_ticks"],500);
}

TEST(collection, all_groups_use_real_elapsed_time_and_reset_on_pid_reuse) {
	Fixture fixture;
	fixture.write("proc/stat","cpu 100 0 50 850 0 0 0 0\ncpu0 100 0 50 850 0 0 0 0\n");
	fixture.write("proc/diskstats","8 0 sda 10 0 100 0 20 0 200 0 0 0 0\n");
	fixture.write("proc/net/dev","eth0: 1000 0 0 0 0 0 0 0 2000 0 0 0 0 0 0 0\n");
	fixture.write("proc/42/stat",fixture.proc_stat(30));
	Collector::Linux collector(fixture.root/"proc",fixture.root/"sys",100,4096);
	Protocol::Subscription options;
	options.detail=options.processes=true;
	auto first=collector.sample(options,1000);
	EXPECT_TRUE(first["cpu"]["data"]["usage_pct"].is_null());
	EXPECT_TRUE(first["network"]["data"]["items"][0]["rx_bytes_per_sec"].is_null());
	EXPECT_EQ(first["memory"]["data"]["used_bytes"],600*1024);
	EXPECT_EQ(first["memory"]["data"]["swap_used_bytes"],0);
	fixture.write("proc/stat","cpu 110 0 60 930 0 0 0 0\ncpu0 110 0 60 930 0 0 0 0\n");
	fixture.write("proc/diskstats","8 0 sda 10 0 104 0 20 0 208 0 0 0 0\n");
	fixture.write("proc/net/dev","eth0: 3000 0 0 0 0 0 0 0 6000 0 0 0 0 0 0 0\n");
	fixture.write("proc/42/stat",fixture.proc_stat(50));
	auto second=collector.sample(options,3000);
	EXPECT_NEAR(second["cpu"]["data"]["usage_pct"].get<double>(),20,1e-8);
	EXPECT_EQ(second["disks"]["data"]["items"][0]["read_bytes_per_sec"],1024);
	EXPECT_EQ(second["network"]["data"]["items"][0]["rx_bytes_per_sec"],1000);
	EXPECT_EQ(second["processes"]["data"]["items"][0]["cpu_pct"],10);
	fixture.write("proc/42/stat",fixture.proc_stat(5,900));
	fixture.write("proc/net/dev","eth0: 1 0 0 0 0 0 0 0 2 0 0 0 0 0 0 0\n");
	auto reset=collector.sample(options,5000);
	EXPECT_TRUE(reset["processes"]["data"]["items"][0]["cpu_pct"].is_null());
	EXPECT_TRUE(reset["network"]["data"]["items"][0]["rx_bytes_per_sec"].is_null());
	options.processes=false;
	EXPECT_FALSE(collector.sample(options,7000).contains("processes"));
}

TEST(protocol, framing_duplicates_depth_and_large_integers) {
	Protocol::Lines lines;
	EXPECT_TRUE(lines.feed("{\"version\":1,").empty());
	auto frames=lines.feed("\"type\":\"test\"}\n{\"version\":1,\"type\":\"test\"}\n");
	ASSERT_EQ(frames.size(),2);
	EXPECT_EQ(Protocol::parse(frames[0])["type"],"test");
	EXPECT_THROW(Protocol::parse("{\"version\":1,\"version\":1,\"type\":\"test\"}"),std::exception);
	EXPECT_THROW(Protocol::parse("{\"version\":2,\"type\":\"test\"}"),std::exception);
	EXPECT_THROW(lines.feed(std::string(Protocol::max_frame+1,'x')),std::exception);
	const auto value=Protocol::parse("{\"version\":1,\"type\":\"test\",\"value\":18446744073709551615}");
	EXPECT_EQ(value["value"].get<std::uint64_t>(),UINT64_MAX);
	Protocol::Subscription options;
	EXPECT_EQ(Protocol::subscription(Protocol::encode(options)).interval_ms,2000);
	auto invalid=Protocol::encode(options); invalid["interval_ms"]=-1;
	EXPECT_THROW(Protocol::subscription(invalid),std::exception);
}

TEST(collection, detailed_metadata_disk_activity_and_selected_process_identity) {
	Fixture fixture;
	fixture.write("proc/cpuinfo","model name : Fixture CPU\n");
	fixture.write("proc/uptime","1000.0 100.0\n");
	fixture.write("proc/stat","cpu 100 0 50 850 0 0 0 0\ncpu0 100 0 50 850 0 0 0 0\n");
	fixture.write("sys/class/hwmon/hwmon0/name","coretemp\n");
	fixture.write("sys/class/hwmon/hwmon0/temp1_label","Package id 0\n");
	fixture.write("sys/class/hwmon/hwmon0/temp1_input","60000\n");
	fixture.write("sys/class/hwmon/hwmon0/temp2_label","Core 0\n");
	fixture.write("sys/class/hwmon/hwmon0/temp2_input","55000\n");
	fixture.write("sys/devices/system/cpu/cpu0/topology/core_id","0\n");
	fixture.write("sys/devices/system/cpu/cpu0/topology/physical_package_id","0\n");
	fixture.write("sys/dev/block/8:1","");
	fixture.write("proc/diskstats","8 1 sda1 10 0 100 0 20 0 200 0 0 100 0\n");
	fixture.write("proc/net/dev","eth0: 1000 0 0 0 0 0 0 0 2000 0 0 0 0 0 0 0\n");
	fixture.write("proc/42/stat",fixture.proc_stat(30));
	fixture.write("proc/42/cmdline",std::string("worker\0--task\0",14));
	fixture.write("proc/42/io","read_bytes: 4096\nwrite_bytes: 8192\n");
	Collector::Linux collector(fixture.root/"proc",fixture.root/"sys",100,4096);
	Protocol::Subscription options;
	options.detail=options.processes=true; options.selected_pid=42; options.selected_start=500;
	collector.sample(options,1000);
	fixture.write("proc/stat","cpu 110 0 60 930 0 0 0 0\ncpu0 110 0 60 930 0 0 0 0\n");
	fixture.write("proc/diskstats","8 1 sda1 10 0 104 0 20 0 208 0 0 1100 0\n");
	auto groups=collector.sample(options,3000);
	EXPECT_EQ(groups["cpu"]["data"]["model"],"Fixture CPU");
	EXPECT_EQ(groups["cpu"]["data"]["uptime_seconds"],1000);
	EXPECT_EQ(groups["cpu"]["data"]["user_pct"],10);
	EXPECT_EQ(groups["cpu"]["data"]["system_pct"],10);
	EXPECT_EQ(groups["cpu"]["data"]["cores"][0]["temperature_c"],55);
	EXPECT_EQ(groups["disks"]["data"]["items"][0]["id"],"8:1");
	EXPECT_EQ(groups["disks"]["data"]["items"][0]["activity_pct"],50);
	const auto& chosen=groups["processes"]["data"]["selected"];
	EXPECT_EQ(chosen["detail_status"],"ok");
	EXPECT_EQ(chosen["elapsed_seconds"],995);
	EXPECT_EQ(chosen["read_bytes_total"],4096);
	EXPECT_EQ(chosen["command"],"worker --task");
	EXPECT_TRUE(chosen["user"].is_string());
	// The selected process is returned separately even when excluded by the list filter.
	options.filter="does-not-match";
	groups=collector.sample(options,4000);
	EXPECT_TRUE(groups["processes"]["data"]["items"].empty());
	EXPECT_EQ(groups["processes"]["data"]["selected"]["detail_status"],"ok");
	fixture.write("proc/42/cmdline",std::string(4096,'\xff'));
	groups=collector.sample(options,4500);
	EXPECT_LE(groups["processes"]["data"]["selected"]["command"].get<std::string>().size(),4096);
	EXPECT_NO_THROW(Protocol::parse(Json{{"version",1},{"type","snapshot"},{"groups",groups}}.dump()));
	fixture.write("proc/42/stat",fixture.proc_stat(5,900));
	fixture.write("sys/devices/system/cpu/cpu0/topology/physical_package_id","1\n");
	groups=collector.sample(options,5000);
	EXPECT_EQ(groups["processes"]["data"]["selected"]["detail_status"],"gone");
	EXPECT_FALSE(groups["processes"]["data"]["selected"].contains("command"));
	EXPECT_TRUE(groups["cpu"]["data"]["cores"][0]["temperature_c"].is_null());
}

TEST(protocol, selection_is_optional_but_requires_stable_identity_and_valid_sort) {
	Protocol::Subscription options; options.selected_pid=42; options.selected_start=500; options.sort="threads"; options.reverse=true;
	const auto parsed=Protocol::subscription(Protocol::encode(options));
	EXPECT_EQ(parsed.selected_pid,42); EXPECT_EQ(parsed.selected_start,500); EXPECT_TRUE(parsed.reverse);
	auto request=Protocol::encode(options);
	request["processes"].erase("selected_start");
	EXPECT_THROW(Protocol::subscription(request),std::exception);
	request["processes"]["selected_pid"]=0;
	request["processes"]["sort"]="invalid";
	EXPECT_THROW(Protocol::subscription(request),std::exception);
}

TEST(collection, batch_views_share_cpu_baseline_and_keep_filters_and_identity_independent) {
	Fixture fixture;
	fixture.write("proc/stat","cpu 100 0 50 850 0 0 0 0\ncpu0 100 0 50 850 0 0 0 0\n");
	fixture.write("proc/42/stat",fixture.proc_stat(30));
	Collector::Linux collector(fixture.root/"proc",fixture.root/"sys",100,4096);
	Protocol::Subscription options, a, b;
	options.detail=true; a.detail=a.processes=b.detail=b.processes=true;
	a.filter="worker"; b.filter="not-matching"; b.selected_pid=42; b.selected_start=500;
	std::map<std::string,Protocol::Subscription> requests{{"alice",a},{"bob",b}};
	collector.sample_batch(options,requests,1000);
	fixture.write("proc/42/stat",fixture.proc_stat(50));
	fixture.write("proc/stat","cpu 110 0 60 930 0 0 0 0\ncpu0 110 0 60 930 0 0 0 0\n");
	const auto result=collector.sample_batch(options,requests,3000);
	EXPECT_NEAR(result["groups"]["cpu"]["data"]["usage_pct"].get<double>(),20,1e-8);
	const auto& alice=result["process_views"]["alice"]["data"];
	const auto& bob=result["process_views"]["bob"]["data"];
	ASSERT_EQ(alice["items"].size(),1); EXPECT_EQ(alice["items"][0]["cpu_pct"],10);
	EXPECT_TRUE(bob["items"].empty()); EXPECT_EQ(bob["selected"]["cpu_pct"],10);
	EXPECT_EQ(bob["selected"]["start_time_ticks"],500); EXPECT_TRUE(alice["selected"].is_null());
	fixture.write("proc/42/stat",fixture.proc_stat(2,900));
	const auto reused=collector.sample_batch(options,requests,5000);
	EXPECT_EQ(reused["process_views"]["bob"]["data"]["selected"]["detail_status"],"gone");
	EXPECT_TRUE(reused["process_views"]["alice"]["data"]["items"][0]["cpu_pct"].is_null());
}
