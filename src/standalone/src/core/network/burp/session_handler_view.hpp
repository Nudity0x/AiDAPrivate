#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../network_view.hpp"

namespace aida {
namespace burp {
namespace session_handler_view {

// The ImGui view half moved to
// qt/network/session_handler/session_handler_view.cpp. What remains here is
// the backend seam used by network_view.cpp's retained artifact dispatch plus
// the staged reviewed-context postbox (validation strings verbatim).
struct staged_reviewed_context_t {
    network_view::artifact_identity_t identity;
    std::vector<std::uint8_t> request_bytes;
};

bool stage_reviewed_context(const network_view::artifact_identity_t& identity,
                            std::string& unavailable_reason);
bool take_staged_reviewed_context(staged_reviewed_context_t& out);
void set_reviewed_context_staged_hook(std::function<void()> hook);

}
}
}
