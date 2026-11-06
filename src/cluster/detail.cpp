// SPDX-License-Identifier: Apache-2.0
// Remote snapshot panels using btop's original Graph, Meter, boxes and themes.
#include "view.hpp"
#include "btop_theme.hpp"
#include "btop_tools.hpp"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <ctime>
#include <fmt/format.h>

namespace Cluster {
using Protocol::Json;
namespace {
const Json empty = Json::object();
const std::deque<long long> no_history;
double number(const Json& value, const char* key, double fallback = -1) {
	const auto found=value.find(key);
	return found!=value.end() and found->is_number() ? found->get<double>() : fallback;
}
std::string text(const Json& value,const char* key,std::string fallback="") {
	const auto found=value.find(key);
	return found!=value.end() and found->is_string() ? found->get<std::string>() : fallback;
}
std::string percent(double value) { return value<0 ? "N/A" : fmt::format("{:.0f}%",value); }
std::string human(double value) {
	if (value<0) return "N/A";
	const char* units[]={"B","KiB","MiB","GiB","TiB","PiB"}; int unit=0;
	while (value>=1024 and unit<5) { value/=1024; ++unit; }
	return fmt::format("{:.{}f}{}",value,value<10 and unit>0 ? 1 : 0,units[unit]);
}
std::string duration(double seconds) {
	if (not std::isfinite(seconds) or seconds<0 or seconds>1e15) return "N/A";
	const auto value=static_cast<long long>(seconds);
	return fmt::format("{}d {:02}:{:02}",value/86400,(value/3600)%24,(value/60)%60);
}
int columns(std::string_view value) {
	std::mbstate_t state{}; int result=0;
	for (std::size_t i=0; i<value.size();) {
		wchar_t wc; auto n=std::mbrtowc(&wc,value.data()+i,value.size()-i,&state);
		if (n==std::size_t(-1) or n==std::size_t(-2) or n==0) { n=1; state={}; ++result; }
		else result+=std::max(0,wcwidth(wc));
		i+=n;
	}
	return result;
}
std::string cell(const std::string& value,int width,bool right=false) {
	const auto clipped=safe_text(value,std::max(0,width));
	const auto padding=std::string(std::max(0,width-columns(clipped)),' ');
	return right ? padding+clipped : clipped+padding;
}
std::string at(int y,int x,const std::string& value,int width,const std::string& color="main_fg") {
	return Mv::to(y,x)+(Theme::colors.contains(color) ? Theme::c(color) : Theme::g(color).at(70))+safe_text(value,width);
}
std::string line(const Rect& r,int row,const std::string& value,const std::string& color="main_fg") {
	return row>0 and row<r.height-1 ? at(r.y+row,r.x+1,value,r.width-2,color) : "";
}
std::string title(const Host& host,const std::string& group,std::string value) {
	const auto found=host.groups.find(group);
	if (found!=host.groups.end() and text(*found,"status")!="ok") value+=" ["+text(*found,"status")+"]";
	return value;
}
std::string box(const Rect& r,const std::string& label,const std::string& color,const Draw::Style& style,const std::string& bottom="") {
	return Draw::createBox(r.x,r.y,r.width,r.height,Theme::c(color),true,safe_text(label,r.width-6),safe_text(bottom,r.width-6),0,style);
}
const std::deque<long long>& history(const Host& host,std::string_view key) {
	const auto found=host.series.find(std::string(key)); return found==host.series.end() ? no_history : found->second;
}
std::string graph(int x,int y,int width,int height,const std::deque<long long>& values,const std::string& color,const Draw::Style& style,long long maximum=100,bool invert=false) {
	if (width<1 or height<1) return "";
	if (values.empty()) return at(y,x,"N/A",width,"inactive_fg");
	Draw::Graph drawing(width,height,color,values,style.graph_symbol,invert,false,maximum,0,style);
	return Mv::to(y,x)+drawing();
}
std::string meter(int x,int y,int width,double value,const std::string& color) {
	if (width<1) return "";
	if (value<0) return at(y,x,"N/A",width,"inactive_fg");
	Draw::Meter drawing(width,color);
	return Mv::to(y,x)+drawing(int(std::clamp(value,0.0,100.0)));
}

// Border labels and inset frames share the same theme and cell coordinates as btop.
std::string rule(int y,int x,int width,const std::string& color="div_line") {
	std::string glyphs;
	for (int i=0; i<width; ++i) glyphs+="─";
	return at(y,x,glyphs,width,color);
}
std::string border(const Rect& r,int offset,const std::string& label,const std::string& color="title",bool bottom=false) {
	return at(r.y+(bottom ? r.height-1 : 0),r.x+offset," "+label+" ",std::max(0,r.width-offset-1),color);
}
std::string pair_line(const Rect& r,int row,const std::string& label,const std::string& value,const std::string& color="main_fg") {
	const int value_width=std::min(columns(value),r.width-2);
	return line(r,row,cell(label,r.width-2-value_width)+cell(value,value_width,true),color);
}
std::string temperature(double value) { return value<0 ? "--°C" : fmt::format("{:.0f}°C",value); }

std::string cpu(const Host& host,const Rect& r,const Draw::Style& style) {
	const auto& data=metric_data(host,"cpu"); const auto& cores=metric_items(data,"cores");
	// Keep the large history plot dominant even on machines with hundreds of cores.
	const int info_width=std::clamp(r.width/3,36,80), chart_width=r.width-info_width-3;
	const int column_count=std::max(1,(info_width-2)/24), column_width=(info_width-2)/column_count;
	const int rows=std::clamp(int((cores.size()+column_count-1)/column_count),1,r.height-6);
	const int page_size=column_count*rows, pages=std::max(1,int((cores.size()+page_size-1)/page_size));
	const int page=host.core_page%pages;
	auto label=title(host,"cpu","cpu "+safe_text(host.name,r.width/5)+" | "+host.status);
	auto out=box(r,label,"cpu_box",style);
	if (not host.sample_time.empty() and r.width>=150)
		out+=border(r,(r.width-13)/2,host.sample_time+" UTC");
	const auto users=r.width>=120 ? session_summary(host)+" [u]" : "users/sess "+session_summary(host,true)+" [u]";
	out+=border(r,r.width-columns(users)-3,users);

	const int upper=(r.height-3)/2, middle=r.y+1+upper, lower=r.height-4-upper;
	const auto lower_metric=host.cpu_metric=="system_pct" ? "user_pct" : "system_pct";
	out+=graph(r.x+1,r.y+1,chart_width,upper,history(host,host.cpu_metric),"cpu",style);
	out+=graph(r.x+1,middle+1,chart_width,lower,history(host,lower_metric),"cpu",style,100,true);
	out+=rule(middle,r.x+1,chart_width);
	const auto metrics=std::string(" ")+(host.cpu_metric=="usage_pct" ? "total" : host.cpu_metric)+" ▲▼ "+(std::string(lower_metric)=="system_pct" ? "system" : "user")+" ";
	out+=at(middle,r.x+1+std::max(0,(chart_width-columns(metrics))/2),metrics,chart_width,"graph_text");
	out+=at(r.y+r.height-2,r.x+2,"up "+duration(number(data,"uptime_seconds")),chart_width-1,"inactive_fg");
	out+=border(r,2,"g graph","inactive_fg",true);
	const Rect info{r.x+r.width-info_width-1,r.y+(r.height-rows-4)/2,info_width,rows+4};
	auto model=text(data,"model","CPU");
	if (number(data,"frequency_mhz")>=0 and columns(model)+12<info.width-6)
		model+=fmt::format("  {:.2f}GHz",number(data,"frequency_mhz")/1000);
	out+=box(info,model,"div_line",style,fmt::format("{} cores  [ ] {}/{}",host.cores,page+1,pages));
	out+=line(info,1,"CPU");
	const int bar_width=info.width-19;
	out+=meter(info.x+5,info.y+1,bar_width,number(data,"usage_pct"),"cpu");
	out+=at(info.y+1,info.x+5+bar_width,cell(percent(number(data,"usage_pct")),5,true)+" "+cell(temperature(number(data,"temperature_c")),6,true),12);
	for (int i=0; i<page_size and page*page_size+i<int(cores.size()); ++i) {
		const auto& core=cores[page*page_size+i]; const int id=core.value("id",0);
		const int x=info.x+1+(i/rows)*column_width, y=info.y+2+i%rows;
		const int plot_width=column_width-17;
		out+=at(y,x,fmt::format("C{:<4}",id),5);
		out+=graph(x+5,y,plot_width,1,history(host,"core:"+std::to_string(id)),"cpu",style);
		out+=at(y,x+5+plot_width,cell(percent(number(core,"usage_pct")),5,true),5);
		out+=Mv::to(y,x+10+plot_width)+Theme::g("temp").at(30)+cell(temperature(number(core,"temperature_c")),6,true);
		if (i/rows+1<column_count) out+=at(y,x+column_width-1,"│",1,"div_line");
	}
	const auto load=fmt::format("Load avg: {:.2f} {:.2f} {:.2f}",host.load[0],host.load[1],host.load[2]);
	out+=line(info,info.height-2,cell(load,info.width-2,true));
	return out;
}

// MEM and DISKS are two columns inside a single outer frame.
std::string memory(const Host& host,const Rect& r,const Draw::Style& style) {
	const auto& data=metric_data(host,"memory"); const double total=number(data,"total_bytes");
	std::string out=pair_line(r,1,"Total:",human(total),"title");
	int index=0;
	for (auto [name,label,color] : {std::tuple{"used","Used:","used"}, {"available","Available:","available"},
		{"cached","Cached:","cached"}, {"free","Free:","free"}}) {
		const int row=2+index*(r.height-3)/4, end=2+(index+1)*(r.height-3)/4;
		++index;
		const int stride=end-row;
		if (stride<1) continue;
		const auto value=number(data,(std::string(name)+"_bytes").c_str());
		const auto pct=total>0 and value>=0 ? 100*value/total : -1;
		out+=pair_line(r,row,label,human(value));
		if (stride>=3) {
			out+=line(r,row+1," "+percent(pct),color);
			const int plot_height=std::max(1,stride-3);
			out+=graph(r.x+1,r.y+row+2,r.width-2,plot_height,history(host,"mem:"+std::string(name)),color,style);
			if (stride>=4) out+=rule(r.y+end-1,r.x+1,r.width-2);
		} else if (stride==2) {
			out+=graph(r.x+1,r.y+row+1,r.width-7,1,history(host,"mem:"+std::string(name)),color,style);
			out+=at(r.y+row+1,r.x+r.width-6,cell(percent(pct),5,true),5,color);
		}
	}
	return out;
}

const Json& disk_for(const Json& devices,const Json& mount) {
	const auto id=text(mount,"device");
	for (const auto& device : devices) if (text(device,"id")==id) return device;
	return empty;
}
std::string disks(const Host& host,const Rect& r,const Draw::Style& style) {
	const auto& mounts=metric_items(metric_data(host,"filesystems"));
	const auto& devices=metric_items(metric_data(host,"disks"));
	const auto& mem=metric_data(host,"memory");
	Json swap{{"mount","swap"},{"total_bytes",number(mem,"swap_total_bytes")},{"used_bytes",number(mem,"swap_used_bytes")}};
	std::vector<const Json*> items;
	for (const auto& item : (host.disk_io ? devices : mounts)) items.push_back(&item);
	if (not host.disk_io and number(mem,"swap_total_bytes")>0) items.insert(items.begin()+std::min(std::size_t(1),items.size()),&swap);
	const int stride=host.disk_io ? (r.height>=12 ? 5 : 3) : (r.height>=12 ? 4 : 3);
	const int count=std::max(1,(r.height-2)/stride), pages=std::max(1,int((items.size()+count-1)/count));
	const int page=host.disk_page%pages;
	auto out=border(r,1,fmt::format("i {} d {}/{}",host.disk_io ? "I/O" : "size",page+1,pages),"title",true);
	if (items.empty()) return out+line(r,1,"No readable disks","inactive_fg");
	for (int i=0; i<count and page*count+i<int(items.size()); ++i) {
		const auto& item=*items[page*count+i]; const int row=1+i*stride;
		const auto& device=host.disk_io ? item : disk_for(devices,item);
		const auto id=text(device,"id");
		if (host.disk_io) {
			out+=pair_line(r,row,text(device,"name"),percent(number(device,"activity_pct")),"title");
			for (int direction=0; direction<2; ++direction) {
				const auto key=direction ? "write_bytes_per_sec" : "read_bytes_per_sec";
				const auto& values=history(host,"disk:"+id+":"+key);
				const auto maximum=values.empty() ? 1 : std::max(1LL,*std::max_element(values.begin(),values.end()));
				const int y=r.y+row+1+direction*(stride==5 ? 2 : 1);
				out+=graph(r.x+1,y,r.width-2,stride==5 ? 2 : 1,values,direction ? "upload" : "download",style,maximum);
				out+=at(y,r.x+1,std::string(direction ? "W " : "R ")+human(number(device,key))+"/s",r.width-2);
			}
		} else {
			auto mount=text(item,"mount"); if (mount=="/") mount="root";
			if (r.width>=36 and not device.empty()) mount+=" ("+text(device,"name")+")";
			out+=pair_line(r,row,mount,human(number(item,"total_bytes")),"title");
			if (&item==&swap) out+=line(r,row+1,"Free: "+human(std::max(0.0,number(item,"total_bytes")-number(item,"used_bytes"))),"inactive_fg");
			else if (device.empty()) out+=line(r,row+1,"I/O unmapped","inactive_fg");
			else {
				out+=line(r,row+1,"IO "+cell(percent(number(device,"activity_pct")),4));
				out+=graph(r.x+8,r.y+row+1,r.width-9,1,history(host,"disk:"+id+":activity_pct"),"available",style);
			}
			const auto total=number(item,"total_bytes"), used=number(item,"used_bytes");
			const auto pct=total>0 and used>=0 ? 100*used/total : -1;
			const int prefix=r.width>=24 ? 10 : 6, suffix=std::min(8,r.width/3), bar=r.width-2-prefix-suffix;
			out+=line(r,row+2,(r.width>=24 ? "Used: " : "U ")+percent(pct));
			out+=meter(r.x+1+prefix,r.y+row+2,bar,pct,"used");
			out+=at(r.y+row+2,r.x+r.width-1-suffix,cell(human(used),suffix,true),suffix);
			if (stride==4) out+=rule(r.y+row+3,r.x+1,r.width-2);
		}
	}
	return out;
}

std::string bits(double bytes) {
	if (bytes<0) return "N/A";
	double value=bytes*8; int unit=0;
	const char* units[]={"bps","Kibps","Mibps","Gibps","Tibps"};
	while (value>=1024 and unit<4) { value/=1024; ++unit; }
	return fmt::format("{:.1f} {}",value,units[unit]);
}
std::string network(const Host& host,const Rect& r,const Draw::Style& style) {
	const auto& interfaces=metric_items(metric_data(host,"network"));
	if (interfaces.empty()) return box(r,title(host,"network","net"),"net_box",style)+line(r,1,"No readable interface");
	const auto& item=interfaces[host.net_index%interfaces.size()]; const auto name=text(item,"name");
	const auto label=title(host,"network","net "+text(item,"address"));
	auto out=box(r,safe_text(label,r.width-6),"net_box",style);
	const auto controls=std::string(host.net_sync ? "sync" : "split")+" "+(host.net_auto ? "auto" : "fixed")+" zero b<"+name+">n";
	const int control_width=columns(controls)+2;
	const bool controls_top=columns(label)+control_width+7<r.width;
	out+=border(r,controls_top ? r.width-control_width-2 : 2,controls,"title",not controls_top);
	const auto rx=host.network_rx.find(name), tx=host.network_tx.find(name);
	const auto& receive=rx==host.network_rx.end() ? no_history : rx->second;
	const auto& send=tx==host.network_tx.end() ? no_history : tx->second;
	long long down_max=receive.empty() ? 10240 : std::max(10240LL,*std::max_element(receive.begin(),receive.end()));
	long long up_max=send.empty() ? 10240 : std::max(10240LL,*std::max_element(send.begin(),send.end()));
	if (not host.net_auto) down_max=up_max=10*1024*1024;
	if (host.net_sync) down_max=up_max=std::max(down_max,up_max);
	const int stat_width=std::clamp(r.width*32/100,17,32), plot_width=r.width-stat_width-3;
	const int down_height=(r.height-2)/2, up_height=r.height-2-down_height;
	out+=graph(r.x+1,r.y+1,plot_width,down_height,receive,"download",style,down_max);
	out+=graph(r.x+1,r.y+1+down_height,plot_width,up_height,send,"upload",style,up_max,true);
	out+=at(r.y+1,r.x+1,human(down_max),plot_width,"graph_text");
	out+=at(r.y+r.height-2,r.x+1,human(up_max),plot_width,"graph_text");
	const auto peaks=host.network_peak.contains(name) ? host.network_peak.at(name) : std::array<double,2>{};
	const auto offsets=host.network_offset.contains(name) ? host.network_offset.at(name) : std::array<double,2>{};
	const bool inset=r.height>=10;
	const int stat_height=inset ? std::min(10,r.height-2) : r.height;
	const Rect info{r.x+r.width-stat_width-1,r.y+(r.height-stat_height)/2,stat_width,stat_height};
	if (inset) out+=box(info,"download","div_line",style,"upload");
	for (int direction=0; direction<2; ++direction) {
		const int row=1+(direction ? (stat_height-2)/2 : 0), rows=(stat_height-2)/2;
		const auto speed=number(item,direction ? "tx_bytes_per_sec" : "rx_bytes_per_sec");
		const auto arrow=direction ? "▲ " : "▼ ";
		auto rate=std::string(arrow)+human(speed)+"/s";
		const auto bit_rate=" ("+bits(speed)+")";
		if (columns(rate+bit_rate)<=info.width-2) rate+=bit_rate;
		out+=line(info,row,rate,direction ? "upload" : "download");
		if (rows>=3) out+=pair_line(info,row+1,std::string(arrow)+"Top:",human(peaks[direction])+"/s");
		if (rows>=2) out+=pair_line(info,row+(rows>=3 ? 2 : 1),std::string(arrow)+"Total:",human(std::max(0.0,number(item,direction ? "tx_bytes_total" : "rx_bytes_total",0)-offsets[direction])));
	}
	return out;
}

const Json& selected_card(const Host& host) {
	for (const auto& item : metric_items(metric_data(host,"cards"),"devices")) if (item.at("id")==host.selected_card) return item;
	return empty;
}
std::string card_color(const std::string& state) {
	return state=="free" ? "cpu_start" : state=="allocated" or state=="error" or state=="external" ? "cpu_end" : state=="reserved" ? "cpu_mid" : "inactive_fg";
}
std::string cards(const Host& host,const Rect& r,const Draw::Style& style,bool folded=false) {
	const auto state=resource_status(host,"cards");
	const bool fresh=state=="ok" or state=="partial";
	const auto& data=metric_data(host,"cards");
	auto out=box(r,"cards"+std::string(host.cards_focus ? " [focus]" : "")+(fresh ? " | queue" : " | "+state),host.cards_focus ? "hi_fg" : "mem_box",style);
	out+=line(r,1,card_summary(host));
	if (folded or r.height<=4) return out+border(r,2,"v cards  ,/. page","inactive_fg",true);
	out+=line(r,2,queue_summary(host));
	const auto& items=metric_items(data,"devices");
	if (items.empty()) {
		out+=line(r,4,fresh ? "No card devices" : "Card data unavailable","inactive_fg");
		const auto found=host.groups.find("cards");
		if (found!=host.groups.end()) out+=line(r,5,text(*found,"reason"),"inactive_fg");
		return out;
	}
	int detail_rows=host.card_detail and r.height>=11 ? 3 : 0;
	const int count=card_visible_rows(host,r.height);
	int selected=0;
	for (std::size_t i=0; i<items.size(); ++i) if (items[i].at("id")==host.selected_card) selected=i;
	const int start=(selected/count)*count;
	const bool show_task=r.width>=68, show_group=r.width>=50;
	const int user_width=std::clamp(r.width/6,8,16), task_width=std::max(8,r.width-user_width-35);
	auto table=[&](const std::string& id,const std::string& group,const std::string& kind,const std::string& user,const std::string& task,const std::string& age) {
		return cell(id,4,true)+" "+(show_group ? cell(group,5)+" " : "")+cell(kind,9)+" "+cell(user,user_width)+" "+(show_task ? cell(task,task_width)+" " : "")+age;
	};
	out+=line(r,3,table("ID","GROUP","STATE","USER","TASK","AGE"),"title");
	for (int i=0; i<count and start+i<int(items.size()); ++i) {
		const auto& item=items[start+i];
		const auto& allocation=item.at("allocation"); const auto& reservation=item.at("reservation");
		const auto& owner=allocation.is_object() ? allocation : reservation.is_object() ? reservation : empty;
		const auto kind=item.at("state").get<std::string>();
		const auto label=kind=="allocated" ? "HELD" : kind=="free" ? "FREE" : kind=="reserved" ? "RESERVED" : kind=="disabled" ? "DISABLED" : kind=="error" ? "ERROR" : kind=="external" ? "EXTERNAL" : "UNKNOWN";
		const auto elapsed=number(allocation,"elapsed_seconds");
		const auto age=elapsed<0 ? "-" : fmt::format("{}m",static_cast<long long>(elapsed)/60);
		const auto value=table(text(item,"id"),text(item,"group_id","?"),fresh ? label : "last",text(owner,"user","-"),text(owner,"task_id","-"),age);
		if (host.cards_focus and start+i==selected) {
			out+=Mv::to(r.y+4+i,r.x+1)+Theme::c("selected_bg")+std::string(r.width-2,' ')+Mv::to(r.y+4+i,r.x+1)+Theme::c("selected_fg")+safe_text(value,r.width-2)+Theme::c("main_bg");
		} else out+=line(r,4+i,value,fresh ? card_color(kind) : "inactive_fg");
	}
	if (detail_rows) {
		const auto& item=selected_card(host); const int row=r.height-4;
		if (not item.empty()) {
			const auto& allocation=item.at("allocation"); const auto& reservation=item.at("reservation");
			out+=line(r,row,"Task: "+text(allocation,"task_id","-")+" / "+text(allocation,"user","-"));
			out+=line(r,row+1,"Reserved: "+text(reservation,"user","-")+" / "+text(reservation,"task_id","-"));
			std::string reasons; for (const auto& reason : item.at("reason_codes")) { if (not reasons.empty()) reasons+=","; reasons+=reason.get<std::string>(); }
			out+=line(r,row+2,"Reason: "+(reasons.empty() ? "none" : reasons),"inactive_fg");
		}
	}
	const auto position=fmt::format("{}/{} p{}/{}",selected+1,items.size(),selected/count+1,(items.size()+count-1)/count);
	const auto controls=host.cards_focus ? (r.width>=65 ? "↑↓ select  PgUp/Dn ,/. page" : "↑↓ select  ,/. page") : "H usage  v focus  ,/. page";
	out+=border(r,2,safe_text(controls,r.width-columns(position)-8),"inactive_fg",true);
	out+=border(r,r.width-columns(position)-3,position,"title",true);
	return out;
}
std::string user_list(const Host& host,const Rect& r,const Draw::Style& style) {
	const auto state=resource_status(host,"sessions");
	auto out=box(r,"users | "+state,"hi_fg",style,"u / Esc close  PgUp/PgDn page");
	out+=line(r,1,session_summary(host));
	out+=line(r,2,"Registered login sessions; accounts are deduplicated","inactive_fg");
	out+=line(r,4,cell("USER",26)+cell("SESSIONS",10)+"SINCE (UTC)","title");
	const auto& items=metric_items(metric_data(host,"sessions"));
	const int count=std::max(1,r.height-7), pages=std::max(1,int(items.size()+count-1)/count), start=(host.users_page%pages)*count;
	for (int i=0; i<count and start+i<int(items.size()); ++i) {
		const auto& item=items[start+i]; std::string since="N/A";
		if (item.at("first_login_at").is_number_integer()) {
			const auto stamp=item.at("first_login_at").get<std::time_t>(); std::tm tm{}; char formatted[32];
			if (gmtime_r(&stamp,&tm) and std::strftime(formatted,sizeof(formatted),"%m-%d %H:%M",&tm)) since=formatted;
		}
		out+=line(r,5+i,cell(text(item,"user"),26)+cell(fmt::format("{:.0f}",number(item,"sessions")),10)+since,state=="ok" or state=="partial" ? "main_fg" : "inactive_fg");
	}
	if (items.empty()) out+=line(r,5,state=="ok" ? "No registered logins" : "Login records unavailable","inactive_fg");
	return out;
}

const Json& chosen_process(const Host& host) {
	const auto& data=metric_data(host,"processes");
	const auto chosen=data.find("selected");
	if (chosen!=data.end() and chosen->is_object() and chosen->value("pid",0)==host.selected_pid and chosen->value("start_time_ticks",std::uint64_t{})==host.selected_start) return *chosen;
	for (const auto& item : metric_items(data)) if (item.value("pid",0)==host.selected_pid and item.value("start_time_ticks",std::uint64_t{})==host.selected_start) return item;
	return empty;
}
std::string process_detail(const Host& host,const Rect& r,const Draw::Style& style) {
	const auto& item=chosen_process(host); const auto key=process_key(item);
	const auto group=host.groups.find("processes");
	const bool loading=(group!=host.groups.end() and text(*group,"status")=="warming_up") or (not item.empty() and text(item,"detail_status").empty());
	auto out=box(r,fmt::format("proc info {} {}{}",host.selected_pid,text(item,"name"),loading ? " [loading]" : ""),"proc_box",style,"Enter hide");
	if (text(item,"detail_status")=="gone") return out+line(r,1,"Process exited or identity changed","inactive_fg");
	if (item.empty() or text(item,"detail_status")=="unavailable") {
		out+=line(r,1,loading ? "Loading selected process details..." : "Process details unavailable","inactive_fg");
		if (group!=host.groups.end()) out+=line(r,2,text(*group,"reason"),"inactive_fg");
		return out;
	}
	const int graph_width=std::max(10,r.width/3), x=r.x+graph_width+2, available=r.width-graph_width-3;
	const auto& cpu_values=history(host,"proc:"+key); const auto& mem_values=history(host,"rss:"+key);
	const auto max_cpu=cpu_values.empty() ? 100 : std::max(100LL,*std::max_element(cpu_values.begin(),cpu_values.end()));
	const int command_rows=r.height>=11 ? 3 : 1, command_row=r.height-1-command_rows;
	out+=graph(r.x+1,r.y+1,graph_width,command_row-2,cpu_values,"cpu",style,max_cpu);
	out+=at(r.y+1,r.x+1,"CPU "+percent(number(item,"cpu_pct")),graph_width);
	out+=at(r.y+1,x,"State "+text(item,"state","?")+"  "+duration(number(item,"elapsed_seconds")),available);
	out+=at(r.y+2,x,"User "+text(item,"user","?")+fmt::format("  Thr {:.0f}",number(item,"threads")),available);
	out+=at(r.y+3,x,fmt::format("PPID {:.0f}  Nice {:.0f}",number(item,"ppid"),number(item,"nice")),available);
	if (command_row>4) out+=at(r.y+4,x,"RSS "+human(number(item,"rss_bytes")),available);
	if (r.height>=9) out+=at(r.y+5,x,"IO R/W "+human(number(item,"read_bytes_total"))+"/"+human(number(item,"write_bytes_total")),available);
	if (not mem_values.empty()) out+=graph(r.x+1,r.y+command_row-1,graph_width,1,mem_values,"used",style,std::max(1LL,*std::max_element(mem_values.begin(),mem_values.end())));
	auto command=safe_text(text(item,"command","N/A"),4096);
	for (int i=0; i<command_rows; ++i) {
		const auto part=safe_text(command,r.width-6);
		out+=line(r,command_row+i,(i ? "    " : "CMD ")+part);
		command.erase(0,part.size());
	}
	return out;
}
std::string processes(const Host& host,Rect r,const Draw::Style& style) {
	std::string out;
	if (host.proc_detail and r.height>=13) {
		const int detail_height=std::min(r.height>=26 ? 11 : 9,r.height-5);
		out=process_detail(host,{r.x,r.y,r.width,detail_height},style); r.y+=detail_height; r.height-=detail_height;
	}
	const auto& data=metric_data(host,"processes"); const auto& items=metric_items(data); const auto rows=process_rows(host);
	int selected=-1;
	for (std::size_t i=0; i<rows.size(); ++i) {
		const auto& item=items[rows[i].index];
		if (item.value("pid",0)==host.selected_pid and item.value("start_time_ticks",std::uint64_t{})==host.selected_start) selected=i;
	}
	const int count=std::max(1,r.height-3), start=selected<0 ? 0 : (selected/count)*count;
	auto label=title(host,"processes","proc | "+host.sort+(host.reverse ? " rev" : ""));
	label+=" /"+(host.filter.empty() ? "filter" : host.filter);
	out+=box(r,label,"proc_box",style);
	const std::string controls=std::string("R reverse  T ")+(host.proc_tree ? "TREE" : "tree")+"  s sort";
	if (columns(label)+columns(controls)+7<r.width) out+=border(r,r.width-columns(controls)-3,controls);
	const auto totals=fmt::format("{}/{:.0f}{}",selected<0 ? 0 : selected+1,number(data,"matched",0),data.value("truncated",false) ? " top" : "");
	const auto actions=r.width>=60 ? "↑↓ select  Enter info" : "↑↓ select ↵ info";
	out+=border(r,2,actions,"title",true);
	out+=border(r,r.width-columns(totals)-3,totals,"title",true);
	// A growing terminal gives the extra columns to the command, as in btop.
	const int width=r.width-3;
	const bool spark=width>=54, threads=width>=50, command=width>=59 and not host.proc_tree;
	const int user_width=std::clamp(width/9,6,12), cpu_width=6;
	const int fixed=7+(threads ? 5 : 0)+user_width+1+8+(spark ? 6 : 0)+cpu_width;
	const int available=width-fixed;
	const int name_width=command ? std::clamp(available/3,10,22) : available-1;
	const int command_width=command ? available-name_width-2 : 0;
	int offset=r.x+1;
	const int pid_x=offset; offset+=7;
	const int name_x=offset; offset+=name_width+1;
	const int cmd_x=offset; if (command) offset+=command_width+1;
	const int thread_x=offset; if (threads) offset+=5;
	const int user_x=offset; offset+=user_width+1;
	const int mem_x=offset; offset+=8;
	const int spark_x=offset; if (spark) offset+=6;
	const int cpu_x=offset;
	auto field=[&](int y,int x,int length,const std::string& value,const std::string& color,bool right=false) {
		return Mv::to(y,x)+color+cell(value,length,right);
	};
	auto table=[&](int y,const std::string& pid,const std::string& name,const std::string& cmd,const std::string& thr,const std::string& user,const std::string& mem,const std::string& cpu,const std::string& plain,const std::string& accent) {
		auto result=field(y,pid_x,6,pid,plain,true)+field(y,name_x,name_width,name,accent);
		if (command) result+=field(y,cmd_x,command_width,cmd,plain);
		if (threads) result+=field(y,thread_x,4,thr,accent,true);
		return result+field(y,user_x,user_width,user,plain)+field(y,mem_x,7,mem,accent,true)+field(y,cpu_x,cpu_width,cpu,accent,true);
	};
	out+=table(r.y+1,"PID",host.proc_tree ? "TREE" : "PROGRAM","COMMAND","THR","USER","MEM","CPU%",Theme::c("title"),Theme::c("title"));
	for (int i=0; i<count and start+i<int(rows.size()); ++i) {
		const auto& row=rows[start+i]; const auto& item=items[row.index]; const bool active=start+i==selected;
		const int y=r.y+2+i;
		if (active) out+=Mv::to(y,r.x+1)+Theme::c("selected_bg")+std::string(width,' ');
		const auto prefix=host.proc_tree ? std::string(std::min(row.depth*2,std::max(0,name_width-4)),' ')+(row.children ? (host.collapsed.contains(process_key(item)) ? "+ " : "- ") : "  ") : "";
		const auto plain=active ? Theme::c("selected_fg") : (style.tty_mode ? Theme::c("main_fg") : Theme::g("proc").at(i*70/count));
		const auto accent=active ? Theme::c("selected_fg") : (style.tty_mode ? Theme::c("main_fg") : Theme::g("proc_color").at(100-i*65/count));
		const auto cpu_value=number(item,"cpu_pct");
		out+=table(y,fmt::format("{:.0f}",number(item,"pid")),prefix+text(item,"name"),text(item,"command"),fmt::format("{:.0f}",number(item,"threads")),text(item,"user",fmt::format("{:.0f}",number(item,"uid"))),human(number(item,"rss_bytes")),cpu_value<0 ? "N/A" : fmt::format("{:.1f}",cpu_value),plain,accent);
		if (spark) {
			const auto& values=history(host,"proc:"+process_key(item));
			const auto maximum=values.empty() ? 100 : std::max(100LL,*std::max_element(values.begin(),values.end()));
			out+=graph(spark_x,y,5,1,values,"cpu",style,maximum);
		}
		out+=Theme::c("main_bg");
	}
	if (int(rows.size())>count) {
		const int track_height=count, thumb_height=std::max(1,track_height*count/int(rows.size()));
		const int thumb_start=(track_height-thumb_height)*start/std::max(1,int(rows.size())-count);
		for (int i=0; i<track_height; ++i)
			out+=at(r.y+2+i,r.x+r.width-2,i>=thumb_start and i<thumb_start+thumb_height ? "█" : "│",1,i>=thumb_start and i<thumb_start+thumb_height ? "main_fg" : "inactive_fg");
	}
	return out;
}
}

std::string detail_view(const Host& host,int width,int height,const Draw::Style& style) {
	const int cpu_height=std::max(7,((height-2)*32+50)/100), y=cpu_height+3;
	const int remaining=height-y, left=std::max(34,(width-4)*45/100);
	const int net_height=std::max(6,remaining*42/100), memory_height=remaining-net_height-1;
	const int mem_width=left/2;
	std::string out=cpu(host,{2,2,width-3,cpu_height},style);
	const Rect mem{2,y,mem_width+1,memory_height}, disk{2+mem_width,y,left-mem_width,memory_height};
	out+=box({2,y,left,memory_height},title(host,"memory","mem"),"mem_box",style);
	out+=at(y,disk.x,"┬",1,"mem_box");
	out+=at(y+memory_height-1,disk.x,"┴",1,"mem_box");
	for (int row=1; row<memory_height-1; ++row) out+=at(y+row,disk.x,"│",1,"div_line");
	out+=border(disk,1,title(host,host.disk_io ? "disks" : "filesystems","disks"));
	out+=memory(host,mem,style);
	out+=disks(host,disk,style);
	out+=network(host,{2,y+memory_height+1,left,net_height},style);
	const Rect right{left+3,y,width-left-4,remaining};
	if (remaining<18 and host.cards_focus) out+=host.usage_tab ? usage_panel(host,right,style) : cards(host,right,style);
	else if (host.proc_detail) out+=processes(host,right,style);
	else {
		const bool folded=host.cards_folded or remaining<18;
		const int cards_height=card_panel_height(host,height);
		out+=host.usage_tab ? usage_panel(host,{right.x,right.y,right.width,cards_height},style,folded) : cards(host,{right.x,right.y,right.width,cards_height},style,folded);
		out+=processes(host,{right.x,right.y+cards_height+1,right.width,right.height-cards_height-1},style);
	}
	return out;
}
std::string users_overlay(const Host& host,int width,int height,const Draw::Style& style) {
	if (width<80 or height<24) return "";
	const int w=std::min(78,width-6),h=std::min(22,height-4);
	return user_list(host,{(width-w)/2,(height-h)/2,w,h},style);
}

}
