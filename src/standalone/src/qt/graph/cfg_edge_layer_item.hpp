#pragma once

#include <QGraphicsItem>
#include <QRectF>

#include <vector>

namespace aida::qt::graph {

enum class cfg_edge_routing_t : std::uint8_t {
    live_cubic,
    workspace_channels
};

struct cfg_edge_data_t {
    QRectF from_rect;
    QRectF to_rect;
    int kind = 0;
    bool true_branch = false;
    bool branching = false;
};

class CfgEdgeLayerItem : public QGraphicsItem {
public:
    explicit CfgEdgeLayerItem(cfg_edge_routing_t routing, QGraphicsItem* parent = nullptr);

    void set_edges(std::vector<cfg_edge_data_t> edges, const QRectF& world_bounds);
    void clear_edges();

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget) override;

private:
    QColor edge_color(int kind, bool branching, bool true_branch) const;
    QString edge_label(int kind, bool branching, bool true_branch) const;

    cfg_edge_routing_t routing_;
    std::vector<cfg_edge_data_t> edges_;
    QRectF bounds_;
};

}
