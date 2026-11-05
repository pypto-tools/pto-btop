// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "view.hpp"
#include "net/common.hpp"
#include <memory>
namespace Cluster {
class HttpsFeed {
	struct Impl;
	std::unique_ptr<Impl> impl_;
public:
	HttpsFeed(const std::string& config,std::vector<Host>& hosts);
	~HttpsFeed();
	bool tick(std::vector<Host>& hosts,const std::vector<Protocol::Subscription>& requests);
	void reconnect();
};
}
