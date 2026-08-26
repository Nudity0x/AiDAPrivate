#include "aida_chip.hpp"
#include <algorithm>

#include <QEnterEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>

#include "../theme/aida_fonts.hpp"
#include "../theme/aida_motion.hpp"
#include "aida_paint_utils.hpp"

namespace aida::qt::widgets {

AidaChip::AidaChip(QWidget* parent)
    : QAbstractButton(parent)
{
    setObjectName(QStringLiteral("aida.chip"));
    chip_color_ = aida::qt::theme::tokens().accent;
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setFont(aida::qt::theme::fonts::body());
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);

    hover_anim_ = aida::qt::theme::motion::hover(this);
    if (hover_anim_) {
        connect(hover_anim_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) { hover_ = v.toReal(); update(); });
    }
}

AidaChip::AidaChip(const QString& label, QWidget* parent)
    : AidaChip(parent)
{
    setText(label);
}

void AidaChip::setChipColor(const QColor& color)
{
    if (chip_color_ == color)
        return;
    chip_color_ = color;
    update();
}

void AidaChip::setRemovable(bool removable)
{
    if (removable_ == removable)
        return;
    removable_ = removable;
    remove_armed_ = false;
    remove_hovered_ = false;
    updateGeometry();
    update();
}

qreal AidaChip::padX() const
{
    const auto& t = aida::qt::theme::tokens();
    return qreal(t.spacing.sm + t.spacing.xxs);
}

qreal AidaChip::focusInset() const
{
    const auto& t = aida::qt::theme::tokens();
    return qreal(t.control.focus_ring + t.panel.border);
}

qreal AidaChip::removeWidth() const
{
    return qreal(aida::qt::theme::tokens().spacing.lg);
}

QSize AidaChip::sizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    const QFontMetricsF fm(font());
    const int h = (std::max)(t.control.height_sm, qRound(fm.height() + qreal(t.spacing.md)));
    qreal w = fm.horizontalAdvance(text()) + padX() * 2.0;
    if (removable_)
        w += removeWidth();
    return QSize(qRound(w), h);
}

QSize AidaChip::minimumSizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(t.control.height_sm, t.control.height_sm);
}

QRectF AidaChip::removeZone() const
{
    const auto& t = aida::qt::theme::tokens();
    const qreal rw = removeWidth();
    return QRectF(rect().right() - rw - qreal(t.spacing.xs), rect().top(),
        rw + qreal(t.spacing.xs), rect().height());
}

void AidaChip::animateHoverTo(qreal target)
{
    if (!hover_anim_) {
        hover_ = target;
        update();
        return;
    }
    hover_anim_->stop();
    hover_anim_->setDuration(aida::qt::theme::AidaMotion::reducedMotion()
        ? 0 : aida::qt::theme::tokens().motion.fast);
    hover_anim_->setStartValue(hover_);
    hover_anim_->setEndValue(target);
    hover_anim_->start();
}

void AidaChip::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const auto& t = aida::qt::theme::tokens();
    const qreal alpha = isEnabled() ? 1.0 : qreal(t.disabled_alpha);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(font());

    const qreal inset = focusInset();
    const QRectF face = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    const qreal radius = face.height() * 0.5;
    const bool selected = isCheckable() && isChecked();

    qreal fill_a = semantic_fill_alpha() + 0.08 * hover_;
    qreal border_a = semantic_edge_alpha() + 0.15 * hover_;
    if (selected) {
        fill_a = 0.40 + 0.05 * hover_;
        border_a = 0.90;
    }

    p.setPen(Qt::NoPen);
    p.setBrush(with_alpha(chip_color_, clamp01(fill_a) * alpha));
    p.drawRoundedRect(face, radius, radius);
    paint_border(p, face, radius, with_alpha(chip_color_, clamp01(border_a) * alpha));

    if (hasFocus() && isEnabled())
        paint_focus_ring(p, face, radius, 0.85);

    const QFontMetricsF fm(font());
    const qreal pad_x = padX();
    qreal text_right = face.right() - pad_x;
    if (removable_)
        text_right -= removeWidth() - qreal(t.spacing.xs);
    const qreal text_avail = text_right - (face.left() + pad_x);
    if (text_avail > 0.0) {
        const QString drawn = fm.elidedText(text(), Qt::ElideRight, text_avail);
        p.setPen(with_alpha(mix_colors(chip_color_, t.text_primary, 0.12), alpha));
        p.drawText(QPointF(face.left() + pad_x, text_baseline_centered(face, fm)), drawn);
    }

    if (removable_) {
        const qreal xx = face.right() - qreal(t.spacing.sm);
        const qreal xy = face.center().y();
        const qreal xs = qreal(t.spacing.xs);
        if (remove_armed_ || remove_hovered_) {
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(chip_color_, (remove_armed_ ? 0.34 : 0.22) * alpha));
            p.drawEllipse(QPointF(xx, xy), xs * 1.5, xs * 1.5);
        }
        const qreal x_alpha = remove_armed_ ? 1.0
            : 0.7 + (remove_hovered_ ? 0.3 : hover_ * 0.3);
        QPen xpen(with_alpha(chip_color_, x_alpha * alpha),
            (std::max)(1.2, qreal(t.control.focus_ring) * 0.75));
        xpen.setCapStyle(Qt::RoundCap);
        p.setPen(xpen);
        p.drawLine(QPointF(xx - xs, xy - xs), QPointF(xx + xs, xy + xs));
        p.drawLine(QPointF(xx - xs, xy + xs), QPointF(xx + xs, xy - xs));
    }
}

void AidaChip::enterEvent(QEnterEvent* event)
{
    if (isEnabled())
        animateHoverTo(1.0);
    QAbstractButton::enterEvent(event);
}

void AidaChip::leaveEvent(QEvent* event)
{
    if (remove_hovered_) {
        remove_hovered_ = false;
        update();
    }
    animateHoverTo(0.0);
    QAbstractButton::leaveEvent(event);
}

void AidaChip::focusInEvent(QFocusEvent* event)
{
    update();
    QAbstractButton::focusInEvent(event);
}

void AidaChip::focusOutEvent(QFocusEvent* event)
{
    update();
    QAbstractButton::focusOutEvent(event);
}

void AidaChip::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::EnabledChange) {
        if (!isEnabled())
            animateHoverTo(0.0);
        update();
    } else if (event->type() == QEvent::FontChange) {
        updateGeometry();
        update();
    }
    QAbstractButton::changeEvent(event);
}

void AidaChip::keyPressEvent(QKeyEvent* event)
{
    if (removable_ && isEnabled()
        && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
        Q_EMIT removeRequested();
        event->accept();
        return;
    }
    QAbstractButton::keyPressEvent(event);
}

void AidaChip::mousePressEvent(QMouseEvent* event)
{
    if (removable_ && isEnabled() && event->button() == Qt::LeftButton
        && removeZone().contains(event->position())) {
        remove_armed_ = true;
        event->accept();
        return;
    }
    QAbstractButton::mousePressEvent(event);
}

void AidaChip::mouseMoveEvent(QMouseEvent* event)
{
    if (removable_ && isEnabled()) {
        const bool hot = removeZone().contains(event->position());
        if (hot != remove_hovered_) {
            remove_hovered_ = hot;
            update();
        }
    }
    QAbstractButton::mouseMoveEvent(event);
}

void AidaChip::mouseReleaseEvent(QMouseEvent* event)
{
    if (remove_armed_) {
        remove_armed_ = false;
        if (removable_ && isEnabled() && event->button() == Qt::LeftButton
            && removeZone().contains(event->position()))
            Q_EMIT removeRequested();
        update();
        event->accept();
        return;
    }
    QAbstractButton::mouseReleaseEvent(event);
}

}
