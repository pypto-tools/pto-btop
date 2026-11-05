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
#include "btop_tools.hpp"

const string Fx::e = "\x1b[";		//* Escape sequence start
const string Fx::b = e + "1m";		//* Bold on/off
const string Fx::ub = e + "22m";	//* Bold off
const string Fx::d = e + "2m";		//* Dark on
const string Fx::ud = e + "22m";	//* Dark off
const string Fx::i = e + "3m";		//* Italic on
const string Fx::ui = e + "23m";	//* Italic off
const string Fx::ul = e + "4m";		//* Underline on
const string Fx::uul = e + "24m";	//* Underline off
const string Fx::bl = e + "5m";		//* Blink on
const string Fx::ubl = e + "25m";	//* Blink off
const string Fx::s = e + "9m";		//* Strike/crossed-out on
const string Fx::us = e + "29m";	//* Strike/crossed-out on/off

//* Reset foreground/background color and text effects
const string Fx::reset_base = e + "0m";

//* Regex for matching color, style and cursor move escape sequences
const std::regex Fx::escape_regex("\033\\[\\d+;?\\d?;?\\d*;?\\d*;?\\d*(m|f|s|u|C|D|A|B){1}");

//* Regex for matching only color and style escape sequences
const std::regex Fx::color_regex("\033\\[\\d+;?\\d?;?\\d*;?\\d*;?\\d*(m){1}");

const string Term::hide_cursor = Fx::e + "?25l";
const string Term::show_cursor = Fx::e + "?25h";
const string Term::alt_screen = Fx::e + "?1049h";
const string Term::normal_screen = Fx::e + "?1049l";
const string Term::clear = Fx::e + "2J" + Fx::e + "0;0f";
const string Term::clear_end = Fx::e + "0J";
const string Term::clear_begin = Fx::e + "1J";
const string Term::mouse_on = Fx::e + "?1002h" + Fx::e + "?1015h" + Fx::e + "?1006h"; //? Enable reporting of mouse position on click and release
const string Term::mouse_off = Fx::e + "?1002l" + Fx::e + "?1015l" + Fx::e + "?1006l";
const string Term::mouse_direct_on = Fx::e + "?1003h"; //? Enable reporting of mouse position at any movement
const string Term::mouse_direct_off = Fx::e + "?1003l";
const string Term::sync_start = Fx::e + "?2026h"; //? Start of terminal synchronized output
const string Term::sync_end = Fx::e + "?2026l"; //? End of terminal synchronized output

namespace Tools {
	string_view ltrim(string_view str, const string_view t_str) {
		while (str.starts_with(t_str))
			str.remove_prefix(t_str.size());

		return str;
	}

	string_view rtrim(string_view str, const string_view t_str) {
		while (str.ends_with(t_str))
			str.remove_suffix(t_str.size());

		return str;
	}

	std::string operator*(const string& str, int64_t n) {
		if (n < 1 or str.empty()) {
			return "";
		}
		else if (n == 1) {
			return str;
		}

		string new_str;
		new_str.reserve(str.size() * n);

		for (; n > 0; n--)
			new_str.append(str);

		return new_str;
	}
}
