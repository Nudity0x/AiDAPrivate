#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace aida {
namespace burp {
namespace sequencer_view {

// The ImGui view half moved to qt/network/sequencer/sequencer_view.cpp. What
// remains here is the backend seam used by network_view.cpp's retained
// artifact dispatch plus the staged-collection postbox (validation strings
// verbatim).
struct staged_new_collection_t {
    std::string url;
    std::string host;
    std::uint16_t port = 0;
    bool use_tls = true;
    std::string raw_request;
};

bool open_new_collection_with(const std::string& url, const std::string& host,
                              std::uint16_t port, bool use_tls,
                              const std::string& raw_request, std::string& reason);
bool take_staged_new_collection(staged_new_collection_t& out);
void set_new_collection_staged_hook(std::function<void()> hook);

}
}
}
