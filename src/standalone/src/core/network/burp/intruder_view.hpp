#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aida {
namespace burp {
namespace intruder_view {

// The ImGui view half moved to qt/network/intruder/intruder_view.cpp. What
// remains here is the backend seam used by network_view.cpp's retained
// artifact dispatch plus the staged-request postbox: open_new_attack_with
// validates with the exact legacy strings, stores the staged payload, and
// fires the staged hook; the Qt IntruderView drains it on creation and on the
// queued hook notification.
struct staged_new_attack_t {
    std::string host;
    std::uint16_t port = 0;
    bool use_tls = true;
    std::string raw_request;
};

bool open_new_attack_with(const std::string& host, std::uint16_t port, bool use_tls,
                          const std::string& raw_request, std::string& reason);
bool resolve_retained_artifact(std::uint64_t job_id, std::uint64_t result_index,
                               std::uint64_t started_ms, std::vector<std::uint8_t>& bytes,
                               std::string& reason);
bool take_staged_new_attack(staged_new_attack_t& out);
void set_new_attack_staged_hook(std::function<void()> hook);

}
}
}
