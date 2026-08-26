#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <Qt>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "core/infra/executor.hpp"
#include "qt/network/shared/network_format.hpp"

namespace aida::qt::net {

// SnapshotRequester reproduces the request_*_runtime_snapshot contract from the
// ImGui network backend: a minimum-interval throttle, a CAS single-flight
// guard, a monotonically increasing request serial, and immutable
// shared_ptr<const T> publication delivered onto the GUI thread through a
// queued QMetaObject::invokeMethod against a long-lived GUI-affinity context.
// Queued delivery deep-copies captures and drops silently when the receiver is
// destroyed (qobject.cpp:201-202 in qtbase 6.8.3); the functor overload keeps
// metatype registration compile-time (qmetaobject.cpp:1735-1754). Never fire
// BlockingQueued from the GUI thread (qobject.cpp:4084-4090).
template <typename SnapshotT>
class SnapshotRequester {
public:
    using snapshot_ptr_t = std::shared_ptr<const SnapshotT>;
    using produce_fn_t = std::function<snapshot_ptr_t()>;
    using deliver_fn_t = std::function<void(snapshot_ptr_t, std::uint64_t serial)>;

    SnapshotRequester() = default;

    void configure(std::uint64_t min_interval_ms, produce_fn_t produce) {
        min_interval_ms_ = min_interval_ms;
        produce_ = std::move(produce);
    }

    void setDelivery(QObject* context, deliver_fn_t deliver) {
        context_ = context;
        deliver_ = std::move(deliver);
    }

    bool request(bool force = false) {
        if (!produce_ || !context_ || !deliver_)
            return false;
        const std::uint64_t now = network_now_ms();
        const std::uint64_t last = requested_ms_.load(std::memory_order_acquire);
        if (!force && last != 0 && now >= last && now - last < min_interval_ms_)
            return true;
        bool expected = false;
        if (!pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return true;
        requested_ms_.store(now, std::memory_order_release);
        const std::uint64_t serial = serial_.fetch_add(1, std::memory_order_acq_rel) + 1;

        aida::infra::executor::submission_t submission;
        submission.owner_subsystem = "network.qt";
        submission.label = label_ ? label_ : "snapshot.request";
        submission.thread_class = "bounded_task";
        submission.domain = aida::infra::executor::domain_t::diagnostics;
        submission.priority = 3;
        QPointer<QObject> context = context_;
        deliver_fn_t deliver = deliver_;
        produce_fn_t produce = produce_;
        submission.body = [this, serial, context, deliver = std::move(deliver),
                           produce = std::move(produce)]() mutable {
            snapshot_ptr_t snapshot;
            try {
                snapshot = produce();
            } catch (...) {
                snapshot.reset();
            }
            pending_.store(false, std::memory_order_release);
            if (!snapshot || !context)
                return;
            if (serial_.load(std::memory_order_acquire) != serial)
                return;
            auto* raw_context = context.data();
            QMetaObject::invokeMethod(raw_context,
                [deliver = std::move(deliver), snapshot = std::move(snapshot), serial]() mutable {
                    deliver(std::move(snapshot), serial);
                }, Qt::QueuedConnection);
        };
        if (!aida::infra::executor::submit(std::move(submission)).submitted) {
            pending_.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }

    void force() { static_cast<void>(request(true)); }

    bool pending() const noexcept { return pending_.load(std::memory_order_acquire); }
    std::uint64_t serial() const noexcept { return serial_.load(std::memory_order_acquire); }

private:
    std::uint64_t min_interval_ms_ = 250;
    produce_fn_t produce_;
    QPointer<QObject> context_;
    deliver_fn_t deliver_;
    std::atomic<bool> pending_{false};
    std::atomic<std::uint64_t> requested_ms_{0};
    std::atomic<std::uint64_t> serial_{0};
public:
    const char* label_ = nullptr;
};

}
