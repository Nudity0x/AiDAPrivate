#include "qt/network/burp_review_dialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "qt/network/burp_operation.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::net {

BurpReviewDialog::BurpReviewDialog(const QString& title, const QStringList& bodyLines,
                                   const QString& confirmLabel, bool destructive,
                                   QWidget* parent)
    : aida::qt::bridge::AidaDialog(parent) {
    setWindowTitle(title);
    setModal(true);
    setAttribute(Qt::WA_DeleteOnClose);
    buildUi(bodyLines, confirmLabel, destructive);

    revalidate_timer_ = new QTimer(this);
    revalidate_timer_->setInterval(250);
    connect(revalidate_timer_, &QTimer::timeout, this, [this] { reevaluate(); });
    revalidate_timer_->start();
}

void BurpReviewDialog::buildUi(const QStringList& bodyLines, const QString& confirmLabel,
                               bool destructive) {
    auto* layout = new QVBoxLayout(this);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);
    setMinimumWidth(dialog_min_width_chars(this, 48));

    for (const QString& line : bodyLines) {
        auto* label = new QLabel(line, this);
        label->setWordWrap(true);
        layout->addWidget(label);
    }

    stale_label_ = new QLabel(this);
    stale_label_->setWordWrap(true);
    stale_label_->setProperty("aidaTone", QStringLiteral("warning"));
    stale_label_->setVisible(false);
    layout->addWidget(stale_label_);

    auto* box = new QDialogButtonBox(this);
    confirm_button_ = box->addButton(confirmLabel,
        destructive ? QDialogButtonBox::DestructiveRole : QDialogButtonBox::AcceptRole);
    box->addButton(QDialogButtonBox::Cancel)->setText("Cancel");
    connect(box, &QDialogButtonBox::accepted, this, [this] { accept(); });
    connect(box, &QDialogButtonBox::rejected, this, [this] { reject(); });
    layout->addWidget(box);
    reevaluate();
}

void BurpReviewDialog::setRevalidator(revalidator_t fn) {
    revalidator_ = std::move(fn);
    reevaluate();
}

void BurpReviewDialog::setRunner(BurpOperationRunner* runner) {
    if (runner_)
        runner_->disconnect(this);
    runner_ = runner;
    if (runner_) {
        connect(runner_, &BurpOperationRunner::submitted, this, [this] { reevaluate(); });
        connect(runner_, &BurpOperationRunner::completed, this, [this] { reevaluate(); });
    }
    reevaluate();
}

void BurpReviewDialog::setSubmitCallback(std::function<void()> fn) {
    submit_ = std::move(fn);
}

void BurpReviewDialog::reevaluate() {
    QString reason;
    const bool valid = !revalidator_ || revalidator_(reason);
    stale_reason_ = reason;
    const bool runner_free = !runner_ || !runner_->pending();
    if (confirm_button_)
        confirm_button_->setEnabled(valid && runner_free);
    if (stale_label_) {
        stale_label_->setText(reason);
        stale_label_->setVisible(!valid && !reason.isEmpty());
    }
}

void BurpReviewDialog::accept() {
    if (revalidator_) {
        QString reason;
        if (!revalidator_(reason)) {
            stale_reason_ = reason;
            reevaluate();
            return;
        }
    }
    if (runner_ && runner_->pending()) {
        reevaluate();
        return;
    }
    auto submit = submit_;
    if (submit)
        submit();
    QDialog::accept();
}

}
