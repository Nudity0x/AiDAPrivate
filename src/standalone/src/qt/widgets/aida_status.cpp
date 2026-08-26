#include "aida_status.hpp"
#include <algorithm>

#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>

#include "../theme/aida_fonts.hpp"
#include "../theme/aida_stylesheet.hpp"
#include "../theme/aida_motion.hpp"

#include <cmath>

namespace aida::qt::widgets {

namespace {
    constexpr qreal kPi = 3.14159265358979323846;
    constexpr int kPulsePeriodMs = 714;
}

AidaStatusDot::AidaStatusDot(QWidget* parent)
    : QWidget(parent)
    , dot_radius_(qreal(aida::qt::theme::tokens().status_bar.dot) * 0.5)
{
    setObjectName(QStringLiteral("aida.status_dot"));
    setFocusPolicy(Qt::NoFocus);
    setProperty("aidaVariant", QStringLiteral("neutral"));
}

AidaStatusDot::AidaStatusDot(AidaSemantic kind, QWidget* parent)
    : AidaStatusDot(parent)
{
    setKind(kind);
}

void AidaStatusDot::setKind(AidaSemantic kind)
{
    if (kind_ == kind)
        return;
    kind_ = kind;
    if (!custom_color_.isValid())
        setProperty("aidaVariant", QString::fromLatin1(semantic_variant_name(kind_)));
    setAccessibleName(QStringLiteral("Status: %1")
        .arg(QString::fromLatin1(semantic_variant_name(kind_))));
    aida::qt::theme::stylesheet::repolish(this);
    update();
}

void AidaStatusDot::setDotColor(const QColor& color)
{
    custom_color_ = color;
    if (custom_color_.isValid())
        setProperty("aidaVariant", QVariant());
    else
        setProperty("aidaVariant", QString::fromLatin1(semantic_variant_name(kind_)));
    aida::qt::theme::stylesheet::repolish(this);
    update();
}

QColor AidaStatusDot::dotColor() const
{
    return custom_color_.isValid() ? custom_color_ : semantic_color(kind_);
}

void AidaStatusDot::setPulsing(bool pulsing)
{
    if (pulsing_ == pulsing)
        return;
    pulsing_ = pulsing;
    if (pulsing_ && isVisible() && !aida::qt::theme::AidaMotion::reducedMotion())
        startPulse();
    else
        stopPulse();
    update();
}

void AidaStatusDot::setDotRadius(qreal radius)
{
    if (dot_radius_ == radius)
        return;
    dot_radius_ = radius;
    updateGeometry();
    update();
}

QSize AidaStatusDot::sizeHint() const
{
    const int side = qRound((dot_radius_ + qreal(aida::qt::theme::tokens().radius.xs)) * 2.0);
    return QSize(side, side);
}

QSize AidaStatusDot::minimumSizeHint() const
{
    return sizeHint();
}

void AidaStatusDot::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const auto& t = aida::qt::theme::tokens();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor col = dotColor();
    const QPointF center = QRectF(rect()).center();
    const qreal r = dot_radius_;
    const qreal border_w = qreal(t.panel.border);

    const bool animating = pulsing_ && !aida::qt::theme::AidaMotion::reducedMotion()
        && pulse_anim_ && pulse_anim_->state() == QAbstractAnimation::Running;
    if (animating) {
        const qreal wave = std::sin(pulse_phase_ * 2.0 * kPi);
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(col, 0.18));
        p.drawEllipse(center, r + qreal(t.spacing.xxs) + wave * border_w,
            r + qreal(t.spacing.xxs) + wave * border_w);
        p.setBrush(with_alpha(col, 0.55));
        p.drawEllipse(center, r + border_w, r + border_w);
    } else if (pulsing_) {
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(col, 0.18));
        p.drawEllipse(center, r + qreal(t.spacing.xxs), r + qreal(t.spacing.xxs));
        p.setBrush(with_alpha(col, 0.55));
        p.drawEllipse(center, r + border_w, r + border_w);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(col);
    p.drawEllipse(center, r, r);
}

void AidaStatusDot::showEvent(QShowEvent* event)
{
    if (pulsing_ && !aida::qt::theme::AidaMotion::reducedMotion())
        startPulse();
    QWidget::showEvent(event);
}

void AidaStatusDot::hideEvent(QHideEvent* event)
{
    stopPulse();
    QWidget::hideEvent(event);
}

void AidaStatusDot::startPulse()
{
    if (!pulse_anim_) {
        pulse_anim_ = aida::qt::theme::motion::loop(kPulsePeriodMs, this);
        if (pulse_anim_) {
            connect(pulse_anim_, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& v) {
                    if (aida::qt::theme::AidaMotion::reducedMotion()) {
                        stopPulse();
                        update();
                        return;
                    }
                    pulse_phase_ = v.toReal();
                    update();
                });
        }
    }
    if (pulse_anim_)
        pulse_anim_->start();
}

void AidaStatusDot::stopPulse()
{
    if (pulse_anim_)
        pulse_anim_->stop();
}

AidaStatusItem::AidaStatusItem(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.status_item"));
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setFont(aida::qt::theme::fonts::caption());
}

AidaStatusItem::AidaStatusItem(const QString& label, const QString& value,
    AidaSemantic kind, QWidget* parent)
    : AidaStatusItem(parent)
{
    label_ = label;
    value_ = value;
    kind_ = kind;
}

void AidaStatusItem::syncAccessibleName()
{
    setAccessibleName(value_.isEmpty()
        ? label_ : label_ + QStringLiteral(": ") + value_);
}

void AidaStatusItem::setLabel(const QString& label)
{
    if (label_ == label)
        return;
    label_ = label;
    syncAccessibleName();
    updateGeometry();
    update();
}

void AidaStatusItem::setValue(const QString& value)
{
    if (value_ == value)
        return;
    value_ = value;
    syncAccessibleName();
    updateGeometry();
    update();
}

void AidaStatusItem::setKind(AidaSemantic kind)
{
    if (kind_ == kind)
        return;
    kind_ = kind;
    updateGeometry();
    update();
}

void AidaStatusItem::setInteractive(bool interactive)
{
    if (interactive_ == interactive)
        return;
    interactive_ = interactive;
    setFocusPolicy(interactive_ ? Qt::TabFocus : Qt::NoFocus);
    setCursor(interactive_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

void AidaStatusItem::setSeparatorVisible(bool visible)
{
    if (separator_visible_ == visible)
        return;
    separator_visible_ = visible;
    updateGeometry();
    update();
}

QSize AidaStatusItem::sizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    const QFontMetricsF fm(font());
    const qreal pad = qreal(t.spacing.xs + t.panel.border);
    qreal w = fm.horizontalAdvance(label_) + pad * 2.0;
    if (!value_.isEmpty())
        w += fm.horizontalAdvance(value_) + pad;
    if (kind_ != AidaSemantic::Neutral)
        w += qreal(t.status_bar.dot + t.spacing.xs + t.panel.border);
    if (separator_visible_)
        w += qreal(t.status_bar.item_gap) * 2.0 + qreal(t.panel.border);
    const qreal h = (std::max)(qreal(t.spacing.xl), fm.height() + qreal(t.spacing.xs));
    return QSize(qRound(w), qRound(h));
}

QSize AidaStatusItem::minimumSizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    const QFontMetricsF fm(font());
    const qreal pad = qreal(t.spacing.xs + t.panel.border);
    const qreal ell = fm.horizontalAdvance(QChar(0x2026));
    qreal w = pad * 2.0 + ell;
    if (!value_.isEmpty())
        w += pad + ell;
    if (kind_ != AidaSemantic::Neutral)
        w += qreal(t.status_bar.dot + t.spacing.xs + t.panel.border);
    if (separator_visible_)
        w += qreal(t.status_bar.item_gap) * 2.0 + qreal(t.panel.border);
    const qreal h = (std::max)(qreal(t.spacing.xl), fm.height() + qreal(t.spacing.xs));
    return QSize(qRound(w), qRound(h));
}

void AidaStatusItem::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const auto& t = aida::qt::theme::tokens();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(font());

    const QRectF r = QRectF(rect());
    const qreal pad = qreal(t.spacing.xs + t.panel.border);
    qreal sep_w = 0.0;
    if (separator_visible_)
        sep_w = qreal(t.status_bar.item_gap) * 2.0 + qreal(t.panel.border);
    const QRectF body(r.left(), r.top(), r.width() - sep_w, r.height());

    if (interactive_ && hovered_) {
        p.setPen(Qt::NoPen);
        p.setBrush(t.hover_wash);
        p.drawRoundedRect(body, qreal(t.radius.xs), qreal(t.radius.xs));
    }

    if (interactive_ && hasFocus())
        paint_focus_ring_inner(p, body, qreal(t.radius.xs), 0.85);

    const QFontMetricsF fm(font());
    const qreal baseline = text_baseline_centered(body, fm);
    qreal x = body.left() + pad;
    if (kind_ != AidaSemantic::Neutral) {
        const qreal dot_r = qreal(t.status_bar.dot) * 0.5;
        p.setPen(Qt::NoPen);
        p.setBrush(semantic_color(kind_));
        p.drawEllipse(QPointF(x + dot_r + qreal(t.panel.border) * 0.5, body.center().y()),
            dot_r, dot_r);
        x += qreal(t.status_bar.dot + t.spacing.xs + t.panel.border);
    }

    const qreal avail = body.right() - pad - x;
    if (avail > 0.0) {
        const qreal ell_w = fm.horizontalAdvance(QChar(0x2026));
        qreal value_budget = value_.isEmpty() ? 0.0 : fm.horizontalAdvance(value_);
        qreal label_budget = fm.horizontalAdvance(label_);
        const qreal total = label_budget + (value_budget > 0.0 ? pad + value_budget : 0.0);
        if (total > avail) {
            if (value_budget > 0.0)
                value_budget = (std::max)(ell_w,
                    (std::min)(value_budget, avail * 0.5 - pad));
            label_budget = (std::max)(ell_w,
                avail - (value_budget > 0.0 ? pad + value_budget : 0.0));
        }
        p.setPen(t.text_secondary);
        const QString label_drawn = fm.elidedText(label_, Qt::ElideRight, label_budget);
        p.drawText(QPointF(x, baseline), label_drawn);
        x += fm.horizontalAdvance(label_drawn);
        if (!value_.isEmpty()) {
            p.setPen(t.text_primary);
            p.drawText(QPointF(x + pad, baseline),
                fm.elidedText(value_, Qt::ElideRight, value_budget));
        }
    }

    if (separator_visible_) {
        QPen sep_pen(t.border_subtle, qreal(t.panel.border));
        sep_pen.setCosmetic(true);
        sep_pen.setCapStyle(Qt::FlatCap);
        p.setPen(sep_pen);
        const qreal sx = body.right() + qreal(t.status_bar.item_gap) + 0.5;
        const qreal half = qreal(t.spacing.md + t.spacing.xxs) * 0.5;
        p.drawLine(QPointF(sx, body.center().y() - half), QPointF(sx, body.center().y() + half));
    }
}

void AidaStatusItem::enterEvent(QEnterEvent* event)
{
    if (interactive_) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void AidaStatusItem::leaveEvent(QEvent* event)
{
    if (hovered_) {
        hovered_ = false;
        if (interactive_)
            update();
    }
    QWidget::leaveEvent(event);
}

void AidaStatusItem::keyPressEvent(QKeyEvent* event)
{
    if (interactive_
        && (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter)) {
        Q_EMIT clicked();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void AidaStatusItem::mousePressEvent(QMouseEvent* event)
{
    if (interactive_ && event->button() == Qt::LeftButton) {
        armed_ = true;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void AidaStatusItem::mouseReleaseEvent(QMouseEvent* event)
{
    if (armed_) {
        armed_ = false;
        if (interactive_ && event->button() == Qt::LeftButton && rect().contains(event->pos()))
            Q_EMIT clicked();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void AidaStatusItem::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange) {
        updateGeometry();
        update();
    }
    QWidget::changeEvent(event);
}

}
