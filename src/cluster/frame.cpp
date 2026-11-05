// SPDX-License-Identifier: Apache-2.0
#include "frame.hpp"
#include "btop_tools.hpp"
#include <algorithm>
#include <charconv>
#include <cwchar>
#include <stdexcept>

namespace Cluster {
namespace {
std::vector<int> parameters(std::string_view input) {
	std::vector<int> values;
	while (true) {
		const auto end = input.find(';');
		const auto token = input.substr(0,end);
		int value = 0;
		if (not token.empty()) {
			const auto [last,error] = std::from_chars(token.data(),token.data()+token.size(),value);
			if (error != std::errc{} or last != token.data()+token.size()) throw std::runtime_error("Invalid drawing parameter");
		}
		values.push_back(value);
		if (end == std::string_view::npos) break;
		input.remove_prefix(end+1);
	}
	return values;
}
void attributes(CellStyle& style,const std::vector<int>& values) {
	for (std::size_t i=0; i<values.size(); ++i) {
		const int value=values[i];
		if (value==0) style={};
		else if (value==1) style.effects|=1;
		else if (value==2) style.effects|=2;
		else if (value==3) style.effects|=4;
		else if (value==4) style.effects|=8;
		else if (value==5) style.effects|=16;
		else if (value==7) style.effects|=32;
		else if (value==9) style.effects|=64;
		else if (value==22) style.effects&=~3u;
		else if (value==23) style.effects&=~4u;
		else if (value==24) style.effects&=~8u;
		else if (value==25) style.effects&=~16u;
		else if (value==27) style.effects&=~32u;
		else if (value==29) style.effects&=~64u;
		else if (value==38 or value==48) {
			if (i+1>=values.size()) throw std::runtime_error("Incomplete drawing color");
			const int count=values[i+1]==2 ? 4 : values[i+1]==5 ? 2 : 0;
			if (not count or i+count>=values.size()) throw std::runtime_error("Invalid drawing color");
			std::string color=std::to_string(value);
			for (int j=0; j<count; ++j) color+=';'+std::to_string(values[++i]);
			(value==38 ? style.foreground : style.background)=std::move(color);
		}
		else if ((value>=30 and value<=37) or (value>=90 and value<=97) or value==39) style.foreground=std::to_string(value);
		else if ((value>=40 and value<=47) or (value>=100 and value<=107) or value==49) style.background=std::to_string(value);
		else throw std::runtime_error("Unsupported drawing attribute");
	}
}
}

std::string CellStyle::ansi() const {
	std::string out="\x1b[0";
	const int codes[]={1,2,3,4,5,7,9};
	for (int i=0; i<7; ++i) if (effects&(1u<<i)) out+=';'+std::to_string(codes[i]);
	return out+';'+foreground+';'+background+'m';
}

Screen::Screen(int width,int height) : width_(width),height_(height) {
	if (width<1 or height<1 or static_cast<long long>(width)*height>1000000) throw std::runtime_error("Invalid screen dimensions");
	cells_.resize(static_cast<std::size_t>(width)*height);
}
void Screen::erase_glyph(int x,int y) {
	auto& cell=cells_[y*width_+x];
	if (cell.width==0 and x>0) cells_[y*width_+x-1]={" ",style_,1};
	if (cell.width==2 and x+1<width_) cells_[y*width_+x+1]={" ",style_,1};
	cell={" ",style_,1};
}
void Screen::put(std::string_view glyph,int width) {
	if (width==0) {
		if (y_>=0 and y_<height_ and x_>0 and x_<=width_) {
			auto column=x_-1;
			if (cells_[y_*width_+column].width==0 and column>0) --column;
			cells_[y_*width_+column].text+=glyph;
		}
		return;
	}
	if (x_<0 or y_<0 or y_>=height_ or x_+width>width_) throw std::runtime_error("Drawing outside terminal bounds");
	erase_glyph(x_,y_);
	if (width==2) erase_glyph(x_+1,y_);
	cells_[y_*width_+x_]={std::string(glyph),style_,width};
	if (width==2) cells_[y_*width_+x_+1]={"",style_,0};
	x_+=width;
}
void Screen::apply(std::string_view drawing) {
	std::mbstate_t state{};
	for (std::size_t i=0; i<drawing.size();) {
		if (drawing[i]=='\x1b') {
			if (i+1>=drawing.size() or drawing[i+1]!='[') throw std::runtime_error("Unsupported drawing escape");
			std::size_t end=i+2;
			while (end<drawing.size() and (drawing[end]<'@' or drawing[end]>'~')) ++end;
			if (end==drawing.size()) throw std::runtime_error("Incomplete drawing escape");
			const auto raw=drawing.substr(i+2,end-i-2);
			const char command=drawing[end]; i=end+1;
			if (raw.starts_with('?')) continue; // Cursor visibility / synchronized output, no cell changes.
			const auto values=parameters(raw);
			const int step=std::max(1,values.front());
			if (command=='m') attributes(style_,values);
			else if (command=='H' or command=='f') { y_=step-1; x_=values.size()>1 ? std::max(1,values[1])-1 : 0; }
			else if (command=='C') x_+=step;
			else if (command=='D') x_-=step;
			else if (command=='A') y_-=step;
			else if (command=='B') y_+=step;
			else if (command=='J' and values[0]==2) std::fill(cells_.begin(),cells_.end(),Cell{" ",style_,1});
			else throw std::runtime_error("Unsupported drawing command");
			continue;
		}
		if (drawing[i]=='\n') { ++y_; ++i; continue; }
		if (drawing[i]=='\r') { x_=0; ++i; continue; }
		wchar_t character{};
		auto count=std::mbrtowc(&character,drawing.data()+i,drawing.size()-i,&state);
		if (count==std::size_t(-1) or count==std::size_t(-2) or count==0) { put("?",1); ++i; state={}; continue; }
		const int width=wcwidth(character);
		if (width<0) put("?",1);
		else put(drawing.substr(i,count),width);
		i+=count;
	}
}

std::string FrameDiff::update(std::string_view drawing,int width,int height) {
	Screen screen(width,height);
	screen.apply(drawing);
	const auto& next=screen.cells();
	const bool full=previous_.empty() or width!=width_ or height!=height_;
	std::string out=full ? "\x1b[0m\x1b[2J" : "";
	CellStyle emitted;
	bool have_style=false;
	for (int y=0; y<height; ++y) {
		std::vector<bool> dirty(width,full);
		if (full) for (int x=0; x<width; ++x) dirty[x]=next[y*width+x]!=Cell{};
		if (not full) for (int x=0; x<width; ++x) {
			const auto index=y*width+x;
			if (next[index]!=previous_[index]) {
				dirty[x]=true;
				// A write to either half of a wide glyph must repaint the entire glyph.
				if (x>0 and (next[index].width==0 or previous_[index].width==0)) dirty[x-1]=true;
				if (x+1<width and (next[index].width==2 or previous_[index].width==2)) dirty[x+1]=true;
			}
		}
		int cursor=-1;
		for (int x=0; x<width; ++x) {
			const auto& cell=next[y*width+x];
			if (not dirty[x] or cell.width==0) continue;
			if (cursor!=x) out+="\x1b["+std::to_string(y+1)+';'+std::to_string(x+1)+'H';
			if (not have_style or cell.style!=emitted) { out+=cell.style.ansi(); emitted=cell.style; have_style=true; }
			out+=cell.text; cursor=x+cell.width;
		}
	}
	previous_=next; width_=width; height_=height;
	return out.empty() ? out : Term::sync_start+out+"\x1b[0m"+Term::sync_end;
}
}
