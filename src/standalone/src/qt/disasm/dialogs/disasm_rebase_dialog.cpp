#include "qt/disasm/dialogs/disasm_rebase_dialog.hpp"

#include "qt/widgets/aida_notice.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace aida::qt::disasm::dialogs {

AidaDisasmRebaseDialog::AidaDisasmRebaseDialog(
    disasm_view::workspace_context_t context, QWidget* parent)
    : bridge::AidaDialog(parent), context_(std::move(context))
{
    setObjectName(QStringLiteral("aida.disasm.dialog.rebase"));
    setWindowTitle(QStringLiteral("Rebase"));
    setModal(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("Image base"), this));
    editor_ = new QLineEdit(this);
    editor_->setObjectName(QStringLiteral("aida.disasm.dialog.rebase.input"));
    editor_->setPlaceholderText(QStringLiteral("0x140000000"));
    editor_->setText(QStringLiteral("0x%1").arg(
        disasm_view::display_image_base(context_), 0, 16));
    layout->addWidget(editor_);
    error_ = new widgets::AidaNotice(QStringLiteral("Invalid image base"), QString(),
        widgets::AidaSemantic::Error, this);
    error_->setObjectName(QStringLiteral("aida.disasm.dialog.rebase.error"));
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
    connect(editor_, &QLineEdit::returnPressed, this, &AidaDisasmRebaseDialog::apply);
    editor_->setFocus();
    editor_->selectAll();
}

void AidaDisasmRebaseDialog::apply()
{
    const auto base = disasm_view::parse_address_text(context_,
        editor_->text().toStdString());
    std::string error;
    if (!base || !disasm_view::apply_rebase(context_, *base, &error)) {
        error_->setMessage(QString::fromStdString(error.empty()
            ? "Invalid image base." : error));
        error_->show();
        return;
    }
    accept();
}

}
