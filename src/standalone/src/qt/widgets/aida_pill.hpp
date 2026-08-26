#pragma once

#include <QWidget>

#include "aida_paint_utils.hpp"

class QVariantAnimation;

namespace aida::qt::widgets {

class AidaPill : public QWidget
{
    Q_OBJECT
public:
    enum class Size { Small, Medium };

    explicit AidaPill(QWidget* parent = nullptr);
    explicit AidaPill(const QString& label, AidaSemantic kind = AidaSemantic::Neutral,
        QWidget* parent = nullptr);

    void setText(const QString& text);
    QString text() const { return text_; }

    void setKind(AidaSemantic kind);
    AidaSemantic kind() const { return kind_; }

    void setCustomColor(const QColor& color);
    void clearCustomColor();
    QColor effectiveColor() const;

    void setSize(Size size);
    Size size() const { return size_; }

    void setLeadingDotVisible(bool visible);
    bool isLeadingDotVisible() const { return leading_dot_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void startPulse();
    void stopPulse();
    qreal padX() const;
    qreal dotAdvance() const;
    qreal pillHeight() const;

    QString text_;
    AidaSemantic kind_ = AidaSemantic::Neutral;
    QColor custom_color_;
    Size size_ = Size::Small;
    bool leading_dot_ = true;
    QVariantAnimation* pulse_anim_ = nullptr;
    qreal pulse_phase_ = 0.6;
};

}
