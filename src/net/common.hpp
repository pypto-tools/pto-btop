// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protocol/protocol.hpp"
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace Net {
using Protocol::Json;
constexpr std::size_t max_body=8*1024*1024;
struct HttpError : std::runtime_error {
	int status;
	HttpError(int code,const std::string& message) : std::runtime_error(message),status(code) {}
};
Json parse_json(std::string_view input,std::size_t limit=max_body);
Json config(const std::filesystem::path& path);
std::string file_path(const Json&,const std::string& key);
std::string secret(const std::string& path);
void keys(const Json&,std::initializer_list<const char*> allowed);
std::string identifier(const Json&,const char* key);
std::string random_id();
std::string digest(std::string_view value);
bool equal(std::string_view a,std::string_view b);
class Client {
	struct Impl;
	std::unique_ptr<Impl> impl_;
public:
	explicit Client(const Json& config);
	~Client();
	Json request(const std::string& method,const std::string& path,const Json& body=Json::object());
	void stop();
};
}
