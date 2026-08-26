#include "qt/pseudocode/local_rename_dialog.hpp"

#include "qt/analysis_bridge/pseudocode_session.hpp"
#include "qt/widgets/aida_notice.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QValidator>
#include <QVBoxLayout>

namespace aida::qt::pseudocode {

namespace {

class LocalIdentifierValidator : public QValidator {
public:
    using QValidator::QValidator;

    State validate(QString& input, int& pos) const override
    {
        static_cast<void>(pos);
        if (input.isEmpty())
            return Intermediate;
        if (input.size() > 128)
            return Invalid;
        const auto letter = [](const QChar character) {
            return (character >= u'a' && character <= u'z') ||
                   (character >= u'A' && character <= u'Z') || character == u'_';
        };
        if (!letter(input.front()))
            return Invalid;
        for (qsizetype index = 1; index < input.size(); ++index) {
            const auto character = input[index];
            if (!letter(character) && !(character >= u'0' && character <= u'9'))
                return Invalid;
        }
        return Acceptable;
    }
};

}

AidaPseudoLocalRenameDialog::AidaPseudoLocalRenameDialog(
    disasm_view::workspace_context_t context, QString old_name, QWidget* parent)
    : bridge::AidaDialog(parent), context_(std::move(context)),
      old_name_(std::move(old_name))
{
    setObjectName(QStringLiteral("aida.pseudocode.dialog.rename_local"));
    setWindowTitle(QStringLiteral("Rename Local"));
    setModal(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("Rename pseudocode-local '%1'")
        .arg(old_name_), this));
    auto* disclaimer = new QLabel(QStringLiteral(
        "Pseudocode only: the disassembly view keeps the original name."), this);
    disclaimer->setProperty("aidaVariant", QStringLiteral("secondary"));
    disclaimer->setWordWrap(true);
    layout->addWidget(disclaimer);
    editor_ = new QLineEdit(this);
    editor_->setObjectName(QStringLiteral("aida.pseudocode.dialog.rename_local.input"));
    editor_->setToolTip(QStringLiteral(
        "New local identifier: letters, digits and underscores; must not start with a digit"));
    editor_->setMaxLength(128);
    editor_->setValidator(new LocalIdentifierValidator(editor_));
    editor_->setText(old_name_);
    layout->addWidget(editor_);
    error_ = new widgets::AidaNotice(QStringLiteral("Rename rejected"), QString(),
        widgets::AidaSemantic::Error, this);
    error_->setObjectName(QStringLiteral("aida.pseudocode.dialog.rename_local.error"));
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

void AidaPseudoLocalRenameDialog::apply()
{
    std::string error;
    if (!pseudocode_view::apply_active_local_rename(context_,
            old_name_.toStdString(), editor_->text().toStdString(), error)) {
        error_->setMessage(QString::fromStdString(error));
        error_->show();
        return;
    }
    accept();
}

}
