#include "qt/overlays/aida_empty_state.hpp"

#include <QPainter>
#include <QHideEvent>
#include <QShowEvent>
#include <QPainterPath>
#include <QVariantAnimation>

#include <cmath>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_kbd_chip.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace aida::qt::overlays {

namespace {

using widgets::with_alpha;

void stroke_circle(QPainter& p, const QPointF& c, qreal r, const QColor& col, qreal th)
{
    p.setPen(QPen(col, th));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, r, r);
}

void fill_circle(QPainter& p, const QPointF& c, qreal r, const QColor& col)
{
    p.setPen(Qt::NoPen);
    p.setBrush(col);
    p.drawEllipse(c, r, r);
}

void stroke_line(QPainter& p, const QPointF& a, const QPointF& b, const QColor& col, qreal th)
{
    p.setPen(QPen(col, th, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(a, b);
}

void fill_rect(QPainter& p, const QRectF& r, const QColor& col, qreal radius)
{
    p.setPen(Qt::NoPen);
    p.setBrush(col);
    p.drawRoundedRect(r, radius, radius);
}

void stroke_rect(QPainter& p, const QRectF& r, const QColor& col, qreal radius, qreal th)
{
    p.setPen(QPen(col, th));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, radius, radius);
}

void glyph_dots(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal)
{
    const qreal r_outer = size * 0.42;
    stroke_circle(p, center, r_outer, with_alpha(col, alpha * 0.55), 1.4);
    for (int i = 0; i < 3; ++i)
        fill_circle(p, QPointF(center.x() + (i - 1) * (size * 0.18), center.y()),
                    size * 0.07, with_alpha(col, alpha * 0.85));
}

void glyph_binary_file(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal t)
{
    const qreal w = size * 0.7;
    const qreal h = size * 0.85;
    const QPointF a(center.x() - w * 0.5, center.y() - h * 0.5);
    const QPointF b(center.x() + w * 0.5, center.y() + h * 0.5);
    const qreal fold = w * 0.30;
    fill_rect(p, QRectF(a, QPointF(b.x() - fold, b.y())), with_alpha(col, alpha * 0.18), 6);
    fill_rect(p, QRectF(QPointF(b.x() - fold, a.y()), b), with_alpha(col, alpha * 0.12), 6);
    stroke_rect(p, QRectF(a, b), with_alpha(col, alpha * 0.7), 6, 1.5);
    for (int i = 0; i < 4; ++i) {
        const qreal y = a.y() + h * 0.30 + i * h * 0.13;
        const qreal pulse = std::sin(t * 1.4 + i * 0.7) * 0.5 + 0.5;
        const qreal lw = w * (0.45 + pulse * 0.35);
        stroke_line(p, QPointF(a.x() + 6, y), QPointF(a.x() + 6 + lw, y),
                    with_alpha(col, alpha * (0.4 + pulse * 0.4)), 1);
    }
}

void glyph_memory(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal t)
{
    const qreal w = size * 0.85;
    const qreal h = size * 0.55;
    const QPointF a(center.x() - w * 0.5, center.y() - h * 0.5);
    const QPointF b(center.x() + w * 0.5, center.y() + h * 0.5);
    fill_rect(p, QRectF(a, b), with_alpha(col, alpha * 0.15), size * 0.06);
    stroke_rect(p, QRectF(a, b), with_alpha(col, alpha * 0.7), size * 0.06, 1.5);
    const int slots = 4;
    for (int i = 0; i < slots; ++i) {
        const qreal x0 = a.x() + 6 + i * (w - 12) / slots;
        const qreal pulse = std::sin(t * 2.2 + i * 0.85) * 0.5 + 0.5;
        fill_rect(p, QRectF(QPointF(x0, a.y() + 4),
                            QPointF(x0 + (w - 12) / slots - 4, b.y() - 4)),
                  with_alpha(col, alpha * (0.25 + pulse * 0.55)), size * 0.04);
    }
    for (int leg = 0; leg < 6; ++leg) {
        const qreal x = a.x() + 6 + leg * (w - 12) / 5.0;
        stroke_line(p, QPointF(x, b.y()), QPointF(x, b.y() + 5),
                    with_alpha(col, alpha * 0.45), 1);
    }
}

void glyph_network(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal t)
{
    const qreal r = size * 0.45;
    stroke_circle(p, center, r, with_alpha(col, alpha * 0.6), 1.5);
    stroke_circle(p, center, r * 0.65, with_alpha(col, alpha * 0.45), 1);
    for (int i = 0; i < 6; ++i) {
        const qreal ang = t * 0.6 + i * 1.0471975;
        const QPointF pt(center.x() + std::cos(ang) * r, center.y() + std::sin(ang) * r);
        const qreal a = (std::sin(t * 1.5 + i) * 0.5 + 0.5) * 0.7 + 0.3;
        fill_circle(p, pt, 3.5, with_alpha(col, alpha * a));
    }
    fill_circle(p, center, 3, with_alpha(col, alpha));
}

void glyph_shield(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal t)
{
    const qreal w = size * 0.6;
    const qreal h = size * 0.78;
    const qreal breath = std::sin(t * 1.1) * 0.5 + 0.5;
    const qreal a_mod = 0.55 + breath * 0.35;
    const QPointF top(center.x(), center.y() - h * 0.5);
    const QPointF lt(center.x() - w * 0.5, center.y() - h * 0.3);
    const QPointF lb(center.x() - w * 0.35, center.y() + h * 0.3);
    const QPointF btm(center.x(), center.y() + h * 0.5);
    const QPointF rb(center.x() + w * 0.35, center.y() + h * 0.3);
    const QPointF rt(center.x() + w * 0.5, center.y() - h * 0.3);
    QPainterPath path;
    path.moveTo(top); path.lineTo(rt); path.lineTo(rb); path.lineTo(btm);
    path.lineTo(lb); path.lineTo(lt); path.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(with_alpha(col, alpha * a_mod * 0.18));
    p.drawPath(path);
    p.setPen(QPen(with_alpha(col, alpha * a_mod), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void glyph_message(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal)
{
    const qreal w = size * 0.78;
    const qreal h = size * 0.58;
    const QPointF a(center.x() - w * 0.5, center.y() - h * 0.5);
    const QPointF b(center.x() + w * 0.5, center.y() + h * 0.5);
    fill_rect(p, QRectF(a, b), with_alpha(col, alpha * 0.18), size * 0.10);
    stroke_rect(p, QRectF(a, b), with_alpha(col, alpha * 0.7), size * 0.10, 1.5);
    const QPointF tip0(center.x() - w * 0.18, b.y());
    const QPointF tip1(center.x() - w * 0.04, b.y() + size * 0.14);
    const QPointF tip2(center.x() + w * 0.06, b.y());
    QPainterPath tip;
    tip.moveTo(tip0); tip.lineTo(tip1); tip.lineTo(tip2); tip.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(with_alpha(col, alpha * 0.18));
    p.drawPath(tip);
    stroke_line(p, tip0, tip1, with_alpha(col, alpha * 0.7), 1.5);
    stroke_line(p, tip1, tip2, with_alpha(col, alpha * 0.7), 1.5);
    for (int i = 0; i < 3; ++i) {
        const qreal y = a.y() + h * (0.30 + i * 0.18);
        const qreal lw = w * (0.55 - i * 0.12);
        stroke_line(p, QPointF(a.x() + size * 0.10, y), QPointF(a.x() + size * 0.10 + lw, y),
                    with_alpha(col, alpha * 0.55), 1);
    }
}

void glyph_search(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal)
{
    const qreal r = size * 0.30;
    const QPointF c(center.x() - size * 0.05, center.y() - size * 0.05);
    stroke_circle(p, c, r, with_alpha(col, alpha * 0.7), 1.8);
    const qreal lx0 = c.x() + r * 0.7071;
    const qreal ly0 = c.y() + r * 0.7071;
    stroke_line(p, QPointF(lx0, ly0), QPointF(c.x() + r * 1.55, c.y() + r * 1.55),
                with_alpha(col, alpha * 0.85), 2.2);
}

void glyph_key(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal)
{
    const qreal r = size * 0.18;
    const QPointF hc(center.x() - size * 0.20, center.y());
    stroke_circle(p, hc, r, with_alpha(col, alpha * 0.8), 1.6);
    fill_circle(p, hc, r * 0.35, with_alpha(col, alpha * 0.6));
    const qreal sx0 = hc.x() + r;
    const qreal sx1 = center.x() + size * 0.40;
    stroke_line(p, QPointF(sx0, center.y()), QPointF(sx1, center.y()),
                with_alpha(col, alpha * 0.8), 1.8);
    stroke_line(p, QPointF(sx1 - size * 0.06, center.y()),
                QPointF(sx1 - size * 0.06, center.y() + size * 0.10),
                with_alpha(col, alpha * 0.8), 1.8);
    stroke_line(p, QPointF(sx1, center.y()), QPointF(sx1, center.y() + size * 0.14),
                with_alpha(col, alpha * 0.8), 1.8);
}

void glyph_layers(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal)
{
    const qreal w = size * 0.7;
    const qreal h = size * 0.18;
    for (int i = 0; i < 3; ++i) {
        const qreal y = center.y() - size * 0.2 + i * size * 0.18;
        const QRectF r(QPointF(center.x() - w * 0.5, y), QSizeF(w, h));
        const qreal la = 0.55 + i * 0.15;
        fill_rect(p, r, with_alpha(col, alpha * la * 0.3), 4);
        stroke_rect(p, r, with_alpha(col, alpha * la), 4, 1.2);
    }
}

void glyph_cpu(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal)
{
    const qreal w = size * 0.55;
    const QPointF a(center.x() - w * 0.5, center.y() - w * 0.5);
    const QPointF b(center.x() + w * 0.5, center.y() + w * 0.5);
    fill_rect(p, QRectF(a, b), with_alpha(col, alpha * 0.18), 4);
    stroke_rect(p, QRectF(a, b), with_alpha(col, alpha * 0.75), 4, 1.4);
    const qreal ic = size * 0.22;
    stroke_rect(p, QRectF(QPointF(center.x() - ic * 0.5, center.y() - ic * 0.5),
                          QPointF(center.x() + ic * 0.5, center.y() + ic * 0.5)),
                with_alpha(col, alpha * 0.6), 2, 1.2);
    for (int side = 0; side < 4; ++side) {
        for (int k = 0; k < 3; ++k) {
            const qreal t01 = 0.30 + k * 0.20;
            QPointF p0, p1;
            if (side == 0) { p0 = QPointF(a.x() + w * t01, a.y()); p1 = QPointF(a.x() + w * t01, a.y() - size * 0.07); }
            else if (side == 1) { p0 = QPointF(a.x() + w * t01, b.y()); p1 = QPointF(a.x() + w * t01, b.y() + size * 0.07); }
            else if (side == 2) { p0 = QPointF(a.x(), a.y() + w * t01); p1 = QPointF(a.x() - size * 0.07, a.y() + w * t01); }
            else { p0 = QPointF(b.x(), a.y() + w * t01); p1 = QPointF(b.x() + size * 0.07, a.y() + w * t01); }
            stroke_line(p, p0, p1, with_alpha(col, alpha * 0.65), 1.2);
        }
    }
}

void glyph_flask(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal t)
{
    const qreal neck_w = size * 0.20;
    const qreal neck_h = size * 0.30;
    const qreal body_w = size * 0.58;
    const qreal body_h = size * 0.46;
    const QPointF n0(center.x() - neck_w * 0.5, center.y() - size * 0.42);
    const QPointF n1(center.x() + neck_w * 0.5, center.y() - size * 0.10);
    const QPointF bl(center.x() - body_w * 0.5, center.y() + body_h * 0.38);
    const QPointF br(center.x() + body_w * 0.5, center.y() + body_h * 0.38);
    const QPointF sl(center.x() - body_w * 0.22, center.y() - body_h * 0.08);
    const QPointF sr(center.x() + body_w * 0.22, center.y() - body_h * 0.08);
    stroke_line(p, n0, QPointF(n0.x(), n0.y() + neck_h), with_alpha(col, alpha * 0.75), 1.5);
    stroke_line(p, n1, QPointF(n1.x(), n1.y()), with_alpha(col, alpha * 0.75), 1.5);
    stroke_line(p, QPointF(n0.x() - size * 0.07, n0.y()), QPointF(n1.x() + size * 0.07, n0.y()),
                with_alpha(col, alpha * 0.78), 1.5);
    QPainterPath body;
    body.moveTo(sl); body.lineTo(bl); body.lineTo(br); body.lineTo(sr); body.closeSubpath();
    p.setPen(QPen(with_alpha(col, alpha * 0.82), 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawPath(body);
    const qreal liquid_y = center.y() + body_h * 0.12;
    fill_rect(p, QRectF(QPointF(bl.x() + size * 0.08, liquid_y),
                        QPointF(br.x() - size * 0.08, br.y() - size * 0.06)),
              with_alpha(col, alpha * 0.18), size * 0.05);
    for (int i = 0; i < 3; ++i) {
        const qreal bx = center.x() - size * 0.18 + i * size * 0.18;
        const qreal by = center.y() - size * 0.06 + std::sin(t * 1.7 + i) * size * 0.025;
        fill_circle(p, QPointF(bx, by), size * 0.035, with_alpha(col, alpha * 0.45));
    }
}

void glyph_bug(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal)
{
    const qreal body_r = size * 0.22;
    const qreal head_r = size * 0.13;
    const QPointF body_c(center.x(), center.y() + size * 0.08);
    const QPointF head(center.x(), center.y() - size * 0.23);
    fill_circle(p, body_c, body_r, with_alpha(col, alpha * 0.18));
    stroke_circle(p, body_c, body_r, with_alpha(col, alpha * 0.78), 1.5);
    stroke_circle(p, head, head_r, with_alpha(col, alpha * 0.68), 1.4);
    stroke_line(p, QPointF(center.x(), body_c.y() - body_r), QPointF(center.x(), body_c.y() + body_r),
                with_alpha(col, alpha * 0.45), 1);
    for (int i = 0; i < 3; ++i) {
        const qreal y = body_c.y() - body_r * 0.55 + i * body_r * 0.55;
        const qreal span = size * (0.30 + (i == 1 ? 0.05 : 0.0));
        stroke_line(p, QPointF(center.x() - body_r * 0.76, y), QPointF(center.x() - span, y + size * 0.06),
                    with_alpha(col, alpha * 0.62), 1.4);
        stroke_line(p, QPointF(center.x() + body_r * 0.76, y), QPointF(center.x() + span, y + size * 0.06),
                    with_alpha(col, alpha * 0.62), 1.4);
    }
    stroke_line(p, QPointF(head.x() - head_r * 0.5, head.y() - head_r * 0.75),
                QPointF(head.x() - size * 0.20, head.y() - size * 0.32),
                with_alpha(col, alpha * 0.60), 1.2);
    stroke_line(p, QPointF(head.x() + head_r * 0.5, head.y() - head_r * 0.75),
                QPointF(head.x() + size * 0.20, head.y() - size * 0.32),
                with_alpha(col, alpha * 0.60), 1.2);
}

void glyph_flow(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal t)
{
    const QPointF nodes[4] = {
        QPointF(center.x() - size * 0.28, center.y() - size * 0.22),
        QPointF(center.x() + size * 0.26, center.y() - size * 0.10),
        QPointF(center.x() - size * 0.18, center.y() + size * 0.24),
        QPointF(center.x() + size * 0.30, center.y() + size * 0.30)
    };
    const int edges[4][2] = { {0, 1}, {0, 2}, {1, 3}, {2, 3} };
    for (int i = 0; i < 4; ++i) {
        const QPointF a = nodes[edges[i][0]];
        const QPointF b = nodes[edges[i][1]];
        const qreal pulse = std::sin(t * 1.5 + i * 0.7) * 0.5 + 0.5;
        stroke_line(p, a, b, with_alpha(col, alpha * (0.35 + pulse * 0.35)), 1.5);
    }
    for (int i = 0; i < 4; ++i) {
        const qreal r = (i == 0) ? size * 0.085 : size * 0.070;
        fill_circle(p, nodes[i], r + 2, with_alpha(col, alpha * 0.14));
        fill_circle(p, nodes[i], r, with_alpha(col, alpha * 0.72));
    }
}

void glyph_spark(QPainter& p, const QPointF& center, qreal size, const QColor& col, qreal alpha, qreal t)
{
    const qreal pulse = std::sin(t * 1.8) * 0.5 + 0.5;
    const qreal r1 = size * (0.34 + pulse * 0.025);
    const qreal r2 = size * 0.12;
    QPolygonF quad;
    quad << QPointF(center.x(), center.y() - r1) << QPointF(center.x() + r2, center.y())
         << QPointF(center.x(), center.y() + r1) << QPointF(center.x() - r2, center.y());
    p.setPen(Qt::NoPen);
    p.setBrush(with_alpha(col, alpha * 0.18));
    p.drawPolygon(quad);
    QPainterPath star;
    for (int i = 0; i < 8; ++i) {
        const qreal a = -1.5707963 + i * 0.7853982;
        const qreal r = (i % 2 == 0) ? r1 : r2;
        const QPointF pt(center.x() + std::cos(a) * r, center.y() + std::sin(a) * r);
        if (i == 0) star.moveTo(pt); else star.lineTo(pt);
    }
    star.closeSubpath();
    p.setPen(QPen(with_alpha(col, alpha * 0.78), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawPath(star);
    fill_circle(p, center, size * 0.045, with_alpha(col, alpha * 0.90));
}

}

void paintGlyph(QPainter& painter, AidaGlyph glyph, const QPointF& center, qreal size,
                const QColor& color, qreal alpha, qreal clock_seconds)
{
    switch (glyph) {
    case AidaGlyph::Dots:       glyph_dots(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::BinaryFile: glyph_binary_file(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Memory:     glyph_memory(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Network:    glyph_network(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Shield:     glyph_shield(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Key:        glyph_key(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Search:     glyph_search(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Flask:      glyph_flask(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Layers:     glyph_layers(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Cpu:        glyph_cpu(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Bug:        glyph_bug(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Flow:       glyph_flow(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Message:    glyph_message(painter, center, size, color, alpha, clock_seconds); break;
    case AidaGlyph::Spark:      glyph_spark(painter, center, size, color, alpha, clock_seconds); break;
    }
}

AidaEmptyGlyph::AidaEmptyGlyph(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.empty_glyph"));
    const auto& t = theme::tokens();
    const int glyph_px = t.control.icon_glyph * 4;
    setMinimumSize(glyph_px + 2 * t.spacing.xs, glyph_px + 2 * t.spacing.xs);
}

AidaEmptyGlyph::AidaEmptyGlyph(AidaGlyph glyph, QWidget* parent)
    : AidaEmptyGlyph(parent)
{
    glyph_ = glyph;
}

AidaEmptyGlyph::~AidaEmptyGlyph()
{
    if (ticker_) {
        ticker_->stop();
        ticker_ = nullptr;
    }
}

void AidaEmptyGlyph::setGlyph(AidaGlyph glyph)
{
    glyph_ = glyph;
    update();
}

QSize AidaEmptyGlyph::sizeHint() const
{
    const auto& t = theme::tokens();
    const int side = t.control.icon_glyph * 4 + 2 * t.spacing.xl;
    return QSize(side, side);
}

void AidaEmptyGlyph::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!ticker_ && !theme::AidaMotion::reducedMotion()) {
        ticker_ = new QVariantAnimation(this);
        ticker_->setStartValue(0.0);
        ticker_->setEndValue(1000.0);
        ticker_->setDuration(1000000);
        ticker_->setLoopCount(-1);
        ticker_->setEasingCurve(theme::easingFor(theme::Ease::Linear));
        connect(ticker_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            if (theme::AidaMotion::reducedMotion()) {
                if (ticker_) {
                    ticker_->stop();
                    ticker_->deleteLater();
                    ticker_ = nullptr;
                }
                update();
                return;
            }
            clock_ = v.toDouble();
            update();
        });
        ticker_->start();
    }
}

void AidaEmptyGlyph::hideEvent(QHideEvent* event)
{
    if (ticker_) {
        ticker_->stop();
        ticker_->deleteLater();
        ticker_ = nullptr;
    }
    QWidget::hideEvent(event);
}

void AidaEmptyGlyph::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    paintGlyph(p, glyph_, rect().center(),
               static_cast<qreal>(theme::tokens().control.icon_glyph * 4),
               theme::tokens().accent_dim, 1.0, clock_);
}

AidaEmptyState::AidaEmptyState(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.empty_state"));
}

AidaEmptyState::AidaEmptyState(const AidaEmptyStateConfig& config, QWidget* parent)
    : AidaEmptyState(parent)
{
    setConfig(config);
}

void AidaEmptyState::setConfig(const AidaEmptyStateConfig& config)
{
    config_ = config;
    rebuild();
}

void AidaEmptyState::rebuild()
{
    if (QLayout* old = layout()) {
        while (QLayoutItem* item = old->takeAt(0)) {
            if (QWidget* w = item->widget())
                w->deleteLater();
            delete item;
        }
        delete old;
    }

    const auto& t = theme::tokens();
    const int max_text_w = config_.max_width > 0
        ? config_.max_width
        : t.panel.overlay_margin * 10 + t.panel.header_h;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding,
                             t.panel.padding, t.panel.padding);
    root->setSpacing(t.spacing.sm);
    root->addStretch(1);

    auto* glyph = new AidaEmptyGlyph(config_.glyph, this);
    root->addWidget(glyph, 0, Qt::AlignHCenter);

    if (!config_.title.isEmpty()) {
        auto* title = new QLabel(config_.title, this);
        title->setObjectName(QStringLiteral("aida.empty_state.title"));
        title->setFont(theme::fonts::strong());
        title->setAlignment(Qt::AlignHCenter);
        title->setWordWrap(true);
        root->addWidget(title);
    }
    if (!config_.body.isEmpty()) {
        auto* body = new QLabel(config_.body, this);
        body->setObjectName(QStringLiteral("aida.empty_state.body"));
        body->setFont(theme::fonts::body());
        body->setAlignment(Qt::AlignHCenter);
        body->setWordWrap(true);
        body->setMaximumWidth(max_text_w);
        root->addWidget(body, 0, Qt::AlignHCenter);
    }
    if (!config_.footer.isEmpty()) {
        auto* footer = new QLabel(config_.footer, this);
        footer->setObjectName(QStringLiteral("aida.empty_state.footer"));
        footer->setFont(theme::fonts::caption());
        footer->setAlignment(Qt::AlignHCenter);
        footer->setWordWrap(true);
        footer->setMaximumWidth(max_text_w);
        root->addWidget(footer, 0, Qt::AlignHCenter);
    }
    if (!config_.actions.empty()) {
        auto* row = new QHBoxLayout();
        row->setSpacing(t.spacing.sm);
        row->addStretch(1);
        for (const auto& action : config_.actions) {
            auto* button = new widgets::AidaButton(action.label, this);
            if (!action.id.isEmpty())
                button->setObjectName(QStringLiteral("aida.empty_state.action.") + action.id);
            button->setKind(action.kind == 0
                ? widgets::AidaButton::Kind::Primary
                : action.kind == 2
                    ? widgets::AidaButton::Kind::Destructive
                    : widgets::AidaButton::Kind::Secondary);
            button->setControlSize(widgets::AidaButton::ControlSize::Small);
            button->setEnabled(!action.disabled);
            if (!action.tooltip.isEmpty())
                button->setToolTip(action.tooltip);
            const QString id = action.id;
            connect(button, &widgets::AidaButton::clicked, this, [this, id] {
                Q_EMIT actionTriggered(id);
            });
            row->addWidget(button);
        }
        row->addStretch(1);
        root->addLayout(row);
    }
    if (!config_.kbd_hints.isEmpty()) {
        auto* hints = new QHBoxLayout();
        hints->setSpacing(t.spacing.xs);
        hints->addStretch(1);
        for (const QString& hint : config_.kbd_hints)
            hints->addWidget(new widgets::AidaKbdChip(hint, this));
        hints->addStretch(1);
        root->addLayout(hints);
    }
    root->addStretch(1);
}

}
