#include "qt/graph/cfg_minimap.hpp"

#include "qt/theme/aida_tokens.hpp"

#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>

#include <algorithm>

namespace aida::qt::graph {

CfgMinimap::CfgMinimap(QWidget* parent) : QGraphicsView(parent)
{
    setObjectName(QStringLiteral("aida.graph.minimap"));
    setProperty("aidaRole", QStringLiteral("overlay_chip"));
    setInteractive(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
    setOptimizationFlag(QGraphicsView::DontAdjustForAntialiasing, true);
    setRenderHint(QPainter::Antialiasing, false);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral(
        "Graph overview — click or drag to pan the view; arrow keys pan when focused"));
    setAccessibleName(QStringLiteral("Graph minimap"));
    setAccessibleDescription(QStringLiteral(
        "Miniature graph overview. Click or drag to pan the tracked view, "
        "or use the arrow keys to pan while the minimap has keyboard focus."));
}

void CfgMinimap::setTrackedView(QGraphicsView* view)
{
    if (tracked_ == view)
        return;
    if (tracked_) {
        disconnect(tracked_->horizontalScrollBar(), nullptr, this, nullptr);
        disconnect(tracked_->verticalScrollBar(), nullptr, this, nullptr);
    }
    tracked_ = view;
    if (tracked_) {
        connect(tracked_->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { viewport()->update(); });
        connect(tracked_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { viewport()->update(); });
    }
}

void CfgMinimap::refit()
{
    if (scene())
        fitInView(scene()->itemsBoundingRect(), Qt::KeepAspectRatio);
}

void CfgMinimap::drawForeground(QPainter* painter, const QRectF& rect)
{
    if (!tracked_ || !tracked_->scene())
        return;
    QGraphicsView::drawForeground(painter, {});
    const auto& t = theme::tokens();
    const QRectF visible = tracked_->mapToScene(tracked_->viewport()->rect())
        .boundingRect();
    if (rect.isValid() && !visible.isNull()) {
        QPainterPath dim_path;
        dim_path.setFillRule(Qt::OddEvenFill);
        dim_path.addRect(rect);
        dim_path.addRect(visible);
        QColor dim = t.title_bar;
        dim.setAlphaF(0.55);
        painter->setPen(Qt::NoPen);
        painter->setBrush(dim);
        painter->drawPath(dim_path);
    }
    painter->setPen(QPen(t.accent, 0.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(visible);
    const qreal offset = static_cast<qreal>(t.spacing.xxs);
    const qreal bracket = static_cast<qreal>(t.spacing.xs + t.spacing.xxs);
    const QPointF corners[4] = {
        visible.topLeft() + QPointF(-offset, -offset),
        visible.topRight() + QPointF(offset, -offset),
        visible.bottomLeft() + QPointF(-offset, offset),
        visible.bottomRight() + QPointF(offset, offset),
    };
    const QPointF dirs[4] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    painter->setPen(QPen(t.accent_hover, 0.0));
    for (int index = 0; index < 4; ++index) {
        painter->drawLine(corners[index],
            corners[index] + QPointF(dirs[index].x() * bracket, 0.0));
        painter->drawLine(corners[index],
            corners[index] + QPointF(0.0, dirs[index].y() * bracket));
    }
    if (hasFocus()) {
        painter->save();
        painter->setWorldTransform(QTransform());
        const qreal inset = static_cast<qreal>(t.control.focus_ring) * 0.5;
        painter->setPen(QPen(t.border_focus, static_cast<qreal>(t.control.focus_ring)));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(QRectF(viewport()->rect()).adjusted(inset, inset, -inset, -inset));
        painter->restore();
    }
}

QPointF CfgMinimap::clampedCenter(const QPointF& center) const
{
    if (!tracked_ || !tracked_->scene())
        return center;
    const QRectF bounds = tracked_->scene()->itemsBoundingRect();
    if (bounds.isNull() || bounds.width() < 1.0 || bounds.height() < 1.0)
        return center;
    const auto& t = theme::tokens();
    const qreal slack = static_cast<qreal>(t.spacing.section) * 6.0 +
        static_cast<qreal>(t.spacing.sm);
    const QRectF visible = tracked_->mapToScene(tracked_->viewport()->rect())
        .boundingRect();
    const qreal margin_x = (std::max)(slack, visible.width() * 0.5);
    const qreal margin_y = (std::max)(slack, visible.height() * 0.5);
    return QPointF(
        (std::clamp)(center.x(), bounds.left() - margin_x, bounds.right() + margin_x),
        (std::clamp)(center.y(), bounds.top() - margin_y, bounds.bottom() + margin_y));
}

void CfgMinimap::trackTo(const QPointF& view_pos)
{
    if (!tracked_ || !scene())
        return;
    tracked_->centerOn(clampedCenter(mapToScene(view_pos.toPoint())));
}

void CfgMinimap::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        trackTo(event->position());
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void CfgMinimap::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_) {
        trackTo(event->position());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void CfgMinimap::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void CfgMinimap::keyPressEvent(QKeyEvent* event)
{
    if (!tracked_ || event->modifiers()) {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    const QRectF visible = tracked_->mapToScene(tracked_->viewport()->rect())
        .boundingRect();
    if (visible.isNull()) {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    QPointF center = visible.center();
    const qreal step_x = visible.width() / 8.0;
    const qreal step_y = visible.height() / 8.0;
    switch (event->key()) {
    case Qt::Key_Left:  center.rx() -= step_x; break;
    case Qt::Key_Right: center.rx() += step_x; break;
    case Qt::Key_Up:    center.ry() -= step_y; break;
    case Qt::Key_Down:  center.ry() += step_y; break;
    default:
        QGraphicsView::keyPressEvent(event);
        return;
    }
    tracked_->centerOn(clampedCenter(center));
    event->accept();
}

void CfgMinimap::focusInEvent(QFocusEvent* event)
{
    QGraphicsView::focusInEvent(event);
    viewport()->update();
}

void CfgMinimap::focusOutEvent(QFocusEvent* event)
{
    QGraphicsView::focusOutEvent(event);
    viewport()->update();
}

}
