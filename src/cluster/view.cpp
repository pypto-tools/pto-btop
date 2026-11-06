// SPDX-License-Identifier: Apache-2.0

#include "view.hpp"
#include "protocol/process_filter.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <fmt/format.h>

#include "btop_theme.hpp"
#include "btop_tools.hpp"

namespace Cluster {
namespace {

constexpr double gib = 1024.0 * 1024 * 1024;

std::string at(int row, int col, const std::string& text) {
	return Mv::to(row, col) + Theme::c("main_fg") + text;
}

std::string amount(const Protocol::Json& value,const char* key) {
	const auto found=value.find(key);
	return found!=value.end() and found->is_number_integer() ? std::to_string(found->get<std::uint64_t>()) : "--";
}

// Derive filled chip colors from the active theme, including 256-color and TTY modes.
std::string chip_background(const std::string& color,const Draw::Style& style) {
	const auto& escape=Theme::c(color);
	if (style.tty_mode or escape.find("38;")==std::string::npos)
		return color=="cpu_end" ? "\x1b[41m" : color=="cpu_mid" ? "\x1b[43m" : "\x1b[100m";
	const auto accent=Theme::dec(color); const auto background=Theme::dec("main_bg");
	std::array<int,3> blended;
	for (int i=0;i<3;++i) blended[i]=std::clamp((std::max(0,background[i])*2+std::max(0,accent[i])*3)/5,0,255);
	return Theme::dec_to_color(blended[0],blended[1],blended[2],escape.find("38;5;")!=std::string::npos,"bg");
}

std::string card_chip(const Protocol::Json& device,int x,int y,int id_width,bool rich,bool fresh,const Draw::Style& style) {
	const auto state=fresh ? device.at("state").get<std::string>() : "unknown";
	const bool held=state=="allocated" or state=="external", reserved=state=="reserved", failed=state=="error";
	const std::string color=held or failed ? "cpu_end" : reserved ? "cpu_mid" : "graph_text";
	const auto mark=state=="free" ? "F" : state=="allocated" ? "H" : reserved ? "R" : state=="disabled" ? "X" : failed ? "!" : state=="external" ? "E" : "?";
	auto id=safe_text(device.at("id").get<std::string>(),id_width);
	id=std::string(std::max(0,id_width-int(id.size())),' ')+id;
	const auto label=rich ? " "+id+" "+mark+" " : id+mark;
	const auto foreground=Theme::c(held or reserved or failed ? "selected_fg" : "main_fg");
	const auto fill=chip_background(color,style);
	const auto reset=Theme::c("main_bg")+Theme::c(color);
	if (not rich) return Mv::to(y,x)+reset+"["+fill+foreground+label+reset+"]"+Theme::c("main_fg");
	std::string top=style.tty_mode ? "+" : "┌", bottom=style.tty_mode ? "+" : "└";
	for (int i=0;i<int(label.size());++i) {
		top+=style.tty_mode ? "-" : i%2 ? "┴" : "─";
		bottom+=style.tty_mode ? "-" : i%2 ? "┬" : "─";
	}
	top+=style.tty_mode ? "+" : "┐"; bottom+=style.tty_mode ? "+" : "┘";
	const auto side=style.tty_mode ? "|" : "│";
	return Mv::to(y,x)+reset+top+Mv::to(y+1,x)+side+fill+foreground+label+reset+side+
		Mv::to(y+2,x)+bottom+Theme::c("main_fg");
}

struct ChipGrid {
	bool rich;
	int id_width,per_row,rows,height;
};
ChipGrid chip_grid(const Host& host,const Rect& rect) {
	const int height=rect.height, inner=rect.width-4;
	const auto& devices=metric_items(metric_data(host,"cards"),"devices");
	int id_width=2;
	for (const auto& device:devices) id_width=std::max(id_width,std::min(8,int(device.at("id").get_ref<const std::string&>().size())));
	const bool rich=height>=20 and inner>=60;
	const int tile_width=id_width+(rich ? 6 : 3), tile_height=rich ? 3 : 1;
	int per_row=std::max(1,(inner+1)/(tile_width+1));
	if (per_row>=8) per_row=per_row/4*4;
	const int needed=std::max(1,(int(devices.size())+per_row-1)/per_row);
	const int rows=std::min(needed,std::max(1,(height-13)/tile_height));
	const int grid_height=devices.empty() ? 1 : rows*tile_height;
	return {rich,id_width,per_row,rows,grid_height};
}

std::string draw_card(const Host& host, const Rect& rect, bool selected, const Draw::Style& style,int reserved_grid_height) {
	const auto& [x,y,width,height]=rect;
	const int left=x+2, inner=width-4;
	auto out=Draw::createBox(x,y,width,height,Theme::c(selected ? "hi_fg" : "cpu_box"),true,
		safe_text(host.name,width-8),host.simulated ? "SIMULATED" : host.status,0,style);
	auto write=[&](int row,const std::string& value,const std::string& color="main_fg") {
		if (row>y and row<y+height-1) out+=Mv::to(row,left)+Theme::c("main_bg")+Theme::c(color)+safe_text(value,inner);
	};
	const auto state=resource_status(host,"cards");
	const bool fresh=state=="ok" or state=="partial";
	const auto& data=metric_data(host,"cards"); const auto& devices=metric_items(data,"devices");
	const auto [rich,id_width,per_row,rows,natural_height]=chip_grid(host,rect);
	const int tile_width=id_width+(rich ? 6 : 3), tile_height=rich ? 3 : 1;
	const int grid_height=std::max(natural_height,reserved_grid_height);
	const int graph_height=std::clamp(height-12-grid_height,1,6);
	write(y+1,host.cpu_valid ? fmt::format("CPU {:5.1f}%  / {} cores",host.cpu,host.cores) : "CPU -- / awaiting sample");
	const auto load=fmt::format("load/core {:.2f}",host.cores ? host.load[0]/host.cores : 0);
	out+=Mv::to(y+1,left+inner-int(load.size()))+Theme::c("graph_text")+load;
	Draw::Graph graph(inner,graph_height,"cpu",host.history,style.graph_symbol,false,false,0,0,style);
	out+=Mv::to(y+2,left)+graph();
	write(y+2+graph_height,host.memory_valid ? fmt::format("MEM {:.0f}/{:.0f} GiB",host.memory_used/gib,host.memory_total/gib) : "MEM --");
	const auto available=host.memory_valid ? fmt::format("{:.0f} GiB available",(host.memory_total-host.memory_used)/gib) : "-- available";
	out+=Mv::to(y+2+graph_height,left+inner-int(available.size()))+Theme::c("cached_mid")+available;
	Draw::Meter meter(inner,"used");
	out+=Mv::to(y+3+graph_height,left)+meter(static_cast<int>(host.memory_total ? 100.0*host.memory_used/host.memory_total : 0));
	const int heading=y+5+graph_height, stats_y=y+height-5;
	std::string capacity="NPU  "+state, owner="";
	if (fresh) {
		capacity="NPU  "+amount(data.at("summary"),"free")+" / "+amount(data.at("summary"),"total")+" FREE"+(state=="partial" ? " [partial]" : "");
		owner="OWNERS "+amount(data.at("summary"),"occupying_users");
	}
	const int visible=std::min(int(devices.size()),rows*per_row);
	if (visible<int(devices.size())) owner=fmt::format("+{} in details",devices.size()-visible);
	write(heading,capacity,"title");
	if (int(capacity.size()+owner.size())+2<inner) out+=Mv::to(heading,left+inner-int(owner.size()))+Theme::c("graph_text")+owner;
	if (devices.empty()) write(heading+1,"No card inventory available","graph_text");
	for (int i=0;i<visible;++i) out+=card_chip(devices[i],left+(i%per_row)*(tile_width+1),heading+1+(i/per_row)*tile_height,id_width,rich,fresh,style);
	const auto users_state=resource_status(host,"sessions"); const auto& users=metric_data(host,"sessions");
	const bool users_fresh=users_state=="ok" or users_state=="partial";
	const auto user_count=users_fresh ? (users_state=="partial" ? "~" : "")+amount(users,"users") : "--";
	const auto session_count=users_fresh ? (users_state=="partial" ? "~" : "")+amount(users,"sessions") : "--";
	const auto run=fresh ? amount(data.at("scheduler"),"running_tasks") : "--";
	const auto limit=fresh ? amount(data.at("scheduler"),"max_concurrent") : "--";
	const auto wait=fresh ? amount(data.at("scheduler"),"pending_tasks") : "--";
	const std::array<std::string,4> labels={"USERS","SESSIONS","RUNNING","WAITING"}, values={user_count,session_count,run,wait}, colors={"cached_mid","download_end","cpu_start","cpu_mid"};
	const int stat_width=(inner-3)/4;
	for (int i=0;i<4;++i) {
		const int sx=left+i*(stat_width+1), sw=i==3 ? left+inner-sx : stat_width;
		out+=Draw::createBox(sx,stats_y,sw,3,Theme::c("div_line"),true,labels[i],"",0,style);
		out+=Mv::to(stats_y+1,sx+2)+Theme::c(values[i]=="--" ? "graph_text" : colors[i])+safe_text(values[i],sw-4);
		if (i==2) {
			const auto caption="LIMIT "+limit;
			out+=Mv::to(stats_y+2,sx+2)+Theme::c("main_bg")+Theme::c("graph_text")+(int(caption.size())<=sw-4 ? caption : "LIMIT...");
		}
	}
	const std::array<std::string,4> legend={"F Free","H Held","R Reserved","X Off"}, keys={"graph_text","cpu_end","cpu_mid","graph_text"};
	int cursor=left;
	for (int i=0;i<4;++i) {
		out+=Mv::to(y+height-2,cursor)+Theme::c(keys[i])+legend[i]; cursor+=legend[i].size()+3;
	}
	return out+Theme::c("main_bg")+Theme::c("main_fg");
}

std::string draw_cluster_summary(const std::vector<Host>& hosts,const Rect& rect,const Draw::Style& style) {
	const auto& [x,y,width,height]=rect;
	const int left=x+2, inner=width-4, count=hosts.size();
	const auto totals=overview_totals(hosts);
	auto out=Draw::createBox(x,y,width,height,Theme::c("mem_box"),true,"cluster | overview","1-3 Host details",0,style);
	auto write=[&](int row,const std::string& value,const std::string& color="main_fg") {
		out+=Mv::to(y+row,left)+Theme::c("main_bg")+Theme::c(color)+safe_text(value,inner);
	};
	auto suffix=[&](int available,bool partial=false){return available<count or partial ? "*" : "";};
	const auto cards=totals.card_hosts ? fmt::format("{}/{}{}",totals.free_cards,totals.total_cards,suffix(totals.card_hosts,totals.cards_partial)) : "--";
	const auto waiting=totals.queue_hosts ? std::to_string(totals.waiting)+suffix(totals.queue_hosts,totals.queue_partial) : "--";
	const std::array<std::string,4> labels={"ONLINE","CORES","NPU FREE","WAITING"};
	const std::array<std::string,4> values={fmt::format("{}/{}",totals.online,count),std::to_string(totals.cores),cards,waiting};
	const std::array<std::string,4> colors={"cpu_start","download_end","cached_mid","cpu_mid"};
	const int tile_width=(inner-3)/4;
	for (int i=0;i<4;++i) {
		const int tx=left+i*(tile_width+1), tw=i==3 ? left+inner-tx : tile_width;
		out+=Draw::createBox(tx,y+1,tw,3,Theme::c("div_line"),true,labels[i],"",0,style);
		out+=Mv::to(y+2,tx+2)+Theme::c(values[i]=="--" ? "graph_text" : colors[i])+safe_text(values[i],tw-4);
	}
	write(4,totals.cpu_hosts ? fmt::format("CPU {:5.1f}%{}  | weighted by core count",totals.cpu,suffix(totals.cpu_hosts)) : "CPU -- | awaiting fresh samples");
	if (totals.cpu_hosts) { Draw::Meter meter(inner,"cpu"); out+=Mv::to(y+5,left)+meter(std::lround(totals.cpu)); }
	write(6,totals.memory_hosts ? fmt::format("MEM {:.0f} / {:.0f} GiB available{}",totals.memory_available/gib,totals.memory_total/gib,suffix(totals.memory_hosts)) : "MEM -- | awaiting fresh samples");
	if (totals.memory_hosts) {
		Draw::Meter meter(inner,"available");
		out+=Mv::to(y+7,left)+meter(std::lround(100.0*totals.memory_available/totals.memory_total));
	}
	const int start=9, stride=std::max(1,(height-3-start)/count);
	write(8,stride==1 ? "HOST          CPU%   LOAD/CORE  NPU FREE" : "HOST COMPARISON  |  CPU trends / free cards","title");
	for (int i=0;i<count;++i) {
		const auto& host=hosts[i]; const int row=start+i*stride;
		const bool online=host.status=="ONLINE", cpu=online and host.cpu_valid and host.cores>0;
		const auto usage=cpu ? fmt::format("{:.1f}",host.cpu) : "--";
		const auto load=cpu ? fmt::format("{:.2f}",host.load[0]/host.cores) : "--";
		const auto free=card_summary(host,true);
		if (stride==1) {
			write(row,fmt::format("{} {:9} {:>6} {:>11} {:>9}",i+1,safe_text(host.name,9),usage,load,free),online ? "main_fg" : "graph_text");
			continue;
		}
		write(row,fmt::format("{} {}  |  CPU {}%  load/core {}",i+1,safe_text(host.name,16),usage,load),online ? "title" : "graph_text");
		const int graph_width=std::max(10,inner/3), info_x=left+graph_width+2, info_width=inner-graph_width-2;
		if (cpu) {
			Draw::Graph graph(graph_width,stride-1,"cpu",host.history,style.graph_symbol,false,false,0,0,style);
			out+=Mv::to(y+row+1,left)+graph();
		}
		auto info=[&](int offset,const std::string& value,const std::string& color="main_fg") {
			out+=Mv::to(y+row+offset,info_x)+Theme::c(color)+safe_text(value,info_width);
		};
		const auto state=resource_status(host,"cards");
		const auto& data=metric_data(host,"cards");
		const auto wait=state=="ok" or state=="partial" ? amount(data.at("scheduler"),"pending_tasks") : "--";
		info(1,"NPU "+free+" free  | wait "+wait,"cached_mid");
		if (stride>=3) info(2,online and host.memory_valid ? fmt::format("MEM {:.0f} GiB available",(host.memory_total-host.memory_used)/gib) : "MEM -- available");
		if (stride>=4) info(3,online ? "CARDS "+state : host.status,"graph_text");
	}
	write(height-3,fmt::format("Samples: CPU {}/{}  MEM {}/{}  CARDS {}/{}",totals.cpu_hosts,count,totals.memory_hosts,count,totals.card_hosts,count),"graph_text");
	write(height-2,"Online data only  |  * partial  -- unavailable","graph_text");
	return out+Theme::c("main_bg")+Theme::c("main_fg");
}

}

Geometry layout(int width, int height, int count) {
	if (count < 1 or count > 4) throw std::invalid_argument("Expected 1 to 4 hosts");
	if (width < 80 or height < 24) return {Layout::too_small, {}};
	if (width < 120 or height < 36) return {Layout::compact, {}};
	Geometry result{Layout::cards, {}};
	const int columns = count == 1 ? 1 : 2;
	const int rows = count <= 2 ? 1 : 2;
	const int card_width = (width - 3) / columns;
	const int card_height = (height - 4) / rows;
	for (int i = 0; i < count; ++i) {
		result.cards.push_back({2 + (i % columns) * card_width,
			3 + (i / columns) * card_height, card_width - 1, card_height - 1});
	}
	return result;
}

std::vector<Host> demo_hosts(int interval_ms) {
	std::vector<Host> hosts;
	for (unsigned i = 0; i < 4; ++i) {
		Host host;
		host.name = fmt::format("dev-{:02}", i + 1);
		host.cores = 16 << i;
		host.memory_total = static_cast<std::uint64_t>(gib) * (32 << i);
		hosts.push_back(host);
	}
	for (unsigned tick = 0; tick < 300; ++tick) advance_demo(hosts, tick, interval_ms);
	return hosts;
}

void advance_demo(std::vector<Host>& hosts, unsigned tick, int interval_ms) {
	for (std::size_t i = 0; auto& host : hosts) {
		host.cpu = std::clamp(18.0 + 17.0 * i + 14 * std::sin(tick * 0.18 + i * 1.4), 0.0, 100.0);
		host.memory_used = static_cast<std::uint64_t>(host.memory_total * (0.22 + 0.14 * i));
		host.load = {host.cpu / 100 * host.cores, host.cpu / 115 * host.cores, host.cpu / 130 * host.cores};
		using Protocol::Json;
		const auto stamp = Protocol::now_ms();
		Json cores=Json::array(), processes=Json::array();
		for (int core=0; core<host.cores; ++core)
			cores.push_back({{"id",core},{"usage_pct",std::clamp(host.cpu+12*std::sin(core+tick*0.1),0.0,100.0)},{"temperature_c",48+core%9}});
		for (int pid=0; pid<40; ++pid)
			processes.push_back({{"pid",1000+pid},{"start_time_ticks",10000+pid},{"ppid",pid ? 1000+(pid-1)/4 : 1},{"uid",1000},{"user","developer"},{"name",pid%2 ? "python" : "compiler"},{"threads",pid%8+1},{"command",pid%2 ? "python train.py" : "clang++ -O2 worker.cpp"},{"state","R"},{"nice",0},{"elapsed_seconds",3000+pid},{"read_bytes_total",12345678},{"write_bytes_total",2345678},
				{"cpu_pct",host.cpu*(40-pid)/10},{"rss_bytes",std::uint64_t(40-pid)*1024*1024}});
		Json groups{
			{"cpu",Protocol::group({{"usage_pct",host.cpu},{"iowait_pct",2.0},{"online_cores",host.cores},
				{"load_avg",host.load},{"cores",cores},{"frequency_mhz",3200},{"temperature_c",52},{"model","AMD EPYC / Demo"},{"uptime_seconds",1234567},{"user_pct",host.cpu*0.7},{"system_pct",host.cpu*0.25}},stamp,interval_ms)},
			{"memory",Protocol::group({{"total_bytes",host.memory_total},{"used_bytes",host.memory_used},
				{"available_bytes",host.memory_total-host.memory_used},{"free_bytes",host.memory_total/4},
				{"cached_bytes",host.memory_total/8},{"buffers_bytes",134217728},{"swap_total_bytes",0},{"swap_used_bytes",0}},stamp,interval_ms)},
			{"filesystems",Protocol::group({{"items",Json::array({{{"mount","/"},{"device","259:0"},
				{"total_bytes",107374182400ULL},{"used_bytes",64424509440ULL},{"available_bytes",42949672960ULL}}})}},stamp,interval_ms)},
			{"disks",Protocol::group({{"items",Json::array({{{"name","nvme0n1"},{"id","259:0"},
				{"read_bytes_per_sec",2097152*(1+std::sin(tick*0.17))},{"write_bytes_per_sec",1048576*(1+std::cos(tick*0.13))},{"activity_pct",25+20*std::sin(tick*0.17)}}})}},stamp,interval_ms)},
			{"network",Protocol::group({{"items",Json::array({{{"name","eth0"},{"ifindex",2},{"address","100.90.0."+std::to_string(i+1)},
				{"rx_bytes_total",1073741824ULL},{"tx_bytes_total",536870912ULL},
				{"rx_bytes_per_sec",std::uint64_t(host.cpu*10000)},{"tx_bytes_per_sec",std::uint64_t(host.cpu*4000)}}})}},stamp,interval_ms)},
			{"processes",Protocol::group({{"items",processes},{"total_visible",40},{"matched",40},{"returned",40},{"truncated",false}},stamp,interval_ms)}
		};
		Json devices=Json::array();
		const int device_count=(i==1 or i==2) ? 16 : 8;
		Json counts{{"total",device_count},{"free",0},{"allocated",0},{"reserved",0},{"disabled",0},{"external",0},{"error",0},{"unknown",0},{"occupying_users",1}};
		for (int id=0; id<device_count; ++id) {
			const std::string state=i==1 ? (id<device_count-2 ? "free" : "allocated") : i==2 ? "allocated" :
				(id==2 or id==3 or id==5) ? "allocated" : id==6 ? "reserved" : id==7 ? "disabled" : "free";
			counts[state]=counts[state].get<int>()+1;
			Json allocation=nullptr, reservation=nullptr;
			if (state=="allocated") allocation={{"task_id","task-demo-running"},{"user","developer"},{"uid",1000},{"started_at","2026-01-01T10:00:00Z"},{"elapsed_seconds",760}};
			if (state=="reserved" or (i!=1 and i!=2 and id==5)) reservation={{"task_id","task-demo-waiting"},{"user","builder"},{"uid",1001},{"requested_devices",2}};
			devices.push_back({{"id",std::to_string(id)},{"group_id","g"+std::to_string(id/4)},{"pool_eligible",state!="disabled"},
				{"state",state},{"reason_codes",state=="free" ? Json::array() : Json::array({state})},{"allocation",allocation},{"reservation",reservation},{"health","unknown"}});
		}
		groups["cards"]=Protocol::group({{"schema_version",1},{"producer",{{"name","pto-task"},{"version","demo"}}},{"boot_id","demo-boot"},
			{"snapshot_seq",tick},{"observed_at","2026-01-01T10:12:40Z"},{"age_ms",0},{"status","ok"},{"reason",nullptr},{"inventory_complete",true},
			{"external_detection","unsupported"},{"scheduler",{{"state","running"},{"mode","pool_aware_reservation"},{"accepting_submissions",true},
			{"dispatching",true},{"running_tasks",1},{"pending_tasks",i==1 ? 0 : 2},{"max_concurrent",8},{"reason",nullptr}}},{"devices",devices},{"summary",counts}},stamp,5000);
		const std::array<std::string,7> names={"developer","builder","reviewer","runner","alice","bob","charlie"};
		const int user_count=std::array{2,4,7,3}[i]; Json logins=Json::array();
		for (int user=0;user<user_count;++user) logins.push_back({{"user",names[user]},{"sessions",user==0 ? 2 : 1},{"first_login_at",1760000000+user*500}});
		groups["sessions"]=Protocol::group({{"source","utmpx"},{"users",user_count},{"sessions",user_count+1},{"age_ms",0},{"items",logins}},stamp,5000);
		groups["filesystems"]["data"]["items"].push_back({{"mount","/data"},{"device","259:1"},{"total_bytes",2199023255552ULL},{"used_bytes",1209462790553ULL},{"available_bytes",989560464999ULL}});
		groups["disks"]["data"]["items"].push_back({{"name","nvme1n1"},{"id","259:1"},{"read_bytes_per_sec",16777216*(1+std::sin(tick*0.09))},{"write_bytes_per_sec",4194304},{"activity_pct",65}});
		const Protocol::ProcessFilter filter(host.filter);
		std::erase_if(processes.get_ref<Json::array_t&>(),[&](const auto& item){return not filter.matches(item);});
		groups["processes"]["data"]["items"]=processes;
		groups["processes"]["data"]["matched"]=groups["processes"]["data"]["returned"]=processes.size();
		for (const auto& item : processes) if (item["pid"]==host.selected_pid and item["start_time_ticks"]==host.selected_start) {
			groups["processes"]["data"]["selected"]=item;
			groups["processes"]["data"]["selected"]["detail_status"]="ok";
		}
		ingest(host,{{"groups",groups},{"wall_time",Protocol::utc_now()}});
		++i;
	}
}

std::string render(const std::vector<Host>& hosts, int width, int height,
	int selected, bool detail, const Draw::Style& style) {
	const auto geometry = layout(width, height, static_cast<int>(hosts.size()));
	if (selected < 0 or selected >= static_cast<int>(hosts.size())) throw std::invalid_argument("Invalid host selection");
	std::string out = Term::clear + Fx::reset;
	if (geometry.layout == Layout::too_small) {
		// Stay inside even a one-column terminal, without writing its last cell.
		const std::string message = "Resize terminal to at least 80x24";
		if (width > 1 and height > 0) out += at(1, 1, message.substr(0, width - 1));
		return out;
	}
	out += at(1, 2, hosts[0].simulated ? "btop-cluster | DEMO - simulated data" : "btop-cluster | LIVE");
	const auto host_keys=hosts.size()==1 ? std::string{"1"} : "1-"+std::to_string(hosts.size());
	const auto overview_keys="Tab Select   Enter Details   "+host_keys+" Host   u Users   t Theme   q Quit";
	if (detail) {
		out += detail_view(hosts.at(selected), width, height, style);
		const std::string controls=host_keys+" Host  Esc Overview  ? Help  q Quit";
		out += at(1,width-1-static_cast<int>(controls.size()),controls);
	} else if (geometry.layout == Layout::cards) {
		int grid_height=0;
		for (std::size_t i=0;i<hosts.size();++i) grid_height=std::max(grid_height,chip_grid(hosts[i],geometry.cards[i]).height);
		for (std::size_t i=0;i<hosts.size();++i)
			out+=draw_card(hosts[i],geometry.cards[i],i==static_cast<std::size_t>(selected),style,grid_height);
		if (hosts.size()==3) {
			const auto& below=geometry.cards[2];
			out+=draw_cluster_summary(hosts,{geometry.cards[1].x,below.y,below.width,below.height},style);
		}
		out += at(height - 1, 2, overview_keys);
	} else {
		out += at(3,2,"HOST       CPU    MEM used/total GiB    NPU free  WAIT   USERS/SESS");
		for (std::size_t i=0; i<hosts.size(); ++i) {
			const auto& host=hosts[i];
			const auto& cards=metric_data(host,"cards");
			const auto card_state=resource_status(host,"cards");
			std::string waiting="--";
			if ((card_state=="ok" or card_state=="partial") and cards.at("scheduler").at("pending_tasks").is_number_integer()) waiting=std::to_string(cards.at("scheduler").at("pending_tasks").get<std::uint64_t>());
			out+=Mv::to(5+i*3,2)+Theme::c(i==static_cast<std::size_t>(selected) ? "hi_fg" : "main_fg");
			out+=safe_text(fmt::format("{} {:7} {:5.1f}% {:6.0f}/{:<6.0f}        {:8}  {:5}  {}",i==static_cast<std::size_t>(selected) ? ">" : " ",safe_text(host.name,7),host.cpu,host.memory_used/gib,host.memory_total/gib,card_summary(host,true),waiting,session_summary(host,true)),width-3);
			if (not host.simulated) out+=at(6+i*3,4,safe_text(host.status+" "+host.error,width-6));
		}

		out += at(19, 2, "Compact overview | expand to 120x36 for graphs");
		out += at(height - 1, 2, overview_keys);
	}
	if (hosts[selected].users_open) out+=users_overlay(hosts[selected],width,height,style);
	return out + Fx::reset;
}

}
