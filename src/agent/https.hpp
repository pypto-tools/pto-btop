// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <csignal>
#include <string>
int run_https_agent(const std::string& path,volatile std::sig_atomic_t& stopped);
