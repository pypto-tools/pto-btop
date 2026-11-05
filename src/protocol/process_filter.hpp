// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protocol.hpp"
#include <cctype>
#include <charconv>
#include <optional>

namespace Protocol {

// Parse once per subscription; filter before sorting and limiting the result.
class ProcessFilter {

	bool user_only_, uid_only_;
	std::optional<std::uint32_t> uid_;
	std::string query_;
	static std::string lowercase(std::string text) {
		for (auto& c : text) c = std::tolower(static_cast<unsigned char>(c));
		return text;
	}

public:
	explicit ProcessFilter(std::string_view query)
		: user_only_(query.starts_with("user:")), uid_only_(query.starts_with("uid:")),
		  query_(user_only_ ? query.substr(5) : uid_only_ ? query.substr(4) : query) {
		if (uid_only_) {
			std::uint32_t value=0;
			const auto [end,error]=std::from_chars(query_.data(),query_.data()+query_.size(),value);
			if (error==std::errc{} and end==query_.data()+query_.size()) uid_=value;
		}
		query_ = lowercase(query_);
	}

	bool matches(const Json& process) const {
		if (uid_only_) { const auto value=process.find("uid"); return uid_.has_value() and value!=process.end() and value->is_number_integer() and *value==*uid_; }
		if (query_.empty()) return not user_only_;
		const auto value = process.find(user_only_ ? "user" : "name");
		if (value == process.end() or not value->is_string()) return false;
		const auto& text = value->get_ref<const std::string&>();
		return lowercase(text).find(query_) != std::string::npos;
	}
};

}
