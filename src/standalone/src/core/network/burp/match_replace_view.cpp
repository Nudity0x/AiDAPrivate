#include "match_replace_view.hpp"

#include <mutex>
#include <utility>

namespace aida {
namespace burp {
namespace match_replace_view {

namespace {

constexpr std::size_t k_staged_host_limit = 256;

bool http1_artifact_kind(network_view::artifact_kind_t kind)
{
    return kind == network_view::artifact_kind_t::exchange ||
        kind == network_view::artifact_kind_t::request ||
        kind == network_view::artifact_kind_t::response ||
        kind == network_view::artifact_kind_t::repeater_request ||
        kind == network_view::artifact_kind_t::repeater_response ||
        kind == network_view::artifact_kind_t::sitemap_request ||
        kind == network_view::artifact_kind_t::sitemap_response ||
        kind == network_view::artifact_kind_t::api_request ||
        kind == network_view::artifact_kind_t::api_response ||
        kind == network_view::artifact_kind_t::intruder_response ||
        kind == network_view::artifact_kind_t::scanner_request ||
        kind == network_view::artifact_kind_t::scanner_response ||
        kind == network_view::artifact_kind_t::intercept_request;
}

std::mutex g_staged_mutex;
bool g_staged_pending = false;
staged_reviewed_context_t g_staged;
std::function<void()> g_staged_hook;

}

bool stage_reviewed_context(const network_view::artifact_identity_t& identity,
                            bool response_target,
                            std::string& unavailable_reason)
{
    if (!identity.valid() || !http1_artifact_kind(identity.kind) ||
        identity.target_host.empty() || identity.target_port == 0 ||
        identity.target_host.size() >= k_staged_host_limit || identity.raw_protocol) {
        unavailable_reason = "Match and Replace requires a current retained HTTP/1 artifact with a bounded target.";
        return false;
    }
    network_view::artifact_snapshot_t snapshot;
    if (!network_view::resolve_artifact(identity, snapshot, unavailable_reason))
        return false;
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(g_staged_mutex);
        g_staged.identity = identity;
        g_staged.response_target = response_target;
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
    out = g_staged;
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
