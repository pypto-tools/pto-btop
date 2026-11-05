// SPDX-License-Identifier: Apache-2.0
#include "filesystems.hpp"
#include <algorithm>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Collector {
using Protocol::Json;
namespace {
std::string unescape(std::string value) {
	for (std::size_t i = 0; i + 3 < value.size(); ++i) {
		if (value[i] == '\\' and value[i + 1] >= '0' and value[i + 1] <= '7' and
			value[i + 2] >= '0' and value[i + 2] <= '7' and value[i + 3] >= '0' and value[i + 3] <= '7')
			value.replace(i, 4, 1, (value[i+1]-'0')*64 + (value[i+2]-'0')*8 + value[i+3]-'0');
	}
	return value;
}

Json query(const std::filesystem::path& proc, const std::vector<std::string>& selection) {
	std::ifstream file(proc / "self/mountinfo");
	Json items = Json::array();
	std::set<std::string> seen;
	std::size_t failures = 0;
	for (std::string line; std::getline(file, line);) {
		std::istringstream input(line);
		std::string id, parent, device, root, mount, ignored, type, source;
		if (not (input >> id >> parent >> device >> root >> mount)) continue;
		while (input >> ignored and ignored != "-") {}
		input >> type >> source;
		mount = unescape(mount);
		if (not selection.empty()) {
			if (std::ranges::find(selection, mount) == selection.end()) continue;
		} else if (mount != "/" and type != "ext4" and type != "xfs" and type != "btrfs" and type != "ext3") continue;
		// Deduplicate a mount path; do not sum bind mounts or capacity across filesystems.
		if (not seen.insert(mount).second) continue;
		struct statvfs stats{};
		if (statvfs(mount.c_str(), &stats) != 0) { ++failures; continue; }
		items.push_back({{"mount", mount}, {"device", device}, {"fstype", type}, {"source", unescape(source)},
			{"total_bytes", std::uint64_t(stats.f_blocks) * stats.f_frsize},
			{"free_bytes", std::uint64_t(stats.f_bfree) * stats.f_frsize},
			{"used_bytes", std::uint64_t(stats.f_blocks - stats.f_bfree) * stats.f_frsize},
			{"available_bytes", std::uint64_t(stats.f_bavail) * stats.f_frsize}});
		if (items.size() >= 256) { ++failures; break; }
	}
	for (const auto& mount : selection) if (not seen.contains(mount)) ++failures;
	auto result = Protocol::group({{"items", items}}, Protocol::now_ms(), 0,
		items.empty() ? "unavailable" : failures ? "partial" : "ok");
	if (failures) result["reason"] = "Some selected mounts are missing or unreadable";
	return result;
}
}

void Filesystems::close_worker() {
	if (fd_ >= 0) close(fd_);
	fd_ = -1;
}

Filesystems::~Filesystems() {
	close_worker();
	if (child_ > 0) { kill(child_, SIGKILL); waitpid(child_, nullptr, WNOHANG); }
}

Json Filesystems::sample(const std::vector<std::string>& mounts, std::uint64_t now) {
	if (cached_.is_null()) cached_ = Protocol::group(nullptr, now, 0, "warming_up");
	if (fd_ >= 0) {
		const bool exited = waitpid(child_, nullptr, WNOHANG) == child_;
		char buffer[8192];
		for (ssize_t n; (n = read(fd_, buffer, sizeof(buffer))) > 0;) {
			for (const auto& line : lines_.feed({buffer, static_cast<std::size_t>(n)})) {
				cached_ = Json::parse(line);
				refreshed_ = now;
			}
		}
		if (not exited and now - started_ > 1000 and refreshed_ < started_) {
			cached_ = Protocol::group(nullptr, now, 0, "timeout", "Filesystem capacity query exceeded 1 second");
			kill(child_, SIGKILL);
		}
		if (exited) {
			if (refreshed_ < started_) cached_ = Protocol::group(nullptr, now, 0, "timeout", "Filesystem worker exited without a result");
			child_ = -1; close_worker();
		}
	}
	if (child_ < 0 and (now - started_ >= 10000 or started_ == 0 or mounts != selection_)) {
		int pipefd[2];
		if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) != 0) return Protocol::group(nullptr, now, 0, "unavailable", "Cannot create filesystem pipe");
		selection_ = mounts;
		lines_ = Protocol::Lines{};
		started_ = now;
		const auto parent = getpid();
		child_ = fork();
		if (child_ == 0) {
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			if (getppid() != parent) _exit(1);
			std::signal(SIGALRM, SIG_DFL);
			alarm(1);
			close(pipefd[0]); close(STDIN_FILENO); close(STDOUT_FILENO);
			try { Protocol::write_line(pipefd[1], query(proc_, mounts), 1000); } catch (...) {}
			close(pipefd[1]); _exit(0);
		}
		close(pipefd[1]);
		if (child_ < 0) { close(pipefd[0]); return Protocol::group(nullptr, now, 0, "unavailable", "Cannot start filesystem worker"); }
		fd_ = pipefd[0];
	}
	return cached_;
}
}
