// SPDX-License-Identifier: Apache-2.0

#include "terminal.hpp"

#include <fcntl.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

#include "btop_tools.hpp"

namespace Cluster {

Terminal::Terminal() {
	if (not isatty(STDIN_FILENO) or not isatty(STDOUT_FILENO))
		throw std::runtime_error("Interactive mode requires a terminal; use --demo --once for a frame");
	if (tcgetattr(STDIN_FILENO, &original_) != 0) throw std::runtime_error("Cannot read terminal settings");
	original_flags_ = fcntl(STDOUT_FILENO,F_GETFL);
	if (original_flags_<0) throw std::runtime_error("Cannot read terminal flags");
	try { enter(); } catch (...) { leave(); throw; }
}

void Terminal::enter() {
	auto settings = original_;
	settings.c_lflag &= ~(ECHO | ICANON);
	settings.c_cc[VMIN] = 0;
	settings.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) != 0) throw std::runtime_error("Cannot configure terminal");
	if (fcntl(STDOUT_FILENO,F_SETFL,original_flags_|O_NONBLOCK)<0) {
		tcsetattr(STDIN_FILENO,TCSANOW,&original_);
		throw std::runtime_error("Cannot configure terminal output");
	}
	active_ = true;
	frames_.reset();
	pending_ = Term::alt_screen+Term::hide_cursor;
	offset_ = 0;
	flush();
}

void Terminal::leave() noexcept {
	if (not active_) return;
	tcsetattr(STDIN_FILENO, TCSANOW, &original_);
	// A full PTY output queue must never prevent signals from restoring termios.
	tcflush(STDOUT_FILENO,TCOFLUSH);
	const auto restore=Term::sync_end+Fx::reset+Term::normal_screen+Term::show_cursor;
	const auto ignored=write(STDOUT_FILENO,restore.data(),restore.size());
	(void)ignored;
	fcntl(STDOUT_FILENO,F_SETFL,original_flags_);
	pending_.clear(); next_.clear(); offset_=0;
	frames_.reset();
	active_ = false;
}

void Terminal::present(std::string frame, int width, int height) {
	next_=std::move(frame);
	next_width_=width; next_height_=height;
	flush();
}

void Terminal::flush() {
	std::size_t budget=65536;
	while (budget) {
		if (offset_==pending_.size()) {
			pending_.clear(); offset_=0;
			if (next_.empty()) return;
			pending_=frames_.update(next_,next_width_,next_height_);
			next_.clear();
		}
		if (pending_.empty()) return;
		const auto count=write(STDOUT_FILENO,pending_.data()+offset_,std::min(budget,pending_.size()-offset_));
		if (count>0) { offset_+=count; budget-=count; }
		else if (count<0 and errno==EINTR) continue;
		else if (count<0 and (errno==EAGAIN or errno==EWOULDBLOCK)) return;
		else throw std::runtime_error("Terminal output closed");
	}
}

Terminal::~Terminal() { leave(); }

std::pair<int, int> Terminal::size() const {
	winsize size{};
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) throw std::runtime_error("Cannot read terminal size");
	return {size.ws_col, size.ws_row};
}

}
