#include "aida_button.hpp"
#include <algorithm>

#include <QEnterEvent>
#include <QFocusEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>

#include "../theme/aida_fonts.hpp"
#include "../theme/aida_stylesheet.hpp"
#include "../theme/aida_motion.hpp"
#include "aida_paint_utils.hpp"

#include <cmath>

namespace aida::qt::widgets {

namespace {
    constexpr qreal kPi = 3.14159265358979323846;

    const char* variant_name(AidaButton::Kind kind)
    {
        switch (kind) {
        case AidaButton::Kind::Primary:        return "primary";
        case AidaButton::Kind::Secondary:      return "secondary";
        case AidaButton::Kind::Ghost:          return "ghost";
        case AidaButton::Kind::Destructive:    return "destructive";
        case AidaButton::Kind::AccentGradient: return "accent";
        }
        return "secondary";
    }
}

AidaButton::AidaButton(QWidget* parent)
    : QAbstractButton(parent)
{
    setObjectName(QStringLiteral("aida.button"));
    setCheckable(false);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setProperty("aidaVariant", variant_name(kind_));
    applySizeFont();

    hover_anim_ = aida::qt::theme::motion::hover(this);
    if (hover_anim_) {
        connect(hover_anim_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) { hover_ = v.toReal(); update(); });
    }
    press_anim_ = aida::qt::theme::motion::press(this);
    if (press_anim_) {
        connect(press_anim_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) { press_ = v.toReal(); update(); });
    }
    flash_anim_ = aida::qt::theme::motion::flash(this);
    if (flash_anim_) {
        connect(flash_anim_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) { flash_ = v.toReal(); update(); });
    }

    connect(this, &QAbstractButton::pressed, this, [this] { animatePressTo(1.0); });
    connect(this, &QAbstractButton::released, this, [this] { animatePressTo(0.0); });
    connect(this, &QAbstractButton::clicked, this, [this] { triggerFlash(); });
}

AidaButton::AidaButton(const QString& text, QWidget* parent)
    : AidaButton(parent)
{
    setText(text);
}

AidaButton::~AidaButton() = default;

void AidaButton::setKind(Kind kind)
{
    if (kind_ == kind)
        return;
    kind_ = kind;
    setProperty("aidaVariant", variant_name(kind_));
    aida::qt::theme::stylesheet::repolish(this);
    update();
}

void AidaButton::setControlSize(ControlSize size)
{
    if (control_size_ == size)
        return;
    control_size_ = size;
    applySizeFont();
    updateGeometry();
    update();
}

void AidaButton::setLoading(bool loading)
{
    if (loading_ == loading)
        return;
    loading_ = loading;
    if (loading_) {
        animateHoverTo(0.0);
        animatePressTo(0.0);
        if (aida::qt::theme::AidaMotion::reducedMotion()) {
            orbit_phase_ = 0.35;
        } else if (isVisible()) {
            startOrbit();
        }
    } else {
        stopOrbit();
        orbit_phase_ = 0.0;
    }
    update();
}

void AidaButton::triggerFlash()
{
    if (!flash_anim_)
        return;
    flash_anim_->stop();
    if (aida::qt::theme::AidaMotion::reducedMotion())
        flash_anim_->setDuration(0);
    flash_anim_->setStartValue(1.0);
    flash_anim_->setEndValue(0.0);
    flash_anim_->start();
}

void AidaButton::applySizeFont()
{
    switch (control_size_) {
    case ControlSize::Small:  setFont(aida::qt::theme::fonts::caption()); break;
    case ControlSize::Medium: setFont(aida::qt::theme::fonts::body()); break;
    case ControlSize::Large:  setFont(aida::qt::theme::fonts::large()); break;
    }
}

qreal AidaButton::controlHeight() const
{
    const auto& t = aida::qt::theme::tokens();
    switch (control_size_) {
    case ControlSize::Small:  return t.control.height_sm;
    case ControlSize::Medium: return t.control.height_md;
    case ControlSize::Large:  return t.control.height_lg;
    }
    return t.control.height_md;
}

qreal AidaButton::paddingX() const
{
    const auto& t = aida::qt::theme::tokens();
    switch (control_size_) {
    case ControlSize::Small:  return t.spacing.md;
    case ControlSize::Medium: return t.spacing.lg;
    case ControlSize::Large:  return t.spacing.xl;
    }
    return t.spacing.lg;
}

QRectF AidaButton::faceRect() const
{
    const qreal inset = qreal(aida::qt::theme::tokens().radius.xs);
    return QRectF(rect()).adjusted(inset, inset, -inset, -inset);
}

void AidaButton::animateHoverTo(qreal target)
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

void AidaButton::animatePressTo(qreal target)
{
    if (!press_anim_) {
        press_ = target;
        update();
        return;
    }
    press_anim_->stop();
    if (aida::qt::theme::AidaMotion::reducedMotion())
        press_anim_->setDuration(0);
    press_anim_->setStartValue(press_);
    press_anim_->setEndValue(target);
    press_anim_->start();
}

void AidaButton::startOrbit()
{
    if (!orbit_anim_) {
        orbit_anim_ = aida::qt::theme::motion::loop(
            aida::qt::theme::tokens().motion.xxl, this);
        if (orbit_anim_) {
            connect(orbit_anim_, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& v) { orbit_phase_ = v.toReal(); update(); });
        }
    }
    if (orbit_anim_)
        orbit_anim_->start();
}

void AidaButton::stopOrbit()
{
    if (orbit_anim_)
        orbit_anim_->stop();
}

QSize AidaButton::sizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    const int h = qRound(controlHeight());
    const bool icon_only = text().isEmpty() && !icon().isNull();
    if (icon_only) {
        const int side = qRound((std::max)(controlHeight(), qreal(t.control.icon_button)));
        return QSize(side, side);
    }
    const QFontMetricsF fm(font());
    qreal w = fm.horizontalAdvance(text());
    if (!icon().isNull())
        w += qreal(t.control.icon_glyph) + qreal(t.spacing.xs + t.spacing.xxs);
    w += paddingX() * 2.0 + qreal(t.spacing.xs);
    return QSize(qRound(w), h);
}

QSize AidaButton::minimumSizeHint() const
{
    const int h = qRound(controlHeight());
    return QSize(h, h);
}

void AidaButton::paintShell(QPainter& p, const QRectF& face)
{
    const auto& t = aida::qt::theme::tokens();
    const qreal alpha = isEnabled() ? 1.0 : qreal(t.disabled_alpha);
    const bool prim = kind_ == Kind::Primary || kind_ == Kind::AccentGradient;
    const bool checked_state = isCheckable() && isChecked();
    const qreal radius = prim ? qreal(t.radius.sm) : qreal(t.radius.md);

    QColor text_col;
    if (prim) {
        const qreal lift = (std::max)(hover_, checked_state ? 0.6 : 0.0);
        const QColor top = with_alpha(mix_colors(t.accent_grad_top, t.accent_hover, lift * 0.55), alpha);
        const QColor bot = with_alpha(mix_colors(t.accent_grad_bot, t.accent, lift * 0.35), alpha);
        QLinearGradient grad(face.topLeft(), face.bottomLeft());
        grad.setColorAt(0.0, top);
        grad.setColorAt(1.0, bot);
        p.setPen(Qt::NoPen);
        p.setBrush(grad);
        p.drawRoundedRect(face, radius, radius);

        QColor edge = t.accent_hover;
        edge.setAlphaF(clamp01((0.40 + lift * 0.40) * alpha));
        paint_border(p, face, radius, edge);

        text_col = t.text_on_accent;
    } else {
        QColor fill;
        QColor border;
        switch (kind_) {
        case Kind::Secondary:
            fill = lighten_color(t.panel_header, 5);
            border = t.border_strong;
            break;
        case Kind::Ghost:
            fill = t.bg_elevated;
            border = t.border_subtle;
            break;
        case Kind::Destructive:
            fill = with_alpha(t.error, 0.16);
            border = with_alpha(t.error, 0.55);
            break;
        default:
            fill = t.panel_header;
            border = t.border_subtle;
            break;
        }
        const qreal fill_blend = hover_ * 0.16 + (checked_state ? 0.16 : 0.0);
        const QColor blend_target = kind_ == Kind::Destructive ? t.error : t.accent;
        const QColor flat = mix_colors(fill, blend_target, fill_blend);
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(flat, alpha));
        p.drawRoundedRect(face, radius, radius);

        QColor top_hi = t.sheen;
        top_hi.setAlphaF(clamp01((0.05 + hover_ * 0.03) * alpha));
        QPen hi_pen(top_hi, qreal(t.panel.border));
        hi_pen.setCapStyle(Qt::FlatCap);
        p.setPen(hi_pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(face.left() + radius, face.top() + 1.0),
            QPointF(face.right() - radius, face.top() + 1.0));

        QColor edge = checked_state ? mix_colors(border, t.accent, 0.65) : border;
        edge.setAlphaF(clamp01(edge.alphaF()
            * (1.0 + hover_ * 0.35 + (checked_state ? 0.25 : 0.0)) * alpha));
        paint_border(p, face, radius, edge);

        text_col = kind_ == Kind::Destructive
            ? mix_colors(t.error, t.text_primary, 0.25 + hover_ * 0.15)
            : t.text_primary;
    }

    if (flash_ > 0.001) {
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(t.sheen, flash_ * 0.20 * alpha));
        p.drawRoundedRect(face, radius, radius);
    }

    if (hasFocus() && isEnabled())
        paint_focus_ring(p, face, radius, 0.85);

    p.setPen(with_alpha(text_col, alpha));
}

void AidaButton::paintContent(QPainter& p, const QRectF& face)
{
    const auto& t = aida::qt::theme::tokens();
    const qreal pad = paddingX();

    if (loading_) {
        const qreal orbit_r = face.height() * 0.18;
        const QPointF center = face.center();
        const qreal base_ang = orbit_phase_ * 2.0 * kPi;
        const qreal dot = qreal(t.spacing.xxs);
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 3; ++i) {
            const qreal ang = base_ang + qreal(i) * 2.094395;
            const qreal aa = (std::sin(ang * 0.8) * 0.5 + 0.5) * 0.6 + 0.4;
            const QPointF dp(center.x() + std::cos(ang) * orbit_r,
                center.y() + std::sin(ang) * orbit_r);
            p.setBrush(with_alpha(p.pen().color(), aa));
            p.drawEllipse(dp, dot, dot);
        }
        return;
    }

    const bool has_icon = !icon().isNull() && !icon_hidden_;
    const bool has_text = !text().isEmpty();
    const QFontMetricsF fm(font());
    const int glyph = t.control.icon_glyph;
    const qreal gap = (has_icon && has_text) ? qreal(t.spacing.xs + t.spacing.xxs) : 0.0;
    const qreal text_w = has_text ? fm.horizontalAdvance(text()) : 0.0;
    const qreal content_w = (has_icon ? qreal(glyph) : 0.0) + gap + text_w;
    const qreal slack = face.width() - content_w;
    qreal x = face.left() + pad;
    if (!has_text || slack >= pad * 2.0)
        x = face.center().x() - content_w * 0.5;

    if (has_icon) {
        const QPixmap pm = icon().pixmap(QSize(glyph, glyph), devicePixelRatioF(),
            isEnabled() ? QIcon::Normal : QIcon::Disabled);
        p.drawPixmap(QPointF(x, face.center().y() - qreal(glyph) * 0.5), pm);
        x += qreal(glyph) + gap;
    }
    if (has_text) {
        const qreal avail = face.right() - pad - x;
        if (avail > 0.0) {
            const QString drawn = fm.elidedText(text(), Qt::ElideRight, avail);
            p.drawText(QPointF(x, text_baseline_centered(face, fm)), drawn);
        }
    }
}

void AidaButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(font());

    const qreal scale = 1.0 - 0.015 * press_;
    const qreal lift = hover_ * 0.5 - press_ * 0.5;
    const QRectF face = faceRect();
    const qreal w = face.width();
    const qreal h = face.height();
    const QRectF cf(face.left() + (1.0 - scale) * w * 0.5,
        face.top() + (1.0 - scale) * h * 0.5 - lift,
        face.right() - (1.0 - scale) * w * 0.5,
        face.bottom() - (1.0 - scale) * h * 0.5 - lift);

    paintShell(p, cf);
    paintContent(p, cf);
}

void AidaButton::enterEvent(QEnterEvent* event)
{
    if (isEnabled() && !loading_)
        animateHoverTo(1.0);
    QAbstractButton::enterEvent(event);
}

void AidaButton::leaveEvent(QEvent* event)
{
    animateHoverTo(0.0);
    QAbstractButton::leaveEvent(event);
}

void AidaButton::focusInEvent(QFocusEvent* event)
{
    update();
    QAbstractButton::focusInEvent(event);
}

void AidaButton::focusOutEvent(QFocusEvent* event)
{
    update();
    QAbstractButton::focusOutEvent(event);
}

void AidaButton::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::EnabledChange) {
        if (!isEnabled()) {
            animateHoverTo(0.0);
            animatePressTo(0.0);
        }
        update();
    } else if (event->type() == QEvent::FontChange) {
        updateGeometry();
        update();
    }
    QAbstractButton::changeEvent(event);
}

void AidaButton::showEvent(QShowEvent* event)
{
    if (loading_ && !aida::qt::theme::AidaMotion::reducedMotion())
        startOrbit();
    QAbstractButton::showEvent(event);
}

void AidaButton::hideEvent(QHideEvent* event)
{
    stopOrbit();
    QAbstractButton::hideEvent(event);
}

void AidaButton::mousePressEvent(QMouseEvent* event)
{
    if (loading_) {
        event->accept();
        return;
    }
    QAbstractButton::mousePressEvent(event);
}

void AidaButton::mouseReleaseEvent(QMouseEvent* event)
{
    if (loading_) {
        event->accept();
        return;
    }
    QAbstractButton::mouseReleaseEvent(event);
}

void AidaButton::mouseMoveEvent(QMouseEvent* event)
{
    if (loading_) {
        event->accept();
        return;
    }
    QAbstractButton::mouseMoveEvent(event);
}

void AidaButton::keyPressEvent(QKeyEvent* event)
{
    if (loading_) {
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && !event->isAutoRepeat()) {
        click();
        return;
    }
    QAbstractButton::keyPressEvent(event);
}

void AidaButton::keyReleaseEvent(QKeyEvent* event)
{
    if (loading_) {
        event->accept();
        return;
    }
    QAbstractButton::keyReleaseEvent(event);
}

void AidaButton::syncAccessibleName()
{
    if (!accessibleName().isEmpty() || !text().isEmpty() || icon().isNull() || toolTip().isEmpty())
        return;
    setAccessibleName(toolTip());
}

bool AidaButton::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTipChange)
        syncAccessibleName();
    return QAbstractButton::event(event);
}

}
