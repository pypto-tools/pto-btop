// SPDX-License-Identifier: Apache-2.0
// Headless Linux accounting adapted from btop's CPU/memory accounting conventions.
// Original btop collectors: Copyright 2021 Aristocratos (jakob@qvantnet.com).
// Modified for btop-cluster: instance-owned baselines and versioned snapshots.
#include "cluster_version.hpp"
#include "linux.hpp"
#include "filesystems.hpp"
#include "protocol/process_filter.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

namespace Collector {
namespace {
std::string read_text(const std::filesystem::path& path) {
	std::ifstream file(path);
	if (not file) throw std::runtime_error("Cannot read " + path.string());
	return {std::istreambuf_iterator<char>(file), {}};
}
Json optional_number(const std::filesystem::path& path, double scale = 1) {
	std::ifstream file(path);
	double number;
	if (file >> number) return number / scale;
	return nullptr;
}
std::string short_file(const std::filesystem::path& path, std::size_t limit) {
	std::ifstream file(path, std::ios::binary);
	std::string value(limit, '\0');
	file.read(value.data(), value.size()); value.resize(file.gcount());
	std::replace(value.begin(), value.end(), '\0', ' ');
	while (not value.empty() and (value.back() == ' ' or value.back() == '\n')) value.pop_back();
	// proc names/arguments are byte strings; keep the wire limit after UTF-8 repair.
	value = Json::parse(Json(value).dump(-1,' ',false,Json::error_handler_t::replace)).get<std::string>();
	if (value.size()>limit) {
		auto end=limit;
		while (end>0 and (static_cast<unsigned char>(value[end])&0xc0)==0x80) --end;
		value.resize(end);
	}
	return value;
}
Json number(std::optional<double> value) { return value ? Json(*value) : Json(nullptr); }
Json rate(std::uint64_t value, std::uint64_t previous, std::uint64_t elapsed) {
	if (elapsed == 0 or value < previous) return nullptr;
	return 1000.0 * (value - previous) / elapsed;
}
bool selected(const std::vector<std::string>& list, const std::string& value) {
	return list.empty() or std::ranges::find(list, value) != list.end();
}
}

CpuTimes parse_cpu(std::string_view fields) {
	std::istringstream input{std::string(fields)};
	std::vector<std::uint64_t> values;
	for (std::uint64_t value; input >> value;) values.push_back(value);
	if (values.size() < 4) throw std::runtime_error("Malformed CPU counters");
	CpuTimes out;
	for (std::size_t i = 0; i < std::min<std::size_t>(8, values.size()); ++i) out.total += values[i];
	out.iowait = values.size() > 4 ? values[4] : 0;
	out.user = values[0] + values[1];
	out.system = values[2] + (values.size() > 6 ? values[5] + values[6] : 0);
	out.idle = values[3] + out.iowait;
	return out;
}

std::optional<double> cpu_usage(CpuTimes before, CpuTimes after) {
	if (after.total <= before.total or after.idle < before.idle or after.idle-before.idle > after.total-before.total)
		return std::nullopt;
	return 100.0 * (1.0 - double(after.idle-before.idle) / (after.total-before.total));
}

Json parse_process(std::string_view stat, std::uint64_t page_size) {
	const auto open = stat.find('('), close = stat.rfind(')');
	if (open == std::string_view::npos or close == std::string_view::npos or close <= open)
		throw std::runtime_error("Malformed process stat");
	std::istringstream fields{std::string(stat.substr(close+1))};
	std::vector<std::string> values;
	for (std::string word; fields >> word;) values.push_back(word);
	if (values.size() < 22) throw std::runtime_error("Short process stat");
	const auto rss = std::stoll(values[21]);
	return {{"pid", std::stoi(std::string(stat.substr(0, open)))}, {"name", std::string(stat.substr(open+1, close-open-1))},
		{"state", values[0]}, {"ppid", std::stoull(values[1])}, {"nice", std::stoi(values[16])},
		{"threads", std::stoull(values[17])}, {"start_time_ticks", std::stoull(values[19])},
		{"cpu_ticks", std::stoull(values[11]) + std::stoull(values[12])},
		{"rss_bytes", std::uint64_t(std::max(0LL, rss)) * page_size}, {"cpu_pct", nullptr}};
}

Linux::Linux(std::filesystem::path proc, std::filesystem::path sys, std::uint64_t ticks, std::uint64_t page_size)
	: proc_(std::move(proc)), sys_(std::move(sys)), filesystems_(std::make_unique<Filesystems>(proc_)),
	  ticks_(ticks ? ticks : std::max(1L, sysconf(_SC_CLK_TCK))),
	  page_size_(page_size ? page_size : std::max(1L, sysconf(_SC_PAGESIZE))) {
	std::ifstream cpuinfo(proc_ / "cpuinfo");
	for (std::string line; std::getline(cpuinfo, line);) {
		const auto colon = line.find(':');
		if (colon == std::string::npos) continue;
		if (line.starts_with("model name") or line.starts_with("Hardware") or line.starts_with("Processor")) {
			model_ = line.substr(colon + 1); model_.erase(0, model_.find_first_not_of(" \t")); break;
		}
	}
	if (model_.empty()) { utsname system{}; uname(&system); model_ = system.machine; }
	// Read the local passwd file once; NSS/LDAP lookups must not block the sampling loop.
	std::ifstream passwd("/etc/passwd");
	for (std::string line; std::getline(passwd, line);) {
		std::istringstream row(line); std::string name, ignored, uid;
		if (std::getline(row,name,':') and std::getline(row,ignored,':') and std::getline(row,uid,':')) {
			try { users_[std::stoul(uid)] = name; } catch (...) {}
		}
	}
}
Linux::~Linux() = default;

Json Linux::hello(const std::string& session) const {
	char hostname[256]{};
	gethostname(hostname, sizeof(hostname)-1);
	utsname system{};
	uname(&system);
	std::string boot;
	try { boot = read_text(proc_ / "sys/kernel/random/boot_id"); boot.erase(boot.find_last_not_of("\r\n") + 1); } catch (...) { boot = "unknown"; }
	return {{"type", "hello"}, {"version", 1}, {"session_id", session}, {"boot_id", boot}, {"agent_version", BTOP_CLUSTER_VERSION},
		{"capabilities", {"cpu", "memory", "filesystems", "disks", "network", "processes"}},
		{"host", {{"hostname", hostname}, {"os", "linux"}, {"arch", system.machine}, {"online_cores", sysconf(_SC_NPROCESSORS_ONLN)}}}};
}

Json Linux::cpu(bool detail, std::uint64_t now) {
	std::istringstream input(read_text(proc_ / "stat"));
	std::map<std::string, CpuTimes> current;
	Json cores = Json::array(), usage = nullptr, wait = nullptr, user = nullptr, system = nullptr;
	for (std::string line; std::getline(input, line);) {
		std::istringstream row(line);
		std::string name;
		row >> name;
		if (not name.starts_with("cpu")) continue;
		std::string fields; std::getline(row, fields);
		const auto value = parse_cpu(fields);
		current[name] = value;
		Json percent = cpus_.contains(name) ? number(cpu_usage(cpus_.at(name), value)) : Json(nullptr);
		if (name == "cpu") {
			usage = percent;
			if (not percent.is_null()) {
				const auto old = cpus_.at(name);
				auto delta = [&](auto current, auto previous) -> Json {
					return current >= previous ? Json(100.0 * (current-previous) / (value.total-old.total)) : Json(nullptr);
				};
				wait = delta(value.iowait,old.iowait); user = delta(value.user,old.user); system = delta(value.system,old.system);
			}
		} else if (detail and cores.size() < 2048) cores.push_back({{"id", std::stoi(name.substr(3))}, {"usage_pct", percent}});
	}
	if (not current.contains("cpu")) throw std::runtime_error("No CPU counters");
	const auto count = current.size()-1;
	cpus_ = std::move(current);
	Json load = Json::array();
	std::istringstream load_input(read_text(proc_ / "loadavg"));
	for (int i = 0; i < 3; ++i) { double value; if (not (load_input >> value)) throw std::runtime_error("Malformed loadavg"); load.push_back(value); }
	Json temperature = nullptr;
	std::map<int, Json> core_temperatures;
	int sensor_package = -1;
	std::error_code ec;
	for (auto it = std::filesystem::directory_iterator(sys_ / "class/hwmon", ec); not ec and it != std::filesystem::directory_iterator{}; it.increment(ec)) {
		const auto& item = *it;
		std::ifstream name(item.path() / "name"); std::string sensor; name >> sensor;
		if (sensor == "coretemp" or sensor == "k10temp" or sensor == "cpu_thermal") {
			temperature = optional_number(item.path() / "temp1_input", 1000);
			const auto package_label = short_file(item.path()/"temp1_label",64);
			if (package_label.starts_with("Package id ")) { try { sensor_package=std::stoi(package_label.substr(11)); } catch (...) {} }
			// Labels identify physical cores; map to logical CPUs through topology below.
			std::error_code sensor_error;
			for (auto sensor_it = std::filesystem::directory_iterator(item.path(), sensor_error);
				not sensor_error and sensor_it != std::filesystem::directory_iterator{}; sensor_it.increment(sensor_error)) {
				const auto label_name = sensor_it->path().filename().string();
				if (not label_name.starts_with("temp") or not label_name.ends_with("_label")) continue;
				const auto label = short_file(sensor_it->path(), 64);
				if (label.starts_with("Core ")) {
					try { core_temperatures[std::stoi(label.substr(5))] = optional_number(item.path() / (label_name.substr(0,label_name.size()-6)+"_input"),1000); } catch (...) {}
				}
			}
			break;
		}
	}
	for (auto& core : cores) {
		const auto id = core["id"].get<int>();
		const auto physical = optional_number(sys_ / ("devices/system/cpu/cpu" + std::to_string(id) + "/topology/core_id"));
		const auto package = optional_number(sys_ / ("devices/system/cpu/cpu" + std::to_string(id) + "/topology/physical_package_id"));
		core["temperature_c"] = sensor_package >= 0 and not package.is_null() and package.get<int>() == sensor_package and not physical.is_null() and core_temperatures.contains(physical.get<int>()) ? core_temperatures.at(physical.get<int>()) : Json(nullptr);
	}
	return Protocol::group({{"model", model_}, {"uptime_seconds", optional_number(proc_ / "uptime")},
		{"user_pct", user}, {"system_pct", system}, {"usage_pct", usage}, {"iowait_pct", wait}, {"online_cores", count}, {"load_avg", load},
		{"cores", cores}, {"frequency_mhz", optional_number(sys_ / "devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 1000)},
		{"temperature_c", temperature}}, now, last_ ? now-last_ : 0,
		usage.is_null() ? "warming_up" : (detail and count > 2048) ? "partial" : "ok");
}

Json Linux::memory(std::uint64_t now) {
	std::istringstream input(read_text(proc_ / "meminfo"));
	std::map<std::string, std::uint64_t> values;
	for (std::string line; std::getline(input, line);) {
		std::istringstream row(line); std::string key; std::uint64_t value;
		if (row >> key >> value) values[key] = value * 1024;
	}
	const auto total = values.at("MemTotal:");
	if (total == 0) throw std::runtime_error("Zero memory total");
	const bool estimate = not values.contains("MemAvailable:");
	const auto avail = std::min(total, estimate ? values["MemFree:"] + values["Cached:"] : values["MemAvailable:"]);
	const auto swap = values["SwapTotal:"];
	return Protocol::group({{"total_bytes", total}, {"available_bytes", avail}, {"used_bytes", total-avail},
		{"free_bytes", values["MemFree:"]}, {"cached_bytes", values["Cached:"]}, {"buffers_bytes", values["Buffers:"]},
		{"swap_total_bytes", swap}, {"swap_used_bytes", swap-std::min(swap, values["SwapFree:"])},
		{"estimated_available", estimate}}, now, last_ ? now-last_ : 0);
}

Json Linux::disks(const Protocol::Subscription& options, std::uint64_t now) {
	std::istringstream input(read_text(proc_ / "diskstats"));
	std::map<std::string, Counter> current;
	Json items = Json::array();
	bool partial = false;
	for (std::string line; std::getline(input, line);) {
		std::istringstream row(line);
		unsigned major, minor; std::string name;
		if (not (row >> major >> minor >> name)) continue;
		if (not selected(options.disks, name)) continue;
		if (options.disks.empty() and (name.starts_with("loop") or name.starts_with("ram") or name.starts_with("zram") or
			(not std::filesystem::exists(sys_ / "block" / name) and not std::filesystem::exists(sys_ / "dev/block" / (std::to_string(major)+":"+std::to_string(minor)))))) continue;
		std::vector<std::uint64_t> fields;
		for (std::uint64_t value; row >> value;) fields.push_back(value);
		if (fields.size() < 11) { partial = true; continue; }
		const auto id = std::to_string(major) + ':' + std::to_string(minor);
		const Counter value{fields[2] * 512, fields[6] * 512, now, fields[9]};
		Json read = nullptr, write = nullptr, activity = nullptr;
		if (disks_.contains(id)) {
			const auto old = disks_.at(id);
			read = rate(value.read, old.read, now-old.time); write = rate(value.write, old.write, now-old.time);
			if (now > old.time and value.busy >= old.busy) activity = std::min(100.0,100.0*(value.busy-old.busy)/(now-old.time));
		}
		current[id] = value;
		items.push_back({{"id", id}, {"name", name}, {"read_bytes_total", value.read}, {"write_bytes_total", value.write},
			{"read_bytes_per_sec", read}, {"write_bytes_per_sec", write}, {"activity_pct", activity}});
		if (items.size() >= 256) { partial = true; break; }
	}
	disks_ = std::move(current);
	return Protocol::group({{"items", items}}, now, last_ ? now-last_ : 0, partial ? "partial" : items.empty() ? "unavailable" : "ok");
}

Json Linux::network(const Protocol::Subscription& options, std::uint64_t now) {
	std::istringstream input(read_text(proc_ / "net/dev"));
	std::map<std::string,std::string> addresses;
	ifaddrs* interfaces = nullptr;
	if (proc_ == "/proc" and getifaddrs(&interfaces) == 0) {
		for (auto* item = interfaces; item; item = item->ifa_next) {
			if (not item->ifa_addr) continue;
			const auto family = item->ifa_addr->sa_family;
			char value[INET6_ADDRSTRLEN]{};
			if (family == AF_INET) inet_ntop(family,&reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr,value,sizeof(value));
			else if (family == AF_INET6 and not addresses.contains(item->ifa_name)) inet_ntop(family,&reinterpret_cast<sockaddr_in6*>(item->ifa_addr)->sin6_addr,value,sizeof(value));
			if (*value) addresses[item->ifa_name] = value;
		}
		freeifaddrs(interfaces);
	}
	std::map<std::string, Counter> current;
	Json items = Json::array();
	for (std::string line; std::getline(input, line);) {
		const auto colon = line.find(':'); if (colon == std::string::npos) continue;
		std::istringstream name_input(line.substr(0, colon)); std::string name; name_input >> name;
		if (not selected(options.interfaces, name) or (options.interfaces.empty() and name == "lo")) continue;
		std::istringstream row(line.substr(colon+1));
		std::vector<std::uint64_t> fields;
		for (std::uint64_t value; row >> value;) fields.push_back(value);
		if (fields.size() < 16) continue;
		std::ifstream index_file(sys_ / "class/net" / name / "ifindex"); unsigned index = 0; index_file >> index;
		const auto id = name + ':' + std::to_string(index);
		const Counter value{fields[0], fields[8], now};
		Json rx = nullptr, tx = nullptr;
		if (nets_.contains(id)) {
			const auto old = nets_.at(id);
			rx = rate(value.read, old.read, now-old.time); tx = rate(value.write, old.write, now-old.time);
		}
		current[id] = value;
		items.push_back({{"name", name}, {"address", addresses[name]}, {"ifindex", index}, {"rx_bytes_total", value.read}, {"tx_bytes_total", value.write},
			{"rx_bytes_per_sec", rx}, {"tx_bytes_per_sec", tx}});
		if (items.size() >= 128) break;
	}
	nets_ = std::move(current);
	return Protocol::group({{"items", items}}, now, last_ ? now-last_ : 0, items.empty() ? "unavailable" : items.size() >= 128 ? "partial" : "ok");
}

Json Linux::processes_many(const std::map<std::string, Protocol::Subscription>& requests, std::uint64_t now) {
	std::map<int, Process> current;
	std::vector<Json> items;
	std::size_t visible = 0, unreadable = 0;
	std::map<std::string, Json> selections;
	for (const auto& [id,options] : requests) {
		selections[id]=nullptr;
		if (options.selected_pid) selections[id]={{"pid",options.selected_pid},{"start_time_ticks",options.selected_start},{"detail_status","unavailable"}};
	}
	const auto uptime = optional_number(proc_ / "uptime");
	const auto deadline = Protocol::now_ms() + 250;
	bool budget = false;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(proc_, ec)) {
		const auto filename = entry.path().filename().string();
		if (filename.empty() or not std::ranges::all_of(filename, [](unsigned char c) { return std::isdigit(c); })) continue;
		if (Protocol::now_ms() > deadline or visible >= 100000) { budget = true; break; }
		try {
			auto item = parse_process(read_text(entry.path() / "stat"), page_size_);
			const int pid = item.at("pid").get<int>();
			const Process value{item.at("start_time_ticks"), item.at("cpu_ticks"), now};
			if (processes_.contains(pid)) {
				const auto old = processes_.at(pid);
				if (old.start == value.start and value.ticks >= old.ticks and now > old.time)
					item["cpu_pct"] = 100000.0 * (value.ticks-old.ticks) / ticks_ / (now-old.time);
			}
			current[pid] = value;
			struct stat info{};
			if (::stat(entry.path().c_str(), &info) == 0) {
				item["uid"] = info.st_uid;
				item["user"] = users_.contains(info.st_uid) ? users_.at(info.st_uid) : std::to_string(info.st_uid);
			}
			else item["uid"] = nullptr;
			item.erase("cpu_ticks");
			item["elapsed_seconds"] = uptime.is_number() ? Json(std::max(0.0,uptime.get<double>()-double(value.start)/ticks_)) : Json(nullptr);
			for (const auto& [id,options] : requests) {
				auto& selected_process=selections[id];
				if (pid == options.selected_pid and value.start != options.selected_start) selected_process["detail_status"]="gone";
				if (pid == options.selected_pid and value.start == options.selected_start) {
					selected_process = item;
					selected_process["command"] = short_file(entry.path()/"cmdline", 4096);
					selected_process["detail_status"] = "partial";
					selected_process["read_bytes_total"] = selected_process["write_bytes_total"] = nullptr;
					std::ifstream io(entry.path()/"io");
					for (std::string key; io >> key;) {
						std::uint64_t count; if (not (io >> count)) break;
						if (key == "read_bytes:") selected_process["read_bytes_total"] = count;
						if (key == "write_bytes:") selected_process["write_bytes_total"] = count;
					}
					const auto check = parse_process(read_text(entry.path()/"stat"),page_size_);
					if (check["start_time_ticks"] == value.start) selected_process["detail_status"] = "ok";
					else selected_process = {{"pid",pid},{"start_time_ticks",options.selected_start},{"detail_status","gone"}};
				}
			}
			++visible;
			items.push_back(std::move(item));
		} catch (...) {
			// A vanished PID is normal; persistent unreadable entries make the group partial.
			if (std::filesystem::exists(entry.path(), ec)) ++unreadable;
		}
	}
	for (const auto& [id,options] : requests) {
		auto& selected_process=selections[id];
		if (options.selected_pid and selected_process.value("detail_status",std::string{})=="unavailable") {
			std::error_code selected_error;
			if (not std::filesystem::exists(proc_/std::to_string(options.selected_pid),selected_error) and not selected_error) selected_process["detail_status"]="gone";
		}
	}
	const bool warm = processes_.empty();
	processes_ = std::move(current);
	Json results=Json::object();
	std::map<int,std::string> commands;
	for (const auto& [id,options] : requests) {
		std::vector<std::size_t> order;
		const Protocol::ProcessFilter filter(options.filter);
		for (std::size_t i=0; i<items.size(); ++i)
			if (filter.matches(items[i])) order.push_back(i);
		std::ranges::sort(order,[&](std::size_t ai,std::size_t bi) {
			const auto& a=items[ai]; const auto& b=items[bi];
			const auto field=options.sort=="rss" ? "rss_bytes" : options.sort=="cpu" ? "cpu_pct" : options.sort;
			int comparison=0;
			if (options.sort=="name" or options.sort=="user") {
				const auto av=a.value(field,std::string{}), bv=b.value(field,std::string{});
				comparison=av<bv ? -1 : av>bv ? 1 : 0;
			} else {
				const double av=a[field].is_null() ? -1 : a[field].get<double>(), bv=b[field].is_null() ? -1 : b[field].get<double>();
				comparison=av<bv ? -1 : av>bv ? 1 : 0;
			}
			if (comparison==0) return a["pid"].get<int>()<b["pid"].get<int>();
			const bool ascending=(options.sort=="pid" or options.sort=="name" or options.sort=="user") != options.reverse;
			return ascending ? comparison<0 : comparison>0;
		});
		const auto matched=order.size();
		if (order.size()>std::size_t(options.limit)) order.resize(options.limit);
		Json selected_items=Json::array();
		for (auto index : order) {
			auto item=items[index]; const int pid=item["pid"].get<int>();
			if (not commands.contains(pid)) commands[pid]=short_file(proc_/std::to_string(pid)/"cmdline",192);
			item["command"]=commands[pid]; selected_items.push_back(std::move(item));
		}
		results[id]=Protocol::group({{"items",selected_items},{"total_visible",visible},{"matched",matched},
			{"returned",selected_items.size()},{"truncated",matched>selected_items.size()},{"selected",selections[id]}},now,last_ ? now-last_ : 0,
			budget or unreadable or ec ? "partial" : warm ? "warming_up" : "ok",
			budget ? "Process scan exceeded 250 ms" : unreadable or ec ? "Some processes are unreadable" : "");
	}
	return results;
}

Json Linux::processes(const Protocol::Subscription& options,std::uint64_t now) {
	return processes_many({{"single",options}},now).at("single");
}

Json Linux::base_groups(const Protocol::Subscription& options, std::uint64_t now) {
	Json groups = Json::object();
	auto collect = [&](const char* name, auto&& fn) {
		try { groups[name] = fn(); }
		catch (const std::exception& e) { groups[name] = Protocol::group(nullptr, now, 0, "unavailable", e.what()); }
	};
	collect("cpu", [&] { return cpu(options.detail, now); });
	collect("memory", [&] { return memory(now); });
	collect("filesystems", [&] { return filesystems_->sample(options.mounts, now); });
	collect("disks", [&] { return disks(options, now); });
	collect("network", [&] { return network(options, now); });
	return groups;
}

Json Linux::sample(const Protocol::Subscription& options,std::uint64_t now) {
	auto groups=base_groups(options,now);
	if (options.processes) {
		try { groups["processes"]=processes(options,now); }
		catch (const std::exception& e) { groups["processes"]=Protocol::group(nullptr,now,0,"unavailable",e.what()); }
	}
	else processes_.clear();
	last_ = now;
	return groups;
}
Json Linux::sample_batch(const Protocol::Subscription& options,const std::map<std::string,Protocol::Subscription>& requests,std::uint64_t now) {
	if (requests.size()>8) throw std::runtime_error("At most eight process views per host");
	auto groups=base_groups(options,now);
	Json views=Json::object();
	if (requests.empty()) processes_.clear();
	else {
		try { views=processes_many(requests,now); }
		catch (const std::exception& e) {
			for (const auto& [id,request] : requests) views[id]=Protocol::group(nullptr,now,0,"unavailable",e.what());
		}
	}
	last_=now;
	return {{"groups",std::move(groups)},{"process_views",std::move(views)}};
}

}
