#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>

#include <functional>

class QSessionManager;

namespace aida::qt {

class AidaSessionBridge : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    explicit AidaSessionBridge(QObject* parent = nullptr);

    void install();

    void setExitReviewGateHook(std::function<bool()> hook);
    void setSessionAbortHook(std::function<void()> hook);

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void onCommitDataRequest(QSessionManager& session);

    std::function<bool()> exit_review_gate_hook_;
    std::function<void()> session_abort_hook_;
    bool installed_ = false;
    bool abort_signaled_this_cycle_ = false;
};

}
