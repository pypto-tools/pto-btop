// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protocol.hpp"
#include <cctype>

namespace Protocol {

// Parse once per subscription; filter before sorting and limiting the result.
class ProcessFilter {

	bool user_only_;
	std::string query_;
	static std::string lowercase(std::string text) {
		for (auto& c : text) c = std::tolower(static_cast<unsigned char>(c));
		return text;
	}

public:
	explicit ProcessFilter(std::string_view query)
		: user_only_(query.starts_with("user:")),
		  query_(user_only_ ? query.substr(5) : query) {
		query_ = lowercase(query_);
	}

	bool matches(const Json& process) const {
		if (query_.empty()) return not user_only_;
		const auto value = process.find(user_only_ ? "user" : "name");
		if (value == process.end() or not value->is_string()) return false;
		const auto& text = value->get_ref<const std::string&>();
		return lowercase(text).find(query_) != std::string::npos;
	}
};

}
