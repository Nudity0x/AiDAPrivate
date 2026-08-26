#include "qt/network/shared/exchange_review_dialog.hpp"

#include <QDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "qt/network/shared/style_helpers.hpp"
#include "qt/widgets/aida_button.hpp"

namespace aida::qt::net {

namespace {

bool request_identity_kind(network_view::artifact_kind_t kind) {
    return kind == network_view::artifact_kind_t::request ||
        kind == network_view::artifact_kind_t::intercept_request ||
        kind == network_view::artifact_kind_t::exchange ||
        kind == network_view::artifact_kind_t::repeater_request ||
        kind == network_view::artifact_kind_t::sitemap_request ||
        kind == network_view::artifact_kind_t::api_request ||
        kind == network_view::artifact_kind_t::http2_request ||
        kind == network_view::artifact_kind_t::scanner_request;
}

bool blank_text(const QString& value) {
    return std::all_of(value.begin(), value.end(), [](const QChar character) {
        return std::isspace(character.toLatin1()) != 0;
    });
}

QString identity_label(const network_view::artifact_identity_t& identity) {
    return identity.label.empty()
        ? QString::fromStdString(identity.id)
        : QString::fromStdString(identity.label);
}

}

ExchangeReviewDialog::ExchangeReviewDialog(
    const network_view::exchange_review_presented_t& presented, QWidget* parent)
    : AidaDialog(parent), presented_(presented) {
    switch (presented_.kind) {
    case network_view::exchange_review_kind_t::create_issue:
        setWindowTitle("Create Network issue");
        buildIssue();
        break;
    case network_view::exchange_review_kind_t::replay:
        setWindowTitle("Review Network replay");
        buildReplay();
        break;
    case network_view::exchange_review_kind_t::remove:
        setWindowTitle("Review Network artifact removal");
        buildRemove();
        break;
    default:
        break;
    }

    RevalidateScope::hooks_t hooks;
    const auto primary = presented_.primary;
    const auto related = presented_.related;
    hooks.generation_fn = [primary, related]() -> quint64 {
        network_view::artifact_snapshot_t snapshot;
        std::string reason;
        if (!network_view::resolve_artifact(primary, snapshot, reason))
            return 0;
        if (related.valid() &&
            !network_view::resolve_artifact(related, snapshot, reason))
            return 0;
        return 1;
    };
    add_revalidate_scope(std::move(hooks),
        "The reviewed network artifact changed; the review was closed.");
}

void ExchangeReviewDialog::buildIssue() {
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel("Create a persistent Scanner issue from the exact reviewed artifact.", this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    nameEdit_ = new QLineEdit(QString::fromStdString(presented_.issue_name), this);
    nameEdit_->setMaxLength(159);
    form->addRow("Name", nameEdit_);

    descriptionEdit_ = new QPlainTextEdit(QString::fromStdString(presented_.issue_description), this);
    descriptionEdit_->setPlaceholderText(QString());
    form->addRow("Description", descriptionEdit_);

    remediationEdit_ = new QPlainTextEdit(QString::fromStdString(presented_.issue_remediation), this);
    form->addRow("Remediation", remediationEdit_);

    auto* severityRow = new QWidget(this);
    auto* severityLayout = new QFormLayout(severityRow);
    severityLayout->setContentsMargins(0, 0, 0, 0);
    severityCombo_ = new QComboBox(severityRow);
    severityCombo_->addItems({"Information", "Low", "Medium", "High", "Critical"});
    severityCombo_->setCurrentIndex(presented_.issue_severity);
    severityLayout->addRow("Severity", severityCombo_);
    confidenceCombo_ = new QComboBox(severityRow);
    confidenceCombo_->addItems({"Tentative", "Firm", "Certain"});
    confidenceCombo_->setCurrentIndex(presented_.issue_confidence);
    severityLayout->addRow("Confidence", confidenceCombo_);
    form->addRow(severityRow);
    layout->addLayout(form);

    auto* evidence = new QLabel(QStringLiteral("Evidence: %1").arg(identity_label(presented_.primary)), this);
    evidence->setWordWrap(true);
    layout->addWidget(evidence);

    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setProperty("aidaTone", QStringLiteral("error"));
    errorLabel_->hide();
    layout->addWidget(errorLabel_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    confirmButton_ = buttons->button(QDialogButtonBox::Ok);
    confirmButton_->setText("Create Issue");
    confirmButton_->setEnabled(!blank_text(nameEdit_->text()));
    connect(nameEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        confirmButton_->setEnabled(!blank_text(text));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &ExchangeReviewDialog::onConfirm);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    setMinimumWidth(dialog_min_width_chars(this, 48));
}

void ExchangeReviewDialog::buildReplay() {
    const auto& primary = presented_.primary;
    const auto& related = presented_.related;
    const network_view::artifact_identity_t& request =
        request_identity_kind(primary.kind) ? primary
            : request_identity_kind(related.kind) ? related : primary;

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel("Send the exact reviewed request to its original target?", this);
    layout->addWidget(intro);
    auto* target = new QLabel(QStringLiteral("Target: %1:%2 (%3)")
        .arg(QString::fromStdString(request.target_host))
        .arg(request.target_port)
        .arg(request.use_tls ? "TLS" : "plaintext"), this);
    layout->addWidget(target);
    auto* size = new QLabel(QStringLiteral("Request: %1 bytes")
        .arg(static_cast<quint64>(request.content_size)), this);
    layout->addWidget(size);
    auto* note = new QLabel("The response will be retained as a new Proxy exchange. TLS verification and pin policy remain enforced.", this);
    note->setWordWrap(true);
    layout->addWidget(note);

    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setProperty("aidaTone", QStringLiteral("error"));
    errorLabel_->hide();
    layout->addWidget(errorLabel_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    confirmButton_ = buttons->button(QDialogButtonBox::Ok);
    confirmButton_->setText("Send Request");
    connect(buttons, &QDialogButtonBox::accepted, this, &ExchangeReviewDialog::onConfirm);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    setMinimumWidth(dialog_min_width_chars(this, 48));
}

void ExchangeReviewDialog::buildRemove() {
    const bool repeater =
        presented_.primary.kind == network_view::artifact_kind_t::repeater_request ||
        presented_.primary.kind == network_view::artifact_kind_t::repeater_response;

    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(QStringLiteral("Remove %1?").arg(identity_label(presented_.primary)), this);
    layout->addWidget(title);
    auto* body = new QLabel(repeater
        ? "The whole reviewed Repeater tab, including its current request and response, will be removed."
        : "The whole reviewed Proxy exchange, including request, response, tags, notes, and evidence identity, will be removed.", this);
    body->setWordWrap(true);
    layout->addWidget(body);
    auto* receipt = new QLabel("A one-step recovery receipt will remain available until it is dismissed or replaced by another removal.", this);
    receipt->setWordWrap(true);
    layout->addWidget(receipt);

    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setProperty("aidaTone", QStringLiteral("error"));
    errorLabel_->hide();
    layout->addWidget(errorLabel_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    confirmButton_ = buttons->button(QDialogButtonBox::Ok);
    confirmButton_->setText("Remove");
    connect(buttons, &QDialogButtonBox::accepted, this, &ExchangeReviewDialog::onConfirm);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    setMinimumWidth(dialog_min_width_chars(this, 48));
}

void ExchangeReviewDialog::onConfirm() {
    std::string reason;
    bool accepted = false;
    switch (presented_.kind) {
    case network_view::exchange_review_kind_t::create_issue: {
        network_view::exchange_review_presented_t values = presented_;
        values.issue_name = nameEdit_->text().toStdString();
        values.issue_description = descriptionEdit_->toPlainText().toStdString();
        values.issue_remediation = remediationEdit_->toPlainText().toStdString();
        values.issue_severity = severityCombo_->currentIndex();
        values.issue_confidence = confidenceCombo_->currentIndex();
        accepted = network_view::submit_exchange_review_issue(values, reason);
        break;
    }
    case network_view::exchange_review_kind_t::replay:
        accepted = network_view::submit_exchange_review_replay(reason);
        break;
    case network_view::exchange_review_kind_t::remove:
        accepted = network_view::submit_exchange_review_removal(reason);
        break;
    default:
        break;
    }
    if (accepted) {
        accept();
        return;
    }
    const QString message = reason.empty()
        ? QStringLiteral("The reviewed operation was rejected.")
        : QString::fromStdString(reason);
    errorLabel_->setText(message);
    errorLabel_->show();
    notify_error(message);
}

ExchangeRemoveReceiptDialog::ExchangeRemoveReceiptDialog(
    const network_view::exchange_remove_receipt_t& receipt, QWidget* parent)
    : AidaDialog(parent), receipt_(receipt) {
    setWindowTitle("Network removal receipt");
    auto* layout = new QVBoxLayout(this);
    titleLabel_ = new QLabel(this);
    layout->addWidget(titleLabel_);
    bodyLabel_ = new QLabel(this);
    bodyLabel_->setWordWrap(true);
    layout->addWidget(bodyLabel_);
    pendingLabel_ = new QLabel("Restoring reviewed artifact...", this);
    pendingLabel_->setProperty("aidaTone", QStringLiteral("info"));
    layout->addWidget(pendingLabel_);
    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setProperty("aidaTone", QStringLiteral("error"));
    layout->addWidget(errorLabel_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    undoButton_ = new widgets::AidaButton("Undo Removal", this);
    undoButton_->setKind(widgets::AidaButton::Kind::Secondary);
    undoButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttons->addButton(undoButton_, QDialogButtonBox::AcceptRole);
    closeButton_ = buttons->button(QDialogButtonBox::Close);
    connect(undoButton_, &QAbstractButton::clicked, this, [this] {
        std::string reason;
        if (!network_view::submit_exchange_remove_undo(reason)) {
            receipt_.error = reason.empty()
                ? "The removed artifact could not be restored." : reason;
            rebuildUi();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        network_view::dismiss_exchange_remove_receipt();
        reject();
    });
    layout->addWidget(buttons);
    setMinimumWidth(dialog_min_width_chars(this, 48));
    auto* fallbackDrain = new QTimer(this);
    fallbackDrain->setInterval(250);
    connect(fallbackDrain, &QTimer::timeout, this, [] {
        network_view::drain_exchange_remove_undo_fallback();
    });
    fallbackDrain->start();
    rebuildUi();
}

void ExchangeRemoveReceiptDialog::refresh(
    const network_view::exchange_remove_receipt_t& receipt) {
    receipt_ = receipt;
    rebuildUi();
}

void ExchangeRemoveReceiptDialog::rebuildUi() {
    titleLabel_->setText(receipt_.restored
        ? "The removed artifact was restored."
        : "The reviewed artifact was removed from its owning Network store.");
    bodyLabel_->setText(receipt_.source == network_view::exchange_remove_source_t::proxy
        ? "Recovery restores the complete Proxy exchange at its reviewed position if its identity remains free and history has capacity."
        : "Recovery restores the complete Repeater tab at its reviewed position if its identity remains free and Repeater has capacity.");
    pendingLabel_->setVisible(receipt_.operation_pending);
    errorLabel_->setText(QString::fromStdString(receipt_.error));
    errorLabel_->setVisible(!receipt_.error.empty());

    bool canRestore = !receipt_.operation_pending && !receipt_.restored;
    if (receipt_.source == network_view::exchange_remove_source_t::proxy)
        canRestore = canRestore && !network_view::proxy_operation_pending();
    if (receipt_.source == network_view::exchange_remove_source_t::repeater)
        canRestore = canRestore && network_view::g_state.repeater_entries.size() < 128;
    undoButton_->setEnabled(canRestore);
    closeButton_->setEnabled(!receipt_.operation_pending);
}

ExchangeReviewHost::ExchangeReviewHost(QObject* parent)
    : QObject(parent) {}

void ExchangeReviewHost::installHooks() {
    network_view::set_exchange_review_display(
        [this](const network_view::exchange_review_presented_t& presented) {
            presentReview(presented);
        });
    network_view::set_exchange_remove_receipt_display(
        [this](const network_view::exchange_remove_receipt_t& receipt) {
            presentReceipt(receipt);
        });
}

void ExchangeReviewHost::presentReview(
    const network_view::exchange_review_presented_t& presented) {
    if (reviewDialog_)
        reviewDialog_->close();
    auto* dialog = new ExchangeReviewDialog(presented, nullptr);
    reviewDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void ExchangeReviewHost::presentReceipt(
    const network_view::exchange_remove_receipt_t& receipt) {
    if (receiptDialog_) {
        receiptDialog_->refresh(receipt);
        if (!receiptDialog_->isVisible())
            receiptDialog_->open();
        return;
    }
    auto* dialog = new ExchangeRemoveReceiptDialog(receipt, nullptr);
    receiptDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

}
