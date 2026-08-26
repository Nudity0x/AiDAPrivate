#include "intruder_view.hpp"
#include "intruder_engine.hpp"
#include "qt/net/http_text_utils.hpp"

#include <mutex>
#include <utility>

namespace aida {
namespace burp {
namespace intruder_view {

namespace {

constexpr std::size_t k_staged_host_limit = 256;
constexpr std::size_t k_staged_request_limit = 65536;

std::mutex g_staged_mutex;
bool g_staged_pending = false;
staged_new_attack_t g_staged;
std::function<void()> g_staged_hook;

}

bool open_new_attack_with(const std::string& host, std::uint16_t port, bool use_tls,
                          const std::string& raw_request, std::string& reason)
{
    if (host.empty() || port == 0 || raw_request.empty()) {
        reason = "Intruder requires a host, port, and non-empty request.";
        return false;
    }
    if (host.size() >= k_staged_host_limit ||
        qt::net::http_text::contains_binary_bytes(host)) {
        reason = "Intruder requires a valid UTF-8 text host shorter than 256 bytes.";
        return false;
    }
    if (raw_request.size() >= k_staged_request_limit ||
        qt::net::http_text::contains_binary_bytes(raw_request)) {
        reason = "Intruder requires a valid UTF-8 text request of at most 65535 bytes.";
        return false;
    }
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(g_staged_mutex);
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

bool resolve_retained_artifact(std::uint64_t job_id, std::uint64_t result_index,
                               std::uint64_t started_ms, std::vector<std::uint8_t>& bytes,
                               std::string& reason)
{
    const auto status = intruder::status(job_id);
    if (status.job_id != job_id || status.started_unix_ms != started_ms) {
        reason = "The Intruder job changed or is no longer retained.";
        return false;
    }
    const auto rows = intruder::results(job_id, static_cast<std::size_t>(result_index), 1);
    if (rows.size() != 1 || rows.front().index != result_index) {
        reason = "The Intruder response is no longer retained.";
        return false;
    }
    bytes = rows.front().response_raw;
    reason.clear();
    return true;
}

bool take_staged_new_attack(staged_new_attack_t& out)
{
    std::lock_guard<std::mutex> lock(g_staged_mutex);
    if (!g_staged_pending)
        return false;
    out = g_staged;
    g_staged = staged_new_attack_t{};
    g_staged_pending = false;
    return true;
}

void set_new_attack_staged_hook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(g_staged_mutex);
    g_staged_hook = std::move(hook);
}

}
}
}
