// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "view.hpp"
#include <sys/types.h>

namespace Cluster {
class Session {
	std::vector<std::string> command_;
	pid_t child_ = -1;
	int input_ = -1, output_ = -1, errors_ = -1;
	Protocol::Lines lines_;
	std::string session_, stderr_, outgoing_, desired_, sent_;
	std::size_t offset_ = 0;
	std::uint64_t request_ = 0, seq_ = 0, started_ = 0, sent_at_ = 0, next_retry_ = 0, stable_since_ = 0;
	int retries_ = 0;
	bool hello_ = false, acknowledged_ = false, fatal_ = false;
	void start();
	void close_child();
	void fail(Host&, std::string message, bool fatal);
public:
	explicit Session(std::vector<std::string> command) : command_(std::move(command)) {}
	~Session();
	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;
	bool tick(Host&, Protocol::Subscription options);
	void reconnect(Host&);
};
}
