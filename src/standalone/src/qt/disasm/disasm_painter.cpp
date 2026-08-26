#include "qt/disasm/disasm_painter.hpp"

#include "qt/theme/aida_tokens.hpp"

#include <QFontInfo>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace aida::qt::disasm {

namespace {

bool operand_equals_ci(QStringView left, std::string_view right)
{
    if (left.size() != static_cast<qsizetype>(right.size()))
        return false;
    for (qsizetype index = 0; index < left.size(); ++index) {
        const auto l = left[index].toLatin1();
        const auto r = right[static_cast<std::size_t>(index)];
        if (std::tolower(static_cast<unsigned char>(l)) !=
            std::tolower(static_cast<unsigned char>(r)))
            return false;
    }
    return true;
}

}

DisasmPainter::DisasmPainter(QPainter& painter,
                             const theme::AidaDisasmTheme& theme,
                             const QFont& code_font, qreal device_pixel_ratio)
    : painter_(painter), theme_(theme), code_font_(code_font),
      metrics_(code_font_), dpr_(device_pixel_ratio)
{
    char_width_ = (std::max)(1.0, metrics_.horizontalAdvance(u'0'));
    fixed_pitch_ = QFontInfo(code_font_).fixedPitch();
}

qreal DisasmPainter::text_width(const QString& text) const
{
    if (fixed_pitch_)
        return char_width_ * static_cast<qreal>(text.size());
    return metrics_.horizontalAdvance(text);
}

qreal DisasmPainter::prefix_width_for(
    const disasm_view::workspace_context_t& context,
    const QFont& font, const QFontMetricsF& metrics, int addr_format,
    qreal device_pixel_ratio, prefix_width_cache_t& cache)
{
    std::uint64_t sections_revision = 0;
    if (context.image) {
        const auto& sections = context.image->sections();
        sections_revision = sections.size();
        if (!sections.empty()) {
            sections_revision ^=
                sections.front().virtual_address * 0x9E3779B185EBCA87ULL;
            sections_revision ^=
                sections.back().virtual_address * 0xC2B2AE3D27D4EB4FULL;
        }
        sections_revision ^= static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(context.image.get()));
    }
    const QString font_key = font.family() + u'|' +
        QString::number(font.pixelSize()) + u'|' +
        QString::number(device_pixel_ratio);
    if (cache.valid && cache.addr_format == addr_format &&
        cache.sections_revision == sections_revision && cache.font_key == font_key)
        return cache.width;
    qreal width = metrics.horizontalAdvance(QStringLiteral(".text"));
    if (context.image) {
        for (const auto& section : context.image->sections())
            width = (std::max)(width,
                metrics.horizontalAdvance(QString::fromStdString(section.name)));
    }
    width += metrics.horizontalAdvance(QStringLiteral(":"));
    const QString label = addr_format == static_cast<int>(disasm_view::addr_format_t::rva)
        ? QStringLiteral("+00000000")
        : addr_format == static_cast<int>(disasm_view::addr_format_t::file_offset)
            ? QStringLiteral("00000000")
            : QStringLiteral("0000000000000000");
    width += metrics.horizontalAdvance(label);
    cache.width = width;
    cache.addr_format = addr_format;
    cache.font_key = font_key;
    cache.sections_revision = sections_revision;
    cache.valid = true;
    return width;
}

void DisasmPainter::fill_row(const QRectF& row, const QColor& color)
{
    painter_.fillRect(row, color);
}

void DisasmPainter::paint_metadata_row(const QRectF& row,
                                       const disasm_frame_geometry_t& geometry,
                                       const QString& section, const QString& address,
                                       const QString& text,
                                       disasm_view::metadata_line_kind_t kind,
                                       bool selected, bool hovered)
{
    if (selected || hovered)
        painter_.fillRect(row, selected ? theme_.selection_bg : theme_.cursor_line_bg);
    const qreal baseline = row.y() + geometry.text_baseline_dy;
    painter_.setPen(theme_.segment);
    painter_.drawText(QPointF(geometry.prefix_x, baseline), section);
    painter_.setPen(theme_.separator);
    const qreal section_width = text_width(section);
    painter_.drawText(QPointF(geometry.prefix_x + section_width, baseline),
        QStringLiteral(":"));
    painter_.setPen(theme_.address);
    painter_.drawText(QPointF(geometry.prefix_x + section_width + char_width_, baseline),
        address);
    QColor line_color = theme_.comment;
    if (kind == disasm_view::metadata_line_kind_t::banner)
        line_color = theme_.banner;
    else if (kind == disasm_view::metadata_line_kind_t::directive)
        line_color = theme_.directive;
    else if (kind == disasm_view::metadata_line_kind_t::keyword)
        line_color = theme_.keyword;
    painter_.setPen(line_color);
    painter_.setClipRect(row, Qt::IntersectClip);
    painter_.drawText(QPointF(geometry.instruction_x, baseline), text);
    painter_.setClipping(false);
}

QColor DisasmPainter::token_color(disasm_view::operand_color_role_t role,
                                  const QString& token_text,
                                  const QString& name) const
{
    const theme::tokens_t& base = theme::tokens();
    switch (role) {
    case disasm_view::operand_color_role_t::reg: return theme_.reg;
    case disasm_view::operand_color_role_t::imm: return theme_.immediate_num;
    case disasm_view::operand_color_role_t::keyword: return theme_.keyword;
    case disasm_view::operand_color_role_t::string_ref: return theme_.string_ref;
    case disasm_view::operand_color_role_t::reg_ptr: return theme_.reg_ptr;
    case disasm_view::operand_color_role_t::sub_label: return theme_.sub_label;
    case disasm_view::operand_color_role_t::name_candidate: {
        const auto text = token_text.toStdString();
        if (!name.isEmpty() && operand_equals_ci(token_text, text.empty() ? "" : name.toStdString()))
            return theme_.func_name;
        if (token_text.size() > 4 &&
            (operand_equals_ci(token_text.left(4), "sub_") ||
             operand_equals_ci(token_text.left(4), "loc_") ||
             operand_equals_ci(token_text.left(4), "off_")))
            return theme_.sub_label;
        if (token_text.contains(u'_'))
            return theme_.sub_label;
        return base.text_secondary;
    }
    case disasm_view::operand_color_role_t::plain:
    default:
        return base.text_secondary;
    }
}

void DisasmPainter::paint_instruction_row(const QRectF& row,
                                           const disasm_frame_geometry_t& geometry,
                                           const QString& section,
                                           const QString& address,
                                           const QString& bytes,
                                           const disasm_view::formatted_instruction_t* formatted,
                                           quint64 runtime_address,
                                           const QString& name, bool function_start,
                                           const QString& user_comment,
                                           const QString& auto_comment,
                                           bool selected, bool range_selected, bool hovered,
                                           bool bookmarked)
{
    const auto& base_tokens = theme::tokens();
    if (selected) {
        painter_.fillRect(row, theme_.selection_bg);
        queue_edge_bar(row.y(), row.height(), base_tokens.accent);
    } else if (range_selected) {
        painter_.fillRect(row, theme::disasm_with_alpha(theme_.selection_bg, 0.55));
    } else if (hovered) {
        painter_.fillRect(row, theme_.cursor_line_bg);
    }
    if (bookmarked)
        queue_edge_bar(row.y(), row.height(), base_tokens.warning);
    const qreal baseline = row.y() + geometry.text_baseline_dy;
    painter_.setPen(theme_.segment);
    painter_.drawText(QPointF(geometry.prefix_x, baseline), section);
    const qreal section_width = text_width(section);
    painter_.setPen(theme_.separator);
    painter_.drawText(QPointF(geometry.prefix_x + section_width, baseline),
        QStringLiteral(":"));
    painter_.setPen(bookmarked ? base_tokens.warning : theme_.address);
    painter_.drawText(QPointF(geometry.prefix_x + section_width + char_width_, baseline),
        address);
    if (geometry.draw_bytes && formatted != nullptr && !bytes.isEmpty()) {
        painter_.setClipRect(QRectF(geometry.bytes_x, row.y(),
            geometry.instruction_x - char_width_ * 0.5 - geometry.bytes_x, row.height()),
            Qt::IntersectClip);
        painter_.setPen(theme_.bytes);
        painter_.drawText(QPointF(geometry.bytes_x, baseline), bytes);
        painter_.setClipping(false);
    }
    qreal cursor_x = geometry.instruction_x;
    qreal label_width_px = 0.0;
    std::uint32_t label_columns = 0;
    if (function_start) {
        painter_.setPen(QPen(theme::disasm_with_alpha(theme_.banner, 0.65), 1.0));
        painter_.drawLine(QPointF(geometry.instruction_x, row.y()),
            QPointF(row.right(), row.y()));
        QString function_name = name;
        if (function_name.isEmpty()) {
            function_name = QStringLiteral("sub_%1").arg(
                QString::number(runtime_address, 16).toUpper());
        }
        function_name += QStringLiteral(":  ");
        painter_.setPen(theme_.func_name);
        painter_.drawText(QPointF(cursor_x, baseline), function_name);
        label_columns = static_cast<std::uint32_t>(function_name.size());
        label_width_px = text_width(function_name);
        cursor_x += label_width_px;
    } else if (!name.isEmpty()) {
        const QString label = name + QStringLiteral(":  ");
        painter_.setPen(theme_.loc_label);
        painter_.drawText(QPointF(cursor_x, baseline), label);
        label_columns = static_cast<std::uint32_t>(label.size());
        label_width_px = text_width(label);
        cursor_x += label_width_px;
    }
    if (formatted != nullptr && formatted->error.empty()) {
        const QString line = QString::fromStdString(formatted->text);
        const auto mnemonic_end = (std::min)(formatted->mnemonic_end,
            static_cast<std::size_t>(line.size()));
        const QString mnemonic = line.left(static_cast<qsizetype>(mnemonic_end));
        QColor mnemonic_color = theme_.mnemonic;
        if (!mnemonic.isEmpty() && (mnemonic.front() == u'j' || mnemonic.front() == u'J'))
            mnemonic_color = theme_.mnem_branch;
        else if (mnemonic == QLatin1String("call") || mnemonic == QLatin1String("CALL"))
            mnemonic_color = theme_.mnem_call;
        else if (mnemonic == QLatin1String("ret") || mnemonic == QLatin1String("retn") ||
                 mnemonic == QLatin1String("RET"))
            mnemonic_color = theme_.mnem_ret;
        painter_.setPen(mnemonic_color);
        painter_.drawText(QPointF(cursor_x, baseline), mnemonic);
        if (fixed_pitch_) {
            cursor_x += char_width_ * static_cast<qreal>(mnemonic.size());
        } else {
            cursor_x += metrics_.horizontalAdvance(mnemonic);
        }
        QVector<qreal>* span_widths = nullptr;
        if (!fixed_pitch_) {
            const auto cache_key = static_cast<quint64>(formatted->instruction_id);
            auto widths_it = span_cache_.find(cache_key);
            if (widths_it == span_cache_.end() ||
                widths_it->size() != static_cast<qsizetype>(formatted->tokens.size())) {
                QVector<qreal> widths;
                widths.reserve(static_cast<qsizetype>(formatted->tokens.size()));
                qreal accumulated = 0.0;
                std::uint32_t consumed = 0;
                for (const auto& span : formatted->tokens) {
                    if (span.offset > consumed) {
                        accumulated += metrics_.horizontalAdvance(line.mid(
                            static_cast<qsizetype>(consumed),
                            static_cast<qsizetype>(span.offset - consumed)));
                    }
                    widths.push_back(accumulated);
                    consumed = span.offset + span.length;
                }
                widths_it = span_cache_.insert(cache_key, widths);
            }
            span_widths = &widths_it.value();
        }
        std::size_t token_index = 0;
        const qreal clip_right = row.right();
        for (const auto& token : formatted->tokens) {
            if (token.offset >= static_cast<std::size_t>(line.size()))
                break;
            const auto token_length = (std::min)(static_cast<std::size_t>(token.length),
                static_cast<std::size_t>(line.size()) - token.offset);
            const QString token_text = line.mid(static_cast<qsizetype>(token.offset),
                static_cast<qsizetype>(token_length));
            painter_.setPen(token_color(
                static_cast<disasm_view::operand_color_role_t>(token.color_role),
                token_text, name));
            qreal token_x;
            if (fixed_pitch_) {
                token_x = geometry.instruction_x + char_width_ *
                    static_cast<qreal>(label_columns + token.offset);
            } else {
                token_x = geometry.instruction_x + label_width_px +
                    span_widths->at(static_cast<qsizetype>(token_index));
            }
            if (token_x > clip_right)
                break;
            painter_.drawText(QPointF(token_x, baseline), token_text);
            ++token_index;
        }
        if (fixed_pitch_) {
            cursor_x = geometry.instruction_x +
                char_width_ * static_cast<qreal>(label_columns + line.size());
        } else {
            cursor_x = geometry.instruction_x + label_width_px +
                metrics_.horizontalAdvance(line);
        }
    } else {
        const QString pending = formatted != nullptr
            ? QString::fromStdString(formatted->error)
            : QStringLiteral("Formatting...");
        painter_.setPen(theme::tokens().text_dim);
        painter_.drawText(QPointF(cursor_x, baseline), pending);
        cursor_x += text_width(pending);
    }
    if ((!user_comment.isEmpty() || !auto_comment.isEmpty()) &&
        cursor_x + char_width_ <= row.right()) {
        const QString combined = user_comment.isEmpty() ? auto_comment
            : auto_comment.isEmpty() ? user_comment
            : user_comment + QStringLiteral("; ") + auto_comment;
        const QString rendered_comment = QStringLiteral("  ; ") + combined;
        painter_.setPen(theme_.comment);
        painter_.drawText(QPointF(cursor_x + char_width_, baseline), rendered_comment);
    }
}

void DisasmPainter::queue_flow_arrow(qreal flow_x, qreal source_mid_y, qreal target_y,
                                     bool upward)
{
    flow_arrow_t arrow;
    arrow.source_mid_y = source_mid_y;
    arrow.target_y = target_y;
    arrow.upward = upward;
    static_cast<void>(flow_x);
    arrows_.push_back(arrow);
}

void DisasmPainter::queue_edge_bar(qreal row_top, qreal row_height, const QColor& color)
{
    edge_bars_.push_back(edge_bar_t{row_top, row_height, color});
}

void DisasmPainter::flush_flow_arrows(const QRectF& viewport_rect, qreal row_right_edge,
                                      qreal gutter_width)
{
    if (!edge_bars_.empty()) {
        const qreal bar_w = static_cast<qreal>(theme::tokens().spacing.xs) - 1.0;
        const qreal bar_gap = static_cast<qreal>(theme::tokens().spacing.xxs);
        qreal stacked_y = -1.0;
        std::size_t slot = 0;
        for (const auto& bar : edge_bars_) {
            if (bar.y != stacked_y) {
                stacked_y = bar.y;
                slot = 0;
            } else {
                ++slot;
            }
            painter_.fillRect(QRectF(viewport_rect.x() +
                static_cast<qreal>(slot) * (bar_w + bar_gap), bar.y, bar_w, bar.height),
                bar.color);
        }
        edge_bars_.clear();
    }
    if (arrows_.empty())
        return;
    std::vector<std::size_t> order(arrows_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [this](std::size_t left, std::size_t right) {
        const qreal lo_left = (std::min)(arrows_[left].source_mid_y, arrows_[left].target_y);
        const qreal lo_right = (std::min)(arrows_[right].source_mid_y,
            arrows_[right].target_y);
        return lo_left < lo_right;
    });
    const qreal lane_step = (std::max)(2.0, char_width_ * 0.5);
    const qreal lane_first_x = viewport_rect.x() + char_width_ * 0.5;
    const qreal lane_last_x = viewport_rect.x() + (std::max)(char_width_ * 0.5,
        gutter_width - char_width_ * 0.25);
    std::vector<qreal> lane_busy_until;
    std::vector<qreal> lane_x(arrows_.size(), lane_first_x);
    for (const std::size_t index : order) {
        const auto& arrow = arrows_[index];
        const qreal lo = (std::min)(arrow.source_mid_y, arrow.target_y);
        const qreal hi = (std::max)(arrow.source_mid_y, arrow.target_y);
        std::size_t lane = 0;
        bool reused = false;
        for (; lane < lane_busy_until.size(); ++lane) {
            if (lane_busy_until[lane] + 2.0 <= lo) {
                reused = true;
                break;
            }
        }
        if (!reused) {
            lane = lane_busy_until.size();
            lane_busy_until.push_back(hi);
        } else {
            lane_busy_until[lane] = hi;
        }
        lane_x[index] = (std::min)(lane_first_x + static_cast<qreal>(lane) * lane_step,
            lane_last_x);
    }
    QList<QLineF> up_lines;
    QList<QLineF> down_lines;
    QList<QPolygonF> up_triangles;
    QList<QPolygonF> down_triangles;
    const qreal stub = char_width_ * 0.65;
    for (std::size_t index = 0; index < arrows_.size(); ++index) {
        const auto& arrow = arrows_[index];
        if (std::abs(arrow.source_mid_y - arrow.target_y) < 1.0)
            continue;
        const qreal x = lane_x[index];
        auto& lines = arrow.upward ? up_lines : down_lines;
        lines.push_back(QLineF(x, arrow.source_mid_y, x, arrow.target_y));
        lines.push_back(QLineF(x, arrow.source_mid_y, x + stub, arrow.source_mid_y));
        lines.push_back(QLineF(x, arrow.target_y, x + stub, arrow.target_y));
        const qreal cue_x = row_right_edge - char_width_ * 0.8;
        const qreal cue_y = arrow.source_mid_y;
        const qreal half = char_width_ * 0.45;
        const qreal depth = char_width_ * 0.4;
        QPolygonF triangle;
        if (arrow.upward) {
            triangle << QPointF(cue_x, cue_y - depth)
                     << QPointF(cue_x - half, cue_y + depth * 0.75)
                     << QPointF(cue_x + half, cue_y + depth * 0.75);
        } else {
            triangle << QPointF(cue_x, cue_y + depth)
                     << QPointF(cue_x - half, cue_y - depth * 0.75)
                     << QPointF(cue_x + half, cue_y - depth * 0.75);
        }
        (arrow.upward ? up_triangles : down_triangles).push_back(triangle);
    }
    const QColor up_color = theme::disasm_with_alpha(theme_.arrow_up, 0.8);
    const QColor down_color = theme::disasm_with_alpha(theme_.arrow_down, 0.8);
    if (!up_lines.isEmpty()) {
        painter_.setPen(QPen(up_color, 1.0));
        painter_.drawLines(up_lines);
    }
    if (!down_lines.isEmpty()) {
        painter_.setPen(QPen(down_color, 1.0));
        painter_.drawLines(down_lines);
    }
    painter_.setPen(Qt::NoPen);
    if (!up_triangles.isEmpty()) {
        painter_.setBrush(up_color);
        for (const auto& triangle : up_triangles)
            painter_.drawPolygon(triangle);
    }
    if (!down_triangles.isEmpty()) {
        painter_.setBrush(down_color);
        for (const auto& triangle : down_triangles)
            painter_.drawPolygon(triangle);
    }
    painter_.setBrush(Qt::NoBrush);
    arrows_.clear();
}

}
