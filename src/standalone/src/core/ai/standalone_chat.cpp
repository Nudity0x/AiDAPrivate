#include <windows.h>
#include <intrin.h>

#include "standalone_chat.hpp"
#include "qt/overlays/aida_loading_bridge.hpp"
#include "mcp_standalone.hpp"
#include "mcp_client.hpp"
#include "mcp_marketplace.hpp"
#include "standalone_ai_client.hpp"
#include "standalone_settings.hpp"
#include "standalone_driver.hpp"
#include "agent_registry.hpp"
#include "binary_map.hpp"
#include "tool_repetition.hpp"
#include "standalone_tools_fwd.hpp"
#include "compaction.hpp"
#include "command_registry.hpp"
#include "conversation_history.hpp"
#include "session_store.hpp"
#include "auth_store.hpp"
#include "standalone_context.hpp"
#include "event_bus.hpp"
#include "provider_catalog.hpp"
#include "zydis_disasm.hpp"
#include "auto_approval.hpp"
#include "file_context_tracker.hpp"
#include "skills.hpp"
#include "../analysis/stealth_engine.hpp"
#include "../ui/task_center.hpp"
#include "../ui/toast_notification.hpp"

#include "../helpers/globals.h"
#include "../session/analysis_session.hpp"
#include "../session/cost_calculator.hpp"

#include <thread>
#include <mutex>
#include <sstream>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include "../editor/code_editor.hpp"
#include <sstream>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "../helpers/diag_log.hpp"

#include "../infra/executor.hpp"
#include "../infra/taskflow_runtime.hpp"
#include "../session/session_store.hpp"
#include "../analysis/workspace/overlay_journal.hpp"
#include "../settings/settings_persistence_service.hpp"
#include "../editor/code_editor.hpp"
#include "../disasm/disasm_view.hpp"
#include "../debugger/debugger_view.hpp"
#include "../network/network_view.hpp"
#include "../ui/ui_thread_dispatcher.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using json = nlohmann::json;

mcp_client::manager_t s_mcp_client_mgr;


static file_context::tracker_t s_file_tracker;


static auto_approval::task_counters_t s_approval_counters;
static std::mutex                     s_approval_counters_mtx;

namespace aida::automation_ui { namespace { void chat_open_view(const std::string& view_id); } }

namespace {

struct ai_update_t
{
    enum type_t { THINKING, CHUNK, COMPLETE, ERR } type;
    std::string text;
};

std::mutex              s_update_mtx;
std::deque<ai_update_t> s_updates;

std::function<void()> s_stream_notify_hook;

void post_update(ai_update_t::type_t type, const std::string& text = {})
{
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lk(s_update_mtx);
        s_updates.push_back({type, text});
        hook = s_stream_notify_hook;
    }
    if (hook) hook();
}


std::mutex        s_ai_thread_mtx;
std::atomic<bool> s_ai_running{false};
std::atomic<bool> s_cancel{false};
std::atomic<bool> s_ai_task_done{true};
std::mutex        s_ai_task_done_mtx;
std::condition_variable s_ai_task_done_cv;

std::mutex   s_chat_session_mtx;
std::string  s_chat_session_id;
std::string  s_chat_last_assistant_message_id;
int64_t      s_chat_used_tokens = 0;

ULONGLONG shutdown_phase_begin(const char* phase)
{
    const ULONGLONG start = GetTickCount64();
    diag::log_tagged_critical_fmt("chat",
        "shutdown_phase_begin phase=%s pid=%lu tid=%lu",
        phase && *phase ? phase : "<unknown>",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()));
    return start;
}

void shutdown_phase_done(const char* phase, ULONGLONG start)
{
    const ULONGLONG elapsed = GetTickCount64() - start;
    diag::log_tagged_critical_fmt("chat",
        "shutdown_phase_done phase=%s elapsed_ms=%llu pid=%lu tid=%lu",
        phase && *phase ? phase : "<unknown>",
        static_cast<unsigned long long>(elapsed),
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()));
}

bool submit_chat_task(const char* label,
                      aida::infra::executor::domain_t domain,
                      const char* thread_class,
                      int priority,
                      std::function<void()> body)
{
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "ai_chat";
    sub.label = label;
    sub.thread_class = thread_class;
    sub.domain = domain;
    sub.priority = priority;
    sub.body = std::move(body);
    const auto submitted = aida::infra::executor::submit(std::move(sub));
    if (submitted.submitted && submitted.task_id != 0) {
        aida::ui::task_center::task_registration_t registration;
        registration.owner = "automation";
        registration.owner_view = "view.ai_chat";
        registration.owner_action = label ? label : "ai_chat.task";
        registration.label = label ? label : "AI Chat task";
        registration.stage = "Queued";
        const bool agentic_request = label != nullptr &&
            std::strcmp(label, "chat.agentic_request") == 0;
        registration.cancellation_is_safe = agentic_request;
        if (agentic_request) {
            registration.callbacks.cancel = [] {
                chat_request_cancel();
                return true;
            };
        }
        registration.callbacks.focus = [] {
            aida::automation_ui::chat_open_view("view.ai_chat");
        };
        (void)aida::ui::task_center::register_executor_job(submitted.task_id, std::move(registration));
    }
    return submitted.submitted;
}

void log_shutdown_queue_snapshot(const char* phase)
{
    const auto exec = aida::infra::executor::active_snapshot();
    const auto runtime = aida::infra::taskflow_runtime::active_snapshot(128);
    diag::log_tagged_critical_fmt("chat",
        "shutdown_executor_snapshot phase=%s accepting=%d shutting_down=%d total_active=%u general_pending=%llu general_active=%u service_pending=%llu service_active=%u critical_pending=%llu critical_active=%u oldest_ms=%llu runtime_submitted=%llu runtime_rejected=%llu runtime_cancelled=%llu runtime_failed=%llu runtime_timed_out=%llu labels=%.760s",
        phase && *phase ? phase : "<unknown>",
        runtime.accepting ? 1 : 0,
        runtime.shutting_down ? 1 : 0,
        static_cast<unsigned>(exec.total_active),
        static_cast<unsigned long long>(exec.work_queue_pending),
        static_cast<unsigned>(exec.work_queue_active),
        static_cast<unsigned long long>(exec.service_queue_pending),
        static_cast<unsigned>(exec.service_queue_active),
        static_cast<unsigned long long>(exec.critical_queue_pending),
        static_cast<unsigned>(exec.critical_queue_active),
        static_cast<unsigned long long>(exec.oldest_active_ms),
        static_cast<unsigned long long>(runtime.total_submitted),
        static_cast<unsigned long long>(runtime.total_rejected),
        static_cast<unsigned long long>(runtime.total_cancelled),
        static_cast<unsigned long long>(runtime.total_failed),
        static_cast<unsigned long long>(runtime.total_timed_out),
        exec.labels_under_pressure.empty() ? "<none>" : exec.labels_under_pressure.c_str());
}

bool shutdown_queues_quiescent(const char* phase)
{
    const auto exec = aida::infra::executor::active_snapshot();
    const bool quiescent =
        exec.critical_queue_pending == 0 && exec.critical_queue_active == 0 &&
        exec.work_queue_pending == 0 && exec.work_queue_active == 0 &&
        exec.service_queue_pending == 0 && exec.service_queue_active == 0;
    if (!quiescent) {
        diag::log_tagged_critical_fmt("chat",
            "shutdown_executor_drain_incomplete phase=%s critical_pending=%llu critical_active=%u general_pending=%llu general_active=%u service_pending=%llu service_active=%u labels=%.760s",
            phase && *phase ? phase : "<unknown>",
            static_cast<unsigned long long>(exec.critical_queue_pending),
            static_cast<unsigned>(exec.critical_queue_active),
            static_cast<unsigned long long>(exec.work_queue_pending),
            static_cast<unsigned>(exec.work_queue_active),
            static_cast<unsigned long long>(exec.service_queue_pending),
            static_cast<unsigned>(exec.service_queue_active),
            exec.labels_under_pressure.empty() ? "<none>" : exec.labels_under_pressure.c_str());
    }
    return quiescent;
}

std::string get_chat_session_id_locked()
{
    std::lock_guard<std::mutex> lk(s_chat_session_mtx);
    return s_chat_session_id;
}

void set_chat_session_id_locked(const std::string& sid)
{
    std::lock_guard<std::mutex> lk(s_chat_session_mtx);
    s_chat_session_id = sid;
    s_chat_used_tokens = 0;
    s_chat_last_assistant_message_id.clear();
}

void add_chat_used_tokens_locked(int64_t n)
{
    std::lock_guard<std::mutex> lk(s_chat_session_mtx);
    s_chat_used_tokens += n;
}

int64_t get_chat_used_tokens_locked()
{
    std::lock_guard<std::mutex> lk(s_chat_session_mtx);
    return s_chat_used_tokens;
}

void set_chat_last_assistant_message_id_locked(const std::string& mid)
{
    std::lock_guard<std::mutex> lk(s_chat_session_mtx);
    s_chat_last_assistant_message_id = mid;
}

std::string get_chat_last_assistant_message_id_locked()
{
    std::lock_guard<std::mutex> lk(s_chat_session_mtx);
    return s_chat_last_assistant_message_id;
}


mcp_standalone::server_t s_mcp_server;
std::atomic<bool>        s_server_started{false};
std::atomic<bool>        s_initialized{false};
std::atomic<bool>        s_mcp_tools_registered{false};
std::atomic<bool>        s_ide_ready_for_mcp_services{false};
std::atomic<bool>        s_mcp_shutdown_in_flight{false};
std::atomic<bool>        s_mcp_start_in_flight{false};
std::atomic<uint64_t>    s_mcp_start_last_post_ms{0};


std::atomic<bool>        s_mcp_clients_connected{false};


struct tool_approval_t {
    std::mutex          mtx;
    std::condition_variable cv;
    bool                pending   = false;
    bool                approved  = false;
    bool                answered  = false;
    std::uint64_t       generation = 0;
    std::string         tool_name;
    std::string         tool_args_preview;
};
tool_approval_t s_tool_approval;
std::function<void()> s_tool_approval_notify_hook;

}

namespace aida::automation_ui {

namespace {

std::atomic<std::uint64_t> s_chat_scroll_seq{0};
std::atomic<std::uint64_t> s_chat_composer_clear_seq{0};

std::mutex              s_chat_inject_mtx;
std::deque<std::string> s_chat_inject_queue;
std::function<void()>   s_chat_inject_notify_hook;

struct pending_message_edit_t {
    message_identity_t identity;
    std::string text;
};
std::mutex                              s_message_edit_mtx;
std::optional<pending_message_edit_t>   s_pending_message_edit;
std::uint64_t                           s_message_edit_seq = 0;
std::function<void()>                   s_message_edit_notify_hook;

std::function<void(const std::string&)> s_chat_clipboard_hook;
std::function<void(const std::string&)> s_chat_open_view_hook;
std::function<void()>                   s_agent_picker_toggle_hook;
std::vector<std::function<void()>>      s_ui_shutdown_hooks;

bool copy_text_to_chat_clipboard(const std::string& text)
{
    if (!s_chat_clipboard_hook) return false;
    s_chat_clipboard_hook(text);
    return true;
}

void chat_open_view(const std::string& view_id)
{
    if (s_chat_open_view_hook) s_chat_open_view_hook(view_id);
}

}

void request_chat_scroll_to_bottom()
{
    s_chat_scroll_seq.fetch_add(1, std::memory_order_acq_rel);
}

std::uint64_t chat_scroll_sequence()
{
    return s_chat_scroll_seq.load(std::memory_order_acquire);
}

void post_chat_inject(std::string text)
{
    if (text.empty()) return;
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lk(s_chat_inject_mtx);
        s_chat_inject_queue.push_back(std::move(text));
        hook = s_chat_inject_notify_hook;
    }
    if (hook) hook();
}

void request_chat_composer_clear()
{
    s_chat_composer_clear_seq.fetch_add(1, std::memory_order_acq_rel);
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lk(s_chat_inject_mtx);
        hook = s_chat_inject_notify_hook;
    }
    if (hook) hook();
}

std::uint64_t chat_composer_clear_sequence()
{
    return s_chat_composer_clear_seq.load(std::memory_order_acquire);
}

std::deque<std::string> drain_chat_inject()
{
    std::deque<std::string> local;
    std::lock_guard<std::mutex> lk(s_chat_inject_mtx);
    local.swap(s_chat_inject_queue);
    return local;
}

void set_stream_notify_hook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lk(s_update_mtx);
    s_stream_notify_hook = std::move(hook);
}

void set_tool_approval_notify_hook(std::function<void()> hook)
{
    s_tool_approval_notify_hook = std::move(hook);
}

void set_chat_inject_notify_hook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lk(s_chat_inject_mtx);
    s_chat_inject_notify_hook = std::move(hook);
}

void set_message_edit_notify_hook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lk(s_message_edit_mtx);
    s_message_edit_notify_hook = std::move(hook);
}

void set_chat_clipboard_hook(std::function<void(const std::string&)> hook)
{
    s_chat_clipboard_hook = std::move(hook);
}

void set_chat_open_view_hook(std::function<void(const std::string& view_id)> hook)
{
    s_chat_open_view_hook = std::move(hook);
}

void set_agent_picker_toggle_hook(std::function<void()> hook)
{
    s_agent_picker_toggle_hook = std::move(hook);
}

void add_ui_shutdown_hook(std::function<void()> hook)
{
    if (hook) s_ui_shutdown_hooks.push_back(std::move(hook));
}

void run_ui_shutdown_hooks()
{
    for (auto& hook : s_ui_shutdown_hooks) {
        try { hook(); } catch (...) {}
    }
}

}

thread_local std::string t_tool_approval_deny_reason;
std::atomic<uint64_t> s_tool_fanout_group_seq{0};

auto_approval::task_counters_t approval_counters_snapshot()
{
    std::lock_guard<std::mutex> lk(s_approval_counters_mtx);
    return s_approval_counters;
}

void reset_approval_counters()
{
    std::lock_guard<std::mutex> lk(s_approval_counters_mtx);
    s_approval_counters = auto_approval::task_counters_t{};
}

void note_tool_execution_for_approval_limits()
{
    std::lock_guard<std::mutex> lk(s_approval_counters_mtx);
    s_approval_counters.auto_approved_requests++;
}

struct mcp_start_in_flight_guard_t
{
    ~mcp_start_in_flight_guard_t()
    {
        s_mcp_start_in_flight.store(false, std::memory_order_release);
    }
};

void start_authorized_mcp_services_worker()
{
    mcp_start_in_flight_guard_t guard;
    const uint64_t started = static_cast<uint64_t>(GetTickCount64());

    if (s_mcp_shutdown_in_flight.load(std::memory_order_acquire)) {
        diag::log_tagged("init_chat", "authorized_mcp_services_start_worker_deferred_shutdown_in_flight");
        return;
    }

    const bool ide_ready = s_ide_ready_for_mcp_services.load(std::memory_order_acquire);
    if (!ide_ready)
    {
        mcp_standalone::set_ide_lifecycle_ready(false);
        diag::log_tagged_fmt("init_chat",
            "authorized_mcp_services_blocked ide=%d",
            ide_ready ? 1 : 0);
        return;
    }

    mcp_standalone::set_ide_lifecycle_ready(true);

    bool tools_expected = false;
    if (s_mcp_tools_registered.compare_exchange_strong(tools_expected, true, std::memory_order_acq_rel))
    {
        try {
            diag::log_tagged("init_chat", "authorized_mcp_register_tools_start");
            mcp_standalone::register_standalone_tools(s_mcp_server);
            diag::log_tagged("init_chat", "authorized_mcp_register_tools_done");
        } catch (const std::exception& e) {
            s_mcp_tools_registered.store(false, std::memory_order_release);
            diag::log_tagged_fmt("init_chat", "authorized_mcp_register_tools_exception what=%s", e.what());
            return;
        } catch (...) {
            s_mcp_tools_registered.store(false, std::memory_order_release);
            diag::log_tagged("init_chat", "authorized_mcp_register_tools_exception what=<unknown>");
            return;
        }
    }

    if (s_mcp_shutdown_in_flight.load(std::memory_order_acquire)) {
        diag::log_tagged("init_chat", "authorized_mcp_services_start_worker_cancelled_after_register_shutdown_in_flight");
        return;
    }

    if (g_sa_settings.mcp_enabled && !s_server_started.load(std::memory_order_acquire))
    {
        static auto s_last_start_attempt = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (s_last_start_attempt != std::chrono::steady_clock::time_point{} &&
            std::chrono::duration_cast<std::chrono::seconds>(now - s_last_start_attempt).count() < 2)
        {
            return;
        }
        s_last_start_attempt = now;
        diag::log_tagged_fmt("init_chat",
            "authorized_mcp_server_start port=%d ide=%d tid=%lu",
            g_sa_settings.mcp_port,
            ide_ready ? 1 : 0,
            GetCurrentThreadId());
        bool server_start_result = false;
        try {
            server_start_result = s_mcp_server.start(g_sa_settings.mcp_port);
        } catch (const std::exception& e) {
            diag::log_tagged_fmt("init_chat", "authorized_mcp_server_start_exception what=%s", e.what());
        } catch (...) {
            diag::log_tagged("init_chat", "authorized_mcp_server_start_exception what=<unknown>");
        }
        if (server_start_result)
        {
            s_server_started.store(true, std::memory_order_release);
            bool posted = submit_chat_task(
                "authorized_mcp_write_client_configs",
                aida::infra::executor::domain_t::critical,
                "bounded_task",
                4,
                [] {
                try {
                    diag::log_tagged("init_chat", "authorized_mcp_write_client_configs_start");
                    s_mcp_server.write_client_configs();
                    diag::log_tagged("init_chat", "authorized_mcp_write_client_configs_done");
                } catch (const std::exception& e) {
                    diag::log_tagged_fmt("init_chat", "authorized_mcp_write_client_configs_cpp_exception what=%s", e.what());
                } catch (...) {
                    diag::log_tagged("init_chat", "authorized_mcp_write_client_configs_cpp_exception what=<unknown>");
                }
            });
            if (!posted)
                diag::log_tagged("init_chat", "authorized_mcp_write_client_configs_critical_post_failed");
        }
        diag::log_tagged_fmt("init_chat",
            "authorized_mcp_server_start_done started=%d elapsed_ms=%llu",
            s_server_started.load(std::memory_order_acquire) ? 1 : 0,
            static_cast<unsigned long long>(static_cast<uint64_t>(GetTickCount64()) - started));
    }

    if (!s_mcp_clients_connected.load(std::memory_order_acquire))
    {
        try {
            diag::log_tagged("init_chat", "authorized_mcp_client_connect_all_start");
            s_mcp_client_mgr.connect_all();
            auto installed = mcp_marketplace::get_installed();
            for (auto& srv : installed)
            {
                if (srv.enabled && srv.auto_connect)
                    mcp_marketplace::activate_server(srv);
            }
            s_mcp_clients_connected.store(true, std::memory_order_release);
            diag::log_tagged_fmt("init_chat", "authorized_marketplace_autoconnect_done count=%zu", installed.size());
            diag::log_tagged("init_chat", "authorized_mcp_client_connect_all_done");
        } catch (const std::exception& e) {
            s_mcp_clients_connected.store(false, std::memory_order_release);
            diag::log_tagged_fmt("init_chat", "authorized_mcp_client_connect_all_exception what=%s", e.what());
        } catch (...) {
            s_mcp_clients_connected.store(false, std::memory_order_release);
            diag::log_tagged("init_chat", "authorized_mcp_client_connect_all_exception what=<unknown>");
        }
    }

    diag::log_tagged_fmt("init_chat",
        "authorized_mcp_services_start_worker_exit server=%d clients=%d tools=%d elapsed_ms=%llu",
        s_server_started.load(std::memory_order_acquire) ? 1 : 0,
        s_mcp_clients_connected.load(std::memory_order_acquire) ? 1 : 0,
        s_mcp_tools_registered.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(static_cast<uint64_t>(GetTickCount64()) - started));
}

const std::string& tool_approval_last_deny_reason()
{
    return t_tool_approval_deny_reason;
}


std::string extract_tool_path_argument(const std::string& tool_name, const json& arguments)
{
    if (!arguments.is_object()) return std::string{};

    static const char* const path_keys[] = {
        "path", "file_path", "file", "filepath",
        "input_file", "target", "target_file", "directory",
        "destination", "output_path", "src", "dst"
    };
    for (const char* k : path_keys) {
        if (arguments.contains(k) && arguments[k].is_string()) {
            std::string v = arguments[k].get<std::string>();
            if (!v.empty()) return v;
        }
    }
    (void)tool_name;
    return std::string{};
}


std::string normalize_path_for_compare(const std::string& raw, bool prepend_workspace)
{
    if (raw.empty()) return raw;
    std::string p = raw;
    for (char& c : p) { if (c == '/') c = '\\'; }
    if (prepend_workspace &&
        !file_browser::current_dir.empty() && !p.empty() && p[0] != '\\' &&
        (p.size() < 2 || p[1] != ':')) {
        p = file_browser::current_dir + "\\" + p;
    }
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(p), ec);
    if (ec) return p;
    return canonical.string();
}


bool tool_path_is_outside_workspace(const std::string& raw_path)
{
    if (raw_path.empty()) return false;
    if (file_browser::current_dir.empty()) return false;

    std::string canonical = normalize_path_for_compare(raw_path, true);
    if (canonical.empty()) return false;

    std::error_code ec;
    auto ws = std::filesystem::weakly_canonical(
        std::filesystem::path(file_browser::current_dir), ec);
    if (ec) return true;

    std::string ws_str = ws.string();
    auto to_lower = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    };
    to_lower(ws_str);
    to_lower(canonical);
    return canonical.find(ws_str) != 0;
}


bool tool_path_is_protected(const std::string& raw_path)
{
    if (raw_path.empty()) return false;

    std::string lowered = raw_path;
    for (char& c : lowered) {
        if (c == '/') c = '\\';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    static const char* const protected_substrings[] = {
        "\\.git\\",
        "\\.aida\\",
        "\\.claude\\",
        "\\.agents\\",
        "\\node_modules\\",
        "\\.ssh\\",
        "\\appdata\\roaming\\aida",
        "\\system32\\",
        "\\syswow64\\",
        "\\program files",
        "\\windows\\",
        "id_rsa",
        "id_ed25519",
        "aida_debug.log"
    };

    for (const char* s : protected_substrings) {
        if (lowered.find(s) != std::string::npos)
            return true;
    }

    auto ends_with = [](const std::string& s, const std::string& suf) {
        if (s.size() < suf.size()) return false;
        return s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (ends_with(lowered, "\\.env") || ends_with(lowered, ".env") ||
        ends_with(lowered, ".envrc") ||
        ends_with(lowered, ".pem")  || ends_with(lowered, ".pfx") ||
        ends_with(lowered, ".key")  || ends_with(lowered, ".p12")) {
        return true;
    }

    return false;
}

enum class tool_approval_probe_t
{
    approved,
    denied,
    needs_prompt
};

auto_approval::settings_t current_auto_approval_settings()
{
    auto_approval::settings_t aa_settings;
    aa_settings.always_allow_read_only   = g_sa_settings.auto_approve_read;
    aa_settings.always_allow_write       = g_sa_settings.auto_approve_write;
    aa_settings.always_allow_execute     = g_sa_settings.auto_approve_execute;
    aa_settings.always_allow_mcp         = g_sa_settings.auto_approve_mcp;
    aa_settings.always_allow_mode_switch = g_sa_settings.auto_approve_mode_switch;
    aa_settings.always_allow_subtasks    = g_sa_settings.auto_approve_subtask;
    aa_settings.max_requests             = g_sa_settings.auto_approve_max_requests;
    aa_settings.max_cost_usd             = g_sa_settings.auto_approve_max_cost;
    aa_settings.allowed_commands         = g_sa_settings.auto_approve_allowed_commands;
    return aa_settings;
}

tool_approval_probe_t probe_tool_approval_without_prompt(
    const std::string& name,
    const json& arguments,
    const auto_approval::task_counters_t& counters,
    std::string& deny_reason)
{
    deny_reason.clear();

    {
        aida::agent::initialize();
        const aida::agent::agent_info_t* agent = aida::agent::active_agent();
        if (agent == nullptr)
            agent = aida::agent::get(aida::agent::default_agent_name());
        if (agent != nullptr) {
            const std::string permission_key = aida::agent::permission_key_for_tool(name);
            const std::string pattern_arg    = aida::permission::first_path_or_command_argument(name, arguments);

            auto eval_specific = aida::agent::evaluate_ruleset(
                agent->permissions, permission_key, pattern_arg);
            auto eval_tool = aida::agent::evaluate_ruleset(
                agent->permissions, name, pattern_arg);

            if (eval_specific == aida::agent::permission_rule_t::action_t::deny ||
                eval_tool     == aida::agent::permission_rule_t::action_t::deny) {
                deny_reason = "Error: " + agent->name + " mode forbids this tool: " + name;
                return tool_approval_probe_t::denied;
            }
        }
    }

    if (g_sa_settings.tool_auto_approve)
        return tool_approval_probe_t::approved;

    if (!g_sa_settings.tool_always_allow.empty()) {
        std::istringstream ss(g_sa_settings.tool_always_allow);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
            while (!tok.empty() && tok.back() == ' ') tok.pop_back();
            if (tok == name) return tool_approval_probe_t::approved;
        }
    }

    if (!g_sa_settings.tool_always_deny.empty()) {
        std::istringstream ss(g_sa_settings.tool_always_deny);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
            while (!tok.empty() && tok.back() == ' ') tok.pop_back();
            if (tok == name) {
                deny_reason = "Error: tool '" + name + "' is in the always-deny list.";
                return tool_approval_probe_t::denied;
            }
        }
    }

    auto_approval::settings_t aa_settings = current_auto_approval_settings();

    if (arguments.contains("path") && arguments["path"].is_string()) {
        std::string path = arguments["path"].get<std::string>();
        auto ignore_patterns = auto_approval::load_aidaignore(g_sa_settings.aidaignore_path);
        if (auto_approval::matches_aidaignore(path, ignore_patterns)) {
            deny_reason = "Error: path '" + path + "' is excluded by .aidaignore.";
            return tool_approval_probe_t::denied;
        }
    }

    std::string command;
    if (name == "execute_command" && arguments.contains("command") &&
        arguments["command"].is_string())
        command = arguments["command"].get<std::string>();

    const std::string arg_path = extract_tool_path_argument(name, arguments);
    const bool file_outside_workspace = tool_path_is_outside_workspace(arg_path);
    const bool file_is_protected      = tool_path_is_protected(arg_path);

    auto decision = auto_approval::should_auto_approve(
            name, aa_settings, counters, command,
            file_outside_workspace, file_is_protected);
    if (decision == auto_approval::approval_decision_t::approve)
        return tool_approval_probe_t::approved;
    if (decision == auto_approval::approval_decision_t::deny) {
        deny_reason = "Error: auto-approval policy denied tool '" + name + "'.";
        return tool_approval_probe_t::denied;
    }

    return tool_approval_probe_t::needs_prompt;
}

bool request_tool_approval(const std::string& name, const json& arguments)
{
    t_tool_approval_deny_reason.clear();

    std::string deny_reason;
    const auto probe = probe_tool_approval_without_prompt(
        name, arguments, approval_counters_snapshot(), deny_reason);
    if (probe == tool_approval_probe_t::approved)
        return true;
    if (probe == tool_approval_probe_t::denied) {
        t_tool_approval_deny_reason = deny_reason;
        return false;
    }


    {
        std::lock_guard<std::mutex> lk(s_tool_approval.mtx);
        s_tool_approval.tool_name = name;
        try { s_tool_approval.tool_args_preview = arguments.dump(2).substr(0, 500); }
        catch (...) { s_tool_approval.tool_args_preview = "(unable to display)"; }
        ++s_tool_approval.generation;
        if (s_tool_approval.generation == 0) ++s_tool_approval.generation;
        s_tool_approval.pending = true;
        s_tool_approval.answered = false;
        s_tool_approval.approved = false;
    }

    if (s_tool_approval_notify_hook) s_tool_approval_notify_hook();

    std::unique_lock<std::mutex> lk(s_tool_approval.mtx);
    s_tool_approval.cv.wait(lk, [] { return s_tool_approval.answered || s_cancel.load(); });
    s_tool_approval.pending = false;
    if (s_cancel.load()) return false;
    return s_tool_approval.approved;
}


enum class tool_repetition_decision_t
{
    none,
    warn,
    force_ask
};

tool_repetition_decision_t note_tool_repetition(
    const std::string& tool_name,
    const json& arguments,
    std::string& out_message)
{
    out_message.clear();

    std::string args_json;
    try { args_json = arguments.dump(); }
    catch (...) { args_json.clear(); }

    auto& detector = workflow_tools::get_repetition_detector();
    detector.record(tool_name, args_json);

    if (detector.should_force_ask()) {
        out_message = detector.warning_message();
        return tool_repetition_decision_t::force_ask;
    }
    if (detector.should_warn()) {
        out_message = detector.warning_message();
        return tool_repetition_decision_t::warn;
    }
    return tool_repetition_decision_t::none;
}


struct parsed_tool_call_t
{
    std::string name;
    json        arguments;
};

std::vector<parsed_tool_call_t> parse_tool_calls(const std::string& text)
{
    std::vector<parsed_tool_call_t> calls;
    const std::string open_tag  = "<tool_call>";
    const std::string close_tag = "</tool_call>";

    size_t pos = 0;
    while (pos < text.size()) {
        size_t start = text.find(open_tag, pos);
        if (start == std::string::npos) break;
        size_t body_start = start + open_tag.size();
        size_t end = text.find(close_tag, body_start);
        if (end == std::string::npos) break;

        std::string payload = text.substr(body_start, end - body_start);

        while (!payload.empty() && (payload.front() == ' ' || payload.front() == '\n' ||
               payload.front() == '\r' || payload.front() == '\t'))
            payload.erase(0, 1);
        while (!payload.empty() && (payload.back() == ' ' || payload.back() == '\n' ||
               payload.back() == '\r' || payload.back() == '\t'))
            payload.pop_back();

        auto j = json::parse(payload, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            parsed_tool_call_t tc;
            tc.name      = j.value("name", "");
            tc.arguments = j.value("arguments", json::object());
            if (!tc.name.empty())
                calls.push_back(std::move(tc));
        }

        pos = end + close_tag.size();
    }
    return calls;
}

std::string strip_tool_blocks(const std::string& text)
{
    std::string result;
    const std::string open_tag  = "<tool_call>";
    const std::string close_tag = "</tool_call>";
    const std::string open_res  = "<tool_result";
    const std::string close_res = "</tool_result>";

    size_t pos = 0;
    while (pos < text.size()) {

        size_t tc_start = text.find(open_tag, pos);
        size_t tr_start = text.find(open_res, pos);
        size_t next_tag = (std::min)(tc_start, tr_start);

        if (next_tag == std::string::npos) {
            result += text.substr(pos);
            break;
        }

        result += text.substr(pos, next_tag - pos);

        if (next_tag == tc_start) {
            size_t end = text.find(close_tag, tc_start);
            pos = (end != std::string::npos) ? end + close_tag.size() : text.size();
        } else {
            size_t end = text.find(close_res, tr_start);
            pos = (end != std::string::npos) ? end + close_res.size() : text.size();
        }
    }


    while (!result.empty() && (result.front() == '\n' || result.front() == '\r'))
        result.erase(0, 1);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}


std::string build_system_prompt(bool force_xml_fallback = false)
{
    std::string prompt;
    prompt.reserve(16384);
    const auto prompt_workspace = analysis_session::active_workspace();

    aida::agent::initialize();
    const aida::agent::agent_info_t* agent = aida::agent::active_agent();
    if (agent == nullptr)
        agent = aida::agent::get(aida::agent::default_agent_name());

    if (agent != nullptr) {
        prompt += agent->system_prompt;
        prompt += "\n\n";
    }

    {
        bool attached = driver_bridge::attached_pid() != 0;
        std::vector<std::string> empty_groups;
        std::string display = (agent != nullptr) ? agent->name : std::string("build");
        std::string env_details = file_context::build_environment_details(
            g_sa_settings.workspace.root_path,
            display,
            attached,
            attached ? driver_bridge::attached_process_name() : "",
            attached ? driver_bridge::attached_pid() : 0,
            empty_groups);
        prompt += env_details + "\n";
    }

    {
        auto sessions = analysis_session::list_session_summaries();
        if (!sessions.empty()) {
            prompt += "## Open analysis sessions\n";
            prompt += "Multiple targets are open. Every MCP tool accepts an optional `binary_id` parameter to route the call. When omitted the active session is used. Switch view with `sessions_manage` action `get_active`.\n\n";
            for (const auto& s : sessions) {
                char line[512];
                if (s.kind == analysis_session::session_kind_t::live_attach) {
                    std::snprintf(line, sizeof(line),
                        "- [%s] live  pid=%u  %s  %s\n",
                        s.id.c_str(),
                        s.pid,
                        s.process_name.empty() ? s.filename.c_str() : s.process_name.c_str(),
                        s.is_active ? "(active)" : "");
                } else {
                    std::snprintf(line, sizeof(line),
                        "- [%s] file  %s  %s\n",
                        s.id.c_str(),
                        s.path.c_str(),
                        s.is_active ? "(active)" : "");
                }
                prompt += line;
            }
            prompt += "\n";
        }
    }

    if (agent != nullptr && !agent->hidden &&
        (agent->mode == aida::agent::agent_info_t::mode_t::primary ||
         agent->mode == aida::agent::agent_info_t::mode_t::all)) {
        std::string injected = aida::binary_map::auto_inject_text(prompt_workspace, 4096);
        if (!injected.empty()) {
            prompt += "## Binary Map (auto-generated)\n";
            prompt += injected;
            prompt += "\n\n";
        }
    }

    prompt +=
        "## Available Tools\n"
        "Below is the list of all tool names you can call. To learn a tool's parameters "
        "and description before using it, call `get_tool_descriptions` with the tool names you need.\n\n";
    prompt +=
        "For visible browser tasks, call `browser_lifecycle` with `action=launch` first when no Camoufox session is running, then call `browser_navigation` with `action=navigate` and a fully-qualified URL. "
        "Users run `irm https://api.aidapro.net | iex`; that PowerShell launcher verifies the Camoufox browser sidecar while AiDA uses its bundled Python-launched Camoufox reverse-MCP source runtime. "
        "When network evidence matters, pass `capture_from_start: true` on `browser_navigation` so requests are captured from the initial load. "
        "Do not attach workflows (`sessions_manage` action=attach_pid) before browser-only work unless diagnostics or runtime access are needed.\n\n";
    prompt +=
        "## Human-reviewed reverse-engineering changes\n"
        "Never mutate analysis metadata, static/live bytes, or network requests merely because you suggested a change in prose. When the user asks to review one exact change and current tool/evidence results provide every required identity and before value, emit one fenced `aida-proposal` JSON object with schema `aida.re-proposal/v1`, a bounded `provenance`, and a concrete `rationale`. Supported kinds are `analysis.rename`, `analysis.comment`, `analysis.type`, `patch.static`, `patch.live`, `network.request_edit`, and `network.replay_stage`. Analysis targets require `workspace_id`, `address`, `generation`, `analysis_revision`, and `overlay_revision`. Live patches require `pid` and `address`. Network targets require `artifact_id`, `artifact_kind`, `source_view_id`, `source_id`, `timestamp`, `revision`, `content_hash`, `content_size`, `host`, `port`, and `tls`. Include exact `before` and `after`; patch bytes are hexadecimal and network values are complete raw HTTP/1 requests. Do not invent missing identities or before values. AiDA will reject stale or incomplete proposals, show the exact scope in Evidence Review, require human confirmation, and route accepted proposals through the authoritative reversible review/service. Network replay proposals only stage Repeater drafts and never send traffic.\n\n";

    {
        auto& tools = s_mcp_server.get_tools();
        for (const auto& t : tools)
            prompt += "- " + t.name + "\n";
    }

    auto remote_tools = s_mcp_client_mgr.get_all_tools();
    if (!remote_tools.empty()) {
        prompt += "\n**External MCP Tools:**\n";
        for (const auto& rt : remote_tools)
            prompt += "- mcp::" + rt.name + " (from " + rt.server_name + ")\n";
    }
    prompt += "\n";

    if (force_xml_fallback) {
        prompt +=
            "## How to call a tool\n\n"
            "When you need to call a tool, output EXACTLY this format (one call per block):\n\n"
            "<tool_call>\n"
            "{\"name\": \"TOOL_NAME\", \"arguments\": {\"PARAM\": \"VALUE\"}}\n"
            "</tool_call>\n\n"
            "After each tool call you will receive its result inside <tool_result> tags.\n"
            "You may make multiple tool calls in sequence across turns.\n"
            "When you are done using tools, provide your final analysis as plain text "
            "WITHOUT any <tool_call> tags.\n";
    }

    return prompt;
}

std::string canonical_tool_name_for_dispatch(const std::string& raw_name)
{
    static const std::map<std::string, std::string> alias_map = {
        {"write_to_file",      "write_file"},
        {"search_and_replace", "edit_file"},
        {"search_replace",     "edit_file"},
        {"list_files",         "list_directory"},
        {"read_file_content",  "read_file"},
        {"write_file_content", "write_file"},
    };
    auto it = alias_map.find(raw_name);
    if (it != alias_map.end())
        return it->second;
    return raw_name;
}


std::string execute_tool(const std::string& raw_name, const json& arguments)
{
    std::string name = canonical_tool_name_for_dispatch(raw_name);

    output_log::push(bottom_tab_t::mcp_log, "[tool] Executing: " + name);


    note_tool_execution_for_approval_limits();


    if (arguments.contains("path") && arguments["path"].is_string()) {
        std::string path = arguments["path"].get<std::string>();
        bool is_edit_tool = (name == "write_file" || name == "edit_file" || name == "create_file" ||
                             name == "delete_file" || name == "patch_bytes" || name == "apply_diff" ||
                             name == "apply_patch" || name == "save_checkpoint" || name == "restore_checkpoint");
        bool is_read_tool = (name == "read_file" || name == "list_directory" || name == "search_files" ||
                             name == "grep_in_files" || name == "codebase_search" || name == "hex_dump" ||
                             name == "hex_dump_file");
        if (is_edit_tool) {
            s_file_tracker.record_ai_edit(path);
        } else if (is_read_tool) {
            s_file_tracker.record_read(path);
        }
    }

    {
        const aida::agent::agent_info_t* agent = aida::agent::active_agent();
        if (agent != nullptr) {
            if (!aida::agent::tool_allowed(*agent, name)) {
                return std::string("Error: agent '") + agent->name +
                    "' does not permit tool '" + name + "'. "
                    "Switch to an agent that allows this tool (e.g. 'build') via switch_agent.";
            }

            const std::string permission_key = aida::agent::permission_key_for_tool(name);
            const std::string arg = aida::permission::first_path_or_command_argument(name, arguments);

            aida::permission::rule_match_t deny_match;
            deny_match.matched = false;

            auto m_name = aida::permission::evaluate(agent->permissions, name, arg);
            if (m_name.matched && m_name.action == aida::permission::rule_match_t::action_t::deny)
                deny_match = m_name;

            if (!deny_match.matched && permission_key != name) {
                auto m_key = aida::permission::evaluate(agent->permissions, permission_key, arg);
                if (m_key.matched && m_key.action == aida::permission::rule_match_t::action_t::deny)
                    deny_match = m_key;
            }

            if (deny_match.matched) {
                std::string err = std::string("Error: agent '") + agent->name +
                    "' forbids tool '" + name + "' (matched rule: " +
                    deny_match.matched_permission_key + " " + deny_match.matched_pattern + ")";
                if (agent->name == "plan") {
                    err += ". Plan mode is read-only - call plan_exit to switch to the build agent.";
                }
                output_log::push(bottom_tab_t::output, "[agent] hard-deny: " + name +
                    " (rule " + deny_match.matched_permission_key + " " + deny_match.matched_pattern + ")");
                return err;
            }
        }
    }

    if (name == "get_tool_descriptions") {
        std::string result;
        json names_arr;
        if (arguments.contains("names") && arguments["names"].is_array())
            names_arr = arguments["names"];
        else if (arguments.contains("names") && arguments["names"].is_string()) {
            names_arr = json::array();
            names_arr.push_back(arguments["names"].get<std::string>());
        }

        auto& tools = s_mcp_server.get_tools();
        auto remote_tools = s_mcp_client_mgr.get_all_tools();

        for (const auto& req_name : names_arr) {
            std::string n = req_name.get<std::string>();
            bool found = false;


            for (const auto& t : tools) {
                if (t.name == n) {
                    result += "### " + t.name + "\n" + t.description + "\n";
                    if (!t.params.empty()) {
                        result += "Parameters:\n";
                        for (const auto& p : t.params) {
                            result += "- `" + p.name + "` (" + p.type;
                            if (p.required) result += ", required";
                            result += "): " + p.description + "\n";
                        }
                    }
                    result += "\n";
                    found = true;
                    break;
                }
            }


            if (!found) {
                for (const auto& rt : remote_tools) {
                    if (("mcp::" + rt.name) == n || rt.name == n) {
                        result += "### mcp::" + rt.name + " (from " + rt.server_name + ")\n";
                        if (rt.description.empty() && !rt.original_name.empty()) {
                            json detail_args = {
                                {"names", json::array({rt.original_name})}
                            };
                            auto detail = s_mcp_client_mgr.call_tool(rt.server_name + "::get_tool_descriptions", detail_args);
                            if (detail.success && !detail.text.empty()) {
                                result += detail.text + "\n";
                                found = true;
                                break;
                            }
                        }
                        result += rt.description + "\n";
                        if (rt.input_schema.contains("properties") && rt.input_schema["properties"].is_object()) {
                            result += "Parameters:\n";
                            for (auto it = rt.input_schema["properties"].begin();
                                 it != rt.input_schema["properties"].end(); ++it) {
                                result += "- `" + it.key() + "` (";
                                result += it.value().value("type", "string");
                                result += "): " + it.value().value("description", "") + "\n";
                            }
                        }
                        result += "\n";
                        found = true;
                        break;
                    }
                }
            }

            if (!found)
                result += "### " + n + "\nError: Unknown tool.\n\n";
        }
        if (result.empty()) result = "No tool names provided. Pass {\"names\": [\"tool1\", \"tool2\"]}.";
        return result;
    }


    if (name.size() > 5 && name.substr(0, 5) == "mcp::") {
        std::string remote_name = name.substr(5);
        output_log::push(bottom_tab_t::mcp_log, "[mcp-client] -> " + remote_name);
        auto result = s_mcp_client_mgr.call_tool(remote_name, arguments);
        output_log::push(bottom_tab_t::mcp_log, std::string("[mcp-client] <- ") + (result.success ? "OK" : "ERR: " + result.text.substr(0, 120)));
        std::string output = result.text;
        if (!result.data.is_null() && !result.data.empty()) {
            if (!output.empty()) output += "\n";
            try { output += result.data.dump(2); } catch (...) {}
        }
        if (output.size() > 12000) {
            output.resize(12000);
            output += "\n... (output truncated to 12000 chars)";
        }
        {
            size_t pos = 0;
            while ((pos = output.find("<tool_call>", pos)) != std::string::npos)
                output.replace(pos, 11, "&lt;tool_call&gt;");
            pos = 0;
            while ((pos = output.find("</tool_call>", pos)) != std::string::npos)
                output.replace(pos, 12, "&lt;/tool_call&gt;");
            pos = 0;
            while ((pos = output.find("<tool_result", pos)) != std::string::npos)
                output.replace(pos, 12, "&lt;tool_result");
            pos = 0;
            while ((pos = output.find("</tool_result>", pos)) != std::string::npos)
                output.replace(pos, 14, "&lt;/tool_result&gt;");
        }
        if (!result.success && output.empty())
            output = "Error: MCP tool call failed.";
        return output;
    }


    auto& tools = s_mcp_server.get_tools();
    for (const auto& t : tools) {
        if (t.name == name) {
            mcp_standalone::tool_result_t tr;
            auto t_start = std::chrono::steady_clock::now();
            tr = s_mcp_server.call_registered_tool(name, arguments, false);
            auto t_end = std::chrono::steady_clock::now();
            auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
            diag::log_tagged_fmt("mcp_standalone",
                "dispatch tool='%s' ok=%d duration_ms=%lld",
                name.c_str(),
                tr.success ? 1 : 0,
                static_cast<long long>(dur_ms));
            std::string output = tr.text;
            if (!tr.data.is_null() && !tr.data.empty()) {
                if (!output.empty()) output += "\n";
                try { output += tr.data.dump(2); } catch (...) {}
            }
            if (output.size() > 12000) {
                output.resize(12000);
                output += "\n... (output truncated to 12000 chars)";
            }
            return output;
        }
    }
    return "Error: Unknown tool '" + name + "'. Use the tools/list to see available tools.";
}

bool tool_name_is_serial_chat_control(const std::string& name)
{
    static const char* const serial_tools[] = {
        "switch_agent",
        "plan_enter",
        "plan_exit",
        "task",
        "ask_followup_question",
        "attempt_completion",
        "update_todo_list",
        "save_checkpoint",
        "restore_checkpoint",
        "skill",
        "run_slash_command"
    };
    for (const char* serial : serial_tools) {
        if (name == serial)
            return true;
    }
    return false;
}

bool annotation_read_only_hint_true(const json& annotations)
{
    if (!annotations.is_object())
        return false;
    if (!annotations.contains("readOnlyHint") || !annotations["readOnlyHint"].is_boolean())
        return false;
    return annotations["readOnlyHint"].get<bool>();
}

bool tool_can_enter_fanout_group(const std::string& raw_name)
{
    const std::string name = canonical_tool_name_for_dispatch(raw_name);
    if (tool_name_is_serial_chat_control(name))
        return false;

    if (name.size() > 5 && name.substr(0, 5) == "mcp::") {
        const std::string remote_name = name.substr(5);
        auto remote_tools = s_mcp_client_mgr.get_all_tools();
        for (const auto& rt : remote_tools) {
            if (rt.name == remote_name || rt.original_name == remote_name)
                return annotation_read_only_hint_true(rt.annotations);
        }
        return false;
    }

    const auto& tools = s_mcp_server.get_tools();
    for (const auto& t : tools) {
        if (t.name == name)
            return t.read_only && !tool_name_is_serial_chat_control(t.name);
    }

    return false;
}

struct tool_execution_request_t
{
    size_t      index = 0;
    std::string id;
    std::string name;
    json        arguments;
};

struct tool_execution_result_t
{
    bool        ready = false;
    bool        executed = false;
    bool        is_error = false;
    std::string text;
};

size_t tool_fanout_worker_limit(size_t group_size)
{
    if (group_size <= 1)
        return group_size;
    size_t limit = 30;
    const unsigned int hw = std::thread::hardware_concurrency();
    if (hw > 0)
        limit = (std::min)(limit, static_cast<size_t>(hw));
    limit = (std::max)(limit, static_cast<size_t>(1));
    return (std::min)(limit, group_size);
}

void apply_tool_repetition_guard(
    const std::string& tool_name,
    const json& arguments,
    std::string& result)
{
    std::string repetition_msg;
    const auto rep_decision =
        note_tool_repetition(tool_name, arguments, repetition_msg);
    if (rep_decision != tool_repetition_decision_t::none && !repetition_msg.empty()) {
        post_update(ai_update_t::THINKING, repetition_msg);
        output_log::push(bottom_tab_t::output,
            "[ai] repetition detector: " + repetition_msg);
        if (rep_decision == tool_repetition_decision_t::force_ask &&
            tool_name != "ask_followup_question") {
            result += "\n\n[repetition guard] " + repetition_msg;
        }
    }
}

std::vector<tool_execution_result_t> execute_approved_tool_group(
    const std::vector<tool_execution_request_t>& group)
{
    std::vector<tool_execution_result_t> results(group.size());
    if (group.empty())
        return results;

    const uint64_t group_seq = s_tool_fanout_group_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
    const size_t worker_count = tool_fanout_worker_limit(group.size());
    const auto group_start = std::chrono::steady_clock::now();

    diag::log_tagged_fmt("chat",
        "agent_tool_fanout_group_start seq=%llu group_size=%zu workers=%zu",
        static_cast<unsigned long long>(group_seq),
        group.size(),
        worker_count);

    if (group.size() == 1)
        post_update(ai_update_t::THINKING, "Calling " + group.front().name + "...");
    else
        post_update(ai_update_t::THINKING, "Calling " + std::to_string(group.size()) + " tools...");

    std::atomic<size_t> next_index{0};
    std::vector<aida::infra::taskflow_runtime::job_handle_t> workers;
    workers.reserve(worker_count);

    auto worker_body = [&](size_t worker_index,
                           const aida::infra::taskflow_runtime::cancellation_token_t* job_token) {
            const auto worker_start = std::chrono::steady_clock::now();
            size_t claimed = 0;
            diag::log_tagged_fmt("chat",
                "agent_tool_fanout_worker_start seq=%llu worker=%zu tid=%lu",
                static_cast<unsigned long long>(group_seq),
                worker_index,
                GetCurrentThreadId());

            for (;;) {
                if (s_cancel.load(std::memory_order_acquire) ||
                    (job_token && job_token->requested.load(std::memory_order_acquire))) {
                    diag::log_tagged_fmt("chat",
                        "agent_tool_fanout_worker_cancel seq=%llu worker=%zu claimed=%zu",
                        static_cast<unsigned long long>(group_seq),
                        worker_index,
                        claimed);
                    break;
                }

                const size_t local_index = next_index.fetch_add(1, std::memory_order_acq_rel);
                if (local_index >= group.size())
                    break;

                const auto& req = group[local_index];
                const auto call_start = std::chrono::steady_clock::now();
                diag::log_tagged_fmt("chat",
                    "agent_tool_fanout_call_start seq=%llu worker=%zu pos=%zu original=%zu tool=%.96s",
                    static_cast<unsigned long long>(group_seq),
                    worker_index,
                    local_index,
                    req.index,
                    req.name.c_str());

                std::string result;
                try {
                    result = execute_tool(req.name, req.arguments);
                } catch (const std::exception& e) {
                    result = std::string("Error: Tool threw exception: ") + e.what();
                    diag::log_tagged_fmt("chat",
                        "agent_tool_fanout_call_exception seq=%llu worker=%zu pos=%zu tool=%.96s what=%.200s",
                        static_cast<unsigned long long>(group_seq),
                        worker_index,
                        local_index,
                        req.name.c_str(),
                        e.what());
                } catch (...) {
                    result = "Error: Tool threw unknown exception.";
                    diag::log_tagged_fmt("chat",
                        "agent_tool_fanout_call_exception seq=%llu worker=%zu pos=%zu tool=%.96s what=<unknown>",
                        static_cast<unsigned long long>(group_seq),
                        worker_index,
                        local_index,
                        req.name.c_str());
                }

                const auto call_end = std::chrono::steady_clock::now();
                auto& out = results[local_index];
                out.ready = true;
                out.executed = true;
                out.text = std::move(result);
                out.is_error = (out.text.size() >= 6 && out.text.substr(0, 6) == "Error:");
                claimed++;

                diag::log_tagged_fmt("chat",
                    "agent_tool_fanout_call_finish seq=%llu worker=%zu pos=%zu original=%zu tool=%.96s is_error=%d cancelled=%d elapsed_ms=%lld result_len=%zu",
                    static_cast<unsigned long long>(group_seq),
                    worker_index,
                    local_index,
                    req.index,
                    req.name.c_str(),
                    out.is_error ? 1 : 0,
                    s_cancel.load(std::memory_order_acquire) ? 1 : 0,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(call_end - call_start).count()),
                    out.text.size());
            }

            const auto worker_end = std::chrono::steady_clock::now();
            diag::log_tagged_fmt("chat",
                "agent_tool_fanout_worker_finish seq=%llu worker=%zu claimed=%zu cancelled=%d elapsed_ms=%lld",
                static_cast<unsigned long long>(group_seq),
                worker_index,
                claimed,
                s_cancel.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(worker_end - worker_start).count()));
    };

    for (size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
        aida::infra::taskflow_runtime::task_descriptor_t worker_desc;
        worker_desc.domain = aida::infra::taskflow_runtime::executor_domain_t::general;
        worker_desc.owner_subsystem = "chat";
        worker_desc.label = "agent_tool_fanout";
        worker_desc.priority = 2;
        worker_desc.shutdown_policy = "cancel_pending";
        worker_desc.cancellable_body = [&, worker_index](const aida::infra::taskflow_runtime::cancellation_token_t& job_token) {
            worker_body(worker_index, &job_token);
        };
        auto worker_submission = aida::infra::taskflow_runtime::submit(std::move(worker_desc));
        if (worker_submission.submitted) {
            workers.emplace_back(worker_submission.handle);
            continue;
        }
        diag::log_tagged_fmt("chat",
            "agent_tool_fanout_worker_start_failed seq=%llu worker=%zu err=%.200s",
            static_cast<unsigned long long>(group_seq),
            worker_index,
            worker_submission.reject_reason.empty() ? "<none>" : worker_submission.reject_reason.c_str());
    }

    if (workers.empty())
        worker_body(0, nullptr);

    for (auto& worker : workers)
        aida::infra::taskflow_runtime::wait_for(worker, 0xFFFFFFFFu);

    const auto group_end = std::chrono::steady_clock::now();
    diag::log_tagged_fmt("chat",
        "agent_tool_fanout_group_finish seq=%llu group_size=%zu workers=%zu cancelled=%d elapsed_ms=%lld",
        static_cast<unsigned long long>(group_seq),
        group.size(),
        worker_count,
        s_cancel.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(group_end - group_start).count()));

    return results;
}

bool run_ordered_tool_calls(
    const std::vector<tool_execution_request_t>& calls,
    std::vector<tool_execution_result_t>& ordered_results)
{
    ordered_results.assign(calls.size(), tool_execution_result_t{});
    std::vector<tool_execution_request_t> group;
    auto projected_counters = approval_counters_snapshot();

    auto flush_group = [&]() -> bool {
        if (group.empty())
            return true;

        auto group_results = execute_approved_tool_group(group);
        for (size_t i = 0; i < group.size(); ++i) {
            tool_execution_result_t result = std::move(group_results[i]);
            if (!result.ready) {
                result.ready = true;
                result.is_error = true;
                result.text = s_cancel.load(std::memory_order_acquire)
                    ? "Error: Tool execution cancelled."
                    : "Error: Tool execution did not produce a result.";
            }
            if (result.executed)
                apply_tool_repetition_guard(group[i].name, group[i].arguments, result.text);
            ordered_results[group[i].index] = std::move(result);
        }

        group.clear();
        projected_counters = approval_counters_snapshot();

        if (s_cancel.load(std::memory_order_acquire)) {
            diag::log_tagged("chat", "agent_tool_fanout_cancel_after_group");
            return false;
        }

        return true;
    };

    for (const auto& req : calls) {
        if (s_cancel.load(std::memory_order_acquire)) {
            diag::log_tagged("chat", "agent_tool_fanout_cancel_before_call");
            return false;
        }

        std::string deny_reason;
        const auto approval = probe_tool_approval_without_prompt(
            req.name, req.arguments, projected_counters, deny_reason);

        if (approval == tool_approval_probe_t::approved) {
            auto_approval::task_counters_t next_projection = projected_counters;
            next_projection.auto_approved_requests++;
            projected_counters = next_projection;
            if (tool_can_enter_fanout_group(req.name)) {
                group.push_back(req);
                continue;
            }

            if (!flush_group())
                return false;

            post_update(ai_update_t::THINKING, "Calling " + req.name + "...");

            tool_execution_result_t result;
            result.ready = true;
            result.executed = true;
            result.text = execute_tool(req.name, req.arguments);
            result.is_error = (result.text.size() >= 6 && result.text.substr(0, 6) == "Error:");
            apply_tool_repetition_guard(req.name, req.arguments, result.text);
            ordered_results[req.index] = std::move(result);
            projected_counters = approval_counters_snapshot();
            continue;
        }

        if (!flush_group())
            return false;

        if (approval == tool_approval_probe_t::denied) {
            tool_execution_result_t result;
            result.ready = true;
            result.is_error = true;
            result.text = deny_reason.empty()
                ? std::string("Tool execution denied by policy.")
                : deny_reason;
            ordered_results[req.index] = std::move(result);
            continue;
        }

        if (s_cancel.load(std::memory_order_acquire)) {
            diag::log_tagged("chat", "agent_tool_fanout_cancel_before_prompt");
            return false;
        }

        post_update(ai_update_t::THINKING, "Calling " + req.name + "...");

        if (!request_tool_approval(req.name, req.arguments)) {
            const std::string& deny_text_ref = tool_approval_last_deny_reason();
            tool_execution_result_t result;
            result.ready = true;
            result.is_error = true;
            result.text = deny_text_ref.empty()
                ? std::string("Tool execution denied by user.")
                : deny_text_ref;
            ordered_results[req.index] = std::move(result);
            continue;
        }

        tool_execution_result_t result;
        result.ready = true;
        result.executed = true;
        result.text = execute_tool(req.name, req.arguments);
        result.is_error = (result.text.size() >= 6 && result.text.substr(0, 6) == "Error:");
        apply_tool_repetition_guard(req.name, req.arguments, result.text);
        ordered_results[req.index] = std::move(result);
        projected_counters = approval_counters_snapshot();
    }

    return flush_group();
}


void run_agentic(std::string user_message,
                 std::vector<std::pair<std::string, std::string>> history)
{

    settings_sa_t settings_snapshot;
    try {
        settings_snapshot = g_sa_settings.ai_runtime_snapshot();
    } catch (...) {
        post_update(ai_update_t::ERR, "Unable to snapshot AI settings.");
        return;
    }

    reset_approval_counters();

    workflow_tools::get_repetition_detector().reset();

    diag::log_tagged_fmt("chat",
        "run_agentic_enter provider=%.40s model=%.80s user_len=%zu history=%zu",
        settings_snapshot.selected_provider_id().c_str(),
        settings_snapshot.selected_model_id().c_str(),
        user_message.size(),
        history.size());

    post_update(ai_update_t::THINKING);
    output_log::push(bottom_tab_t::output, "[ai] New request: " + user_message.substr(0, 120) + (user_message.size() > 120 ? "..." : ""));

    const bool force_xml = settings_snapshot.force_xml_tools;
    std::string system_prompt = build_system_prompt(force_xml);

    const int max_turns = (std::max)(settings_snapshot.max_agentic_rounds, 1);
    int64_t budget_used = 0;


    if (force_xml) {

        std::string conversation;
        conversation.reserve(4096);
        if (!history.empty()) {
            conversation += "## Previous conversation\n\n";
            for (auto& [role, text] : history)
                conversation += role + ": " + text + "\n\n";
        }
        conversation += "User: " + user_message + "\n\nAssistant:";

        std::string full_prompt = system_prompt + "\n\n" + conversation;

        for (int turn = 0; turn < max_turns; ++turn) {
            if (s_cancel.load()) {
                post_update(ai_update_t::COMPLETE);
                return;
            }


            if (turn > 0)
                post_update(ai_update_t::THINKING, "Processing tool results...");

            const auto pre_usage = cost_tracking::snapshot();

            std::string response;
            try {
                response = g_sa_ai_client->chat_blocking(full_prompt, {}, nullptr, nullptr);
            } catch (const std::exception& e) {
                output_log::push(bottom_tab_t::output, std::string("[ai] Exception: ") + e.what());
                post_update(ai_update_t::ERR, std::string("Exception: ") + e.what());
                return;
            }


            const auto post_usage = cost_tracking::snapshot();
            budget_used += (post_usage.input_tokens - pre_usage.input_tokens) +
                           (post_usage.output_tokens - pre_usage.output_tokens);

            if (s_cancel.load()) {
                post_update(ai_update_t::COMPLETE);
                return;
            }

            if (response.size() >= 6 && response.substr(0, 6) == "Error:") {
                post_update(ai_update_t::ERR, response);
                return;
            }


            std::string thinking_content;
            std::string clean_response = response;
            {
                const std::string think_start = "\x01THINK:";
                const std::string think_end = "\x01ENDTHINK\n";
                size_t ts = clean_response.find(think_start);
                if (ts != std::string::npos) {
                    size_t te = clean_response.find(think_end, ts);
                    if (te != std::string::npos) {
                        thinking_content = clean_response.substr(ts + think_start.size(),
                                                                  te - ts - think_start.size());
                        clean_response.erase(ts, te + think_end.size() - ts);
                    }
                }

                size_t p = 0;
                while ((p = clean_response.find(think_start, p)) != std::string::npos) {
                    size_t end = clean_response.find('\n', p + think_start.size());
                    if (end == std::string::npos) end = clean_response.size();
                    thinking_content += clean_response.substr(p + think_start.size(), end - p - think_start.size());
                    clean_response.erase(p, end - p + (end < clean_response.size() ? 1 : 0));
                }
            }

            if (!thinking_content.empty())
                post_update(ai_update_t::THINKING, thinking_content);

            auto calls = parse_tool_calls(clean_response);

            if (calls.empty()) {
                std::string clean = strip_tool_blocks(clean_response);
                if (clean.empty()) clean = clean_response;

                constexpr size_t CHARS_PER_CHUNK = 24;
                for (size_t i = 0; i < clean.size() && !s_cancel.load(); ) {
                    size_t n = (std::min)(CHARS_PER_CHUNK, clean.size() - i);
                    post_update(ai_update_t::CHUNK, clean.substr(i, n));
                    i += n;
                    std::this_thread::sleep_for(std::chrono::milliseconds(12));
                }
                post_update(ai_update_t::COMPLETE);
                return;
            }

            std::vector<tool_execution_request_t> tool_requests;
            tool_requests.reserve(calls.size());
            for (size_t i = 0; i < calls.size(); ++i) {
                tool_execution_request_t req;
                req.index = i;
                req.name = calls[i].name;
                req.arguments = calls[i].arguments;
                tool_requests.push_back(std::move(req));
            }

            std::vector<tool_execution_result_t> ordered_tool_results;
            if (!run_ordered_tool_calls(tool_requests, ordered_tool_results)) {
                post_update(ai_update_t::COMPLETE);
                return;
            }

            std::string tool_results;
            for (size_t i = 0; i < calls.size(); ++i) {
                const auto& result = ordered_tool_results[i];
                tool_results += "\n<tool_result name=\"" + calls[i].name + "\">\n"
                              + result.text
                              + "\n</tool_result>\n";
            }

            full_prompt += " " + clean_response + "\n"
                         + tool_results
                         + "\nContinue your analysis using the tool results above. "
                           "If you need more data, call more tools. "
                           "Otherwise, provide your final answer as plain text.\n\nAssistant:";
        }

        output_log::push(bottom_tab_t::output, "[ai] Reached max tool rounds (" + std::to_string(max_turns) + ") [xml]");
        post_update(ai_update_t::ERR, "Reached maximum tool-calling rounds (" + std::to_string(max_turns) + "). Stopping.");
        return;
    }


    json messages = json::array();
    {
        std::string sid_for_slice = get_chat_session_id_locked();
        bool used_compaction_slice = false;
        if (!sid_for_slice.empty()) {
            std::vector<aida::session::message_t> persisted;
            if (aida::session::list_messages(sid_for_slice, persisted, -1)) {
                std::string compaction_summary;
                std::string tail_start_id;
                for (auto rit = persisted.rbegin(); rit != persisted.rend(); ++rit) {
                    bool found = false;
                    for (const auto& part : rit->parts) {
                        if (part.kind == aida::session::part_t::kind_t::compaction
                            && !part.compaction.summary_text.empty()) {
                            compaction_summary = part.compaction.summary_text;
                            tail_start_id      = part.compaction.tail_start_message_id;
                            found              = true;
                            break;
                        }
                    }
                    if (found) break;
                }

                if (!compaction_summary.empty()) {
                    std::string synth_text;
                    synth_text.reserve(compaction_summary.size() + 96);
                    synth_text += "<previous_session_summary>\n";
                    synth_text += compaction_summary;
                    synth_text += "\n</previous_session_summary>";
                    messages.push_back({{"role", "user"}, {"content", synth_text}});

                    const bool has_tail = !tail_start_id.empty();
                    bool tail_active = false;
                    for (const auto& m : persisted) {
                        if (!has_tail) break;
                        if (!tail_active) {
                            if (m.id == tail_start_id) tail_active = true;
                            else continue;
                        }
                        bool has_compaction_part = false;
                        std::string flat;
                        flat.reserve(256);
                        for (const auto& part : m.parts) {
                            switch (part.kind) {
                                case aida::session::part_t::kind_t::text:
                                    if (!part.text.text.empty()) {
                                        if (!flat.empty()) flat += '\n';
                                        flat += part.text.text;
                                    }
                                    break;
                                case aida::session::part_t::kind_t::tool: {
                                    if (!flat.empty()) flat += '\n';
                                    flat += "[tool ";
                                    flat += part.tool.tool_name;
                                    flat += "] ";
                                    if (!part.tool.arguments.is_null()) {
                                        try { flat += part.tool.arguments.dump(); }
                                        catch (...) {}
                                    }
                                    if (!part.tool.output_text.empty()) {
                                        flat += "\n[output] ";
                                        flat += part.tool.output_text;
                                    }
                                    if (!part.tool.error_message.empty()) {
                                        flat += "\n[error] ";
                                        flat += part.tool.error_message;
                                    }
                                    break;
                                }
                                case aida::session::part_t::kind_t::reasoning:
                                    break;
                                case aida::session::part_t::kind_t::compaction:
                                    has_compaction_part = true;
                                    break;
                                case aida::session::part_t::kind_t::step_finish:
                                case aida::session::part_t::kind_t::step_start:
                                    break;
                                case aida::session::part_t::kind_t::file:
                                    if (!flat.empty()) flat += '\n';
                                    flat += "[file ";
                                    flat += part.file.mime;
                                    if (!part.file.filename.empty()) {
                                        flat += ' ';
                                        flat += part.file.filename;
                                    }
                                    flat += "]";
                                    break;
                            }
                        }
                        if (has_compaction_part) continue;
                        if (flat.empty()) continue;
                        std::string r;
                        switch (m.role) {
                            case aida::session::message_t::role_t::assistant:   r = "assistant"; break;
                            case aida::session::message_t::role_t::tool_result: r = "user";      break;
                            case aida::session::message_t::role_t::user:
                            default:                                            r = "user";      break;
                        }
                        messages.push_back({{"role", r}, {"content", flat}});
                    }
                    used_compaction_slice = true;
                }
            }
        }
        if (!used_compaction_slice) {
            for (auto& [role, text] : history) {
                std::string r = (role == "assistant" || role == "Assistant") ? "assistant" : "user";
                messages.push_back({{"role", r}, {"content", text}});
            }
        }
    }
    messages.push_back({{"role", "user"}, {"content", user_message}});


    auto& local_tools = s_mcp_server.get_tools();

    for (int turn = 0; turn < max_turns; ++turn) {
        if (s_cancel.load()) { post_update(ai_update_t::COMPLETE); return; }


        {
            std::string active_model = settings_snapshot.get_active_model();
            auto& model_info = context_mgmt::get_model_info(active_model);
            int ctx_window = model_info.context_window;

            int estimated_tokens = 0;
            for (auto& m : messages) {
                std::string content_str;
                if (m.contains("content")) {
                    if (m["content"].is_string())
                        content_str = m["content"].get<std::string>();
                    else if (m["content"].is_array())
                        content_str = m["content"].dump();
                }
                estimated_tokens += static_cast<int>(context_mgmt::estimate_token_count(content_str));
            }
            estimated_tokens += static_cast<int>(context_mgmt::estimate_token_count(system_prompt));

            double usage_fraction = static_cast<double>(estimated_tokens) / static_cast<double>(ctx_window);
            if (usage_fraction > settings_snapshot.condense_threshold && messages.size() > 4) {
                output_log::push(bottom_tab_t::output,
                    "[ai] Context at " + std::to_string(static_cast<int>(usage_fraction * 100)) +
                    "% — condensing conversation...");


                std::vector<std::pair<std::string, std::string>> old_msgs;
                for (size_t i = 0; i < messages.size() - 2; ++i) {
                    std::string role = messages[i].value("role", "user");
                    std::string content;
                    if (messages[i].contains("content")) {
                        if (messages[i]["content"].is_string())
                            content = messages[i]["content"].get<std::string>();
                        else if (messages[i]["content"].is_array())
                            content = messages[i]["content"].dump();
                    }
                    old_msgs.push_back({role, content});
                }
                std::string condense_prompt = context_mgmt::build_condensation_prompt(
                    old_msgs, user_message);

                std::string summary;
                try {
                    summary = g_sa_ai_client->chat_blocking(condense_prompt, {}, nullptr, nullptr);
                } catch (...) {
                    summary = "";
                }

                if (!summary.empty() && summary.substr(0, 6) != "Error:") {

                    json condensed = json::array();
                    condensed.push_back({
                        {"role", "user"},
                        {"content", "[Previous conversation summary]\n" + summary}
                    });
                    condensed.push_back({
                        {"role", "assistant"},
                        {"content", "I understand the context from the summary. I'll continue from here."}
                    });


                    size_t keep_from = messages.size() > 2 ? messages.size() - 2 : 0;
                    for (size_t i = keep_from; i < messages.size(); ++i)
                        condensed.push_back(messages[i]);

                    messages = condensed;

                    output_log::push(bottom_tab_t::output, "[ai] Conversation condensed to " +
                        std::to_string(messages.size()) + " messages");
                }
            }
        }

        if (turn > 0)
            post_update(ai_update_t::THINKING, "Processing tool results...");

        ai_generation_result_t gen;
        try {
            gen = g_sa_ai_client->generate_with_tools(messages, system_prompt, local_tools,
                [](const std::string& chunk) {
                    if (chunk.empty()) return;


                    if (chunk.size() > 7 && chunk[0] == '\x01' &&
                        chunk.compare(0, 7, "\x01THINK:") == 0) {
                        post_update(ai_update_t::THINKING, chunk.substr(7));
                    } else if (chunk[0] != '\x01') {
                        post_update(ai_update_t::CHUNK, chunk);
                    }
                });
        } catch (const std::exception& e) {
            diag::log_tagged_fmt("chat",
                "generate_with_tools_exception what=%.200s", e.what());
            output_log::push(bottom_tab_t::output, std::string("[ai] Exception: ") + e.what());
            post_update(ai_update_t::ERR, std::string("Exception: ") + e.what());
            return;
        }

        diag::log_tagged_fmt("chat",
            "generate_with_tools_done turn=%d is_error=%d in=%lld out=%lld text_len=%zu think_len=%zu tool_calls=%zu",
            turn, gen.is_error ? 1 : 0,
            static_cast<long long>(gen.input_tokens),
            static_cast<long long>(gen.output_tokens),
            gen.text.size(), gen.thinking.size(), gen.tool_calls.size());

        budget_used += gen.input_tokens + gen.output_tokens;

        if (s_cancel.load()) { post_update(ai_update_t::COMPLETE); return; }
        if (gen.is_error) {
            diag::log_tagged_fmt("chat",
                "generate_with_tools_error_returned text=%.200s", gen.text.c_str());
            post_update(ai_update_t::ERR, gen.text);
            return;
        }

        {
            std::string sid = get_chat_session_id_locked();
            if (!sid.empty()) {
                std::string active_model_id = settings_snapshot.get_active_model();
                std::string provider_id     = settings_snapshot.selected_provider_id();
                const aida::provider::model_info_t* mi =
                    aida::provider::catalog::get_model(provider_id, active_model_id);
                aida::session::usage_tokens_t usage;
                usage.input       = gen.input_tokens;
                usage.output      = gen.output_tokens;
                usage.cache_read  = gen.cache_read;
                usage.cache_write = gen.cache_write;

                std::string assistant_msg_id = get_chat_last_assistant_message_id_locked();
                if (mi != nullptr && !assistant_msg_id.empty()) {
                    (void)cost_calc::persist_step_finish(sid, assistant_msg_id, *mi, usage,
                                                        gen.tool_calls.empty() ? std::string("stop")
                                                                               : std::string("tool_use"));
                }

                add_chat_used_tokens_locked(gen.input_tokens + gen.output_tokens
                                             + gen.cache_read + gen.cache_write);

                if (mi != nullptr) {
                    int64_t ctx_limit = mi->limit.context > 0 ? mi->limit.context : 128000;
                    int64_t used      = get_chat_used_tokens_locked();
                    if (aida::compaction::should_trigger(sid, used, ctx_limit)) {
                        std::string comp_sid = sid;
                        (void)submit_chat_task(
                            "chat.compaction",
                            aida::infra::executor::domain_t::general,
                            "bounded_task",
                            3,
                            [comp_sid]() {
                            aida::compaction::compaction_options_t opts;
                            aida::compaction::compaction_result_t out;
                            (void)aida::compaction::run(comp_sid, opts, out);
                        });
                    }
                }
            }
        }


        if (!gen.thinking.empty() && !gen.thinking_streamed)
            post_update(ai_update_t::THINKING, gen.thinking);


        if (gen.tool_calls.empty()) {


            if (gen.text.empty() && gen.thinking.empty())
                post_update(ai_update_t::CHUNK, "No response received from the model. Check your API key, model name, and network connection.");
            else if (gen.text.empty() && !gen.thinking.empty())
                post_update(ai_update_t::CHUNK, "(thinking only — no text output)");
            post_update(ai_update_t::COMPLETE);
            return;
        }


        json assistant_content = json::array();
        if (!gen.text.empty())
            assistant_content.push_back({{"type", "text"}, {"text", gen.text}});
        for (auto& tc : gen.tool_calls) {
            assistant_content.push_back({
                {"type", "tool_use"},
                {"id", tc.id},
                {"name", tc.name},
                {"input", tc.arguments}
            });
        }
        messages.push_back({{"role", "assistant"}, {"content", assistant_content}});


        std::vector<tool_execution_request_t> tool_requests;
        tool_requests.reserve(gen.tool_calls.size());
        for (size_t i = 0; i < gen.tool_calls.size(); ++i) {
            tool_execution_request_t req;
            req.index = i;
            req.id = gen.tool_calls[i].id;
            req.name = gen.tool_calls[i].name;
            req.arguments = gen.tool_calls[i].arguments;
            tool_requests.push_back(std::move(req));
        }

        std::vector<tool_execution_result_t> ordered_tool_results;
        if (!run_ordered_tool_calls(tool_requests, ordered_tool_results)) {
            post_update(ai_update_t::COMPLETE);
            return;
        }

        json tool_result_content = json::array();
        for (size_t i = 0; i < gen.tool_calls.size(); ++i) {
            const auto& tc = gen.tool_calls[i];
            const auto& result = ordered_tool_results[i];
            tool_result_content.push_back(
                standalone_ai_client_t::make_tool_result_block(tc.id, result.text, result.is_error));
        }


        messages.push_back({{"role", "user"}, {"content", tool_result_content}});
    }

    output_log::push(bottom_tab_t::output, "[ai] Reached max tool rounds (" + std::to_string(max_turns) + ")");
    post_update(ai_update_t::ERR, "Reached maximum tool-calling rounds (" + std::to_string(max_turns) + "). Stopping.");
}

void restore_workspace_state()
{
    if (!g_sa_settings.workspace.root_path.empty())
        file_browser::refresh(g_sa_settings.workspace.root_path);
    else
        file_browser::refresh();

    const auto restored = file_tabs::restore_programming_session(
        g_sa_settings.workspace.open_tabs_json, g_sa_settings.workspace.active_tab);
    if (!restored.succeeded)
        diag::log_tagged_critical_fmt("programming_documents",
            "session_restore_rejected reason=%.512s", restored.detail.c_str());
    else if (!restored.detail.empty())
        diag::log_tagged_fmt("programming_documents", "session_restore detail=%.512s",
            restored.detail.c_str());

    if (!g_sa_settings.workspace.last_active_path.empty() &&
        g_sa_settings.workspace.active_view == "disasm") {
        loading_binary_overlay::begin_load(g_sa_settings.workspace.last_active_path,
            loading_binary_overlay::completion_action_t::none);
    }
}

void persist_workspace_state()
{
    g_sa_settings.workspace.root_path = file_browser::current_dir;
    g_sa_settings.workspace.active_tab = file_tabs::active_tab;

    file_tabs::write_hot_exit_snapshot_all();
    g_sa_settings.workspace.open_tabs_json =
        file_tabs::serialize_programming_session();

	const auto editor_document = code_editor_widget::document_state();
	if (editor_document.active && !editor_document.filepath.empty()) {
		g_sa_settings.workspace.last_active_path = editor_document.filepath;
        g_sa_settings.workspace.active_view = "editor";
    } else if (const auto workspace = analysis_session::active_workspace();
               workspace && !workspace->identity().normalized_source_path().empty()) {
        g_sa_settings.workspace.last_active_path =
            workspace->identity().normalized_source_path();
        g_sa_settings.workspace.active_view = "disasm";
    }
}


__declspec(noinline) static DWORD seh_settings_load(settings_sa_t& s, bool& out_ok)
{
    out_ok = false;
    __try {
        out_ok = s.load();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return 0;
}

__declspec(noinline) static DWORD seh_restore_workspace_state()
{
    __try {
        restore_workspace_state();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return 0;
}

void init_standalone_chat()
{
    if (s_initialized.load(std::memory_order_acquire)) return;

    diag::log_tagged_fmt("init_chat", "startup_context pid=%lu tid=%lu",
        GetCurrentProcessId(),
        GetCurrentThreadId());
    diag::log_tagged("init_chat", "settings_load_start");
    bool settings_loaded = false;
    DWORD seh_load = seh_settings_load(g_sa_settings, settings_loaded);
    if (seh_load != 0)
        diag::log_tagged_fmt("init_chat", "settings_load_seh code=0x%08lX last_err=%lu", seh_load, GetLastError());
    diag::log_tagged_fmt("init_chat", "settings_load_done loaded=%d", settings_loaded ? 1 : 0);


    editor_config::tab_size               = g_sa_settings.editor_tab_size;
    editor_config::font_size              = g_sa_settings.editor_font_size;
    editor_config::auto_complete          = g_sa_settings.editor_auto_complete;
    editor_config::show_line_numbers      = g_sa_settings.editor_line_numbers;
    editor_config::highlight_current_line = g_sa_settings.editor_highlight_line;
    editor_config::word_wrap              = g_sa_settings.editor_word_wrap;
    editor_config::minimap                = g_sa_settings.editor_minimap;
    editor_config::bracket_match          = g_sa_settings.editor_bracket_match;


    diag::log_tagged("init_chat", "ai_client_create_start");
    g_sa_ai_client = std::make_unique<standalone_ai_client_t>(g_sa_settings);
    diag::log_tagged("init_chat", "ai_client_create_done");

    diag::log_tagged("init_chat", "auth_store_load_start");
    const bool auth_store_loaded = aida::auth::store::load();
    if (auth_store_loaded) {
        diag::log_tagged("init_chat", "auth_store_load_done ok=1");
    } else {
        const std::string auth_store_error = aida::auth::store::last_error();
        diag::log_tagged_fmt("init_chat", "auth_store_load_done ok=0 error=%s",
            auth_store_error.empty() ? "auth_store_load_failed" : auth_store_error.c_str());
    }

    diag::log_tagged("init_chat", "session_store_init_start");
    (void)aida::session::initialize();
    diag::log_tagged("init_chat", "session_store_init_done");

    diag::log_tagged("init_chat", "agent_registry_init_start");
    aida::agent::initialize();
    (void)aida::agent::load_custom_from_disk();
    if (!g_sa_settings.default_agent_name.empty() &&
        aida::agent::get(g_sa_settings.default_agent_name) != nullptr) {
        aida::agent::set_default_agent_name(g_sa_settings.default_agent_name);
        aida::agent::set_active_agent(g_sa_settings.default_agent_name);
    } else {
        aida::agent::set_active_agent(aida::agent::default_agent_name());
    }
    diag::log_tagged("init_chat", "agent_registry_init_done");

    diag::log_tagged("init_chat", "command_registry_init_start");
    (void)aida::commands::initialize();
    diag::log_tagged("init_chat", "command_registry_init_done");

    diag::log_tagged("init_chat", "mcp_register_tools_deferred_until_authorized_ide");
    diag::log_tagged("init_chat", "mcp_server_start_deferred_until_authorized_ide");

    diag::log_tagged_fmt("init_chat", "mcp_client_add_servers count=%zu", g_sa_settings.mcp_client_servers.size());
    for (const auto& srv : g_sa_settings.mcp_client_servers) {
        mcp_client::server_config_t cfg;
        cfg.name         = srv.name;
        cfg.url          = srv.url;
        cfg.api_key      = srv.api_key;
        cfg.enabled      = srv.enabled;
        cfg.auto_connect = srv.auto_connect;
        if (srv.transport == "stdio") {
            cfg.transport = mcp_client::transport_type_t::stdio;
            cfg.command   = srv.command;

            if (!srv.args.empty()) {
                std::istringstream iss(srv.args);
                std::string arg;
                while (iss >> arg)
                    cfg.args.push_back(arg);
            }
        } else {
            cfg.transport = mcp_client::transport_type_t::http_sse;
        }
        s_mcp_client_mgr.add_server(cfg);
    }
    diag::log_tagged("init_chat", "mcp_client_connect_all_start");
    diag::log_tagged("init_chat", "mcp_client_connect_all_deferred_until_authorized_ide");

    diag::log_tagged("init_chat", "marketplace_load_installed_start");
    mcp_marketplace::load_installed(g_sa_settings.marketplace_installed_json);
    diag::log_tagged("init_chat", "marketplace_autoconnect_deferred_until_authorized_ide");
    diag::log_tagged("init_chat", "marketplace_load_installed_done");

    const std::string run_id = "init_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
    diag::log_tagged_fmt("init_chat", "driver_bridge_initialize_start run_id=%s", run_id.c_str());
    driver_bridge::initialize();
    diag::log_tagged_fmt("init_chat",
        "driver_bridge_initialize_done run_id=%s loaded=%d kernel=%d status=%.160s",
        run_id.c_str(),
        driver_bridge::is_loaded() ? 1 : 0,
        driver_bridge::using_kernel_driver() ? 1 : 0,
        driver_bridge::status().c_str());

    diag::log_tagged("init_chat", "restore_workspace_state_start");
    DWORD seh_rws = seh_restore_workspace_state();
    if (seh_rws != 0)
        diag::log_tagged_fmt("init_chat", "restore_workspace_state_seh code=0x%08lX last_err=%lu", seh_rws, GetLastError());
    diag::log_tagged("init_chat", "restore_workspace_state_done");

    output_log::push(bottom_tab_t::output, "[init] AiDA Standalone initialized");
    if (s_server_started.load(std::memory_order_acquire))
        output_log::push(bottom_tab_t::mcp_log, "[mcp-server] Started on port " + std::to_string(g_sa_settings.mcp_port));
    output_log::push(bottom_tab_t::driver_log, "[driver] Bridge initialized");

    s_initialized.store(true, std::memory_order_release);
}

void start_authorized_mcp_services()
{
    if (!s_initialized.load(std::memory_order_acquire))
        return;

    if (s_mcp_shutdown_in_flight.load(std::memory_order_acquire)) {
        static auto s_last_shutdown_wait_log = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (s_last_shutdown_wait_log == std::chrono::steady_clock::time_point{} ||
            std::chrono::duration_cast<std::chrono::seconds>(now - s_last_shutdown_wait_log).count() >= 2)
        {
            diag::log_tagged("init_chat", "authorized_mcp_services_start_deferred_shutdown_in_flight");
            s_last_shutdown_wait_log = now;
        }
        return;
    }

    const bool ide_ready = s_ide_ready_for_mcp_services.load(std::memory_order_acquire);
    if (!ide_ready)
    {
        mcp_standalone::set_ide_lifecycle_ready(false);
        static auto s_last_block_log = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (s_last_block_log == std::chrono::steady_clock::time_point{} ||
            std::chrono::duration_cast<std::chrono::seconds>(now - s_last_block_log).count() >= 2)
        {
            diag::log_tagged_fmt("init_chat",
                "authorized_mcp_services_blocked ide=%d",
                ide_ready ? 1 : 0);
            s_last_block_log = now;
        }
        return;
    }

    mcp_standalone::set_ide_lifecycle_ready(true);

    const bool server_ready = !g_sa_settings.mcp_enabled || s_server_started.load(std::memory_order_acquire);
    const bool tools_ready = s_mcp_tools_registered.load(std::memory_order_acquire);
    const bool clients_ready = s_mcp_clients_connected.load(std::memory_order_acquire);
    if (tools_ready && server_ready && clients_ready)
        return;

    const uint64_t now_ms = static_cast<uint64_t>(GetTickCount64());
    const uint64_t last_post_ms = s_mcp_start_last_post_ms.load(std::memory_order_acquire);
    if (last_post_ms != 0 && now_ms >= last_post_ms && now_ms - last_post_ms < 2000ULL)
        return;

    bool expected = false;
    if (!s_mcp_start_in_flight.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        static std::atomic<uint64_t> s_last_in_flight_log_ms{0};
        const uint64_t last_log_ms = s_last_in_flight_log_ms.load(std::memory_order_acquire);
        if (last_log_ms == 0 || now_ms - last_log_ms >= 2000ULL) {
            s_last_in_flight_log_ms.store(now_ms, std::memory_order_release);
            diag::log_tagged("init_chat", "authorized_mcp_services_start_already_in_flight");
        }
        return;
    }

    s_mcp_start_last_post_ms.store(now_ms, std::memory_order_release);
    bool posted = submit_chat_task(
        "authorized_mcp_services_start",
        aida::infra::executor::domain_t::security_liveness,
        "lifecycle_gate",
        5,
        [] {
        start_authorized_mcp_services_worker();
    });
    if (!posted) {
        s_mcp_start_in_flight.store(false, std::memory_order_release);
        diag::log_tagged("init_chat", "authorized_mcp_services_start_post_failed");
    }
}

void mark_ide_ready_for_mcp_services()
{
    s_ide_ready_for_mcp_services.store(true, std::memory_order_release);
}

void shutdown_standalone_chat()
{
    ULONGLONG shutdown_start = shutdown_phase_begin("shutdown_standalone_chat");
    diag::log_tagged_critical("chat", "shutdown_standalone_chat enter");
    log_shutdown_queue_snapshot("shutdown_enter");
    s_cancel = true;
    {
        ULONGLONG phase_start = shutdown_phase_begin("ai_task_cancel");
        std::lock_guard<std::mutex> lk(s_ai_thread_mtx);
        s_cancel = true;
        if (g_sa_ai_client) g_sa_ai_client->cancel();
        if (!s_ai_task_done.load()) {
            std::unique_lock<std::mutex> lk2(s_ai_task_done_mtx);
            s_ai_task_done_cv.wait(lk2, []() { return s_ai_task_done.load(); });
        }
        shutdown_phase_done("ai_task_cancel", phase_start);
    }
    ULONGLONG producer_start = shutdown_phase_begin("producer_stop");
    s_mcp_server.stop();
    s_server_started.store(false, std::memory_order_release);
    mcp_standalone::set_ide_lifecycle_ready(false);
    s_ide_ready_for_mcp_services.store(false, std::memory_order_release);


    if (s_mcp_clients_connected.load(std::memory_order_acquire)) {
        s_mcp_client_mgr.disconnect_all();
        s_mcp_clients_connected.store(false, std::memory_order_release);
    }


    g_sa_settings.marketplace_installed_json = mcp_marketplace::save_installed();
    mcp_marketplace::shutdown();
    shutdown_phase_done("producer_stop", producer_start);

    ULONGLONG ui_start = shutdown_phase_begin("ui_services_shutdown");
    aida::automation_ui::run_ui_shutdown_hooks();
    shutdown_phase_done("ui_services_shutdown", ui_start);

    ULONGLONG session_start = shutdown_phase_begin("session_shutdown");
    aida::automation_ui::prepare_proposal_reviews_for_shutdown();
    (void)aida::session::shutdown();
    shutdown_phase_done("session_shutdown", session_start);

    ULONGLONG queue_start = shutdown_phase_begin("queue_drain");
    log_shutdown_queue_snapshot("queue_drain_before");
    aida::infra::executor::shutdown();
    log_shutdown_queue_snapshot("queue_drain_after_executor");
    const bool queues_quiescent = shutdown_queues_quiescent("queue_drain_after_executor");
    shutdown_phase_done("queue_drain", queue_start);

    conversations::process_store_completion(false);
    ULONGLONG conversation_commit_start = shutdown_phase_begin("conversation_store_commit");
    std::string conversation_commit_error;
    const bool conversation_committed = conversations::commit_shutdown(
        conversation_commit_error);
    diag::log_tagged_critical_fmt("chat",
        "conversation_store_shutdown_commit_done committed=%d error_present=%d",
        conversation_committed ? 1 : 0,
        conversation_commit_error.empty() ? 0 : 1);
    shutdown_phase_done("conversation_store_commit", conversation_commit_start);

    if (queues_quiescent) {
        ULONGLONG dependency_start = shutdown_phase_begin("driver_event_shutdown");
        driver_bridge::shutdown("chat.shutdown_after_queues");
        aida::events::shutdown();
        shutdown_phase_done("driver_event_shutdown", dependency_start);
    } else {
        diag::log_tagged_critical("chat", "driver_event_shutdown_deferred reason=queue_drain_incomplete");
    }

    ULONGLONG persist_start = shutdown_phase_begin("persist_state");
    persist_workspace_state();
    std::string settings_commit_error;
    const bool settings_committed = aida::settings_persistence::shutdown_commit(
        g_sa_settings, settings_commit_error);
    if (!settings_committed)
        diag::log_tagged_critical_fmt("chat",
            "settings_shutdown_commit_failed detail=%.512s",
            settings_commit_error.c_str());
    else
        diag::log_tagged_critical("chat", "settings_shutdown_commit_done");
    g_sa_ai_client.reset();
    shutdown_phase_done("persist_state", persist_start);

    s_initialized.store(false, std::memory_order_release);
    log_shutdown_queue_snapshot("shutdown_done");
    shutdown_phase_done("shutdown_standalone_chat", shutdown_start);
    diag::log_tagged_critical("chat", "shutdown_standalone_chat done");
}


void tick_ai_chat()
{
    if (!s_initialized.load(std::memory_order_acquire)) return;
    if (g_chat_messages.empty()) return;

    auto& last = g_chat_messages.back();
    if (!last.is_user || s_ai_running.load()) return;


    std::string user_text = last.text;


    if (!user_text.empty() && user_text[0] == '/') {
        size_t name_end = 1;
        while (name_end < user_text.size() &&
               user_text[name_end] != ' ' &&
               user_text[name_end] != '\t' &&
               user_text[name_end] != '\n') {
            ++name_end;
        }
        const std::string cmd_name = user_text.substr(1, name_end - 1);
        std::vector<std::string> cmd_args;
        if (name_end < user_text.size()) {
            std::string rest = user_text.substr(name_end);
            size_t s = 0;
            while (s < rest.size() && (rest[s] == ' ' || rest[s] == '\t')) ++s;
            while (s < rest.size()) {
                if (rest[s] == '"' || rest[s] == '\'') {
                    const char quote = rest[s];
                    ++s;
                    std::string tok;
                    while (s < rest.size() && rest[s] != quote) {
                        if (rest[s] == '\\' && s + 1 < rest.size() &&
                            (rest[s + 1] == quote || rest[s + 1] == '\\')) {
                            tok.push_back(rest[s + 1]);
                            s += 2;
                        } else {
                            tok.push_back(rest[s]);
                            ++s;
                        }
                    }
                    if (s < rest.size() && rest[s] == quote) ++s;
                    cmd_args.push_back(std::move(tok));
                } else {
                    size_t e = s;
                    while (e < rest.size() && rest[e] != ' ' && rest[e] != '\t') ++e;
                    cmd_args.push_back(rest.substr(s, e - s));
                    s = e;
                }
                while (s < rest.size() && (rest[s] == ' ' || rest[s] == '\t')) ++s;
            }
        }

        aida::commands::command_t cmd;
        const bool found = aida::commands::find(cmd_name, cmd);
        if (found) {
            std::string resolved;
            const bool ok = aida::commands::execute(cmd_name, cmd_args, resolved);
            if (!ok) {
                ChatMessage err;
                err.is_user = false;
                err.has_thinking = false;
                err.streaming = false;
                err.text = std::string("[/")+ cmd_name + "] " + aida::commands::last_error();
                g_chat_messages.push_back(err);
                aida::automation_ui::request_chat_scroll_to_bottom();
                return;
            }

            const bool is_programmatic =
                (cmd.source == aida::commands::command_source_t::builtin && cmd.template_text.empty()) ||
                (cmd.source == aida::commands::command_source_t::agent);
            if (is_programmatic) {
                ChatMessage out_msg;
                out_msg.is_user = false;
                out_msg.has_thinking = false;
                out_msg.streaming = false;
                out_msg.text = resolved.empty()
                    ? (std::string("[/") + cmd_name + "] done")
                    : resolved;
                g_chat_messages.push_back(out_msg);
                aida::automation_ui::request_chat_scroll_to_bottom();
                return;
            }

            user_text = resolved;
            last.text = resolved;
        }
    }


    if (!g_sa_ai_client || !g_sa_ai_client->is_available()) {
        ChatMessage ai;
        ai.is_user       = false;
        ai.has_thinking   = false;
        ai.streaming      = false;
        ai.text           = "AI not configured. Click \"Settings\" in the chat header to set your API key and model.";
        g_chat_messages.push_back(ai);
        aida::automation_ui::request_chat_scroll_to_bottom();
        return;
    }


    ChatMessage ai;
    ai.is_user       = false;
    ai.has_thinking   = true;
    ai.streaming      = true;
    ai.thinking_text  = "";
    ai.text           = "";
    {
        const auto settings = g_sa_settings.ai_runtime_snapshot();
        const std::string sel_provider = settings.selected_provider_id();
        const std::string sel_model    = settings.selected_model_id();
        std::string m_disp = sel_model;
        if (!sel_provider.empty() && !sel_model.empty()) {
            const auto* m = aida::provider::catalog::get_model(sel_provider, sel_model);
            if (m != nullptr && !m->name.empty())
                m_disp = m->name;
        }
        if (m_disp.empty()) {
            const auto* prof = settings.get_active_profile();
            if (prof) m_disp = prof->model;
        }
        ai.model_id = m_disp;
        diag::log_tagged_fmt("chat",
            "new_assistant_message provider=%.40s model=%.80s",
            sel_provider.c_str(), m_disp.c_str());
    }
    g_chat_messages.push_back(ai);
    aida::automation_ui::request_chat_scroll_to_bottom();


    std::vector<std::pair<std::string, std::string>> history;
    const std::size_t history_end = g_chat_messages.size() > 2 ? g_chat_messages.size() - 2 : 0;
    for (std::size_t i = 0; i < history_end; ++i) {
        auto& m = g_chat_messages[i];
        if (!m.text.empty())
            history.emplace_back(m.is_user ? "User" : "Assistant", m.text);
    }

    {
        std::string sid_check = get_chat_session_id_locked();
        if (sid_check.empty() && conversations::current_id.empty()) {
            aida::session::session_info_t info;
            if (aida::session::create(info, std::string{}, std::string{}, std::string{})) {
                conversations::current_id = info.id;
                chat_bind_session(info.id);
            }
        } else if (sid_check.empty() && !conversations::current_id.empty()) {
            chat_bind_session(conversations::current_id);
        } else if (!sid_check.empty() && conversations::current_id.empty()) {
            conversations::current_id = sid_check;
        }
    }

    {
        std::string sid = get_chat_session_id_locked();
        if (!sid.empty()) {
            int user_count = 0;
            for (const auto& m : g_chat_messages) {
                if (m.is_user) ++user_count;
            }
            if (user_count == 1) {
                std::string first_user_text = user_text;
                std::string provider_id =
                    g_sa_settings.ai_runtime_snapshot().selected_provider_id();
                (void)submit_chat_task(
                    "chat.auto_title",
                    aida::infra::executor::domain_t::general,
                    "bounded_task",
                    3,
                    [sid, first_user_text, provider_id]() {
                    (void)aida::compaction::maybe_auto_title(sid, first_user_text, provider_id);
                });
            }
        }
    }


    {
        std::lock_guard<std::mutex> lk(s_ai_thread_mtx);
        if (s_ai_running.load()) {
            s_cancel = true;
            if (g_sa_ai_client) g_sa_ai_client->cancel();
        }
        if (!s_ai_task_done.load()) {
            std::unique_lock<std::mutex> lk2(s_ai_task_done_mtx);
            s_ai_task_done_cv.wait(lk2, []() { return s_ai_task_done.load(); });
        }
        s_cancel      = false;
        s_ai_running  = true;
        s_ai_task_done.store(false);
        const bool posted = submit_chat_task(
            "chat.agentic_request",
            aida::infra::executor::domain_t::external_tool,
            "bounded_task",
            3,
            [user_text = std::move(user_text), history = std::move(history)]() mutable {
            run_agentic(std::move(user_text), std::move(history));
            s_ai_task_done.store(true);
            s_ai_task_done_cv.notify_all();
        });
        if (!posted) {
            post_update(ai_update_t::ERR, "Error: Service unavailable. Please restart the application.");
            s_ai_running = false;
            s_ai_task_done.store(true);
            s_ai_task_done_cv.notify_all();
        }
    }
}


ai_chat_poll_result_t poll_ai_chat()
{
    ai_chat_poll_result_t result;
    if (!s_initialized.load(std::memory_order_acquire)) return result;

    std::deque<ai_update_t> local;
    {
        std::lock_guard<std::mutex> lk(s_update_mtx);
        std::swap(local, s_updates);
    }

    for (auto& u : local) {
        if (g_chat_messages.empty()) continue;
        auto& last = g_chat_messages.back();
        if (last.is_user) continue;

        result.any = true;
        switch (u.type) {
        case ai_update_t::THINKING:
            if (!u.text.empty()) {
                if (!last.thinking_text.empty())
                    last.thinking_text += "\n";
                last.thinking_text += u.text;
                result.thinking_started = true;
            }
            break;

        case ai_update_t::CHUNK:
            last.text += u.text;
            result.content_grew = true;
            aida::automation_ui::request_chat_scroll_to_bottom();
            break;

        case ai_update_t::COMPLETE:
            last.streaming = false;
            s_ai_running         = false;
            result.settled       = true;
            aida::automation_ui::request_chat_scroll_to_bottom();
            break;

        case ai_update_t::ERR:
            if (!u.text.empty()) last.text = u.text;
            last.streaming       = false;
            s_ai_running         = false;
            result.settled       = true;
            aida::automation_ui::request_chat_scroll_to_bottom();
            break;
        }
    }
    result.message_total = g_chat_messages.size();
    result.ai_busy = s_ai_running.load();
    return result;
}


bool is_ai_busy()
{
    return s_ai_running.load();
}

void chat_request_cancel()
{
    if (!s_ai_running.load()) return;
    s_cancel = true;
    if (g_sa_ai_client) g_sa_ai_client->cancel();
}


std::atomic<bool>* chat_cancel_flag()
{
    return &s_cancel;
}


void chat_bind_session(const std::string& session_id)
{
    diag::log_tagged_fmt("chat", "chat_bind_session id='%s'", session_id.c_str());
    const bool changed = get_chat_session_id_locked() != session_id;
    set_chat_session_id_locked(session_id);
    if (changed)
        aida::automation_ui::restore_proposal_reviews_for_session(session_id);
}


std::string chat_active_session()
{
    return get_chat_session_id_locked();
}


void chat_record_assistant_message_id(const std::string& message_id)
{
    set_chat_last_assistant_message_id_locked(message_id);
}


std::string start_new_conversation()
{
    diag::log_tagged("chat", "start_new_conversation enter");
    conversations::new_chat();
    diag::log_tagged("chat", "start_new_conversation queued");
    return {};
}

namespace aida::automation_ui {

namespace {

constexpr std::size_t max_evidence_bytes = 64U * 1024U;
constexpr std::size_t max_rendered_messages = 256U;
constexpr std::size_t max_evidence_items = 256U;
std::mutex s_evidence_mutex;
std::deque<evidence_envelope_t> s_evidence;
std::deque<std::pair<std::string, std::function<bool(std::string&)>>>
    s_evidence_source_returns;
std::shared_ptr<const std::vector<evidence_envelope_t>> s_evidence_publication =
    std::make_shared<const std::vector<evidence_envelope_t>>();
std::atomic<std::uint64_t> s_evidence_sequence{1};
std::string s_loaded_evidence_session;
std::string s_requested_evidence_session;
struct evidence_session_publication_t {
    std::string loaded;
    std::string requested;
};
std::shared_ptr<const evidence_session_publication_t> s_evidence_session_publication =
    std::make_shared<const evidence_session_publication_t>();
std::optional<aida::conversation_store::request_t> s_deferred_evidence_save;
std::mutex s_editor_proposal_mutex;
editor_proposal_snapshot_t s_editor_proposal;
std::atomic<std::uint64_t> s_editor_proposal_operation{1};
std::mutex s_editor_proposal_hunks_mutex;
std::string s_editor_proposal_hunks_audit_id;
std::vector<aida::session::proposal_audit_hunk_t> s_editor_proposal_hunks;
std::atomic<std::uint64_t> s_proposal_restore_operation{1};
std::atomic<bool> s_proposal_restore_pending{false};
std::mutex s_reverse_engineering_proposal_mutex;
reverse_engineering_proposal_snapshot_t s_reverse_engineering_proposal;
std::atomic<std::uint64_t> s_reverse_engineering_proposal_operation{1};
std::shared_ptr<const reverse_engineering_proposal_snapshot_t>
    s_reverse_engineering_proposal_publication =
        std::make_shared<const reverse_engineering_proposal_snapshot_t>();

struct reverse_engineering_proposal_binding_t {
    disasm_view::workspace_context_t workspace;
    aida::analysis::address_t address;
    std::vector<std::uint8_t> before_bytes;
    std::vector<std::uint8_t> after_bytes;
    network_view::artifact_identity_t network_source;
    network_view::artifact_identity_t network_staged;
    std::vector<std::uint8_t> network_after;
    std::uint32_t target_pid = 0;
};

reverse_engineering_proposal_binding_t s_reverse_engineering_proposal_binding;

void publish_reverse_engineering_proposal_locked()
{
    std::shared_ptr<const reverse_engineering_proposal_snapshot_t> publication =
        std::make_shared<reverse_engineering_proposal_snapshot_t>(
            s_reverse_engineering_proposal);
    std::atomic_store_explicit(&s_reverse_engineering_proposal_publication,
        std::move(publication), std::memory_order_release);
}

void publish_evidence_locked()
{
    auto publication = std::make_shared<const std::vector<evidence_envelope_t>>(
        s_evidence.begin(), s_evidence.end());
    std::atomic_store_explicit(&s_evidence_publication, std::move(publication),
        std::memory_order_release);
}

void publish_evidence_session_locked()
{
    auto mutable_publication = std::make_shared<evidence_session_publication_t>();
    mutable_publication->loaded = s_loaded_evidence_session;
    mutable_publication->requested = s_requested_evidence_session;
    std::shared_ptr<const evidence_session_publication_t> publication =
        std::move(mutable_publication);
    std::atomic_store_explicit(&s_evidence_session_publication,
        std::move(publication), std::memory_order_release);
}

std::uint64_t hash_append(std::uint64_t hash, std::string_view text)
{
    for (const char character : text) {
        const auto value = static_cast<unsigned char>(character);
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string bounded_metadata_string(std::string value, std::size_t limit);

std::uint64_t exact_content_hash(std::string_view text)
{
    std::uint64_t hash = hash_append(14695981039346656037ULL, text);
    hash ^= static_cast<std::uint64_t>(text.size());
    hash *= 1099511628211ULL;
    return hash == 0 ? 1 : hash;
}

std::string hash_text(std::uint64_t hash)
{
    char value[17]{};
    std::snprintf(value, sizeof(value), "%016llX",
        static_cast<unsigned long long>(hash));
    return value;
}

std::string stable_proposal_id(std::string_view family,
                               const message_identity_t& source,
                               std::uint64_t operation)
{
    std::uint64_t identity_hash = hash_append(14695981039346656037ULL,
        source.session_id);
    identity_hash ^= source.fingerprint;
    identity_hash *= 1099511628211ULL;
    identity_hash ^= static_cast<std::uint64_t>(source.timestamp);
    identity_hash *= 1099511628211ULL;
    identity_hash ^= static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    identity_hash *= 1099511628211ULL;
    identity_hash ^= operation;
    identity_hash *= 1099511628211ULL;
    return "proposal." + std::string(family) + "." + hash_text(identity_hash) +
        "." + std::to_string(operation);
}

std::string proposal_task_id(const std::string& audit_id)
{
    return "task." + audit_id;
}

bool persist_proposal_audit(aida::session::proposal_audit_record_t record,
                            std::string& reason)
{
    if (aida::session::upsert_proposal_audit(std::move(record))) {
        reason.clear();
        return true;
    }
    reason = aida::session::last_error();
    if (reason.empty()) reason = "The proposal audit transaction failed.";
    return false;
}

std::string hunk_decision(code_editor_widget::diff_hunk_state_t state)
{
    if (state == code_editor_widget::diff_hunk_state_t::accepted) return "accepted";
    if (state == code_editor_widget::diff_hunk_state_t::rejected) return "rejected";
    return "pending";
}

std::vector<aida::session::proposal_audit_hunk_t> editor_hunk_audit_snapshot()
{
    std::vector<aida::session::proposal_audit_hunk_t> result;
    if (!code_editor_widget::has_pending_diff()) return result;
    const auto diff = code_editor_widget::pending_diff();
    if (diff.hunks.size() > 512U) return {};
    result.reserve(diff.hunks.size());
    for (std::size_t index = 0; index < diff.hunks.size(); ++index) {
        const auto& source = diff.hunks[index];
        std::uint64_t before_hash = 14695981039346656037ULL;
        std::uint64_t after_hash = 14695981039346656037ULL;
        std::uint64_t before_size = 0;
        std::uint64_t after_size = 0;
        auto append_framed = [](std::uint64_t& hash, std::uint64_t& size,
                                std::string_view value) {
            hash = hash_append(hash, value);
            size += static_cast<std::uint64_t>(value.size());
        };
        for (const auto& line : source.lines) {
            const std::string positions = std::to_string(line.old_line) + ":" +
                std::to_string(line.new_line) + ":";
            if (line.kind != code_editor_widget::diff_line_kind_t::added) {
                append_framed(before_hash, before_size, positions);
                append_framed(before_hash, before_size, line.text);
                append_framed(before_hash, before_size, "\n");
            }
            if (line.kind != code_editor_widget::diff_line_kind_t::removed) {
                append_framed(after_hash, after_size, positions);
                append_framed(after_hash, after_size, line.text);
                append_framed(after_hash, after_size, "\n");
            }
        }
        before_hash ^= before_size;
        before_hash *= 1099511628211ULL;
        after_hash ^= after_size;
        after_hash *= 1099511628211ULL;
        if (before_hash == 0) before_hash = 1;
        if (after_hash == 0) after_hash = 1;
        aida::session::proposal_audit_hunk_t hunk;
        hunk.index = static_cast<std::uint32_t>(index);
        hunk.old_start = source.old_start;
        hunk.old_count = source.old_count;
        hunk.new_start = source.new_start;
        hunk.new_count = source.new_count;
        hunk.decision = hunk_decision(source.state);
        hunk.before_hash = hash_text(before_hash);
        hunk.after_hash = hash_text(after_hash);
        result.push_back(std::move(hunk));
    }
    return result;
}

std::vector<aida::session::proposal_audit_hunk_t> editor_proposal_hunks(
    const editor_proposal_snapshot_t& proposal)
{
    const bool live_identity = !proposal.stale &&
        proposal.target_document_numeric_id != 0 &&
        code_editor_widget::active_document_id() == proposal.target_document_numeric_id &&
        code_editor_widget::document_revision() == proposal.base_document_revision &&
        code_editor_widget::document_content_fingerprint() == proposal.base_content_hash;
    if (live_identity) {
        auto live = editor_hunk_audit_snapshot();
        if (!live.empty()) {
            std::lock_guard<std::mutex> lock(s_editor_proposal_hunks_mutex);
            s_editor_proposal_hunks_audit_id = proposal.audit_id;
            s_editor_proposal_hunks = live;
            return live;
        }
    }
    std::lock_guard<std::mutex> lock(s_editor_proposal_hunks_mutex);
    return s_editor_proposal_hunks_audit_id == proposal.audit_id
        ? s_editor_proposal_hunks
        : std::vector<aida::session::proposal_audit_hunk_t>{};
}

void retain_editor_proposal_hunks(
    const std::string& audit_id,
    const std::vector<aida::session::proposal_audit_hunk_t>& hunks)
{
    std::lock_guard<std::mutex> lock(s_editor_proposal_hunks_mutex);
    s_editor_proposal_hunks_audit_id = audit_id;
    s_editor_proposal_hunks = hunks;
}

aida::session::proposal_audit_record_t editor_audit_record(
    const editor_proposal_snapshot_t& proposal, std::string lifecycle_state,
    std::string outcome, std::string detail)
{
    aida::session::proposal_audit_record_t record;
    record.audit_id = proposal.audit_id;
    record.proposal_id = proposal.id;
    record.family = "editor";
    record.kind = "full_content_diff";
    record.session_id = proposal.source.session_id;
    record.source_index = proposal.source.index;
    record.source_timestamp = proposal.source.timestamp;
    record.source_fingerprint = proposal.source.fingerprint;
    record.target_id = proposal.target_document_id;
    record.target_view_id = "document.code";
    record.target_generation = proposal.generation;
    record.source_revision = proposal.base_document_revision;
    record.before_hash = hash_text(proposal.base_content_hash);
    record.after_hash = hash_text(proposal.proposed_content_hash);
    record.result_hash = proposal.result_content_hash == 0
        ? std::string{} : hash_text(proposal.result_content_hash);
    record.result_revision = proposal.result_revision;
    record.provenance = "chat:" + proposal.source.session_id + ":" +
        std::to_string(proposal.source.fingerprint);
    record.detail = bounded_metadata_string(std::move(detail), 4096U);
    record.lifecycle_state = std::move(lifecycle_state);
    record.outcome = std::move(outcome);
    record.task_id = proposal.task_id;
    record.diagnostic_id = proposal.diagnostic_id;
    if (proposal.result_revision != 0)
        record.undo_revert_identity = "editor.undo:" +
            std::to_string(proposal.target_document_numeric_id) + ":" +
            std::to_string(proposal.result_revision);
    record.revalidation_required = record.outcome == "stale" ||
        (record.outcome == "failure" && proposal.pending);
    record.hunks = editor_proposal_hunks(proposal);
    return record;
}

aida::session::proposal_audit_record_t reverse_audit_record(
    const reverse_engineering_proposal_snapshot_t& proposal,
    std::string lifecycle_state, std::string outcome, std::string detail)
{
    aida::session::proposal_audit_record_t record;
    record.audit_id = proposal.audit_id;
    record.proposal_id = proposal.id;
    record.family = "reverse_engineering";
    switch (proposal.kind) {
    case reverse_engineering_proposal_kind_t::analysis_rename:
        record.kind = "analysis.rename";
        break;
    case reverse_engineering_proposal_kind_t::analysis_comment:
        record.kind = "analysis.comment";
        break;
    case reverse_engineering_proposal_kind_t::analysis_type:
        record.kind = "analysis.type";
        break;
    case reverse_engineering_proposal_kind_t::static_patch:
        record.kind = "patch.static";
        break;
    case reverse_engineering_proposal_kind_t::live_patch:
        record.kind = "patch.live";
        break;
    case reverse_engineering_proposal_kind_t::network_request_edit:
        record.kind = "network.request_edit";
        break;
    case reverse_engineering_proposal_kind_t::network_replay_staging:
        record.kind = "network.replay_stage";
        break;
    default:
        record.kind = "unknown";
        break;
    }
    record.session_id = proposal.source.session_id;
    record.source_index = proposal.source.index;
    record.source_timestamp = proposal.source.timestamp;
    record.source_fingerprint = proposal.source.fingerprint;
    record.target_id = proposal.target_id.empty() ? "unresolved" : proposal.target_id;
    record.target_view_id = proposal.target_view_id;
    record.target_generation = proposal.expected_generation;
    record.source_revision = proposal.expected_revision;
    record.target_overlay_revision = proposal.expected_overlay_revision;
    record.before_hash = hash_text(exact_content_hash(proposal.before_value));
    record.after_hash = hash_text(exact_content_hash(proposal.after_value));
    record.result_hash = proposal.result_hash == 0
        ? std::string{} : hash_text(proposal.result_hash);
    record.result_revision = proposal.result_revision;
    record.provenance = bounded_metadata_string(proposal.provenance, 512U);
    record.detail = bounded_metadata_string(std::move(detail), 4096U);
    record.lifecycle_state = std::move(lifecycle_state);
    record.outcome = std::move(outcome);
    record.task_id = proposal.task_id;
    record.diagnostic_id = proposal.diagnostic_id;
    record.undo_revert_identity = proposal.rollback_action_id;
    record.revalidation_required = record.outcome == "stale" ||
        (record.outcome == "failure" && proposal.pending);
    return record;
}

bool register_proposal_task(const std::string& task_id,
                            const std::string& session_id,
                            const std::string& target,
                            const std::string& label)
{
    aida::ui::task_center::task_registration_t registration;
    registration.id = task_id;
    registration.source = "ai_proposal";
    registration.owner = "ai_proposal";
    registration.owner_view = "view.ai.evidence";
    registration.owner_action = "ai.proposal.review";
    registration.session = session_id;
    registration.target = target;
    registration.label = label;
    registration.stage = "Queued for validation";
    registration.affected_entity = target;
    registration.callbacks.focus = [] {
        chat_open_view("view.ai.evidence");
    };
    return aida::ui::task_center::register_task(std::move(registration));
}

void update_proposal_task(const std::string& task_id,
                          aida::ui::task_center::task_state_t state,
                          float progress, std::string stage,
                          std::string detail, std::string diagnostic_id = {})
{
    if (task_id.empty()) return;
    static_cast<void>(aida::ui::task_center::update_task(task_id, state, progress,
        std::move(stage), bounded_metadata_string(std::move(detail), 4096U),
        std::move(diagnostic_id)));
}

void raise_proposal_diagnostic(const std::string& diagnostic_id,
                               const std::string& task_id,
                               const std::string& target,
                               std::string summary, std::string detail)
{
    if (diagnostic_id.empty()) return;
    aida::ui::task_center::diagnostic_registration_t diagnostic;
    diagnostic.id = diagnostic_id;
    diagnostic.task_id = task_id;
    diagnostic.owner = "ai_proposal";
    diagnostic.target = target;
    diagnostic.summary = bounded_metadata_string(std::move(summary), 512U);
    diagnostic.details = bounded_metadata_string(std::move(detail), 4096U);
    diagnostic.severity = aida::ui::task_center::diagnostic_severity_t::error;
    diagnostic.callbacks.focus = [] {
        chat_open_view("view.ai.evidence");
    };
    static_cast<void>(aida::ui::task_center::raise_diagnostic(
        std::move(diagnostic)));
}

bool submit_proposal_job(const char* label, std::function<void()> body)
{
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "ai_proposal";
    submission.label = label;
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::general;
    submission.priority = 2;
    submission.body = std::move(body);
    return aida::infra::executor::submit(std::move(submission)).submitted;
}

std::string bounded_metadata_string(std::string value, std::size_t limit)
{
    if (value.size() > limit) value.resize(limit);
    return value;
}

std::string evidence_session_id()
{
    std::string session = chat_active_session();
    if (session.empty()) session = conversations::current_id;
    return session.size() <= 128U ? session : std::string{};
}

void persist_evidence_metadata(const std::string& session,
                               std::vector<aida::conversation_store::evidence_t> items)
{
    if (session.empty()) return;
    aida::conversation_store::request_t request;
    request.operation = aida::conversation_store::operation_t::save_evidence;
    request.current.id = session;
    request.current.revision = conversations::current_revision;
    request.catalog_generation = conversations::catalog_generation;
    if (items.size() > max_evidence_items)
        items.erase(items.begin(), items.end() - max_evidence_items);
    request.current.evidence = std::move(items);
    const auto submitted = aida::conversation_store::submit(request);
    if (submitted == aida::conversation_store::request_result_t::busy) {
        s_deferred_evidence_save = std::move(request);
    } else if (submitted == aida::conversation_store::request_result_t::queued ||
        submitted == aida::conversation_store::request_result_t::preview_recorded) {
        s_deferred_evidence_save.reset();
    } else {
        conversations::persistence_error = "Evidence metadata persistence was rejected.";
    }
}

void synchronize_evidence_session_view()
{
    synchronize_evidence_session();
}

}

void synchronize_evidence_session()
{
    if (s_deferred_evidence_save) {
        const auto persistence = aida::conversation_store::status();
        if (persistence.pending || persistence.failed) return;
        const auto submitted = aida::conversation_store::submit(*s_deferred_evidence_save);
        if (submitted == aida::conversation_store::request_result_t::queued ||
            submitted == aida::conversation_store::request_result_t::preview_recorded)
            s_deferred_evidence_save.reset();
        if (submitted != aida::conversation_store::request_result_t::busy)
            return;
    }
    const std::string session = evidence_session_id();
    const auto published_session = std::atomic_load_explicit(
        &s_evidence_session_publication, std::memory_order_acquire);
    if (published_session && (session == published_session->loaded ||
            session == published_session->requested))
        return;
    {
        std::lock_guard<std::mutex> lock(s_evidence_mutex);
        if (session == s_loaded_evidence_session || session == s_requested_evidence_session)
            return;
    }
    if (session.empty()) {
        std::lock_guard<std::mutex> lock(s_evidence_mutex);
        s_evidence.clear();
        s_evidence_source_returns.clear();
        s_loaded_evidence_session.clear();
        s_requested_evidence_session.clear();
        publish_evidence_session_locked();
        publish_evidence_locked();
        return;
    }
    const auto persistence = aida::conversation_store::status();
    if (persistence.pending || persistence.failed) return;
    aida::conversation_store::request_t request;
    request.operation = aida::conversation_store::operation_t::load_evidence;
    request.target_id = session;
    request.catalog_generation = conversations::catalog_generation;
    const auto submitted = aida::conversation_store::submit(std::move(request));
    if (submitted == aida::conversation_store::request_result_t::queued) {
        std::lock_guard<std::mutex> lock(s_evidence_mutex);
        s_requested_evidence_session = session;
        publish_evidence_session_locked();
    }
}

namespace {

std::uint64_t message_fingerprint(const ChatMessage& message)
{
    std::uint64_t hash = 14695981039346656037ULL;
    hash = hash_append(hash, message.text);
    hash = hash_append(hash, message.thinking_text);
    hash = hash_append(hash, message.tool_name);
    hash = hash_append(hash, message.model_id);
    hash ^= static_cast<std::uint64_t>(message.timestamp);
    hash *= 1099511628211ULL;
    hash ^= message.is_user ? 1ULL : 0ULL;
    hash *= 1099511628211ULL;
    hash ^= message.is_tool_result ? 1ULL : 0ULL;
    return hash;
}

const ChatMessage* resolve_message(const message_identity_t& identity, std::string& reason)
{
    const std::string session = chat_active_session();
    if (identity.session_id != session) {
        reason = "The conversation changed; reopen the message menu.";
        return nullptr;
    }
    if (identity.index >= g_chat_messages.size()) {
        reason = "The message no longer exists.";
        return nullptr;
    }
    const auto& message = g_chat_messages[identity.index];
    if (message.timestamp != identity.timestamp || message_fingerprint(message) != identity.fingerprint) {
        reason = "The message changed after this menu was opened; reopen it.";
        return nullptr;
    }
    reason.clear();
    return &message;
}

action_result_t failed(std::string reason)
{
    action_result_t result;
    result.detail = std::move(reason);
    return result;
}

action_result_t completed(std::string detail = {})
{
    action_result_t result;
    result.succeeded = true;
    result.detail = std::move(detail);
    return result;
}

constexpr std::size_t max_proposal_payload_bytes = 64U * 1024U;
constexpr std::size_t max_proposal_text_bytes = 4096U;
constexpr std::size_t max_proposal_patch_bytes = 4096U;
constexpr std::size_t max_network_request_bytes = 65535U;

std::string bounded_proposal_text(const nlohmann::json& value, const char* key,
                                  std::size_t limit, std::string& reason,
                                  bool required = false)
{
    if (!value.contains(key)) {
        if (required) reason = std::string("The proposal is missing '") + key + "'.";
        return {};
    }
    if (!value[key].is_string()) {
        reason = std::string("The proposal field '") + key + "' must be text.";
        return {};
    }
    std::string result = value[key].get<std::string>();
    if (result.size() > limit) {
        reason = std::string("The proposal field '") + key + "' exceeds its safety bound.";
        return {};
    }
    if (required && result.empty())
        reason = std::string("The proposal field '") + key + "' cannot be empty.";
    return result;
}

bool proposal_document(const ChatMessage& message, nlohmann::json& document,
                       std::string& reason)
{
    if (message.is_user || message.streaming) {
        reason = message.is_user
            ? "Only an assistant or tool result can originate a reviewed change."
            : "Wait for the assistant response to finish before reviewing its change.";
        return false;
    }
    constexpr std::string_view marker = "```aida-proposal";
    const auto marker_position = message.text.find(marker);
    if (marker_position == std::string::npos) {
        reason = "No fenced aida-proposal document is present in this message.";
        return false;
    }
    if (message.text.find("```") != marker_position) {
        reason = "The aida-proposal document must be the message's only fenced block.";
        return false;
    }
    if ((marker_position != 0 && message.text[marker_position - 1U] != '\n') ||
        marker_position + marker.size() >= message.text.size() ||
        (message.text[marker_position + marker.size()] != '\r' &&
         message.text[marker_position + marker.size()] != '\n')) {
        reason = "The aida-proposal fence label must be exact and begin on its own line.";
        return false;
    }
    if (message.text.find(marker, marker_position + marker.size()) != std::string::npos) {
        reason = "A message may contain exactly one aida-proposal fence.";
        return false;
    }
    const auto opening_line_end = message.text.find('\n', marker_position + marker.size());
    if (opening_line_end == std::string::npos) {
        reason = "The aida-proposal fence has no JSON payload.";
        return false;
    }
    const auto closing_line = message.text.find("\n```", opening_line_end + 1U);
    if (closing_line == std::string::npos) {
        reason = "The aida-proposal fence is not closed.";
        return false;
    }
    const auto payload_begin = opening_line_end + 1U;
    const auto payload_end = closing_line;
    const auto closing_end = closing_line + 4U;
    if (payload_end <= payload_begin ||
        (closing_end < message.text.size() && message.text[closing_end] != '\r' &&
         message.text[closing_end] != '\n')) {
        reason = "The aida-proposal closing fence must be exact and on its own line.";
        return false;
    }
    if (message.text.find("```", closing_end) != std::string::npos) {
        reason = "A message containing aida-proposal may not contain additional code fences.";
        return false;
    }
    const std::size_t payload_size = payload_end - payload_begin;
    if (payload_size > max_proposal_payload_bytes) {
        reason = "The aida-proposal payload exceeds 64 KiB.";
        return false;
    }
    document = nlohmann::json::parse(
        message.text.begin() + static_cast<std::ptrdiff_t>(payload_begin),
        message.text.begin() + static_cast<std::ptrdiff_t>(payload_end), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        reason = "The aida-proposal payload is not a valid JSON object.";
        return false;
    }
    if (!document.contains("schema") || !document["schema"].is_string() ||
        document["schema"].get<std::string>() != "aida.re-proposal/v1") {
        reason = "The proposal schema must be aida.re-proposal/v1.";
        return false;
    }
    reason.clear();
    return true;
}

reverse_engineering_proposal_kind_t proposal_kind(std::string_view value)
{
    if (value == "analysis.rename") return reverse_engineering_proposal_kind_t::analysis_rename;
    if (value == "analysis.comment") return reverse_engineering_proposal_kind_t::analysis_comment;
    if (value == "analysis.type") return reverse_engineering_proposal_kind_t::analysis_type;
    if (value == "patch.static") return reverse_engineering_proposal_kind_t::static_patch;
    if (value == "patch.live") return reverse_engineering_proposal_kind_t::live_patch;
    if (value == "network.request_edit") return reverse_engineering_proposal_kind_t::network_request_edit;
    if (value == "network.replay_stage") return reverse_engineering_proposal_kind_t::network_replay_staging;
    return reverse_engineering_proposal_kind_t::none;
}

const char* proposal_kind_label(reverse_engineering_proposal_kind_t kind)
{
    switch (kind) {
    case reverse_engineering_proposal_kind_t::analysis_rename: return "Analysis rename";
    case reverse_engineering_proposal_kind_t::analysis_comment: return "Analysis comment";
    case reverse_engineering_proposal_kind_t::analysis_type: return "Analysis type";
    case reverse_engineering_proposal_kind_t::static_patch: return "Static patch";
    case reverse_engineering_proposal_kind_t::live_patch: return "Live patch review";
    case reverse_engineering_proposal_kind_t::network_request_edit: return "Network request edit";
    case reverse_engineering_proposal_kind_t::network_replay_staging: return "Network replay staging";
    default: return "Unknown proposal";
    }
}

bool parse_u64(const nlohmann::json& value, std::uint64_t& result)
{
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
        return true;
    }
    if (!value.is_string()) return false;
    const std::string text = value.get<std::string>();
    if (text.empty() || text.size() > 20U) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    int base = 10;
    if (text.size() > 2U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        if (text.size() > 18U) return false;
        begin += 2;
        base = 16;
    }
    const auto parsed = std::from_chars(begin, end, result, base);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool required_u64(const nlohmann::json& object, const char* key,
                  std::uint64_t& result, std::string& reason)
{
    if (!object.contains(key) || !parse_u64(object[key], result)) {
        reason = std::string("The proposal target '") + key + "' is missing or invalid.";
        return false;
    }
    return true;
}

bool parse_hex_bytes(std::string_view text, std::size_t limit,
                     std::vector<std::uint8_t>& bytes, std::string& reason)
{
    bytes.clear();
    int high = -1;
    for (char character : text) {
        if (std::isspace(static_cast<unsigned char>(character))) continue;
        int nibble = -1;
        if (character >= '0' && character <= '9') nibble = character - '0';
        else if (character >= 'a' && character <= 'f') nibble = 10 + character - 'a';
        else if (character >= 'A' && character <= 'F') nibble = 10 + character - 'A';
        else {
            reason = "Patch bytes must be hexadecimal pairs separated only by whitespace.";
            return false;
        }
        if (high < 0) high = nibble;
        else {
            if (bytes.size() >= limit) {
                reason = "The patch exceeds the 4096-byte reviewed-change bound.";
                return false;
            }
            bytes.push_back(static_cast<std::uint8_t>((high << 4) | nibble));
            high = -1;
        }
    }
    if (high >= 0 || bytes.empty()) {
        reason = high >= 0 ? "Patch bytes contain an incomplete hexadecimal pair."
                           : "Patch bytes cannot be empty.";
        return false;
    }
    return true;
}

std::string format_hex_bytes(const std::vector<std::uint8_t>& bytes)
{
    std::string result;
    result.reserve(bytes.size() * 3U);
    char value[4]{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0) result.push_back(' ');
        std::snprintf(value, sizeof(value), "%02X", bytes[index]);
        result.append(value);
    }
    return result;
}

std::string overlay_type_at(const disasm_view::workspace_context_t& context,
                            const aida::analysis::address_t& address)
{
    const auto overlay = context.workspace ? context.workspace->overlay() : nullptr;
    if (!overlay) return {};
    const auto snapshot = overlay->snapshot();
    for (const auto& item : snapshot.items) {
        const auto& operation = item.second;
        if (operation.kind == aida::analysis::overlay_operation_kind_t::type_application &&
            operation.address == address)
            return operation.type;
    }
    return "<analysis-default>";
}

std::vector<std::uint8_t> static_bytes_at(
    const disasm_view::workspace_context_t& context,
    const aida::analysis::address_t& address, std::size_t size,
    std::string& reason)
{
    const auto overlay = context.workspace ? context.workspace->overlay() : nullptr;
    if (overlay) {
        const auto patches = overlay->patch_operations();
        const auto found = std::find_if(patches.rbegin(), patches.rend(),
            [&](const aida::analysis::overlay_operation_t& operation) {
                return operation.address == address && operation.bytes.size() == size;
            });
        if (found != patches.rend()) return found->bytes;
    }
    auto read = disasm_view::read_bytes(context, address, size);
    if (!read) {
        reason = read.error().stable_code() + ": " + read.error().message;
        return {};
    }
    return read.take_value();
}

bool parse_network_kind(std::string_view value, network_view::artifact_kind_t& kind)
{
    using kind_t = network_view::artifact_kind_t;
    if (value == "request") kind = kind_t::request;
    else if (value == "intercept_request") kind = kind_t::intercept_request;
    else if (value == "exchange") kind = kind_t::exchange;
    else if (value == "sitemap_request") kind = kind_t::sitemap_request;
    else if (value == "api_request") kind = kind_t::api_request;
    else if (value == "scanner_request") kind = kind_t::scanner_request;
    else if (value == "repeater_request") kind = kind_t::repeater_request;
    else return false;
    return true;
}

bool parse_network_identity(const nlohmann::json& target,
                            network_view::artifact_identity_t& identity,
                            std::string& reason)
{
    identity = {};
    identity.id = bounded_proposal_text(target, "artifact_id", 512U, reason, true);
    if (!reason.empty()) return false;
    identity.source_view_id = bounded_proposal_text(target, "source_view_id", 128U, reason, true);
    if (!reason.empty()) return false;
    const std::string kind = bounded_proposal_text(target, "artifact_kind", 64U, reason, true);
    if (!reason.empty() || !parse_network_kind(kind, identity.kind)) {
        if (reason.empty()) reason = "The proposal artifact kind is not a supported HTTP/1 request kind.";
        return false;
    }
    if (!required_u64(target, "source_id", identity.source_id, reason) ||
        !required_u64(target, "timestamp", identity.timestamp, reason) ||
        !required_u64(target, "revision", identity.revision, reason) ||
        !required_u64(target, "content_hash", identity.content_hash, reason))
        return false;
    std::uint64_t content_size = 0;
    std::uint64_t port = 0;
    if (!required_u64(target, "content_size", content_size, reason) ||
        !required_u64(target, "port", port, reason) || content_size > max_network_request_bytes ||
        port == 0 || port > 65535U) {
        if (reason.empty()) reason = "The proposal request size or target port is invalid.";
        return false;
    }
    identity.content_size = static_cast<std::size_t>(content_size);
    identity.target_port = static_cast<std::uint16_t>(port);
    identity.target_host = bounded_proposal_text(target, "host", 253U, reason, true);
    if (!reason.empty()) return false;
    if (!target.contains("tls") || !target["tls"].is_boolean()) {
        reason = "The proposal target 'tls' flag is missing or invalid.";
        return false;
    }
    identity.use_tls = target["tls"].get<bool>();
    identity.label = identity.id;
    return identity.valid();
}

}

std::size_t message_count()
{
    return g_chat_messages.size();
}

message_identity_t message_identity(std::size_t index)
{
    message_identity_t result;
    result.session_id = chat_active_session();
    result.index = index;
    if (index >= g_chat_messages.size()) return result;
    const auto& message = g_chat_messages[index];
    result.timestamp = message.timestamp;
    result.fingerprint = message_fingerprint(message);
    return result;
}

bool message_selection(const message_identity_t& identity, message_selection_t& selection, std::string& reason)
{
    const ChatMessage* message = resolve_message(identity, reason);
    if (!message) return false;
    selection = message_selection_t{};
    selection.identity = identity;
    selection.text = message->text;
    selection.reasoning = message->thinking_text;
    selection.tool_name = message->tool_name;
    selection.model_id = message->model_id;
    selection.is_user = message->is_user;
    selection.is_tool_result = message->is_tool_result;
    selection.streaming = message->streaming;
    return true;
}

bool open_message_context(const message_identity_t& identity, context_open_origin_t origin, message_context_request_t& request, std::string& reason)
{
    request = message_context_request_t{};
    request.origin = origin;
    return message_selection(identity, request.selection, reason);
}

action_result_t stage_editor_proposal(const message_identity_t& source,
                                      const std::string& proposed_content)
{
    if (s_proposal_restore_pending.load(std::memory_order_acquire))
        return failed("Wait for the bounded proposal-review restore to finish before staging another proposal.");
    std::string reason;
    if (!resolve_message(source, reason)) return failed(std::move(reason));
    if (proposed_content.empty()) return failed("The proposed editor content is empty.");
    {
        std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
        if (s_editor_proposal.pending || s_editor_proposal.applying)
            return failed("Review, apply, or reject the currently pending editor proposal first.");
    }
    const std::uint64_t document_id = code_editor_widget::active_document_id();
    const std::uint64_t base_revision = code_editor_widget::document_revision();
    const std::uint64_t base_hash = code_editor_widget::document_content_fingerprint();
    if (document_id == 0 || base_revision == 0 || base_hash == 0)
        return failed("The active code document has no stable identity, revision, or content hash.");
    const std::string origin = "chat:" + source.session_id + ":" + std::to_string(source.fingerprint);
    if (!code_editor_widget::begin_agent_edit(origin))
        return failed(code_editor_widget::last_error());
    if (!code_editor_widget::propose_full_content(proposed_content)) {
        code_editor_widget::cancel_agent_edit();
        return failed(code_editor_widget::last_error());
    }
    const int hunk_count = code_editor_widget::pending_hunk_count();
    if (hunk_count <= 0 || hunk_count > 512) {
        code_editor_widget::cancel_agent_edit();
        return failed(hunk_count <= 0
            ? "The proposed editor content does not produce a reviewable change."
            : "The proposal exceeds the bounded 512-hunk review limit.");
    }
    const std::uint64_t operation =
        s_editor_proposal_operation.fetch_add(1, std::memory_order_acq_rel);
    editor_proposal_snapshot_t proposal;
    proposal.id = stable_proposal_id("editor", source, operation);
    proposal.audit_id = proposal.id;
    proposal.task_id = proposal_task_id(proposal.audit_id);
    proposal.source = source;
    proposal.target_document_id = "document.code:" + std::to_string(document_id);
    proposal.target_document_numeric_id = document_id;
    proposal.base_document_revision = base_revision;
    proposal.base_content_hash = base_hash;
    proposal.proposed_content_hash = exact_content_hash(proposed_content);
    proposal.generation = operation;
    proposal.pending = true;
    proposal.detail = "Review every editor hunk before applying.";
    if (!register_proposal_task(proposal.task_id, source.session_id,
            proposal.target_document_id, "Review AI editor proposal")) {
        code_editor_widget::cancel_agent_edit();
        return failed("Task Center rejected the proposal-specific editor review task.");
    }
    std::string persistence_error;
    if (!persist_proposal_audit(editor_audit_record(proposal, "pending_review",
            "stage", proposal.detail), persistence_error)) {
        code_editor_widget::cancel_agent_edit();
        const std::string diagnostic_id = "diagnostic." + proposal.audit_id;
        update_proposal_task(proposal.task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Audit persistence failed", persistence_error, diagnostic_id);
        raise_proposal_diagnostic(diagnostic_id, proposal.task_id,
            proposal.target_document_id, "AI editor proposal audit was not durable",
            persistence_error);
        return failed("The editor proposal was not staged because its audit record could not be committed: " +
            persistence_error);
    }
    {
        std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
        s_editor_proposal = proposal;
    }
    update_proposal_task(proposal.task_id,
        aida::ui::task_center::task_state_t::running, 0.35f,
        "Awaiting per-hunk review", proposal.detail);
    action_result_t result = completed("Editor proposal staged for per-hunk review.");
    result.target_view_id = "document.code";
    return result;
}

editor_proposal_snapshot_t editor_proposal_snapshot()
{
    editor_proposal_snapshot_t result;
    bool became_stale = false;
    {
        std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
        if (s_editor_proposal.pending && !s_editor_proposal.stale &&
            (code_editor_widget::active_document_id() !=
                    s_editor_proposal.target_document_numeric_id ||
             code_editor_widget::document_revision() !=
                    s_editor_proposal.base_document_revision ||
             code_editor_widget::document_content_fingerprint() !=
                    s_editor_proposal.base_content_hash)) {
            s_editor_proposal.stale = true;
            s_editor_proposal.detail = "The target code document identity, revision, or content hash changed after staging; explicit revalidation is required.";
            became_stale = true;
        }
        result = s_editor_proposal;
    }
    if (became_stale) {
        std::string persistence_error;
        const bool persisted = persist_proposal_audit(editor_audit_record(result,
            "stale", "stale", result.detail), persistence_error);
        const std::string diagnostic_id = persisted ? std::string{} :
            "diagnostic." + result.audit_id + ".stale";
        update_proposal_task(result.task_id, persisted
                ? aida::ui::task_center::task_state_t::interrupted
                : aida::ui::task_center::task_state_t::failed,
            1.0f, persisted ? "Revalidation required" : "Audit persistence failed",
            persisted ? result.detail : persistence_error, diagnostic_id);
        if (!persisted)
            raise_proposal_diagnostic(diagnostic_id, result.task_id,
                result.target_document_id, "Stale editor proposal audit was not durable",
                persistence_error);
    }
    return result;
}

action_result_t confirm_editor_proposal_review(const editor_proposal_snapshot_t& proposal)
{
    const int pending_hunks = code_editor_widget::pending_hunk_count();
    std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
    if (s_editor_proposal.pending && !s_editor_proposal.stale &&
        s_editor_proposal.generation == proposal.generation &&
        s_editor_proposal.target_document_numeric_id ==
            code_editor_widget::active_document_id() &&
        s_editor_proposal.base_document_revision ==
            code_editor_widget::document_revision() &&
        s_editor_proposal.base_content_hash ==
            code_editor_widget::document_content_fingerprint() &&
        pending_hunks == code_editor_widget::pending_hunk_count() &&
        code_editor_widget::pending_diff().fully_resolved()) {
        s_editor_proposal.reviewed_generation = proposal.generation;
        s_editor_proposal.reviewed_content_hash = proposal.base_content_hash;
        s_editor_proposal.reviewed_pending_hunks = pending_hunks;
        return completed("Editor proposal review confirmed.");
    }
    return failed("The target document or staged hunk set changed before confirmation; review the current proposal again.");
}

action_result_t validate_reverse_engineering_proposal(
    const message_identity_t& source, const nlohmann::json& document,
    disasm_view::workspace_context_t captured_workspace,
    std::uint64_t operation_id)
{
    std::string reason;
    const std::string kind_text = bounded_proposal_text(
        document, "kind", 64U, reason, true);
    if (!reason.empty()) return failed(std::move(reason));
    const auto kind = proposal_kind(kind_text);
    if (kind == reverse_engineering_proposal_kind_t::none)
        return failed("The aida-proposal kind is unsupported.");
    if (!document.contains("target") || !document["target"].is_object())
        return failed("The aida-proposal target must be an object.");
    const auto& target = document["target"];

    reverse_engineering_proposal_snapshot_t proposal;
    reverse_engineering_proposal_binding_t binding;
    {
        std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
        if (s_reverse_engineering_proposal.operation_id != operation_id)
            return failed("The proposal validation was superseded before target resolution.");
        proposal.id = s_reverse_engineering_proposal.id;
        proposal.audit_id = s_reverse_engineering_proposal.audit_id;
        proposal.task_id = s_reverse_engineering_proposal.task_id;
    }
    proposal.source = source;
    proposal.kind = kind;
    proposal.kind_label = proposal_kind_label(kind);
    proposal.generation = operation_id;
    proposal.operation_id = operation_id;
    proposal.state = reverse_engineering_proposal_state_t::valid;
    proposal.provenance = bounded_proposal_text(document, "provenance", 512U, reason, true);
    if (!reason.empty()) return failed(std::move(reason));
    proposal.rationale = bounded_proposal_text(document, "rationale", 2048U, reason, true);
    if (!reason.empty()) return failed(std::move(reason));
    proposal.pending = true;
    proposal.detail = "Review the exact before/after scope and consequence before applying.";

    const bool analysis_kind = kind == reverse_engineering_proposal_kind_t::analysis_rename ||
        kind == reverse_engineering_proposal_kind_t::analysis_comment ||
        kind == reverse_engineering_proposal_kind_t::analysis_type ||
        kind == reverse_engineering_proposal_kind_t::static_patch;
    if (analysis_kind) {
        binding.workspace = std::move(captured_workspace);
        if (!binding.workspace)
            return failed("Open the proposal's analysis workspace before reviewing this change.");
        const std::string workspace_id = bounded_proposal_text(
            target, "workspace_id", 128U, reason, true);
        if (!reason.empty()) return failed(std::move(reason));
        if (workspace_id != binding.workspace.workspace->identity().binary_id().to_hex())
            return failed("The selected workspace does not match the proposal workspace identity.");
        std::uint64_t runtime_address = 0;
        if (!required_u64(target, "address", runtime_address, reason) ||
            !required_u64(target, "generation", proposal.expected_generation, reason) ||
            !required_u64(target, "analysis_revision", proposal.expected_revision, reason) ||
            !required_u64(target, "overlay_revision", proposal.expected_overlay_revision, reason))
            return failed(std::move(reason));
        if (proposal.expected_generation != binding.workspace.publication->generation ||
            proposal.expected_revision != binding.workspace.publication->analysis_revision ||
            proposal.expected_overlay_revision != binding.workspace.workspace->overlay_revision())
            return failed("The analysis generation or revision changed after the proposal was produced.");
        const auto typed = disasm_view::typed_address(binding.workspace, runtime_address);
        if (!typed) return failed("The proposal address is outside the selected workspace.");
        binding.address = *typed;
        char address_label[32]{};
        std::snprintf(address_label, sizeof(address_label), "0x%016llX",
            static_cast<unsigned long long>(runtime_address));
        proposal.target_id = workspace_id + ":" + address_label;
        proposal.target_label = std::string(address_label) + " in " + workspace_id.substr(0, 12U);
        proposal.target_view_id = "document.disassembly";
        proposal.rollback_action_id = "analysis.overlay.undo";

        if (kind == reverse_engineering_proposal_kind_t::analysis_rename ||
            kind == reverse_engineering_proposal_kind_t::analysis_comment ||
            kind == reverse_engineering_proposal_kind_t::analysis_type) {
            if (!document.contains("before"))
                return failed("The proposal is missing its exact 'before' field.");
            proposal.before_value = bounded_proposal_text(document, "before",
                max_proposal_text_bytes, reason);
            if (!reason.empty()) return failed(std::move(reason));
            if (!document.contains("after"))
                return failed("The proposal is missing its exact 'after' field.");
            proposal.after_value = bounded_proposal_text(document, "after",
                max_proposal_text_bytes, reason,
                kind != reverse_engineering_proposal_kind_t::analysis_comment);
            if (!reason.empty()) return failed(std::move(reason));
            const std::string current = kind == reverse_engineering_proposal_kind_t::analysis_rename
                ? disasm_view::resolve_name(binding.workspace, binding.address)
                : kind == reverse_engineering_proposal_kind_t::analysis_comment
                ? disasm_view::comment(binding.workspace, binding.address)
                : overlay_type_at(binding.workspace, binding.address);
            if (proposal.before_value != current)
                return failed("The proposal's before value does not match the current analysis overlay state.");
            if (proposal.after_value == proposal.before_value)
                return failed("The proposal does not change the selected analysis value.");
            if (kind == reverse_engineering_proposal_kind_t::analysis_rename) {
                proposal.consequence = "Changes the displayed symbol name at exactly one address through the reversible analysis overlay; cross-view publications update after commit.";
            } else if (kind == reverse_engineering_proposal_kind_t::analysis_comment) {
                proposal.consequence = "Adds, replaces, or removes the comment at exactly one address through the reversible analysis overlay.";
            } else {
                proposal.consequence = "Applies the canonical type at exactly one address through the reversible analysis overlay and invalidates dependent presentation caches.";
            }
            proposal.reversibility = "Use Analysis Overlay Undo after the terminal commit to restore the previous overlay value.";
        } else {
            const std::string before = bounded_proposal_text(
                document, "before", max_proposal_payload_bytes, reason, true);
            if (!reason.empty() || !parse_hex_bytes(before, max_proposal_patch_bytes,
                    binding.before_bytes, reason)) return failed(std::move(reason));
            const std::string after = bounded_proposal_text(
                document, "after", max_proposal_payload_bytes, reason, true);
            if (!reason.empty() || !parse_hex_bytes(after, max_proposal_patch_bytes,
                    binding.after_bytes, reason)) return failed(std::move(reason));
            if (binding.before_bytes.size() != binding.after_bytes.size())
                return failed("Static patch before/after ranges must have the same length.");
            if (binding.before_bytes == binding.after_bytes)
                return failed("The static patch does not change any bytes.");
            const auto current = static_bytes_at(binding.workspace, binding.address,
                binding.before_bytes.size(), reason);
            if (!reason.empty()) return failed(std::move(reason));
            if (current != binding.before_bytes)
                return failed("The static bytes changed after the proposal was produced.");
            proposal.before_value = format_hex_bytes(binding.before_bytes);
            proposal.after_value = format_hex_bytes(binding.after_bytes);
            proposal.consequence = "Opens the generation-fenced Static Patch Review with the exact AI before/after bytes prefilled. This confirmation does not commit the overlay; Apply Patch in that review is the second explicit human action.";
            proposal.reversibility = "The Static Patch Review can be cancelled before commit and exposes generation-fenced Undo/Revert after a committed overlay patch.";
        }
    } else if (kind == reverse_engineering_proposal_kind_t::live_patch) {
        std::uint64_t address = 0;
        std::uint64_t pid = 0;
        if (!required_u64(target, "address", address, reason) ||
            !required_u64(target, "pid", pid, reason) || pid == 0 ||
            pid > (std::numeric_limits<std::uint32_t>::max)())
            return failed(reason.empty() ? "The live patch PID is invalid." : std::move(reason));
        const std::string before = bounded_proposal_text(
            document, "before", max_proposal_payload_bytes, reason, true);
        if (!reason.empty() || !parse_hex_bytes(before, max_proposal_patch_bytes,
                binding.before_bytes, reason)) return failed(std::move(reason));
        const std::string after = bounded_proposal_text(
            document, "after", max_proposal_payload_bytes, reason, true);
        if (!reason.empty() || !parse_hex_bytes(after, max_proposal_patch_bytes,
                binding.after_bytes, reason)) return failed(std::move(reason));
        if (binding.before_bytes.size() != binding.after_bytes.size())
            return failed("Live patch before/after ranges must have the same length.");
        if (binding.before_bytes == binding.after_bytes)
            return failed("The live patch does not change any bytes.");
        binding.address.value = address;
        binding.target_pid = static_cast<std::uint32_t>(pid);
        if (!driver_bridge::is_loaded() || driver_bridge::attached_pid() != binding.target_pid)
            return failed("Attach the driver-backed debugger to the proposal PID before reviewing the live patch.");
        std::vector<std::uint8_t> current;
        if (!driver_bridge::read_memory(address, binding.before_bytes.size(), current) ||
            driver_bridge::attached_pid() != binding.target_pid ||
            current != binding.before_bytes)
            return failed("The live target bytes are unreadable or changed after the proposal was produced.");
        char target_label[64]{};
        std::snprintf(target_label, sizeof(target_label), "PID %u at 0x%016llX",
            binding.target_pid, static_cast<unsigned long long>(address));
        proposal.target_id = std::string("process:") + std::to_string(binding.target_pid) +
            ":" + std::to_string(address);
        proposal.target_label = target_label;
        proposal.before_value = format_hex_bytes(binding.before_bytes);
        proposal.after_value = format_hex_bytes(binding.after_bytes);
        proposal.target_view_id = "view.debug.patches";
        proposal.rollback_action_id = "debugger.patch.revert";
        proposal.consequence = "Stages the exact live-memory byte range in the debugger Patch Review surface; this confirmation does not write target memory. Applying the staged debugger patch is a second explicit reviewed action.";
        proposal.reversibility = "A subsequently applied debugger patch retains its original bytes and can be reverted from Patches.";
    } else {
        if (!parse_network_identity(target, binding.network_source, reason))
            return failed(std::move(reason));
        const std::string before = bounded_proposal_text(
            document, "before", max_network_request_bytes, reason, true);
        if (!reason.empty()) return failed(std::move(reason));
        const std::string after = bounded_proposal_text(
            document, "after", max_network_request_bytes, reason, true);
        if (!reason.empty()) return failed(std::move(reason));
        network_view::artifact_snapshot_t current;
        if (!network_view::resolve_artifact(binding.network_source, current, reason))
            return failed(std::move(reason));
        const std::vector<std::uint8_t> before_bytes(before.begin(), before.end());
        if (current.bytes != before_bytes)
            return failed("The proposal's request bytes do not match the retained Network artifact.");
        binding.network_after.assign(after.begin(), after.end());
        if (binding.network_after == before_bytes)
            return failed("The Network proposal does not change the retained request.");
        network_view::artifact_identity_t canonical_source;
        if (!network_view::validate_reviewed_request(binding.network_source,
                binding.network_after, canonical_source, reason))
            return failed(std::move(reason));
        binding.network_source = std::move(canonical_source);
        proposal.target_id = binding.network_source.id;
        proposal.target_label = binding.network_source.label.empty()
            ? binding.network_source.id : binding.network_source.label;
        proposal.before_value = before;
        proposal.after_value = after;
        proposal.expected_generation = binding.network_source.timestamp;
        proposal.expected_revision = binding.network_source.revision;
        proposal.expected_overlay_revision = binding.network_source.content_hash;
        proposal.target_view_id = "view.network.repeater";
        proposal.consequence = kind == reverse_engineering_proposal_kind_t::network_replay_staging
            ? "Creates a reviewed Repeater draft with the proposed request. No network traffic is sent until the user separately presses Send in Repeater."
            : "Creates an editable Repeater draft from the retained request and proposed bytes. The captured history remains unchanged and no request is sent.";
        proposal.reversibility = "Close the staged Repeater tab to discard it; captured history and the original artifact are not modified.";
    }

    std::string persistence_error;
    {
        std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
        if (s_reverse_engineering_proposal.operation_id != operation_id)
            return failed("The proposal validation was superseded before publication.");
        if (!persist_proposal_audit(reverse_audit_record(proposal,
                "pending_review", "review", proposal.detail), persistence_error))
            return failed("The validated proposal was not published because its audit transaction failed: " +
                persistence_error);
        s_reverse_engineering_proposal = proposal;
        s_reverse_engineering_proposal_binding = std::move(binding);
        publish_reverse_engineering_proposal_locked();
    }
    update_proposal_task(proposal.task_id,
        aida::ui::task_center::task_state_t::running, 0.4f,
        "Awaiting exact before/after review", proposal.detail);
    action_result_t result = completed("Reverse-engineering proposal staged for exact before/after review.");
    result.target_view_id = "view.ai.evidence";
    return result;
}

void fail_reverse_engineering_proposal(std::uint64_t operation_id,
                                       std::string detail,
                                       std::string summary)
{
    reverse_engineering_proposal_snapshot_t proposal;
    {
        std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
        if (s_reverse_engineering_proposal.operation_id != operation_id) return;
        s_reverse_engineering_proposal.pending = true;
        s_reverse_engineering_proposal.applying = false;
        s_reverse_engineering_proposal.state =
            reverse_engineering_proposal_state_t::error;
        s_reverse_engineering_proposal.stale = true;
        s_reverse_engineering_proposal.diagnostic_id =
            "diagnostic." + s_reverse_engineering_proposal.audit_id + ".failure";
        s_reverse_engineering_proposal.disabled_reason = detail;
        s_reverse_engineering_proposal.detail = detail;
        proposal = s_reverse_engineering_proposal;
        publish_reverse_engineering_proposal_locked();
    }
    std::string persistence_error;
    if (!persist_proposal_audit(reverse_audit_record(proposal, "failure",
            "failure", detail), persistence_error))
        detail += " Audit persistence also failed: " + persistence_error;
    update_proposal_task(proposal.task_id,
        aida::ui::task_center::task_state_t::failed, 1.0f,
        "Proposal failed", detail, proposal.diagnostic_id);
    raise_proposal_diagnostic(proposal.diagnostic_id, proposal.task_id,
        proposal.target_id, std::move(summary), std::move(detail));
}

action_result_t stage_reverse_engineering_proposal(const message_identity_t& source)
{
    if (s_proposal_restore_pending.load(std::memory_order_acquire))
        return failed("Wait for the bounded proposal-review restore to finish before staging another proposal.");
    std::string reason;
    const ChatMessage* message = resolve_message(source, reason);
    if (!message) return failed(std::move(reason));
    nlohmann::json document;
    if (!proposal_document(*message, document, reason)) return failed(std::move(reason));
    const std::string kind_text = bounded_proposal_text(
        document, "kind", 64U, reason, true);
    if (!reason.empty()) return failed(std::move(reason));
    const auto kind = proposal_kind(kind_text);
    if (kind == reverse_engineering_proposal_kind_t::none)
        return failed("The aida-proposal kind is unsupported.");
    if (!document.contains("target") || !document["target"].is_object())
        return failed("The aida-proposal target must be an object.");
    const std::string provenance = bounded_proposal_text(
        document, "provenance", 512U, reason, true);
    if (!reason.empty()) return failed(std::move(reason));
    const std::string rationale = bounded_proposal_text(
        document, "rationale", 2048U, reason, true);
    if (!reason.empty()) return failed(std::move(reason));

    const bool analysis_kind = kind == reverse_engineering_proposal_kind_t::analysis_rename ||
        kind == reverse_engineering_proposal_kind_t::analysis_comment ||
        kind == reverse_engineering_proposal_kind_t::analysis_type ||
        kind == reverse_engineering_proposal_kind_t::static_patch;
    disasm_view::workspace_context_t workspace;
    if (analysis_kind) workspace = disasm_view::capture_selected_workspace();
    const std::uint64_t operation_id =
        s_reverse_engineering_proposal_operation.fetch_add(1, std::memory_order_acq_rel);
    reverse_engineering_proposal_snapshot_t queued_proposal;
    {
        std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
        if (s_reverse_engineering_proposal.pending)
            return failed("Review, apply, or reject the currently pending reverse-engineering proposal first.");
        s_reverse_engineering_proposal = {};
        s_reverse_engineering_proposal.id =
            stable_proposal_id("reverse", source, operation_id);
        s_reverse_engineering_proposal.audit_id =
            s_reverse_engineering_proposal.id;
        s_reverse_engineering_proposal.task_id =
            proposal_task_id(s_reverse_engineering_proposal.audit_id);
        s_reverse_engineering_proposal.source = source;
        s_reverse_engineering_proposal.kind = kind;
        s_reverse_engineering_proposal.kind_label = proposal_kind_label(kind);
        s_reverse_engineering_proposal.provenance = provenance;
        s_reverse_engineering_proposal.rationale = rationale;
        s_reverse_engineering_proposal.generation = operation_id;
        s_reverse_engineering_proposal.operation_id = operation_id;
        s_reverse_engineering_proposal.state =
            reverse_engineering_proposal_state_t::queued;
        s_reverse_engineering_proposal.pending = true;
        s_reverse_engineering_proposal.detail =
            "Proposal validation is queued on the bounded AI review executor.";
        s_reverse_engineering_proposal_binding = {};
        queued_proposal = s_reverse_engineering_proposal;
        publish_reverse_engineering_proposal_locked();
    }
    if (!register_proposal_task(queued_proposal.task_id, source.session_id,
            "unresolved:" + queued_proposal.kind_label,
            "Review AI reverse-engineering proposal")) {
        std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
        if (s_reverse_engineering_proposal.operation_id == operation_id) {
            s_reverse_engineering_proposal.pending = false;
            s_reverse_engineering_proposal.state =
                reverse_engineering_proposal_state_t::error;
            s_reverse_engineering_proposal.stale = true;
            s_reverse_engineering_proposal.detail =
                "Task Center rejected the proposal-specific review task.";
            s_reverse_engineering_proposal.disabled_reason =
                s_reverse_engineering_proposal.detail;
            publish_reverse_engineering_proposal_locked();
        }
        return failed("Task Center rejected the proposal-specific reverse-engineering review task.");
    }
    std::string persistence_error;
    if (!persist_proposal_audit(reverse_audit_record(queued_proposal,
            "validating", "stage", queued_proposal.detail), persistence_error)) {
        const std::string diagnostic_id =
            "diagnostic." + queued_proposal.audit_id + ".persistence";
        update_proposal_task(queued_proposal.task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Audit persistence failed", persistence_error, diagnostic_id);
        raise_proposal_diagnostic(diagnostic_id, queued_proposal.task_id,
            queued_proposal.target_id,
            "Reverse-engineering proposal audit was not durable", persistence_error);
        std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
        if (s_reverse_engineering_proposal.operation_id == operation_id) {
            s_reverse_engineering_proposal.pending = false;
            s_reverse_engineering_proposal.state =
                reverse_engineering_proposal_state_t::error;
            s_reverse_engineering_proposal.stale = true;
            s_reverse_engineering_proposal.detail = persistence_error;
            s_reverse_engineering_proposal.disabled_reason = persistence_error;
            s_reverse_engineering_proposal.diagnostic_id = diagnostic_id;
            publish_reverse_engineering_proposal_locked();
        }
        return failed("The proposal was not staged because its audit transaction failed: " +
            persistence_error);
    }
    const bool submitted = submit_proposal_job(
        "ai.proposal.validate",
        [source, document, workspace = std::move(workspace), operation_id]() mutable {
            try {
                reverse_engineering_proposal_snapshot_t running;
                {
                    std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
                    if (s_reverse_engineering_proposal.operation_id != operation_id ||
                        !s_reverse_engineering_proposal.pending)
                        return;
                    s_reverse_engineering_proposal.state =
                        reverse_engineering_proposal_state_t::running;
                    s_reverse_engineering_proposal.detail =
                        "Validating proposal identity, exact before value, and retained target.";
                    running = s_reverse_engineering_proposal;
                    publish_reverse_engineering_proposal_locked();
                }
                update_proposal_task(running.task_id,
                    aida::ui::task_center::task_state_t::running, 0.15f,
                    "Validating target identity and revision", running.detail);
                const auto validation = validate_reverse_engineering_proposal(
                    source, document, std::move(workspace), operation_id);
                if (validation.succeeded) return;
                fail_reverse_engineering_proposal(operation_id, validation.detail,
                    "AI proposal validation failed");
            } catch (const std::exception& exception) {
                fail_reverse_engineering_proposal(operation_id,
                    std::string("Proposal validation raised an exception: ") + exception.what(),
                    "AI proposal validation failed");
            } catch (...) {
                fail_reverse_engineering_proposal(operation_id,
                    "Proposal validation raised an unknown exception.",
                    "AI proposal validation failed");
            }
        });
    if (!submitted) {
        fail_reverse_engineering_proposal(operation_id,
            "The bounded AI proposal validator rejected the task.",
            "AI proposal validation dispatch failed");
        return failed("The bounded AI proposal validator rejected the task.");
    }
    action_result_t result = completed("Proposal validation queued.");
    result.target_view_id = "view.ai.evidence";
    return result;
}

std::shared_ptr<const reverse_engineering_proposal_snapshot_t>
reverse_engineering_proposal_snapshot()
{
    return std::atomic_load_explicit(&s_reverse_engineering_proposal_publication,
        std::memory_order_acquire);
}

std::uint64_t parse_audit_hash(const std::string& text)
{
    if (text.empty() || text.size() > 16U) return 0;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
        value, 16);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
        ? value : 0;
}

void install_restored_proposal_reviews(
    const std::string& session_id, std::uint64_t operation,
    std::vector<aida::session::proposal_audit_record_t> records)
{
    if (s_proposal_restore_operation.load(std::memory_order_acquire) != operation ||
        get_chat_session_id_locked() != session_id)
        return;
    s_proposal_restore_pending.store(false, std::memory_order_release);
    bool restored_editor = false;
    bool restored_reverse = false;
    for (const auto& record : records) {
        if (record.family == "editor" && !restored_editor) {
            editor_proposal_snapshot_t restored;
            restored.id = record.proposal_id;
            restored.audit_id = record.audit_id;
            restored.task_id = record.task_id;
            restored.diagnostic_id = record.diagnostic_id;
            restored.source.session_id = record.session_id;
            restored.source.index = static_cast<std::size_t>(record.source_index);
            restored.source.timestamp = record.source_timestamp;
            restored.source.fingerprint = record.source_fingerprint;
            restored.target_document_id = record.target_id;
            const std::string prefix = "document.code:";
            if (record.target_id.rfind(prefix, 0) == 0) {
                const std::string numeric = record.target_id.substr(prefix.size());
                static_cast<void>(std::from_chars(numeric.data(),
                    numeric.data() + numeric.size(),
                    restored.target_document_numeric_id));
            }
            restored.base_document_revision = record.source_revision;
            restored.base_content_hash = parse_audit_hash(record.before_hash);
            restored.proposed_content_hash = parse_audit_hash(record.after_hash);
            restored.generation = record.target_generation;
            restored.pending = true;
            restored.stale = true;
            restored.detail = record.detail;
            bool installed = false;
            {
                std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
                if (!s_editor_proposal.pending && !s_editor_proposal.applying) {
                    s_editor_proposal = restored;
                    installed = true;
                }
            }
            if (installed) {
                retain_editor_proposal_hunks(restored.audit_id, record.hunks);
                static_cast<void>(register_proposal_task(restored.task_id,
                    record.session_id, record.target_id,
                    "Restored AI editor proposal"));
                update_proposal_task(restored.task_id,
                    aida::ui::task_center::task_state_t::interrupted, 1.0f,
                    "Revalidation required", restored.detail);
            }
            restored_editor = true;
        } else if (record.family == "reverse_engineering" && !restored_reverse) {
            reverse_engineering_proposal_snapshot_t restored;
            restored.id = record.proposal_id;
            restored.audit_id = record.audit_id;
            restored.task_id = record.task_id;
            restored.diagnostic_id = record.diagnostic_id;
            restored.source.session_id = record.session_id;
            restored.source.index = static_cast<std::size_t>(record.source_index);
            restored.source.timestamp = record.source_timestamp;
            restored.source.fingerprint = record.source_fingerprint;
            restored.kind = proposal_kind(record.kind);
            restored.kind_label = proposal_kind_label(restored.kind);
            restored.target_id = record.target_id;
            restored.target_label = record.target_id;
            restored.target_view_id = record.target_view_id;
            restored.expected_generation = record.target_generation;
            restored.expected_revision = record.source_revision;
            restored.expected_overlay_revision = record.target_overlay_revision;
            restored.before_value = "Restored before hash: " + record.before_hash;
            restored.after_value = "Restored after hash: " + record.after_hash;
            restored.provenance = record.provenance;
            restored.rationale = record.detail;
            restored.rollback_action_id = record.undo_revert_identity;
            restored.generation = record.target_generation;
            restored.operation_id =
                s_reverse_engineering_proposal_operation.fetch_add(
                    1, std::memory_order_acq_rel);
            restored.state = reverse_engineering_proposal_state_t::stale;
            restored.pending = true;
            restored.stale = true;
            restored.disabled_reason = record.detail;
            restored.detail = record.detail;
            bool installed = false;
            {
                std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
                if (!s_reverse_engineering_proposal.pending &&
                    !s_reverse_engineering_proposal.applying) {
                    s_reverse_engineering_proposal = restored;
                    s_reverse_engineering_proposal_binding = {};
                    publish_reverse_engineering_proposal_locked();
                    installed = true;
                }
            }
            if (installed) {
                static_cast<void>(register_proposal_task(restored.task_id,
                    record.session_id, record.target_id,
                    "Restored AI reverse-engineering proposal"));
                update_proposal_task(restored.task_id,
                    aida::ui::task_center::task_state_t::interrupted, 1.0f,
                    "Revalidation required", restored.detail,
                    restored.diagnostic_id);
            }
            restored_reverse = true;
        }
        if (restored_editor && restored_reverse) break;
    }
}

void restore_proposal_reviews_for_session(const std::string& session_id)
{
    const std::uint64_t previous_operation =
        s_proposal_restore_operation.fetch_add(1, std::memory_order_acq_rel);
    const std::uint64_t operation = previous_operation + 1U;
    if (s_proposal_restore_pending.exchange(false,
            std::memory_order_acq_rel)) {
        update_proposal_task("task.proposal.restore." +
                std::to_string(previous_operation),
            aida::ui::task_center::task_state_t::interrupted, 1.0f,
            "Restore superseded by another conversation",
            "A newer conversation identity superseded this restore.");
    }
    editor_proposal_snapshot_t displaced_editor;
    bool editor_displaced = false;
    {
        std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
        if ((s_editor_proposal.pending || s_editor_proposal.applying) &&
            !s_editor_proposal.source.session_id.empty() &&
            s_editor_proposal.source.session_id != session_id) {
            s_editor_proposal.pending = true;
            s_editor_proposal.applying = false;
            s_editor_proposal.stale = true;
            s_editor_proposal.detail =
                "The conversation changed before proposal completion; explicit revalidation is required after restore.";
            displaced_editor = s_editor_proposal;
            s_editor_proposal = {};
            editor_displaced = true;
        }
    }
    if (editor_displaced) {
        std::string persistence_error;
        const bool persisted = persist_proposal_audit(editor_audit_record(
            displaced_editor, "stale", "stale", displaced_editor.detail),
            persistence_error);
        update_proposal_task(displaced_editor.task_id, persisted
                ? aida::ui::task_center::task_state_t::interrupted
                : aida::ui::task_center::task_state_t::failed,
            1.0f, persisted ? "Conversation changed" :
                "Session-switch audit persistence failed",
            persisted ? displaced_editor.detail : persistence_error);
        code_editor_widget::cancel_agent_edit();
    }
    reverse_engineering_proposal_snapshot_t displaced_reverse;
    bool reverse_displaced = false;
    {
        std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
        if ((s_reverse_engineering_proposal.pending ||
                s_reverse_engineering_proposal.applying) &&
            !s_reverse_engineering_proposal.source.session_id.empty() &&
            s_reverse_engineering_proposal.source.session_id != session_id) {
            s_reverse_engineering_proposal.operation_id =
                s_reverse_engineering_proposal_operation.fetch_add(
                    1, std::memory_order_acq_rel);
            s_reverse_engineering_proposal.applying = false;
            s_reverse_engineering_proposal.stale = true;
            s_reverse_engineering_proposal.state =
                reverse_engineering_proposal_state_t::stale;
            s_reverse_engineering_proposal.detail =
                "The conversation changed before proposal completion; explicit target revalidation is required after restore.";
            s_reverse_engineering_proposal.disabled_reason =
                s_reverse_engineering_proposal.detail;
            displaced_reverse = s_reverse_engineering_proposal;
            s_reverse_engineering_proposal = {};
            s_reverse_engineering_proposal_binding = {};
            publish_reverse_engineering_proposal_locked();
            reverse_displaced = true;
        }
    }
    if (reverse_displaced) {
        std::string persistence_error;
        const bool persisted = persist_proposal_audit(reverse_audit_record(
            displaced_reverse, "stale", "stale", displaced_reverse.detail),
            persistence_error);
        update_proposal_task(displaced_reverse.task_id, persisted
                ? aida::ui::task_center::task_state_t::interrupted
                : aida::ui::task_center::task_state_t::failed,
            1.0f, persisted ? "Conversation changed" :
                "Session-switch audit persistence failed",
            persisted ? displaced_reverse.detail : persistence_error);
    }
    if (session_id.empty()) return;
    const std::string restore_task_id = "task.proposal.restore." +
        std::to_string(operation);
    if (!register_proposal_task(restore_task_id, session_id, session_id,
            "Restore AI proposal reviews")) {
        const std::string diagnostic_id = "diagnostic.proposal.restore." +
            std::to_string(operation);
        raise_proposal_diagnostic(diagnostic_id, {}, session_id,
            "AI proposal review restore could not start",
            "Task Center rejected restore ownership before executor submission.");
        return;
    }
    s_proposal_restore_pending.store(true, std::memory_order_release);
    update_proposal_task(restore_task_id,
        aida::ui::task_center::task_state_t::running, 0.1f,
        "Restoring durable proposal audit state",
        "Reading only bounded stale/revalidation-required proposal records; no proposal can be applied automatically.");
    const bool submitted = submit_proposal_job("ai.proposal.restore",
        [session_id, operation, restore_task_id] {
            std::vector<aida::session::proposal_audit_record_t> records;
            const bool restored = aida::session::restore_proposal_audits(
                session_id, records, 16U);
            std::string error = restored ? std::string{} : aida::session::last_error();
            aida::ui_thread::post_options_t options;
            options.subsystem = "ai_proposal";
            options.label = "proposal.restore_completion";
            options.phase = "restore";
            options.owner = "ai_proposal";
            options.priority = aida::ui_thread::priority_t::high;
            const auto posted = aida::ui_thread::post(
                [session_id, operation, restore_task_id, restored,
                 error = std::move(error),
                 records = std::move(records)]() mutable {
                    if (s_proposal_restore_operation.load(
                            std::memory_order_acquire) != operation ||
                        get_chat_session_id_locked() != session_id) {
                        update_proposal_task(restore_task_id,
                            aida::ui::task_center::task_state_t::interrupted,
                            1.0f, "Restore superseded by another conversation",
                            "A newer conversation identity superseded this restore before UI publication.");
                        return;
                    }
                    if (!restored) {
                        s_proposal_restore_pending.store(false,
                            std::memory_order_release);
                        const std::string diagnostic_id =
                            "diagnostic.proposal.restore." +
                            std::to_string(operation);
                        raise_proposal_diagnostic(diagnostic_id, {}, session_id,
                            "AI proposal review restore failed", error);
                        update_proposal_task(restore_task_id,
                            aida::ui::task_center::task_state_t::failed, 1.0f,
                            "Proposal review restore failed", error,
                            diagnostic_id);
                        diag::log_tagged_fmt("chat",
                            "proposal_audit_restore_failed session='%.128s' error='%.512s'",
                            session_id.c_str(), error.c_str());
                        return;
                    }
                    install_restored_proposal_reviews(session_id, operation,
                        std::move(records));
                    update_proposal_task(restore_task_id,
                        aida::ui::task_center::task_state_t::completed, 1.0f,
                        "Proposal review restore complete",
                        "Durable proposal records were restored as stale and require explicit target revalidation.");
                }, std::move(options));
            if (posted != aida::ui_thread::enqueue_result_t::accepted) {
                const bool current = s_proposal_restore_operation.load(
                    std::memory_order_acquire) == operation;
                if (current)
                    s_proposal_restore_pending.store(false,
                        std::memory_order_release);
                update_proposal_task(restore_task_id, current
                        ? aida::ui::task_center::task_state_t::failed
                        : aida::ui::task_center::task_state_t::interrupted,
                    1.0f, current ? "Restore completion dispatch failed" :
                        "Restore superseded by another conversation",
                    current ? "The bounded UI dispatcher rejected the proposal restore completion."
                        : "A newer conversation identity superseded this restore.");
                diag::log_tagged_fmt("chat",
                    "proposal_audit_restore_dispatch_failed session='%.128s'",
                    session_id.c_str());
            }
        });
    if (!submitted) {
        s_proposal_restore_pending.store(false, std::memory_order_release);
        const std::string diagnostic_id = "diagnostic.proposal.restore." +
            std::to_string(operation);
        raise_proposal_diagnostic(diagnostic_id, {}, session_id,
            "AI proposal review restore could not start",
            "The bounded proposal restore executor rejected the request.");
        update_proposal_task(restore_task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Proposal restore dispatch failed",
            "The bounded proposal restore executor rejected the request.",
            diagnostic_id);
    }
}

void prepare_proposal_reviews_for_shutdown()
{
    const std::uint64_t restore_operation =
        s_proposal_restore_operation.fetch_add(1, std::memory_order_acq_rel);
    if (s_proposal_restore_pending.exchange(false,
            std::memory_order_acq_rel)) {
        update_proposal_task("task.proposal.restore." +
                std::to_string(restore_operation),
            aida::ui::task_center::task_state_t::interrupted, 1.0f,
            "Restore interrupted by shutdown",
            "The IDE shut down before proposal restore publication.");
    }
    editor_proposal_snapshot_t editor;
    bool editor_active = false;
    {
        std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
        if (s_editor_proposal.pending || s_editor_proposal.applying) {
            s_editor_proposal.applying = false;
            s_editor_proposal.stale = true;
            s_editor_proposal.detail =
                "The IDE closed before proposal completion; explicit revalidation is required after restore.";
            editor = s_editor_proposal;
            editor_active = true;
        }
    }
    if (editor_active) {
        std::string persistence_error;
        const bool persisted = persist_proposal_audit(editor_audit_record(editor,
            "stale", "stale", editor.detail), persistence_error);
        update_proposal_task(editor.task_id, persisted
                ? aida::ui::task_center::task_state_t::interrupted
                : aida::ui::task_center::task_state_t::failed,
            1.0f, persisted ? "Interrupted by shutdown" :
                "Shutdown audit persistence failed",
            persisted ? editor.detail : persistence_error);
    }

    reverse_engineering_proposal_snapshot_t reverse;
    bool reverse_active = false;
    {
        std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
        if (s_reverse_engineering_proposal.pending ||
            s_reverse_engineering_proposal.applying) {
            s_reverse_engineering_proposal.operation_id =
                s_reverse_engineering_proposal_operation.fetch_add(
                    1, std::memory_order_acq_rel);
            s_reverse_engineering_proposal.applying = false;
            s_reverse_engineering_proposal.stale = true;
            s_reverse_engineering_proposal.state =
                reverse_engineering_proposal_state_t::stale;
            s_reverse_engineering_proposal.detail =
                "The IDE closed before proposal completion; explicit target revalidation is required after restore.";
            s_reverse_engineering_proposal.disabled_reason =
                s_reverse_engineering_proposal.detail;
            reverse = s_reverse_engineering_proposal;
            s_reverse_engineering_proposal_binding = {};
            publish_reverse_engineering_proposal_locked();
            reverse_active = true;
        }
    }
    if (reverse_active) {
        std::string persistence_error;
        const bool persisted = persist_proposal_audit(reverse_audit_record(reverse,
            "stale", "stale", reverse.detail), persistence_error);
        update_proposal_task(reverse.task_id, persisted
                ? aida::ui::task_center::task_state_t::interrupted
                : aida::ui::task_center::task_state_t::failed,
            1.0f, persisted ? "Interrupted by shutdown" :
                "Shutdown audit persistence failed",
            persisted ? reverse.detail : persistence_error);
    }
}

void finish_reverse_engineering_proposal(std::uint64_t operation_id,
                                         bool succeeded,
                                         std::string detail,
                                         bool terminal_readback,
                                         std::string failure_outcome = "failure",
                                         std::uint64_t result_revision = 0,
                                         std::uint64_t result_hash = 0,
                                         std::string result_identity = {},
                                         bool mutation_committed = false)
{
    reverse_engineering_proposal_snapshot_t proposal;
    {
        std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
        if (s_reverse_engineering_proposal.operation_id != operation_id) return;
        s_reverse_engineering_proposal.applying = false;
        s_reverse_engineering_proposal.pending = !succeeded && !mutation_committed;
        s_reverse_engineering_proposal.review_staged = succeeded && !terminal_readback;
        s_reverse_engineering_proposal.applied = succeeded && terminal_readback;
        s_reverse_engineering_proposal.partial = mutation_committed;
        s_reverse_engineering_proposal.terminal_readback =
            succeeded && terminal_readback;
        s_reverse_engineering_proposal.stale = !succeeded;
        s_reverse_engineering_proposal.state = succeeded ? terminal_readback
            ? reverse_engineering_proposal_state_t::applied
            : reverse_engineering_proposal_state_t::staged_review
            : failure_outcome == "stale"
            ? reverse_engineering_proposal_state_t::stale
            : reverse_engineering_proposal_state_t::error;
        s_reverse_engineering_proposal.disabled_reason = succeeded
            ? std::string{} : detail;
        s_reverse_engineering_proposal.detail = std::move(detail);
        if (succeeded || mutation_committed) {
            s_reverse_engineering_proposal.result_revision = result_revision != 0
                ? result_revision
                : (terminal_readback || mutation_committed) &&
                        s_reverse_engineering_proposal.expected_overlay_revision !=
                            (std::numeric_limits<std::uint64_t>::max)()
                ? s_reverse_engineering_proposal.expected_overlay_revision + 1U
                : s_reverse_engineering_proposal.expected_revision;
            s_reverse_engineering_proposal.result_hash = result_hash != 0
                ? result_hash
                : exact_content_hash(s_reverse_engineering_proposal.after_value);
            if (!result_identity.empty())
                s_reverse_engineering_proposal.rollback_action_id =
                    std::move(result_identity);
            else if (!s_reverse_engineering_proposal.rollback_action_id.empty())
                s_reverse_engineering_proposal.rollback_action_id += ":" +
                    s_reverse_engineering_proposal.target_id + ":" +
                    std::to_string(
                        s_reverse_engineering_proposal.result_revision);
            if (mutation_committed)
                s_reverse_engineering_proposal.detail +=
                    " Rollback identity: " +
                    s_reverse_engineering_proposal.rollback_action_id + ".";
        } else {
            s_reverse_engineering_proposal.diagnostic_id =
                "diagnostic." + s_reverse_engineering_proposal.audit_id + "." +
                failure_outcome;
        }
        proposal = s_reverse_engineering_proposal;
        publish_reverse_engineering_proposal_locked();
    }
    if (!succeeded && mutation_committed) {
        proposal.diagnostic_id = "diagnostic." + proposal.audit_id + ".partial";
        std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
        if (s_reverse_engineering_proposal.operation_id == operation_id) {
            s_reverse_engineering_proposal.diagnostic_id = proposal.diagnostic_id;
            publish_reverse_engineering_proposal_locked();
        }
    }
    const std::string lifecycle = succeeded
        ? terminal_readback ? "applied" : "review_staged"
        : mutation_committed ? "partial"
        : failure_outcome == "stale" ? "stale" : "failure";
    const std::string outcome = succeeded
        ? terminal_readback ? "apply" : "partial"
        : mutation_committed ? "partial"
        : failure_outcome == "stale" ? "stale" : "failure";
    std::string persistence_error;
    const bool persisted = persist_proposal_audit(reverse_audit_record(proposal,
        lifecycle, outcome, proposal.detail), persistence_error);
    if (!persisted) {
        const std::string diagnostic_id =
            "diagnostic." + proposal.audit_id + ".persistence";
        const std::string combined = proposal.detail +
            " Audit persistence failed: " + persistence_error;
        update_proposal_task(proposal.task_id,
            succeeded || mutation_committed
                ? aida::ui::task_center::task_state_t::partial
                : aida::ui::task_center::task_state_t::failed, 1.0f,
            "Authoritative result was not durably audited", combined, diagnostic_id);
        raise_proposal_diagnostic(diagnostic_id, proposal.task_id, proposal.target_id,
            "AI proposal result audit was not durable", combined);
        return;
    }
    if (succeeded) {
        update_proposal_task(proposal.task_id, terminal_readback
                ? aida::ui::task_center::task_state_t::completed
                : aida::ui::task_center::task_state_t::partial,
            1.0f, terminal_readback ? "Applied and verified" :
                "Staged for authoritative downstream review", proposal.detail);
    } else {
        update_proposal_task(proposal.task_id, mutation_committed
                ? aida::ui::task_center::task_state_t::partial
                : failure_outcome == "stale"
                ? aida::ui::task_center::task_state_t::interrupted
                : aida::ui::task_center::task_state_t::failed,
            1.0f, mutation_committed ? "Committed; terminal verification failed" :
                failure_outcome == "stale" ? "Revalidation required" :
                "Proposal failed", proposal.detail, proposal.diagnostic_id);
        raise_proposal_diagnostic(proposal.diagnostic_id, proposal.task_id,
            proposal.target_id,
            mutation_committed
                ? "AI proposal committed but terminal verification failed"
                : failure_outcome == "stale"
                ? "Reviewed AI proposal became stale before dispatch"
                : "Reviewed AI proposal did not reach authoritative completion",
            proposal.detail);
    }
}

bool revalidate_reverse_engineering_proposal(
    const reverse_engineering_proposal_snapshot_t& proposal,
    reverse_engineering_proposal_binding_t& binding,
    std::string& reason)
{
    const bool analysis_kind =
        proposal.kind == reverse_engineering_proposal_kind_t::analysis_rename ||
        proposal.kind == reverse_engineering_proposal_kind_t::analysis_comment ||
        proposal.kind == reverse_engineering_proposal_kind_t::analysis_type ||
        proposal.kind == reverse_engineering_proposal_kind_t::static_patch;
    if (analysis_kind) {
        const auto& context = binding.workspace;
        if (!context || context.workspace->closing() || context.workspace->closed() ||
            context.workspace->generation() != proposal.expected_generation ||
            context.workspace->analysis_revision() != proposal.expected_revision ||
            context.workspace->overlay_revision() != proposal.expected_overlay_revision) {
            reason = "The exact analysis publication fence changed before apply revalidation.";
            return false;
        }
        std::string current;
        if (proposal.kind == reverse_engineering_proposal_kind_t::analysis_rename)
            current = disasm_view::resolve_name(context, binding.address);
        else if (proposal.kind == reverse_engineering_proposal_kind_t::analysis_comment)
            current = disasm_view::comment(context, binding.address);
        else if (proposal.kind == reverse_engineering_proposal_kind_t::analysis_type)
            current = overlay_type_at(context, binding.address);
        else {
            const auto bytes = static_bytes_at(context, binding.address,
                binding.before_bytes.size(), reason);
            if (!reason.empty()) return false;
            if (bytes != binding.before_bytes) {
                reason = "The exact static patch bytes changed before apply revalidation.";
                return false;
            }
            current = proposal.before_value;
        }
        if (current != proposal.before_value) {
            reason = "The reviewed analysis before value changed before apply revalidation.";
            return false;
        }
        reason.clear();
        return true;
    }
    if (proposal.kind == reverse_engineering_proposal_kind_t::live_patch) {
        std::vector<std::uint8_t> current;
        if (!driver_bridge::is_loaded() ||
            driver_bridge::attached_pid() != binding.target_pid ||
            !driver_bridge::read_memory(binding.address.value,
                binding.before_bytes.size(), current) ||
            driver_bridge::attached_pid() != binding.target_pid ||
            current != binding.before_bytes) {
            reason = "The proposal PID, attachment, or exact live bytes changed before staging.";
            return false;
        }
        reason.clear();
        return true;
    }
    network_view::artifact_snapshot_t current;
    if (!network_view::resolve_artifact(binding.network_source, current, reason))
        return false;
    const std::vector<std::uint8_t> before(
        proposal.before_value.begin(), proposal.before_value.end());
    if (current.bytes != before) {
        reason = "The retained Network request changed before Repeater staging.";
        return false;
    }
    network_view::artifact_identity_t canonical_source;
    if (!network_view::validate_reviewed_request(binding.network_source,
            binding.network_after, canonical_source, reason)) return false;
    binding.network_source = std::move(canonical_source);
    reason.clear();
    return true;
}

action_result_t queue_reverse_engineering_proposal_apply(
    const reverse_engineering_proposal_snapshot_t& proposal)
{
    reverse_engineering_proposal_binding_t binding;
    reverse_engineering_proposal_snapshot_t applying_proposal;
    const std::uint64_t operation_id =
        s_reverse_engineering_proposal_operation.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
        if (!s_reverse_engineering_proposal.pending ||
            s_reverse_engineering_proposal.operation_id != proposal.operation_id ||
            s_reverse_engineering_proposal.state !=
                reverse_engineering_proposal_state_t::valid)
            return failed("The proposal generation changed; review the current proposal.");
        binding = s_reverse_engineering_proposal_binding;
        s_reverse_engineering_proposal.operation_id = operation_id;
        s_reverse_engineering_proposal.applying = true;
        s_reverse_engineering_proposal.state =
            reverse_engineering_proposal_state_t::applying;
        s_reverse_engineering_proposal.detail =
            "Apply-time identity and before-value revalidation is queued.";
        applying_proposal = s_reverse_engineering_proposal;
        publish_reverse_engineering_proposal_locked();
    }
    std::string persistence_error;
    if (!persist_proposal_audit(reverse_audit_record(applying_proposal,
            "reviewed", "review", "The human confirmed the exact proposal scope and consequence."),
            persistence_error) ||
        !persist_proposal_audit(reverse_audit_record(applying_proposal,
            "applying", "apply", applying_proposal.detail), persistence_error)) {
        {
            std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
            if (s_reverse_engineering_proposal.operation_id == operation_id) {
                s_reverse_engineering_proposal.applying = false;
                s_reverse_engineering_proposal.stale = true;
                s_reverse_engineering_proposal.state =
                    reverse_engineering_proposal_state_t::error;
                s_reverse_engineering_proposal.diagnostic_id =
                    "diagnostic." + applying_proposal.audit_id + ".persistence";
                s_reverse_engineering_proposal.disabled_reason = persistence_error;
                s_reverse_engineering_proposal.detail = persistence_error;
                applying_proposal = s_reverse_engineering_proposal;
                publish_reverse_engineering_proposal_locked();
            }
        }
        update_proposal_task(applying_proposal.task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Apply audit persistence failed", persistence_error,
            applying_proposal.diagnostic_id);
        raise_proposal_diagnostic(applying_proposal.diagnostic_id,
            applying_proposal.task_id, applying_proposal.target_id,
            "AI proposal apply was blocked by audit persistence",
            persistence_error);
        return failed("Apply was blocked because the proposal audit transaction failed: " +
            persistence_error);
    }
    update_proposal_task(applying_proposal.task_id,
        aida::ui::task_center::task_state_t::running, 0.65f,
        "Revalidating before authoritative dispatch", applying_proposal.detail);
    const bool submitted = submit_proposal_job(
        "ai.proposal.apply",
        [proposal, binding = std::move(binding), operation_id]() mutable {
          try {
            std::string reason;
            if (!revalidate_reverse_engineering_proposal(proposal, binding, reason)) {
                finish_reverse_engineering_proposal(operation_id, false,
                    reason.empty() ? "Apply-time proposal revalidation failed." :
                        std::move(reason), false, "stale");
                return;
            }
            if (proposal.kind == reverse_engineering_proposal_kind_t::analysis_rename ||
                proposal.kind == reverse_engineering_proposal_kind_t::analysis_comment ||
                proposal.kind == reverse_engineering_proposal_kind_t::analysis_type) {
                auto completion = [operation_id, proposal,
                                   context = binding.workspace,
                                   address = binding.address](
                    bool succeeded, std::string error) {
                    bool verified = false;
                    try {
                        if (succeeded && context.workspace &&
                            proposal.expected_generation !=
                                (std::numeric_limits<std::uint64_t>::max)() &&
                            proposal.expected_overlay_revision !=
                                (std::numeric_limits<std::uint64_t>::max)()) {
                            auto terminal = context;
                            terminal.publication =
                                context.workspace->analysis_publication();
                            terminal.image = terminal.publication &&
                                    terminal.publication->snapshot
                                ? terminal.publication->snapshot->image
                                : nullptr;
                            const bool terminal_fence = terminal.publication &&
                                terminal.publication->generation ==
                                    proposal.expected_generation + 1U &&
                                terminal.publication->analysis_revision ==
                                    proposal.expected_revision &&
                                terminal.publication->overlay_revision ==
                                    proposal.expected_overlay_revision + 1U;
                            if (terminal_fence && proposal.kind ==
                                    reverse_engineering_proposal_kind_t::analysis_rename)
                                verified = disasm_view::resolve_name(
                                    terminal, address) == proposal.after_value;
                            else if (terminal_fence && proposal.kind ==
                                    reverse_engineering_proposal_kind_t::analysis_comment)
                                verified = disasm_view::comment(
                                    terminal, address) == proposal.after_value;
                            else if (terminal_fence && proposal.kind ==
                                    reverse_engineering_proposal_kind_t::analysis_type)
                                verified = overlay_type_at(
                                    terminal, address) == proposal.after_value;
                            verified = verified &&
                                context.workspace->analysis_publication() ==
                                    terminal.publication;
                        }
                    } catch (const std::exception& exception) {
                        if (succeeded)
                            error = std::string(
                                "The authoritative analysis overlay committed, but exact terminal readback failed: ") +
                                exception.what();
                    } catch (...) {
                        if (succeeded)
                            error = "The authoritative analysis overlay committed, but exact terminal readback failed.";
                    }
                    if (succeeded && !verified && error.empty())
                        error = "The authoritative analysis overlay committed, but its exact generation-fenced terminal readback did not match the proposal.";
                    finish_reverse_engineering_proposal(operation_id,
                        succeeded && verified,
                        verified
                            ? "The authoritative analysis overlay committed and its proposal-specific completion matched."
                            : error.empty()
                            ? "The authoritative analysis overlay rejected the proposal mutation."
                            : std::move(error), verified, "failure", 0, 0, {},
                        succeeded && !verified);
                };
                bool accepted = false;
                if (proposal.kind == reverse_engineering_proposal_kind_t::analysis_rename)
                    accepted = disasm_view::queue_rename(binding.workspace, binding.address,
                        proposal.after_value, proposal.expected_generation,
                        proposal.expected_revision, proposal.expected_overlay_revision,
                        completion);
                else if (proposal.kind == reverse_engineering_proposal_kind_t::analysis_comment)
                    accepted = disasm_view::queue_comment(binding.workspace, binding.address,
                        proposal.after_value, proposal.expected_generation,
                        proposal.expected_revision, proposal.expected_overlay_revision,
                        completion);
                else
                    accepted = disasm_view::queue_type_application(binding.workspace,
                        binding.address, proposal.after_value, proposal.expected_generation,
                        proposal.expected_revision, proposal.expected_overlay_revision,
                        completion);
                if (!accepted)
                    finish_reverse_engineering_proposal(operation_id, false,
                        "The generation-fenced analysis mutation queue rejected this proposal-specific operation.",
                        false);
                return;
            }
            aida::ui_thread::post_options_t options;
            options.subsystem = "ai_proposal";
            options.label = "proposal.review_surface";
            options.phase = "apply_revalidated";
            options.owner = "ai_proposal";
            options.priority = aida::ui_thread::priority_t::high;
            options.cancelled = [operation_id] {
                std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
                return s_reverse_engineering_proposal.operation_id != operation_id ||
                    !s_reverse_engineering_proposal.applying;
            };
            const auto posted = aida::ui_thread::post(
                [proposal, binding = std::move(binding), operation_id]() mutable {
                    try {
                        bool accepted = false;
                        std::string dispatch_reason;
                        if (proposal.kind == reverse_engineering_proposal_kind_t::static_patch) {
                            accepted = disasm_view::open_exact_static_patch_review(
                                binding.workspace, binding.address, binding.before_bytes,
                                binding.after_bytes, proposal.provenance,
                                proposal.expected_generation, proposal.expected_revision,
                                proposal.expected_overlay_revision, &dispatch_reason);
                        } else if (proposal.kind ==
                                reverse_engineering_proposal_kind_t::live_patch) {
                            accepted = debugger_view::stage_exact_patch_review(
                                binding.address.value, binding.before_bytes,
                                binding.after_bytes, binding.target_pid,
                                "AI proposal " + proposal.id + " from " +
                                    proposal.provenance, &dispatch_reason);
                        } else {
                            accepted = network_view::stage_validated_reviewed_request(
                                binding.network_source, binding.network_after,
                                proposal.provenance, binding.network_staged,
                                dispatch_reason);
                        }
                        const std::uint64_t staged_revision = accepted &&
                                (proposal.kind == reverse_engineering_proposal_kind_t::network_request_edit ||
                                 proposal.kind == reverse_engineering_proposal_kind_t::network_replay_staging)
                            ? binding.network_staged.revision : 0;
                        const std::uint64_t staged_hash = accepted &&
                                (proposal.kind == reverse_engineering_proposal_kind_t::network_request_edit ||
                                 proposal.kind == reverse_engineering_proposal_kind_t::network_replay_staging)
                            ? binding.network_staged.content_hash
                            : exact_content_hash(proposal.after_value);
                        const std::string staged_identity = accepted &&
                                !binding.network_staged.id.empty()
                            ? "network.repeater.close:" + binding.network_staged.id
                            : proposal.rollback_action_id + ":" + proposal.id;
                        finish_reverse_engineering_proposal(operation_id, accepted,
                            accepted
                                ? proposal.kind == reverse_engineering_proposal_kind_t::static_patch
                                    ? "Exact static bytes were staged in the generation-fenced Static Patch Review; the overlay is unchanged until its second confirmation."
                                    : proposal.kind == reverse_engineering_proposal_kind_t::live_patch
                                    ? "Exact live bytes were revalidated off the UI thread and staged in debugger Patch Review; target memory is unchanged."
                                    : "The canonical retained endpoint and strict HTTP/1 draft were staged in Repeater; no request was sent."
                                : dispatch_reason.empty()
                                    ? "The authoritative review surface rejected the proposal."
                                    : std::move(dispatch_reason), false, "failure",
                            staged_revision, staged_hash, staged_identity);
                    } catch (const std::exception& exception) {
                        finish_reverse_engineering_proposal(operation_id, false,
                            std::string("The UI review-surface dispatch raised an exception: ") +
                                exception.what(), false, "failure");
                    } catch (...) {
                        finish_reverse_engineering_proposal(operation_id, false,
                            "The UI review-surface dispatch raised an unknown exception.",
                            false, "failure");
                    }
                }, std::move(options));
            if (posted != aida::ui_thread::enqueue_result_t::accepted)
                finish_reverse_engineering_proposal(operation_id, false,
                    "The UI review-surface handoff was rejected by the bounded dispatcher.",
                    false, "failure");
          } catch (const std::exception& exception) {
              finish_reverse_engineering_proposal(operation_id, false,
                  std::string("Apply-time proposal dispatch raised an exception: ") +
                      exception.what(), false, "failure");
          } catch (...) {
              finish_reverse_engineering_proposal(operation_id, false,
                  "Apply-time proposal dispatch raised an unknown exception.",
                  false, "failure");
          }
        });
    if (!submitted) {
        finish_reverse_engineering_proposal(operation_id, false,
            "The bounded apply-time proposal validator rejected the task.", false);
        return failed("The bounded apply-time proposal validator rejected the task.");
    }
    action_result_t result = completed(
        "Apply-time proposal revalidation queued; no bytes, metadata, or network traffic changed.");
    result.target_view_id = proposal.target_view_id;
    return result;
}

action_result_t reject_editor_proposal(
    const editor_proposal_snapshot_t& proposal)
{
    if (!proposal.pending)
        return failed("The staged editor proposal is no longer pending.");
    if (proposal.applying)
        return failed("The editor proposal is already committing its reviewed hunks.");
    bool changed_before_reject = false;
    {
        std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
        if (!s_editor_proposal.pending ||
            s_editor_proposal.generation != proposal.generation)
            changed_before_reject = true;
    }
    if (changed_before_reject) {
        editor_proposal_snapshot_t interrupted = proposal;
        interrupted.applying = false;
        interrupted.stale = true;
        interrupted.diagnostic_id =
            "diagnostic." + proposal.audit_id + ".reject_generation";
        interrupted.detail =
            "The editor proposal ownership changed before rejection could be committed; no editor mutation occurred.";
        std::string persistence_error;
        const bool persisted = persist_proposal_audit(editor_audit_record(
            interrupted, "stale", "stale", interrupted.detail), persistence_error);
        update_proposal_task(proposal.task_id, persisted
                ? aida::ui::task_center::task_state_t::interrupted
                : aida::ui::task_center::task_state_t::failed,
            1.0f, persisted ? "Proposal ownership changed before reject" :
                "Reject interruption audit failed",
            persisted ? interrupted.detail : persistence_error,
            interrupted.diagnostic_id);
        raise_proposal_diagnostic(interrupted.diagnostic_id, proposal.task_id,
            proposal.target_document_id,
            "AI editor proposal ownership changed before reject",
            interrupted.detail + (persisted ? std::string{} :
                " Audit persistence failed: " + persistence_error));
        return failed(interrupted.detail);
    }
    std::string persistence_error;
    if (!persist_proposal_audit(editor_audit_record(proposal, "rejected",
            "reject", "The proposal was rejected without changing the document."),
            persistence_error)) {
        const std::string diagnostic_id =
            "diagnostic." + proposal.audit_id + ".reject_persistence";
        {
            std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
            if (s_editor_proposal.generation == proposal.generation) {
                s_editor_proposal.stale = true;
                s_editor_proposal.diagnostic_id = diagnostic_id;
                s_editor_proposal.detail = persistence_error;
            }
        }
        update_proposal_task(proposal.task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Reject audit persistence failed", persistence_error, diagnostic_id);
        raise_proposal_diagnostic(diagnostic_id, proposal.task_id,
            proposal.target_document_id,
            "AI editor rejection was blocked by audit persistence",
            persistence_error);
        return failed("Reject was blocked because the proposal audit transaction failed: " +
            persistence_error);
    }
    bool ownership_changed = false;
    {
        std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
        if (!s_editor_proposal.pending ||
            s_editor_proposal.generation != proposal.generation)
            ownership_changed = true;
        else {
            s_editor_proposal.pending = false;
            s_editor_proposal.rejected = true;
            s_editor_proposal.detail =
                "The proposal was rejected without changing the document.";
        }
    }
    if (ownership_changed) {
        update_proposal_task(proposal.task_id,
            aida::ui::task_center::task_state_t::cancelled, 1.0f,
            "Rejected after proposal ownership changed",
            "The durable rejection completed without interacting with the newer editor proposal.");
        return completed(
            "Editor proposal rejected durably without interacting with the newer proposal.");
    }
    code_editor_widget::cancel_agent_edit();
    update_proposal_task(proposal.task_id,
        aida::ui::task_center::task_state_t::cancelled, 1.0f,
        "Rejected by reviewer",
        "The proposal was rejected without changing the document.");
    return completed("Editor proposal rejected without writes.");
}

action_capability_t message_action_capability(const message_identity_t& identity, message_action_t action)
{
    action_capability_t result;
    if (s_proposal_restore_pending.load(std::memory_order_acquire) &&
        (action == message_action_t::review_change ||
         action == message_action_t::apply_change ||
         action == message_action_t::reject_change)) {
        result.disabled_reason =
            "Wait for the bounded proposal-review restore to finish.";
        return result;
    }
    if (action == message_action_t::reject_change) {
        const auto reverse_proposal_publication = reverse_engineering_proposal_snapshot();
        const auto& reverse_proposal = *reverse_proposal_publication;
        if (reverse_proposal.pending &&
            reverse_proposal.source.session_id == identity.session_id &&
            reverse_proposal.source.fingerprint == identity.fingerprint) {
            result.enabled = !reverse_proposal.applying;
            result.disabled_reason = result.enabled ? std::string{} :
                "The proposal operation is already committing its reviewed handoff.";
            return result;
        }
        const auto editor_proposal = editor_proposal_snapshot();
        if (editor_proposal.pending &&
            editor_proposal.source.session_id == identity.session_id &&
            editor_proposal.source.fingerprint == identity.fingerprint) {
            result.enabled = !editor_proposal.applying;
            result.disabled_reason = result.enabled ? std::string{} :
                "The editor proposal is already committing its reviewed hunks.";
            return result;
        }
    }
    std::string reason;
    const ChatMessage* message = resolve_message(identity, reason);
    if (!message) {
        result.disabled_reason = std::move(reason);
        return result;
    }
    const bool busy = is_ai_busy();
    const auto persistence = aida::conversation_store::status();
    const bool persistence_blocked = persistence.pending || persistence.failed;
    switch (action) {
        case message_action_t::copy_text:
            result.enabled = !message->text.empty();
            result.disabled_reason = result.enabled ? "" : "This message has no response text to copy.";
            break;
        case message_action_t::copy_reasoning:
            result.enabled = message->has_thinking && !message->thinking_text.empty();
            result.disabled_reason = result.enabled ? "" : "This message has no displayed reasoning text.";
            break;
        case message_action_t::copy_tool_name:
            result.enabled = !message->tool_name.empty();
            result.disabled_reason = result.enabled ? "" : "This message is not associated with a named tool.";
            break;
        case message_action_t::send_to_chat_input:
        case message_action_t::create_evidence_handoff:
            result.enabled = !message->text.empty();
            result.disabled_reason = result.enabled ? "" : "This message has no text that can be handed off.";
            break;
        case message_action_t::edit_message:
            result.enabled = message->is_user && !message->streaming && !busy &&
                !persistence_blocked;
            result.disabled_reason = !message->is_user ? "Only user messages can be edited." :
                message->streaming ? "Wait for the streaming message to finish." :
                busy ? "Cancel or wait for the active AI operation before editing history." :
                persistence_blocked ? "Resolve the conversation persistence transaction before editing history." : "";
            break;
        case message_action_t::retry_from_here:
            if (message->is_user) {
                result.disabled_reason = "Choose an assistant response to retry from.";
                break;
            }
            if (busy) {
                result.disabled_reason = "Cancel or wait for the active AI operation before retrying.";
                break;
            }
            if (persistence_blocked) {
                result.disabled_reason = "Resolve the conversation persistence transaction before retrying history.";
                break;
            }
            for (std::size_t index = identity.index; index > 0; --index) {
                if (g_chat_messages[index - 1U].is_user && !g_chat_messages[index - 1U].text.empty()) {
                    result.enabled = true;
                    break;
                }
            }
            if (!result.enabled) result.disabled_reason = "No earlier user message is available to retry.";
            break;
        case message_action_t::delete_message:
            result.enabled = !message->streaming && !busy && !persistence_blocked;
            result.disabled_reason = message->streaming ? "Wait for the streaming message to finish." :
                busy ? "Cancel or wait for the active AI operation before deleting history." :
                persistence_blocked ? "Resolve the conversation persistence transaction before deleting history." : "";
            break;
        case message_action_t::inspect_tool_activity:
            result.enabled = message->is_tool_result || !message->tool_name.empty();
            result.disabled_reason = result.enabled ? "" : "This message has no MCP tool activity to inspect.";
            break;
        case message_action_t::review_change:
        case message_action_t::apply_change:
        case message_action_t::reject_change: {
            const auto reverse_proposal_publication = reverse_engineering_proposal_snapshot();
            const auto& reverse_proposal = *reverse_proposal_publication;
            const bool reverse_linked = reverse_proposal.pending &&
                reverse_proposal.source.session_id == identity.session_id &&
                reverse_proposal.source.fingerprint == identity.fingerprint;
            if (reverse_linked) {
                if (action == message_action_t::reject_change) {
                    result.enabled = !reverse_proposal.applying;
                    result.disabled_reason = result.enabled ? std::string{} :
                        "The proposal operation is already committing its reviewed handoff.";
                    break;
                }
                if (reverse_proposal.stale ||
                    reverse_proposal.state == reverse_engineering_proposal_state_t::error ||
                    reverse_proposal.state == reverse_engineering_proposal_state_t::stale) {
                    if (action == message_action_t::review_change) {
                        result.enabled = true;
                        break;
                    }
                    result.disabled_reason = reverse_proposal.disabled_reason.empty()
                        ? reverse_proposal.detail : reverse_proposal.disabled_reason;
                    break;
                }
                result.enabled = !reverse_proposal.applying &&
                    reverse_proposal.state == reverse_engineering_proposal_state_t::valid;
                result.disabled_reason = result.enabled ? std::string{} :
                    reverse_proposal.state == reverse_engineering_proposal_state_t::queued
                    ? "Proposal validation is queued."
                    : reverse_proposal.state == reverse_engineering_proposal_state_t::running
                    ? "Proposal identity and before-value validation is running."
                    : "The reviewed proposal is already being applied.";
                break;
            }
            const auto editor_proposal = editor_proposal_snapshot();
            const bool editor_linked = editor_proposal.pending &&
                editor_proposal.source.session_id == identity.session_id &&
                editor_proposal.source.fingerprint == identity.fingerprint;
            if (editor_linked) {
                if (editor_proposal.stale) {
                    if (action == message_action_t::review_change) {
                        result.enabled = true;
                        break;
                    }
                    result.disabled_reason = editor_proposal.detail;
                    break;
                }
                if (action == message_action_t::apply_change &&
                    code_editor_widget::pending_hunk_count() == 0) {
                    result.disabled_reason = "The staged change has no pending editor hunks to apply.";
                    break;
                }
                if (action == message_action_t::apply_change &&
                    (!code_editor_widget::has_pending_diff() ||
                     !code_editor_widget::pending_diff().fully_resolved())) {
                    result.disabled_reason =
                        "Accept or reject every editor hunk before applying the resolved diff.";
                    break;
                }
                result.enabled = true;
                break;
            }
            if (action == message_action_t::review_change) {
                nlohmann::json document;
                std::string proposal_reason;
                if (proposal_document(*message, document, proposal_reason)) {
                    result.enabled = true;
                    break;
                }
                result.disabled_reason = std::move(proposal_reason);
                break;
            }
            result.disabled_reason = "No pending reviewed change identity is linked to this chat message.";
            break;
        }
            break;
        case message_action_t::cancel_active_operation:
            result.enabled = busy;
            result.disabled_reason = busy ? "" : "No AI operation is currently running.";
            break;
    }
    return result;
}

action_result_t execute_message_action(const message_identity_t& identity, message_action_t action)
{
    const auto capability = message_action_capability(identity, action);
    if (!capability.enabled) return failed(capability.disabled_reason);
    if (action == message_action_t::reject_change) {
        const auto reverse_proposal_publication = reverse_engineering_proposal_snapshot();
        const auto& reverse_proposal = *reverse_proposal_publication;
        if (reverse_proposal.pending &&
            reverse_proposal.source.session_id == identity.session_id &&
            reverse_proposal.source.fingerprint == identity.fingerprint) {
            bool generation_changed = false;
            bool applying = false;
            {
                std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
                generation_changed = s_reverse_engineering_proposal.operation_id !=
                    reverse_proposal.operation_id;
                applying = s_reverse_engineering_proposal.applying;
            }
            if (generation_changed) {
                update_proposal_task(reverse_proposal.task_id,
                    aida::ui::task_center::task_state_t::interrupted, 1.0f,
                    "Proposal ownership changed before reject",
                    "The proposal generation changed before rejection; no mutation was dispatched.");
                return failed("The proposal generation changed; review the current proposal.");
            }
            if (applying)
                return failed("The proposal is already committing its reviewed handoff.");
            std::string persistence_error;
            if (!persist_proposal_audit(reverse_audit_record(reverse_proposal,
                    "rejected", "reject",
                    "The reverse-engineering proposal was rejected without dispatching a mutation."),
                    persistence_error)) {
                const std::string diagnostic_id =
                    "diagnostic." + reverse_proposal.audit_id + ".reject_persistence";
                {
                    std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
                    if (s_reverse_engineering_proposal.operation_id ==
                            reverse_proposal.operation_id) {
                        s_reverse_engineering_proposal.stale = true;
                        s_reverse_engineering_proposal.state =
                            reverse_engineering_proposal_state_t::error;
                        s_reverse_engineering_proposal.diagnostic_id = diagnostic_id;
                        s_reverse_engineering_proposal.disabled_reason = persistence_error;
                        s_reverse_engineering_proposal.detail = persistence_error;
                        publish_reverse_engineering_proposal_locked();
                    }
                }
                update_proposal_task(reverse_proposal.task_id,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Reject audit persistence failed", persistence_error,
                    diagnostic_id);
                raise_proposal_diagnostic(diagnostic_id, reverse_proposal.task_id,
                    reverse_proposal.target_id,
                    "AI proposal rejection was blocked by audit persistence",
                    persistence_error);
                return failed("Reject was blocked because the proposal audit transaction failed: " +
                    persistence_error);
            }
            bool ownership_changed = false;
            {
                std::lock_guard<std::mutex> lock(s_reverse_engineering_proposal_mutex);
                ownership_changed = s_reverse_engineering_proposal.operation_id !=
                    reverse_proposal.operation_id;
                if (!ownership_changed) {
                    s_reverse_engineering_proposal.pending = false;
                    s_reverse_engineering_proposal.rejected = true;
                    s_reverse_engineering_proposal.state =
                        reverse_engineering_proposal_state_t::rejected;
                    s_reverse_engineering_proposal.operation_id =
                        s_reverse_engineering_proposal_operation.fetch_add(
                            1, std::memory_order_acq_rel);
                    s_reverse_engineering_proposal.detail =
                        "The reverse-engineering proposal was rejected without dispatching a mutation.";
                    publish_reverse_engineering_proposal_locked();
                }
            }
            if (ownership_changed) {
                update_proposal_task(reverse_proposal.task_id,
                    aida::ui::task_center::task_state_t::cancelled, 1.0f,
                    "Rejected after proposal ownership changed",
                    "The durable rejection completed without interacting with the newer proposal.");
                return completed(
                    "Reverse-engineering proposal rejected durably without interacting with the newer proposal.");
            }
            update_proposal_task(reverse_proposal.task_id,
                aida::ui::task_center::task_state_t::cancelled, 1.0f,
                "Rejected by reviewer",
                "The proposal was rejected without dispatching a mutation.");
            return completed(
                "Reverse-engineering proposal rejected without writes or network activity.");
        }
        const auto editor_proposal = editor_proposal_snapshot();
        if (editor_proposal.pending &&
            editor_proposal.source.session_id == identity.session_id &&
            editor_proposal.source.fingerprint == identity.fingerprint)
            return reject_editor_proposal(editor_proposal);
    }
    std::string reason;
    const ChatMessage* message = resolve_message(identity, reason);
    if (!message) return failed(std::move(reason));
    const std::string text = message->text;
    switch (action) {
        case message_action_t::copy_text:
            if (!copy_text_to_chat_clipboard(text))
                return failed("The clipboard bridge is unavailable.");
            return completed("Message text copied.");
        case message_action_t::copy_reasoning:
            if (!copy_text_to_chat_clipboard(message->thinking_text))
                return failed("The clipboard bridge is unavailable.");
            return completed("Reasoning text copied.");
        case message_action_t::copy_tool_name:
            if (!copy_text_to_chat_clipboard(message->tool_name))
                return failed("The clipboard bridge is unavailable.");
            return completed("Tool name copied.");
        case message_action_t::send_to_chat_input:
            if (text.size() <= 3800U) {
                post_chat_inject(text);
                return completed("Message text added to the chat input.");
            }
            post_chat_inject(text.substr(0, 3760U) + "\n[Message truncated for chat input]");
            return completed("A bounded excerpt was added to the chat input.");
        case message_action_t::create_evidence_handoff: {
            action_result_t result = completed("Evidence handoff created.");
            result.evidence.source = identity;
            result.evidence.source_kind = message->is_tool_result ? "tool_result" :
                message->is_user ? "user_message" : "assistant_message";
            result.evidence.tool_name = message->tool_name;
            result.evidence.truncated = text.size() > max_evidence_bytes;
            result.evidence.text.assign(text.data(), (std::min)(text.size(), max_evidence_bytes));
            evidence_envelope_t envelope;
            envelope.id = "evidence.chat." + std::to_string(identity.fingerprint);
            envelope.session_id = identity.session_id;
            envelope.source_view_id = "view.ai_chat";
            envelope.source_kind = result.evidence.source_kind;
            envelope.entity_id = "message." + std::to_string(identity.index) + "." +
                std::to_string(identity.timestamp);
            envelope.display_label = message->is_tool_result ? "Tool result" :
                message->is_user ? "User message" : "Assistant response";
            envelope.return_target = envelope.entity_id;
            envelope.excerpt = result.evidence.text;
            envelope.revision = identity.fingerprint;
            envelope.generation = static_cast<std::uint64_t>(identity.timestamp);
            envelope.snapshot_hash = identity.fingerprint;
            envelope.content_hash = identity.fingerprint;
            envelope.truncated = result.evidence.truncated;
            result.evidence.evidence_id = register_evidence(std::move(envelope));
            if (result.evidence.evidence_id.empty())
                return failed("The message evidence identity could not be registered.");
            return result;
        }
        case message_action_t::edit_message: {
            {
                std::lock_guard<std::mutex> lock(s_message_edit_mtx);
                s_pending_message_edit = pending_message_edit_t{ identity, text };
                ++s_message_edit_seq;
            }
            if (s_message_edit_notify_hook) s_message_edit_notify_hook();
            return completed("Message editor opened.");
        }
        case message_action_t::delete_message:
            return failed("Delete Message requires confirmation in the Chat view.");
        case message_action_t::inspect_tool_activity: {
            action_result_t result = completed("Open the MCP Log view for the complete activity record.");
            result.target_view_id = "view.mcp_log";
            return result;
        }
        case message_action_t::cancel_active_operation:
            chat_request_cancel();
            return completed("Cancellation requested.");
        case message_action_t::retry_from_here:
            for (std::size_t index = identity.index; index > 0; --index) {
                const auto& candidate = g_chat_messages[index - 1U];
                if (!candidate.is_user || candidate.text.empty()) continue;
                ChatMessage retry;
                retry.text = candidate.text;
                retry.is_user = true;
                retry.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                g_chat_messages.push_back(std::move(retry));
                aida::automation_ui::request_chat_scroll_to_bottom();
                conversations::save_current();
                tick_ai_chat();
                return completed("Retry request queued from the selected response.");
            }
            return failed("No earlier user message is available to retry.");
        case message_action_t::review_change: {
            const auto reverse_proposal_publication = reverse_engineering_proposal_snapshot();
            const auto& reverse_proposal = *reverse_proposal_publication;
            if (reverse_proposal.pending &&
                reverse_proposal.source.session_id == identity.session_id &&
                reverse_proposal.source.fingerprint == identity.fingerprint) {
                action_result_t result = completed(
                    "Reverse-engineering proposal opened for exact before/after review.");
                result.target_view_id = "view.ai.evidence";
                return result;
            }
            const auto editor_proposal = editor_proposal_snapshot();
            if (editor_proposal.pending &&
                editor_proposal.source.session_id == identity.session_id &&
                editor_proposal.source.fingerprint == identity.fingerprint) {
                action_result_t result = completed("Editor proposal opened for per-hunk review.");
                result.target_view_id = "document.code";
                return result;
            }
            return stage_reverse_engineering_proposal(identity);
        }
        case message_action_t::apply_change: {
            const auto reverse_proposal_publication = reverse_engineering_proposal_snapshot();
            const auto& reverse_proposal = *reverse_proposal_publication;
            if (reverse_proposal.pending &&
                reverse_proposal.source.session_id == identity.session_id &&
                reverse_proposal.source.fingerprint == identity.fingerprint) {
                if (reverse_proposal.stale)
                    return failed(reverse_proposal.disabled_reason.empty()
                        ? reverse_proposal.detail : reverse_proposal.disabled_reason);
                return queue_reverse_engineering_proposal_apply(reverse_proposal);
            }
            const auto proposal = editor_proposal_snapshot();
            if (!proposal.pending || proposal.source.fingerprint != identity.fingerprint)
                return failed("The staged proposal is no longer linked to this message.");
            if (proposal.stale || code_editor_widget::active_document_id() != proposal.target_document_numeric_id ||
                code_editor_widget::document_revision() != proposal.base_document_revision ||
                code_editor_widget::document_content_fingerprint() != proposal.base_content_hash)
                return failed("The code document changed after review began; apply is blocked until the proposal is regenerated.");
            const int pending_hunks = code_editor_widget::pending_hunk_count();
            const auto pending_diff = code_editor_widget::pending_diff();
            if (proposal.reviewed_generation != proposal.generation ||
                proposal.reviewed_content_hash != proposal.base_content_hash ||
                proposal.reviewed_pending_hunks != pending_hunks || pending_hunks <= 0 ||
                !pending_diff.active || !pending_diff.fully_resolved())
                return failed("Review and confirm the exact target revision, content hash, and pending hunks before applying.");
            std::string persistence_error;
            if (!persist_proposal_audit(editor_audit_record(proposal, "reviewed",
                    "review", "The human confirmed the exact per-hunk decisions and target revision."),
                    persistence_error) ||
                !persist_proposal_audit(editor_audit_record(proposal, "applying",
                    "apply", "Applying the resolved per-hunk decisions through the code editor backend."),
                    persistence_error)) {
                const std::string diagnostic_id =
                    "diagnostic." + proposal.audit_id + ".persistence";
                {
                    std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
                    if (s_editor_proposal.generation == proposal.generation) {
                        s_editor_proposal.stale = true;
                        s_editor_proposal.detail = persistence_error;
                    }
                }
                update_proposal_task(proposal.task_id,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Apply audit persistence failed", persistence_error, diagnostic_id);
                raise_proposal_diagnostic(diagnostic_id, proposal.task_id,
                    proposal.target_document_id,
                    "AI editor apply was blocked by audit persistence",
                    persistence_error);
                return failed("Apply was blocked because the editor proposal audit transaction failed: " +
                    persistence_error);
            }
            int accepted_hunks = 0;
            int rejected_hunks = 0;
            for (const auto& hunk : pending_diff.hunks) {
                if (hunk.state == code_editor_widget::diff_hunk_state_t::accepted)
                    ++accepted_hunks;
                else if (hunk.state == code_editor_widget::diff_hunk_state_t::rejected)
                    ++rejected_hunks;
            }
            bool generation_changed_before_commit = false;
            {
                std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
                generation_changed_before_commit = !s_editor_proposal.pending ||
                    s_editor_proposal.generation != proposal.generation;
                if (!generation_changed_before_commit) {
                    s_editor_proposal.applying = true;
                    s_editor_proposal.detail = "Applying " + std::to_string(pending_hunks) +
                        " reviewed editor hunks to document " +
                        std::to_string(proposal.target_document_numeric_id) + ".";
                }
            }
            if (generation_changed_before_commit) {
                editor_proposal_snapshot_t interrupted = proposal;
                interrupted.applying = false;
                interrupted.stale = true;
                interrupted.diagnostic_id =
                    "diagnostic." + proposal.audit_id + ".generation";
                interrupted.detail =
                    "The editor proposal ownership changed after apply was audited but before any editor mutation; no hunks were committed.";
                std::string terminal_persistence;
                const bool persisted = persist_proposal_audit(editor_audit_record(
                    interrupted, "stale", "stale", interrupted.detail),
                    terminal_persistence);
                update_proposal_task(proposal.task_id, persisted
                        ? aida::ui::task_center::task_state_t::interrupted
                        : aida::ui::task_center::task_state_t::failed,
                    1.0f, persisted ? "Proposal ownership changed before commit" :
                        "Proposal interruption audit failed",
                    persisted ? interrupted.detail : terminal_persistence,
                    interrupted.diagnostic_id);
                raise_proposal_diagnostic(interrupted.diagnostic_id,
                    proposal.task_id, proposal.target_document_id,
                    "AI editor proposal ownership changed before commit",
                    interrupted.detail + (persisted ? std::string{} :
                        " Audit persistence failed: " + terminal_persistence));
                return failed(interrupted.detail);
            }
            update_proposal_task(proposal.task_id,
                aida::ui::task_center::task_state_t::running, 0.75f,
                "Applying resolved editor hunks",
                std::to_string(accepted_hunks) + " accepted; " +
                    std::to_string(rejected_hunks) + " rejected");
            if (!code_editor_widget::commit_resolved_diff()) {
                editor_proposal_snapshot_t failed_proposal = proposal;
                failed_proposal.applying = false;
                failed_proposal.stale = true;
                failed_proposal.diagnostic_id =
                    "diagnostic." + proposal.audit_id + ".apply";
                failed_proposal.detail =
                    "The code editor rejected the resolved diff; no completion was recorded.";
                {
                    std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
                    if (s_editor_proposal.generation == proposal.generation) {
                        s_editor_proposal.applying = false;
                        s_editor_proposal.stale = true;
                        s_editor_proposal.diagnostic_id = failed_proposal.diagnostic_id;
                        s_editor_proposal.detail = failed_proposal.detail;
                        failed_proposal = s_editor_proposal;
                    } else {
                        failed_proposal.detail +=
                            " Proposal ownership also changed before failure publication.";
                    }
                }
                std::string failure_persistence;
                static_cast<void>(persist_proposal_audit(editor_audit_record(
                    failed_proposal, "failure", "failure", failed_proposal.detail),
                    failure_persistence));
                const std::string diagnostic_id = failed_proposal.diagnostic_id;
                update_proposal_task(proposal.task_id,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Editor apply failed", failed_proposal.detail, diagnostic_id);
                raise_proposal_diagnostic(diagnostic_id, proposal.task_id,
                    proposal.target_document_id, "AI editor proposal apply failed",
                    failed_proposal.detail + (failure_persistence.empty()
                        ? std::string{} : " Audit persistence also failed: " +
                            failure_persistence));
                return failed(failed_proposal.detail);
            }
            editor_proposal_snapshot_t applied_proposal = proposal;
            applied_proposal.pending = false;
            applied_proposal.applying = false;
            applied_proposal.applied = true;
            applied_proposal.result_revision =
                code_editor_widget::document_revision();
            applied_proposal.result_content_hash =
                code_editor_widget::document_content_fingerprint();
            applied_proposal.detail = rejected_hunks == 0
                ? "All reviewed hunks were applied through the code editor backend."
                : std::to_string(accepted_hunks) +
                    " reviewed hunks were applied and " +
                    std::to_string(rejected_hunks) + " were retained unchanged.";
            bool generation_changed_after_commit = false;
            {
                std::lock_guard<std::mutex> lock(s_editor_proposal_mutex);
                generation_changed_after_commit =
                    s_editor_proposal.generation != proposal.generation;
                if (!generation_changed_after_commit) {
                    s_editor_proposal = applied_proposal;
                    applied_proposal = s_editor_proposal;
                }
            }
            if (generation_changed_after_commit) {
                applied_proposal.diagnostic_id =
                    "diagnostic." + proposal.audit_id + ".committed_generation";
                applied_proposal.detail +=
                    " The authoritative editor commit succeeded, but proposal ownership changed before terminal publication; use the exact editor undo identity from the audit record if rollback is required.";
                std::string committed_persistence;
                const bool persisted = persist_proposal_audit(editor_audit_record(
                    applied_proposal, "partial", "partial", applied_proposal.detail),
                    committed_persistence);
                update_proposal_task(proposal.task_id,
                    aida::ui::task_center::task_state_t::partial,
                    1.0f, persisted ? "Committed; proposal ownership changed" :
                        "Committed result audit failed",
                    persisted ? applied_proposal.detail : committed_persistence,
                    applied_proposal.diagnostic_id);
                raise_proposal_diagnostic(applied_proposal.diagnostic_id,
                    proposal.task_id, proposal.target_document_id,
                    "AI editor commit completed after proposal ownership changed",
                    applied_proposal.detail + (persisted ? std::string{} :
                        " Audit persistence failed: " + committed_persistence));
                return failed(applied_proposal.detail);
            }
            const bool partial = rejected_hunks != 0;
            std::string terminal_persistence;
            if (!persist_proposal_audit(editor_audit_record(applied_proposal,
                    partial ? "applied_partial" : "applied",
                    partial ? "partial" : "apply", applied_proposal.detail),
                    terminal_persistence)) {
                const std::string diagnostic_id =
                    "diagnostic." + proposal.audit_id + ".result_persistence";
                update_proposal_task(proposal.task_id,
                    aida::ui::task_center::task_state_t::partial, 1.0f,
                    "Applied result was not durably audited", terminal_persistence,
                    diagnostic_id);
                raise_proposal_diagnostic(diagnostic_id, proposal.task_id,
                    proposal.target_document_id,
                    "Applied AI editor result audit failed",
                    applied_proposal.detail + " Audit persistence failed: " +
                        terminal_persistence);
                return failed(applied_proposal.detail +
                    " The authoritative edit succeeded, but its audit transaction failed: " +
                    terminal_persistence);
            }
            update_proposal_task(proposal.task_id, partial
                    ? aida::ui::task_center::task_state_t::partial
                    : aida::ui::task_center::task_state_t::completed,
                1.0f, partial ? "Partially applied by hunk decision" :
                    "Applied and revision-verified", applied_proposal.detail);
            return completed(applied_proposal.detail);
        }
        case message_action_t::reject_change: {
            return failed("The staged proposal is no longer linked to this message.");
        }
    }
    return failed("The action has no executable provider.");
}

message_window_t bounded_message_window(std::size_t first_visible, std::size_t visible_count, std::size_t overscan)
{
    message_window_t result;
    result.total = g_chat_messages.size();
    if (result.total == 0) return result;
    const std::size_t safe_overscan = (std::min)(overscan, std::size_t{32});
    result.first = first_visible > safe_overscan ? first_visible - safe_overscan : 0;
    const std::size_t requested = visible_count + safe_overscan * 2U;
    const std::size_t bounded_count = (std::min)(requested, max_rendered_messages);
    result.last = (std::min)(result.total, result.first + bounded_count);
    if (result.last == result.total && result.last - result.first < bounded_count)
        result.first = result.last > bounded_count ? result.last - bounded_count : 0;
    result.bounded = result.first != 0 || result.last != result.total;
    return result;
}

tool_approval_snapshot_t tool_approval_snapshot()
{
    tool_approval_snapshot_t result;
    std::lock_guard<std::mutex> lock(s_tool_approval.mtx);
    result.pending = s_tool_approval.pending && !s_tool_approval.answered;
    if (result.pending) {
        result.tool_name = s_tool_approval.tool_name;
        result.arguments_preview = s_tool_approval.tool_args_preview;
        result.identity = s_tool_approval.generation;
    }
    return result;
}

action_result_t respond_to_tool_approval(std::uint64_t identity, bool approve)
{
    std::lock_guard<std::mutex> lock(s_tool_approval.mtx);
    if (!s_tool_approval.pending || s_tool_approval.answered)
        return failed("The tool approval request is no longer pending.");
    if (identity == 0 || identity != s_tool_approval.generation)
        return failed("The tool approval request changed; review the current request before responding.");
    s_tool_approval.approved = approve;
    s_tool_approval.answered = true;
    s_tool_approval.cv.notify_one();
    return completed(approve ? "Tool execution approved." : "Tool execution denied.");
}

bool message_snapshot(std::size_t index, chat_message_snapshot_t& out)
{
    if (index >= g_chat_messages.size()) return false;
    const auto& message = g_chat_messages[index];
    out.text = message.text;
    out.thinking_text = message.thinking_text;
    out.is_user = message.is_user;
    out.has_thinking = message.has_thinking;
    out.streaming = message.streaming;
    out.timestamp = message.timestamp;
    out.input_tokens = message.input_tokens;
    out.output_tokens = message.output_tokens;
    out.cache_read_tokens = message.cache_read_tokens;
    out.cache_write_tokens = message.cache_write_tokens;
    out.cost = message.cost;
    out.tool_name = message.tool_name;
    out.is_tool_result = message.is_tool_result;
    out.model_id = message.model_id;
    return true;
}

action_result_t append_user_message(std::string text)
{
    std::string prompt = std::move(text);
    const auto first = prompt.find_first_not_of(" \t\r\n");
    const auto last = prompt.find_last_not_of(" \t\r\n");
    if (first == std::string::npos)
        return failed("The message is empty.");
    prompt = prompt.substr(first, last - first + 1U);
    ChatMessage message;
    message.text = std::move(prompt);
    message.is_user = true;
    message.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    g_chat_messages.push_back(std::move(message));
    request_chat_scroll_to_bottom();
    const bool assigned_new_session = conversations::current_id.empty();
    conversations::save_current();
    if (assigned_new_session && !conversations::current_id.empty()) {
        chat_bind_session(conversations::current_id);
        synchronize_evidence_session();
    }
    tick_ai_chat();
    return completed("Message queued.");
}

action_result_t delete_message(const message_identity_t& identity)
{
    std::string reason;
    const ChatMessage* pending_message = resolve_message(identity, reason);
    const auto capability = message_action_capability(identity, message_action_t::delete_message);
    if (!pending_message)
        return failed(reason);
    if (!capability.enabled)
        return failed(capability.disabled_reason.empty() ? reason : capability.disabled_reason);
    g_chat_messages.erase(g_chat_messages.begin() +
        static_cast<std::ptrdiff_t>(identity.index));
    conversations::save_current();
    return completed("Message deleted.");
}

action_result_t truncate_messages_from(const message_identity_t& identity)
{
    std::string reason;
    if (!resolve_message(identity, reason))
        return failed(std::move(reason));
    g_chat_messages.erase(g_chat_messages.begin() +
        static_cast<std::ptrdiff_t>(identity.index), g_chat_messages.end());
    conversations::save_current();
    return completed("Conversation truncated from the selected message.");
}

bool consume_pending_message_edit(message_identity_t& identity, std::string& text)
{
    std::lock_guard<std::mutex> lock(s_message_edit_mtx);
    if (!s_pending_message_edit) return false;
    identity = s_pending_message_edit->identity;
    text = std::move(s_pending_message_edit->text);
    s_pending_message_edit.reset();
    return true;
}

surface_capabilities_t surface_capabilities()
{
    surface_capabilities_t result;
    result.evidence_pane = true;
    result.background_tasks_pane = true;
    result.evidence_reason.clear();
    result.mcp_activity_reason = "MCP activity is exposed by the existing MCP Log output view; no separate activity renderer exists.";
    result.scripts_reason = "Isolated script execution exists as an approval-gated MCP tool, but no standalone Scripts pane exists.";
    result.background_tasks_reason.clear();
    return result;
}

std::string register_evidence(evidence_envelope_t envelope)
{
    synchronize_evidence_session();
    if (envelope.source_view_id.empty() || envelope.source_kind.empty() ||
        envelope.entity_id.empty() || envelope.content_hash == 0)
        return {};
    if (envelope.session_id.empty()) envelope.session_id = evidence_session_id();
    envelope.id = bounded_metadata_string(std::move(envelope.id), 256U);
    envelope.project_id = bounded_metadata_string(std::move(envelope.project_id), 256U);
    envelope.workspace_id = bounded_metadata_string(std::move(envelope.workspace_id), 256U);
    envelope.session_id = bounded_metadata_string(std::move(envelope.session_id), 256U);
    envelope.source_view_id = bounded_metadata_string(std::move(envelope.source_view_id), 256U);
    envelope.source_kind = bounded_metadata_string(std::move(envelope.source_kind), 128U);
    envelope.entity_id = bounded_metadata_string(std::move(envelope.entity_id), 512U);
    envelope.display_label = bounded_metadata_string(std::move(envelope.display_label), 512U);
    envelope.return_target = bounded_metadata_string(std::move(envelope.return_target), 512U);
    envelope.stale_reason = bounded_metadata_string(std::move(envelope.stale_reason), 1024U);
    if (envelope.excerpt.size() > max_evidence_bytes) {
        envelope.excerpt.resize(max_evidence_bytes);
        envelope.truncated = true;
    }
    if (envelope.created_ms == 0)
        envelope.created_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (envelope.id.empty())
        envelope.id = "evidence." + std::to_string(envelope.created_ms) + "." +
            std::to_string(s_evidence_sequence.fetch_add(1, std::memory_order_relaxed));
    const std::string result_id = envelope.id;
    const std::string session = envelope.session_id;
    {
        std::lock_guard<std::mutex> lock(s_evidence_mutex);
        const auto duplicate = std::find_if(s_evidence.begin(), s_evidence.end(), [&](const evidence_envelope_t& current) {
            return current.id == envelope.id;
        });
        if (duplicate != s_evidence.end()) *duplicate = std::move(envelope);
        else s_evidence.push_back(std::move(envelope));
        while (s_evidence.size() > max_evidence_items) s_evidence.pop_front();
        publish_evidence_locked();
    }
    if (!session.empty())
        persist_evidence_metadata(session, persisted_evidence_snapshot(session));
    return result_id;
}

void register_evidence_source_return(const std::string& evidence_id,
    std::function<bool(std::string&)> navigate)
{
    if (evidence_id.empty() || !navigate) return;
    std::lock_guard<std::mutex> lock(s_evidence_mutex);
    const auto found = std::find_if(s_evidence.begin(), s_evidence.end(),
        [&](const evidence_envelope_t& item) { return item.id == evidence_id; });
    if (found == s_evidence.end()) return;
    const auto existing = std::find_if(s_evidence_source_returns.begin(),
        s_evidence_source_returns.end(), [&](const auto& item) {
            return item.first == evidence_id;
        });
    if (existing != s_evidence_source_returns.end())
        existing->second = std::move(navigate);
    else
        s_evidence_source_returns.emplace_back(evidence_id, std::move(navigate));
    while (s_evidence_source_returns.size() > max_evidence_items)
        s_evidence_source_returns.pop_front();
}

std::shared_ptr<const std::vector<evidence_envelope_t>> evidence_snapshot()
{
    synchronize_evidence_session();
    return std::atomic_load_explicit(&s_evidence_publication,
        std::memory_order_acquire);
}

std::vector<aida::conversation_store::evidence_t> persisted_evidence_snapshot(
    const std::string& session_id)
{
    std::vector<aida::conversation_store::evidence_t> result;
    std::lock_guard<std::mutex> lock(s_evidence_mutex);
    result.reserve((std::min)(s_evidence.size(), max_evidence_items));
    for (const auto& item : s_evidence) {
        if (item.session_id != session_id || item.sensitive) continue;
        aida::conversation_store::evidence_t persisted;
        persisted.id = item.id;
        persisted.project_id = item.project_id;
        persisted.workspace_id = item.workspace_id;
        persisted.session_id = item.session_id;
        persisted.source_view_id = item.source_view_id;
        persisted.source_kind = item.source_kind;
        persisted.entity_id = item.entity_id;
        persisted.display_label = item.display_label;
        persisted.return_target = item.return_target;
        persisted.address = item.address;
        persisted.revision = item.revision;
        persisted.generation = item.generation;
        persisted.snapshot_hash = item.snapshot_hash;
        persisted.content_hash = item.content_hash;
        persisted.created_ms = item.created_ms;
        persisted.truncated = item.truncated;
        persisted.sensitive = item.sensitive;
        result.push_back(std::move(persisted));
        if (result.size() >= max_evidence_items) break;
    }
    return result;
}

bool persisted_evidence_session_loaded(const std::string& session_id)
{
    const auto publication = std::atomic_load_explicit(
        &s_evidence_session_publication, std::memory_order_acquire);
    return publication && session_id == publication->loaded;
}

void apply_persisted_evidence(const std::string& session_id,
    std::vector<aida::conversation_store::evidence_t> evidence)
{
    std::deque<evidence_envelope_t> replacement;
    for (auto& persisted : evidence) {
        if (replacement.size() >= max_evidence_items) break;
        if (persisted.session_id != session_id) continue;
        evidence_envelope_t item;
        item.id = std::move(persisted.id);
        item.project_id = std::move(persisted.project_id);
        item.workspace_id = std::move(persisted.workspace_id);
        item.session_id = std::move(persisted.session_id);
        item.source_view_id = std::move(persisted.source_view_id);
        item.source_kind = std::move(persisted.source_kind);
        item.entity_id = std::move(persisted.entity_id);
        item.display_label = std::move(persisted.display_label);
        item.return_target = std::move(persisted.return_target);
        item.address = persisted.address;
        item.revision = persisted.revision;
        item.generation = persisted.generation;
        item.snapshot_hash = persisted.snapshot_hash;
        item.content_hash = persisted.content_hash;
        item.created_ms = persisted.created_ms;
        item.truncated = persisted.truncated;
        item.sensitive = persisted.sensitive;
        item.stale = true;
        item.stale_reason = "Snapshot content is not persisted; return to source and recapture.";
        replacement.push_back(std::move(item));
    }
    std::lock_guard<std::mutex> lock(s_evidence_mutex);
    s_evidence = std::move(replacement);
    s_evidence_source_returns.clear();
    s_loaded_evidence_session = session_id;
    s_requested_evidence_session.clear();
    publish_evidence_session_locked();
    publish_evidence_locked();
}

static bool evidence_payload(const std::string& evidence_id, evidence_envelope_t& envelope, std::string& reason)
{
    std::lock_guard<std::mutex> lock(s_evidence_mutex);
    const auto found = std::find_if(s_evidence.begin(), s_evidence.end(), [&](const evidence_envelope_t& current) {
        return current.id == evidence_id;
    });
    if (found == s_evidence.end()) {
        reason = "The evidence item is no longer retained; capture it again from the source view.";
        return false;
    }
    if (found->stale) {
        reason = found->stale_reason.empty()
            ? "The evidence source changed; capture a current snapshot."
            : found->stale_reason;
        return false;
    }
    envelope = *found;
    reason.clear();
    return true;
}

static bool queue_evidence(const std::string& evidence_id, bool agent, std::string& reason)
{
    evidence_envelope_t envelope;
    if (!evidence_payload(evidence_id, envelope, reason)) return false;
    if (envelope.sensitive) {
        reason = "Sensitive evidence is blocked from AI transfer. Return to the source and create an explicitly redacted evidence envelope.";
        return false;
    }
    std::ostringstream payload;
    payload << "[Evidence " << envelope.id << "]\nSource: " << envelope.source_view_id
            << " / " << envelope.entity_id << "\nRevision: " << envelope.revision
            << "  Generation: " << envelope.generation << "\nSnapshot hash: 0x" << std::hex
            << envelope.snapshot_hash << "  Content hash: 0x" << envelope.content_hash << std::dec
            << "\n\n" << envelope.excerpt;
    if (envelope.truncated)
        payload << "\n[Evidence excerpt is bounded; return to the source for the complete artifact]";
    post_chat_inject(payload.str());
    chat_open_view(agent ? "view.ai.agents" : "view.ai_chat");
    return true;
}

bool queue_evidence_for_chat(const std::string& evidence_id, std::string& reason)
{
    return queue_evidence(evidence_id, false, reason);
}

bool queue_evidence_for_agent(const std::string& evidence_id, std::string& reason)
{
    evidence_envelope_t envelope;
    if (!evidence_payload(evidence_id, envelope, reason)) return false;
    if (envelope.sensitive) {
        reason = "Sensitive evidence is blocked from agent transfer. Return to the source and create an explicitly redacted evidence envelope.";
        return false;
    }
    const std::string agent_name = aida::agent::active_agent_name();
    if (agent_name.empty() || aida::agent::get(agent_name) == nullptr) {
        reason = "Select an available agent before assigning evidence.";
        return false;
    }
    std::ostringstream prompt;
    prompt << "Review the assigned AiDA evidence without assuming ambient selection.\n"
           << "Evidence ID: " << envelope.id << "\n"
           << "Source view: " << envelope.source_view_id << "\n"
           << "Entity: " << envelope.entity_id << "\n"
           << "Revision: " << envelope.revision << "\n"
           << "Generation: " << envelope.generation << "\n"
           << "Snapshot hash: 0x" << std::hex << envelope.snapshot_hash << "\n"
           << "Content hash: 0x" << envelope.content_hash << std::dec << "\n\n"
           << envelope.excerpt;
    if (envelope.truncated)
        prompt << "\n[The evidence excerpt is bounded. Return to the retained source before making any mutation.]";
    const std::string parent_session = envelope.session_id.empty()
        ? chat_active_session() : envelope.session_id;
    const std::string task_id = "automation.evidence.agent." +
        std::to_string(s_evidence_sequence.fetch_add(1, std::memory_order_acq_rel));
    const auto cancelled = std::make_shared<std::atomic<bool>>(false);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "automation.evidence";
    submission.label = "evidence.assign_agent";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 3;
    submission.ui_access_policy = "immutable_snapshots_only";
    submission.failure_policy = "typed_diagnostic";
    submission.cancel_hook = [cancelled] {
        cancelled->store(true, std::memory_order_release);
    };
    submission.body = [agent_name, parent_session, assigned = prompt.str(),
                       task_id, cancelled] {
        std::string result;
        const bool completed = aida::agent::task::execute(
            agent_name, assigned, 0, parent_session, result, cancelled.get());
        const bool was_cancelled = cancelled->load(std::memory_order_acquire);
        std::string detail = completed ? result : aida::agent::task::last_error();
        if (detail.size() > 512U) detail.resize(512U);
        if (!completed && !was_cancelled)
            diag::log_tagged_fmt("chat", "evidence_agent_assignment_failed agent=%.96s error=%.256s",
                agent_name.c_str(), detail.c_str());
        const bool posted = aida::ui_thread::post(
            [task_id, completed, was_cancelled, detail = std::move(detail)]() mutable {
                const auto state = was_cancelled
                    ? aida::ui::task_center::task_state_t::cancelled
                    : completed ? aida::ui::task_center::task_state_t::completed
                                : aida::ui::task_center::task_state_t::failed;
                const std::string diagnostic = completed || was_cancelled
                    ? std::string{} : "automation.evidence.agent.failure." + task_id;
                static_cast<void>(aida::ui::task_center::update_task(task_id, state, 1.0f,
                    was_cancelled ? "Cancelled" : completed ? "Completed" : "Failed",
                    detail.empty() ? (was_cancelled ? "Agent assignment cancelled" :
                        completed ? "Agent assignment completed" : "Agent assignment failed")
                        : std::move(detail), diagnostic));
                if (!completed && !was_cancelled) {
                    aida::ui::task_center::diagnostic_registration_t registration;
                    registration.id = diagnostic;
                    registration.task_id = task_id;
                    registration.owner = "automation.evidence";
                    registration.summary = "Agent evidence assignment failed";
                    registration.details = "The selected agent did not complete the retained evidence assignment.";
                    registration.callbacks.focus = [] {
                        chat_open_view("view.ai.agents");
                    };
                    static_cast<void>(aida::ui::task_center::raise_diagnostic(
                        std::move(registration)));
                }
            }, "automation_evidence", "agent_assignment_result", "worker_result");
        if (!posted)
            diag::log_tagged_fmt("chat",
                "evidence_agent_assignment_result_dispatch_failed task=%.128s",
                task_id.c_str());
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        reason = submitted.reject_reason.empty()
            ? "The agent evidence assignment executor rejected the request."
            : submitted.reject_reason;
        return false;
    }
    aida::ui::task_center::task_registration_t registration;
    registration.id = task_id;
    registration.source = "automation.evidence";
    registration.owner = "automation.evidence";
    registration.owner_view = "view.ai.agents";
    registration.owner_action = "evidence.assign_agent";
    registration.session = parent_session;
    registration.target = envelope.id;
    registration.label = "Assign evidence to " + agent_name;
    registration.stage = "Queued for agent review";
    registration.cancellation_is_safe = true;
    registration.callbacks.focus = [] {
        chat_open_view("view.ai.agents");
    };
    if (!aida::ui::task_center::try_register_executor_job(submitted.task_id,
            std::move(registration))) {
        cancelled->store(true, std::memory_order_release);
        static_cast<void>(aida::infra::executor::cancel(submitted.task_id));
        reason = "Task Center rejected the agent evidence assignment.";
        return false;
    }
    chat_open_view("view.ai.agents");
    reason.clear();
    return true;
}

bool navigate_to_evidence_source(const std::string& evidence_id, std::string& reason)
{
    synchronize_evidence_session();
    evidence_envelope_t envelope;
    {
        std::lock_guard<std::mutex> lock(s_evidence_mutex);
        const auto found = std::find_if(s_evidence.begin(), s_evidence.end(), [&](const evidence_envelope_t& current) {
            return current.id == evidence_id;
        });
        if (found == s_evidence.end()) {
            reason = "The evidence item is no longer retained; capture it again from the source view.";
            return false;
        }
        envelope = *found;
    }
    std::function<bool(std::string&)> exact_return;
    {
        std::lock_guard<std::mutex> lock(s_evidence_mutex);
        const auto found = std::find_if(s_evidence_source_returns.begin(),
            s_evidence_source_returns.end(), [&](const auto& item) {
                return item.first == evidence_id;
            });
        if (found != s_evidence_source_returns.end()) exact_return = found->second;
    }
    if (exact_return) {
        reason.clear();
        return exact_return(reason);
    }
    if (s_chat_open_view_hook) {
        s_chat_open_view_hook(envelope.source_view_id);
        return true;
    }
    reason = "The source view host is unavailable.";
    return false;
}


}

mcp_client::manager_t& get_mcp_client_manager()
{
    return s_mcp_client_mgr;
}

mcp_standalone::server_t& get_local_mcp_server()
{
    return s_mcp_server;
}

std::vector<mcp_standalone::tool_def_t> snapshot_local_tools()
{
    std::vector<mcp_standalone::tool_def_t> out;
    const auto& tools = s_mcp_server.get_tools();
    out.reserve(tools.size());
    for (const auto& t : tools) out.push_back(t);
    return out;
}

std::string execute_local_tool(const std::string& name, const nlohmann::json& arguments)
{
    return execute_tool(name, arguments);
}

file_context::tracker_t& get_file_tracker()
{
    return s_file_tracker;
}

void do_process_attach(unsigned long pid)
{
    diag::log_tagged_fmt("chat", "do_process_attach pid=%lu driver_loaded=%d",
        pid, static_cast<int>(driver_bridge::is_loaded()));
    const uint32_t target_pid = static_cast<uint32_t>(pid);
    const uint32_t previous_pid = driver_bridge::attached_pid();
    if (previous_pid != 0 && previous_pid != target_pid)
        stealth_engine::disable_for_detach(previous_pid, "chat.process_attach.replace");
    if (driver_bridge::attach(pid)) {
        const bool stealth_ok = stealth_engine::ensure_default_enabled(target_pid, "chat.process_attach");
        output_log::push(bottom_tab_t::driver_log, "[driver] Attached to PID " + std::to_string(pid));
        diag::log_tagged_fmt("chat", "do_process_attach SUCCESS pid=%lu stealth_ok=%d", pid, stealth_ok ? 1 : 0);
    } else {
        if (previous_pid != 0 && driver_bridge::attached_pid() == previous_pid)
            (void)stealth_engine::ensure_default_enabled(previous_pid, "chat.process_attach.restore_failed_switch");
        output_log::push(bottom_tab_t::driver_log, "[driver] Failed to attach to PID " + std::to_string(pid) +
                         ": " + driver_bridge::last_error());
        diag::log_tagged_fmt("chat", "do_process_attach FAILED pid=%lu error='%s'",
            pid, driver_bridge::last_error().c_str());
    }
}

void do_process_detach()
{
    diag::log_tagged_fmt("chat", "do_process_detach pid=%u",
        driver_bridge::attached_pid());
    stealth_engine::disable_for_detach(driver_bridge::attached_pid(), "chat.process_detach");
    driver_bridge::detach();
    output_log::push(bottom_tab_t::driver_log, "[driver] Detached from process");
    diag::log_tagged("chat", "do_process_detach done");
}

bool is_process_attached()
{
    return driver_bridge::attached_pid() != 0;
}

std::string get_attached_process_name()
{
    return driver_bridge::status();
}

unsigned long get_attached_pid()
{
    return driver_bridge::attached_pid();
}



bool chat_toggle_agent_picker(std::string& error)
{
    if (!aida::automation_ui::s_agent_picker_toggle_hook) {
        error = "The agent picker is unavailable.";
        return false;
    }
    aida::automation_ui::s_agent_picker_toggle_hook();
    error.clear();
    return true;
}

bool chat_toggle_plan_build_agent(std::string& error)
{
    const std::string current = aida::agent::active_agent_name();
    const std::string target = current == "plan" ? "build" : "plan";
    if (!aida::agent::set_active_agent(target)) {
        error = "The requested plan/build agent is unavailable.";
        return false;
    }
    aida::events::publish(aida::events::event_agent_changed,
        aida::events::agent_changed_t{chat_active_session(), current, target});
    error.clear();
    return true;
}

