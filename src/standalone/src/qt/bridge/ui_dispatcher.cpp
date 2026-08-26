#include "qt/bridge/ui_dispatcher.hpp"

#include "core/ui/ui_thread_dispatcher.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include "helpers/diag_log.hpp"
#include "core/diagnostics/crash_snapshot.hpp"
#include "core/runtime/diagnostic_exception_scope.hpp"

namespace aida::qt {

static std::atomic<UiDispatcher*> g_ui_dispatch_context{nullptr};

}

namespace aida::ui_thread {

namespace {

struct ui_dispatch_task_t {
    std::uint64_t id = 0;
    DWORD producer_pid = 0;
    DWORD producer_tid = 0;
    DWORD ui_owner_tid_at_enqueue = 0;
    std::uint64_t queued_ms = 0;
    std::uint64_t deadline_ms = 0;
    std::string subsystem;
    std::string label;
    std::string phase;
    std::string owner;
    priority_t priority = priority_t::normal;
    bool cancellation_registered = false;
    std::function<bool()> cancelled;
    task_t task;
};

static constexpr std::size_t kUiDispatchMaxDepth = 2048;
static constexpr std::size_t kUiDispatchPressureDepth = 128;
static constexpr std::uint64_t kUiDispatchPressureIntervalMs = 1000;
static std::mutex g_ui_dispatch_mtx;
static std::deque<ui_dispatch_task_t> g_ui_dispatch_queue;
static std::atomic<DWORD> g_ui_owner_tid{0};
static std::atomic<bool> g_ui_dispatch_ready{false};
static std::atomic<bool> g_ui_dispatch_window_destroying{false};
static std::atomic<bool> g_ui_dispatch_wake_pending{false};
static std::atomic<bool> g_ui_dispatch_shutdown{false};
static std::atomic<std::uint64_t> g_ui_dispatch_next_id{0};
static std::atomic<std::uint64_t> g_ui_dispatch_enqueued{0};
static std::atomic<std::uint64_t> g_ui_dispatch_executed{0};
static std::atomic<std::uint64_t> g_ui_dispatch_discarded{0};
static std::atomic<std::uint64_t> g_ui_dispatch_rejected{0};
static std::atomic<std::uint64_t> g_ui_dispatch_rejected_shutdown{0};
static std::atomic<std::uint64_t> g_ui_dispatch_rejected_full{0};
static std::atomic<std::uint64_t> g_ui_dispatch_rejected_not_ready{0};
static std::atomic<std::uint64_t> g_ui_dispatch_rejected_cancelled{0};
static std::atomic<std::uint64_t> g_ui_dispatch_wake_posted{0};
static std::atomic<std::uint64_t> g_ui_dispatch_wake_thread_posted{0};
static std::atomic<std::uint64_t> g_ui_dispatch_wake_failed{0};
static std::atomic<std::uint64_t> g_ui_dispatch_wake_coalesced{0};
static std::atomic<std::uint64_t> g_ui_dispatch_drain_calls{0};
static std::atomic<std::uint64_t> g_ui_dispatch_drain_cancelled{0};
static std::atomic<std::uint64_t> g_ui_dispatch_budget_hits{0};
static std::atomic<std::uint64_t> g_ui_dispatch_backlog_logs{0};
static std::atomic<std::uint64_t> g_ui_dispatch_last_drain_ms{0};
static std::atomic<std::uint64_t> g_ui_dispatch_last_task_ms{0};
static std::atomic<std::uint64_t> g_ui_dispatch_last_task_id{0};
static std::atomic<std::size_t> g_ui_dispatch_last_depth{0};
static std::atomic<std::size_t> g_ui_dispatch_max_depth{0};
static std::atomic<std::uint64_t> g_ui_dispatch_oldest_queued_ms{0};
static std::atomic<std::uint64_t> g_ui_dispatch_last_backlog_log_ms{0};
static std::atomic<std::uint64_t> g_ui_dispatch_active_task_id{0};
static std::atomic<DWORD> g_ui_dispatch_active_producer_tid{0};
static std::atomic<std::uint64_t> g_ui_dispatch_active_started_ms{0};
static std::atomic<std::uint64_t> g_ui_dispatch_affinity_violations{0};
static std::atomic<std::uint64_t> g_ui_dispatch_last_drain_ts{0};
static std::atomic<std::uint64_t> g_ui_dispatch_last_wake_ts{0};
static std::atomic<std::uint64_t> g_ui_dispatch_task_budget_hits{0};
static std::atomic<std::uint64_t> g_ui_dispatch_time_budget_hits{0};

static std::uint64_t ui_dispatch_now_ms()
{
    return static_cast<std::uint64_t>(::GetTickCount64());
}

static const char* ui_dispatch_text(const char* value)
{
    return value && value[0] ? value : "<none>";
}

static const char* ui_dispatch_text(const std::string& value)
{
    return value.empty() ? "<none>" : value.c_str();
}

static std::string ui_dispatch_copy_text(const char* value, const char* fallback)
{
    const char* source = value && value[0] ? value : fallback;
    std::string out = source ? source : "";
    if (out.size() > 96)
        out.resize(96);
    for (char& ch : out) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F)
            ch = '_';
    }
    return out;
}

static int ui_dispatch_priority_rank(priority_t priority)
{
    switch (priority) {
    case priority_t::critical: return 3;
    case priority_t::high: return 2;
    case priority_t::normal: return 1;
    case priority_t::low: return 0;
    default: return 1;
    }
}

static void ui_dispatch_refresh_metrics_locked()
{
    const std::size_t depth = g_ui_dispatch_queue.size();
    g_ui_dispatch_last_depth.store(depth, std::memory_order_release);
    std::size_t max_depth = g_ui_dispatch_max_depth.load(std::memory_order_acquire);
    while (depth > max_depth && !g_ui_dispatch_max_depth.compare_exchange_weak(max_depth, depth, std::memory_order_acq_rel)) {
    }
    std::uint64_t oldest = 0;
    for (const ui_dispatch_task_t& task : g_ui_dispatch_queue) {
        if (task.queued_ms != 0 && (oldest == 0 || task.queued_ms < oldest))
            oldest = task.queued_ms;
    }
    g_ui_dispatch_oldest_queued_ms.store(oldest, std::memory_order_release);
}

static std::uint64_t ui_dispatch_oldest_age_ms(std::uint64_t now)
{
    const std::uint64_t oldest = g_ui_dispatch_oldest_queued_ms.load(std::memory_order_acquire);
    if (oldest == 0 || now < oldest)
        return 0;
    return now - oldest;
}

static void ui_dispatch_count_reject(enqueue_result_t result)
{
    g_ui_dispatch_rejected.fetch_add(1, std::memory_order_acq_rel);
    switch (result) {
    case enqueue_result_t::rejected_shutdown:
        g_ui_dispatch_rejected_shutdown.fetch_add(1, std::memory_order_acq_rel);
        break;
    case enqueue_result_t::rejected_full:
        g_ui_dispatch_rejected_full.fetch_add(1, std::memory_order_acq_rel);
        break;
    case enqueue_result_t::rejected_not_ui_ready:
        g_ui_dispatch_rejected_not_ready.fetch_add(1, std::memory_order_acq_rel);
        break;
    case enqueue_result_t::rejected_cancelled:
        g_ui_dispatch_rejected_cancelled.fetch_add(1, std::memory_order_acq_rel);
        break;
    default:
        break;
    }
}

static void ui_dispatch_log_reject(enqueue_result_t result,
    const char* reason,
    const ui_dispatch_task_t& task,
    std::size_t depth)
{
    ui_dispatch_count_reject(result);
    const std::uint64_t now = ui_dispatch_now_ms();
    diag::log_tagged_critical_fmt("ui_dispatcher",
        "UI-DISPATCHER-REJECT result=%s reason=%s task_id=%llu label=\"%.96s\" owner=\"%.96s\" subsystem=\"%.96s\" phase=\"%.96s\" priority=%s enqueue_pid=%lu enqueue_tid=%lu ui_owner_tid=%lu queued_ms=%llu now_ms=%llu deadline_ms=%llu cancellation=%d ready=%d shutdown=%d destroying=%d depth=%zu oldest_age_ms=%llu rejected_shutdown=%llu rejected_full=%llu rejected_not_ui_ready=%llu rejected_cancelled=%llu",
        result_name(result),
        ui_dispatch_text(reason),
        static_cast<unsigned long long>(task.id),
        ui_dispatch_text(task.label),
        ui_dispatch_text(task.owner),
        ui_dispatch_text(task.subsystem),
        ui_dispatch_text(task.phase),
        priority_name(task.priority),
        static_cast<unsigned long>(task.producer_pid),
        static_cast<unsigned long>(task.producer_tid),
        static_cast<unsigned long>(task.ui_owner_tid_at_enqueue),
        static_cast<unsigned long long>(task.queued_ms),
        static_cast<unsigned long long>(now),
        static_cast<unsigned long long>(task.deadline_ms),
        task.cancellation_registered ? 1 : 0,
        g_ui_dispatch_ready.load(std::memory_order_acquire) ? 1 : 0,
        g_ui_dispatch_shutdown.load(std::memory_order_acquire) ? 1 : 0,
        g_ui_dispatch_window_destroying.load(std::memory_order_acquire) ? 1 : 0,
        depth,
        static_cast<unsigned long long>(ui_dispatch_oldest_age_ms(now)),
        static_cast<unsigned long long>(g_ui_dispatch_rejected_shutdown.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_rejected_full.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_rejected_not_ready.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_rejected_cancelled.load(std::memory_order_acquire)));
}

static bool ui_dispatch_cancelled(const ui_dispatch_task_t& task, std::uint64_t now, const char** reason_out)
{
    if (task.deadline_ms != 0 && now >= task.deadline_ms) {
        if (reason_out)
            *reason_out = "deadline_expired";
        return true;
    }
    if (!task.cancelled)
        return false;
    bool cancelled_now = true;
    try {
        cancelled_now = task.cancelled();
    } catch (const std::exception& ex) {
        diag::log_tagged_critical_fmt("ui_dispatcher",
            "UI-DISPATCHER-REJECT result=%s reason=cancellation_probe_exception task_id=%llu label=\"%.96s\" owner=\"%.96s\" what=%.180s",
            result_name(enqueue_result_t::rejected_cancelled),
            static_cast<unsigned long long>(task.id),
            ui_dispatch_text(task.label),
            ui_dispatch_text(task.owner),
            ex.what());
    } catch (...) {
        aida::diagnostics::crash::emit_crash_breadcrumb(0xE06D7363u, nullptr, "ui_dispatcher_cancellation_probe");
        diag::log_tagged_critical_fmt("ui_dispatcher",
            "UI-DISPATCHER-REJECT result=%s reason=cancellation_probe_exception task_id=%llu label=\"%.96s\" owner=\"%.96s\" what=<unknown>",
            result_name(enqueue_result_t::rejected_cancelled),
            static_cast<unsigned long long>(task.id),
            ui_dispatch_text(task.label),
            ui_dispatch_text(task.owner));
    }
    if (cancelled_now && reason_out)
        *reason_out = "cancelled";
    return cancelled_now;
}

static bool ui_dispatch_post_wake_locked(const char* subsystem, const char* label, const char* phase)
{
    if (g_ui_dispatch_shutdown.load(std::memory_order_acquire) ||
        g_ui_dispatch_window_destroying.load(std::memory_order_acquire) ||
        !g_ui_dispatch_ready.load(std::memory_order_acquire)) {
        g_ui_dispatch_wake_failed.fetch_add(1, std::memory_order_acq_rel);
        diag::log_tagged_critical_fmt("UI-DISPATCHER-BACKLOG",
            "UI-DISPATCHER-WAKE reason=blocked subsystem=%s label=%s phase=%s owner_tid=%lu ready=%d shutdown=%d destroying=%d depth=%zu wake_failed=%llu payload=0",
            ui_dispatch_text(subsystem),
            ui_dispatch_text(label),
            ui_dispatch_text(phase),
            static_cast<unsigned long>(g_ui_owner_tid.load(std::memory_order_acquire)),
            g_ui_dispatch_ready.load(std::memory_order_acquire) ? 1 : 0,
            g_ui_dispatch_shutdown.load(std::memory_order_acquire) ? 1 : 0,
            g_ui_dispatch_window_destroying.load(std::memory_order_acquire) ? 1 : 0,
            g_ui_dispatch_last_depth.load(std::memory_order_acquire),
            static_cast<unsigned long long>(g_ui_dispatch_wake_failed.load(std::memory_order_acquire)));
        return false;
    }
    if (g_ui_dispatch_wake_pending.exchange(true, std::memory_order_acq_rel)) {
        g_ui_dispatch_wake_coalesced.fetch_add(1, std::memory_order_acq_rel);
        diag::log_tagged_fmt("ui_dispatcher",
            "UI-DISPATCHER-WAKE reason=coalesced subsystem=%s label=%s phase=%s owner_tid=%lu depth=%zu oldest_age_ms=%llu wake_pending=1 coalesced=%llu payload=0",
            ui_dispatch_text(subsystem),
            ui_dispatch_text(label),
            ui_dispatch_text(phase),
            static_cast<unsigned long>(g_ui_owner_tid.load(std::memory_order_acquire)),
            g_ui_dispatch_last_depth.load(std::memory_order_acquire),
            static_cast<unsigned long long>(ui_dispatch_oldest_age_ms(ui_dispatch_now_ms())),
            static_cast<unsigned long long>(g_ui_dispatch_wake_coalesced.load(std::memory_order_acquire)));
        return true;
    }

    aida::qt::UiDispatcher* context = aida::qt::g_ui_dispatch_context.load(std::memory_order_acquire);
    bool posted = false;
    if (context)
        posted = QMetaObject::invokeMethod(context, &aida::qt::UiDispatcher::drainWakeSlot, Qt::QueuedConnection);

    if (!posted) {
        g_ui_dispatch_wake_pending.store(false, std::memory_order_release);
        g_ui_dispatch_wake_failed.fetch_add(1, std::memory_order_acq_rel);
        diag::log_tagged_critical_fmt("UI-DISPATCHER-BACKLOG",
            "UI-DISPATCHER-WAKE reason=failed subsystem=%s label=%s phase=%s ctx=0x%llX owner_tid=%lu pending=%zu wake_failed=%llu tid=%lu payload=0",
            ui_dispatch_text(subsystem),
            ui_dispatch_text(label),
            ui_dispatch_text(phase),
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(context)),
            static_cast<unsigned long>(g_ui_owner_tid.load(std::memory_order_acquire)),
            g_ui_dispatch_last_depth.load(std::memory_order_acquire),
            static_cast<unsigned long long>(g_ui_dispatch_wake_failed.load(std::memory_order_acquire)),
            static_cast<unsigned long>(::GetCurrentThreadId()));
        return false;
    }

    g_ui_dispatch_wake_posted.fetch_add(1, std::memory_order_acq_rel);
    g_ui_dispatch_last_wake_ts.store(ui_dispatch_now_ms(), std::memory_order_release);
    diag::log_tagged_critical_fmt("ui_dispatcher",
        "UI-DISPATCHER-WAKE reason=posted subsystem=%s label=%s phase=%s ctx=0x%llX invoke_posted=%d owner_tid=%lu wake_pending=%d depth=%zu oldest_age_ms=%llu posts=%llu thread_posts=%llu failures=%llu payload=0",
        ui_dispatch_text(subsystem),
        ui_dispatch_text(label),
        ui_dispatch_text(phase),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(context)),
        posted ? 1 : 0,
        static_cast<unsigned long>(g_ui_owner_tid.load(std::memory_order_acquire)),
        g_ui_dispatch_wake_pending.load(std::memory_order_acquire) ? 1 : 0,
        g_ui_dispatch_last_depth.load(std::memory_order_acquire),
        static_cast<unsigned long long>(ui_dispatch_oldest_age_ms(ui_dispatch_now_ms())),
        static_cast<unsigned long long>(g_ui_dispatch_wake_posted.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_wake_thread_posted.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_wake_failed.load(std::memory_order_acquire)));
    return true;
}

static void ui_dispatch_log_pressure(const char* event,
    const char* subsystem,
    const char* label,
    const char* phase,
    std::size_t depth,
    std::uint64_t id,
    std::uint64_t wait_ms,
    bool force)
{
    static std::atomic<std::uint64_t> s_last_pressure_log_ms{0};
    const std::uint64_t now = ui_dispatch_now_ms();
    std::uint64_t last = s_last_pressure_log_ms.load(std::memory_order_acquire);
    if (!force) {
        if (depth < kUiDispatchPressureDepth && now - last < kUiDispatchPressureIntervalMs)
            return;
        if (now - last < kUiDispatchPressureIntervalMs)
            return;
        if (!s_last_pressure_log_ms.compare_exchange_strong(last, now, std::memory_order_acq_rel))
            return;
    } else {
        s_last_pressure_log_ms.store(now, std::memory_order_release);
    }
    g_ui_dispatch_backlog_logs.fetch_add(1, std::memory_order_acq_rel);
    diag::log_tagged_critical_fmt("UI-DISPATCHER-BACKLOG",
        "event=%s subsystem=%s label=%s phase=%s id=%llu depth=%zu wait_ms=%llu owner_tid=%lu current_tid=%lu enqueued=%llu executed=%llu discarded=%llu rejected=%llu wake_pending=%d wake_posted=%llu wake_failed=%llu shutdown=%d",
        ui_dispatch_text(event),
        ui_dispatch_text(subsystem),
        ui_dispatch_text(label),
        ui_dispatch_text(phase),
        static_cast<unsigned long long>(id),
        depth,
        static_cast<unsigned long long>(wait_ms),
        static_cast<unsigned long>(g_ui_owner_tid.load(std::memory_order_acquire)),
        static_cast<unsigned long>(::GetCurrentThreadId()),
        static_cast<unsigned long long>(g_ui_dispatch_enqueued.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_executed.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_discarded.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_rejected.load(std::memory_order_acquire)),
        g_ui_dispatch_wake_pending.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(g_ui_dispatch_wake_posted.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_wake_failed.load(std::memory_order_acquire)),
        g_ui_dispatch_shutdown.load(std::memory_order_acquire) ? 1 : 0);
}

static void ui_affinity_log_marker(const char* marker,
    const char* subsystem,
    const char* label,
    const char* phase,
    std::size_t depth,
    DWORD gle)
{
    static std::atomic<std::uint64_t> s_last_violation_log_ms{0};
    static std::atomic<std::uint64_t> s_last_routed_log_ms{0};
    static std::atomic<std::uint64_t> s_violation_suppressed{0};
    static std::atomic<std::uint64_t> s_routed_suppressed{0};
    const bool routed = marker && std::strcmp(marker, "UI-AFFINITY-ROUTED") == 0;
    if (!routed)
        g_ui_dispatch_affinity_violations.fetch_add(1, std::memory_order_acq_rel);
    std::atomic<std::uint64_t>& last_ref = routed ? s_last_routed_log_ms : s_last_violation_log_ms;
    std::atomic<std::uint64_t>& suppressed_ref = routed ? s_routed_suppressed : s_violation_suppressed;
    const std::uint64_t now = ui_dispatch_now_ms();
    std::uint64_t last = last_ref.load(std::memory_order_acquire);
    if (last != 0 && now - last < 1000ULL) {
        suppressed_ref.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    if (!last_ref.compare_exchange_strong(last, now, std::memory_order_acq_rel)) {
        suppressed_ref.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    const std::uint64_t suppressed = suppressed_ref.exchange(0, std::memory_order_acq_rel);
    diag::log_tagged_critical_fmt("ui_affinity",
        "%s owner_tid=%lu current_tid=%lu subsystem=%s label=%s phase=%s queue_depth=%zu enqueued=%llu executed=%llu discarded=%llu rejected=%llu wake_pending=%d wake_posted=%llu wake_failed=%llu suppressed=%llu gle=%lu",
        marker ? marker : "UI-AFFINITY-VIOLATION",
        static_cast<unsigned long>(g_ui_owner_tid.load(std::memory_order_acquire)),
        static_cast<unsigned long>(::GetCurrentThreadId()),
        ui_dispatch_text(subsystem),
        ui_dispatch_text(label),
        ui_dispatch_text(phase),
        depth,
        static_cast<unsigned long long>(g_ui_dispatch_enqueued.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_executed.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_discarded.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_rejected.load(std::memory_order_acquire)),
        g_ui_dispatch_wake_pending.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(g_ui_dispatch_wake_posted.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_wake_failed.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(suppressed),
        static_cast<unsigned long>(gle));
}

}

const char* result_name(enqueue_result_t result)
{
    switch (result) {
    case enqueue_result_t::accepted: return "accepted";
    case enqueue_result_t::rejected_shutdown: return "rejected_shutdown";
    case enqueue_result_t::rejected_full: return "rejected_full";
    case enqueue_result_t::rejected_not_ui_ready: return "rejected_not_ui_ready";
    case enqueue_result_t::rejected_cancelled: return "rejected_cancelled";
    default: return "unknown";
    }
}

const char* priority_name(priority_t priority)
{
    switch (priority) {
    case priority_t::low: return "low";
    case priority_t::normal: return "normal";
    case priority_t::high: return "high";
    case priority_t::critical: return "critical";
    default: return "normal";
    }
}

void capture_owner_tid(unsigned long tid, const char* subsystem, const char* label, const char* phase)
{
    if (tid == 0)
        return;
    DWORD expected = 0;
    if (g_ui_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
        diag::log_tagged_critical_fmt("UI-DISPATCHER-BACKLOG",
            "owner_capture subsystem=%s label=%s phase=%s previous_tid=%lu owner_tid=%lu current_tid=%lu",
            ui_dispatch_text(subsystem),
            ui_dispatch_text(label),
            ui_dispatch_text(phase),
            0UL,
            static_cast<unsigned long>(tid),
            static_cast<unsigned long>(::GetCurrentThreadId()));
        return;
    }
    if (expected != tid)
        ui_affinity_log_marker("UI-AFFINITY-VIOLATION", subsystem, label, phase, pending_count(), GetLastError());
}

unsigned long owner_tid()
{
    return g_ui_owner_tid.load(std::memory_order_acquire);
}

bool is_owner_thread()
{
    const DWORD owner = static_cast<DWORD>(owner_tid());
    return owner != 0 && owner == ::GetCurrentThreadId();
}

bool require_owner(const char* subsystem, const char* label, const char* phase)
{
    if (is_owner_thread())
        return true;
    g_ui_dispatch_rejected.fetch_add(1, std::memory_order_acq_rel);
    ui_affinity_log_marker("UI-AFFINITY-VIOLATION", subsystem, label, phase, pending_count(), GetLastError());
    return false;
}

enqueue_result_t post(task_t task, post_options_t options)
{
    const std::uint64_t queued_ms = ui_dispatch_now_ms();
    ui_dispatch_task_t item;
    item.id = g_ui_dispatch_next_id.fetch_add(1, std::memory_order_acq_rel) + 1;
    item.producer_pid = ::GetCurrentProcessId();
    item.producer_tid = ::GetCurrentThreadId();
    item.ui_owner_tid_at_enqueue = static_cast<DWORD>(owner_tid());
    item.queued_ms = queued_ms;
    item.deadline_ms = options.deadline_ms;
    item.subsystem = ui_dispatch_copy_text(options.subsystem, "ui");
    item.label = ui_dispatch_copy_text(options.label, "task");
    item.phase = ui_dispatch_copy_text(options.phase, "unspecified");
    item.owner = ui_dispatch_copy_text(options.owner, item.subsystem.c_str());
    item.priority = options.priority;
    item.cancellation_registered = static_cast<bool>(options.cancelled);
    item.cancelled = std::move(options.cancelled);
    item.task = std::move(task);

    auto log_route_reject = [&](std::size_t depth, DWORD gle) {
        if (!is_owner_thread())
            ui_affinity_log_marker("UI-AFFINITY-VIOLATION",
                item.subsystem.c_str(),
                item.label.c_str(),
                item.phase.c_str(),
                depth,
                gle);
    };

    if (!item.task) {
        ui_dispatch_log_reject(enqueue_result_t::rejected_cancelled, "empty_task", item, pending_count());
        log_route_reject(g_ui_dispatch_last_depth.load(std::memory_order_acquire), ERROR_CANCELLED);
        return enqueue_result_t::rejected_cancelled;
    }
    if (g_ui_dispatch_shutdown.load(std::memory_order_acquire) || g_ui_dispatch_window_destroying.load(std::memory_order_acquire)) {
        ui_dispatch_log_reject(enqueue_result_t::rejected_shutdown, "shutdown_or_window_destroying", item, pending_count());
        log_route_reject(g_ui_dispatch_last_depth.load(std::memory_order_acquire), ERROR_SHUTDOWN_IN_PROGRESS);
        return enqueue_result_t::rejected_shutdown;
    }
    if (!g_ui_dispatch_ready.load(std::memory_order_acquire) || item.ui_owner_tid_at_enqueue == 0) {
        ui_dispatch_log_reject(enqueue_result_t::rejected_not_ui_ready, "ui_not_ready", item, pending_count());
        log_route_reject(g_ui_dispatch_last_depth.load(std::memory_order_acquire), ERROR_NOT_READY);
        return enqueue_result_t::rejected_not_ui_ready;
    }
    const char* cancel_reason = nullptr;
    if (ui_dispatch_cancelled(item, queued_ms, &cancel_reason)) {
        ui_dispatch_log_reject(enqueue_result_t::rejected_cancelled, cancel_reason ? cancel_reason : "cancelled", item, pending_count());
        log_route_reject(g_ui_dispatch_last_depth.load(std::memory_order_acquire), ERROR_CANCELLED);
        return enqueue_result_t::rejected_cancelled;
    }
    const std::uint64_t log_id = item.id;
    const DWORD log_pid = item.producer_pid;
    const DWORD log_tid = item.producer_tid;
    const DWORD log_owner_tid = item.ui_owner_tid_at_enqueue;
    const std::uint64_t log_deadline = item.deadline_ms;
    const std::string log_label = item.label;
    const std::string log_owner = item.owner;
    const std::string log_subsystem = item.subsystem;
    const std::string log_phase = item.phase;
    const priority_t log_priority = item.priority;
    const bool log_cancellation = item.cancellation_registered;

    std::size_t depth = 0;
    {
        std::lock_guard<std::mutex> lock(g_ui_dispatch_mtx);
        if (g_ui_dispatch_shutdown.load(std::memory_order_acquire) || g_ui_dispatch_window_destroying.load(std::memory_order_acquire)) {
            ui_dispatch_refresh_metrics_locked();
            const std::size_t locked_depth = g_ui_dispatch_last_depth.load(std::memory_order_acquire);
            ui_dispatch_log_reject(enqueue_result_t::rejected_shutdown, "shutdown_or_window_destroying_locked", item, locked_depth);
            log_route_reject(locked_depth, ERROR_SHUTDOWN_IN_PROGRESS);
            return enqueue_result_t::rejected_shutdown;
        }
        if (!g_ui_dispatch_ready.load(std::memory_order_acquire) || owner_tid() == 0) {
            ui_dispatch_refresh_metrics_locked();
            const std::size_t locked_depth = g_ui_dispatch_last_depth.load(std::memory_order_acquire);
            ui_dispatch_log_reject(enqueue_result_t::rejected_not_ui_ready, "ui_not_ready_locked", item, locked_depth);
            log_route_reject(locked_depth, ERROR_NOT_READY);
            return enqueue_result_t::rejected_not_ui_ready;
        }
        if (g_ui_dispatch_queue.size() >= kUiDispatchMaxDepth) {
            depth = g_ui_dispatch_queue.size();
            ui_dispatch_refresh_metrics_locked();
            ui_dispatch_log_reject(enqueue_result_t::rejected_full, "queue_full", item, depth);
            log_route_reject(depth, ERROR_NOT_ENOUGH_MEMORY);
            return enqueue_result_t::rejected_full;
        }
        g_ui_dispatch_queue.push_back(std::move(item));
        depth = g_ui_dispatch_queue.size();
        ui_dispatch_refresh_metrics_locked();
        g_ui_dispatch_enqueued.fetch_add(1, std::memory_order_acq_rel);
    }

    const std::uint64_t now = ui_dispatch_now_ms();
    diag::log_tagged_critical_fmt("ui_dispatcher",
        "UI-DISPATCHER-ENQUEUE result=%s task_id=%llu label=\"%.96s\" owner=\"%.96s\" subsystem=\"%.96s\" phase=\"%.96s\" priority=%s enqueue_pid=%lu enqueue_tid=%lu ui_owner_tid=%lu queued_ms=%llu deadline_ms=%llu cancellation=%d depth=%zu max_depth=%zu oldest_age_ms=%llu accepted=%llu",
        result_name(enqueue_result_t::accepted),
        static_cast<unsigned long long>(log_id),
        ui_dispatch_text(log_label),
        ui_dispatch_text(log_owner),
        ui_dispatch_text(log_subsystem),
        ui_dispatch_text(log_phase),
        priority_name(log_priority),
        static_cast<unsigned long>(log_pid),
        static_cast<unsigned long>(log_tid),
        static_cast<unsigned long>(log_owner_tid),
        static_cast<unsigned long long>(queued_ms),
        static_cast<unsigned long long>(log_deadline),
        log_cancellation ? 1 : 0,
        depth,
        g_ui_dispatch_max_depth.load(std::memory_order_acquire),
        static_cast<unsigned long long>(ui_dispatch_oldest_age_ms(now)),
        static_cast<unsigned long long>(g_ui_dispatch_enqueued.load(std::memory_order_acquire)));
    ui_dispatch_log_pressure("queued", options.subsystem, options.label, options.phase, depth, g_ui_dispatch_next_id.load(std::memory_order_acquire), 0, false);
    const bool wake_posted = ui_dispatch_post_wake_locked(options.subsystem, options.label, options.phase);
    const unsigned long owner = owner_tid();
    if (owner != 0 && ::GetCurrentThreadId() != owner)
        ui_affinity_log_marker(wake_posted ? "UI-AFFINITY-ROUTED" : "UI-AFFINITY-VIOLATION",
            options.subsystem,
            options.label,
            options.phase,
            depth,
            wake_posted ? 0UL : GetLastError());
    return enqueue_result_t::accepted;
}

bool post(task_t task, const char* subsystem, const char* label, const char* phase)
{
    post_options_t options;
    options.subsystem = subsystem;
    options.label = label;
    options.phase = phase;
    options.owner = subsystem;
    options.priority = priority_t::normal;
    return post(std::move(task), std::move(options)) == enqueue_result_t::accepted;
}

bool wake(const char* subsystem, const char* label, const char* phase)
{
    const bool posted = ui_dispatch_post_wake_locked(subsystem, label, phase);
    const unsigned long owner = owner_tid();
    if (owner != 0 && owner != ::GetCurrentThreadId())
        ui_affinity_log_marker(posted ? "UI-AFFINITY-ROUTED" : "UI-AFFINITY-VIOLATION",
            subsystem,
            label,
            phase,
            pending_count(),
            posted ? 0UL : GetLastError());
    return posted;
}

std::uint32_t drain(std::uint32_t task_budget, std::uint64_t time_budget_ms, const char* phase)
{
    capture_owner_tid(::GetCurrentThreadId(), "ui_dispatcher", "drain", phase);
    g_ui_dispatch_drain_calls.fetch_add(1, std::memory_order_acq_rel);
    if (g_ui_dispatch_shutdown.load(std::memory_order_acquire)) {
        shutdown();
        return 0;
    }
    if (!g_ui_dispatch_ready.load(std::memory_order_acquire) ||
        g_ui_dispatch_window_destroying.load(std::memory_order_acquire))
        return 0;

    const std::uint32_t max_tasks = task_budget == 0 ? 1u : task_budget;
    const std::uint64_t budget_ms = time_budget_ms == 0 ? 1u : time_budget_ms;
    const std::uint64_t drain_start = ui_dispatch_now_ms();
    std::uint32_t ran = 0;
    for (;;) {
        ui_dispatch_task_t item;
        std::size_t depth_after_pop = 0;
        {
            std::lock_guard<std::mutex> lock(g_ui_dispatch_mtx);
            if (g_ui_dispatch_queue.empty()) {
                ui_dispatch_refresh_metrics_locked();
                break;
            }
            auto best = g_ui_dispatch_queue.begin();
            for (auto it = g_ui_dispatch_queue.begin(); it != g_ui_dispatch_queue.end(); ++it) {
                const int rank = ui_dispatch_priority_rank(it->priority);
                const int best_rank = ui_dispatch_priority_rank(best->priority);
                if (rank > best_rank || (rank == best_rank && it->queued_ms < best->queued_ms))
                    best = it;
            }
            item = std::move(*best);
            g_ui_dispatch_queue.erase(best);
            depth_after_pop = g_ui_dispatch_queue.size();
            ui_dispatch_refresh_metrics_locked();
        }

        if (!item.task) {
            g_ui_dispatch_discarded.fetch_add(1, std::memory_order_acq_rel);
            ui_dispatch_log_pressure("discard_empty_task", item.subsystem.c_str(), item.label.c_str(), item.phase.c_str(), depth_after_pop, item.id, 0, true);
        } else {
            const std::uint64_t task_start = ui_dispatch_now_ms();
            const std::uint64_t wait_ms = task_start >= item.queued_ms ? task_start - item.queued_ms : 0;
            const char* cancel_reason = nullptr;
            if (ui_dispatch_cancelled(item, task_start, &cancel_reason)) {
                g_ui_dispatch_drain_cancelled.fetch_add(1, std::memory_order_acq_rel);
                g_ui_dispatch_discarded.fetch_add(1, std::memory_order_acq_rel);
                ui_dispatch_log_reject(enqueue_result_t::rejected_cancelled, cancel_reason ? cancel_reason : "cancelled_before_drain", item, depth_after_pop);
            } else {
                if (wait_ms >= 250 || depth_after_pop >= kUiDispatchPressureDepth)
                    ui_dispatch_log_pressure("dequeue_pressure", item.subsystem.c_str(), item.label.c_str(), item.phase.c_str(), depth_after_pop, item.id, wait_ms, false);
                g_ui_dispatch_active_task_id.store(item.id, std::memory_order_release);
                g_ui_dispatch_active_producer_tid.store(item.producer_tid, std::memory_order_release);
                g_ui_dispatch_active_started_ms.store(task_start, std::memory_order_release);
                diag::log_tagged_critical_fmt("ui_dispatcher",
                    "UI-DISPATCHER-DRAIN event=task_start phase=%s task_id=%llu label=\"%.96s\" owner=\"%.96s\" subsystem=\"%.96s\" priority=%s enqueue_pid=%lu enqueue_tid=%lu ui_owner_tid=%lu ui_tid=%lu queued_ms=%llu queued_age_ms=%llu deadline_ms=%llu cancellation=%d remaining_before=%zu",
                    ui_dispatch_text(phase),
                    static_cast<unsigned long long>(item.id),
                    ui_dispatch_text(item.label),
                    ui_dispatch_text(item.owner),
                    ui_dispatch_text(item.subsystem),
                    priority_name(item.priority),
                    static_cast<unsigned long>(item.producer_pid),
                    static_cast<unsigned long>(item.producer_tid),
                    static_cast<unsigned long>(item.ui_owner_tid_at_enqueue),
                    static_cast<unsigned long>(::GetCurrentThreadId()),
                    static_cast<unsigned long long>(item.queued_ms),
                    static_cast<unsigned long long>(wait_ms),
                    static_cast<unsigned long long>(item.deadline_ms),
                    item.cancellation_registered ? 1 : 0,
                    depth_after_pop);
            try {
                aida::diagnostic_exception_scope::scope_t exception_scope("ui_thread_dispatcher.task");
                item.task();
                g_ui_dispatch_executed.fetch_add(1, std::memory_order_acq_rel);
            } catch (const std::exception& ex) {
                g_ui_dispatch_discarded.fetch_add(1, std::memory_order_acq_rel);
                diag::log_tagged_critical_fmt("UI-DISPATCHER-BACKLOG",
                    "task_exception subsystem=%s label=%s phase=%s id=%llu wait_ms=%llu err=%s",
                    ui_dispatch_text(item.subsystem),
                    ui_dispatch_text(item.label),
                    ui_dispatch_text(item.phase),
                    static_cast<unsigned long long>(item.id),
                    static_cast<unsigned long long>(wait_ms),
                    ex.what());
            } catch (...) {
                aida::diagnostics::crash::emit_crash_breadcrumb(0xE06D7363u, nullptr, "ui_dispatcher_backlog");
                g_ui_dispatch_discarded.fetch_add(1, std::memory_order_acq_rel);
                diag::log_tagged_critical_fmt("UI-DISPATCHER-BACKLOG",
                    "task_unknown_exception subsystem=%s label=%s phase=%s id=%llu wait_ms=%llu",
                    ui_dispatch_text(item.subsystem),
                    ui_dispatch_text(item.label),
                    ui_dispatch_text(item.phase),
                    static_cast<unsigned long long>(item.id),
                    static_cast<unsigned long long>(wait_ms));
            }
            const std::uint64_t task_ms = ui_dispatch_now_ms() - task_start;
            g_ui_dispatch_last_task_ms.store(task_ms, std::memory_order_release);
            g_ui_dispatch_last_task_id.store(item.id, std::memory_order_release);
            g_ui_dispatch_active_task_id.store(0, std::memory_order_release);
            g_ui_dispatch_active_producer_tid.store(0, std::memory_order_release);
            g_ui_dispatch_active_started_ms.store(0, std::memory_order_release);
            diag::log_tagged_critical_fmt("ui_dispatcher",
                "UI-DISPATCHER-DRAIN event=task_end phase=%s task_id=%llu label=\"%.96s\" owner=\"%.96s\" run_ms=%llu ran=%u remaining_after=%zu total_drained=%llu",
                ui_dispatch_text(phase),
                static_cast<unsigned long long>(item.id),
                ui_dispatch_text(item.label),
                ui_dispatch_text(item.owner),
                static_cast<unsigned long long>(task_ms),
                ran + 1,
                g_ui_dispatch_last_depth.load(std::memory_order_acquire),
                static_cast<unsigned long long>(g_ui_dispatch_executed.load(std::memory_order_acquire)));
            if (task_ms >= 8) {
                ui_dispatch_log_pressure("task_slow", item.subsystem.c_str(), item.label.c_str(), item.phase.c_str(), depth_after_pop, item.id, wait_ms, true);
            }
            }
        }

        ++ran;
        const std::uint64_t elapsed = ui_dispatch_now_ms() - drain_start;
        if (ran >= max_tasks) {
            g_ui_dispatch_task_budget_hits.fetch_add(1, std::memory_order_acq_rel);
            break;
        }
        if (elapsed >= budget_ms) {
            g_ui_dispatch_time_budget_hits.fetch_add(1, std::memory_order_acq_rel);
            break;
        }
    }

    const std::uint64_t drain_ms = ui_dispatch_now_ms() - drain_start;
    g_ui_dispatch_last_drain_ms.store(drain_ms, std::memory_order_release);
    g_ui_dispatch_last_drain_ts.store(ui_dispatch_now_ms(), std::memory_order_release);
    const std::size_t remaining = pending_count();
    if (remaining != 0) {
        g_ui_dispatch_budget_hits.fetch_add(1, std::memory_order_acq_rel);
        diag::log_tagged_critical_fmt("ui_dispatcher",
            "UI-DISPATCHER-BUDGET-HIT phase=%s ran=%u remaining=%zu task_budget=%u time_budget_ms=%llu elapsed_ms=%llu oldest_age_ms=%llu budget_hits=%llu",
            ui_dispatch_text(phase),
            ran,
            remaining,
            max_tasks,
            static_cast<unsigned long long>(budget_ms),
            static_cast<unsigned long long>(drain_ms),
            static_cast<unsigned long long>(ui_dispatch_oldest_age_ms(ui_dispatch_now_ms())),
            static_cast<unsigned long long>(g_ui_dispatch_budget_hits.load(std::memory_order_acquire)));
        ui_dispatch_log_pressure("drain_budget_yield", "ui_dispatcher", "drain", phase, remaining, 0, drain_ms, false);
        wake("ui_dispatcher", "drain_rewake", phase);
    } else {
        g_ui_dispatch_wake_pending.store(false, std::memory_order_release);
    }
    if (ran != 0 || remaining != 0) {
        diag::log_tagged_fmt("ui_dispatcher",
            "UI-DISPATCHER-DRAIN event=summary phase=%s ran=%u remaining=%zu elapsed_ms=%llu task_budget=%u time_budget_ms=%llu drain_calls=%llu drain_cancelled=%llu oldest_age_ms=%llu",
            ui_dispatch_text(phase),
            ran,
            remaining,
            static_cast<unsigned long long>(drain_ms),
            max_tasks,
            static_cast<unsigned long long>(budget_ms),
            static_cast<unsigned long long>(g_ui_dispatch_drain_calls.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(g_ui_dispatch_drain_cancelled.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(ui_dispatch_oldest_age_ms(ui_dispatch_now_ms())));
    }
    return ran;
}

std::size_t pending_count()
{
    std::lock_guard<std::mutex> lock(g_ui_dispatch_mtx);
    ui_dispatch_refresh_metrics_locked();
    return g_ui_dispatch_queue.size();
}

void format_snapshot(char* out, std::size_t cap)
{
    if (!out || cap == 0)
        return;
    const std::uint64_t now = ui_dispatch_now_ms();
    const std::size_t pending = pending_count();
    const std::uint64_t active_started = g_ui_dispatch_active_started_ms.load(std::memory_order_acquire);
    const std::uint64_t active_age = active_started != 0 && now >= active_started ? now - active_started : 0;
    _snprintf_s(out, cap, _TRUNCATE,
        "ui_dispatcher{ready=%d shutdown=%d destroying=%d ctx=0x%llX pending=%zu max_depth=%zu oldest_age_ms=%llu owner_tid=%lu current_tid=%lu wake_pending=%d enqueued=%llu executed=%llu discarded=%llu rejected=%llu rejected_shutdown=%llu rejected_full=%llu rejected_not_ready=%llu rejected_cancelled=%llu drain_calls=%llu drain_cancelled=%llu budget_hits=%llu task_budget_hits=%llu time_budget_hits=%llu affinity_violations=%llu last_drain_ts=%llu last_wake_ts=%llu wake_posted=%llu wake_thread_posted=%llu wake_coalesced=%llu wake_failed=%llu backlog_logs=%llu last_drain_ms=%llu last_task_id=%llu last_task_ms=%llu active_task=%llu active_producer_tid=%lu active_age_ms=%llu}",
        g_ui_dispatch_ready.load(std::memory_order_acquire) ? 1 : 0,
        g_ui_dispatch_shutdown.load(std::memory_order_acquire) ? 1 : 0,
        g_ui_dispatch_window_destroying.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(aida::qt::g_ui_dispatch_context.load(std::memory_order_acquire))),
        pending,
        g_ui_dispatch_max_depth.load(std::memory_order_acquire),
        static_cast<unsigned long long>(ui_dispatch_oldest_age_ms(now)),
        static_cast<unsigned long>(owner_tid()),
        static_cast<unsigned long>(::GetCurrentThreadId()),
        g_ui_dispatch_wake_pending.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(g_ui_dispatch_enqueued.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_executed.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_discarded.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_rejected.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_rejected_shutdown.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_rejected_full.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_rejected_not_ready.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_rejected_cancelled.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_drain_calls.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_drain_cancelled.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_budget_hits.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_task_budget_hits.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_time_budget_hits.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_affinity_violations.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_last_drain_ts.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_last_wake_ts.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_wake_posted.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_wake_thread_posted.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_wake_coalesced.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_wake_failed.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_backlog_logs.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_last_drain_ms.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_last_task_id.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_last_task_ms.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_dispatch_active_task_id.load(std::memory_order_acquire)),
        static_cast<unsigned long>(g_ui_dispatch_active_producer_tid.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(active_age));
}

std::uint64_t affinity_violation_count()
{
    return g_ui_dispatch_affinity_violations.load(std::memory_order_acquire);
}

std::uint64_t last_drain_timestamp()
{
    return g_ui_dispatch_last_drain_ts.load(std::memory_order_acquire);
}

std::uint64_t last_wake_timestamp()
{
    return g_ui_dispatch_last_wake_ts.load(std::memory_order_acquire);
}

std::uint64_t task_budget_hit_count()
{
    return g_ui_dispatch_task_budget_hits.load(std::memory_order_acquire);
}

std::uint64_t time_budget_hit_count()
{
    return g_ui_dispatch_time_budget_hits.load(std::memory_order_acquire);
}

std::uint64_t budget_hit_count()
{
    return g_ui_dispatch_budget_hits.load(std::memory_order_acquire);
}

std::uint64_t rejected_count()
{
    return g_ui_dispatch_rejected.load(std::memory_order_acquire);
}

std::uint64_t drained_count()
{
    return g_ui_dispatch_drain_calls.load(std::memory_order_acquire);
}

bool wake_pending()
{
    return g_ui_dispatch_wake_pending.load(std::memory_order_acquire);
}

std::uint64_t oldest_queued_age_ms()
{
    const std::uint64_t now = ui_dispatch_now_ms();
    std::lock_guard<std::mutex> lock(g_ui_dispatch_mtx);
    ui_dispatch_refresh_metrics_locked();
    return ui_dispatch_oldest_age_ms(now);
}

std::string top_queued_labels(std::size_t max_entries)
{
    if (max_entries == 0)
        max_entries = 8;
    std::string out;
    std::lock_guard<std::mutex> lock(g_ui_dispatch_mtx);
    std::size_t count = 0;
    for (const auto& item : g_ui_dispatch_queue) {
        if (count >= max_entries)
            break;
        if (count > 0)
            out += '|';
        out += ui_dispatch_copy_text(item.label.c_str(), "");
        out += '/';
        out += ui_dispatch_copy_text(item.owner.c_str(), "");
        ++count;
    }
    return out;
}

void mark_ready(const char* subsystem, const char* label, const char* phase)
{
    aida::qt::UiDispatcher* context = aida::qt::g_ui_dispatch_context.load(std::memory_order_acquire);
    const bool context_on_gui_thread =
        context && context->thread() == QThread::currentThread();
    g_ui_dispatch_window_destroying.store(false, std::memory_order_release);
    g_ui_dispatch_shutdown.store(false, std::memory_order_release);
    g_ui_dispatch_ready.store(owner_tid() != 0 && context_on_gui_thread, std::memory_order_release);
    g_ui_dispatch_wake_pending.store(false, std::memory_order_release);
    diag::log_tagged_critical_fmt("ui_dispatcher",
        "UI-DISPATCHER-DRAIN event=ready subsystem=%s label=%s phase=%s ctx=0x%llX gui_thread=%lu owner_tid=%lu ready=%d depth=%zu",
        ui_dispatch_text(subsystem),
        ui_dispatch_text(label),
        ui_dispatch_text(phase),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(context)),
        static_cast<unsigned long>(::GetCurrentThreadId()),
        static_cast<unsigned long>(owner_tid()),
        g_ui_dispatch_ready.load(std::memory_order_acquire) ? 1 : 0,
        pending_count());
}

void mark_window_destroying(const char* subsystem, const char* label, const char* phase)
{
    g_ui_dispatch_ready.store(false, std::memory_order_release);
    g_ui_dispatch_window_destroying.store(true, std::memory_order_release);
    aida::qt::g_ui_dispatch_context.store(nullptr, std::memory_order_release);
    g_ui_dispatch_wake_pending.store(false, std::memory_order_release);
    std::size_t discarded = 0;
    {
        std::lock_guard<std::mutex> lock(g_ui_dispatch_mtx);
        discarded = g_ui_dispatch_queue.size();
        g_ui_dispatch_queue.clear();
        ui_dispatch_refresh_metrics_locked();
    }
    if (discarded != 0) {
        g_ui_dispatch_discarded.fetch_add(static_cast<std::uint64_t>(discarded), std::memory_order_acq_rel);
        g_ui_dispatch_rejected_shutdown.fetch_add(static_cast<std::uint64_t>(discarded), std::memory_order_acq_rel);
        g_ui_dispatch_rejected.fetch_add(static_cast<std::uint64_t>(discarded), std::memory_order_acq_rel);
    }
    diag::log_tagged_critical_fmt("ui_dispatcher",
        "UI-DISPATCHER-DRAIN event=window_destroying subsystem=%s label=%s phase=%s owner_tid=%lu depth=%zu dropped=%zu oldest_age_ms=%llu",
        ui_dispatch_text(subsystem),
        ui_dispatch_text(label),
        ui_dispatch_text(phase),
        static_cast<unsigned long>(owner_tid()),
        pending_count(),
        discarded,
        static_cast<unsigned long long>(ui_dispatch_oldest_age_ms(ui_dispatch_now_ms())));
    if (discarded != 0) {
        diag::log_tagged_critical_fmt("ui_dispatcher",
            "UI-DISPATCHER-REJECT result=%s reason=window_destroying_drop dropped=%zu owner_tid=%lu rejected_shutdown=%llu",
            result_name(enqueue_result_t::rejected_shutdown),
            discarded,
            static_cast<unsigned long>(owner_tid()),
            static_cast<unsigned long long>(g_ui_dispatch_rejected_shutdown.load(std::memory_order_acquire)));
    }
}

void shutdown()
{
    g_ui_dispatch_shutdown.store(true, std::memory_order_release);
    g_ui_dispatch_ready.store(false, std::memory_order_release);
    g_ui_dispatch_window_destroying.store(true, std::memory_order_release);
    aida::qt::g_ui_dispatch_context.store(nullptr, std::memory_order_release);
    std::size_t discarded = 0;
    {
        std::lock_guard<std::mutex> lock(g_ui_dispatch_mtx);
        discarded = g_ui_dispatch_queue.size();
        g_ui_dispatch_queue.clear();
        ui_dispatch_refresh_metrics_locked();
    }
    if (discarded != 0) {
        g_ui_dispatch_discarded.fetch_add(static_cast<std::uint64_t>(discarded), std::memory_order_acq_rel);
        g_ui_dispatch_rejected_shutdown.fetch_add(static_cast<std::uint64_t>(discarded), std::memory_order_acq_rel);
        g_ui_dispatch_rejected.fetch_add(static_cast<std::uint64_t>(discarded), std::memory_order_acq_rel);
        diag::log_tagged_critical_fmt("ui_dispatcher",
            "UI-DISPATCHER-REJECT result=%s reason=shutdown_drop dropped=%zu owner_tid=%lu accepted=%llu drained=%llu rejected_shutdown=%llu",
            result_name(enqueue_result_t::rejected_shutdown),
            discarded,
            static_cast<unsigned long>(owner_tid()),
            static_cast<unsigned long long>(g_ui_dispatch_enqueued.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(g_ui_dispatch_executed.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(g_ui_dispatch_rejected_shutdown.load(std::memory_order_acquire)));
    }
    g_ui_dispatch_wake_pending.store(false, std::memory_order_release);
}

}

namespace aida::qt {

UiDispatcher::UiDispatcher(QObject* parent)
    : QObject(parent)
{
}

void UiDispatcher::installEventHooks()
{
    QAbstractEventDispatcher* dispatcher = QCoreApplication::eventDispatcher();
    if (!dispatcher) {
        diag::log_tagged_critical("ui_dispatcher", "qt_event_dispatcher_missing about_to_block_hook=0");
        return;
    }
    QObject::connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, [this]() {
        drainAboutToBlockSlot();
    });
    diag::log_tagged_critical_fmt("ui_dispatcher",
        "qt_event_dispatcher_hooked dispatcher=%s about_to_block_hook=1 tid=%lu",
        dispatcher->metaObject()->className(),
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

void UiDispatcher::drainWakeSlot()
{
    ui_thread::g_ui_dispatch_wake_pending.store(false, std::memory_order_release);
    diag::log_tagged_fmt("ui_dispatcher",
        "UI-DISPATCHER-WAKE reason=dequeued tid=%lu depth=%zu wake_pending=0",
        static_cast<unsigned long>(::GetCurrentThreadId()),
        ui_thread::g_ui_dispatch_last_depth.load(std::memory_order_acquire));
    aida::ui_thread::drain(32, 2, "qt_wake");
}

void UiDispatcher::drainAboutToBlockSlot()
{
    aida::ui_thread::drain(64, 4, "about_to_block");
}

UiDispatcher* create_ui_dispatcher(QObject* parent)
{
    UiDispatcher* existing = g_ui_dispatch_context.load(std::memory_order_acquire);
    if (existing)
        return existing;
    UiDispatcher* dispatcher = new UiDispatcher(parent);
    g_ui_dispatch_context.store(dispatcher, std::memory_order_release);
    QObject::connect(dispatcher, &QObject::destroyed, [](QObject*) {
        g_ui_dispatch_context.store(nullptr, std::memory_order_release);
    });
    dispatcher->installEventHooks();
    diag::log_tagged_critical_fmt("ui_dispatcher",
        "qt_dispatch_context_created ctx=0x%llX tid=%lu",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(dispatcher)),
        static_cast<unsigned long>(::GetCurrentThreadId()));
    return dispatcher;
}

UiDispatcher* ui_dispatcher_instance()
{
    return g_ui_dispatch_context.load(std::memory_order_acquire);
}

}
