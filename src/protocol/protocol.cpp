// SPDX-License-Identifier: Apache-2.0
#include "protocol.hpp"

#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace Protocol {
std::uint64_t now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string utc_now() {
	const auto now = std::time(nullptr);
	std::tm value{};
	gmtime_r(&now, &value);
	char output[32];
	std::strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &value);
	return output;
}

Json parse(std::string_view line) {
	if (line.size() > max_frame) throw std::runtime_error("Frame exceeds 1 MiB");
	std::vector<std::set<std::string>> keys;
	auto result = Json::parse(line, [&](int depth, Json::parse_event_t event, Json& value) {
		if (depth > 16) throw std::runtime_error("JSON nesting exceeds 16");
		if (value.is_string() and value.get_ref<const std::string&>().size() > 4096)
			throw std::runtime_error("String exceeds 4096 bytes");
		if (event == Json::parse_event_t::object_start) keys.emplace_back();
		else if (event == Json::parse_event_t::object_end) keys.pop_back();
		else if (event == Json::parse_event_t::key and not keys.back().insert(value.get<std::string>()).second)
			throw std::runtime_error("Duplicate JSON key");
		return true;
	});
	if (not result.is_object() or not result.contains("version") or
		not result["version"].is_number_integer() or result["version"] != 1 or
		not result.contains("type") or not result["type"].is_string())
		throw std::runtime_error("Invalid protocol version or message type");
	return result;
}

Subscription subscription(const Json& m) {
	if (m.at("type") != "subscribe") throw std::runtime_error("Expected subscribe");
	Subscription out;
	if (not m.at("request_id").is_number_unsigned() or m.at("request_id") == 0)
		throw std::runtime_error("request_id must be positive");
	out.request_id = m.at("request_id").get<std::uint64_t>();
	if (not m.at("interval_ms").is_number_integer() or m.at("interval_ms") < 1000 or m.at("interval_ms") > 10000)
		throw std::runtime_error("interval_ms must be 1000..10000");
	out.interval_ms = m.at("interval_ms").get<int>();
	out.detail = m.at("detail").get<bool>();
	const auto& proc = m.at("processes");
	out.processes = proc.at("enabled").get<bool>();
	out.sort = proc.at("sort").get<std::string>();
	out.filter = proc.at("filter").get<std::string>();
	if (not proc.at("limit").is_number_integer() or proc.at("limit") < 1 or proc.at("limit") > 500)
		throw std::runtime_error("Process limit must be 1..500");
	out.limit = proc.at("limit").get<int>();
	if ((out.sort != "cpu" and out.sort != "rss" and out.sort != "pid" and out.sort != "name" and out.sort != "user" and out.sort != "threads") or out.filter.size() > 128)
		throw std::runtime_error("Invalid process sort or filter");
	if (proc.contains("reverse")) out.reverse = proc.at("reverse").get<bool>();
	if (proc.contains("selected_pid")) {
		if (not proc["selected_pid"].is_number_integer() or proc["selected_pid"] < 0 or proc["selected_pid"] > 4194304)
			throw std::runtime_error("Invalid selected PID");
		out.selected_pid = proc["selected_pid"].get<int>();
	}
	if (proc.contains("selected_start")) {
		if (not proc["selected_start"].is_number_unsigned()) throw std::runtime_error("Invalid process identity");
		out.selected_start = proc["selected_start"].get<std::uint64_t>();
	}
	if (out.selected_pid and not proc.contains("selected_start")) throw std::runtime_error("Selected process requires start time");
	for (auto [key, dest] : {std::pair{"mounts", &out.mounts}, {"disk_devices", &out.disks}, {"network_interfaces", &out.interfaces}}) {
		const auto& list = m.at(key);
		if (not list.is_array() or list.size() > 256) throw std::runtime_error("Invalid selection list");
		for (const auto& item : list) {
			auto name = item.get<std::string>();
			if (name.empty() or name.size() > 4096) throw std::runtime_error("Invalid selection name");
			dest->push_back(name);
		}
	}
	return out;
}

Json encode(const Subscription& s) {
	return {{"type", "subscribe"}, {"version", 1}, {"request_id", s.request_id},
		{"interval_ms", s.interval_ms}, {"detail", s.detail},
		{"processes", {{"enabled", s.processes}, {"sort", s.sort}, {"filter", s.filter}, {"limit", s.limit},
			{"reverse", s.reverse}, {"selected_pid", s.selected_pid}, {"selected_start", s.selected_start}}},
		{"mounts", s.mounts}, {"disk_devices", s.disks}, {"network_interfaces", s.interfaces}};
}

Json group(Json data, std::uint64_t observed, std::uint64_t interval, std::string status, std::string reason) {
	Json out{{"status", status}, {"observed_monotonic_ms", observed}, {"interval_ms", interval}, {"data", std::move(data)}};
	if (not reason.empty()) out["reason"] = reason;
	return out;
}

void nonblocking(int fd) {
	const int flags = fcntl(fd, F_GETFL);
	if (flags < 0 or fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) throw std::runtime_error("Cannot set nonblocking pipe");
}

bool write_line(int fd, const Json& value, int timeout_ms) {
	const auto line = value.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
	if (line.size() > max_frame) throw std::runtime_error("Outgoing frame exceeds 1 MiB");
	const auto deadline = now_ms() + timeout_ms;
	std::size_t offset = 0;
	while (offset < line.size()) {
		const auto count = write(fd, line.data() + offset, line.size() - offset);
		if (count > 0) { offset += count; continue; }
		if (count < 0 and errno == EINTR) continue;
		if (count < 0 and errno != EAGAIN and errno != EWOULDBLOCK) return false;
		if (now_ms() >= deadline) return false;
		pollfd p{fd, POLLOUT, 0};
		poll(&p, 1, 50);
	}
	return true;
}

std::vector<std::string> Lines::feed(std::string_view bytes) {
	std::vector<std::string> result;
	for (char c : bytes) {
		if (c == '\n') { result.push_back(std::move(pending_)); pending_.clear(); }
		else {
			if (pending_.size() >= max_frame) throw std::runtime_error("Frame exceeds 1 MiB");
			pending_ += c;
		}
	}
	return result;
}
}
