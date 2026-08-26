#include "session_handler_view.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace aida {
namespace burp {
namespace session_handler_view {

namespace {

constexpr std::size_t k_staged_host_limit = 256;
constexpr std::size_t k_staged_request_limit = 8192;

bool request_artifact_kind(network_view::artifact_kind_t kind)
{
    return kind == network_view::artifact_kind_t::exchange ||
        kind == network_view::artifact_kind_t::request ||
        kind == network_view::artifact_kind_t::repeater_request ||
        kind == network_view::artifact_kind_t::sitemap_request ||
        kind == network_view::artifact_kind_t::api_request ||
        kind == network_view::artifact_kind_t::scanner_request ||
        kind == network_view::artifact_kind_t::intercept_request;
}

std::mutex g_staged_mutex;
bool g_staged_pending = false;
staged_reviewed_context_t g_staged;
std::function<void()> g_staged_hook;

}

bool stage_reviewed_context(const network_view::artifact_identity_t& identity,
                            std::string& unavailable_reason)
{
    if (!identity.valid() || !request_artifact_kind(identity.kind) ||
        identity.target_host.empty() || identity.target_port == 0 ||
        identity.target_host.size() >= k_staged_host_limit || identity.raw_protocol) {
        unavailable_reason = "Session Handling requires a current retained HTTP/1 request with a bounded target.";
        return false;
    }
    network_view::artifact_snapshot_t snapshot;
    if (!network_view::resolve_artifact(identity, snapshot, unavailable_reason))
        return false;
    if (snapshot.bytes.empty() || snapshot.bytes.size() >= k_staged_request_limit ||
        std::find(snapshot.bytes.begin(), snapshot.bytes.end(), 0) != snapshot.bytes.end()) {
        unavailable_reason = "Session Handling accepts a NUL-free reviewed request of at most 8191 bytes.";
        return false;
    }
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(g_staged_mutex);
        g_staged.identity = identity;
        g_staged.request_bytes = snapshot.bytes;
        g_staged_pending = true;
        hook = g_staged_hook;
    }
    if (hook)
        hook();
    unavailable_reason.clear();
    return true;
}

bool take_staged_reviewed_context(staged_reviewed_context_t& out)
{
    std::lock_guard<std::mutex> lock(g_staged_mutex);
    if (!g_staged_pending)
        return false;
    out = std::move(g_staged);
    g_staged = staged_reviewed_context_t{};
    g_staged_pending = false;
    return true;
}

void set_reviewed_context_staged_hook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(g_staged_mutex);
    g_staged_hook = std::move(hook);
}

}
}
}
