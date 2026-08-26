#pragma once

#include "core/disasm/cfg_view.hpp"
#include "qt/theme/disasm_theme_tokens.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsItem>
#include <QRectF>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

class QVariantAnimation;

namespace aida::qt::graph {

class CfgSceneController;

namespace cfg_block_metrics {

qreal hover_lift_max();
qreal entrance_drop_max();
qreal shadow_offset();

}

struct cfg_block_row_t {
    std::uint64_t address = 0;
    QString text;
};

struct cfg_block_data_t {
    int node_id = 0;
    QRectF rect;
    QString header;
    bool is_entry = false;
    bool is_exit = false;
    bool has_breakpoint = false;
    bool current_rip = false;
    std::uint64_t current_rip_address = 0;
    std::optional<unsigned> confidence;
    std::vector<cfg_block_row_t> rows;
    std::size_t total_instructions = 0;
    std::vector<std::pair<int, QString>> injections;
    qreal header_height = 25.0;
    qreal row_height = 18.0;
    qreal addr_column_width = 112.0;
    qreal body_padding = 8.0;
    qreal font_size = 13.0;
};

class CfgBlockItem : public QGraphicsItem {
public:
    CfgBlockItem(const cfg_block_data_t& data, CfgSceneController* controller,
                 QGraphicsItem* parent = nullptr);
    ~CfgBlockItem() override;

    int node_id() const noexcept { return data_.node_id; }
    const cfg_block_data_t& data() const noexcept { return data_; }

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget) override;

    void setSelected(bool selected);
    bool selected() const noexcept { return selected_; }
    void startEntrance();
    void setTextSelection(int anchor_line, int extent_line);
    void clearTextSelection();
    std::pair<int, int> textSelection() const noexcept {
        return {text_sel_anchor_, text_sel_extent_};
    }
    int lineAt(qreal scene_y) const;
    bool textSelectionActive() const noexcept { return text_sel_anchor_ >= 0; }

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;
    void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    void ensure_metrics();

    cfg_block_data_t data_;
    CfgSceneController* controller_ = nullptr;
    bool selected_ = false;
    bool hovered_ = false;
    QVariantAnimation* entrance_anim_ = nullptr;
    QVariantAnimation* hover_anim_ = nullptr;
    qreal hover_lift_ = 0.0;
    qreal entrance_offset_ = 0.0;
    qreal entrance_alpha_ = 1.0;
    int text_sel_anchor_ = -1;
    int text_sel_extent_ = -1;
    bool text_sel_dragging_ = false;
    qreal bound_pad_left_ = 0.0;
    qreal bound_pad_top_ = 0.0;
    qreal bound_pad_right_ = 0.0;
    qreal bound_pad_bottom_ = 0.0;
    QFont header_font_;
    QFont code_font_;
    QFontMetricsF header_metrics_{ header_font_ };
    QFontMetricsF code_metrics_{ code_font_ };
    quint64 metrics_theme_revision_ = 0;
};

QColor cfg_injection_color(int kind, const theme::AidaDisasmTheme& theme);

}
