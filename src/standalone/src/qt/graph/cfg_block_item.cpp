#include "qt/graph/cfg_block_item.hpp"

#include "qt/graph/cfg_scene_controller.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"

#include <QFontMetricsF>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneHoverEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

namespace aida::qt::graph {

namespace {

constexpr qreal k_lod_text_threshold = 0.22;

}

namespace cfg_block_metrics {

qreal hover_lift_max()
{
    return static_cast<qreal>(theme::tokens().spacing.xxs);
}

qreal entrance_drop_max()
{
    return static_cast<qreal>(theme::tokens().spacing.sm);
}

qreal shadow_offset()
{
    return static_cast<qreal>(theme::tokens().spacing.xs);
}

}

QColor cfg_injection_color(int kind, const theme::AidaDisasmTheme& theme)
{
    using injection_t = function_index::injection_t;
    switch (static_cast<injection_t>(kind)) {
    case injection_t::function_banner:
    case injection_t::endp_separator:
        return theme.banner;
    case injection_t::attributes_line:
    case injection_t::prototype_line:
        return theme.comment;
    case injection_t::var_decl:
        return theme.var_decl;
    case injection_t::label_line:
        return theme.loc_label;
    case injection_t::proc_header:
    case injection_t::proc_endp:
        return theme.sub_label;
    case injection_t::spacer_line:
    case injection_t::noreturn_separator:
    default:
        return theme.comment;
    }
}

CfgBlockItem::CfgBlockItem(const cfg_block_data_t& data, CfgSceneController* controller,
                           QGraphicsItem* parent)
    : QGraphicsItem(parent), data_(data), controller_(controller)
{
    setPos(data_.rect.topLeft());
    setFlag(QGraphicsItem::ItemIsSelectable);
    setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
    setAcceptHoverEvents(true);
    setZValue(1.0);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    const qreal pen_bleed = 1.0;
    bound_pad_left_ = cfg_block_metrics::hover_lift_max() + pen_bleed;
    bound_pad_top_ = cfg_block_metrics::hover_lift_max() + pen_bleed;
    bound_pad_right_ = cfg_block_metrics::shadow_offset() + pen_bleed;
    bound_pad_bottom_ = cfg_block_metrics::entrance_drop_max() +
        cfg_block_metrics::shadow_offset() + pen_bleed;
    const QString count = QStringLiteral("%1 instruction%2")
        .arg(data_.total_instructions)
        .arg(data_.total_instructions == 1 ? QString() : QStringLiteral("s"));
    setToolTip(QStringLiteral("<pre>%1\n%2</pre>")
        .arg(data_.header.toHtmlEscaped(), count));
}

CfgBlockItem::~CfgBlockItem()
{
    if (entrance_anim_) {
        entrance_anim_->stop();
        entrance_anim_->deleteLater();
    }
    if (hover_anim_) {
        hover_anim_->stop();
        hover_anim_->deleteLater();
    }
}

QRectF CfgBlockItem::boundingRect() const
{
    return QRectF(QPointF(0.0, 0.0), data_.rect.size())
        .adjusted(-bound_pad_left_, -bound_pad_top_, bound_pad_right_,
            bound_pad_bottom_);
}

int CfgBlockItem::lineAt(qreal scene_y) const
{
    const qreal body_top = data_.header_height + data_.body_padding;
    if (scene_y < body_top)
        return -1;
    const int raw = static_cast<int>((scene_y - body_top) / data_.row_height);
    const int index = raw - static_cast<int>(data_.injections.size());
    return index >= 0 && index < static_cast<int>(data_.rows.size()) ? index : -1;
}

void CfgBlockItem::setSelected(bool selected)
{
    if (selected_ == selected)
        return;
    selected_ = selected;
    setZValue(selected ? 2.0 : 1.0);
    QGraphicsItem::setSelected(selected);
    update();
}

void CfgBlockItem::startEntrance()
{
    entrance_offset_ = cfg_block_metrics::entrance_drop_max();
    entrance_alpha_ = 0.0;
    if (theme::AidaMotion::reducedMotion()) {
        entrance_offset_ = 0.0;
        entrance_alpha_ = 1.0;
        update();
        return;
    }
    if (!entrance_anim_) {
        entrance_anim_ = new QVariantAnimation(controller_);
        entrance_anim_->setStartValue(0.0);
        entrance_anim_->setEndValue(1.0);
        entrance_anim_->setDuration(theme::tokens().motion.standard);
        entrance_anim_->setEasingCurve(theme::easingFor(theme::Ease::OutCubic));
        QObject::connect(entrance_anim_, &QVariantAnimation::valueChanged, controller_,
            [this](const QVariant& value) {
                const qreal progress = value.toReal();
                entrance_offset_ = (1.0 - progress) *
                    cfg_block_metrics::entrance_drop_max();
                entrance_alpha_ = progress;
                update();
            });
    }
    entrance_anim_->start();
}

void CfgBlockItem::setTextSelection(int anchor_line, int extent_line)
{
    text_sel_anchor_ = anchor_line;
    text_sel_extent_ = extent_line;
    update();
}

void CfgBlockItem::clearTextSelection()
{
    text_sel_anchor_ = -1;
    text_sel_extent_ = -1;
    text_sel_dragging_ = false;
    update();
}

void CfgBlockItem::ensure_metrics()
{
    const quint64 revision = theme::disasm_theme_revision();
    if (metrics_theme_revision_ == revision)
        return;
    header_font_ = theme::fonts::codeEm();
    header_font_.setPixelSize((std::max)(1, static_cast<int>(data_.font_size)));
    code_font_ = theme::fonts::codeRegular();
    code_font_.setPixelSize((std::max)(1, static_cast<int>(data_.font_size)));
    header_metrics_ = QFontMetricsF(header_font_);
    code_metrics_ = QFontMetricsF(code_font_);
    metrics_theme_revision_ = revision;
}

void CfgBlockItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
                         QWidget*)
{
    ensure_metrics();
    const auto& theme = theme::disasm_theme_snapshot();
    const auto& t = theme::tokens();
    const QRectF card(QPointF(0.0, 0.0), data_.rect.size());
    const qreal lod = QStyleOptionGraphicsItem::levelOfDetailFromTransform(
        painter->worldTransform());
    const qreal alpha = entrance_alpha_;
    const QRectF exposed = option->exposedRect;
    const qreal card_radius = static_cast<qreal>(t.radius.lg);
    const qreal pad = static_cast<qreal>(t.spacing.sm);
    const qreal dy = entrance_offset_ - hover_lift_;
    const QRectF frame = card.translated(0.0, dy);

    painter->setRenderHint(QPainter::Antialiasing, true);
    QColor shadow = t.title_bar;
    shadow.setAlphaF(0.34 * alpha);
    painter->setPen(Qt::NoPen);
    painter->setBrush(shadow);
    painter->drawRoundedRect(card.translated(cfg_block_metrics::shadow_offset(),
        cfg_block_metrics::shadow_offset()), card_radius, card_radius);

    QColor panel = t.panel_bg;
    if (selected_)
        panel = t.bg_overlay;
    panel.setAlphaF(panel.alphaF() * alpha);
    painter->setBrush(panel);
    painter->drawRoundedRect(frame, card_radius, card_radius);

    const qreal header_height = data_.header_height;
    const QRectF header_rect(frame.x(), frame.y(), frame.width(), header_height);
    QColor header_color = data_.is_entry ? t.accent_dim : t.panel_header;
    header_color.setAlphaF(header_color.alphaF() * alpha);
    painter->setBrush(header_color);
    painter->drawRoundedRect(header_rect, card_radius, card_radius);
    painter->fillRect(QRectF(header_rect.x(),
        header_rect.y() + header_rect.height() * 0.5, header_rect.width(),
        header_rect.height() * 0.5), header_color);

    QColor border = (selected_) ? t.accent
        : (data_.is_exit ? t.warning : t.border_strong);
    painter->setPen(QPen(theme::disasm_with_alpha(border, alpha),
        selected_ ? 2.0 : 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(frame, card_radius, card_radius);

    if (data_.current_rip) {
        QColor glow = t.accent_glow;
        glow.setAlphaF(glow.alphaF() * alpha);
        painter->setPen(QPen(glow, 2.0));
        painter->drawRoundedRect(frame, card_radius, card_radius);
    }

    if (data_.has_breakpoint) {
        QColor bp = t.error;
        bp.setAlphaF(bp.alphaF() * alpha);
        painter->fillRect(QRectF(frame.x(), frame.y() + header_height,
            static_cast<qreal>(t.spacing.xs), frame.height() - header_height), bp);
    }

    if (lod < k_lod_text_threshold)
        return;

    const auto& header_metrics = header_metrics_;
    painter->setFont(header_font_);
    const qreal text_baseline_header = header_rect.y() +
        (header_rect.height() - header_metrics.height()) * 0.5 + header_metrics.ascent();
    qreal confidence_reserve = 0.0;
    QString confidence;
    if (data_.confidence) {
        confidence = QStringLiteral("%1%").arg(*data_.confidence);
        confidence_reserve = header_metrics.horizontalAdvance(confidence) + pad;
    }
    const qreal header_text_width = (std::max)(0.0,
        header_rect.width() - pad - pad - confidence_reserve);
    const QString header_text = header_metrics.elidedText(data_.header,
        Qt::ElideRight, header_text_width);
    painter->setPen(theme::disasm_with_alpha(t.text_primary, alpha));
    painter->drawText(QPointF(header_rect.x() + pad, text_baseline_header),
        header_text);
    if (data_.confidence) {
        const qreal confidence_width = header_metrics.horizontalAdvance(confidence);
        painter->setPen(theme::disasm_with_alpha(t.text_dim, alpha));
        painter->drawText(QPointF(header_rect.right() - confidence_width - pad,
            text_baseline_header), confidence);
    }

    const auto& code_metrics = code_metrics_;
    painter->setFont(code_font_);
    const qreal line_height = data_.row_height;
    const qreal body_top = frame.y() + header_height + data_.body_padding;
    const qreal row_baseline_offset = (line_height - code_metrics.height()) * 0.5 +
        code_metrics.ascent();
    const qreal text_right = frame.x() + frame.width() - pad;
    const QString ellipsis = QStringLiteral("\u2026");
    const qreal ellipsis_width = code_metrics.horizontalAdvance(ellipsis);

    painter->save();
    painter->setClipRect(QRectF(frame.x() + 1.0, frame.y() + header_height,
        frame.width() - 2.0, frame.height() - header_height - 2.0), Qt::IntersectClip);

    const qreal body_floor = frame.bottom() - 2.0;
    qreal inj_y = body_top;
    for (const auto& injection : data_.injections) {
        if (inj_y + line_height > body_floor)
            break;
        if (!exposed.isNull() && inj_y + line_height < exposed.top()) {
            inj_y += line_height;
            continue;
        }
        if (injection.first == static_cast<int>(function_index::injection_t::spacer_line) ||
            injection.second.isEmpty()) {
            inj_y += line_height;
            continue;
        }
        painter->setPen(theme::disasm_with_alpha(
            cfg_injection_color(injection.first, theme), alpha));
        painter->drawText(QPointF(frame.x() + pad, inj_y + row_baseline_offset),
            code_metrics.elidedText(injection.second, Qt::ElideRight,
                (std::max)(0.0, text_right - frame.x() - pad)));
        inj_y += line_height;
    }

    int sel_lo = -1;
    int sel_hi = -1;
    if (text_sel_anchor_ >= 0 && text_sel_extent_ >= 0) {
        sel_lo = (std::min)(text_sel_anchor_, text_sel_extent_);
        sel_hi = (std::max)(text_sel_anchor_, text_sel_extent_);
    }
    qreal row_y = body_top + static_cast<qreal>(data_.injections.size()) * line_height;
    const qreal addr_width = data_.addr_column_width;
    for (std::size_t index = 0; index < data_.rows.size(); ++index) {
        if (row_y + line_height > body_floor)
            break;
        if (!exposed.isNull() &&
            (row_y + line_height < exposed.top() || row_y > exposed.bottom())) {
            row_y += line_height;
            continue;
        }
        const auto& row = data_.rows[index];
        const qreal inset = static_cast<qreal>(t.spacing.xxs);
        const bool line_selected = sel_lo >= 0 &&
            static_cast<int>(index) >= sel_lo && static_cast<int>(index) <= sel_hi;
        if (row.address != 0 && data_.current_rip &&
            row.address == data_.current_rip_address) {
            painter->fillRect(QRectF(frame.x() + inset, row_y,
                frame.width() - inset * 2.0, line_height),
                theme::disasm_with_alpha(t.accent_glow, alpha * 0.22));
        }
        if (line_selected) {
            painter->fillRect(QRectF(frame.x() + inset, row_y,
                frame.width() - inset * 2.0, line_height),
                theme::disasm_with_alpha(t.selection, alpha));
            painter->fillRect(QRectF(frame.x() + inset, row_y, inset, line_height),
                theme::disasm_with_alpha(t.accent, alpha));
        }
        painter->setPen(theme::disasm_with_alpha(theme.address, alpha * 0.85));
        painter->drawText(QPointF(frame.x() + pad, row_y + row_baseline_offset),
            QString::number(row.address, 16).toUpper());
        const QString text = row.text.isEmpty()
            ? QStringLiteral("formatting...") : row.text;
        int position = 0;
        const int length = text.size();
        while (position < length && text[position] <= u' ')
            ++position;
        int mnemonic_end = position;
        while (mnemonic_end < length && text[mnemonic_end] > u' ')
            ++mnemonic_end;
        qreal cursor_x = frame.x() + pad + addr_width;
        bool truncated = false;
        if (mnemonic_end > position) {
            const QString mnemonic = text.mid(position, mnemonic_end - position);
            const qreal mnemonic_width = code_metrics.horizontalAdvance(mnemonic);
            if (cursor_x + mnemonic_width > text_right - ellipsis_width)
                truncated = true;
            else {
                painter->setPen(theme::disasm_with_alpha(theme.mnemonic, alpha));
                painter->drawText(QPointF(cursor_x, row_y + row_baseline_offset),
                    mnemonic);
                cursor_x += mnemonic_width;
            }
        }
        int cursor = mnemonic_end;
        while (!truncated && cursor < length) {
            if (text[cursor] <= u' ') {
                int ws = cursor;
                while (cursor < length && text[cursor] <= u' ')
                    ++cursor;
                cursor_x += code_metrics.horizontalAdvance(text.mid(ws, cursor - ws));
                continue;
            }
            int token_begin = cursor;
            QColor color = t.text_secondary;
            if (text[cursor] == u'[') {
                int depth = 0;
                while (cursor < length) {
                    if (text[cursor] == u'[')
                        ++depth;
                    else if (text[cursor] == u']') {
                        ++cursor;
                        --depth;
                        if (depth <= 0)
                            break;
                        continue;
                    }
                    ++cursor;
                }
                color = theme.reg_ptr;
            } else if (text[cursor] == u'0' && cursor + 1 < length &&
                       (text[cursor + 1] == u'x' || text[cursor + 1] == u'X')) {
                cursor += 2;
                while (cursor < length &&
                       ((text[cursor] >= u'0' && text[cursor] <= u'9') ||
                        (text[cursor] >= u'a' && text[cursor] <= u'f') ||
                        (text[cursor] >= u'A' && text[cursor] <= u'F')))
                    ++cursor;
                color = theme.immediate_num;
            } else if (text[cursor] >= u'0' && text[cursor] <= u'9') {
                while (cursor < length && text[cursor] >= u'0' && text[cursor] <= u'9')
                    ++cursor;
                color = theme.immediate_num;
            } else if ((text[cursor] >= u'a' && text[cursor] <= u'z') ||
                       (text[cursor] >= u'A' && text[cursor] <= u'Z') ||
                       text[cursor] == u'_') {
                while (cursor < length &&
                       ((text[cursor] >= u'a' && text[cursor] <= u'z') ||
                        (text[cursor] >= u'A' && text[cursor] <= u'Z') ||
                        (text[cursor] >= u'0' && text[cursor] <= u'9') ||
                        text[cursor] == u'_'))
                    ++cursor;
                color = theme.reg;
            } else if (text[cursor] == u',' || text[cursor] == u'+' ||
                       text[cursor] == u'-' || text[cursor] == u'*' ||
                       text[cursor] == u':' || text[cursor] == u'.') {
                ++cursor;
                color = t.text_secondary;
            } else {
                ++cursor;
            }
            const QString span = text.mid(token_begin, cursor - token_begin);
            const qreal span_width = code_metrics.horizontalAdvance(span);
            if (cursor_x + span_width > text_right - ellipsis_width) {
                truncated = true;
                break;
            }
            painter->setPen(theme::disasm_with_alpha(color, alpha));
            painter->drawText(QPointF(cursor_x, row_y + row_baseline_offset), span);
            cursor_x += span_width;
        }
        if (truncated) {
            painter->setPen(theme::disasm_with_alpha(t.text_dim, alpha));
            painter->drawText(QPointF(text_right - ellipsis_width,
                row_y + row_baseline_offset), ellipsis);
        }
        row_y += line_height;
    }
    if (data_.total_instructions > data_.rows.size() &&
        row_y + row_baseline_offset < body_floor) {
        painter->setPen(theme::disasm_with_alpha(t.text_dim, alpha));
        painter->drawText(QPointF(frame.x() + pad, row_y + row_baseline_offset),
            QStringLiteral("+ %1 more instructions")
                .arg(data_.total_instructions - data_.rows.size()));
    }
    painter->restore();
}

void CfgBlockItem::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const int line = lineAt(event->pos().y());
        controller_->blockClicked(this, line,
            (event->modifiers() & Qt::ShiftModifier) != 0);
        if (line >= 0) {
            text_sel_dragging_ = true;
            if (event->modifiers() & Qt::ShiftModifier) {
                if (text_sel_anchor_ >= 0)
                    text_sel_extent_ = line;
                else {
                    text_sel_anchor_ = line;
                    text_sel_extent_ = line;
                }
            } else {
                text_sel_anchor_ = line;
                text_sel_extent_ = line;
            }
            update();
        }
        event->accept();
        return;
    }
    QGraphicsItem::mousePressEvent(event);
}

void CfgBlockItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event)
{
    if (text_sel_dragging_ && text_sel_anchor_ >= 0) {
        const int line = lineAt(event->pos().y());
        if (line >= 0 && line != text_sel_extent_) {
            text_sel_extent_ = line;
            update();
        }
        event->accept();
        return;
    }
    QGraphicsItem::mouseMoveEvent(event);
}

void CfgBlockItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && text_sel_dragging_) {
        text_sel_dragging_ = false;
        event->accept();
        return;
    }
    QGraphicsItem::mouseReleaseEvent(event);
}

void CfgBlockItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        controller_->blockDoubleClicked(this, lineAt(event->pos().y()));
        event->accept();
        return;
    }
    QGraphicsItem::mouseDoubleClickEvent(event);
}

void CfgBlockItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
    controller_->blockContextMenu(this, lineAt(event->pos().y()), event->screenPos());
    event->accept();
}

void CfgBlockItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = true;
    if (!theme::AidaMotion::reducedMotion()) {
        if (!hover_anim_) {
            hover_anim_ = new QVariantAnimation(controller_);
            hover_anim_->setStartValue(0.0);
            hover_anim_->setEndValue(1.0);
            hover_anim_->setDuration(theme::tokens().motion.fast);
            hover_anim_->setEasingCurve(theme::easingFor(theme::Ease::OutCubic));
        QObject::connect(hover_anim_, &QVariantAnimation::valueChanged, controller_,
            [this](const QVariant& value) {
                hover_lift_ = value.toReal() * cfg_block_metrics::hover_lift_max();
                update();
            });
        }
        hover_anim_->setDirection(QAbstractAnimation::Forward);
        hover_anim_->start();
    }
    QGraphicsItem::hoverEnterEvent(event);
}

void CfgBlockItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = false;
    if (hover_anim_) {
        hover_anim_->setDirection(QAbstractAnimation::Backward);
        hover_anim_->start();
    } else {
        hover_lift_ = 0.0;
        update();
    }
    QGraphicsItem::hoverLeaveEvent(event);
}

}
