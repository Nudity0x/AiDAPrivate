#include "aida_line_edit.hpp"

#include <QIntValidator>

namespace aida::qt::widgets {

AidaLineEdit::AidaLineEdit(QWidget* parent)
    : QLineEdit(parent)
{
    setObjectName(QStringLiteral("aida.line_edit"));
    setClearButtonEnabled(false);
}

AidaLineEdit::AidaLineEdit(const QString& placeholder, QWidget* parent)
    : AidaLineEdit(parent)
{
    setPlaceholderText(placeholder);
    if (!placeholder.isEmpty())
        setAccessibleName(placeholder);
}

void AidaLineEdit::setVariant(Variant variant)
{
    if (variant_ == variant)
        return;
    variant_ = variant;
    switch (variant_) {
    case Variant::Text:
        setEchoMode(QLineEdit::Normal);
        setValidator(nullptr);
        break;
    case Variant::Password:
        setEchoMode(QLineEdit::Password);
        setValidator(nullptr);
        break;
    case Variant::Int:
        setEchoMode(QLineEdit::Normal);
        if (!int_validator_)
            int_validator_ = new QIntValidator(this);
        setValidator(int_validator_);
        break;
    }
}

void AidaLineEdit::setIntRange(int bottom, int top)
{
    if (variant_ != Variant::Int)
        setVariant(Variant::Int);
    int_validator_->setRange(bottom, top);
}

int AidaLineEdit::intValue() const
{
    bool ok = false;
    const int v = text().toInt(&ok);
    return ok ? v : 0;
}

void AidaLineEdit::setIntValue(int value)
{
    setText(QString::number(value));
}

}
