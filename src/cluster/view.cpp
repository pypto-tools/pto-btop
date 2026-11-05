// SPDX-License-Identifier: Apache-2.0

#include "view.hpp"
#include "protocol/process_filter.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <fmt/format.h>

#include "btop_theme.hpp"
#include "btop_tools.hpp"

namespace Cluster {
namespace {

constexpr double gib = 1024.0 * 1024 * 1024;

std::string at(int row, int col, const std::string& text) {
	return Mv::to(row, col) + Theme::c("main_fg") + text;
}

std::string draw_card(const Host& host, const Rect& rect, bool selected, const Draw::Style& style) {
	const auto& [x, y, width, height] = rect;
	const auto color = Theme::c(selected ? "hi_fg" : "cpu_box");
	auto out = Draw::createBox(x, y, width, height, color, true,
		safe_text(host.name, width - 8), host.simulated ? "SIMULATED" : host.status, 0, style);
	out += at(y + 1, x + 2, host.cpu_valid ? fmt::format("CPU {:5.1f}%  / {} logical cores", host.cpu, host.cores) : "CPU N/A / waiting for a valid sample");
	const int graph_height = std::clamp(height - 10, 2, 6);
	// Construct from this host's history only. Selection and redraw never add samples.
	Draw::Graph graph(width - 4, graph_height, "cpu", host.history,
		style.graph_symbol, false, false, 0, 0, style);
	out += Mv::to(y + 3, x + 2) + graph();
	int row = y + 4 + graph_height;
	out += at(row, x + 2, host.memory_valid ? fmt::format("MEM {:.1f} / {:.0f} GiB", host.memory_used / gib, host.memory_total / gib) : "MEM N/A");
	Draw::Meter meter(width - 4, "used");
	out += Mv::to(++row, x + 2) + meter(static_cast<int>(host.memory_total ? 100.0 * host.memory_used / host.memory_total : 0));
	out += at(++row, x + 2, fmt::format("LOAD {:.2f} / {:.2f} / {:.2f}", host.load[0], host.load[1], host.load[2]));
	out += at(++row, x + 2, host.simulated ? fmt::format("Load/core {:.2f}", host.cores ? host.load[0] / host.cores : 0) : safe_text(host.status + (host.error.empty() ? "" : ": " + host.error), width - 4));
	return out;
}

}

Geometry layout(int width, int height, int count) {
	if (count < 1 or count > 4) throw std::invalid_argument("Expected 1 to 4 hosts");
	if (width < 80 or height < 24) return {Layout::too_small, {}};
	if (width < 120 or height < 36) return {Layout::compact, {}};
	Geometry result{Layout::cards, {}};
	const int columns = count == 1 ? 1 : 2;
	const int rows = count <= 2 ? 1 : 2;
	const int card_width = (width - 3) / columns;
	const int card_height = (height - 4) / rows;
	for (int i = 0; i < count; ++i) {
		result.cards.push_back({2 + (i % columns) * card_width,
			3 + (i / columns) * card_height, card_width - 1, card_height - 1});
	}
	return result;
}

std::vector<Host> demo_hosts(int interval_ms) {
	std::vector<Host> hosts;
	for (unsigned i = 0; i < 4; ++i) {
		Host host;
		host.name = fmt::format("dev-{:02}", i + 1);
		host.cores = 16 << i;
		host.memory_total = static_cast<std::uint64_t>(gib) * (32 << i);
		hosts.push_back(host);
	}
	for (unsigned tick = 0; tick < 300; ++tick) advance_demo(hosts, tick, interval_ms);
	return hosts;
}

void advance_demo(std::vector<Host>& hosts, unsigned tick, int interval_ms) {
	for (std::size_t i = 0; auto& host : hosts) {
		host.cpu = std::clamp(18.0 + 17.0 * i + 14 * std::sin(tick * 0.18 + i * 1.4), 0.0, 100.0);
		host.memory_used = static_cast<std::uint64_t>(host.memory_total * (0.22 + 0.14 * i));
		host.load = {host.cpu / 100 * host.cores, host.cpu / 115 * host.cores, host.cpu / 130 * host.cores};
		using Protocol::Json;
		const auto stamp = Protocol::now_ms();
		Json cores=Json::array(), processes=Json::array();
		for (int core=0; core<host.cores; ++core)
			cores.push_back({{"id",core},{"usage_pct",std::clamp(host.cpu+12*std::sin(core+tick*0.1),0.0,100.0)},{"temperature_c",48+core%9}});
		for (int pid=0; pid<40; ++pid)
			processes.push_back({{"pid",1000+pid},{"start_time_ticks",10000+pid},{"ppid",pid ? 1000+(pid-1)/4 : 1},{"uid",1000},{"user","developer"},{"name",pid%2 ? "python" : "compiler"},{"threads",pid%8+1},{"command",pid%2 ? "python train.py" : "clang++ -O2 worker.cpp"},{"state","R"},{"nice",0},{"elapsed_seconds",3000+pid},{"read_bytes_total",12345678},{"write_bytes_total",2345678},
				{"cpu_pct",host.cpu*(40-pid)/10},{"rss_bytes",std::uint64_t(40-pid)*1024*1024}});
		Json groups{
			{"cpu",Protocol::group({{"usage_pct",host.cpu},{"iowait_pct",2.0},{"online_cores",host.cores},
				{"load_avg",host.load},{"cores",cores},{"frequency_mhz",3200},{"temperature_c",52},{"model","AMD EPYC / Demo"},{"uptime_seconds",1234567},{"user_pct",host.cpu*0.7},{"system_pct",host.cpu*0.25}},stamp,interval_ms)},
			{"memory",Protocol::group({{"total_bytes",host.memory_total},{"used_bytes",host.memory_used},
				{"available_bytes",host.memory_total-host.memory_used},{"free_bytes",host.memory_total/4},
				{"cached_bytes",host.memory_total/8},{"buffers_bytes",134217728},{"swap_total_bytes",0},{"swap_used_bytes",0}},stamp,interval_ms)},
			{"filesystems",Protocol::group({{"items",Json::array({{{"mount","/"},{"device","259:0"},
				{"total_bytes",107374182400ULL},{"used_bytes",64424509440ULL},{"available_bytes",42949672960ULL}}})}},stamp,interval_ms)},
			{"disks",Protocol::group({{"items",Json::array({{{"name","nvme0n1"},{"id","259:0"},
				{"read_bytes_per_sec",2097152*(1+std::sin(tick*0.17))},{"write_bytes_per_sec",1048576*(1+std::cos(tick*0.13))},{"activity_pct",25+20*std::sin(tick*0.17)}}})}},stamp,interval_ms)},
			{"network",Protocol::group({{"items",Json::array({{{"name","eth0"},{"ifindex",2},{"address","100.90.0."+std::to_string(i+1)},
				{"rx_bytes_total",1073741824ULL},{"tx_bytes_total",536870912ULL},
				{"rx_bytes_per_sec",std::uint64_t(host.cpu*10000)},{"tx_bytes_per_sec",std::uint64_t(host.cpu*4000)}}})}},stamp,interval_ms)},
			{"processes",Protocol::group({{"items",processes},{"total_visible",40},{"matched",40},{"returned",40},{"truncated",false}},stamp,interval_ms)}
		};
		groups["filesystems"]["data"]["items"].push_back({{"mount","/data"},{"device","259:1"},{"total_bytes",2199023255552ULL},{"used_bytes",1209462790553ULL},{"available_bytes",989560464999ULL}});
		groups["disks"]["data"]["items"].push_back({{"name","nvme1n1"},{"id","259:1"},{"read_bytes_per_sec",16777216*(1+std::sin(tick*0.09))},{"write_bytes_per_sec",4194304},{"activity_pct",65}});
		const Protocol::ProcessFilter filter(host.filter);
		std::erase_if(processes.get_ref<Json::array_t&>(),[&](const auto& item){return not filter.matches(item);});
		groups["processes"]["data"]["items"]=processes;
		groups["processes"]["data"]["matched"]=groups["processes"]["data"]["returned"]=processes.size();
		for (const auto& item : processes) if (item["pid"]==host.selected_pid and item["start_time_ticks"]==host.selected_start) {
			groups["processes"]["data"]["selected"]=item;
			groups["processes"]["data"]["selected"]["detail_status"]="ok";
		}
		ingest(host,{{"groups",groups},{"wall_time",Protocol::utc_now()}});
		++i;
	}
}

std::string render(const std::vector<Host>& hosts, int width, int height,
	int selected, bool detail, const Draw::Style& style) {
	const auto geometry = layout(width, height, static_cast<int>(hosts.size()));
	if (selected < 0 or selected >= static_cast<int>(hosts.size())) throw std::invalid_argument("Invalid host selection");
	std::string out = Term::clear + Fx::reset;
	if (geometry.layout == Layout::too_small) {
		// Stay inside even a one-column terminal, without writing its last cell.
		const std::string message = "Resize terminal to at least 80x24";
		if (width > 1 and height > 0) out += at(1, 1, message.substr(0, width - 1));
		return out;
	}
	out += at(1, 2, hosts[0].simulated ? "btop-cluster | DEMO - simulated data" : "btop-cluster | LIVE");
	if (detail) {
		out += detail_view(hosts.at(selected), width, height, style);
		const std::string controls="1-4 Host  Esc Overview  ? Help  q Quit";
		out += at(1,width-1-static_cast<int>(controls.size()),controls);
	} else if (geometry.layout == Layout::cards) {
		for (std::size_t i = 0; i < hosts.size(); ++i)
			out += draw_card(hosts[i], geometry.cards[i], i == static_cast<std::size_t>(selected), style);
		out += at(height - 1, 2, "Tab Select   Enter Details   1-4 Host   t Theme   q Quit");
	} else {
		out += at(3, 2, "HOST       CPU       MEMORY GiB         LOAD 1/5/15       CORES");
		for (std::size_t i = 0; i < hosts.size(); ++i) {
			const auto& host = hosts[i];
			out += Mv::to(5 + i * 3, 2) + Theme::c(i == static_cast<std::size_t>(selected) ? "hi_fg" : "main_fg");
			out += fmt::format("{} {:7} {:5.1f}%   {:5.1f}/{:<5.0f}      {:5.1f}/{:5.1f}/{:5.1f}  {:4}",
				i == static_cast<std::size_t>(selected) ? ">" : " ", safe_text(host.name, 7), host.cpu,
				host.memory_used / gib, host.memory_total / gib, host.load[0], host.load[1], host.load[2], host.cores);
			if (not host.cpu_valid or host.status != "ONLINE") out += at(6 + i * 3, 4, safe_text(host.status + " " + host.error, width - 6));
		}
		out += at(19, 2, "Compact overview | expand to 120x36 for graphs");
		out += at(height - 1, 2, "Tab Select   Enter Details   1-4 Host   t Theme   q Quit");
	}
	return out + Fx::reset;
}

}
