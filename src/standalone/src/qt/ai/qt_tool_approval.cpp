#include "qt/ai/qt_tool_approval.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "core/ai/standalone_chat.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::ai {

AidaToolApprovalDialog::AidaToolApprovalDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("aida.ai.tool_approval"));
    setProperty("aidaRole", QStringLiteral("dialog"));
    setProperty("aidaSeverity", QStringLiteral("high"));
    setWindowTitle(QStringLiteral("Tool Approval Required"));
    setWindowModality(Qt::ApplicationModal);
    setMinimumSize(420, 280);
    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(t.panel.padding, t.panel.padding,
                               t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);
    auto* title = new QLabel(QStringLiteral("Tool Approval Required"), this);
    title->setFont(theme::fonts::h2());
    layout->addWidget(title);
    auto* intro = new QLabel(QStringLiteral("The AI wants to execute:"), this);
    intro->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(intro);
    tool_label_ = new QLabel(this);
    tool_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    tool_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(tool_label_);
    auto* args_label = new QLabel(QStringLiteral("Arguments:"), this);
    args_label->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(args_label);
    args_ = new QPlainTextEdit(this);
    args_->setObjectName(QStringLiteral("aida.ai.tool_approval.args"));
    args_->setReadOnly(true);
    args_->setFont(theme::fonts::codeRegular());
    args_->setMaximumBlockCount(1024);
    layout->addWidget(args_, 1);
    auto* buttons = new QDialogButtonBox(this);
    auto* allow = buttons->addButton(QStringLiteral("Allow"), QDialogButtonBox::AcceptRole);
    allow->setDefault(true);
    buttons->addButton(QStringLiteral("Deny"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        (void)aida::automation_ui::respond_to_tool_approval(identity_, true);
        identity_ = 0;
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        (void)aida::automation_ui::respond_to_tool_approval(identity_, false);
        identity_ = 0;
        QDialog::reject();
    });
}

void AidaToolApprovalDialog::present(std::uint64_t identity, const QString& tool_name,
                                      const QString& args_preview) {
    identity_ = identity;
    tool_label_->setText(tool_name);
    args_->setPlainText(args_preview);
    if (!isVisible())
        open();
    raise();
    activateWindow();
}

void AidaToolApprovalDialog::reject() {
    if (identity_ != 0)
        (void)aida::automation_ui::respond_to_tool_approval(identity_, false);
    identity_ = 0;
    QDialog::reject();
}

AidaToolApprovalController::AidaToolApprovalController(QObject* parent) : QObject(parent) {
    watch_timer_ = new QTimer(this);
    watch_timer_->setInterval(250);
    connect(watch_timer_, &QTimer::timeout, this, &AidaToolApprovalController::onWatchTick);
}

void AidaToolApprovalController::installBackendHook(QWidget* dialog_parent) {
    dialog_parent_ = dialog_parent;
    aida::automation_ui::set_tool_approval_notify_hook([this] {
        schedulePendingCheck();
    });
}

void AidaToolApprovalController::schedulePendingCheck() {
    if (queued_.exchange(true, std::memory_order_acq_rel))
        return;
    QMetaObject::invokeMethod(this, [this] {
        queued_.store(false, std::memory_order_release);
        onPending();
    }, Qt::QueuedConnection);
}

void AidaToolApprovalController::onPending() {
    const auto snapshot = aida::automation_ui::tool_approval_snapshot();
    if (!snapshot.pending) {
        if (dialog_)
            dialog_->reject();
        return;
    }
    if (!dialog_) {
        dialog_ = new AidaToolApprovalDialog(dialog_parent_);
        connect(dialog_, &QDialog::finished, this, [this](int) {
            dialog_->deleteLater();
            dialog_ = nullptr;
            watch_timer_->stop();
        });
    }
    dialog_->present(snapshot.identity, QString::fromStdString(snapshot.tool_name),
                     QString::fromStdString(snapshot.arguments_preview));
    if (!watch_timer_->isActive())
        watch_timer_->start();
}

void AidaToolApprovalController::onWatchTick() {
    if (!dialog_)
        return;
    const auto snapshot = aida::automation_ui::tool_approval_snapshot();
    if (!snapshot.pending) {
        dialog_->reject();
        return;
    }
    if (snapshot.identity != dialog_->identity())
        dialog_->present(snapshot.identity, QString::fromStdString(snapshot.tool_name),
                         QString::fromStdString(snapshot.arguments_preview));
}

}
