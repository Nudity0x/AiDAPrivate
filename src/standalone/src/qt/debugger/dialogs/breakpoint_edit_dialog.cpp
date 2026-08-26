#include "qt/debugger/dialogs/breakpoint_edit_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/debugger/debugger_engine.hpp"

#include "qt/debugger/debugger_mutation_queue.hpp"
#include "qt/theme/aida_fonts.hpp"

namespace aida::qt::debugger {

QPointer<BreakpointEditDialog> BreakpointEditDialog::active_;

void BreakpointEditDialog::openFor(
    const debugger_interaction::context_t& context, int index,
    debugger_view::breakpoint_editor_focus_t focus, QWidget* parent) {
    if (active_) {
        active_->raise();
        active_->activateWindow();
        return;
    }
    const auto breakpoints = debugger_engine::snapshot_breakpoints();
    if (index < 0 || index >= static_cast<int>(breakpoints.size()))
        return;
    debugger_view::breakpoint_edit_state_t state;
    if (!debugger_view::retain_breakpoint_edit(state, index,
            breakpoints[static_cast<std::size_t>(index)], context, focus))
        return;
    auto* dialog = new BreakpointEditDialog(std::move(state), parent);
    active_ = dialog;
    dialog->open();
}

BreakpointEditDialog::BreakpointEditDialog(
    debugger_view::breakpoint_edit_state_t state, QWidget* parent)
    : AidaDialog(parent), state_(std::move(state)) {
    setObjectName(QStringLiteral("aida.debugger.breakpoint_edit"));
    setWindowTitle(QStringLiteral("Edit Breakpoint"));
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    auto* address_label = new QLabel(QString::asprintf("0x%016llX",
        static_cast<unsigned long long>(state_.address)), this);
    address_label->setFont(theme::fonts::codeRegular());
    form->addRow(QStringLiteral("Address:"), address_label);
    const char* type_label =
        state_.type == static_cast<int>(debugger_engine::bp_type_t::software)
            ? "Software"
            : state_.type ==
                    static_cast<int>(debugger_engine::bp_type_t::hardware_execute)
                ? "HW Exec"
                : state_.type ==
                        static_cast<int>(debugger_engine::bp_type_t::hardware_write)
                    ? "HW Write"
                    : state_.type ==
                            static_cast<int>(
                                debugger_engine::bp_type_t::hardware_read)
                        ? "HW Read"
                        : "Memory";
    form->addRow(QStringLiteral("Type:"),
        new QLabel(QString::fromLatin1(type_label), this));
    layout->addLayout(form);

    gate_label_ = new QLabel(this);
    gate_label_->setWordWrap(true);
    gate_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    gate_label_->setVisible(false);
    layout->addWidget(gate_label_);

    auto* edit_form = new QFormLayout();
    condition_edit_ = new QLineEdit(
        QString::fromStdString(state_.original_condition), this);
    condition_edit_->setObjectName(
        QStringLiteral("aida.debugger.breakpoint_edit.condition"));
    condition_edit_->setFont(theme::fonts::codeRegular());
    condition_edit_->setMaxLength(160);
    edit_form->addRow(
        QStringLiteral("Condition (evaluated when hit, 0 = skip):"),
        condition_edit_);
    log_edit_ = new QLineEdit(QString::fromStdString(state_.original_log), this);
    log_edit_->setObjectName(
        QStringLiteral("aida.debugger.breakpoint_edit.log"));
    log_edit_->setFont(theme::fonts::codeRegular());
    log_edit_->setMaxLength(160);
    edit_form->addRow(
        QStringLiteral("Log message (use {RAX}, {[RSP+8]} placeholders):"),
        log_edit_);
    layout->addLayout(edit_form);
    auto_continue_check_ = new QCheckBox(
        QStringLiteral("Auto-continue after log"), this);
    auto_continue_check_->setChecked(state_.original_auto_continue);
    layout->addWidget(auto_continue_check_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    apply_button_ = buttons->button(QDialogButtonBox::Apply);
    connect(buttons, &QDialogButtonBox::clicked, this,
        [this, buttons](QAbstractButton* button) {
            if (button == buttons->button(QDialogButtonBox::Apply))
                apply();
            else
                reject();
        });
    layout->addWidget(buttons);

    switch (state_.focus) {
        case debugger_view::breakpoint_editor_focus_t::log_message:
            log_edit_->setFocus();
            break;
        case debugger_view::breakpoint_editor_focus_t::auto_continue:
            auto_continue_check_->setFocus();
            break;
        case debugger_view::breakpoint_editor_focus_t::condition:
        default:
            condition_edit_->setFocus();
            break;
    }

    std::string reason;
    const bool current = debugger_view::breakpoint_edit_is_current(state_,
        reason);
    if (!current) {
        gate_label_->setText(QString::fromStdString(reason));
        gate_label_->setVisible(true);
        apply_button_->setEnabled(false);
    }

    bridge::AidaDialog::RevalidateScope::hooks_t hooks;
    const auto retained = state_;
    hooks.identity_fn = [retained]() {
        return QString::number(retained.address) + QStringLiteral(":") +
            QString::number(retained.fingerprint) + QStringLiteral(":") +
            QString::number(retained.idx);
    };
    hooks.generation_fn = []() {
        return debugger_engine::g_state.breakpoints_generation.load(
            std::memory_order_acquire);
    };
    add_revalidate_scope(hooks, QStringLiteral(
        "The breakpoint collection changed while the editor was open."));
}

void BreakpointEditDialog::apply() {
    std::string reason;
    if (!debugger_view::breakpoint_edit_is_current(state_, reason)) {
        notify_error(QString::fromStdString(reason));
        return;
    }
    debugger_view::submit_breakpoint_edit(state_,
        condition_edit_->text().toStdString(), log_edit_->text().toStdString(),
        auto_continue_check_->isChecked());
    accept();
}

}
