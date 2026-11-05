// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protocol/protocol.hpp"
#include <filesystem>
#include <sys/types.h>

namespace Collector {
class Filesystems {
	std::filesystem::path proc_;
	pid_t child_ = -1;
	int fd_ = -1;
	std::uint64_t started_ = 0, refreshed_ = 0;
	Protocol::Json cached_;
	std::vector<std::string> selection_;
	Protocol::Lines lines_;
	void close_worker();
public:
	explicit Filesystems(std::filesystem::path proc) : proc_(std::move(proc)) {}
	~Filesystems();
	Protocol::Json sample(const std::vector<std::string>& mounts, std::uint64_t now);
};
}
