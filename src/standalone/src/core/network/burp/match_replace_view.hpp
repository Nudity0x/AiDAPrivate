#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "../network_view.hpp"

namespace aida {
namespace burp {
namespace match_replace_view {

// The ImGui view half moved to
// qt/network/match_replace/match_replace_view.cpp. What remains here is the
// backend seam used by network_view.cpp's retained artifact dispatch plus the
// staged reviewed-context postbox (validation strings verbatim).
struct staged_reviewed_context_t {
    network_view::artifact_identity_t identity;
    bool response_target = false;
};

bool stage_reviewed_context(const network_view::artifact_identity_t& identity,
                            bool response_target,
                            std::string& unavailable_reason);
bool take_staged_reviewed_context(staged_reviewed_context_t& out);
void set_reviewed_context_staged_hook(std::function<void()> hook);

}
}
}
