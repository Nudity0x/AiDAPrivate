#include "qt/graph/cfg_edge_layer_item.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/theme/disasm_theme_tokens.hpp"

#include "core/analysis/workspace/compact_ir.hpp"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionGraphicsItem>

#include <algorithm>
#include <cmath>

namespace aida::qt::graph {

namespace {

constexpr qreal k_overview_lod = 0.12;

}

using aida::analysis::edge_kind_t;

CfgEdgeLayerItem::CfgEdgeLayerItem(cfg_edge_routing_t routing, QGraphicsItem* parent)
    : QGraphicsItem(parent), routing_(routing)
{
    setZValue(0.0);
    setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
}

void CfgEdgeLayerItem::set_edges(std::vector<cfg_edge_data_t> edges,
                                 const QRectF& world_bounds)
{
    prepareGeometryChange();
    edges_ = std::move(edges);
    const qreal margin = static_cast<qreal>(theme::tokens().spacing.section) * 2.0;
    bounds_ = world_bounds.adjusted(-margin, -margin, margin, margin);
    update();
}

void CfgEdgeLayerItem::clear_edges()
{
    prepareGeometryChange();
    edges_.clear();
    update();
}

QRectF CfgEdgeLayerItem::boundingRect() const
{
    return bounds_.isNull() ? QRectF(0.0, 0.0, 1.0, 1.0) : bounds_;
}

QColor CfgEdgeLayerItem::edge_color(int kind, bool branching, bool true_branch) const
{
    const auto& t = theme::tokens();
    if (routing_ == cfg_edge_routing_t::live_cubic) {
        if (branching)
            return true_branch ? t.success : t.error;
        return t.text_secondary.lighter(110);
    }
    switch (static_cast<edge_kind_t>(kind)) {
    case edge_kind_t::fallthrough:
        return branching ? t.error : t.text_secondary;
    case edge_kind_t::conditional_taken: return t.success;
    case edge_kind_t::unconditional: return t.info;
    case edge_kind_t::call: return t.accent_hover;
    case edge_kind_t::tail_call: return t.warning;
    case edge_kind_t::return_edge: return t.error;
    case edge_kind_t::exception_edge: return t.warning;
    case edge_kind_t::indirect: return t.syn_keyword;
    }
    return t.text_secondary;
}

QString CfgEdgeLayerItem::edge_label(int kind, bool branching, bool true_branch) const
{
    if (routing_ == cfg_edge_routing_t::live_cubic)
        return branching ? (true_branch ? QStringLiteral("T") : QStringLiteral("F"))
                         : QString();
    switch (static_cast<edge_kind_t>(kind)) {
    case edge_kind_t::fallthrough: return branching ? QStringLiteral("F") : QString();
    case edge_kind_t::conditional_taken: return QStringLiteral("T");
    case edge_kind_t::unconditional: return QStringLiteral("J");
    case edge_kind_t::call: return QStringLiteral("CALL");
    case edge_kind_t::tail_call: return QStringLiteral("TAIL");
    case edge_kind_t::return_edge: return QStringLiteral("RET");
    case edge_kind_t::exception_edge: return QStringLiteral("EX");
    case edge_kind_t::indirect: return QStringLiteral("IND");
    }
    return {};
}

void CfgEdgeLayerItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
                             QWidget*)
{
    if (edges_.empty())
        return;
    const auto& t = theme::tokens();
    const qreal zoom = QStyleOptionGraphicsItem::levelOfDetailFromTransform(
        painter->worldTransform());
    const bool overview = zoom < k_overview_lod;
    const qreal halo_thickness = (std::max)(3.5, 5.5 * zoom);
    const qreal line_thickness = (std::max)(1.5, 2.0 * zoom);
    const qreal arrow_size = (std::max)(5.0, 7.0 * zoom);
    const qreal channel_offset = static_cast<qreal>(t.spacing.section) +
        static_cast<qreal>(t.spacing.xxl);
    const auto label_font = theme::fonts::codeRegular();
    const QFontMetricsF label_metrics(label_font);
    const QRectF exposed = option->exposedRect;
    painter->setRenderHint(QPainter::Antialiasing, true);
    for (const auto& edge : edges_) {
        QPainterPath path;
        QPointF p1;
        QPointF p4;
        if (routing_ == cfg_edge_routing_t::workspace_channels &&
            edge.to_rect.y() <= edge.from_rect.y() + edge.from_rect.height() * 0.25) {
            p1 = QPointF(edge.from_rect.x() + edge.from_rect.width(),
                edge.from_rect.y() + edge.from_rect.height() * 0.5);
            p4 = QPointF(edge.to_rect.x() + edge.to_rect.width(),
                edge.to_rect.y() + edge.to_rect.height() * 0.5);
            const qreal route_x = (std::max)(p1.x(), p4.x()) + channel_offset;
            path.moveTo(p1);
            path.cubicTo(QPointF(route_x, p1.y()), QPointF(route_x, p4.y()), p4);
        } else if (routing_ == cfg_edge_routing_t::workspace_channels) {
            p1 = QPointF(edge.from_rect.x() + edge.from_rect.width() * 0.5,
                edge.from_rect.y() + edge.from_rect.height());
            p4 = QPointF(edge.to_rect.x() + edge.to_rect.width() * 0.5,
                edge.to_rect.y());
            const qreal middle = (p1.y() + p4.y()) * 0.5;
            path.moveTo(p1);
            path.cubicTo(QPointF(p1.x(), middle), QPointF(p4.x(), middle), p4);
        } else {
            p1 = QPointF(edge.from_rect.x() + edge.from_rect.width() * 0.5,
                edge.from_rect.y() + edge.from_rect.height());
            p4 = QPointF(edge.to_rect.x() + edge.to_rect.width() * 0.5,
                edge.to_rect.y());
            const qreal middle = (p1.y() + p4.y()) * 0.5;
            path.moveTo(p1);
            path.cubicTo(QPointF(p1.x(), middle), QPointF(p4.x(), middle), p4);
        }
        const QRectF edge_bounds = path.boundingRect();
        if (!exposed.isNull() && !edge_bounds.intersects(exposed))
            continue;
        const QColor color = edge_color(edge.kind, edge.branching, edge.true_branch);
        if (overview) {
            painter->setPen(QPen(color, line_thickness));
            painter->drawPath(path);
            continue;
        }
        QColor halo = color;
        halo.setAlphaF(0.18);
        painter->setPen(QPen(halo, halo_thickness));
        painter->drawPath(path);
        painter->setPen(QPen(color, line_thickness));
        painter->drawPath(path);

        const QPointF tip = path.pointAtPercent(1.0);
        const qreal angle = path.angleAtPercent(1.0);
        const qreal radians = angle * 3.14159265358979323846 / 180.0;
        const QPointF direction(std::cos(radians), -std::sin(radians));
        const QPointF perpendicular(-direction.y(), direction.x());
        const QPointF a1(tip.x() - direction.x() * arrow_size +
                perpendicular.x() * arrow_size * 0.5,
            tip.y() - direction.y() * arrow_size +
                perpendicular.y() * arrow_size * 0.5);
        const QPointF a2(tip.x() - direction.x() * arrow_size -
                perpendicular.x() * arrow_size * 0.5,
            tip.y() - direction.y() * arrow_size -
                perpendicular.y() * arrow_size * 0.5);
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawPolygon(QPolygonF() << tip << a1 << a2);

        const QString label = edge_label(edge.kind, edge.branching, edge.true_branch);
        if (!label.isEmpty() &&
            (routing_ == cfg_edge_routing_t::live_cubic || zoom > 0.32)) {
            const qreal chip_pad = static_cast<qreal>(t.spacing.xs);
            const qreal chip_radius = static_cast<qreal>(t.radius.xs);
            painter->setFont(label_font);
            const qreal label_width = label_metrics.horizontalAdvance(label);
            const qreal label_height = label_metrics.height();
            const QPointF label_position((p1.x() + p4.x()) * 0.5 +
                static_cast<qreal>(t.spacing.xs + t.spacing.xxs),
                (p1.y() + p4.y()) * 0.5 - label_height * 0.5);
            QColor chip = t.bg_overlay;
            chip.setAlphaF(chip.alphaF() * 0.96);
            painter->setPen(Qt::NoPen);
            painter->setBrush(chip);
            painter->drawRoundedRect(QRectF(label_position.x() - chip_pad,
                label_position.y() - 1.0, label_width + chip_pad * 2.0,
                label_height + 2.0), chip_radius, chip_radius);
            painter->setPen(QPen(color, 1.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(label_position.x() - chip_pad,
                label_position.y() - 1.0, label_width + chip_pad * 2.0,
                label_height + 2.0), chip_radius, chip_radius);
            painter->setPen(color);
            painter->drawText(QPointF(label_position.x(),
                label_position.y() + label_metrics.ascent()), label);
        }
    }
}

}
