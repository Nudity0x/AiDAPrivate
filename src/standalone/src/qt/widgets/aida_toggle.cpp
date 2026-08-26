#include "aida_toggle.hpp"
#include <algorithm>

#include <QEnterEvent>
#include <QFocusEvent>
#include <QPainter>
#include <QVariantAnimation>

#include "../theme/aida_fonts.hpp"
#include "../theme/aida_motion.hpp"
#include "aida_paint_utils.hpp"

namespace aida::qt::widgets {

AidaToggleSwitch::AidaToggleSwitch(QWidget* parent)
    : QAbstractButton(parent)
{
    setObjectName(QStringLiteral("aida.toggle"));
    setCheckable(true);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setFont(aida::qt::theme::fonts::body());

    slide_anim_ = new QVariantAnimation(this);
    slide_anim_->setDuration(aida::qt::theme::AidaMotion::reducedMotion()
        ? 0 : aida::qt::theme::tokens().motion.standard);
    slide_anim_->setEasingCurve(QEasingCurve::OutBack);
    connect(slide_anim_, &QVariantAnimation::valueChanged, this,
        [this](const QVariant& v) { slide_ = v.toReal(); update(); });

    hover_anim_ = aida::qt::theme::motion::hover(this);
    if (hover_anim_) {
        connect(hover_anim_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) { hover_ = v.toReal(); update(); });
    }

    connect(this, &QAbstractButton::toggled, this, [this](bool checked) {
        animateTo(checked ? 1.0 : 0.0);
    });
}

AidaToggleSwitch::AidaToggleSwitch(const QString& label, QWidget* parent)
    : AidaToggleSwitch(parent)
{
    setText(label);
}

void AidaToggleSwitch::setSize(Size size)
{
    if (size_ == size)
        return;
    size_ = size;
    updateGeometry();
    update();
}

qreal AidaToggleSwitch::trackWidth() const
{
    return trackHeight() * 2.0 - qreal(aida::qt::theme::tokens().spacing.xs);
}

qreal AidaToggleSwitch::trackHeight() const
{
    const auto& t = aida::qt::theme::tokens();
    const qreal base = size_ == Size::Small
        ? qreal(t.control.height_sm) : qreal(t.control.height_md);
    return base - qreal(t.spacing.xs) - qreal(t.radius.xs);
}

QRectF AidaToggleSwitch::trackRect() const
{
    const auto& t = aida::qt::theme::tokens();
    const qreal inset = qreal(t.radius.xs);
    const QRectF face = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    const qreal th = trackHeight();
    return QRectF(face.left() + qreal(t.spacing.xxs), face.center().y() - th * 0.5, trackWidth(), th);
}

void AidaToggleSwitch::animateTo(qreal target)
{
    slide_anim_->stop();
    slide_anim_->setDuration(aida::qt::theme::AidaMotion::reducedMotion()
        ? 0 : aida::qt::theme::tokens().motion.standard);
    slide_anim_->setStartValue(slide_);
    slide_anim_->setEndValue(target);
    slide_anim_->start();
}

void AidaToggleSwitch::animateHoverTo(qreal target)
{
    if (!hover_anim_) {
        hover_ = target;
        update();
        return;
    }
    hover_anim_->stop();
    if (aida::qt::theme::AidaMotion::reducedMotion())
        hover_anim_->setDuration(0);
    hover_anim_->setStartValue(hover_);
    hover_anim_->setEndValue(target);
    hover_anim_->start();
}

QSize AidaToggleSwitch::sizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    const QFontMetricsF fm(font());
    const bool has_label = !text().isEmpty();
    const qreal text_w = has_label ? fm.horizontalAdvance(text()) : 0.0;
    const qreal gap = has_label ? qreal(t.spacing.sm) : 0.0;
    const qreal w = trackWidth() + qreal(t.spacing.xs) + gap + text_w + qreal(t.radius.xs) * 2.0;
    const qreal h = (std::max)(trackHeight() + qreal(t.spacing.xs),
        fm.height() + qreal(t.spacing.xs)) + qreal(t.radius.xs) * 2.0;
    return QSize(qRound(w), qRound(h));
}

QSize AidaToggleSwitch::minimumSizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(qRound(trackWidth() + qreal(t.spacing.xs) + qreal(t.radius.xs) * 2.0),
        qRound(trackHeight() + qreal(t.spacing.xs) + qreal(t.radius.xs) * 2.0));
}

void AidaToggleSwitch::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const auto& t = aida::qt::theme::tokens();
    const qreal alpha = isEnabled() ? 1.0 : qreal(t.disabled_alpha);
    const qreal inset = qreal(t.radius.xs);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(font());

    const QRectF track = trackRect();
    const qreal clamped = clamp01(slide_);

    QColor track_col = mix_colors(t.panel_header, t.accent, clamped);
    track_col = mix_colors(track_col, t.accent_hover, hover_ * 0.30 * clamped);
    p.setPen(Qt::NoPen);
    p.setBrush(with_alpha(track_col, alpha));
    p.drawRoundedRect(track, track.height() * 0.5, track.height() * 0.5);

    QColor track_edge = mix_colors(t.border_subtle, with_alpha(t.accent_hover, 0.65), clamped);
    track_edge.setAlphaF(clamp01(track_edge.alphaF() * (1.0 + hover_ * 0.25) * alpha));
    paint_border(p, track, track.height() * 0.5, track_edge);

    const qreal knob_margin = qreal(t.spacing.xxs);
    const qreal knob_r = (track.height() - qreal(t.spacing.xs)) * 0.5;
    const qreal knob_x = track.left() + knob_margin + knob_r
        + (track.width() - knob_margin * 2.0 - knob_r * 2.0) * clamped;
    const qreal knob_y = track.center().y();
    p.setPen(Qt::NoPen);
    p.setBrush(with_alpha(t.text_on_accent, alpha));
    p.drawEllipse(QPointF(knob_x, knob_y), knob_r, knob_r);
    p.setPen(QPen(with_alpha(t.shade_shadow, 0.20 * alpha), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(knob_x, knob_y), knob_r - 0.5, knob_r - 0.5);

    if (!text().isEmpty()) {
        const QFontMetricsF fm(font());
        const QRectF face = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
        const qreal lx = track.right() + qreal(t.spacing.sm);
        const qreal avail = face.right() - lx;
        if (avail > 0.0) {
            p.setPen(with_alpha(t.text_primary, alpha));
            p.drawText(QPointF(lx, text_baseline_centered(face, fm)),
                fm.elidedText(text(), Qt::ElideRight, avail));
        }
    }

    if (hasFocus() && isEnabled()) {
        const QRectF face = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
        paint_focus_ring(p, face, qreal(t.radius.md), 0.85);
    }
}

void AidaToggleSwitch::enterEvent(QEnterEvent* event)
{
    if (isEnabled())
        animateHoverTo(1.0);
    QAbstractButton::enterEvent(event);
}

void AidaToggleSwitch::leaveEvent(QEvent* event)
{
    animateHoverTo(0.0);
    QAbstractButton::leaveEvent(event);
}

void AidaToggleSwitch::syncAccessibleName()
{
    if (!accessibleName().isEmpty() || !text().isEmpty() || toolTip().isEmpty())
        return;
    setAccessibleName(toolTip());
}

bool AidaToggleSwitch::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTipChange)
        syncAccessibleName();
    return QAbstractButton::event(event);
}

void AidaToggleSwitch::focusInEvent(QFocusEvent* event)
{
    update();
    QAbstractButton::focusInEvent(event);
}

void AidaToggleSwitch::focusOutEvent(QFocusEvent* event)
{
    update();
    QAbstractButton::focusOutEvent(event);
}

void AidaToggleSwitch::changeEvent(QEvent* event)
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

}
