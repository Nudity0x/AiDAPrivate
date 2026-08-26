#include "qt/debugger/debugger_session_controller.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include <functional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_definition_store.hpp"
#include "core/debugger/debugger_engine.hpp"
#include "core/debugger/debugger_interaction_context.hpp"
#include "core/infra/event_bus.hpp"
#include "core/runtime/standalone_driver.hpp"

#include "qt/qt_eventbus_bridge.hpp"

namespace aida::qt::debugger {

namespace {
constexpr int k_sync_tick_ms = 250;
constexpr int k_watchdog_tick_ms = 1000;
constexpr quint64 k_watchdog_degraded_ms = 3ULL * 4000ULL;
}

DebuggerSessionController& DebuggerSessionController::instance() {
    static QPointer<DebuggerSessionController> instance;
    if (!instance) {
        instance = new DebuggerSessionController();
    }
    return *instance;
}

DebuggerSessionController::DebuggerSessionController(QObject* parent)
    : QObject(parent) {
}

void DebuggerSessionController::install() {
    if (installed_)
        return;
    if (thread() != QThread::currentThread()) {
        diag::log_tagged_critical("qt_debugger",
            "session_controller_install_rejected reason=wrong_thread");
        return;
    }
    installed_ = true;

    bus_ = new AidaEventBusBridge(this);
    installSubscriptions();

    sync_timer_ = new QTimer(this);
    sync_timer_->setInterval(k_sync_tick_ms);
    sync_timer_->setTimerType(Qt::CoarseTimer);
    connect(sync_timer_, &QTimer::timeout, this,
        &DebuggerSessionController::onSyncTick);
    sync_timer_->start();

    watchdog_timer_ = new QTimer(this);
    watchdog_timer_->setInterval(k_watchdog_tick_ms);
    watchdog_timer_->setTimerType(Qt::CoarseTimer);
    connect(watchdog_timer_, &QTimer::timeout, this,
        &DebuggerSessionController::onWatchdogTick);
    watchdog_timer_->start();

    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
        &DebuggerSessionController::stopTimers);

    diag::log_tagged_fmt("qt_debugger",
        "session_controller_installed gui_tid=%lu sync_ms=%d watchdog_ms=%d",
        static_cast<unsigned long>(::GetCurrentThreadId()), k_sync_tick_ms,
        k_watchdog_tick_ms);
}

void DebuggerSessionController::stopTimers() {
    if (sync_timer_)
        sync_timer_->stop();
    if (watchdog_timer_)
        watchdog_timer_->stop();
    diag::log_tagged("qt_debugger", "session_controller_timers_stopped");
}

void DebuggerSessionController::installSubscriptions() {
    bus_->subscribe_gui(aida::events::event_debug_events_drained,
        std::function<void(const aida::events::debug_events_drained_t&)>(
            [this](const aida::events::debug_events_drained_t& payload) {
                Q_EMIT debugEventsDrained(payload.returned_count,
                    payload.dropped_since_last_drain);
            }));
    bus_->subscribe_gui(aida::events::event_dll_loaded,
        std::function<void(const aida::events::dll_loaded_t&)>(
            [this](const aida::events::dll_loaded_t& payload) {
                const quint32 attached = driver_bridge::attached_pid();
                if (attached == 0 || attached != payload.process_id)
                    return;
                Q_EMIT modulesRefreshRequested();
            }));
    bus_->subscribe_gui(aida::events::event_process_created,
        std::function<void(const aida::events::process_created_t&)>(
            [this](const aida::events::process_created_t&) {
                Q_EMIT modulesRefreshRequested();
            }));
    bus_->subscribe_gui(aida::events::event_process_exited,
        std::function<void(const aida::events::process_exited_t&)>(
            [this](const aida::events::process_exited_t& payload) {
                Q_EMIT processExited(payload.process_id);
            }));
}

void DebuggerSessionController::onSyncTick() {
    const auto status = debugger_engine::g_state.status.load(
        std::memory_order_acquire);
    debugger_interaction::synchronize_target(driver_bridge::attached_pid(),
        status != debugger_engine::dbg_status_t::running);
    debugger_definition_store::synchronize(++definition_sync_tick_);
    const quint64 generation = debugger_interaction::current_stop_generation();
    const quint32 pid = driver_bridge::attached_pid();
    const int status_value = static_cast<int>(status);
    if (status_value != last_status_ || pid != last_pid_ ||
        generation != last_generation_) {
        last_status_ = status_value;
        last_pid_ = pid;
        last_generation_ = generation;
        Q_EMIT sessionStateChanged(status_value, pid, generation);
    }
    Q_EMIT sessionTick();
}

void DebuggerSessionController::onWatchdogTick() {
    const quint64 age_ms = driver_bridge::driver_watchdog_age_ms();
    Q_EMIT watchdogSampled(age_ms, age_ms > k_watchdog_degraded_ms);
}

int DebuggerSessionController::status() const noexcept {
    return static_cast<int>(debugger_engine::g_state.status.load(
        std::memory_order_acquire));
}

quint32 DebuggerSessionController::targetPid() const noexcept {
    return driver_bridge::attached_pid();
}

quint64 DebuggerSessionController::stopGeneration() const noexcept {
    return debugger_interaction::current_stop_generation();
}

bool DebuggerSessionController::pausedOrStepping() const noexcept {
    const auto status = debugger_engine::g_state.status.load(
        std::memory_order_acquire);
    return status == debugger_engine::dbg_status_t::paused ||
        status == debugger_engine::dbg_status_t::stepping;
}

bool DebuggerSessionController::running() const noexcept {
    return debugger_engine::g_state.status.load(std::memory_order_acquire) ==
        debugger_engine::dbg_status_t::running;
}

}
