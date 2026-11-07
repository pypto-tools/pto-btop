// SPDX-License-Identifier: Apache-2.0
#include "view.hpp"
#include "btop_theme.hpp"
#include "btop_tools.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fmt/format.h>

namespace Cluster {
using Protocol::Json;
namespace {
const Json missing;
const std::array<std::string,3> periods={"today","7d","30d"};
const Json& window(const Host& host,int period) {
	const auto& windows=metric_items(metric_data(host,"card_usage"),"windows");
	for (const auto& w : windows) if (w.at("id")==periods[period%3]) return w;
	return missing;
}
bool fresh(const Host& host) { const auto s=resource_status(host,"card_usage"); return s=="ok" or s=="partial"; }
std::string lower(std::string text) { for (auto& c : text) if (static_cast<unsigned char>(c)<128) c=std::tolower(static_cast<unsigned char>(c)); return text; }
struct Row { std::string key,host,user,state; std::uint64_t seconds,tasks; int cards; double share; };
struct Summary {
	std::vector<Row> rows; std::deque<long long> daily;
	std::uint64_t seconds=0,covered=0; int included=0,total=0; bool partial=false,mismatch=false;
	std::string zone,observed;
};
Summary summarize(const std::vector<const Host*>& hosts,const Host& selection) {
	Summary result; result.total=hosts.size(); std::string start;
	for (const auto* ptr : hosts) {
		const auto& host=*ptr; const auto& w=window(host,selection.usage_period);
		if (w.is_null()) continue;
		const auto& data=metric_data(host,"card_usage");
		const auto zone=data.at("timezone").get<std::string>();
		bool include=fresh(host);
		if (include and result.zone.empty()) { result.zone=zone; start=w.at("start").dump(); }
		if (include and (zone!=result.zone or w.at("start").dump()!=start)) { include=false; result.mismatch=true; }
		if (include) {
			if (data.at("covered_from").is_number_integer()) result.covered=std::max(result.covered,data.at("covered_from").get<std::uint64_t>());
			++result.included; result.seconds+=w.at("card_seconds").get<std::uint64_t>();
			result.partial=result.partial or not w.at("complete").get<bool>();
			const auto stamp=data.at("observed_at").get<std::string>();
			if (result.observed.empty() or stamp<result.observed) result.observed=stamp;
			const auto& days=w.at("daily"); if (result.daily.empty()) result.daily.resize(days.size());
			for (std::size_t i=0;i<days.size();++i) result.daily[i]+=days[i].at("card_seconds").get<long long>();
		}
		for (const auto& user : w.at("users")) {
			const auto name=user.at("user").get<std::string>();
			if (lower(name).find(lower(selection.usage_filter))==std::string::npos) continue;
			const auto seconds=user.at("card_seconds").get<std::uint64_t>();
			const auto total=w.at("card_seconds").get<std::uint64_t>();
			result.rows.push_back({Json::array({host.name,name,user.at("uid")}).dump(),host.name,name,
				include ? (w.at("complete")==true ? "" : "*") : "old",seconds,user.at("task_count").get<std::uint64_t>(),
				include and user.at("active_cards").is_number_integer() ? user.at("active_cards").get<int>() : -1,
				total ? 100.0*seconds/total : 0});
		}
	}
	std::sort(result.rows.begin(),result.rows.end(),[](const auto& a,const auto& b){ return a.seconds!=b.seconds ? a.seconds>b.seconds : a.key<b.key; });
	result.partial=result.partial or result.included<result.total;
	return result;
}
std::vector<const Host*> sources(const std::vector<Host>& hosts,int selected,bool cluster) {
	std::vector<const Host*> result;
	for (std::size_t i=0;i<hosts.size();++i) if (cluster or i==std::size_t(selected)) result.push_back(&hosts[i]);
	return result;
}
int position(const Summary& summary,const Host& selected) {
	for (std::size_t i=0;i<summary.rows.size();++i) if (summary.rows[i].key==selected.usage_selected) return i;
	return 0;
}
std::string panel(const std::vector<const Host*>& hosts,const Host& selected,const Rect& r,const Draw::Style& style,bool full,bool folded) {
	const bool cluster=hosts.size()>1; const auto sum=summarize(hosts,selected);
	const int left=r.x+2,inner=r.width-4;
	auto out=Draw::createBox(r.x,r.y,r.width,r.height,Theme::c("mem_box"),true,
		full ? "card hours | "+(cluster ? std::string("all servers") : safe_text(hosts.front()->name,24)) : "cards | devices / USAGE",
		"",0,style);
	auto write=[&](int row,const std::string& value,const std::string& color="main_fg") {
		if (row>0 and row<r.height-1) out+=Mv::to(r.y+row,left)+Theme::c("main_bg")+Theme::c(color)+safe_text(value,inner);
	};
	const auto total=sum.included ? fmt::format("{:.1f}{} card.h",sum.seconds/3600.0,sum.partial ? "*" : "") : "-- card.h";
	write(1,fmt::format("{}  {}  DATA {}/{}",periods[selected.usage_period%3],total,sum.included,sum.total),"title");
	if (folded or r.height<6) return out;
	const bool graph=full and r.height>=20;
	write(2,graph ? "Timezone "+sum.zone+" | oldest sample "+(sum.observed.empty() ? "--" : sum.observed) : "p Period  h Expand  H Devices","graph_text");
	const int first=graph ? 9 : 4, count=usage_visible_rows(r.height,full), chosen=position(sum,selected), start=(chosen/count)*count;
	if (graph) {
		if (selected.usage_period==0) {
			const int tile=inner/int(hosts.size());
			for (std::size_t i=0;i<hosts.size();++i) {
				const int tx=left+int(i)*tile, tw=i+1==hosts.size() ? left+inner-tx : tile-1;
				out+=Draw::createBox(tx,r.y+4,tw,3,Theme::c("div_line"),true,safe_text(hosts[i]->name,tw-4),"",0,style);
				const auto& w=window(*hosts[i],0);
				const auto value=w.is_null() ? std::string("--") : fmt::format("{:.1f}{}",w.at("card_seconds").get<double>()/3600,w.at("complete")==true ? "" : "*");
				out+=Mv::to(r.y+5,tx+2)+Theme::c(fresh(*hosts[i]) ? "cached_mid" : "graph_text")+safe_text(value+" card.h",tw-4);
			}
		} else if (not sum.daily.empty()) {
			const auto max=std::max(1LL,*std::max_element(sum.daily.begin(),sum.daily.end()));
			std::deque<long long> bars;
			for (int x=0;x<2*inner;++x) bars.push_back(sum.daily[std::min(sum.daily.size()-1,std::size_t(x)*sum.daily.size()/(2*inner))]);
			Draw::Graph chart(inner,3,"cached",bars,style.graph_symbol,false,false,max,0,style);
			out+=Mv::to(r.y+4,left)+chart();
		}
		write(3,selected.usage_period==0 ? "Today by server | card hours" : "Daily card hours | oldest to newest","graph_text");
		write(7,"/ Filter: "+selected.usage_filter+(selected.usage_editing ? "_" : ""),"title");
	}
	const int host_width=cluster ? 12 : 0;
	const int user_width=std::max(8,inner-host_width-38);
	const int hours_x=left+host_width+user_width, tasks_x=hours_x+11, cards_x=tasks_x+7, share_x=cards_x+6;
	auto field=[&](int row,int x,const std::string& text,int width,const std::string& color="main_fg") {
		out+=Mv::to(r.y+row,x)+Theme::c(color)+safe_text(text,std::max(0,std::min(width,left+inner-x)));
	};
	if (cluster) field(first-1,left,"HOST",host_width,"title");
	field(first-1,left+host_width,"USER",user_width,"title"); field(first-1,hours_x,"CARD.H",10,"title");
	field(first-1,tasks_x,"TASKS",6,"title"); field(first-1,cards_x,"NOW",5,"title"); field(first-1,share_x,cluster ? "HOST %" : "SHARE",9,"title");
	for (int i=0;i<count and start+i<int(sum.rows.size());++i) {
		const auto& row=sum.rows[start+i]; const int y=first+i;
		const auto color=row.state=="old" ? "graph_text" : start+i==chosen ? "hi_fg" : "main_fg";
		if (cluster) field(y,left,row.host,host_width-1,color);
		field(y,left+host_width,(start+i==chosen ? ">" : " ")+row.user,user_width-1,color);
		field(y,hours_x,fmt::format("{:.1f}{}",row.seconds/3600.0,row.state),10,color);
		field(y,tasks_x,std::to_string(row.tasks),6,color);
		field(y,cards_x,row.cards<0 ? "--" : std::to_string(row.cards),5,color);
		const int bar_width=std::max(0,left+inner-share_x-5);
		if (bar_width>=3) { Draw::Meter meter(bar_width,"cached"); out+=Mv::to(r.y+y,share_x)+meter(std::lround(row.share)); }
		field(y,left+inner-4,fmt::format("{:3.0f}%",row.share),4,color);
	}
	if (sum.rows.empty()) write(first,sum.included ? (selected.usage_filter.empty() ? "No recorded card usage" : "No matching users") : "Card usage unavailable / awaiting sample","graph_text");
	std::string warning="Share per host | old: frozen sample | NOW: held cards";
	if (sum.mismatch) warning="Range/timezone mismatch: totals exclude other ranges";
	else if (sum.partial) {
		warning="* partial history";
		if (sum.covered) {
			const std::time_t coverage=sum.covered; std::tm coverage_tm{}; gmtime_r(&coverage,&coverage_tm);
			char coverage_date[32]; std::strftime(coverage_date,sizeof(coverage_date),"%Y-%m-%d %H:%M UTC",&coverage_tm);
			warning+=std::string(" | known since ")+coverage_date;
		} else warning+=" | old: frozen sample | NOW: held cards";
	}
	write(r.height-3,warning,"graph_text");
	write(r.height-2,fmt::format("PgUp/PgDn  {}/{}  p Period{}",sum.rows.empty() ? 0 : chosen+1,sum.rows.size(),full ? "  / Filter  Tab Scope  Esc Close" : "  h Expand"),"graph_text");
	return out+Theme::c("main_bg")+Theme::c("main_fg");
}
}

int usage_visible_rows(int height,bool full) { return std::max(1,height-(full and height>=20 ? 12 : 7)); }
std::string usage_summary(const Host& host) {
	const auto& w=window(host,host.usage_period);
	if (w.is_null()) return "CARD-H --  h Usage";
	return fmt::format("CARD-H {:.1f}{} {}{}",w.at("card_seconds").get<double>()/3600,
		w.at("complete")==true ? "" : "*",periods[host.usage_period%3],fresh(host) ? "" : " [old]");
}
std::string usage_cluster_summary(const std::vector<Host>& hosts) {
	const auto sum=summarize(sources(hosts,0,true),hosts.front());
	return sum.included ? fmt::format("CARD-H {:.1f}{} {} | DATA {}/{}",sum.seconds/3600.0,sum.partial ? "*" : "",periods[hosts.front().usage_period%3],sum.included,sum.total) : "CARD-H -- | h Usage";
}
void move_usage(std::vector<Host>& hosts,int selected,int delta,bool cluster) {
	auto& host=hosts[selected]; const auto sum=summarize(sources(hosts,selected,cluster),host);
	if (sum.rows.empty()) { host.usage_selected.clear(); return; }
	const int index=std::clamp(position(sum,host)+delta,0,int(sum.rows.size())-1); host.usage_selected=sum.rows[index].key;
}
std::string usage_panel(const Host& host,const Rect& rect,const Draw::Style& style,bool folded) {
	return panel({&host},host,rect,style,false,folded);
}
int usage_overlay_rows(const std::vector<Host>& hosts,int selected,int height) {
	const auto sum=summarize(sources(hosts,selected,hosts[selected].usage_cluster),hosts[selected]);
	return usage_visible_rows(std::min(height-3,std::max(20,int(sum.rows.size())+12)),true);
}
std::string usage_overlay(const std::vector<Host>& hosts,int selected,int width,int height,const Draw::Style& style) {
	if (width<80 or height<24) return "";
	const int h=usage_overlay_rows(hosts,selected,height)+12, w=std::min(120,width-3);
	return panel(sources(hosts,selected,hosts[selected].usage_cluster),hosts[selected],{(width-w)/2+1,(height-h)/2+1,w,h},style,true,false);
}
Json demo_usage(int index) {
	const std::int64_t now=1793966400; // 2026-11-06 12:00 UTC, simulated data only.
	const auto today=now/86400*86400; Json windows=Json::array();
	const std::array<int,3> lengths={1,7,30};
	for (int k=0;k<3;++k) {
		const int days=lengths[k]; const auto start=today-(days-1)*86400;
		Json daily=Json::array(),users=Json::array(); int factor=0;
		for (int d=0;d<days;++d) {
			const int f=d==days-1 ? 1 : (d+index)%3+1; factor+=f;
			const std::time_t stamp=start+d*86400; std::tm tm{}; gmtime_r(&stamp,&tm); char date[16]; std::strftime(date,sizeof(date),"%Y-%m-%d",&tm);
			daily.push_back({{"date",date},{"card_seconds",462240LL*f*(index+1)},{"complete",index!=2 or k==0}});
		}
		const std::array<std::string,3> names={"alice","bob","ci-runner"}; const std::array<int,3> seconds={259200,144000,59040};
		for (int u=0;u<3;++u) users.push_back({{"user",names[u]},{"uid",1000+u},{"card_seconds",1LL*seconds[u]*factor*(index+1)},{"task_count",days},{"active_cards",u==0 ? 4 : 0}});
		windows.push_back({{"id",periods[k]},{"start",start},{"end",now},{"complete",index!=2 or k==0},
			{"card_seconds",462240LL*factor*(index+1)},{"task_count",3*days},{"users",users},{"daily",daily}});
	}
	return {{"schema_version",1},{"status",index==2 ? "partial" : "ok"},{"reason",index==2 ? Json("history_partial") : Json(nullptr)},
		{"observed_at","2026-11-06T12:00:00Z"},{"age_ms",0},{"cached",false},{"timezone","UTC"},{"covered_from",index==2 ? today : today-35*86400},{"windows",windows}};
}
}
