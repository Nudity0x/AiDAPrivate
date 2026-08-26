#pragma once

#include <QLabel>

#include "aida_paint_utils.hpp"

namespace aida::qt::widgets {

class AidaBadge : public QLabel
{
    Q_OBJECT
public:
    explicit AidaBadge(QWidget* parent = nullptr);
    explicit AidaBadge(const QString& text, AidaSemantic kind = AidaSemantic::Neutral,
        QWidget* parent = nullptr);

    void setKind(AidaSemantic kind);
    AidaSemantic kind() const { return kind_; }

    void setBadgeColor(const QColor& color);
    void clearBadgeColor();
    bool hasCustomColor() const { return custom_color_.isValid(); }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    AidaSemantic kind_ = AidaSemantic::Neutral;
    QColor custom_color_;
};

}
