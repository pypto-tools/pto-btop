// SPDX-License-Identifier: Apache-2.0
#include "cluster_version.hpp"
#include "collector/linux.hpp"
#include "https.hpp"
#include "protocol/protocol.hpp"

#include <csignal>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
std::string session_id() {
	std::ifstream file("/proc/sys/kernel/random/uuid");
	std::string value;
	if (not (file >> value)) throw std::runtime_error("Cannot generate session UUID");
	return value;
}
}

int main(int argc, char** argv) {
	using Protocol::Json;
	try {
		if (argc == 2 and std::string_view(argv[1]) == "--version") { std::cout << "pto-btop-agent " BTOP_CLUSTER_VERSION "\n"; return 0; }
		if (argc == 2 and std::string_view(argv[1]) == "--help") {
			std::cout << "btop-agent --stdio  Version 1 JSON Lines over stdin/stdout\n"
				"btop-agent --once   One real snapshot including all detail groups\n"
				"btop-agent --config agent.json   Upload to an HTTPS hub\n";
			return 0;
		}
		if (argc==3 and std::string_view(argv[1])=="--config") {
			std::signal(SIGPIPE,SIG_IGN); std::signal(SIGINT,stop); std::signal(SIGTERM,stop); std::signal(SIGHUP,stop);
			return run_https_agent(argv[2],stopped);
		}
		if (argc != 2 or (std::string_view(argv[1]) != "--stdio" and std::string_view(argv[1]) != "--once"))
			throw std::runtime_error("Use --stdio or --once");
		std::signal(SIGPIPE, SIG_IGN);
		std::signal(SIGINT, stop);
		std::signal(SIGTERM, stop);
		std::signal(SIGHUP, stop);
		Protocol::nonblocking(STDOUT_FILENO);
		Collector::Linux collector;
		const auto session = session_id();
		Protocol::Subscription options;
		std::uint64_t seq = 0;
		auto snapshot = [&] {
			auto groups = collector.sample(options);
			return Json{{"type", "snapshot"}, {"version", 1}, {"session_id", session},
				{"subscription_id", options.request_id}, {"seq", ++seq},
				{"monotonic_ms", Protocol::now_ms()}, {"wall_time", Protocol::utc_now()}, {"groups", groups}};
		};
		if (std::string_view(argv[1]) == "--once") {
			options.detail = options.processes = true;
			collector.sample(options);
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			return Protocol::write_line(STDOUT_FILENO, snapshot()) ? 0 : 1;
		}
		Protocol::nonblocking(STDIN_FILENO);
		if (not Protocol::write_line(STDOUT_FILENO, collector.hello(session))) return 0;
		Protocol::Lines lines;
		bool subscribed = false;
		std::uint64_t next = 0, last_request = 0;
		const auto first_deadline = Protocol::now_ms() + 10000;
		while (not stopped) {
			pollfd input{STDIN_FILENO, POLLIN, 0};
			poll(&input, 1, 50);
			if (input.revents & (POLLIN | POLLHUP)) {
				char buffer[8192];
				const auto n = read(STDIN_FILENO, buffer, sizeof(buffer));
				if (n == 0) return 0;
				if (n > 0) {
					try {
						for (const auto& line : lines.feed({buffer, static_cast<std::size_t>(n)})) {
							const auto message = Protocol::parse(line);
							if (message.at("type") == "stop") return 0;
							if (message.at("type") != "subscribe") throw std::runtime_error("Unknown message type");
							try {
								auto replacement = Protocol::subscription(message);
								if (replacement.request_id <= last_request) throw std::runtime_error("request_id must increase");
								options = std::move(replacement);
								last_request = options.request_id;
								auto effective = Protocol::encode(options);
								effective.erase("type"); effective.erase("version"); effective.erase("request_id");
								if (not Protocol::write_line(STDOUT_FILENO, {{"type", "subscribed"}, {"version", 1},
									{"session_id", session}, {"request_id", options.request_id}, {"effective", effective}})) return 0;
								subscribed = true; next = 0;
							} catch (const std::exception& e) {
								if (not Protocol::write_line(STDOUT_FILENO, {{"type", "error"}, {"version", 1},
									{"session_id", session}, {"code", "INVALID_SUBSCRIPTION"}, {"message", e.what()}, {"fatal", false}})) return 0;
							}
						}
					} catch (const std::exception& e) {
						Protocol::write_line(STDOUT_FILENO, {{"type", "error"}, {"version", 1}, {"session_id", session},
							{"code", "PROTOCOL_ERROR"}, {"message", e.what()}, {"fatal", true}});
						return 1;
					}
				}
			}
			if (not subscribed and Protocol::now_ms() >= first_deadline) return 0;
			if (subscribed and Protocol::now_ms() >= next) {
				if (not Protocol::write_line(STDOUT_FILENO, snapshot())) return 0;
				next = Protocol::now_ms() + options.interval_ms;
			}
		}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "btop-agent: " << e.what() << '\n';
		return 1;
	}
}
