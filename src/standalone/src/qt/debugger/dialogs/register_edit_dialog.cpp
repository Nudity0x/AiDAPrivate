#include "qt/debugger/dialogs/register_edit_dialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "core/debugger/debugger_engine.hpp"
#include "core/debugger/debugger_view.hpp"

#include "qt/debugger/debugger_mutation_queue.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::debugger {

QPointer<RegisterEditDialog> RegisterEditDialog::active_;

namespace {

std::uint64_t parse_hex_text(const QString& text) {
    std::string trimmed;
    for (const QChar c : text) {
        if (c == u' ' || c == u'\t' || c == u'\r' || c == u'\n')
            continue;
        trimmed.push_back(c.toLatin1());
    }
    if (trimmed.empty())
        return 0;
    const char* s = trimmed.c_str();
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    std::uint64_t value = 0;
    for (; *s; ++s) {
        const char c = *s;
        std::uint8_t digit;
        if (c >= '0' && c <= '9') digit = static_cast<std::uint8_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<std::uint8_t>(10 + (c - 'a'));
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<std::uint8_t>(10 + (c - 'A'));
        else break;
        value = (value << 4) | digit;
    }
    return value;
}

QString lowercase_name(QString name) {
    return name.toLower();
}

}

void RegisterEditDialog::openFor(
    const debugger_interaction::context_t& context, const QString& registerName,
    std::uint64_t initialValue, QWidget* parent) {
    if (active_) {
        active_->raise();
        active_->activateWindow();
        return;
    }
    auto* dialog = new RegisterEditDialog(context, registerName, initialValue,
        parent);
    active_ = dialog;
    dialog->open();
}

RegisterEditDialog::RegisterEditDialog(
    const debugger_interaction::context_t& context, QString registerName,
    std::uint64_t initialValue, QWidget* parent)
    : AidaDialog(parent), context_(context),
      register_name_(std::move(registerName)), initial_value_(initialValue) {
    setObjectName(QStringLiteral("aida.debugger.register_edit"));
    setWindowTitle(QStringLiteral("Edit Register"));
    setAttribute(Qt::WA_DeleteOnClose);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    auto* name_label = new QLabel(register_name_, this);
    name_label->setFont(theme::fonts::bodyEm());
    form->addRow(QStringLiteral("Register:"), name_label);
    current_label_ = new QLabel(
        QString::asprintf("0x%016llX",
            static_cast<unsigned long long>(initial_value_)), this);
    current_label_->setFont(theme::fonts::codeRegular());
    form->addRow(QStringLiteral("Current:"), current_label_);
    value_edit_ = new QLineEdit(this);
    value_edit_->setObjectName(QStringLiteral("aida.debugger.register_edit.value"));
    value_edit_->setFont(theme::fonts::codeRegular());
    value_edit_->setMaxLength(24);
    value_edit_->setText(QString::asprintf("%016llX",
        static_cast<unsigned long long>(initial_value_)));
    value_edit_->selectAll();
    form->addRow(QStringLiteral("New value (hex):"), value_edit_);
    layout->addLayout(form);

    gate_label_ = new QLabel(this);
    gate_label_->setWordWrap(true);
    gate_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    gate_label_->setVisible(false);
    layout->addWidget(gate_label_);

    const auto gate = debugger_interaction::evaluate(
        debugger_interaction::capability_t::edit_register, context_);
    if (!gate.enabled) {
        gate_label_->setText(QStringLiteral("Unavailable: %1")
            .arg(QString::fromLatin1(gate.disabled_reason
                ? gate.disabled_reason : "The register edit is unavailable.")));
        gate_label_->setVisible(true);
    }

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    apply_button_ = buttons->button(QDialogButtonBox::Apply);
    apply_button_->setEnabled(gate.enabled);
    connect(buttons, &QDialogButtonBox::clicked, this,
        [this, buttons](QAbstractButton* button) {
            if (button == buttons->button(QDialogButtonBox::Apply))
                apply();
            else
                reject();
        });
    connect(value_edit_, &QLineEdit::returnPressed, this, [this] {
        if (apply_button_->isEnabled())
            apply();
    });
    layout->addWidget(buttons);

    bridge::AidaDialog::RevalidateScope::hooks_t hooks;
    hooks.identity_fn = [context, name = register_name_]() {
        return QString::fromStdString(name.toStdString()) +
            QStringLiteral("@") +
            QString::number(context.process_creation_time_100ns) +
            QStringLiteral(":") + QString::number(context.target_pid);
    };
    add_revalidate_scope(hooks, QStringLiteral(
        "The register editor's target process identity changed."));

    auto* gate_timer = new QTimer(this);
    gate_timer->setInterval(250);
    gate_timer->setTimerType(Qt::CoarseTimer);
    connect(gate_timer, &QTimer::timeout, this, [this] {
        const auto gate = debugger_interaction::evaluate(
            debugger_interaction::capability_t::edit_register, context_);
        apply_button_->setEnabled(gate.enabled);
        gate_label_->setVisible(!gate.enabled);
        if (!gate.enabled)
            gate_label_->setText(QStringLiteral("Unavailable: %1")
                .arg(QString::fromLatin1(gate.disabled_reason
                    ? gate.disabled_reason
                    : "The register edit is unavailable.")));
    });
    gate_timer->start();
}

void RegisterEditDialog::apply() {
    const std::uint64_t new_value = parse_hex_text(value_edit_->text());
    const QString lowered = lowercase_name(register_name_);
    const std::string lowered_std = lowered.toStdString();
    const auto context = context_;
    DebuggerMutationQueue::instance().queueMutation("Edit register",
        "debugger.register_edit", context,
        [lowered_std, new_value]() {
            debugger_view::mutation_result_t result;
            result.ok = debugger_engine::set_register(lowered_std, new_value);
            result.verified = result.ok &&
                debugger_view::resolve_register_token(lowered_std,
                    debugger_engine::get_registers()) == new_value;
            if (!result.verified)
                result.detail = result.ok ? "Register readback did not match."
                    : "Edit register failed: " + debugger_engine::last_error();
            else
                debugger_engine::invalidate_cache();
            return result;
        });
    accept();
}

}
