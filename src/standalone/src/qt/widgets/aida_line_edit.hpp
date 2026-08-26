#pragma once

#include <QLineEdit>

class QIntValidator;

namespace aida::qt::widgets {

class AidaLineEdit : public QLineEdit
{
    Q_OBJECT
public:
    enum class Variant { Text, Password, Int };

    explicit AidaLineEdit(QWidget* parent = nullptr);
    explicit AidaLineEdit(const QString& placeholder, QWidget* parent = nullptr);

    void setVariant(Variant variant);
    Variant variant() const { return variant_; }

    void setIntRange(int bottom, int top);
    int intValue() const;
    void setIntValue(int value);

private:
    Variant variant_ = Variant::Text;
    QIntValidator* int_validator_ = nullptr;
};

}
