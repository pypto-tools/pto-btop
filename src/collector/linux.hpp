// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protocol/protocol.hpp"
#include <filesystem>
#include <map>
#include <memory>
#include <optional>

namespace Collector {
using Protocol::Json;
struct CpuTimes { std::uint64_t total = 0, idle = 0, iowait = 0, user = 0, system = 0; };
CpuTimes parse_cpu(std::string_view fields);
std::optional<double> cpu_usage(CpuTimes before, CpuTimes after);
Json parse_process(std::string_view stat, std::uint64_t page_size);

class Filesystems;
class Resources;
class Linux {
	std::filesystem::path proc_, sys_;
	std::map<std::string, CpuTimes> cpus_;
	struct Counter { std::uint64_t read, write, time, busy = 0; };
	std::map<std::string, Counter> disks_, nets_;
	struct Process { std::uint64_t start, ticks, time; };
	std::map<int, Process> processes_;
	std::unique_ptr<Filesystems> filesystems_;
	std::unique_ptr<Resources> resources_;
	std::uint64_t last_ = 0, ticks_, page_size_;
	std::string model_;
	std::map<unsigned, std::string> users_;
	Json cpu(bool detail, std::uint64_t now);
	Json memory(std::uint64_t now);
	Json disks(const Protocol::Subscription&, std::uint64_t now);
	Json network(const Protocol::Subscription&, std::uint64_t now);
	Json base_groups(const Protocol::Subscription&, std::uint64_t now);
	Json processes_many(const std::map<std::string,Protocol::Subscription>&, std::uint64_t now);
	Json processes(const Protocol::Subscription&, std::uint64_t now);
public:
	explicit Linux(std::filesystem::path proc = "/proc", std::filesystem::path sys = "/sys",
		std::uint64_t ticks = 0, std::uint64_t page_size = 0);
	~Linux();
	Json sample(const Protocol::Subscription&, std::uint64_t now = Protocol::now_ms());
	Json sample_batch(const Protocol::Subscription&, const std::map<std::string,Protocol::Subscription>&,
		std::uint64_t now = Protocol::now_ms());
	Json hello(const std::string& session) const;
};
}
