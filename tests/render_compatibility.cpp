// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "btop_config.hpp"
#include "btop_draw.hpp"
#include "btop_theme.hpp"

TEST(render_compatibility, legacy_and_explicit_options_produce_identical_output) {
	for (bool tty : {false, true}) {
		Config::bools.at("tty_mode") = tty;
		Config::strings.at("color_theme") = "Default";
		Theme::setTheme();
		const auto colors = Theme::colors;
		const auto gradients = Theme::gradients;
		Theme::setTheme(Theme::Options{"Default", Config::getB("lowcolor"), Config::getB("theme_background"), tty});
		EXPECT_EQ(colors, Theme::colors);
		EXPECT_EQ(gradients, Theme::gradients);
		const Draw::Style style{tty, Config::getB("rounded_corners"), Config::getS("graph_symbol")};
		const std::deque<long long> data{0, 10, 50, 100, 5, 99, 32, 68};
		Draw::Graph old_graph(12, 3, "cpu", data);
		Draw::Graph new_graph(12, 3, "cpu", data, "default", false, false, 0, 0, style);
		EXPECT_EQ(old_graph(), new_graph());
		EXPECT_EQ(Draw::createBox(1, 2, 40, 10, "", true, "title", "footer", 1),
			Draw::createBox(1, 2, 40, 10, "", true, "title", "footer", 1, style));
	}
	Config::bools.at("tty_mode") = false;
}
