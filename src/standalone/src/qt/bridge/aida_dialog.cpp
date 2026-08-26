#include "qt/bridge/aida_dialog.hpp"

#include <QTimer>

#include <utility>

#include "helpers/diag_log.hpp"

namespace aida::qt::bridge {

namespace {
constexpr int k_revalidate_interval_ms = 250;
}

AidaDialog::notification_hook_t AidaDialog::notification_hook_ = nullptr;

AidaDialog::RevalidateScope::RevalidateScope(AidaDialog& dialog, hooks_t hooks,
                                             QString stale_message)
    : dialog_(&dialog), hooks_(std::move(hooks)),
      stale_message_(std::move(stale_message)) {
    if (hooks_.identity_fn) {
        captured_identity_ = hooks_.identity_fn();
        has_identity_ = true;
    }
    if (hooks_.generation_fn) {
        captured_generation_ = hooks_.generation_fn();
        has_generation_ = true;
    }
    if (has_identity_ || has_generation_) {
        timer_ = new QTimer(&dialog);
        timer_->setInterval(k_revalidate_interval_ms);
        timer_->setSingleShot(false);
        QObject::connect(timer_, &QTimer::timeout, dialog_, [this] {
            if (detached_ || !dialog_ || !stale())
                return;
            if (timer_)
                timer_->stop();
            const QString message = stale_message_;
            dialog_->notify_error(message);
            dialog_->reject();
        });
        timer_->start();
    }
}

AidaDialog::RevalidateScope::~RevalidateScope() {
    detached_ = true;
    if (timer_)
        timer_->stop();
}

bool AidaDialog::RevalidateScope::stale() const {
    if (has_identity_ && hooks_.identity_fn &&
        hooks_.identity_fn() != captured_identity_)
        return true;
    if (has_generation_ && hooks_.generation_fn &&
        hooks_.generation_fn() != captured_generation_)
        return true;
    return false;
}

bool AidaDialog::RevalidateScope::valid() const {
    return !stale();
}

bool AidaDialog::RevalidateScope::check_now() {
    if (!stale())
        return true;
    if (timer_)
        timer_->stop();
    if (!detached_ && dialog_) {
        const QString message = stale_message_;
        dialog_->notify_error(message);
        dialog_->reject();
    }
    return false;
}

AidaDialog::AidaDialog(QWidget* parent, Qt::WindowFlags flags)
    : QDialog(parent, flags) {
    setProperty("aidaRole", QStringLiteral("dialog"));
}

AidaDialog::~AidaDialog() = default;

AidaDialog::RevalidateScope& AidaDialog::add_revalidate_scope(
    RevalidateScope::hooks_t hooks, QString stale_message) {
    scopes_.push_back(std::make_unique<RevalidateScope>(*this, std::move(hooks),
                                                        std::move(stale_message)));
    return *scopes_.back();
}

void AidaDialog::accept() {
    if (!revalidate_all())
        return;
    QDialog::accept();
}

void AidaDialog::set_notification_hook(notification_hook_t hook) {
    notification_hook_ = std::move(hook);
}

void AidaDialog::notify_error(const QString& message) {
    diag::log_tagged_fmt("qt_dialog", "dialog_revalidate_stale dialog=%s message=%s",
        objectName().toUtf8().constData(), message.toUtf8().constData());
    Q_EMIT validationStale(message);
    if (notification_hook_)
        notification_hook_(message);
}

bool AidaDialog::revalidate_all() {
    for (const auto& scope : scopes_) {
        if (!scope->valid()) {
            scope->check_now();
            return false;
        }
    }
    return true;
}

}
