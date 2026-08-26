#include "sequencer_view.hpp"
#include "qt/net/http_text_utils.hpp"

#include <mutex>
#include <utility>

namespace aida {
namespace burp {
namespace sequencer_view {

namespace {

constexpr std::size_t k_staged_url_limit = 1024;
constexpr std::size_t k_staged_host_limit = 256;
constexpr std::size_t k_staged_request_limit = 8192;

std::mutex g_staged_mutex;
bool g_staged_pending = false;
staged_new_collection_t g_staged;
std::function<void()> g_staged_hook;

}

bool open_new_collection_with(const std::string& url, const std::string& host,
                              std::uint16_t port, bool use_tls,
                              const std::string& raw_request, std::string& reason)
{
    if (url.empty() || host.empty() || port == 0 || raw_request.empty()) {
        reason = "Sequencer requires a URL, host, port, and non-empty request.";
        return false;
    }
    if (url.size() >= k_staged_url_limit ||
        qt::net::http_text::contains_binary_bytes(url)) {
        reason = "Sequencer requires a valid UTF-8 text URL shorter than 1024 bytes.";
        return false;
    }
    if (host.size() >= k_staged_host_limit ||
        qt::net::http_text::contains_binary_bytes(host)) {
        reason = "Sequencer requires a valid UTF-8 text host shorter than 256 bytes.";
        return false;
    }
    if (raw_request.size() >= k_staged_request_limit ||
        qt::net::http_text::contains_binary_bytes(raw_request)) {
        reason = "Sequencer requires a valid UTF-8 text request of at most 8191 bytes.";
        return false;
    }
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(g_staged_mutex);
        g_staged.url = url;
        g_staged.host = host;
        g_staged.port = port;
        g_staged.use_tls = use_tls;
        g_staged.raw_request = raw_request;
        g_staged_pending = true;
        hook = g_staged_hook;
    }
    if (hook)
        hook();
    reason.clear();
    return true;
}

bool take_staged_new_collection(staged_new_collection_t& out)
{
    std::lock_guard<std::mutex> lock(g_staged_mutex);
    if (!g_staged_pending)
        return false;
    out = g_staged;
    g_staged = staged_new_collection_t{};
    g_staged_pending = false;
    return true;
}

void set_new_collection_staged_hook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(g_staged_mutex);
    g_staged_hook = std::move(hook);
}

}
}
}
