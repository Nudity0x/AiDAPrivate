#include "aida_copy_button.hpp"

#include <QEasingCurve>
#include <QPainter>

#include "../theme/aida_motion.hpp"
#include "aida_paint_utils.hpp"

namespace aida::qt::widgets {

AidaCopyButton::AidaCopyButton(QWidget* parent)
    : AidaButton(parent)
{
    setObjectName(QStringLiteral("aida.copy_button"));
    setKind(Kind::Ghost);
    setControlSize(ControlSize::Small);
    setIcon(QIcon(QStringLiteral(":/icons/copy.svg")));
    setToolTip(QStringLiteral("Copy"));
}

void AidaCopyButton::flashCopied()
{
    triggerFlash();
}

void AidaCopyButton::paintCheck(QPainter& p, const QRectF& face, qreal t01)
{
    if (t01 <= 0.0)
        return;
    const auto& t = aida::qt::theme::tokens();
    const qreal alpha = isEnabled() ? 1.0 : qreal(t.disabled_alpha);
    const QEasingCurve ease(QEasingCurve::OutQuint);
    const qreal eased = ease.valueForProgress(clamp01(t01));
    const qreal size = qreal(t.spacing.sm + t.spacing.xxs);
    const QPointF center = face.center();
    const QPointF a(center.x() - size * 0.45, center.y());
    const QPointF b(center.x() - size * 0.10, center.y() + size * 0.30);
    const QPointF c(center.x() + size * 0.50, center.y() - size * 0.35);
    const qreal pa = eased < 0.5 ? eased * 2.0 : 1.0;
    const qreal pb = eased < 0.5 ? 0.0 : (eased - 0.5) * 2.0;
    QPen pen(with_alpha(t.success, alpha), (std::max)(1.6, qreal(t.control.focus_ring)));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const QPointF ab(a.x() + (b.x() - a.x()) * pa, a.y() + (b.y() - a.y()) * pa);
    p.drawLine(a, ab);
    if (pb > 0.0) {
        const QPointF bc(b.x() + (c.x() - b.x()) * pb, b.y() + (c.y() - b.y()) * pb);
        p.drawLine(b, bc);
    }
}

void AidaCopyButton::paintEvent(QPaintEvent* event)
{
    const qreal copied = flashValue();
    const bool show_check = copied > 0.001;
    if (show_check)
        setContentIconHidden(true);
    AidaButton::paintEvent(event);
    if (show_check) {
        setContentIconHidden(false);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal scale = 1.0 - 0.015 * pressValue();
        const qreal lift = hoverValue() * 0.5 - pressValue() * 0.5;
        const QRectF face = faceRect();
        const qreal w = face.width();
        const qreal h = face.height();
        const QRectF cf(face.left() + (1.0 - scale) * w * 0.5,
            face.top() + (1.0 - scale) * h * 0.5 - lift,
            face.right() - (1.0 - scale) * w * 0.5,
            face.bottom() - (1.0 - scale) * h * 0.5 - lift);
        paintCheck(p, cf, 1.0 - copied);
    }
}

}
