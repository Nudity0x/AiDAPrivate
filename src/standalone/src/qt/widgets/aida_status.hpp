#pragma once

#include <QWidget>

#include "aida_paint_utils.hpp"

class QVariantAnimation;

namespace aida::qt::widgets {

class AidaStatusDot : public QWidget
{
    Q_OBJECT
public:
    explicit AidaStatusDot(QWidget* parent = nullptr);
    explicit AidaStatusDot(AidaSemantic kind, QWidget* parent = nullptr);

    void setKind(AidaSemantic kind);
    AidaSemantic kind() const { return kind_; }

    void setDotColor(const QColor& color);
    QColor dotColor() const;

    void setPulsing(bool pulsing);
    bool isPulsing() const { return pulsing_; }

    void setDotRadius(qreal radius);
    qreal dotRadius() const { return dot_radius_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void startPulse();
    void stopPulse();

    AidaSemantic kind_ = AidaSemantic::Neutral;
    QColor custom_color_;
    bool pulsing_ = true;
    qreal dot_radius_;
    QVariantAnimation* pulse_anim_ = nullptr;
    qreal pulse_phase_ = 0.0;
};

class AidaStatusItem : public QWidget
{
    Q_OBJECT
public:
    explicit AidaStatusItem(QWidget* parent = nullptr);
    explicit AidaStatusItem(const QString& label, const QString& value = QString(),
        AidaSemantic kind = AidaSemantic::Neutral, QWidget* parent = nullptr);

    void setLabel(const QString& label);
    QString label() const { return label_; }

    void setValue(const QString& value);
    QString value() const { return value_; }

    void setKind(AidaSemantic kind);
    AidaSemantic kind() const { return kind_; }

    void setInteractive(bool interactive);
    bool isInteractive() const { return interactive_; }

    void setSeparatorVisible(bool visible);
    bool isSeparatorVisible() const { return separator_visible_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void syncAccessibleName();

    QString label_;
    QString value_;
    AidaSemantic kind_ = AidaSemantic::Neutral;
    bool interactive_ = false;
    bool separator_visible_ = true;
    bool hovered_ = false;
    bool armed_ = false;
};

}
