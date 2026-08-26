#pragma once

#include "core/disasm/disasm_view.hpp"
#include "qt/theme/disasm_theme_tokens.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class QPainter;

namespace aida::qt::disasm {

struct disasm_frame_geometry_t {
    qreal row_height = 0.0;
    qreal gutter_width = 0.0;
    qreal char_width = 1.0;
    qreal bytes_width = 0.0;
    bool draw_bytes = false;
    qreal prefix_x = 0.0;
    qreal bytes_x = 0.0;
    qreal instruction_x = 0.0;
    qreal text_baseline_dy = 0.0;
    qreal prefix_width = 0.0;
};

struct prefix_width_cache_t {
    qreal width = 0.0;
    int addr_format = -1;
    QString font_key;
    quint64 sections_revision = 0;
    bool valid = false;
};

struct flow_arrow_t {
    qreal source_mid_y = 0.0;
    qreal target_y = 0.0;
    bool upward = false;
};

struct edge_bar_t {
    qreal y = 0.0;
    qreal height = 0.0;
    QColor color;
};

class DisasmPainter {
public:
    DisasmPainter(QPainter& painter, const theme::AidaDisasmTheme& theme,
                  const QFont& code_font, qreal device_pixel_ratio);

    const QFontMetricsF& metrics() const noexcept { return metrics_; }
    qreal char_width() const noexcept { return char_width_; }
    bool fixed_pitch() const noexcept { return fixed_pitch_; }

    static qreal prefix_width_for(const disasm_view::workspace_context_t& context,
                                  const QFont& font, const QFontMetricsF& metrics,
                                  int addr_format, qreal device_pixel_ratio,
                                  prefix_width_cache_t& cache);

    qreal text_width(const QString& text) const;

    void fill_row(const QRectF& row, const QColor& color);
    void paint_metadata_row(const QRectF& row, const disasm_frame_geometry_t& geometry,
                            const QString& section, const QString& address,
                            const QString& text, disasm_view::metadata_line_kind_t kind,
                            bool selected, bool hovered);
    void paint_instruction_row(const QRectF& row, const disasm_frame_geometry_t& geometry,
                               const QString& section, const QString& address,
                               const QString& bytes,
                               const disasm_view::formatted_instruction_t* formatted,
                               quint64 runtime_address,
                               const QString& name, bool function_start,
                               const QString& user_comment, const QString& auto_comment,
                               bool selected, bool range_selected, bool hovered,
                               bool bookmarked);
    void queue_flow_arrow(qreal flow_x, qreal source_mid_y, qreal target_y,
                          bool upward);
    void queue_edge_bar(qreal row_top, qreal row_height, const QColor& color);
    void flush_flow_arrows(const QRectF& viewport_rect, qreal row_right_edge,
                           qreal gutter_width);

    void invalidate_span_cache() { span_cache_.clear(); }

private:
    QColor token_color(disasm_view::operand_color_role_t role,
                       const QString& token_text, const QString& name) const;

    QPainter& painter_;
    const theme::AidaDisasmTheme& theme_;
    QFont code_font_;
    QFontMetricsF metrics_;
    qreal char_width_ = 1.0;
    qreal dpr_ = 1.0;
    bool fixed_pitch_ = false;
    std::vector<flow_arrow_t> arrows_;
    std::vector<edge_bar_t> edge_bars_;
    QHash<quint64, QVector<qreal>> span_cache_;
};

}
