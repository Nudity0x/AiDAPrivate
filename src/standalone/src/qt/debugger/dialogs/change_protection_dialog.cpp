#include "qt/debugger/dialogs/change_protection_dialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "qt/debugger/debugger_mutation_queue.hpp"
#include "qt/theme/aida_fonts.hpp"

namespace aida::qt::debugger {

QPointer<ChangeProtectionDialog> ChangeProtectionDialog::active_;

namespace {
constexpr const char* k_protect_labels[] = {
    "PAGE_NOACCESS (0x01)",
    "PAGE_READONLY (0x02)",
    "PAGE_READWRITE (0x04)",
    "PAGE_WRITECOPY (0x08)",
    "PAGE_EXECUTE (0x10)",
    "PAGE_EXECUTE_READ (0x20)",
    "PAGE_EXECUTE_READWRITE (0x40)",
    "PAGE_EXECUTE_WRITECOPY (0x80)"
};
constexpr std::uint32_t k_protect_values[] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
};
}

void ChangeProtectionDialog::openFor(
    const debugger_interaction::context_t& context, std::uint64_t address,
    std::uint64_t size, std::uint32_t old_protect, QWidget* parent) {
    if (active_) {
        active_->raise();
        active_->activateWindow();
        return;
    }
    auto* dialog = new ChangeProtectionDialog(context, address, size,
        old_protect, parent);
    active_ = dialog;
    dialog->open();
}

ChangeProtectionDialog::ChangeProtectionDialog(
    const debugger_interaction::context_t& context, std::uint64_t address,
    std::uint64_t size, std::uint32_t old_protect, QWidget* parent)
    : AidaDialog(parent), context_(context), address_(address), size_(size),
      old_protect_(old_protect) {
    setObjectName(QStringLiteral("aida.debugger.change_protection"));
    setWindowTitle(QStringLiteral("Change Protection"));
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumWidth(400);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    auto* address_label = new QLabel(QString::asprintf("%016llX",
        static_cast<unsigned long long>(address_)), this);
    address_label->setFont(theme::fonts::codeRegular());
    form->addRow(QStringLiteral("Address:"), address_label);
    form->addRow(QStringLiteral("Size:"),
        new QLabel(QString::asprintf("%llu bytes",
            static_cast<unsigned long long>(size_)), this));
    auto* current_label = new QLabel(QString::asprintf("0x%X", old_protect_),
        this);
    current_label->setFont(theme::fonts::codeRegular());
    form->addRow(QStringLiteral("Current:"), current_label);
    layout->addLayout(form);

    protect_combo_ = new QComboBox(this);
    protect_combo_->setObjectName(
        QStringLiteral("aida.debugger.change_protection.combo"));
    for (const char* label : k_protect_labels)
        protect_combo_->addItem(QString::fromLatin1(label));
    protect_combo_->setCurrentIndex(0);
    form = new QFormLayout();
    form->addRow(QStringLiteral("New protection:"), protect_combo_);
    layout->addLayout(form);

    gate_label_ = new QLabel(this);
    gate_label_->setWordWrap(true);
    gate_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    gate_label_->setVisible(false);
    layout->addWidget(gate_label_);

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

    auto* gate_timer = new QTimer(this);
    gate_timer->setInterval(250);
    gate_timer->setTimerType(Qt::CoarseTimer);
    connect(gate_timer, &QTimer::timeout, this, [this] {
        const auto gate = debugger_interaction::evaluate(
            debugger_interaction::capability_t::change_memory_protection,
            context_);
        const bool pending =
            DebuggerMutationQueue::instance().protectionPending();
        apply_button_->setEnabled(gate.enabled && !pending);
        gate_label_->setVisible(!gate.enabled);
        if (!gate.enabled)
            gate_label_->setText(QStringLiteral("Unavailable: %1")
                .arg(QString::fromLatin1(gate.disabled_reason
                    ? gate.disabled_reason
                    : "The protection change is unavailable.")));
    });
    gate_timer->start();

    bridge::AidaDialog::RevalidateScope::hooks_t hooks;
    const auto identity_context = context_;
    hooks.identity_fn = [identity_context]() {
        return QString::number(identity_context.target_pid) +
            QStringLiteral(":") +
            QString::number(identity_context.process_creation_time_100ns) +
            QStringLiteral(":") + QString::number(identity_context.address);
    };
    hooks.generation_fn = []() {
        return static_cast<quint64>(
            debugger_interaction::current_stop_generation());
    };
    add_revalidate_scope(hooks, QStringLiteral(
        "The debugger target or stop generation changed while the protection "
        "dialog was open."));
}

void ChangeProtectionDialog::apply() {
    const int choice = protect_combo_->currentIndex();
    if (choice < 0 ||
        choice >= static_cast<int>(sizeof(k_protect_values) /
            sizeof(k_protect_values[0])))
        return;
    if (DebuggerMutationQueue::instance().changeProtection(context_, address_,
            size_, k_protect_values[choice]))
        accept();
}

}
