#include "qt/disasm/dialogs/disasm_rename_dialog.hpp"

#include "qt/widgets/aida_notice.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QValidator>
#include <QVBoxLayout>

namespace aida::qt::disasm::dialogs {

namespace {

class SymbolNameValidator : public QValidator {
public:
    using QValidator::QValidator;

    State validate(QString& input, int& pos) const override
    {
        static_cast<void>(pos);
        if (input.isEmpty())
            return Intermediate;
        if (input.size() > 511)
            return Invalid;
        const auto first = input.front();
        const auto first_char = [&] {
            const ushort value = first.unicode();
            return (first.isLetter() || first == u'_' || first == u'?' ||
                    first == u'$' || first == u'@') &&
                value < 128;
        };
        if (!first_char())
            return Invalid;
        for (qsizetype index = 1; index < input.size(); ++index) {
            const QChar character = input[index];
            const ushort value = character.unicode();
            if (value >= 128)
                return Invalid;
            if (!(character.isLetterOrNumber() || character == u'_' ||
                  character == u'?' || character == u'$' || character == u'@' ||
                  character == u':' || character == u'.'))
                return Invalid;
        }
        return Acceptable;
    }
};

}

AidaDisasmRenameDialog::AidaDisasmRenameDialog(
    disasm_view::workspace_context_t context, aida::analysis::address_t address,
    QWidget* parent)
    : bridge::AidaDialog(parent), context_(std::move(context)), address_(address)
{
    setObjectName(QStringLiteral("aida.disasm.dialog.rename"));
    setWindowTitle(QStringLiteral("Rename item"));
    setModal(true);
    auto* layout = new QVBoxLayout(this);
    const auto runtime = disasm_view::runtime_address(context_, address_);
    auto* address_label = new QLabel(QStringLiteral("Address: 0x%1")
        .arg(runtime.value_or(address_.value), 0, 16), this);
    layout->addWidget(address_label);
    editor_ = new QLineEdit(this);
    editor_->setObjectName(QStringLiteral("aida.disasm.dialog.rename.input"));
    editor_->setMaxLength(511);
    editor_->setValidator(new SymbolNameValidator(editor_));
    editor_->setText(QString::fromStdString(disasm_view::resolve_name(context_, address_)));
    layout->addWidget(editor_);
    error_ = new widgets::AidaNotice(QStringLiteral("Rename rejected"), QString(),
        widgets::AidaSemantic::Error, this);
    error_->setObjectName(QStringLiteral("aida.disasm.dialog.rename.error"));
    error_->hide();
    layout->addWidget(error_);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Apply)->setText(QStringLiteral("Apply"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::clicked, this,
        [this](QAbstractButton* button) {
            auto* box = qobject_cast<QDialogButtonBox*>(sender());
            if (box && box->buttonRole(button) == QDialogButtonBox::ApplyRole)
                apply();
            else
                reject();
        });
    connect(editor_, &QLineEdit::returnPressed, this, [this] {
        if (editor_->hasAcceptableInput())
            apply();
    });
    editor_->setFocus();
    editor_->selectAll();
}

void AidaDisasmRenameDialog::apply()
{
    const auto proposed = editor_->text().toStdString();
    QString text = editor_->text();
    int pos = 0;
    if (proposed.empty() || !editor_->hasAcceptableInput() ||
        editor_->validator()->validate(text, pos) != QValidator::Acceptable) {
        error_->setMessage(QStringLiteral("Use a non-empty identifier with no whitespace."));
        error_->show();
        return;
    }
    if (disasm_view::queue_rename(context_, address_, proposed)) {
        accept();
        return;
    }
    error_->setMessage(QStringLiteral("The workspace is unavailable or closing."));
    error_->show();
}

}
