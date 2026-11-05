// SPDX-License-Identifier: Apache-2.0

#include "cluster_version.hpp"
#include "terminal.hpp"
#include "view.hpp"
#include "session.hpp"
#include "https.hpp"
#include <memory>
#include <regex>
#include <sstream>
#include <algorithm>
#include <thread>

#include <charconv>
#include <chrono>
#include <clocale>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

#include "btop_theme.hpp"
#include "btop_tools.hpp"

namespace {
volatile std::sig_atomic_t interrupted = 0;
volatile std::sig_atomic_t suspended = 0;
void stop(int) { interrupted = 1; }
void suspend(int) { suspended = 1; }

std::string read_key() {
	char first;
	if (read(STDIN_FILENO,&first,1)!=1) return "";
	if (first!='\x1b') return std::string(1,first);
	std::string sequence;
	for (int i=0; i<12; ++i) {
		pollfd input{STDIN_FILENO,POLLIN,0};
		if (poll(&input,1,25)<=0) break;
		char next; if (read(STDIN_FILENO,&next,1)!=1) break;
		sequence+=next;
		if (sequence.size()>1 and ((next>='A' and next<='Z') or next=='~')) break;
	}
	if (sequence=="[A" or sequence=="OA") return "up";
	if (sequence=="[B" or sequence=="OB") return "down";
	if (sequence=="[5~") return "pageup";
	if (sequence=="[6~") return "pagedown";
	if (sequence=="[H" or sequence=="OH") return "home";
	if (sequence=="[F" or sequence=="OF") return "end";
	return sequence.empty() ? "escape" : "ignored";
}

int positive(std::string_view value, int limit) {
	int result = 0;
	const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
	if (error != std::errc{} or end != value.data() + value.size() or result < 1 or result > limit)
		throw std::invalid_argument("Invalid numeric option");
	return result;
}
}

int main(int argc, char** argv) {
	try {
		std::setlocale(LC_ALL, "");
		bool demo = false, local = false, once = false, initial_detail = false, check = false;
		std::vector<std::string> aliases;
		std::string agent_path, https_config;
		int width = 120, height = 36, interval = 2000;
		Theme::Options theme;
		for (int i = 1; i < argc; ++i) {
			const std::string_view arg = argv[i];
			if (arg == "--version") { std::cout << "pto-btop " BTOP_CLUSTER_VERSION "\n"; return 0; }
			else if (arg == "--demo") demo = true;
			else if (arg == "--once") once = true;
			else if (arg == "--check") check = true;
			else if (arg == "--local") local = true;
			else if (arg == "--detail") initial_detail = true;
			else if (arg == "--low-color") theme.lowcolor = true;
			else if (arg == "--help") {
				std::cout << "pto-btop " BTOP_CLUSTER_VERSION " live monitor\n"
					"  --version          Print the release version\n"
					"  --config FILE      Read an HTTPS hub configuration\n"
					"  --check            Check live connectivity without entering the TUI\n"
					"  --demo             Four simulated hosts\n"
					"  --local            Collect this machine through a local agent\n"
					"  --ssh ALIAS        Collect over SSH (repeat up to 4 times)\n"
					"  --agent-path PATH  Override the local or remote agent executable\n"
					"  --detail           Start with cpu/mem/disks/net/proc details\n"
					"  --once             Print one ANSI frame (live mode waits for two samples)\n"
					"  --width N --height N  Frame dimensions for --once (default 120x36)\n"
					"  --theme Default|TTY|PATH  Original btop theme file\n"
					"  --low-color        Use 256 colors\n"
					"  --interval-ms N    Sample interval, 1000-10000 (default 2000)\n";
				return 0;
			} else if (arg == "--width" or arg == "--height" or arg == "--theme" or arg == "--interval-ms" or arg == "--ssh" or arg == "--agent-path" or arg == "--config") {
				if (++i == argc) throw std::invalid_argument("Missing option value");
				if (arg == "--width") width = positive(argv[i], 1000);
				else if (arg == "--height") height = positive(argv[i], 500);
				else if (arg == "--interval-ms") interval = positive(argv[i], 10000);
				else if (arg == "--ssh") aliases.emplace_back(argv[i]);
				else if (arg == "--agent-path") agent_path = argv[i];
				else if (arg == "--config") https_config = argv[i];
				else theme.name = argv[i];
			} else throw std::invalid_argument("Unknown option: " + std::string(arg));
		}
		if (int(demo) + int(local) + int(not aliases.empty()) + int(not https_config.empty()) != 1)
			throw std::invalid_argument("Choose --config FILE, --demo, --local or --ssh ALIAS");
		if (check and demo) throw std::invalid_argument("--check requires live collection");
		if (not https_config.empty() and not agent_path.empty()) throw std::invalid_argument("--agent-path does not apply to HTTPS");
		if (aliases.size()>4) throw std::invalid_argument("At most four SSH hosts");
		for (const auto& alias : aliases)
			if (not std::regex_match(alias,std::regex("[A-Za-z0-9_][A-Za-z0-9_.-]*"))) throw std::invalid_argument("Invalid SSH alias");
		if (agent_path.empty()) agent_path = local ? (std::filesystem::canonical("/proc/self/exe").parent_path()/"btop-agent").string() : "/opt/btop-cluster/bin/btop-agent";
		if (not aliases.empty() and not std::regex_match(agent_path,std::regex("/[A-Za-z0-9_./-]+")))
			throw std::invalid_argument("Remote agent path must be absolute and shell-safe");
		if (interval < 1000) throw std::invalid_argument("Interval must be at least 1000 ms");
		Theme::themes = {"Default", "TTY"};
		if (theme.name != "Default" and theme.name != "TTY") {
			if (not std::filesystem::is_regular_file(theme.name)) throw std::invalid_argument("Theme file not found");
			Theme::themes.push_back(theme.name);
		}
		theme.tty_mode = theme.name == "TTY";
		Theme::setTheme(theme);
		Draw::Style style{theme.tty_mode, true, theme.tty_mode ? "tty" : "braille"};
		std::signal(SIGPIPE,SIG_IGN);
		std::vector<Cluster::Host> hosts;
		std::vector<std::unique_ptr<Cluster::Session>> sessions;
		std::unique_ptr<Cluster::HttpsFeed> https;
		if (demo) hosts=Cluster::demo_hosts(interval);
		else if (not https_config.empty()) https=std::make_unique<Cluster::HttpsFeed>(https_config,hosts);
		else {
			if (local) aliases={"localhost"};
			for (const auto& alias : aliases) {
				Cluster::Host host;
				host.name=alias; host.simulated=false; host.cpu_valid=host.memory_valid=false; host.status="CONNECTING";
				hosts.push_back(std::move(host));
				std::vector<std::string> command;
				if (local) command={agent_path,"--stdio"};
				else command={"ssh","-T","-o","BatchMode=yes","-o","StrictHostKeyChecking=yes",
					"-o","ConnectTimeout=5","-o","ServerAliveInterval=5","-o","ServerAliveCountMax=2",alias,agent_path,"--stdio"};
				sessions.push_back(std::make_unique<Cluster::Session>(std::move(command)));
			}
		}
		int selected = 0;
		bool detail = initial_detail, dirty = true, editing_filter = false, help = false;
		std::string filter_draft;
		auto collect = [&] {
			bool changed=false;
			std::vector<Protocol::Subscription> requests;
			for (std::size_t i=0; i<hosts.size(); ++i) {
				Protocol::Subscription request;
				request.interval_ms=interval;
				request.detail=request.processes=detail and i==std::size_t(selected);
				request.sort=hosts[i].sort; request.filter=hosts[i].filter;
				request.limit=500; request.reverse=hosts[i].reverse;
				if (request.processes and hosts[i].proc_detail) {
					request.selected_pid=hosts[i].selected_pid;
					request.selected_start=hosts[i].selected_start;
				}
				requests.push_back(request);
				if (not https) changed=sessions[i]->tick(hosts[i],request) or changed;
			}
			return https ? https->tick(hosts,requests) : changed;
		};
		if (once or check) {
			bool ready=demo;
			if (not demo) {
				const auto deadline=Protocol::now_ms()+std::max(12000,interval*3);
				while (Protocol::now_ms()<deadline) {
					collect();
					const bool samples_ready=std::ranges::all_of(hosts,[](const auto& host){return host.cpu_valid and host.memory_valid and host.received_ms>0 and host.status=="ONLINE";});
					const auto& selected_host=hosts[selected];
					const bool processes_ready=not https or not detail or (selected_host.groups.contains("processes") and selected_host.groups.at("processes").value("status",std::string{})!="warming_up");
					ready=samples_ready and processes_ready;
					if (ready) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
			}
			if (check) {
				for (const auto& host : hosts) std::cout<<Cluster::safe_text(host.name,64)<<"  "<<host.status
					<<"  CPU="<<(host.cpu_valid ? std::to_string(host.cpu)+"%" : "N/A")
					<<"  cores="<<host.cores<<"  "<<Cluster::safe_text(host.error,160)<<'\n';
			} else std::cout << Cluster::render(hosts,width,height,selected,detail,style) << '\n';
			return ready ? 0 : 1;
		}
		std::signal(SIGINT, stop);
		std::signal(SIGTERM, stop);
		std::signal(SIGHUP, stop);
		std::signal(SIGTSTP, suspend);
		Cluster::Terminal terminal;
		unsigned tick = 300;
		using Clock = std::chrono::steady_clock;
		auto next = Clock::now() + std::chrono::milliseconds(interval);
		while (not interrupted) {
			if (suspended) {
				terminal.leave();
				suspended = 0;
				std::raise(SIGSTOP);
				terminal.enter();
				dirty = true;
			}
			const auto [new_width, new_height] = terminal.size();
			if (width != new_width or height != new_height) {
				width = new_width; height = new_height; dirty = true;
			}
			if (not demo) dirty=collect() or dirty;
			if (demo and Clock::now() >= next) {
				Cluster::advance_demo(hosts, tick++, interval);
				next = Clock::now() + std::chrono::milliseconds(interval);
				dirty = true;
			}
			if (dirty) {
				std::ostringstream frame;
				frame << Cluster::render(hosts, width, height, selected, detail, style);
				if (help and width>=80 and height>=24) {
					const int x=4,y=5,w=std::min(width-8,100),h=16;
					frame << Draw::createBox(x,y,w,h,Theme::c("hi_fg"),true,"Keys","? / Esc close",0,style);
					int row=y+1;
					for (const auto& text : {"Tab / 1-4: switch host    Esc: overview    q: quit",
						"Up/Down or j/k: select process   PgUp/PgDn: scroll   Home/End",
						"Enter: toggle selected process details (PID + start time)",
						"c / m: CPU / memory sort   s: cycle sort   R: reverse",
						"T: tree of returned processes   Space: collapse / expand",
						"/: name or user:TEXT substring   Enter: apply   Esc: cancel",
						"[ / ]: core pages   g: total / user / system / iowait",
						"d: next disk page   i: disk capacity / I/O graphs",
						"b / n: previous / next interface   a: auto / 10MiB/s scale",
						"y: shared / separate RX/TX scale   z: reset displayed totals",
						"t: TTY theme   r: reconnect   ?: this help",
						"Top 500 processes; tree may have missing ancestors.",
						"CPU process usage: one logical core = 100%."})
						frame << Mv::to(row++,x+1) << Theme::c("main_fg") << Cluster::safe_text(text,w-2);
				}
				if (editing_filter and width>=80 and height>=24)
					frame << "\x1b[" << height-1 << ";2H" << std::string(width-3,' ') << "\x1b[" << height-1 << ";2HFilter (name / user:TEXT): " << Cluster::safe_text(filter_draft,width-29);
				terminal.present(frame.str(),width,height);
				dirty = false;
			}
			terminal.flush();
			pollfd input{STDIN_FILENO, POLLIN, 0};
			if (poll(&input, 1, 50) <= 0) continue;
			if (input.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
			const auto input_key=read_key();
			if (input_key.empty()) continue;
			const char key=input_key.size()==1 ? input_key.front() : '\0';
			if (help) { if (key=='?' or key=='q' or input_key=="escape") help=false; dirty=true; continue; }
			if (editing_filter) {
				if (key=='\r' or key=='\n') { hosts[selected].filter=filter_draft; editing_filter=false; }
				else if (input_key=="escape") editing_filter=false;
				else if (key==127 or key=='\b') {
					if (not filter_draft.empty()) {
						auto pos=filter_draft.size()-1;
						while (pos>0 and (static_cast<unsigned char>(filter_draft[pos])&0xc0)==0x80) --pos;
						filter_draft.erase(pos);
					}
				}
				else if (static_cast<unsigned char>(key)>=32 and filter_draft.size()<128) filter_draft+=key;
				hosts[selected].proc_page=0; dirty=true; continue;
			}
			if (key == 'q') break;
			if (key == '\t') selected = (selected + 1) % hosts.size();
			else if (key == '\r' or key == '\n') { if (detail) hosts[selected].proc_detail=not hosts[selected].proc_detail; else detail=true; }
			else if (input_key=="escape") { if (hosts[selected].proc_detail) hosts[selected].proc_detail=false; else detail=false; }
			else if (key >= '1' and key < '1'+int(hosts.size())) { selected = key - '1'; detail = true; }
			else if (detail and key=='c') hosts[selected].sort="cpu";
			else if (detail and key=='m') hosts[selected].sort="rss";
			else if (detail and key=='/') { editing_filter=true; filter_draft=hosts[selected].filter; }
			else if (detail and (key=='j' or input_key=="down")) Cluster::move_process(hosts[selected],1);
			else if (detail and (key=='k' or input_key=="up")) Cluster::move_process(hosts[selected],-1);
			else if (detail and input_key=="pagedown") Cluster::move_process(hosts[selected],10);
			else if (detail and input_key=="pageup") Cluster::move_process(hosts[selected],-10);
			else if (detail and input_key=="home") Cluster::move_process(hosts[selected],-10000);
			else if (detail and input_key=="end") Cluster::move_process(hosts[selected],10000);
			else if (detail and key=='T') { hosts[selected].proc_tree=not hosts[selected].proc_tree; hosts[selected].collapsed.clear(); }
			else if (detail and key==' ') Cluster::toggle_process(hosts[selected]);
			else if (detail and key=='R') hosts[selected].reverse=not hosts[selected].reverse;
			else if (detail and key=='s') {
				const std::vector<std::string> sorts={"cpu","rss","pid","name","user","threads"};
				const auto current=std::ranges::find(sorts,hosts[selected].sort)-sorts.begin();
				hosts[selected].sort=sorts[(current+1)%sorts.size()];
			}
			else if (detail and key=='g') {
				const std::vector<std::string> metrics={"usage_pct","user_pct","system_pct","iowait_pct"};
				const auto current=std::ranges::find(metrics,hosts[selected].cpu_metric)-metrics.begin();
				hosts[selected].cpu_metric=metrics[(current+1)%metrics.size()];
			}
			else if (detail and key==']') ++hosts[selected].core_page;
			else if (detail and key=='[') hosts[selected].core_page=std::max(0,hosts[selected].core_page-1);
			else if (detail and key=='n') ++hosts[selected].net_index;
			else if (detail and key=='b') hosts[selected].net_index=std::max(0,hosts[selected].net_index-1);
			else if (detail and key=='a') hosts[selected].net_auto=not hosts[selected].net_auto;
			else if (detail and key=='y') hosts[selected].net_sync=not hosts[selected].net_sync;
			else if (detail and key=='z') {
				const auto& items=Cluster::metric_items(Cluster::metric_data(hosts[selected],"network"));
				if (not items.empty()) {
					const auto& item=items[hosts[selected].net_index%items.size()];
					hosts[selected].network_offset[item.value("name",std::string{})]={item.value("rx_bytes_total",0.0),item.value("tx_bytes_total",0.0)};
				}
			}
			else if (detail and key=='d') ++hosts[selected].disk_page;
			else if (detail and key=='i') { hosts[selected].disk_io=not hosts[selected].disk_io; hosts[selected].disk_page=0; }
			else if (key=='?') help=true;
			else if (key=='r' and not demo) {if (https) https->reconnect(); else sessions[selected]->reconnect(hosts[selected]);}
			else if (key == 't') {
				theme.tty_mode = not theme.tty_mode;
				Theme::setTheme(theme);
				style.tty_mode = theme.tty_mode;
				style.graph_symbol = theme.tty_mode ? "tty" : "braille";
			}
			dirty = true;
		}
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "btop-cluster: " << error.what() << '\n';
		return 1;
	}
}
