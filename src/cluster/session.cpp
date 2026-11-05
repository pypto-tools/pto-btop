// SPDX-License-Identifier: Apache-2.0
#include "session.hpp"
#include <algorithm>
#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace Cluster {
using Protocol::Json;

void Session::start() {
	int in[2]{-1,-1}, out[2]{-1,-1}, err[2]{-1,-1};
	if (pipe2(in,O_CLOEXEC) or pipe2(out,O_CLOEXEC) or pipe2(err,O_CLOEXEC)) {
		for (int fd : {in[0],in[1],out[0],out[1],err[0],err[1]}) if (fd >= 0) close(fd);
		throw std::runtime_error("Cannot create agent pipes");
	}
	const auto parent = getpid();
	child_ = fork();
	if (child_ == 0) {
		prctl(PR_SET_PDEATHSIG,SIGTERM);
		if (getppid() != parent) _exit(1);
		dup2(in[0],STDIN_FILENO); dup2(out[1],STDOUT_FILENO); dup2(err[1],STDERR_FILENO);
		for (int fd : {in[0],in[1],out[0],out[1],err[0],err[1]}) close(fd);
		std::vector<char*> argv;
		for (auto& arg : command_) argv.push_back(arg.data());
		argv.push_back(nullptr);
		execvp(argv[0], argv.data());
		const char failure[] = "Cannot execute agent or ssh\n";
		write(STDERR_FILENO,failure,sizeof(failure)-1);
		_exit(127);
	}
	close(in[0]); close(out[1]); close(err[1]);
	input_ = in[1]; output_ = out[0]; errors_ = err[0];
	if (child_ < 0) { close_child(); throw std::runtime_error("Cannot fork agent"); }
	for (int fd : {input_,output_,errors_}) Protocol::nonblocking(fd);
	lines_ = Protocol::Lines{};
	session_.clear(); stderr_.clear(); outgoing_.clear(); sent_.clear();
	hello_ = acknowledged_ = false;
	request_ = seq_ = 0; offset_ = 0;
	started_ = Protocol::now_ms(); stable_since_ = 0;
}

void Session::close_child() {
	// EOF lets the agent shut down its own worker; no remote PID is signalled.
	if (input_ >= 0) close(input_);
	input_ = -1;
	if (child_ > 0) {
		for (int attempt=0; attempt<10; ++attempt) {
			if (waitpid(child_,nullptr,WNOHANG) == child_) { child_=-1; break; }
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (child_ > 0) {
			kill(child_,SIGKILL);
			while (waitpid(child_,nullptr,0) < 0 and errno == EINTR) {}
			child_=-1;
		}
	}
	for (int fd : {output_,errors_}) if (fd >= 0) close(fd);
	output_=errors_=-1;
}
Session::~Session() { close_child(); }

void Session::fail(Host& host, std::string message, bool fatal) {
	const bool auth = stderr_.find("Permission denied") != std::string::npos or stderr_.find("Host key verification failed") != std::string::npos;
	fatal_ = fatal or auth;
	host.status = auth ? "AUTH_REQUIRED" : fatal ? "PROTOCOL_ERROR" : "DISCONNECTED";
	host.error = safe_text(message + (stderr_.empty() ? "" : ": " + stderr_), 160);
	clear_histories(host);
	close_child();
	const auto delay = std::min(30, 1 << std::min(5,retries_++));
	next_retry_ = Protocol::now_ms() + delay * (800 + Protocol::now_ms()%401);
}

void Session::reconnect(Host& host) {
	close_child(); fatal_=false; retries_=0; next_retry_=0;
	host.status="CONNECTING"; host.error.clear();
	clear_histories(host);
}

bool Session::tick(Host& host, Protocol::Subscription options) {
	const auto now = Protocol::now_ms();
	bool changed = false;
	try {
		if (child_ < 0) {
			if (fatal_ or now < next_retry_) return false;
			start(); host.status="CONNECTING"; host.error.clear(); changed=true;
		}
		options.request_id=1;
		desired_ = Protocol::encode(options).dump(-1,' ',false,Json::error_handler_t::replace);
		char bytes[8192];
		for (int i=0; i<8; ++i) {
			const auto n=read(errors_,bytes,sizeof(bytes));
			if (n<=0) break;
			stderr_.append(bytes,n);
			if (stderr_.size()>65536) stderr_.erase(0,stderr_.size()-65536);
		}
		bool eof=false;
		for (int i=0; i<16; ++i) {
			const auto n=read(output_,bytes,sizeof(bytes));
			if (n==0) { eof=true; break; }
			if (n<0) { if (errno!=EAGAIN and errno!=EWOULDBLOCK and errno!=EINTR) eof=true; break; }
			for (const auto& line : lines_.feed({bytes,static_cast<std::size_t>(n)})) {
				const auto message=Protocol::parse(line);
				const auto type=message.at("type").get<std::string>();
				if (type=="hello") {
					if (hello_) throw std::runtime_error("Duplicate hello");
					session_=message.at("session_id").get<std::string>();
					if (session_.empty()) throw std::runtime_error("Missing session id");
					hello_=true;
				} else {
					if (not hello_ or message.at("session_id")!=session_) throw std::runtime_error("Wrong session id");
					if (type=="subscribed") {
						if (message.at("request_id")==request_) acknowledged_=true;
					} else if (type=="snapshot") {
						if (not message.at("seq").is_number_unsigned()) throw std::runtime_error("Invalid sequence");
						const auto seq=message.at("seq").get<std::uint64_t>();
						if (seq<=seq_) throw std::runtime_error("Sequence did not increase");
						seq_=seq;
						if (acknowledged_ and message.at("subscription_id")==request_) {
							ingest(host,message); changed=true;
							if (not stable_since_) stable_since_=now;
							if (now-stable_since_>=30000) retries_=0;
						}
					} else if (type=="error") throw std::runtime_error(message.at("message").get<std::string>());
					else throw std::runtime_error("Unknown agent message");
				}
			}
		}
		if (eof) { fail(host,"Agent connection closed",false); return true; }
		if (hello_ and outgoing_.empty() and sent_!=desired_) {
			options.request_id=++request_;
			outgoing_=Protocol::encode(options).dump(-1,' ',false,Json::error_handler_t::replace)+'\n'; offset_=0;
			sent_=desired_; sent_at_=now; acknowledged_=false;
		}
		if (not outgoing_.empty()) {
			const auto n=write(input_,outgoing_.data()+offset_,outgoing_.size()-offset_);
			if (n>0) offset_+=n;
			else if (n<0 and errno!=EAGAIN and errno!=EWOULDBLOCK and errno!=EINTR) { fail(host,"Agent input closed",false); return true; }
			if (offset_==outgoing_.size()) outgoing_.clear();
		}
		if ((not hello_ and now-started_>5000) or (hello_ and not acknowledged_ and sent_at_ and now-sent_at_>5000)) {
			fail(host,"Agent handshake timeout",false); return true;
		}
		const auto fresh=std::max(started_,host.received_ms);
		// ingest() timestamps receipt after this tick starts; never subtract it from the older `now`.
		const auto age=Protocol::now_ms()-fresh;
		if (age>std::uint64_t(std::max(10000,options.interval_ms*5))) { fail(host,"No recent samples",false); return true; }
		if (age>std::uint64_t(std::max(6000,options.interval_ms*3)) and host.status!="STALE") {
			host.status="STALE"; clear_histories(host); changed=true;
		}
	} catch (const std::exception& e) { fail(host,e.what(),true); return true; }
	return changed;
}
}
