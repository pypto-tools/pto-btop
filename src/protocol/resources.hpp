// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protocol.hpp"

namespace Protocol {
// Validate and retain only the public, bounded monitor fields used by the UI.
Json usage_snapshot(const Json& source);
Json card_snapshot(const Json& source);
void validate_resource_group(const std::string& name, const Json& group);
}
