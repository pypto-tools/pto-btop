// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <clocale>
#include "view.hpp"
#include "frame.hpp"
#include "btop_theme.hpp"
#include "btop_tools.hpp"

TEST(cluster_state, demo_user_filters_are_partial_and_independent_per_host) {
	auto hosts=Cluster::demo_hosts();
	hosts[0].filter="user:VELO";
	hosts[1].filter="user:nobody";
	hosts[2].filter="PyThOn";
	Cluster::advance_demo(hosts,301);
	EXPECT_EQ(Cluster::metric_data(hosts[0],"processes")["matched"],40);
	EXPECT_TRUE(Cluster::metric_items(Cluster::metric_data(hosts[1],"processes")).empty());
	EXPECT_EQ(Cluster::metric_data(hosts[2],"processes")["matched"],20);
	EXPECT_EQ(Cluster::metric_data(hosts[3],"processes")["matched"],40);
	hosts[1].filter="";
	Cluster::advance_demo(hosts,302);
	EXPECT_EQ(Cluster::metric_data(hosts[1],"processes")["matched"],40);
}

TEST(cluster_render, remote_names_cannot_control_terminal_or_exceed_width) {
	const std::string previous = std::setlocale(LC_CTYPE, nullptr);
	ASSERT_NE(std::setlocale(LC_CTYPE, "C.UTF-8"), nullptr);
	EXPECT_EQ(Cluster::safe_text("worker\x1b[2J\nname", 80), "worker?[2J?name");
	EXPECT_EQ(Cluster::safe_text("a\xe2\x80\xae" "b", 80), "a?b");
	EXPECT_EQ(Cluster::safe_text("\xe7\xa3\x81\xe7\x9b\x98-ab", 5), "\xe7\xa3\x81\xe7\x9b\x98-");
	EXPECT_EQ(Cluster::safe_text(std::string("\xff") + "abc", 3), "?ab");
	EXPECT_TRUE(Cluster::safe_text("worker", 0).empty());
	std::setlocale(LC_CTYPE, previous.c_str());
}

TEST(cluster_layout, bounded_cards_and_compact_fallback) {
	for (const auto& [width, height] : {std::pair{120, 36}, {160, 48}, {121, 37}}) {
		for (int count = 1; count <= 4; ++count) {
			const auto geometry = Cluster::layout(width, height, count);
			ASSERT_EQ(geometry.layout, Cluster::Layout::cards);
			ASSERT_EQ(geometry.cards.size(), count);
			for (const auto& card : geometry.cards) {
				EXPECT_GE(card.x, 1); EXPECT_GE(card.y, 3);
				EXPECT_LT(card.x + card.width, width);
				EXPECT_LE(card.y + card.height, height - 1);
			}
		}
	}
	EXPECT_EQ(Cluster::layout(80, 24, 4).layout, Cluster::Layout::compact);
	EXPECT_EQ(Cluster::layout(119, 60, 4).layout, Cluster::Layout::compact);
	EXPECT_EQ(Cluster::layout(120, 35, 4).layout, Cluster::Layout::compact);
	EXPECT_EQ(Cluster::layout(79, 24, 4).layout, Cluster::Layout::too_small);
	EXPECT_THROW(Cluster::layout(120, 36, 0), std::invalid_argument);
}

TEST(cluster_render, explicit_theme_and_independent_graphs) {
	Theme::setTheme(Theme::Options{});
	const Draw::Style style;
	std::deque<long long> low(40, 10), high(40, 90);
	Draw::Graph a(20, 4, "cpu", low, "braille", false, false, 0, 0, style);
	const auto before = a();
	Draw::Graph b(20, 4, "cpu", high, "braille", false, false, 0, 0, style);
	EXPECT_NE(before, b());
	high.push_back(0); b(high);
	EXPECT_EQ(before, a());
	EXPECT_THROW((Draw::Graph(0, 4, "cpu", low, "braille", false, false, 0, 0, style)), std::invalid_argument);
}

TEST(cluster_render, redraw_does_not_advance_history) {
	Theme::setTheme(Theme::Options{});
	const auto hosts = Cluster::demo_hosts();
	const auto before = hosts[0].history;
	const auto frame = Cluster::render(hosts, 120, 36, 0, false, {});
	const auto other = Cluster::render(hosts, 120, 36, 1, false, {});
	EXPECT_NE(frame, other);
	EXPECT_EQ(hosts[0].history, before);
	EXPECT_EQ(frame, Cluster::render(hosts, 120, 36, 0, false, {}));
	EXPECT_NE(frame.find("DEMO - simulated data"), std::string::npos);
	for (const auto& host : hosts) EXPECT_NE(frame.find(host.name), std::string::npos);
}

TEST(cluster_render, small_terminal_and_tty_theme) {
	Theme::Options theme;
	theme.tty_mode = true;
	Theme::setTheme(theme);
	const auto hosts = Cluster::demo_hosts();
	EXPECT_NE(Cluster::render(hosts, 40, 10, 0, false, {}).find("Resize terminal"), std::string::npos);
	const auto frame = Cluster::render(hosts, 80, 24, 0, false, {true, false, "tty"});
	EXPECT_NE(frame.find("Compact overview"), std::string::npos);
	EXPECT_EQ(frame.find("38;2;"), std::string::npos);
	for (const auto& host : hosts) EXPECT_NE(frame.find(host.name), std::string::npos);
}

TEST(cluster_state, tree_selection_collapse_and_identity_history) {
	auto hosts=Cluster::demo_hosts(); auto& host=hosts.front();
	host.proc_tree=true;
	const auto before=Cluster::process_rows(host);
	ASSERT_EQ(before.size(),40);
	EXPECT_EQ(before.front().depth,0);
	EXPECT_TRUE(before.front().children);
	Cluster::move_process(host,-1000);
	EXPECT_EQ(host.selected_pid,1000);
	Cluster::toggle_process(host);
	EXPECT_EQ(Cluster::process_rows(host).size(),1);
	Cluster::toggle_process(host);
	Cluster::move_process(host,1);
	EXPECT_EQ(host.selected_pid,1001);
	host.proc_detail=true;
	const auto old_key=std::to_string(host.selected_pid)+":"+std::to_string(host.selected_start);
	ASSERT_TRUE(host.series.contains("proc:"+old_key));
	auto groups=host.groups;
	for (auto& process : groups["processes"]["data"]["items"]) if (process["pid"]==host.selected_pid) process["start_time_ticks"]=999999;
	groups["processes"]["data"]["selected"]=nullptr;
	Cluster::ingest(host,{{"groups",groups}});
	EXPECT_FALSE(host.series.contains("proc:"+old_key));
	EXPECT_NE(host.selected_start,999999); // Pin must not follow a reused PID.
	EXPECT_TRUE(hosts[1].series.contains("proc:"+old_key));
	Cluster::clear_histories(host);
	EXPECT_TRUE(host.series.empty()); EXPECT_FALSE(hosts[1].series.empty());
}

TEST(cluster_state, tree_handles_missing_parents_and_cycles_without_duplicate_rows) {
	auto hosts=Cluster::demo_hosts(); auto& host=hosts.front(); host.proc_tree=true;
	auto& items=host.groups["processes"]["data"]["items"];
	items[0]["ppid"]=1001; items[1]["ppid"]=1000; items[2]["ppid"]=99999;
	const auto rows=Cluster::process_rows(host);
	EXPECT_EQ(rows.size(),40);
	std::set<std::size_t> unique;
	for (const auto& row : rows) unique.insert(row.index);
	EXPECT_EQ(unique.size(),rows.size());
}

TEST(cluster_render, full_detail_uses_btop_panels_and_never_advances_metric_history) {
	std::setlocale(LC_CTYPE,"C.UTF-8");
	Theme::setTheme(Theme::Options{});
	auto hosts=Cluster::demo_hosts(); auto& host=hosts.front();
	const auto before=host.series;
	auto frame=Cluster::render(hosts,160,48,0,true,{});
	for (auto label : {"AMD EPYC","Total","Avail","Cache","Free","root","100.90.0.1","Top","COMMAND","THR","USER"}) EXPECT_NE(frame.find(label),std::string::npos)<<label;
	host.proc_detail=true; host.proc_tree=true; host.disk_io=true;
	frame=Cluster::render(hosts,160,48,0,true,{});
	for (auto label : {"proc info","CMD","State","TREE","I/O"}) EXPECT_NE(frame.find(label),std::string::npos)<<label;
	EXPECT_EQ(host.series,before);
	host.proc_detail=false; host.proc_tree=false; host.disk_io=false;
	Cluster::Screen screen(200,60);
	screen.apply(Cluster::render(hosts,200,60,0,true,{}));
	std::string visible;
	for (std::size_t i=0; i<screen.cells().size(); ++i) {
		visible+=screen.cells()[i].text;
		if ((i+1)%200==0) visible+="\n";
	}
	for (const auto label : {"total ▲▼ system","Load avg:","download","upload","clang++ -O2 worker.cpp"})
		EXPECT_NE(visible.find(label),std::string::npos)<<label;
	EXPECT_NE(visible.find("┬ disks"),std::string::npos); // One shared MEM/DISKS border.
}


TEST(cluster_frame, unchanged_frame_emits_nothing_and_changes_preserve_other_cells) {
	std::setlocale(LC_CTYPE,"C.UTF-8");
	Cluster::FrameDiff frames;
	Cluster::Screen terminal(20,4);
	const std::string initial="\x1b[2J\x1b[1;1HStatic title\x1b[2;1H\x1b[31mCPU 10%";
	const auto first=frames.update(initial,20,4);
	EXPECT_NE(first.find("\x1b[2J"),std::string::npos);
	terminal.apply(first);
	EXPECT_TRUE(frames.update(initial,20,4).empty());
	const std::string changed="\x1b[2J\x1b[1;1HStatic title\x1b[2;1H\x1b[32mCPU 20%";
	const auto delta=frames.update(changed,20,4);
	EXPECT_EQ(delta.find("\x1b[2J"),std::string::npos);
	EXPECT_EQ(delta.find("Static title"),std::string::npos);
	EXPECT_LT(delta.size(),first.size());
	EXPECT_TRUE(delta.starts_with(Term::sync_start)); EXPECT_TRUE(delta.ends_with(Term::sync_end));
	terminal.apply(delta);
	Cluster::Screen expected(20,4); expected.apply(changed);
	EXPECT_EQ(terminal.cells(),expected.cells());
	EXPECT_EQ(terminal.cells()[20].style.foreground,"32");
	EXPECT_NE(frames.update(changed,21,4).find("\x1b[2J"),std::string::npos);
}

TEST(cluster_frame, wide_combining_shorter_text_and_color_only_updates) {
	std::setlocale(LC_CTYPE,"C.UTF-8");
	Cluster::FrameDiff frames; Cluster::Screen terminal(12,3);
	for (const std::string drawing : {
		"\x1b[2J\x1b[1;2H\x1b[31;44m界e\xcc\x81",
		"\x1b[2J\x1b[1;2H\x1b[31;44mA",
		"\x1b[2J\x1b[1;2H\x1b[32;45mA",
		"\x1b[2J\x1b[1;1H\x1b[38;2;1;2;3;48;5;22m宽字"}) {
		terminal.apply(frames.update(drawing,12,3));
		Cluster::Screen expected(12,3); expected.apply(drawing);
		EXPECT_EQ(terminal.cells(),expected.cells());
	}
	EXPECT_EQ(terminal.cells()[0].style.foreground,"38;2;1;2;3");
	EXPECT_EQ(terminal.cells()[0].style.background,"48;5;22");
	EXPECT_EQ(terminal.cells()[1].width,0);
}

TEST(cluster_frame, real_panels_theme_and_overlays_match_full_frames_without_clears) {
	std::setlocale(LC_CTYPE,"C.UTF-8");
	for (const auto& [width,height] : {std::pair{80,24},{120,36},{160,48},{200,60}}) {
		Cluster::FrameDiff frames; Cluster::Screen terminal(width,height);
		auto hosts=Cluster::demo_hosts();
		Theme::Options theme;
		for (int step=0; step<7; ++step) {
			theme.tty_mode=step==5; Theme::setTheme(theme);
			hosts[0].proc_detail=step>=2 and step<4; hosts[0].disk_io=step==4;
			if (step==1) Cluster::advance_demo(hosts,100);
			auto frame=Cluster::render(hosts,width,height,0,step!=6,{theme.tty_mode,true,theme.tty_mode ? "tty" : "braille"});
			if (step==3) frame+="\x1b[8;8H\x1b[0;44mFilter: test";
			const auto delta=frames.update(frame,width,height);
			if (step>0) EXPECT_EQ(delta.find("\x1b[2J"),std::string::npos);
			terminal.apply(delta);
			Cluster::Screen expected(width,height); expected.apply(frame);
			EXPECT_EQ(terminal.cells(),expected.cells())<<width<<"x"<<height<<" step "<<step;
		}
	}
}

TEST(cluster_state, card_selection_owner_uid_and_unavailable_resources) {
	auto hosts=Cluster::demo_hosts(); auto& host=hosts[0];
	Cluster::move_card(host,2); EXPECT_EQ(host.selected_card,"2");
	host.cards_focus=true;
	EXPECT_TRUE(Cluster::filter_card_owner(host)); EXPECT_EQ(host.filter,"uid:1000"); EXPECT_FALSE(host.cards_focus);
	Cluster::advance_demo(hosts,400); EXPECT_EQ(Cluster::metric_data(host,"processes")["matched"],40);
	std::reverse(host.groups["cards"]["data"]["devices"].begin(),host.groups["cards"]["data"]["devices"].end());
	Cluster::move_card(host,0); EXPECT_EQ(host.selected_card,"2");
	host.groups["cards"]["data"]["age_ms"]=16000;
	EXPECT_EQ(Cluster::resource_status(host,"cards"),"STALE"); EXPECT_FALSE(Cluster::filter_card_owner(host));
	EXPECT_EQ(Cluster::card_summary(host,true),"--");
	host.groups["cards"]["data"]["age_ms"]=0;
	host.groups["cards"]["data"]["scheduler"]["state"]="maintenance";
	EXPECT_EQ(Cluster::resource_status(host,"cards"),"MAINTENANCE"); EXPECT_FALSE(Cluster::filter_card_owner(host));
	host.groups["cards"]["data"]["scheduler"]["state"]="running";
	host.groups["cards"]["data"]["inventory_complete"]=false;
	EXPECT_EQ(Cluster::resource_status(host,"cards"),"UNKNOWN"); EXPECT_EQ(Cluster::card_summary(host,true),"--");
	host.groups.erase("cards"); host.groups.erase("sessions");
	EXPECT_EQ(Cluster::resource_status(host,"cards"),"N/A"); EXPECT_EQ(Cluster::session_summary(host,true),"--/--");
}
TEST(cluster_frame, card_focus_details_fold_and_users_overlay_preserve_frames) {
	std::setlocale(LC_CTYPE,"C.UTF-8"); Theme::setTheme(Theme::Options{});
	for (const	auto& [width,height] : {std::pair{80,24},{120,36},{160,48},{200,60}}) {
		auto hosts=Cluster::demo_hosts(); Cluster::FrameDiff frames; Cluster::Screen terminal(width,height);
		for (int step=0; step<7; ++step) {
			hosts[0].cards_focus=step>=1 and step<=3; hosts[0].card_detail=step==2;
			hosts[0].cards_folded=step==4; hosts[0].users_open=step==5;
			Cluster::move_card(hosts[0],step==3 ? 7 : 0);
			const	auto frame=Cluster::render(hosts,width,height,0,true,{});
			const	auto delta=frames.update(frame,width,height);
			if (step)	EXPECT_EQ(delta.find("\x1b[2J"),std::string::npos);
			terminal.apply(delta); Cluster::Screen expected(width,height); expected.apply(frame);
			EXPECT_EQ(terminal.cells(),expected.cells())<<width<<"x"<<height<<" step "<<step;
		}
	}
}

TEST(cluster_render, pending_process_details_keep_basic_fields_visible) {
	Theme::setTheme(Theme::Options{});
	auto hosts=Cluster::demo_hosts();
	auto& host=hosts[0];
	Cluster::move_process(host,0); host.proc_detail=true;
	host.groups["processes"]["status"]="warming_up";
	host.groups["processes"]["data"].erase("selected");
	const auto frame=Cluster::render(hosts,160,48,0,true,{});
	EXPECT_NE(frame.find("[loading]"),std::string::npos);
	EXPECT_NE(frame.find("PPID"),std::string::npos);
	EXPECT_EQ(frame.find("Process details unavailable"),std::string::npos);
}
