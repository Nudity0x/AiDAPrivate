#include "qt/ai/qt_ai_chat_dialogs.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/editor/code_editor.hpp"
#include "qt/ai/qt_ai_domain.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::ai {

AidaConfirmDialog::AidaConfirmDialog(const aida_confirm_request_t& request, QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("aida.ai.confirm_dialog"));
    setProperty("aidaRole", QStringLiteral("dialog"));
    if (request.destructive)
        setProperty("aidaSeverity", QStringLiteral("high"));
    setWindowTitle(request.verb + QStringLiteral(" — confirm"));
    setModal(true);
    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(t.panel.padding, t.panel.padding,
                               t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setVerticalSpacing(t.spacing.xs);
    auto add_row = [form](const QString& label, const QString& value, bool dim) {
        if (value.isEmpty())
            return;
        auto* value_label = new QLabel(value);
        value_label->setWordWrap(true);
        if (dim)
            value_label->setProperty("aidaVariant", QStringLiteral("secondary"));
        form->addRow(label + QLatin1Char(':'), value_label);
    };
    add_row(QStringLiteral("Target"), request.target, false);
    add_row(QStringLiteral("Scope"), request.scope, false);
    add_row(QStringLiteral("Effect"), request.effect, false);
    add_row(QStringLiteral("Reversibility"), request.reversibility, true);
    if (!request.prerequisite.isEmpty()) {
        auto* prerequisite = new QLabel(request.prerequisite);
        prerequisite->setWordWrap(true);
        prerequisite->setProperty("aidaVariant", QStringLiteral("warning"));
        form->addRow(QStringLiteral("Blocked:"), prerequisite);
    }
    layout->addLayout(form, 1);

    auto* buttons = new QDialogButtonBox(this);
    confirm_button_ = buttons->addButton(request.confirm_label.isEmpty()
        ? request.verb : request.confirm_label,
        request.destructive ? QDialogButtonBox::DestructiveRole : QDialogButtonBox::AcceptRole);
    confirm_button_->setEnabled(request.confirm_enabled);
    auto* cancel = buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(buttons);
    setMinimumWidth(420);
}

void AidaConfirmDialog::request(const aida_confirm_request_t& request, QWidget* parent,
                                std::function<void()> on_confirm) {
    auto* dialog = new AidaConfirmDialog(request, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &QDialog::accepted, dialog,
                     [on_confirm = std::move(on_confirm)] {
        if (on_confirm)
            on_confirm();
    });
    dialog->open();
}

AidaApplyChangeDialog::AidaApplyChangeDialog(
    const aida::automation_ui::message_identity_t& identity, QWidget* parent)
    : QDialog(parent), identity_(identity) {
    setObjectName(QStringLiteral("aida.ai.apply_change"));
    setProperty("aidaRole", QStringLiteral("dialog"));
    setProperty("aidaSeverity", QStringLiteral("high"));
    setWindowTitle(QStringLiteral("Apply AI Change"));
    setModal(true);
    setMinimumSize(560, 380);
    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(t.panel.padding, t.panel.padding,
                               t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);
    auto* form = new QFormLayout();
    layout->addLayout(form);
    before_ = new QPlainTextEdit(this);
    before_->setObjectName(QStringLiteral("aida.ai.apply_change.before"));
    before_->setReadOnly(true);
    before_->setFont(theme::fonts::codeRegular());
    before_->setMaximumBlockCount(4096);
    after_ = new QPlainTextEdit(this);
    after_->setObjectName(QStringLiteral("aida.ai.apply_change.after"));
    after_->setReadOnly(true);
    after_->setFont(theme::fonts::codeRegular());
    after_->setMaximumBlockCount(4096);
    prerequisite_ = new QLabel(this);
    prerequisite_->setWordWrap(true);
    prerequisite_->setProperty("aidaVariant", QStringLiteral("secondary"));
    auto* buttons = new QDialogButtonBox(this);
    confirm_button_ = buttons->addButton(QStringLiteral("Apply Reviewed Change"),
                                         QDialogButtonBox::DestructiveRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &AidaApplyChangeDialog::onConfirm);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(new QLabel(QStringLiteral("BEFORE"), this));
    layout->addWidget(before_);
    layout->addWidget(new QLabel(QStringLiteral("AFTER"), this));
    layout->addWidget(after_);
    layout->addWidget(prerequisite_);
    layout->addWidget(buttons);
    populate();
}

void AidaApplyChangeDialog::populate() {
    using namespace aida::automation_ui;
    const auto capability = message_action_capability(identity_, message_action_t::apply_change);
    const auto reverse_publication = reverse_engineering_proposal_snapshot();
    const auto& reverse_proposal = *reverse_publication;
    reverse_linked_ = reverse_proposal.pending &&
        reverse_proposal.source.session_id == identity_.session_id &&
        reverse_proposal.source.fingerprint == identity_.fingerprint;

    if (reverse_linked_) {
        const QString scope = QString::fromStdString(reverse_proposal.kind_label) +
            QStringLiteral("; expected generation %1; expected revision %2; overlay/artifact fence %3")
                .arg(reverse_proposal.expected_generation)
                .arg(reverse_proposal.expected_revision)
                .arg(reverse_proposal.expected_overlay_revision);
        before_->setPlainText(QString::fromStdString(reverse_proposal.before_value));
        after_->setPlainText(QString::fromStdString(reverse_proposal.after_value));
        prerequisite_->setText(capability.enabled
            ? QString::fromStdString(reverse_proposal.consequence)
            : QString::fromStdString(capability.disabled_reason));
        confirm_button_->setText(
            reverse_proposal.kind == reverse_engineering_proposal_kind_t::static_patch
                ? QStringLiteral("Open Static Patch Review")
                : reverse_proposal.kind == reverse_engineering_proposal_kind_t::live_patch
                    ? QStringLiteral("Stage in Patch Review")
                    : reverse_proposal.kind ==
                            reverse_engineering_proposal_kind_t::network_replay_staging
                        ? QStringLiteral("Stage in Repeater")
                        : QStringLiteral("Apply Reviewed Change"));
        confirm_button_->setEnabled(capability.enabled && !reverse_proposal.stale &&
                                    !reverse_proposal.applying);
        return;
    }

    const auto proposal = editor_proposal_snapshot();
    const int pending_hunks = code_editor_widget::pending_hunk_count();
    const bool hunks_resolved = code_editor_widget::has_pending_diff() &&
        code_editor_widget::pending_diff().fully_resolved();
    before_->setVisible(false);
    after_->setVisible(false);
    const QString scope = QStringLiteral("%1 reviewed hunks; base revision %2; base hash 0x%3")
        .arg((std::max)(pending_hunks, 0))
        .arg(proposal.base_document_revision)
        .arg(proposal.base_content_hash, 0, 16);
    prerequisite_->setText(scope + QLatin1Char('\n') +
        QStringLiteral("Applies accepted hunks to the in-memory editor buffer and leaves rejected hunks unchanged; it does not save the file to disk.\n") +
        (capability.enabled && hunks_resolved
            ? QStringLiteral("Use the code editor Undo command before saving to reverse the applied buffer change.")
            : hunks_resolved ? QString::fromStdString(capability.disabled_reason)
                : QStringLiteral("Accept or reject every hunk before applying the resolved diff.")));
    confirm_button_->setText(QStringLiteral("Apply Reviewed Hunks"));
    confirm_button_->setEnabled(capability.enabled && pending_hunks > 0 &&
                                proposal.pending && !proposal.stale && hunks_resolved);
}

void AidaApplyChangeDialog::onConfirm() {
    using namespace aida::automation_ui;
    if (!confirm_button_->isEnabled())
        return;
    if (reverse_linked_) {
        const auto applied = execute_message_action(identity_, message_action_t::apply_change);
        if (feedback_)
            feedback_(QString::fromStdString(applied.detail));
        if (applied.succeeded) {
            if (!applied.target_view_id.empty())
                open_ai_view(applied.target_view_id);
            accept();
        }
        return;
    }
    const auto proposal = editor_proposal_snapshot();
    const auto reviewed = confirm_editor_proposal_review(proposal);
    if (!reviewed.succeeded) {
        if (feedback_)
            feedback_(QString::fromStdString(reviewed.detail));
        return;
    }
    const auto applied = execute_message_action(identity_, message_action_t::apply_change);
    if (feedback_)
        feedback_(QString::fromStdString(applied.detail));
    if (applied.succeeded) {
        if (!applied.target_view_id.empty())
            open_ai_view(applied.target_view_id);
        accept();
    }
}

void AidaApplyChangeDialog::request(
    const aida::automation_ui::message_identity_t& identity, QWidget* parent,
    std::function<void(const QString& detail)> feedback) {
    auto* dialog = new AidaApplyChangeDialog(identity, parent);
    dialog->feedback_ = std::move(feedback);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

}
