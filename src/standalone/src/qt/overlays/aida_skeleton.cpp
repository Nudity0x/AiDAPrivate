#include "qt/overlays/aida_skeleton.hpp"

#include <QPainter>
#include <QLinearGradient>
#include <QHideEvent>
#include <QShowEvent>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::overlays {

namespace {
constexpr int k_sweep_ms = 1500;
}

AidaSkeletonBlock::AidaSkeletonBlock(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.skeleton"));
    setAccessibleName(QStringLiteral("Content is loading"));
}

AidaSkeletonBlock::AidaSkeletonBlock(Kind kind, QWidget* parent)
    : AidaSkeletonBlock(parent)
{
    kind_ = kind;
}

AidaSkeletonBlock::~AidaSkeletonBlock()
{
    stopSweep();
}

void AidaSkeletonBlock::setKind(Kind kind)
{
    kind_ = kind;
    update();
}

void AidaSkeletonBlock::setLineCount(int lines)
{
    line_count_ = (std::max)(1, (std::min)(lines, 8));
    update();
}

void AidaSkeletonBlock::setTableShape(int columns, int rows)
{
    table_columns_ = (std::max)(1, columns);
    table_rows_ = (std::max)(1, rows);
    update();
}

void AidaSkeletonBlock::setRadius(qreal radius)
{
    radius_ = radius;
    update();
}

void AidaSkeletonBlock::setSweepEnabled(bool enabled)
{
    sweep_enabled_ = enabled;
    if (!enabled)
        stopSweep();
    else if (isVisible())
        startSweep();
    update();
}

QSize AidaSkeletonBlock::sizeHint() const
{
    const auto& t = theme::tokens();
    switch (kind_) {
    case Kind::TextLine:
        return QSize(t.panel.overlay_margin * 5, t.spacing.md);
    case Kind::Paragraph:
        return QSize(t.shell.min_panel_w * 3 - t.spacing.sm,
            t.panel.header_h * 2 + t.spacing.xl);
    case Kind::Avatar:
        return QSize(t.control.height_sm, t.control.height_sm);
    case Kind::Card:
        return QSize(t.shell.min_panel_w * 3 - t.spacing.sm, t.panel.header_h * 3);
    case Kind::TableRows:
        return QSize(t.shell.min_panel_w * 4 + t.spacing.section + t.spacing.xs,
            t.panel.header_h * 5);
    case Kind::Block: break;
    }
    return QSize(t.shell.min_panel_w * 2 + t.spacing.sm, t.panel.header_h);
}

QSize AidaSkeletonBlock::minimumSizeHint() const
{
    const auto& t = theme::tokens();
    return QSize(t.row.compact, t.spacing.md);
}

void AidaSkeletonBlock::startSweep()
{
    if (sweep_ || !sweep_enabled_ || theme::AidaMotion::reducedMotion())
        return;
    sweep_ = new QVariantAnimation(this);
    sweep_->setStartValue(0.0);
    sweep_->setEndValue(1.0);
    sweep_->setDuration(k_sweep_ms);
    sweep_->setEasingCurve(theme::easingFor(theme::Ease::Linear));
    sweep_->setLoopCount(-1);
    connect(sweep_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        if (theme::AidaMotion::reducedMotion()) {
            stopSweep();
            update();
            return;
        }
        phase_ = v.toDouble();
        update();
    });
    sweep_->start();
}

void AidaSkeletonBlock::stopSweep()
{
    if (sweep_) {
        sweep_->stop();
        sweep_->deleteLater();
        sweep_ = nullptr;
    }
}

void AidaSkeletonBlock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    startSweep();
}

void AidaSkeletonBlock::hideEvent(QHideEvent* event)
{
    stopSweep();
    QWidget::hideEvent(event);
}

void AidaSkeletonBlock::paintBlock(QPainter& painter, const QRectF& rect, qreal radius)
{
    const auto& t = theme::tokens();
    const QColor base = widgets::with_alpha(t.panel_header, 0.85);
    painter.setPen(Qt::NoPen);
    painter.setBrush(base);
    painter.drawRoundedRect(rect, radius, radius);

    if (phase_ <= 0.0 || !sweep_enabled_ || theme::AidaMotion::reducedMotion())
        return;

    const qreal w = rect.width();
    const qreal sweep_x = rect.left() - w * 0.4 + (w * 1.4) * phase_;
    const qreal sweep_w = w * 0.35;
    const QColor sweep_col = widgets::with_alpha(t.accent_dim, 0.55);

    painter.save();
    painter.setClipRect(rect);
    QLinearGradient gradient(QPointF(sweep_x, rect.top()),
                             QPointF(sweep_x + sweep_w, rect.top()));
    gradient.setColorAt(0.0, Qt::transparent);
    gradient.setColorAt(0.5, sweep_col);
    gradient.setColorAt(1.0, Qt::transparent);
    painter.setBrush(gradient);
    painter.drawRect(QRectF(sweep_x, rect.top(), sweep_w, rect.height()));
    painter.restore();
}

void AidaSkeletonBlock::paintTextLine(QPainter& painter, const QPointF& origin,
                                      qreal width, qreal height)
{
    paintBlock(painter, QRectF(origin, QSizeF(width, height)), height * 0.5);
}

void AidaSkeletonBlock::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const auto& t = theme::tokens();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = rect();

    switch (kind_) {
    case Kind::Block:
        paintBlock(p, r, radius_);
        break;
    case Kind::TextLine:
        paintTextLine(p, r.topLeft(), r.width(), (std::min)(qreal(t.spacing.md), r.height()));
        break;
    case Kind::Paragraph: {
        static constexpr qreal widths[8] = { 1.0, 0.92, 0.86, 0.97, 0.78, 0.92, 0.71, 0.85 };
        const qreal line_h = qreal(t.spacing.md + t.spacing.xxs);
        const qreal gap = qreal(t.spacing.sm);
        for (int i = 0; i < line_count_; ++i) {
            const qreal w = r.width() * widths[i % 8];
            const qreal y = r.top() + (line_h + gap) * i;
            paintTextLine(p, QPointF(r.left(), y), w, line_h);
        }
        break;
    }
    case Kind::Avatar: {
        const qreal radius = (std::min)(r.width(), r.height()) * 0.5;
        qreal pulse = 0.6;
        if (sweep_enabled_ && !theme::AidaMotion::reducedMotion())
            pulse = (std::sin(phase_ * 6.2831853) * 0.5 + 0.5) * 0.4 + 0.6;
        p.setPen(Qt::NoPen);
        p.setBrush(widgets::with_alpha(t.panel_header, pulse));
        p.drawEllipse(r.center(), radius, radius);
        break;
    }
    case Kind::Card: {
        paintBlock(p, r, (std::max)(radius_, qreal(t.radius.modal)));
        const qreal pad = qreal(t.spacing.md + t.spacing.xxs);
        const qreal left = r.left() + pad;
        const qreal top = r.top() + pad;
        const qreal aw = r.width() - pad * 2;
        const qreal avatar_r = qreal(t.control.icon_glyph);
        const qreal text_x = left + avatar_r * 2.0 + qreal(t.spacing.sm);
        p.setClipRect(r.adjusted(1, 1, -1, -1));
        p.setClipping(true);
        p.setPen(Qt::NoPen);
        p.setBrush(widgets::with_alpha(t.panel_header, 0.9));
        p.drawEllipse(QPointF(left + avatar_r, top + avatar_r), avatar_r, avatar_r);
        paintTextLine(p, QPointF(text_x, top + qreal(t.spacing.sm - t.spacing.xxs)),
            aw * 0.55, qreal(t.spacing.md - t.panel.border));
        paintTextLine(p, QPointF(text_x, top + qreal(t.spacing.xl)),
            aw * 0.30, qreal(t.spacing.sm + t.panel.border));
        const qreal body_top = top + pad + avatar_r * 2.0 + qreal(t.spacing.sm);
        paintTextLine(p, QPointF(left, body_top), aw * 0.95, qreal(t.spacing.sm + t.spacing.xxs));
        paintTextLine(p, QPointF(left, body_top + qreal(t.spacing.lg)),
            aw * 0.78, qreal(t.spacing.sm + t.spacing.xxs));
        paintTextLine(p, QPointF(left, body_top + qreal(t.spacing.lg) * 2.0),
            aw * 0.65, qreal(t.spacing.sm + t.spacing.xxs));
        p.setClipping(false);
        break;
    }
    case Kind::TableRows: {
        const qreal row_h = qreal(t.row.compact - t.spacing.xxs);
        const qreal col_w = r.width() / table_columns_;
        for (int row = 0; row < table_rows_; ++row) {
            const qreal y = r.top() + row * (row_h + qreal(t.spacing.xs));
            if (y + row_h > r.bottom())
                break;
            for (int ci = 0; ci < table_columns_; ++ci) {
                const qreal cx = r.left() + col_w * ci + qreal(t.spacing.sm);
                qreal cw = col_w * (0.6 + 0.3 * static_cast<qreal>((row * 3 + ci * 7) % 5) / 4.0)
                    - qreal(t.spacing.lg);
                if (cw < qreal(t.spacing.md))
                    cw = qreal(t.spacing.md);
                paintTextLine(p, QPointF(cx, y + qreal(t.spacing.sm - t.spacing.xxs)),
                    cw, qreal(t.spacing.sm + t.spacing.xxs));
            }
        }
        break;
    }
    }
}

}
