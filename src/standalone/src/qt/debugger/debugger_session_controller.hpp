#pragma once

#include <QObject>
#include <QPointer>

#include <atomic>
#include <cstdint>

class QTimer;

namespace aida::qt {
class AidaEventBusBridge;
}

namespace aida::qt::debugger {

// DebuggerSessionController: the GUI-thread engine<->Qt bridge. Replaces the
// ImGui per-frame pulls: debug events arrive from the driver-bridge
// event_poller via the event bus (synchronous on the poller thread); the
// AidaEventBusBridge performs the queued hand-off (QMetaObject::invokeMethod,
// Qt::QueuedConnection, deep-copied payload). This controller's slots then do
// only GUI-safe work: atomic reads, mutex-guarded cache copies, timer drives.
// It never calls driver_bridge blocking IOCTLs and never runs a
// WaitForDebugEvent loop.
class DebuggerSessionController : public QObject {
    Q_OBJECT
public:
    static DebuggerSessionController& instance();

    // GUI thread only. Idempotent. Called by install_debugger_domain after the
    // dock host exists.
    void install();
    bool installed() const noexcept { return installed_; }

    // GUI-safe atomic reads (callable on the GUI thread; they never block).
    int status() const noexcept;
    quint32 targetPid() const noexcept;
    quint64 stopGeneration() const noexcept;
    bool pausedOrStepping() const noexcept;
    bool running() const noexcept;

Q_SIGNALS:
    // status/pid/stopGeneration changed as observed at the 250ms sync tick.
    void sessionStateChanged(int status, quint32 pid, quint64 stopGeneration);
    // Every 250ms synchronize tick (aligned to the driver event poller).
    void sessionTick();
    // 1s watchdog sample: driver watchdog age + degradation flag (>3x4000ms).
    void watchdogSampled(quint64 ageMs, bool degraded);
    // event_dll_loaded / event_process_created for the attached pid.
    void modulesRefreshRequested();
    void processExited(quint32 pid);
    void debugEventsDrained(quint32 returned, quint32 droppedSinceLast);

private Q_SLOTS:
    void onSyncTick();
    void onWatchdogTick();

private:
    explicit DebuggerSessionController(QObject* parent = nullptr);
    void installSubscriptions();
    void stopTimers();

    bool installed_ = false;
    AidaEventBusBridge* bus_ = nullptr;
    QTimer* sync_timer_ = nullptr;
    QTimer* watchdog_timer_ = nullptr;
    int definition_sync_tick_ = 0;
    int last_status_ = -1;
    quint32 last_pid_ = 0;
    quint64 last_generation_ = 0;
};

}
