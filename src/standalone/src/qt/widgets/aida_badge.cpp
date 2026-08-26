#include "aida_badge.hpp"
#include <algorithm>

#include <QEvent>
#include <QPainter>

#include "../theme/aida_fonts.hpp"
#include "../theme/aida_stylesheet.hpp"

namespace aida::qt::widgets {

AidaBadge::AidaBadge(QWidget* parent)
    : QLabel(parent)
{
    setObjectName(QStringLiteral("aida.badge"));
    setAlignment(Qt::AlignCenter);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setFont(aida::qt::theme::fonts::caption());
    setProperty("aidaVariant", QStringLiteral("neutral"));
}

AidaBadge::AidaBadge(const QString& text, AidaSemantic kind, QWidget* parent)
    : AidaBadge(parent)
{
    setKind(kind);
    setText(text);
}

void AidaBadge::setKind(AidaSemantic kind)
{
    if (kind_ == kind)
        return;
    kind_ = kind;
    if (!custom_color_.isValid())
        setProperty("aidaVariant", QString::fromLatin1(semantic_variant_name(kind_)));
    aida::qt::theme::stylesheet::repolish(this);
    updateGeometry();
    update();
}

void AidaBadge::setBadgeColor(const QColor& color)
{
    if (custom_color_ == color)
        return;
    custom_color_ = color;
    if (custom_color_.isValid())
        setProperty("aidaVariant", QVariant());
    aida::qt::theme::stylesheet::repolish(this);
    updateGeometry();
    update();
}

void AidaBadge::clearBadgeColor()
{
    if (!custom_color_.isValid())
        return;
    custom_color_ = QColor();
    setProperty("aidaVariant", QString::fromLatin1(semantic_variant_name(kind_)));
    aida::qt::theme::stylesheet::repolish(this);
    updateGeometry();
    update();
}

QSize AidaBadge::sizeHint() const
{
    if (!custom_color_.isValid())
        return QLabel::sizeHint();
    const auto& t = aida::qt::theme::tokens();
    const QFontMetricsF fm(font());
    return QSize(qRound(fm.horizontalAdvance(text()) + qreal(t.spacing.lg)),
        (std::max)(t.spacing.xl, qRound(fm.height() + qreal(t.spacing.sm))));
}

QSize AidaBadge::minimumSizeHint() const
{
    return sizeHint();
}

void AidaBadge::paintEvent(QPaintEvent* event)
{
    if (!custom_color_.isValid()) {
        QLabel::paintEvent(event);
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(font());
    const auto& t = aida::qt::theme::tokens();
    const qreal alpha = isEnabled() ? 1.0 : qreal(t.disabled_alpha);
    const QRectF r = QRectF(rect());
    p.setPen(Qt::NoPen);
    p.setBrush(with_alpha(custom_color_, 0.85 * alpha));
    p.drawRoundedRect(r, qreal(t.radius.xs), qreal(t.radius.xs));

    QColor edge = custom_color_;
    edge.setAlphaF(alpha);
    paint_border(p, r, qreal(t.radius.xs), edge);

    QColor text_col = with_alpha(t.text_primary, 0.94 * alpha);
    if (relative_luminance(custom_color_) > 0.7)
        text_col = with_alpha(t.bg_base, 0.94 * alpha);
    p.setPen(text_col);
    const QFontMetricsF fm(font());
    const qreal text_avail = r.width() - qreal(t.spacing.lg);
    if (text_avail > 0.0) {
        const QString drawn = fm.elidedText(text(), Qt::ElideRight, text_avail);
        const qreal tw = fm.horizontalAdvance(drawn);
        p.drawText(QPointF(r.left() + (r.width() - tw) * 0.5, text_baseline_centered(r, fm)), drawn);
    }
}

void AidaBadge::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange) {
        updateGeometry();
        update();
    }
    QLabel::changeEvent(event);
}

}
