#include "qt/qt_eventloop_monitor.hpp"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include "helpers/diag_log.hpp"
#include "core/diagnostics/observer.hpp"
#include "core/ui/ui_thread_dispatcher.hpp"

namespace aida::qt {

namespace {

constexpr std::uint64_t kHeartbeatIntervalMs = 250;
constexpr std::uint64_t kTransitionLogIntervalMs = 1000;
constexpr std::uint64_t kHeartbeatLogIntervalMs = 30000;

std::uint64_t monitor_now_ms()
{
    return static_cast<std::uint64_t>(::GetTickCount64());
}

}

AidaEventLoopMonitor::AidaEventLoopMonitor(QObject* parent)
    : QObject(parent)
{
}

void AidaEventLoopMonitor::setTracerHooks(tracer_hooks_t hooks)
{
    tracer_hooks_ = std::move(hooks);
    diag::log_tagged_critical_fmt("qt_eventloop",
        "tracer_hooks_set pulse=%d phase=%d tid=%lu",
        tracer_hooks_.render_pulse ? 1 : 0,
        tracer_hooks_.mark_render_phase ? 1 : 0,
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

void AidaEventLoopMonitor::start()
{
    if (started_) {
        diag::log_tagged_critical("qt_eventloop", "monitor_start_skipped already=1");
        return;
    }
    QAbstractEventDispatcher* dispatcher = QCoreApplication::eventDispatcher();
    if (!dispatcher) {
        diag::log_tagged_critical("qt_eventloop", "monitor_start_failed reason=no_event_dispatcher");
        return;
    }
    heartbeat_timer_ = new QTimer(this);
    heartbeat_timer_->setInterval(static_cast<int>(kHeartbeatIntervalMs));
    connect(heartbeat_timer_, &QTimer::timeout, this, &AidaEventLoopMonitor::onHeartbeat);
    heartbeat_timer_->start();
    connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, [this]() {
        onAboutToBlock();
    });
    connect(dispatcher, &QAbstractEventDispatcher::awake, this, [this]() {
        onAwake();
    });
    about_to_block_hooked_ = true;
    started_ = true;
    last_heartbeat_log_ms_ = monitor_now_ms();
    diag::log_tagged_critical_fmt("qt_eventloop",
        "monitor_started dispatcher=%s heartbeat_ms=%llu tid=%lu",
        dispatcher->metaObject()->className(),
        static_cast<unsigned long long>(kHeartbeatIntervalMs),
        static_cast<unsigned long>(::GetCurrentThreadId()));
    maybeLogInvariants();
}

void AidaEventLoopMonitor::stop()
{
    if (!started_)
        return;
    if (heartbeat_timer_) {
        heartbeat_timer_->stop();
        heartbeat_timer_->deleteLater();
        heartbeat_timer_ = nullptr;
    }
    started_ = false;
    diag::log_tagged_critical_fmt("qt_eventloop",
        "monitor_stopped heartbeats=%llu blocks=%llu awakes=%llu tid=%lu",
        static_cast<unsigned long long>(heartbeat_frame_),
        static_cast<unsigned long long>(block_count_),
        static_cast<unsigned long long>(awake_count_),
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

bool AidaEventLoopMonitor::isRunning() const
{
    return started_;
}

void AidaEventLoopMonitor::setObservedWindow(HWND hwnd)
{
    if (!hwnd) {
        diag::log_tagged_critical("qt_eventloop", "observer_repoint_failed reason=null_hwnd");
        return;
    }
    if (observed_hwnd_ == hwnd) {
        diag::log_tagged_fmt("qt_eventloop", "observer_repoint_skipped same_hwnd=0x%llX",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(hwnd)));
        maybeLogInvariants();
        return;
    }
    if (aida::diagnostics::observer::is_running()) {
        aida::diagnostics::observer::stop();
        int waited = 0;
        while (aida::diagnostics::observer::is_running() && waited < 30) {
            ::Sleep(100);
            ++waited;
        }
        diag::log_tagged_critical_fmt("qt_eventloop",
            "observer_repoint_stop_waited slices=%d running=%d",
            waited,
            aida::diagnostics::observer::is_running() ? 1 : 0);
    }
    aida::diagnostics::observer::observer_config_t obs_cfg;
    obs_cfg.enabled = true;
    obs_cfg.poll_interval_ms = 5000;
    obs_cfg.hung_threshold_ms = 5000;
    obs_cfg.max_lifetime_ms = 600000;
    obs_cfg.wm_null_timeout_ms = 200;
    const bool started = aida::diagnostics::observer::start(::GetCurrentProcessId(), hwnd, obs_cfg);
    observed_hwnd_ = hwnd;
    DWORD_PTR probe_result = 0;
    ::SetLastError(0);
    observer_probe_ok_ = ::SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_NORMAL, 200, &probe_result) != 0;
    observer_probe_gle_ = observer_probe_ok_ ? 0UL : ::GetLastError();
    diag::log_tagged_critical_fmt("qt_eventloop",
        "observer_repointed hwnd=0x%llX started=%d probe_ok=%d probe_gle=%lu tid=%lu",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(hwnd)),
        started ? 1 : 0,
        observer_probe_ok_ ? 1 : 0,
        observer_probe_gle_,
        static_cast<unsigned long>(::GetCurrentThreadId()));
    maybeLogInvariants();
}

void AidaEventLoopMonitor::maybeLogInvariants()
{
    if (invariants_logged_ || !started_ || !observed_hwnd_)
        return;
    invariants_logged_ = true;
    QAbstractEventDispatcher* dispatcher = QCoreApplication::eventDispatcher();
    diag::log_tagged_critical_fmt("qt_eventloop",
        "qt_eventloop_invariants dispatcher=%s about_to_block_hook=%d ui_dispatcher_ready=%d ui_owner_tid=%lu ui_wake_pending=%d observer_hwnd=0x%llX observer_running=%d observer_probe_ok=%d observer_probe_gle=%lu tid=%lu",
        dispatcher ? dispatcher->metaObject()->className() : "<none>",
        about_to_block_hooked_ ? 1 : 0,
        aida::ui_thread::is_owner_thread() ? 1 : 0,
        aida::ui_thread::owner_tid(),
        aida::ui_thread::wake_pending() ? 1 : 0,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(observed_hwnd_)),
        aida::diagnostics::observer::is_running() ? 1 : 0,
        observer_probe_ok_ ? 1 : 0,
        observer_probe_gle_,
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

void AidaEventLoopMonitor::onHeartbeat()
{
    ++heartbeat_frame_;
    if (tracer_hooks_.render_pulse)
        tracer_hooks_.render_pulse(heartbeat_frame_);
    if (tracer_hooks_.mark_render_phase)
        tracer_hooks_.mark_render_phase("qt_eventloop_heartbeat");
    const std::uint64_t now = monitor_now_ms();
    if (now - last_heartbeat_log_ms_ >= kHeartbeatLogIntervalMs) {
        last_heartbeat_log_ms_ = now;
        diag::log_tagged_fmt("qt_eventloop",
            "qt_eventloop_heartbeat frame=%llu blocks=%llu awakes=%llu ui_pending=%zu tid=%lu",
            static_cast<unsigned long long>(heartbeat_frame_),
            static_cast<unsigned long long>(block_count_),
            static_cast<unsigned long long>(awake_count_),
            aida::ui_thread::pending_count(),
            static_cast<unsigned long>(::GetCurrentThreadId()));
    }
}

void AidaEventLoopMonitor::onAboutToBlock()
{
    ++block_count_;
    last_block_ts_ms_ = monitor_now_ms();
    if (tracer_hooks_.mark_render_phase)
        tracer_hooks_.mark_render_phase("about_to_block");
    const std::uint64_t now = last_block_ts_ms_;
    if (now - last_block_log_ms_ < kTransitionLogIntervalMs) {
        ++block_log_suppressed_;
        return;
    }
    last_block_log_ms_ = now;
    const std::uint64_t suppressed = block_log_suppressed_;
    block_log_suppressed_ = 0;
    diag::log_tagged_fmt("qt_eventloop",
        "eventloop_about_to_block ts_ms=%llu blocks=%llu suppressed=%llu ui_pending=%zu",
        static_cast<unsigned long long>(now),
        static_cast<unsigned long long>(block_count_),
        static_cast<unsigned long long>(suppressed),
        aida::ui_thread::pending_count());
}

void AidaEventLoopMonitor::onAwake()
{
    ++awake_count_;
    last_awake_ts_ms_ = monitor_now_ms();
    if (tracer_hooks_.mark_render_phase)
        tracer_hooks_.mark_render_phase("awake");
    const std::uint64_t now = last_awake_ts_ms_;
    if (now - last_awake_log_ms_ < kTransitionLogIntervalMs) {
        ++awake_log_suppressed_;
        return;
    }
    last_awake_log_ms_ = now;
    const std::uint64_t suppressed = awake_log_suppressed_;
    awake_log_suppressed_ = 0;
    diag::log_tagged_fmt("qt_eventloop",
        "eventloop_awake ts_ms=%llu awakes=%llu suppressed=%llu blocked_ms=%llu",
        static_cast<unsigned long long>(now),
        static_cast<unsigned long long>(awake_count_),
        static_cast<unsigned long long>(suppressed),
        static_cast<unsigned long long>(last_block_ts_ms_ != 0 && now >= last_block_ts_ms_ ? now - last_block_ts_ms_ : 0));
}

}
