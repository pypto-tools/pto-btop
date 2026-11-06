// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "btop_draw.hpp"
#include "protocol/protocol.hpp"

namespace Cluster {

struct Rect {
	int x, y, width, height;
};

enum class Layout { cards, compact, too_small };

struct Geometry {
	Layout layout;
	std::vector<Rect> cards;
};

struct Host {
	std::string name;
	int cores = 0;
	double cpu = 0;
	std::uint64_t memory_total = 0;
	std::uint64_t memory_used = 0;
	std::array<double, 3> load{};
	std::deque<long long> history;
	bool simulated = true;
	bool cpu_valid = true, memory_valid = true;
	std::string status = "ONLINE", error;
	std::uint64_t received_ms = 0;
	std::string sample_time; // UTC clock from the latest snapshot; stable across redraws.
	Protocol::Json groups = Protocol::Json::object();
	std::map<std::string, std::deque<long long>> network_rx, network_tx;
	int core_page = 0, disk_page = 0, net_index = 0, proc_page = 0;
	std::string sort = "cpu", filter;
	std::map<std::string, std::deque<long long>> series;
	std::map<std::string, std::array<double, 2>> network_peak, network_offset;
	std::string cpu_metric = "usage_pct";
	bool disk_io = false, net_sync = true, net_auto = true, proc_tree = false, reverse = false;
	bool proc_detail = false;
	bool cards_focus = false, cards_folded = false, card_detail = false, users_open = false;
	std::string selected_card;
	int users_page = 0;
	int selected_pid = 0;
	std::uint64_t selected_start = 0;
	std::set<std::string> collapsed;
};

struct OverviewTotals {
	int online=0, cpu_hosts=0, memory_hosts=0, card_hosts=0, queue_hosts=0;
	std::uint64_t cores=0, memory_total=0, memory_available=0, free_cards=0, total_cards=0, waiting=0;
	double cpu=-1;
	bool cards_partial=false, queue_partial=false;
};
OverviewTotals overview_totals(const std::vector<Host>& hosts);

struct ProcessRow { std::size_t index; int depth = 0; bool children = false; };
std::vector<ProcessRow> process_rows(const Host& host);
void move_process(Host& host, int delta);
void toggle_process(Host& host);
void move_card(Host& host, int delta);
int card_panel_height(const Host& host, int terminal_height);
int card_visible_rows(const Host& host, int panel_height);
bool filter_card_owner(Host& host);
std::string resource_status(const Host& host, const std::string& name);
std::string card_summary(const Host& host, bool compact = false);
std::string queue_summary(const Host& host);
std::string session_summary(const Host& host, bool compact = false);
std::string users_overlay(const Host& host, int width, int height, const Draw::Style& style);
void clear_histories(Host& host);
const Protocol::Json& metric_data(const Host& host, std::string_view name);
const Protocol::Json& metric_items(const Protocol::Json& value, const char* key = "items");
std::string process_key(const Protocol::Json& value);

Geometry layout(int width, int height, int count);
std::vector<Host> demo_hosts(int interval_ms = 2000);
void advance_demo(std::vector<Host>& hosts, unsigned tick, int interval_ms = 2000);
void ingest(Host& host, const Protocol::Json& snapshot);
std::string safe_text(std::string_view text, int columns);
std::string detail_view(const Host& host, int width, int height, const Draw::Style& style);
std::string render(const std::vector<Host>& hosts, int width, int height,
	int selected, bool detail, const Draw::Style& style);

}
