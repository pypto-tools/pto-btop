// SPDX-License-Identifier: Apache-2.0
#include "resources.hpp"
#include <array>
#include <set>
#include <stdexcept>

namespace Protocol {
namespace {
void check(bool ok) { if (not ok) throw std::runtime_error("Invalid card usage snapshot"); }
bool num(const Json& v,std::uint64_t max=1000000000000ULL) { return v.is_number_integer() and v>=0 and v<=max; }
bool text(const Json& v,std::size_t max=256) { return v.is_string() and v.get_ref<const std::string&>().size()<=max; }
bool date(const Json& v) {
	if (not text(v,10) or v.get_ref<const std::string&>().size()!=10) return false;
	const auto s=v.get<std::string>();
	for (int i=0;i<10;++i) if (i==4 or i==7 ? s[i]!='-' : s[i]<'0' or s[i]>'9') return false;
	return s.substr(5,2)>="01" and s.substr(5,2)<="12" and s.substr(8,2)>="01" and s.substr(8,2)<="31";
}
}
Json usage_snapshot(const Json& source) {
	check(source.is_object() and num(source.at("schema_version"),1) and source.at("schema_version")==1);
	const auto status=source.at("status").get<std::string>();
	check(status=="ok" or status=="partial" or status=="unavailable");
	check(source.at("reason").is_null() or text(source.at("reason"),4096));
	check(source.at("observed_at").is_null() or text(source.at("observed_at"),64));
	check(source.at("age_ms").is_null() or num(source.at("age_ms")));
	check(source.at("covered_from").is_null() or num(source.at("covered_from")));
	check(text(source.at("timezone"),128) and not source.at("timezone").get<std::string>().empty());
	check(source.at("cached").is_boolean());
	Json result;
	for (const auto* key : {"schema_version","status","reason","observed_at","age_ms","cached","timezone","covered_from"}) result[key]=source.at(key);
	result["windows"]=Json::array(); const auto& windows=source.at("windows");
	check(windows.is_array());
	if (windows.empty()) { check(status=="unavailable"); return result; }
	check(windows.size()==3 and text(source.at("observed_at"),64));
	const std::array<std::string,3> ids={"today","7d","30d"}; const std::array<int,3> days={1,7,30};
	for (std::size_t i=0;i<windows.size();++i) {
		const auto& w=windows[i]; check(w.at("id")==ids[i]);
		for (const auto* key : {"start","end","card_seconds","task_count"}) check(num(w.at(key)));
		check(w.at("start")<=w.at("end") and w.at("complete").is_boolean());
		if (w.at("complete")==true) check(not source.at("covered_from").is_null() and source.at("covered_from")<=w.at("start"));
		if (status=="ok") check(w.at("complete")==true);
		check(w.at("end")==windows[0].at("end"));
		const auto& users=w.at("users"); const auto& daily=w.at("daily");
		check(users.is_array() and users.size()<=512 and daily.is_array() and daily.size()==std::size_t(days[i]));
		Json window; for (const auto* key : {"id","start","end","complete","card_seconds","task_count"}) window[key]=w.at(key);
		window["users"]=Json::array(); window["daily"]=Json::array();
		std::uint64_t seconds=0,tasks=0,day_seconds=0; std::set<std::string> seen;
		for (const auto& row : users) {
			check(text(row.at("user")) and not row.at("user").get<std::string>().empty());
			check(row.at("uid").is_null() or num(row.at("uid"),4294967295ULL));
			check(num(row.at("card_seconds")) and num(row.at("task_count")));
			check(row.at("active_cards").is_null() or num(row.at("active_cards"),256));
			check(seen.insert(Json::array({row.at("user"),row.at("uid")}).dump()).second);
			seconds+=row.at("card_seconds").get<std::uint64_t>(); tasks+=row.at("task_count").get<std::uint64_t>();
			Json item; for (const auto* key : {"user","uid","card_seconds","task_count","active_cards"}) item[key]=row.at(key);
			window["users"].push_back(std::move(item));
		}
		std::string previous;
		for (const auto& row : daily) {
			check(date(row.at("date")) and row.at("date").get<std::string>()>previous and num(row.at("card_seconds")) and row.at("complete").is_boolean());
			previous=row.at("date"); day_seconds+=row.at("card_seconds").get<std::uint64_t>();
			if (w.at("complete")==true) check(row.at("complete")==true);
			window["daily"].push_back({{"date",row.at("date")},{"card_seconds",row.at("card_seconds")},{"complete",row.at("complete")}});
		}
		check(seconds==w.at("card_seconds") and day_seconds==seconds and tasks==w.at("task_count"));
		result["windows"].push_back(std::move(window));
	}
	return result;
}
}
