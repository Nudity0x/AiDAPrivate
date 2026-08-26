#include "qt/debugger/dialogs/confirm_dialogs.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "qt/bridge/aida_dialog.hpp"

#include "qt/theme/aida_fonts.hpp"

namespace aida::qt::debugger::confirm_dialogs {

namespace {

class ConfirmMutationDialog : public bridge::AidaDialog {
public:
    ConfirmMutationDialog(debugger_view::context_mutation_t mutation,
                          debugger_interaction::context_t context,
                          QWidget* parent)
        : AidaDialog(parent), mutation_(mutation), context_(std::move(context)) {
        setObjectName(QStringLiteral("aida.debugger.confirm_mutation"));
        setWindowTitle(QStringLiteral("Confirm Debugger Mutation"));
        setAttribute(Qt::WA_DeleteOnClose);

        const auto review = debugger_view::review_context_mutation(mutation_);

        auto* layout = new QVBoxLayout(this);
        auto* scope_label = new QLabel(
            QStringLiteral("Scope: %1").arg(QString::fromLatin1(review.scope)),
            this);
        layout->addWidget(scope_label);
        auto* consequence_label = new QLabel(
            QString::fromLatin1(review.consequence), this);
        consequence_label->setWordWrap(true);
        layout->addWidget(consequence_label);
        if (context_.address != 0) {
            auto* address_label = new QLabel(QString::asprintf(
                "Address: 0x%016llX",
                static_cast<unsigned long long>(context_.address)), this);
            address_label->setFont(theme::fonts::codeRegular());
            layout->addWidget(address_label);
        }
        if (context_.thread_id != 0)
            layout->addWidget(new QLabel(QString::asprintf("Thread: %u",
                static_cast<unsigned>(context_.thread_id)), this));

        auto* gate_label = new QLabel(this);
        gate_label->setWordWrap(true);
        gate_label->setProperty("aidaVariant", QStringLiteral("warning"));
        const auto gate = debugger_interaction::evaluate(review.capability,
            context_);
        const auto retention = debugger_view::context_item_retention(context_);
        const bool retained =
            retention == debugger_view::context_retention_t::current;
        if (retention == debugger_view::context_retention_t::busy)
            gate_label->setText(QStringLiteral(
                "Unavailable: debugger state is updating; retry when the "
                "current refresh completes."));
        else if (!retained)
            gate_label->setText(QStringLiteral(
                "Unavailable: the selected debugger item changed; select a "
                "current row."));
        else if (!gate.enabled)
            gate_label->setText(QStringLiteral("Unavailable: %1")
                .arg(QString::fromLatin1(gate.disabled_reason
                    ? gate.disabled_reason : "The action is unavailable.")));
        layout->addWidget(gate_label);
        gate_label->setVisible(!gate_label->text().isEmpty());

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Confirm"));
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled(gate.enabled && retained);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            debugger_view::execute_context_mutation(mutation_, context_);
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this,
            &QDialog::reject);
        layout->addWidget(buttons);

        bridge::AidaDialog::RevalidateScope::hooks_t hooks;
        const auto identity_context = context_;
        hooks.identity_fn = [identity_context]() {
            return QString::number(identity_context.target_pid) +
                QStringLiteral(":") +
                QString::number(identity_context.process_creation_time_100ns) +
                QStringLiteral(":") + QString::number(identity_context.address) +
                QStringLiteral(":") + QString::number(identity_context.value) +
                QStringLiteral(":") + QString::number(identity_context.index);
        };
        hooks.generation_fn = []() {
            return static_cast<quint64>(
                debugger_interaction::current_stop_generation());
        };
        add_revalidate_scope(hooks, QStringLiteral(
            "The debugger target or stop generation changed before the "
            "mutation could be confirmed."));
    }

private:
    debugger_view::context_mutation_t mutation_;
    debugger_interaction::context_t context_;
};

void present(debugger_view::context_mutation_t mutation,
             const debugger_interaction::context_t& context, QWidget* parent) {
    auto* dialog = new ConfirmMutationDialog(mutation, context, parent);
    dialog->open();
}

}

void present_mutation(debugger_view::context_mutation_t mutation,
                      const debugger_interaction::context_t& context,
                      QWidget* parent) {
    present(mutation, context, parent);
}

void confirm_set_instruction_pointer(
    const debugger_interaction::context_t& context, QWidget* parent) {
    present(debugger_view::context_mutation_t::set_instruction_pointer,
        context, parent);
}

void confirm_terminate_thread(const debugger_interaction::context_t& context,
                              QWidget* parent) {
    present(debugger_view::context_mutation_t::terminate_thread, context,
        parent);
}

void confirm_close_handle(const debugger_interaction::context_t& context,
                          QWidget* parent) {
    present(debugger_view::context_mutation_t::close_handle, context, parent);
}

void confirm_apply_patch(const debugger_interaction::context_t& context,
                         QWidget* parent) {
    present(debugger_view::context_mutation_t::apply_patch, context, parent);
}

void confirm_revert_patch(const debugger_interaction::context_t& context,
                          QWidget* parent) {
    present(debugger_view::context_mutation_t::revert_patch, context, parent);
}

void confirm_remove_patch(const debugger_interaction::context_t& context,
                          QWidget* parent) {
    present(debugger_view::context_mutation_t::remove_patch, context, parent);
}

void confirm_revert_all_patches(
    const debugger_interaction::context_t& context, QWidget* parent) {
    present(debugger_view::context_mutation_t::revert_all_patches, context,
        parent);
}

void confirm_remove_watch(const debugger_interaction::context_t& context,
                          QWidget* parent) {
    present(debugger_view::context_mutation_t::remove_watch, context, parent);
}

void confirm_remove_bookmark(const debugger_interaction::context_t& context,
                             QWidget* parent) {
    present(debugger_view::context_mutation_t::remove_bookmark, context,
        parent);
}

}
