#pragma once

#include <QAbstractButton>

class QVariantAnimation;

namespace aida::qt::widgets {

class AidaChip : public QAbstractButton
{
    Q_OBJECT
public:
    explicit AidaChip(QWidget* parent = nullptr);
    explicit AidaChip(const QString& label, QWidget* parent = nullptr);

    void setChipColor(const QColor& color);
    QColor chipColor() const { return chip_color_; }

    void setRemovable(bool removable);
    bool isRemovable() const { return removable_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void removeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QRectF removeZone() const;
    qreal padX() const;
    qreal focusInset() const;
    qreal removeWidth() const;
    void animateHoverTo(qreal target);

    QColor chip_color_;
    bool removable_ = false;
    bool remove_armed_ = false;
    bool remove_hovered_ = false;
    QVariantAnimation* hover_anim_ = nullptr;
    qreal hover_ = 0.0;
};

}
