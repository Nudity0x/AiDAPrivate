#pragma once

#include <QDialog>
#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>

class QLabel;
class QPlainTextEdit;
class QTimer;

namespace aida::qt::ai {

class AidaToolApprovalDialog : public QDialog {
    Q_OBJECT
public:
    explicit AidaToolApprovalDialog(QWidget* parent = nullptr);

    void present(std::uint64_t identity, const QString& tool_name,
                 const QString& args_preview);
    std::uint64_t identity() const noexcept { return identity_; }

    void reject() override;

private:
    std::uint64_t identity_ = 0;
    QLabel* tool_label_ = nullptr;
    QPlainTextEdit* args_ = nullptr;
};

class AidaToolApprovalController : public QObject {
    Q_OBJECT
public:
    explicit AidaToolApprovalController(QObject* parent = nullptr);

    void installBackendHook(QWidget* dialog_parent);

private:
    void schedulePendingCheck();
    void onPending();
    void onWatchTick();

    QWidget* dialog_parent_ = nullptr;
    QPointer<AidaToolApprovalDialog> dialog_;
    QTimer* watch_timer_ = nullptr;
    std::atomic<bool> queued_{false};
};

}
