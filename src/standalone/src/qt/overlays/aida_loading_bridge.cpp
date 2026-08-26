#include "qt/overlays/aida_loading_bridge.hpp"

#include <algorithm>

#include "core/session/analysis_session.hpp"
#include "helpers/diag_log.hpp"

namespace loading_binary_overlay {

namespace {

std::function<void(const char* view_id)> g_view_focus_hook;

void focus_view(const char* view_id)
{
    if (g_view_focus_hook) {
        g_view_focus_hook(view_id);
        return;
    }
    diag::log_tagged_fmt("loading_binary_overlay",
        "focus_hook_missing view=%s", view_id ? view_id : "<null>");
}

}

namespace detail {

std::mutex& registry_mutex()
{
    static std::mutex value;
    return value;
}

std::unordered_map<std::string, std::shared_ptr<state_t>>& registry()
{
    static std::unordered_map<std::string, std::shared_ptr<state_t>> value;
    return value;
}

std::string derive_filename(const std::string& path)
{
    const size_t separator = path.find_last_of("/\\");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

const char* phase_name(phase_t phase)
{
    switch (phase) {
    case phase_t::idle: return "idle";
    case phase_t::loading: return "loading";
    case phase_t::awaiting_analysis: return "awaiting_analysis";
    case phase_t::awaiting_pdb_decision: return "awaiting_pdb_decision";
    case phase_t::loading_pdb: return "loading_pdb";
    case phase_t::finalizing: return "finalizing";
    case phase_t::complete: return "complete";
    default: return "unknown";
    }
}

std::shared_ptr<state_t> selected_state()
{
    const size_t active = analysis_session::active_session_idx();
    if (active == static_cast<size_t>(-1)) return {};
    const auto summary = analysis_session::summarize_session_at(active);
    if (summary.id.empty()) return {};
    std::lock_guard<std::mutex> lock(registry_mutex());
    const auto found = registry().find(summary.id);
    return found == registry().end() ? nullptr : found->second;
}

phase_t phase_for(const analysis_session::session_summary_t& summary)
{
    if (summary.pdb_loading) return phase_t::loading_pdb;
    if (summary.pdb_remote_pending || summary.pdb_local_pending)
        return phase_t::awaiting_pdb_decision;
    switch (summary.load_state) {
    case analysis_session::session_load_state_t::opening:
        return phase_t::loading;
    case analysis_session::session_load_state_t::analyzing:
        return phase_t::awaiting_analysis;
    case analysis_session::session_load_state_t::ready:
    case analysis_session::session_load_state_t::failed:
    case analysis_session::session_load_state_t::closed:
        return phase_t::complete;
    case analysis_session::session_load_state_t::closing:
        return phase_t::finalizing;
    default:
        return phase_t::idle;
    }
}

float progress_for(const analysis_session::session_summary_t& summary)
{
    if (summary.pdb_loading) {
        if (summary.pdb_bytes_total != 0) {
            return static_cast<float>((std::min)(1.0L,
                static_cast<long double>(summary.pdb_bytes_received) /
                static_cast<long double>(summary.pdb_bytes_total)));
        }
        return static_cast<float>((std::max)(0, (std::min)(100,
            summary.pdb_progress_percent))) / 100.0f;
    }
    const auto workspace = analysis_session::workspace_for_session_id(summary.id);
    if (!workspace) return -1.f;
    const auto progress = workspace->progress();
    if (summary.load_state == analysis_session::session_load_state_t::ready) return 1.f;
    if (progress.total_bytes != 0) {
        return (std::min)(0.99f, static_cast<float>(
            static_cast<long double>(progress.completed_bytes) /
            static_cast<long double>(progress.total_bytes)));
    }
    if (progress.total_units != 0) {
        return (std::min)(0.99f, static_cast<float>(
            static_cast<long double>(progress.completed_units) /
            static_cast<long double>(progress.total_units)));
    }
    return -1.f;
}

std::string label_for(const analysis_session::session_summary_t& summary)
{
    if (summary.error)
        return summary.error->stable_code() + ": " + summary.error->message;
    if (summary.pdb_loading || summary.pdb_remote_pending || summary.pdb_local_pending)
        return summary.pdb_status.empty() ? "Debug symbols require attention"
            : summary.pdb_status;
    const auto workspace = analysis_session::workspace_for_session_id(summary.id);
    if (!workspace) return "Opening mapped workspace...";
    const auto progress = workspace->progress();
    if (!progress.phase.empty()) return progress.phase;
    return summary.load_state == analysis_session::session_load_state_t::ready
        ? "Analysis ready"
        : "Analyzing workspace...";
}

}

void set_view_focus_hook(std::function<void(const char* view_id)> hook)
{
    g_view_focus_hook = std::move(hook);
}

void track_session(const std::string& session_id, const std::string& path,
                   completion_action_t action)
{
    if (session_id.empty() || path.empty()) return;
    auto value = std::make_shared<detail::state_t>();
    value->session_id = session_id;
    value->path = path;
    value->filename = detail::derive_filename(path);
    value->action = action;
    std::lock_guard<std::mutex> lock(detail::registry_mutex());
    detail::registry().insert_or_assign(session_id, std::move(value));
}

void release_session(const std::string& session_id)
{
    if (session_id.empty()) return;
    std::lock_guard<std::mutex> lock(detail::registry_mutex());
    detail::registry().erase(session_id);
}

phase_t current_phase()
{
    const size_t active = analysis_session::active_session_idx();
    if (active == static_cast<size_t>(-1)) return phase_t::idle;
    return detail::phase_for(analysis_session::summarize_session_at(active));
}

const char* current_phase_name()
{
    return detail::phase_name(current_phase());
}

bool is_active()
{
    const phase_t phase = current_phase();
    return phase != phase_t::idle && phase != phase_t::complete;
}

bool is_waiting_for_user_decision()
{
    return current_phase() == phase_t::awaiting_pdb_decision;
}

void log_state(const char* reason)
{
    const size_t active = analysis_session::active_session_idx();
    const auto summary = active == static_cast<size_t>(-1)
        ? analysis_session::session_summary_t{}
        : analysis_session::summarize_session_at(active);
    diag::log_tagged_fmt("loading_binary_overlay",
        "state reason=%s session=%s binary_id=%s phase=%s load_state=%u readiness=%u error=%s",
        reason ? reason : "",
        summary.id.c_str(), summary.binary_id.c_str(), current_phase_name(),
        static_cast<unsigned>(summary.load_state),
        static_cast<unsigned>(summary.readiness),
        summary.error ? summary.error->stable_code().c_str() : "none");
}

bool cancel_queued_load(const char* reason)
{
    const size_t active = analysis_session::active_session_idx();
    if (active == static_cast<size_t>(-1)) return false;
    const bool cancelled = analysis_session::cancel_session(active);
    diag::log_tagged_fmt("loading_binary_overlay",
        "cancel reason=%s active=%llu cancelled=%d",
        reason ? reason : "",
        static_cast<unsigned long long>(active), cancelled ? 1 : 0);
    return cancelled;
}

bool is_load_ready_for_tools()
{
    const size_t active = analysis_session::active_session_idx();
    if (active == static_cast<size_t>(-1)) return false;
    const auto summary = analysis_session::summarize_session_at(active);
    return summary.load_state == analysis_session::session_load_state_t::ready &&
        (summary.kind == analysis_session::session_kind_t::live_attach ||
         summary.readiness == aida::analysis::workspace_readiness_t::baseline_ready ||
         summary.readiness == aida::analysis::workspace_readiness_t::partial);
}

bool is_blocking_views()
{
    return current_phase() == phase_t::loading;
}

void begin_load(const std::string& path, completion_action_t action)
{
    if (path.empty()) return;
    size_t existing = 0;
    if (analysis_session::find_session_by_path(path, &existing)) {
        (void)analysis_session::switch_session(existing);
        const auto summary = analysis_session::summarize_session_at(existing);
        track_session(summary.id, path, action);
        return;
    }
    if (!analysis_session::open_session(path)) return;
    const size_t active = analysis_session::active_session_idx();
    if (active == static_cast<size_t>(-1)) return;
    const auto summary = analysis_session::summarize_session_at(active);
    track_session(summary.id, path, action);
}

void poll_completion()
{
    const size_t active = analysis_session::active_session_idx();
    if (active == static_cast<size_t>(-1)) return;
    const auto summary = analysis_session::summarize_session_at(active);
    if (summary.id.empty() || summary.load_state != analysis_session::session_load_state_t::ready)
        return;
    auto state = detail::selected_state();
    if (!state || state->completion_applied.exchange(true, std::memory_order_acq_rel))
        return;
    if (state->action == completion_action_t::switch_to_disassembly ||
        state->action == completion_action_t::switch_to_disassembly_or_hex)
        focus_view("document.disassembly");
}

}
