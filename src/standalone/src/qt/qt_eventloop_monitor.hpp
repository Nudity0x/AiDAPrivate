#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QObject>

#include <cstdint>
#include <functional>

class QTimer;

namespace aida::qt {

class AidaEventLoopMonitor : public QObject
{
    Q_OBJECT
public:
    struct tracer_hooks_t {
        std::function<void(std::uint64_t frame)> render_pulse;
        std::function<void(const char* name)> mark_render_phase;
    };

    explicit AidaEventLoopMonitor(QObject* parent = nullptr);

    void start();
    void stop();

    void setTracerHooks(tracer_hooks_t hooks);
    void setObservedWindow(HWND hwnd);

    bool isRunning() const;

private:
    void onHeartbeat();
    void onAboutToBlock();
    void onAwake();
    void maybeLogInvariants();

    tracer_hooks_t tracer_hooks_;
    QTimer* heartbeat_timer_ = nullptr;
    std::uint64_t heartbeat_frame_ = 0;
    std::uint64_t block_count_ = 0;
    std::uint64_t awake_count_ = 0;
    std::uint64_t last_block_ts_ms_ = 0;
    std::uint64_t last_awake_ts_ms_ = 0;
    std::uint64_t last_block_log_ms_ = 0;
    std::uint64_t last_awake_log_ms_ = 0;
    std::uint64_t block_log_suppressed_ = 0;
    std::uint64_t awake_log_suppressed_ = 0;
    std::uint64_t last_heartbeat_log_ms_ = 0;
    HWND observed_hwnd_ = nullptr;
    bool started_ = false;
    bool about_to_block_hooked_ = false;
    bool invariants_logged_ = false;
    bool observer_probe_ok_ = false;
    unsigned long observer_probe_gle_ = 0;
};

}
