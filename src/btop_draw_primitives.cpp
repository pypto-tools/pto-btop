/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

// Modified for btop-cluster: separate reusable rendering from single-host UI state.
#include <algorithm>
#include <cmath>
#include <ranges>
#include <stdexcept>
#include <utility>
#include "btop_draw.hpp"
#include "btop_theme.hpp"
#include "btop_tools.hpp"
using std::clamp;
using std::max;
using std::round;
using std::views::iota;
using namespace Tools;
using namespace std::literals;

const string Symbols::h_line			= "─";
const string Symbols::v_line			= "│";
const string Symbols::dotted_v_line		= "╎";
const string Symbols::left_up			= "┌";
const string Symbols::right_up			= "┐";
const string Symbols::left_down			= "└";
const string Symbols::right_down		= "┘";
const string Symbols::round_left_up		= "╭";
const string Symbols::round_right_up	= "╮";
const string Symbols::round_left_down	= "╰";
const string Symbols::round_right_down	= "╯";
const string Symbols::title_left_down	= "┘";
const string Symbols::title_right_down	= "└";
const string Symbols::title_left		= "┐";
const string Symbols::title_right		= "┌";
const string Symbols::div_right			= "┤";
const string Symbols::div_left			= "├";
const string Symbols::div_up			= "┬";
const string Symbols::div_down			= "┴";


const string Symbols::up = "↑";
const string Symbols::down = "↓";
const string Symbols::left = "←";
const string Symbols::right = "→";
const string Symbols::enter = "↵";

namespace Symbols {
	const string meter = "■";

	const array<string, 10> superscript = { "⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹" };

	const std::unordered_map<string, vector<string>> graph_symbols = {
		{ "braille_up", {
			" ", "⢀", "⢠", "⢰", "⢸",
			"⡀", "⣀", "⣠", "⣰", "⣸",
			"⡄", "⣄", "⣤", "⣴", "⣼",
			"⡆", "⣆", "⣦", "⣶", "⣾",
			"⡇", "⣇", "⣧", "⣷", "⣿"
		}},
		{"braille_down", {
			" ", "⠈", "⠘", "⠸", "⢸",
			"⠁", "⠉", "⠙", "⠹", "⢹",
			"⠃", "⠋", "⠛", "⠻", "⢻",
			"⠇", "⠏", "⠟", "⠿", "⢿",
			"⡇", "⡏", "⡟", "⡿", "⣿"
		}},
		{"block_up", {
			" ", "▗", "▗", "▐", "▐",
			"▖", "▄", "▄", "▟", "▟",
			"▖", "▄", "▄", "▟", "▟",
			"▌", "▙", "▙", "█", "█",
			"▌", "▙", "▙", "█", "█"
		}},
		{"block_down", {
			" ", "▝", "▝", "▐", "▐",
			"▘", "▀", "▀", "▜", "▜",
			"▘", "▀", "▀", "▜", "▜",
			"▌", "▛", "▛", "█", "█",
			"▌", "▛", "▛", "█", "█"
		}},
		{"tty_up", {
			" ", "░", "░", "▒", "▒",
			"░", "░", "▒", "▒", "█",
			"░", "▒", "▒", "▒", "█",
			"▒", "▒", "▒", "█", "█",
			"▒", "█", "█", "█", "█"
		}},
		{"tty_down", {
			" ", "░", "░", "▒", "▒",
			"░", "░", "▒", "▒", "█",
			"░", "▒", "▒", "▒", "█",
			"▒", "▒", "▒", "█", "█",
			"▒", "█", "█", "█", "█"
		}}
	};

}

namespace Draw {
	string createBox(
			const int x, const int y, const int width, const int height, string line_color, bool fill, const std::string_view title,
			const std::string_view title2, const int num, const Style& style
	) {
		string out;

		if (line_color.empty())
			line_color = Theme::c("div_line");

		auto tty_mode = style.tty_mode;
		auto rounded = style.rounded_corners;
		const string numbering = (num == 0) ? "" : Theme::c("hi_fg") + (tty_mode ? std::to_string(num) : Symbols::superscript.at(clamp(num, 0, 9)));
		const auto& right_up = (tty_mode or not rounded ? Symbols::right_up : Symbols::round_right_up);
		const auto& left_up = (tty_mode or not rounded ? Symbols::left_up : Symbols::round_left_up);
		const auto& right_down = (tty_mode or not rounded ? Symbols::right_down : Symbols::round_right_down);
		const auto& left_down = (tty_mode or not rounded ? Symbols::left_down : Symbols::round_left_down);

		out = Fx::reset + line_color;

		//? Draw horizontal lines
		for (const int& hpos : {y, y + height - 1}) {
			out += Mv::to(hpos, x) + Symbols::h_line * (width - 1);
		}

		//? Draw vertical lines and fill if enabled
		for (const int& hpos : iota(y + 1, y + height - 1)) {
			out += Mv::to(hpos, x) + Symbols::v_line
				+  ((fill) ? string(width - 2, ' ') : Mv::r(width - 2))
				+  Symbols::v_line;
		}

		//? Draw corners
		out += 	Mv::to(y, x) + left_up
			+	Mv::to(y, x + width - 1) + right_up
			+	Mv::to(y + height - 1, x) +left_down
			+	Mv::to(y + height - 1, x + width - 1) + right_down;

		//? Draw titles if defined
		if (not title.empty()) {
			out += fmt::format(
				"{}{}{}{}{}{}{}{}{}", Mv::to(y, x + 2), Symbols::title_left, Fx::b, numbering, Theme::c("title"), title, Fx::ub,
				line_color, Symbols::title_right
			);
		}
		if (not title2.empty()) {
			out += fmt::format(
				"{}{}{}{}{}{}{}{}{}", Mv::to(y + height - 1, x + 2), Symbols::title_left_down, Fx::b, numbering, Theme::c("title"), title2, Fx::ub,
				line_color, Symbols::title_right_down
			);
		}

		return out + Fx::reset + Mv::to(y + 1, x + 1);
	}

	//* Meter class ------------------------------------------------------------------------------------------------------------>
	Meter::Meter() {}

	Meter::Meter(const int width, string color_gradient, bool invert)
		: width(width), color_gradient(std::move(color_gradient)), invert(invert) {}

	string Meter::operator()(int value) {
		if (width < 1) return "";
		value = clamp(value, 0, 100);
		if (not cache.at(value).empty()) return cache.at(value);
		auto& out = cache.at(value);
		for (const int& i : iota(1, width + 1)) {
			int y = round((double)i * 100.0 / width);
			if (value >= y)
				out += Theme::g(color_gradient).at(invert ? 100 - y : y) + Symbols::meter;
			else {
				out += Theme::c("meter_bg") + Symbols::meter * (width + 1 - i);
				break;
			}
		}
		out += Fx::reset;
		return out;
	}

	//* Graph class ------------------------------------------------------------------------------------------------------------>
	void Graph::_create(const deque<long long>& data, int data_offset) {
		bool mult = (data.size() - data_offset > 1);
		const auto& graph_symbol = Symbols::graph_symbols.at(symbol + '_' + (invert ? "down" : "up"));
		array<int, 2> result;
		const float mod = (height == 1) ? 0.3 : 0.1;
		long long data_value = 0;
		if (mult and data_offset > 0) {
			last = data.at(data_offset - 1);
			if (max_value > 0) last = clamp((last + offset) * 100 / max_value, 0ll, 100ll);
		}

		//? Horizontal iteration over values in <data>
		for (const int& i : iota(data_offset, (int)data.size())) {
			// if (tty_mode and mult and i % 2 != 0) continue;
			if (not tty_mode and mult) current = not current;
			if (i < 0) {
				data_value = 0;
				last = 0;
			}
			else {
				data_value = data.at(i);
				if (max_value > 0) data_value = clamp((data_value + offset) * 100 / max_value, 0ll, 100ll);
			}

			//? Vertical iteration over height of graph
			for (const int& horizon : iota(0, height)) {
				const int cur_high = (height > 1) ? round(100.0 * (height - horizon) / height) : 100;
				const int cur_low = (height > 1) ? round(100.0 * (height - (horizon + 1)) / height) : 0;
				//? Calculate previous + current value to fit two values in 1 braille character
				for (int ai = 0; const auto& value : {last, data_value}) {
					const int clamp_min = (no_zero and horizon == height - 1 and not (mult and i == data_offset and ai == 0)) ? 1 : 0;
					if (value >= cur_high)
						result[ai++] = 4;
					else if (value <= cur_low)
						result[ai++] = clamp_min;
					else {
						result[ai++] = clamp((int)round((float)(value - cur_low) * 4 / (cur_high - cur_low) + mod), clamp_min, 4);
					}
				}
				//? Generate graph symbol from 5x5 2D vector
				if (height == 1) {
					if (result.at(0) + result.at(1) == 0) graphs.at(current).at(horizon) += Mv::r(1);
					else {
						if (not color_gradient.empty()) graphs.at(current).at(horizon) += Theme::g(color_gradient).at(clamp(max(last, data_value), 0ll, 100ll));
						graphs.at(current).at(horizon) += graph_symbol.at((result.at(0) * 5 + result.at(1)));
					}
				}
				else graphs.at(current).at(horizon) += graph_symbol.at((result.at(0) * 5 + result.at(1)));
			}
			if (mult and i >= 0) last = data_value;
		}
		last = data_value;
		out.clear();
		if (height == 1) {
			//if (not color_gradient.empty())
			//	out += (last < 1 ? Theme::c("inactive_fg") : Theme::g(color_gradient).at(clamp(last, 0ll, 100ll)));
			out += graphs.at(current).at(0);
		}
		else {
			for (const int& i : iota(1, height + 1)) {
				if (i > 1) out += Mv::d(1) + Mv::l(width);
				if (not color_gradient.empty())
					out += (invert) ? Theme::g(color_gradient).at(i * 100 / height) : Theme::g(color_gradient).at(100 - ((i - 1) * 100 / height));
				out += (invert) ? graphs.at(current).at(height - i) : graphs.at(current).at(i-1);
			}
		}
		if (not color_gradient.empty()) out += Fx::reset;
	}

	Graph::Graph() {}

	Graph::Graph(int width, int height, const string& color_gradient,
				 const deque<long long>& data, const string& symbol,
				 bool invert, bool no_zero, long long max_value, long long offset, const Style& style)
	: width(width), height(height), color_gradient(color_gradient),
	  invert(invert), no_zero(no_zero), offset(offset) {
		if (width < 1 or height < 1) throw std::invalid_argument("Graph dimensions must be positive");
		if (style.tty_mode or symbol == "tty") this->symbol = "tty";
		else if (symbol != "default") this->symbol = symbol;
		else this->symbol = style.graph_symbol;
		if (this->symbol == "tty") tty_mode = true;

		if (max_value == 0 and offset > 0) max_value = 100;
		this->max_value = max_value;
		const int value_width = (tty_mode ? data.size() : ceil((double)data.size() / 2));
		int data_offset = (value_width > width) ? data.size() - width * (tty_mode ? 1 : 2) : 0;

		if (not tty_mode and (data.size() - data_offset) % 2 != 0) {
			data_offset--;
		}

		//? Populate the two switching graph vectors and fill empty space if data size < width
		for (const int& i : iota(0, height * 2)) {
			if (tty_mode and i % 2 != current) continue;
			graphs[(i % 2 != 0)].push_back((value_width < width) ? ((height == 1) ? Mv::r(1) : " "s) * (width - value_width) : "");
		}
		if (data.size() == 0) return;
		this->_create(data, data_offset);
	}

	string& Graph::operator()(const deque<long long>& data, bool data_same) {
		if (data_same) return out;

		//? Make room for new characters on graph
		if (not tty_mode) current = not current;
		for (const int& i : iota(0, height)) {
			if (height == 1 and graphs.at(current).at(i).at(1) == '[') {
				if (graphs.at(current).at(i).at(3) == 'C') graphs.at(current).at(i).erase(0, 4);
				else graphs.at(current).at(i).erase(0, graphs.at(current).at(i).find_first_of('m') + 4);
			}
			else if (graphs.at(current).at(i).at(0) == ' ') graphs.at(current).at(i).erase(0, 1);
			else graphs.at(current).at(i).erase(0, 3);
		}
		this->_create(data, (int)data.size() - 1);
		return out;
	}

	string& Graph::operator()() {
		return out;
	}
	//*------------------------------------------------------------------------------------------------------------------------->

}
