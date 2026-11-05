// SPDX-License-Identifier: Apache-2.0
#include "common.hpp"
#include <httplib/httplib.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <regex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace Net {
Json parse_json(std::string_view input,std::size_t limit) {
	if (input.size()>limit) throw std::runtime_error("JSON body exceeds limit");
	std::vector<std::set<std::string>> names;
	auto value=Json::parse(input,[&](int depth,Json::parse_event_t event,Json& item) {
		if (depth>24) throw std::runtime_error("JSON nesting exceeds limit");
		if (item.is_string() and item.get_ref<const std::string&>().size()>8192) throw std::runtime_error("JSON string exceeds limit");
		if (event==Json::parse_event_t::object_start) names.emplace_back();
		else if (event==Json::parse_event_t::object_end) names.pop_back();
		else if (event==Json::parse_event_t::key and not names.back().insert(item.get<std::string>()).second) throw std::runtime_error("Duplicate JSON key");
		return true;
	});
	if (not value.is_object()) throw std::runtime_error("Expected JSON object");
	return value;
}
void keys(const Json& object,std::initializer_list<const char*> allowed) {
	if (not object.is_object()) throw std::runtime_error("Expected configuration object");
	for (const auto& [key,value] : object.items())
		if (key!="_directory" and std::find(allowed.begin(),allowed.end(),key)==allowed.end()) throw std::runtime_error("Unknown configuration field: "+key);
}
Json config(const std::filesystem::path& path) {
	if (not std::filesystem::is_regular_file(path)) throw std::runtime_error("Configuration must be a regular file");
	std::ifstream input(path); std::string bytes(65537,'\0');
	input.read(bytes.data(),bytes.size()); bytes.resize(input.gcount());
	auto value=parse_json(bytes,65536);
	if (not value.contains("version") or not value["version"].is_number_integer() or value["version"]!=1) throw std::runtime_error("Configuration version must be 1");
	if (value.contains("_directory")) throw std::runtime_error("Reserved configuration field");
	value["_directory"]=std::filesystem::absolute(path).parent_path().string();
	return value;
}
std::string file_path(const Json& settings,const std::string& key) {
	auto path=std::filesystem::path(settings.at(key).get<std::string>());
	if (path.empty()) throw std::runtime_error("Empty path: "+key);
	return path.is_absolute() ? path.string() : (std::filesystem::path(settings.value("_directory","."))/path).string();
}
std::string secret(const std::string& path) {
	const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
	if (fd<0) throw std::runtime_error("Cannot open token file");
	struct stat st{}; char bytes[513];
	if (fstat(fd,&st) or not S_ISREG(st.st_mode) or (st.st_mode&0077) or st.st_size>512) { close(fd); throw std::runtime_error("Token file must be regular, at most 512 bytes, and private (mode 0600)"); }
	const auto count=read(fd,bytes,sizeof(bytes)); close(fd);
	if (count<0) throw std::runtime_error("Cannot read token file");
	std::string value(bytes,count);
	while (not value.empty() and (value.back()=='\n' or value.back()=='\r')) value.pop_back();
	if (value.size()<32 or value.size()>256 or not std::ranges::all_of(value,[](unsigned char c) { return (c>='a' and c<='z') or (c>='A' and c<='Z') or (c>='0' and c<='9') or c=='-' or c=='_'; })) throw std::runtime_error("Token must contain 32..256 URL-safe characters");
	return value;
}
std::string identifier(const Json& object,const char* key) {
	const auto value=object.at(key).get<std::string>();
	if (not std::regex_match(value,std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,63}"))) throw std::runtime_error("Invalid identifier");
	return value;
}
std::string random_id() {
	unsigned char bytes[24]; if (RAND_bytes(bytes,sizeof(bytes))!=1) throw std::runtime_error("Random source unavailable");
	const char* hex="0123456789abcdef"; std::string out;
	for (auto byte : bytes) { out+=hex[byte>>4]; out+=hex[byte&15]; }
	return out;
}
std::string digest(std::string_view value) {
	unsigned char bytes[SHA256_DIGEST_LENGTH]; SHA256(reinterpret_cast<const unsigned char*>(value.data()),value.size(),bytes);
	return std::string(reinterpret_cast<char*>(bytes),sizeof(bytes));
}
bool equal(std::string_view a,std::string_view b) { return a.size()==b.size() and CRYPTO_memcmp(a.data(),b.data(),a.size())==0; }
struct Client::Impl {
	httplib::Client http;
	Impl(const std::string& url) : http(url) {}
};
Client::Client(const Json& settings) {
	const auto url=settings.at("server").get<std::string>();
	if (not std::regex_match(url,std::regex("https://([A-Za-z0-9.-]+|\\[[A-Fa-f0-9:]+\\])(:[0-9]{1,5})?"))) throw std::runtime_error("Server must be an HTTPS origin without a path or credentials");
	impl_=std::make_unique<Impl>(url);
	auto& http=impl_->http;
	http.set_ca_cert_path(file_path(settings,"ca_file"));
	http.enable_server_certificate_verification(true);
	http.enable_server_hostname_verification(true);
	http.set_follow_location(false);
	http.set_connection_timeout(1); http.set_read_timeout(2); http.set_write_timeout(2); http.set_max_timeout(3000);
	http.set_payload_max_length(max_body);
	http.set_default_headers({{"Authorization","Bearer "+secret(file_path(settings,"token_file"))},{"Accept","application/json"}});
}
Client::~Client()=default;
Json Client::request(const std::string& method,const std::string& path,const Json& body) {
	auto& http=impl_->http;
	const auto encoded=body.dump(-1,' ',false,Json::error_handler_t::replace);
	if (encoded.size()>max_body) throw std::runtime_error("Outgoing HTTPS body exceeds limit");
	auto result=method=="GET" ? http.Get(path) : method=="DELETE" ? http.Delete(path) : http.Post(path,encoded,"application/json");
	if (not result) throw std::runtime_error("HTTPS transport: "+httplib::to_string(result.error()));
	if (result->status<200 or result->status>=300) throw HttpError(result->status,"HTTPS status "+std::to_string(result->status));
	return parse_json(result->body);
}
void Client::stop() { impl_->http.stop(); }
}
