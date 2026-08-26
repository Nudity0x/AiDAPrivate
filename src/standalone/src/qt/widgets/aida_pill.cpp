#include "aida_pill.hpp"
#include <algorithm>

#include <QEvent>
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

AidaPill::AidaPill(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.pill"));
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setFont(aida::qt::theme::fonts::caption());
    setProperty("aidaVariant", QStringLiteral("neutral"));
}

AidaPill::AidaPill(const QString& label, AidaSemantic kind, QWidget* parent)
    : AidaPill(parent)
{
    setKind(kind);
    setText(label);
}

void AidaPill::setText(const QString& text)
{
    if (text_ == text)
        return;
    text_ = text;
    updateGeometry();
    update();
}

void AidaPill::setKind(AidaSemantic kind)
{
    if (kind_ == kind)
        return;
    kind_ = kind;
    if (!custom_color_.isValid())
        setProperty("aidaVariant", QString::fromLatin1(semantic_variant_name(kind_)));
    aida::qt::theme::stylesheet::repolish(this);
    update();
}

void AidaPill::setCustomColor(const QColor& color)
{
    custom_color_ = color;
    if (custom_color_.isValid())
        setProperty("aidaVariant", QVariant());
    else
        setProperty("aidaVariant", QString::fromLatin1(semantic_variant_name(kind_)));
    aida::qt::theme::stylesheet::repolish(this);
    update();
}

void AidaPill::clearCustomColor()
{
    if (!custom_color_.isValid())
        return;
    custom_color_ = QColor();
    setProperty("aidaVariant", QString::fromLatin1(semantic_variant_name(kind_)));
    aida::qt::theme::stylesheet::repolish(this);
    update();
}

QColor AidaPill::effectiveColor() const
{
    return custom_color_.isValid() ? custom_color_ : semantic_color(kind_);
}

void AidaPill::setSize(Size size)
{
    if (size_ == size)
        return;
    size_ = size;
    setFont(size == Size::Small
        ? aida::qt::theme::fonts::caption() : aida::qt::theme::fonts::body());
    updateGeometry();
    update();
}

void AidaPill::setLeadingDotVisible(bool visible)
{
    if (leading_dot_ == visible)
        return;
    leading_dot_ = visible;
    if (leading_dot_ && isVisible() && !aida::qt::theme::AidaMotion::reducedMotion())
        startPulse();
    else
        stopPulse();
    updateGeometry();
    update();
}

qreal AidaPill::padX() const
{
    const auto& t = aida::qt::theme::tokens();
    return size_ == Size::Small
        ? qreal(t.spacing.sm + t.radius.xs)
        : qreal(t.spacing.md + t.panel.border);
}

qreal AidaPill::dotAdvance() const
{
    const auto& t = aida::qt::theme::tokens();
    return qreal(t.status_bar.dot + t.panel.border + t.spacing.xs);
}

qreal AidaPill::pillHeight() const
{
    const auto& t = aida::qt::theme::tokens();
    const QFontMetricsF fm(font());
    return size_ == Size::Small
        ? (std::max)(qreal(t.row.compact), fm.height() + qreal(t.spacing.sm + t.panel.border))
        : (std::max)(qreal(t.control.height_sm), fm.height() + qreal(t.spacing.md));
}

QSize AidaPill::sizeHint() const
{
    const QFontMetricsF fm(font());
    qreal w = fm.horizontalAdvance(text_) + padX() * 2.0;
    if (leading_dot_)
        w += dotAdvance();
    return QSize(qRound(w), qRound(pillHeight()));
}

QSize AidaPill::minimumSizeHint() const
{
    const QFontMetricsF fm(font());
    qreal w = padX() * 2.0 + fm.horizontalAdvance(QChar(0x2026));
    if (leading_dot_)
        w += dotAdvance();
    return QSize(qRound(w), qRound(pillHeight()));
}

void AidaPill::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const auto& t = aida::qt::theme::tokens();
    const qreal enabled_alpha = isEnabled() ? 1.0 : qreal(t.disabled_alpha);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(font());

    const QColor col = effectiveColor();
    const QRectF r = QRectF(rect());
    const qreal radius = r.height() * 0.5;

    p.setPen(Qt::NoPen);
    p.setBrush(with_alpha(col, semantic_fill_alpha() * enabled_alpha));
    p.drawRoundedRect(r, radius, radius);
    paint_border(p, r, radius, with_alpha(col, semantic_edge_alpha() * enabled_alpha));

    qreal x = r.left() + padX();
    if (leading_dot_) {
        const qreal dot_r = qreal(t.status_bar.dot + t.panel.border) * 0.5;
        qreal dot_alpha;
        if (!aida::qt::theme::AidaMotion::reducedMotion() && pulse_anim_
            && pulse_anim_->state() == QAbstractAnimation::Running)
            dot_alpha = (std::sin(pulse_phase_ * 2.0 * kPi) * 0.5 + 0.5) * 0.4 + 0.6;
        else
            dot_alpha = 0.8;
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(col, dot_alpha * enabled_alpha));
        p.drawEllipse(QPointF(x + dot_r, r.center().y()), dot_r, dot_r);
        x += dotAdvance();
    }

    const QFontMetricsF fm(font());
    const qreal text_avail = r.right() - padX() - x;
    if (text_avail > 0.0) {
        p.setPen(with_alpha(col, enabled_alpha));
        const QString drawn = fm.elidedText(text_, Qt::ElideRight, text_avail);
        p.drawText(QPointF(x, text_baseline_centered(r, fm)), drawn);
    }
}

void AidaPill::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange) {
        updateGeometry();
        update();
    }
    QWidget::changeEvent(event);
}

void AidaPill::showEvent(QShowEvent* event)
{
    if (leading_dot_ && !aida::qt::theme::AidaMotion::reducedMotion())
        startPulse();
    QWidget::showEvent(event);
}

void AidaPill::hideEvent(QHideEvent* event)
{
    stopPulse();
    QWidget::hideEvent(event);
}

void AidaPill::startPulse()
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

void AidaPill::stopPulse()
{
    if (pulse_anim_)
        pulse_anim_->stop();
}

}
