// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <termios.h>
#include <utility>
#include <string>
#include "frame.hpp"

namespace Cluster {

class Terminal {
	termios original_{};
	bool active_ = false;
	int original_flags_ = 0;
	std::string pending_, next_;
	std::size_t offset_ = 0;
	int next_width_ = 0, next_height_ = 0;
	FrameDiff frames_;
public:
	Terminal();
	~Terminal();
	Terminal(const Terminal&) = delete;
	Terminal& operator=(const Terminal&) = delete;
	void enter();
	void leave() noexcept;
	void present(std::string frame, int width, int height);
	void flush();
	std::pair<int, int> size() const;
};

}
