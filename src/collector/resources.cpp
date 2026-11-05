// SPDX-License-Identifier: Apache-2.0
#include "resources.hpp"
#include "protocol/resources.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utmpx.h>

namespace Collector {
using Protocol::Json;
namespace {
constexpr std::size_t output_limit=256*1024;
Json failure(const std::string& reason, const std::string& status="unavailable") {
	return Protocol::group(nullptr,Protocol::now_ms(),5000,status,reason);
}
Json read_cards(const std::filesystem::path& command) {
	if (command.empty() or access(command.c_str(),X_OK)!=0) return failure("task-submit monitor command unavailable");
	int pipefd[2]; if (pipe2(pipefd,O_CLOEXEC|O_NONBLOCK)!=0) return failure("Cannot create monitor pipe");
	const auto parent=getpid(); const auto child=fork();
	if (child==0) {
		prctl(PR_SET_PDEATHSIG,SIGKILL); if (getppid()!=parent) _exit(1);
		close(pipefd[0]);
		fcntl(pipefd[1],F_SETFL,fcntl(pipefd[1],F_GETFL)&~O_NONBLOCK);
		dup2(pipefd[1],STDOUT_FILENO); close(pipefd[1]);
		const int null=open("/dev/null",O_RDWR);
		if (null>=0) { dup2(null,STDIN_FILENO); dup2(null,STDERR_FILENO); if (null>2) close(null); }
		chdir("/");
		execl(command.c_str(),command.c_str(),"--monitor","--json",static_cast<char*>(nullptr)); _exit(127);
	}
	close(pipefd[1]);
	if (child<0) { close(pipefd[0]); return failure("Cannot start task-submit monitor"); }
	std::string output; bool eof=false, exited=false, timed_out=false, excessive=false; int status=0;
	const auto deadline=Protocol::now_ms()+1000;
	while (not (eof and exited)) {
		char buffer[8192]; ssize_t count;
		while ((count=read(pipefd[0],buffer,sizeof(buffer)))>0) {
			output.append(buffer,count);
			if (output.size()>output_limit) { excessive=true; break; }
		}
		if (count==0) eof=true;
		if (not exited) exited=waitpid(child,&status,WNOHANG)==child;
		if (excessive) break;
		if (Protocol::now_ms()>=deadline) { timed_out=true; break; }
		pollfd p{pipefd[0],POLLIN,0}; poll(&p,1,10);
	}
	close(pipefd[0]);
	if (not exited) { kill(child,SIGKILL); while (waitpid(child,&status,0)<0 and errno==EINTR) {} }
	if (timed_out) return failure("task-submit monitor exceeded 1 second","timeout");
	if (excessive) return failure("task-submit monitor exceeded 256 KiB");
	try {
		auto data=Protocol::card_snapshot(Protocol::document(output));
		if ((not WIFEXITED(status) or WEXITSTATUS(status)!=0) and data["status"]!="unavailable") return failure("task-submit monitor failed");
		const auto status_text=data["status"].get<std::string>();
		const auto reason=data["reason"].is_string() ? data["reason"].get<std::string>() : std::string{};
		return Protocol::group(std::move(data),Protocol::now_ms(),5000,status_text,reason);
	} catch (const std::exception&) { return failure("Invalid task-submit monitor JSON"); }
}
}

Json login_sessions(const std::filesystem::path& path) {
	std::ifstream input(path,std::ios::binary);
	if (not input) return failure("Login records unavailable");
	std::map<std::string,Json> users; std::uint64_t sessions=0; bool partial=false;
	utmpx entry{}; std::size_t records=0;
	while (input.read(reinterpret_cast<char*>(&entry),sizeof(entry))) {
		if (++records>16384) { partial=true; break; }
		if (entry.ut_type!=USER_PROCESS) continue;
		const auto user=std::string(entry.ut_user,strnlen(entry.ut_user,sizeof(entry.ut_user)));
		if (user.empty()) continue;
		if (sessions>=4096) { partial=true; break; }
		auto [item,inserted]=users.try_emplace(user,Json{{"user",user},{"sessions",0},{"first_login_at",nullptr}});
		(void)inserted; item->second["sessions"]=item->second["sessions"].get<int>()+1; ++sessions;
		if (entry.ut_tv.tv_sec>0 and (item->second["first_login_at"].is_null() or item->second["first_login_at"]>entry.ut_tv.tv_sec)) item->second["first_login_at"]=entry.ut_tv.tv_sec;
	}
	if (input.bad() or (input.gcount()!=0 and input.gcount()!=sizeof(entry))) partial=true;
	Json items=Json::array(); for (const auto& [name,item] : users) items.push_back(item);
	return Protocol::group({{"source","utmpx"},{"users",users.size()},{"sessions",sessions},{"items",items},{"age_ms",0}},
		Protocol::now_ms(),5000,partial ? "partial" : "ok",partial ? "Login records incomplete" : "");
}

void Resources::finish(bool stop) {
	if (fd_>=0) close(fd_);
	fd_=-1;
	if (child_>0) {
		// Only this worker's private process group, never the caller's terminal group.
		kill(-child_,SIGKILL);
		if (stop) kill(child_,SIGKILL);
		while (waitpid(child_,nullptr,0)<0 and errno==EINTR) {}
	}
	child_=-1;
}
Resources::~Resources() { finish(true); }
Json Resources::sample(std::uint64_t now) {
	if (cached_.is_null()) cached_={{"sessions",failure("Awaiting login sample","warming_up")},{"cards",failure("Awaiting card sample","warming_up")}};
	if (child_>0) {
		bool eof=false, invalid=false; char buffer[8192]; ssize_t count;
		while ((count=read(fd_,buffer,sizeof(buffer)))>0) { output_.append(buffer,count); if (output_.size()>2*output_limit) { invalid=true; break; } }
		if (count==0) eof=true;
		if (eof or invalid or (now>=started_ and now-started_>2000)) {
			try {
				if (not eof or invalid) throw std::runtime_error("Worker timeout");
				auto result=Protocol::document(output_);
				for (const auto* name : {"sessions","cards"}) Protocol::validate_resource_group(name,result.at(name));
				cached_=std::move(result);
			} catch (const std::exception&) {
				cached_={{"sessions",failure("Resource worker failed","timeout")},{"cards",failure("Resource worker failed","timeout")}};
			}
			finish(true);
		}
	}
	if (child_<0 and (started_==0 or (now>=started_ and now-started_>=5000))) {
		started_=now; output_.clear(); int pipefd[2];
		if (pipe2(pipefd,O_CLOEXEC|O_NONBLOCK)==0) {
			const auto parent=getpid(); child_=fork();
			if (child_==0) {
				setpgid(0,0); prctl(PR_SET_PDEATHSIG,SIGKILL); if (getppid()!=parent) _exit(1);
				close(pipefd[0]);
				const int null=open("/dev/null",O_RDWR);
				if (null>=0) { dup2(null,STDIN_FILENO); dup2(null,STDOUT_FILENO); dup2(null,STDERR_FILENO); if (null>2) close(null); }
				std::signal(SIGALRM,SIG_DFL); alarm(2);
				try {
					Json result{{"sessions",login_sessions(utmp_)},{"cards",read_cards(command_)}};
					Protocol::write_line(pipefd[1],result,500);
				} catch (const std::exception&) {}
				close(pipefd[1]); _exit(0);
			}
			close(pipefd[1]);
			if (child_>0) { setpgid(child_,child_); fd_=pipefd[0]; }
			else { close(pipefd[0]); cached_={{"sessions",failure("Cannot start resource worker")},{"cards",failure("Cannot start resource worker")}}; }
		} else cached_={{"sessions",failure("Cannot create resource worker pipe")},{"cards",failure("Cannot create resource worker pipe")}};
	}
	auto result=cached_;
	for (auto& [name,group] : result.items()) {
		(void)name;
		auto& data=group["data"]; const auto observed=group["observed_monotonic_ms"].get<std::uint64_t>();
		if (data.is_object() and data.contains("age_ms") and data["age_ms"].is_number_unsigned()) data["age_ms"]=data["age_ms"].get<std::uint64_t>()+(now>observed ? now-observed : 0);
		if (data.is_object() and (data["age_ms"].is_null() or data["age_ms"]>15000 or data.value("cached",false))) { group["status"]="unavailable"; group["reason"]="Resource snapshot is stale"; }
	}
	return result;
}
}
