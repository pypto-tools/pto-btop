// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protocol/protocol.hpp"
#include <filesystem>
#include <sys/types.h>

namespace Collector {
Protocol::Json login_sessions(const std::filesystem::path& path);
class Resources {
	std::filesystem::path utmp_, command_;
	pid_t child_=-1;
	int fd_=-1;
	std::uint64_t started_=0;
	std::string output_;
	Protocol::Json cached_;
	void finish(bool stop);
public:
	Resources(std::filesystem::path utmp="/run/utmp", std::filesystem::path command="/usr/local/bin/task-submit")
		: utmp_(std::move(utmp)), command_(std::move(command)) {}
	~Resources();
	Protocol::Json sample(std::uint64_t now=Protocol::now_ms());
};
}
