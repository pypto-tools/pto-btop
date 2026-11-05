// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Cluster {
// Resolve btop's drawing commands in memory before exposing a frame to the terminal.
struct CellStyle {
	std::string foreground = "39", background = "49";
	unsigned effects = 0;
	bool operator==(const CellStyle&) const = default;
	std::string ansi() const;
};
struct Cell {
	std::string text = " ";
	CellStyle style;
	int width = 1; // Zero denotes the trailing cell of a wide glyph.
	bool operator==(const Cell&) const = default;
};
class Screen {
	int width_, height_, x_ = 0, y_ = 0;
	CellStyle style_;
	std::vector<Cell> cells_;
	void erase_glyph(int x, int y);
	void put(std::string_view glyph, int width);
public:
	Screen(int width, int height);
	void apply(std::string_view drawing);
	const std::vector<Cell>& cells() const { return cells_; }
};
class FrameDiff {
	int width_ = 0, height_ = 0;
	std::vector<Cell> previous_;
public:
	// Call only when the preceding output has finished; queued frames may be dropped.
	std::string update(std::string_view drawing, int width, int height);
	void reset() { previous_.clear(); width_ = height_ = 0; }
};
}
