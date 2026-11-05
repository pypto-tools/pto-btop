// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace Protocol {
using Json = nlohmann::json;
constexpr std::size_t max_frame = 1024 * 1024;

struct Subscription {
	std::uint64_t request_id = 1;
	int interval_ms = 2000;
	bool detail = false;
	bool processes = false;
	std::string sort = "cpu", filter;
	int limit = 100;
	bool reverse = false;
	int selected_pid = 0;
	std::uint64_t selected_start = 0;
	std::vector<std::string> mounts, disks, interfaces;
};

std::uint64_t now_ms();
std::string utc_now();
Json document(std::string_view line);
Json parse(std::string_view line);
Subscription subscription(const Json& message);
Json encode(const Subscription& request);
Json group(Json data, std::uint64_t observed, std::uint64_t interval,
	std::string status = "ok", std::string reason = "");
void nonblocking(int fd);
bool write_line(int fd, const Json& value, int timeout_ms = 10000);

class Lines {
	std::string pending_;
public:
	std::vector<std::string> feed(std::string_view bytes);
	bool empty() const { return pending_.empty(); }
};
}
