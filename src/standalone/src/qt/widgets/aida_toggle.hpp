#pragma once

#include <QAbstractButton>

class QVariantAnimation;

namespace aida::qt::widgets {

class AidaToggleSwitch : public QAbstractButton
{
    Q_OBJECT
public:
    enum class Size { Small, Medium };

    explicit AidaToggleSwitch(QWidget* parent = nullptr);
    explicit AidaToggleSwitch(const QString& label, QWidget* parent = nullptr);

    void setSize(Size size);
    Size size() const { return size_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    qreal trackWidth() const;
    qreal trackHeight() const;
    QRectF trackRect() const;
    void animateTo(qreal target);
    void animateHoverTo(qreal target);
    void syncAccessibleName();

    Size size_ = Size::Small;
    QVariantAnimation* slide_anim_ = nullptr;
    QVariantAnimation* hover_anim_ = nullptr;
    qreal slide_ = 0.0;
    qreal hover_ = 0.0;
};

}
