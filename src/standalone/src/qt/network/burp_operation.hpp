#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "core/network/burp/burp_ui_operation.hpp"

namespace aida::qt::net {

// QObject port of aida::burp::ui_operation::state_t
// (core/network/burp/burp_ui_operation.hpp:45-157), semantics 1:1. The only
// transport change is step 8 of plan 11 section 3.1: after the completion is
// stored and pending_ released, the runner posts a queued functor to itself
// (QMetaObject::invokeMethod context overload, qobjectdefs.h:418-429) so the
// completion signal is emitted on the GUI thread; queued delivery deep-copies
// the captured arguments (qmetaobject.cpp:1642-1657) and is dropped silently
// when the runner is destroyed (qobject.cpp:201-202). This replaces every
// pane's render-loop drain of observed_operation_generation. The identity
// revalidation lambdas stay inside the caller-supplied execute bodies,
// unchanged from the legacy view TUs.
struct BurpRequest {
    QString owner;
    QString ownerView;
    QString ownerAction;
    QString label;
    QString target;
    QString affectedEntity;
    aida::infra::executor::domain_t domain = aida::infra::executor::domain_t::external_tool;
    int priority = 3;
    std::function<aida::burp::ui_operation::result_t()> execute;
};

class BurpOperationRunner : public QObject {
    Q_OBJECT
public:
    explicit BurpOperationRunner(const QString& taskSource, QObject* parent = nullptr);

    bool submit(BurpRequest req);
    bool retry();
    bool pending() const noexcept { return pending_.load(std::memory_order_acquire); }
    std::uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    std::shared_ptr<const aida::burp::ui_operation::completion_t> completion() const;

Q_SIGNALS:
    void submitted(quint64 generation);
    void completed(quint64 generation, bool success, bool partial, QString message);

private:
    const QString task_source_;
    std::atomic<bool> pending_{false};
    std::atomic<std::uint64_t> generation_{0};
    mutable std::mutex mutex_;
    std::shared_ptr<const aida::burp::ui_operation::completion_t> completion_;
    std::optional<BurpRequest> last_request_;
};

}
