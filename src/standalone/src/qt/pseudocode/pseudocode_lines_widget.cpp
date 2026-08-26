#include "qt/pseudocode/pseudocode_lines_widget.hpp"

#include "qt/analysis_bridge/disasm_workspace_model.hpp"
#include "qt/analysis_bridge/pseudocode_session.hpp"
#include "qt/analysis_bridge/revision_combine.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace aida::qt::pseudocode {

using aida::workbench::pseudocode_document::pseudocode_document_model_t;
using aida::workbench::pseudocode_document::k_pseudocode_document_max_page_lines;

PseudocodeLinesWidget::PseudocodeLinesWidget(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    setObjectName(QStringLiteral("aida.pseudocode.lines"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_StaticContents);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
}

pseudocode_document_model_t* PseudocodeLinesWidget::model() const
{
    return pseudocode_view::document_model(context_);
}

void PseudocodeLinesWidget::setContext(const disasm_view::workspace_context_t& context)
{
    context_ = context;
    reload();
}

void PseudocodeLinesWidget::reload()
{
    const auto code_font = theme::fonts::codeRegular();
    const QFontMetricsF metrics(code_font);
    char_width_ = (std::max)(1.0, metrics.horizontalAdvance(u'0'));
    const auto& t = theme::tokens();
    row_height_ = (std::max)(metrics.lineSpacing() + t.spacing.xs - 1.0,
        static_cast<qreal>(t.table.compact_row_h));
    total_lines_ = 0;
    if (auto* model_ptr = model()) {
        aida::workbench::pseudocode_document::pseudocode_page_t first_page;
        if (model_ptr->page({0, 1}, first_page))
            total_lines_ = first_page.total_lines;
    }
    max_text_width_ = 0.0;
    updateGutterMetrics();
    const auto selection = pseudocode_view::line_selection(context_);
    selected_line_ = selection.selected_line;
    selected_token_begin_ = selection.selected_token_begin;
    selected_token_end_ = selection.selected_token_end;
    if (selected_line_ >= static_cast<int>(total_lines_)) {
        selected_line_ = -1;
        selected_token_begin_ = 0;
        selected_token_end_ = 0;
    }
    updateScrollbars();
    viewport()->update();
}

void PseudocodeLinesWidget::updateGutterMetrics()
{
    const auto& t = theme::tokens();
    const QFontMetricsF metrics(theme::fonts::codeRegular());
    int digits = 4;
    for (std::uint32_t value = total_lines_; value >= 10000; value /= 10)
        ++digits;
    gutter_text_pad_ = t.spacing.sm;
    text_pad_ = t.spacing.sm + t.spacing.xxs;
    gutter_width_ = char_width_ * digits + 2.0 * gutter_text_pad_ +
        t.status_bar.dot + t.spacing.xs;
}

void PseudocodeLinesWidget::updateScrollbars()
{
    const int visible_rows = (std::max)(1,
        viewport()->height() / (std::max)(1, static_cast<int>(row_height_)));
    const int maximum = total_lines_ > static_cast<std::uint32_t>(visible_rows)
        ? static_cast<int>(total_lines_ - static_cast<std::uint32_t>(visible_rows)) : 0;
    verticalScrollBar()->setRange(0, maximum);
    verticalScrollBar()->setSingleStep(3);
    verticalScrollBar()->setPageStep(visible_rows);
    const auto& t = theme::tokens();
    const qreal content_width = gutter_width_ + text_pad_ + max_text_width_ +
        t.spacing.sm;
    const int horizontal_maximum = (std::max)(0,
        static_cast<int>(std::ceil(content_width - viewport()->width())));
    horizontalScrollBar()->setRange(0, horizontal_maximum);
    horizontalScrollBar()->setSingleStep(
        (std::max)(1, static_cast<int>(char_width_)));
    horizontalScrollBar()->setPageStep(
        (std::max)(1, viewport()->width() - static_cast<int>(char_width_)));
}

void PseudocodeLinesWidget::scrollContentsBy(int dx, int dy)
{
    if (dy != 0)
        viewport()->scroll(0, dy * static_cast<int>(row_height_));
    if (dx != 0)
        viewport()->update();
}

bool PseudocodeLinesWidget::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::Leave) {
        if (hover_line_ != -1) {
            hover_line_ = -1;
            viewport()->update();
        }
        QToolTip::hideText();
    }
    return QAbstractScrollArea::viewportEvent(event);
}

void PseudocodeLinesWidget::focusInEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusInEvent(event);
    viewport()->update();
}

void PseudocodeLinesWidget::focusOutEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusOutEvent(event);
    viewport()->update();
}

int PseudocodeLinesWidget::lineAt(const QPointF& pos) const
{
    if (pos.y() < 0.0)
        return -1;
    const auto row = static_cast<int>(pos.y() / row_height_) +
        verticalScrollBar()->value();
    return row < static_cast<int>(total_lines_) ? row : -1;
}

qreal PseudocodeLinesWidget::contentX(const QPointF& viewport_pos) const
{
    return viewport_pos.x() - gutter_width_ - text_pad_ +
        horizontalScrollBar()->value();
}

void PseudocodeLinesWidget::ensureLineVisible(int line_index)
{
    if (line_index < 0)
        return;
    auto* bar = verticalScrollBar();
    const int visible_rows = (std::max)(1,
        viewport()->height() / (std::max)(1, static_cast<int>(row_height_)));
    if (line_index < bar->value())
        bar->setValue(line_index);
    else if (line_index >= bar->value() + visible_rows)
        bar->setValue(line_index - visible_rows + 1);
}

QColor PseudocodeLinesWidget::tokenColor(
    aida::analysis::decompiler_document_token_kind_t kind) const
{
    const auto& t = theme::tokens();
    using token_kind_t = aida::analysis::decompiler_document_token_kind_t;
    switch (kind) {
    case token_kind_t::keyword: return t.syn_keyword;
    case token_kind_t::identifier: return t.syn_identifier;
    case token_kind_t::type_name: return t.syn_type;
    case token_kind_t::literal: return t.syn_number;
    case token_kind_t::operator_token: return t.syn_operator;
    case token_kind_t::punctuation: return t.text_secondary;
    case token_kind_t::whitespace: return t.text_primary;
    case token_kind_t::unknown: return t.warning;
    }
    return t.text_primary;
}

bool PseudocodeLinesWidget::buildLineLayout(int line_index, line_layout_t& output)
{
    auto* model_ptr = model();
    if (!model_ptr)
        return false;
    aida::workbench::pseudocode_document::pseudocode_page_t page;
    if (!model_ptr->page({static_cast<std::uint32_t>(line_index), 1}, page) ||
        page.lines.empty())
        return false;
    output.line = page.lines.front();
    output.page = page;
    output.spans.clear();
    const auto code_font = theme::fonts::codeRegular();
    const QFontMetricsF metrics(code_font);
    qreal x = 0.0;
    std::uint32_t rendered_until = output.line.text_begin;
    for (const auto& token : page.tokens) {
        const auto begin = (std::max)(token.range.begin, output.line.text_begin);
        const auto end = (std::min)(token.range.end, output.line.text_end);
        if (begin >= end)
            continue;
        if (begin > rendered_until) {
            x += metrics.horizontalAdvance(QString::fromStdString(
                output.line.text.substr(rendered_until - output.line.text_begin,
                    begin - rendered_until)));
        }
        const auto local_begin = begin - output.line.text_begin;
        const auto local_end = end - output.line.text_begin;
        if (local_end > output.line.text.size())
            continue;
        const QString span_text = QString::fromStdString(
            output.line.text.substr(local_begin, local_end - local_begin));
        const qreal span_width = metrics.horizontalAdvance(span_text);
        token_span_t span;
        span.begin = token.range.begin;
        span.end = token.range.end;
        span.rect = QRectF(x, 0.0, (std::max)(span_width, 2.0), row_height_);
        span.text = span_text;
        span.address = pseudocode_view::token_source_address(*model_ptr, &token,
            output.line, page);
        output.spans.push_back(span);
        x += span_width;
        rendered_until = (std::max)(rendered_until, end);
    }
    if (rendered_until < output.line.text_end) {
        x += metrics.horizontalAdvance(QString::fromStdString(
            output.line.text.substr(rendered_until - output.line.text_begin)));
    }
    output.text_width = x;
    if (x > max_text_width_) {
        max_text_width_ = x;
        updateScrollbars();
    }
    return true;
}

void PseudocodeLinesWidget::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    const auto& t = theme::tokens();
    painter.fillRect(event->rect(), t.bg_base);
    auto* model_ptr = model();
    if (!model_ptr) {
        if (hasFocus()) {
            const qreal ring = static_cast<qreal>(t.control.focus_ring);
            painter.setPen(QPen(t.border_focus, ring));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(viewport()->rect()).adjusted(
                ring * 0.5, ring * 0.5, -ring * 0.5, -ring * 0.5));
        }
        return;
    }
    const auto code_font = theme::fonts::codeRegular();
    painter.setFont(code_font);
    const QFontMetricsF metrics(code_font);
    const qreal baseline_dy = (row_height_ - metrics.height()) * 0.5 + metrics.ascent();
    const int first = verticalScrollBar()->value();
    const int visible = viewport()->height() / (std::max)(1,
        static_cast<int>(row_height_)) + 2;
    const int last = (std::min)(static_cast<int>(total_lines_), first + visible);
    const auto& clip = event->rect();
    const int clip_first = (std::max)(first,
        first + (std::max)(0, clip.top()) / (std::max)(1, static_cast<int>(row_height_)));
    const int clip_last = (std::min)(last,
        first + (std::max)(0, clip.bottom()) / (std::max)(1, static_cast<int>(row_height_)) + 1);
    const qreal viewport_width = viewport()->width();
    const qreal viewport_height = viewport()->height();
    const int hscroll = horizontalScrollBar()->value();
    const qreal dot_radius = t.status_bar.dot * 0.35;

    QVector<aida::workbench::pseudocode_document::pseudocode_page_t> pages;
    std::uint32_t page_first = static_cast<std::uint32_t>((std::max)(0, clip_first));
    while (page_first < static_cast<std::uint32_t>(clip_last)) {
        const auto count = (std::min)(
            static_cast<std::uint32_t>(clip_last) - page_first,
            k_pseudocode_document_max_page_lines);
        aida::workbench::pseudocode_document::pseudocode_page_t page;
        if (!model_ptr->page({page_first, count}, page))
            break;
        page_first += count;
        pages.push_back(std::move(page));
    }

    qreal widest_seen = max_text_width_;
    for (const auto& page : pages) {
        for (const auto& line : page.lines) {
            const int line_index = static_cast<int>(line.line_number - 1U);
            const qreal row_y = (static_cast<qreal>(line_index) -
                static_cast<qreal>(first)) * row_height_;
            const QRectF row_rect(0.0, row_y, viewport_width, row_height_);
            if (selected_line_ == line_index)
                painter.fillRect(row_rect, t.selection);
            else if (hover_line_ == line_index)
                painter.fillRect(row_rect, t.hover_wash);
            painter.fillRect(QRectF(0.0, row_y, gutter_width_, row_height_),
                t.bg_elevated);
            painter.setPen(t.border_subtle);
            painter.drawLine(QPointF(gutter_width_, row_y),
                QPointF(gutter_width_, row_y + row_height_));
            painter.setPen(selected_line_ == line_index ? t.text_primary : t.text_lineno);
            const QString number_text = QString::number(line.line_number);
            const qreal number_width = metrics.horizontalAdvance(number_text);
            painter.drawText(QPointF(gutter_width_ - gutter_text_pad_ - number_width,
                row_y + baseline_dy), number_text);
            const auto line_address = pseudocode_view::line_source_address(*model_ptr,
                line, page);
            if (line_address) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(t.accent);
                painter.drawEllipse(QPointF(gutter_text_pad_ * 0.5,
                    row_y + row_height_ * 0.5), dot_radius, dot_radius);
            }
            const qreal line_width = metrics.horizontalAdvance(
                QString::fromStdString(line.text));
            if (line_width > widest_seen)
                widest_seen = line_width;
        }
    }

    painter.save();
    painter.setClipRect(QRectF(gutter_width_, 0.0,
        (std::max)(0.0, viewport_width - gutter_width_), viewport_height));
    painter.translate(gutter_width_ + text_pad_ - hscroll, 0.0);
    for (const auto& page : pages) {
        for (const auto& line : page.lines) {
            const int line_index = static_cast<int>(line.line_number - 1U);
            const qreal row_y = (static_cast<qreal>(line_index) -
                static_cast<qreal>(first)) * row_height_;
            const qreal text_origin_y = row_y + baseline_dy;
            std::uint32_t rendered_until = line.text_begin;
            qreal x = 0.0;
            for (const auto& token : page.tokens) {
                const auto begin = (std::max)(token.range.begin, line.text_begin);
                const auto end = (std::min)(token.range.end, line.text_end);
                if (begin >= end)
                    continue;
                if (begin > rendered_until) {
                    const QString gap_text = QString::fromStdString(line.text.substr(
                        rendered_until - line.text_begin, begin - rendered_until));
                    painter.setPen(t.text_primary);
                    painter.drawText(QPointF(x, text_origin_y), gap_text);
                    x += metrics.horizontalAdvance(gap_text);
                }
                const auto local_begin = begin - line.text_begin;
                const auto local_end = end - line.text_begin;
                if (local_end > line.text.size())
                    continue;
                const QString span_text = QString::fromStdString(
                    line.text.substr(local_begin, local_end - local_begin));
                const qreal span_width = metrics.horizontalAdvance(span_text);
                if (selected_token_begin_ == token.range.begin &&
                    selected_token_end_ == token.range.end &&
                    selected_line_ == line_index && span_width > 0.0) {
                    QColor token_fill = t.selection_strong;
                    token_fill.setAlphaF(0.30);
                    painter.fillRect(QRectF(x, row_y, span_width, row_height_),
                        token_fill);
                    QColor token_edge = t.selection_strong;
                    token_edge.setAlphaF(0.85);
                    painter.fillRect(QRectF(x, row_y + row_height_ - 2.0,
                        span_width, 2.0), token_edge);
                }
                painter.setPen(tokenColor(token.kind));
                painter.drawText(QPointF(x, text_origin_y), span_text);
                x += span_width;
                rendered_until = (std::max)(rendered_until, end);
            }
            if (rendered_until < line.text_end) {
                painter.setPen(t.text_primary);
                painter.drawText(QPointF(x, text_origin_y), QString::fromStdString(
                    line.text.substr(rendered_until - line.text_begin)));
            }
        }
    }
    painter.restore();

    if (widest_seen > max_text_width_) {
        max_text_width_ = widest_seen;
        updateScrollbars();
    }
    if (hasFocus()) {
        const qreal ring = static_cast<qreal>(t.control.focus_ring);
        painter.setPen(QPen(t.border_focus, ring));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(viewport()->rect()).adjusted(
            ring * 0.5, ring * 0.5, -ring * 0.5, -ring * 0.5));
    }
}

void PseudocodeLinesWidget::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollbars();
}

void PseudocodeLinesWidget::applyLineSelection(const line_layout_t& layout,
    std::uint32_t token_begin, std::uint32_t token_end,
    std::optional<std::uint64_t> source_address)
{
    auto* model_ptr = model();
    if (!model_ptr)
        return;
    const int line_index = static_cast<int>(layout.line.line_number - 1U);
    selected_line_ = line_index;
    selected_token_begin_ = token_begin;
    selected_token_end_ = token_end;
    pseudocode_view::set_line_selection(context_, line_index, token_begin, token_end);
    aida::workbench::pseudocode_document::pseudocode_selection_t selection;
    selection.line_number = layout.line.line_number;
    if (source_address) {
        selection.kind = aida::workbench::selection_kind_t::address;
        selection.has_address = true;
        selection.address = *source_address;
        selection.token_begin = token_begin;
        selection.token_end = (std::max)(token_end, token_begin + 1U);
    } else if (layout.line.text_end > layout.line.text_begin) {
        selection.kind = aida::workbench::selection_kind_t::source;
        selection.token_begin = token_begin;
        selection.token_end = token_end;
    }
    if (selection.kind != aida::workbench::selection_kind_t::none)
        static_cast<void>(model_ptr->select(selection));
    const auto tab = pseudocode_view::active_tab_view(context_);
    pseudocode_view::persist_line_selection(context_,
        tab ? tab->address : 0,
        tab ? std::string_view(tab->entity_locator) : std::string_view(),
        layout.line, source_address);
    if (source_address) {
        const auto typed = pseudocode_view::typed_source_address(context_, *source_address);
        if (typed)
            disasm_view::select_address(*typed, context_, false);
    }
    Q_EMIT lineSelectionChanged();
    viewport()->update();
}

void PseudocodeLinesWidget::moveLineSelection(int line_index)
{
    auto* model_ptr = model();
    if (!model_ptr || total_lines_ == 0)
        return;
    const int clamped = (std::max)(0,
        (std::min)(line_index, static_cast<int>(total_lines_) - 1));
    line_layout_t layout;
    if (!buildLineLayout(clamped, layout))
        return;
    const auto source_address = pseudocode_view::line_source_address(*model_ptr,
        layout.line, layout.page);
    applyLineSelection(layout, layout.line.text_begin, layout.line.text_end,
        source_address);
    ensureLineVisible(clamped);
}

std::optional<std::uint64_t> PseudocodeLinesWidget::selectionSourceAddress()
{
    auto* model_ptr = model();
    if (!model_ptr || selected_line_ < 0)
        return std::nullopt;
    line_layout_t layout;
    if (!buildLineLayout(selected_line_, layout))
        return std::nullopt;
    for (const auto& span : layout.spans) {
        if (span.begin == selected_token_begin_ && span.address)
            return span.address;
    }
    return pseudocode_view::line_source_address(*model_ptr, layout.line, layout.page);
}

void PseudocodeLinesWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    auto* model_ptr = model();
    const int line_index = lineAt(event->position());
    if (!model_ptr || line_index < 0) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    line_layout_t layout;
    if (!buildLineLayout(line_index, layout))
        return;
    const qreal content_x = contentX(event->position());
    std::uint32_t token_begin = layout.line.text_begin;
    std::uint32_t token_end = layout.line.text_end;
    std::optional<std::uint64_t> source_address;
    for (const auto& span : layout.spans) {
        if (content_x >= span.rect.x() &&
            content_x < span.rect.x() + (std::max)(span.rect.width(), 2.0)) {
            token_begin = span.begin;
            token_end = span.end;
            source_address = span.address;
            break;
        }
    }
    if (!source_address)
        source_address = pseudocode_view::line_source_address(*model_ptr, layout.line,
            layout.page);
    applyLineSelection(layout, token_begin, token_end, source_address);
}

void PseudocodeLinesWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const int line_index = lineAt(event->position());
    if (line_index < 0)
        return;
    line_layout_t layout;
    if (!buildLineLayout(line_index, layout))
        return;
    const qreal content_x = contentX(event->position());
    std::optional<std::uint64_t> source_address;
    for (const auto& span : layout.spans) {
        if (content_x >= span.rect.x() &&
            content_x < span.rect.x() + (std::max)(span.rect.width(), 2.0)) {
            source_address = span.address;
            break;
        }
    }
    if (source_address)
        Q_EMIT navigateToDisassembly(*source_address);
}

void PseudocodeLinesWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int line_index = lineAt(event->position());
    if (line_index != hover_line_) {
        hover_line_ = line_index;
        viewport()->update();
    }
    if (line_index >= 0) {
        const qreal content_x = contentX(event->position());
        line_layout_t layout;
        if (buildLineLayout(line_index, layout)) {
            for (const auto& span : layout.spans) {
                if (content_x >= span.rect.x() &&
                    content_x < span.rect.x() + (std::max)(span.rect.width(), 2.0) &&
                    span.address) {
                    QToolTip::showText(event->globalPosition().toPoint(),
                        span.text + QStringLiteral("\nMapped address  0x") +
                            QString::fromStdString(
                                pseudocode_view::canonical_address_text(*span.address)) +
                            QStringLiteral("\nEnter: disassembly   Space: graph"),
                        viewport());
                    QAbstractScrollArea::mouseMoveEvent(event);
                    return;
                }
            }
        }
    }
    QToolTip::hideText();
    QAbstractScrollArea::mouseMoveEvent(event);
}

void PseudocodeLinesWidget::contextMenuEvent(QContextMenuEvent* event)
{
    const auto origin = event->reason() == QContextMenuEvent::Keyboard
        ? aida::ui::context_menu_open_origin_t::menu_key
        : aida::ui::context_menu_open_origin_t::pointer;
    int line_index = selected_line_;
    QPoint global_pos = event->globalPos();
    if (origin == aida::ui::context_menu_open_origin_t::pointer) {
        const int hit = lineAt(event->pos());
        if (hit < 0)
            return;
        line_index = hit;
    } else if (line_index < 0) {
        return;
    } else {
        const qreal local_y = (static_cast<qreal>(line_index) -
            static_cast<qreal>(verticalScrollBar()->value())) * row_height_;
        global_pos = viewport()->mapToGlobal(QPoint(0,
            static_cast<int>(local_y + row_height_)));
    }
    openLineContextMenu(line_index, origin, global_pos);
}

void PseudocodeLinesWidget::keyPressEvent(QKeyEvent* event)
{
    const bool copy_pressed = (event->modifiers() & Qt::ControlModifier) &&
        (event->key() == Qt::Key_C || event->key() == Qt::Key_Insert);
    if (copy_pressed && selected_line_ >= 0) {
        auto* model_ptr = model();
        line_layout_t layout;
        if (model_ptr && buildLineLayout(selected_line_, layout)) {
            std::optional<std::uint64_t> source_address;
            std::string token_text;
            for (const auto& span : layout.spans) {
                if (span.begin == selected_token_begin_) {
                    source_address = span.address;
                    token_text = span.text.toStdString();
                    break;
                }
            }
            if (!source_address)
                source_address = pseudocode_view::line_source_address(*model_ptr,
                    layout.line, layout.page);
            aida::ui::analysis_context_menu::execute_shortcut(
                makeLineContextMenu(layout.line, source_address, std::move(token_text),
                    selected_line_, selected_token_begin_, selected_token_end_, {}),
                "analysis.copy.text");
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_N && !event->modifiers()) {
        auto* model_ptr = model();
        line_layout_t layout;
        if (model_ptr && selected_line_ >= 0 &&
            buildLineLayout(selected_line_, layout)) {
            const auto candidate = pseudocode_view::local_rename_candidate(*model_ptr,
                layout.page, selected_token_begin_, context_);
            if (!candidate.empty() && rename_local_handler_) {
                rename_local_handler_(candidate);
                event->accept();
                return;
            }
        }
    }
    if (!event->modifiers() || event->modifiers() == Qt::ControlModifier) {
        const int visible_rows = (std::max)(1,
            viewport()->height() / (std::max)(1, static_cast<int>(row_height_)));
        const int anchor = selected_line_ >= 0 ? selected_line_
            : verticalScrollBar()->value();
        bool navigated = true;
        switch (event->key()) {
        case Qt::Key_Up: moveLineSelection(anchor - 1); break;
        case Qt::Key_Down: moveLineSelection(anchor + 1); break;
        case Qt::Key_PageUp: moveLineSelection(anchor - visible_rows); break;
        case Qt::Key_PageDown: moveLineSelection(anchor + visible_rows); break;
        case Qt::Key_Home: moveLineSelection(0); break;
        case Qt::Key_End:
            moveLineSelection(static_cast<int>(total_lines_) - 1);
            break;
        default: navigated = false; break;
        }
        if (navigated) {
            event->accept();
            return;
        }
    }
    if (!event->modifiers() &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        if (const auto address = selectionSourceAddress()) {
            Q_EMIT navigateToDisassembly(*address);
            event->accept();
            return;
        }
    }
    if (!event->modifiers() && event->key() == Qt::Key_Space) {
        if (const auto address = selectionSourceAddress()) {
            Q_EMIT navigateToGraph(*address);
            event->accept();
            return;
        }
    }
    if (!event->modifiers() && event->key() == Qt::Key_F5) {
        pseudocode_view::refresh_active_tab(context_);
        event->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PseudocodeLinesWidget::copyAll()
{
    auto* model_ptr = model();
    if (!model_ptr)
        return;
    const auto* cached = model_ptr->cached_document();
    if (!cached || !cached->document)
        return;
    aida::qt::clipboard::set_text(QString::fromStdString(cached->document->rendered_text));
}

void PseudocodeLinesWidget::openLineContextMenu(int line_index,
    aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos)
{
    auto* model_ptr = model();
    if (!model_ptr)
        return;
    line_layout_t layout;
    if (!buildLineLayout(line_index, layout))
        return;
    std::optional<std::uint64_t> source_address;
    std::string token_text;
    for (const auto& span : layout.spans) {
        if (span.begin == selected_token_begin_) {
            source_address = span.address;
            token_text = span.text.toStdString();
            break;
        }
    }
    if (!source_address)
        source_address = pseudocode_view::line_source_address(*model_ptr, layout.line,
            layout.page);
    std::string rename_candidate;
    {
        aida::workbench::pseudocode_document::pseudocode_page_t page;
        if (model_ptr->page({static_cast<std::uint32_t>(line_index), 1}, page))
            rename_candidate = pseudocode_view::local_rename_candidate(*model_ptr, page,
                selected_token_begin_, context_);
    }
    aida::ui::analysis_context_menu::open(
        makeLineContextMenu(layout.line, source_address, std::move(token_text),
            line_index, selected_token_begin_, selected_token_end_,
            std::move(rename_candidate)), origin, global_pos, this);
}

void PseudocodeLinesWidget::set_rename_local_handler(
    std::function<void(std::string old_name)> handler)
{
    rename_local_handler_ = std::move(handler);
}

aida::ui::analysis_context_menu::context_t PseudocodeLinesWidget::makeLineContextMenu(
    const aida::workbench::pseudocode_document::pseudocode_line_view_t& line,
    std::optional<std::uint64_t> source_address, std::string token_text,
    int selected_line, std::uint32_t selected_token_begin,
    std::uint32_t selected_token_end, std::string rename_candidate)
{
    using namespace aida::ui::analysis_context_menu;
    using aida::ui::action_handler_result_t;
    using aida::ui::capability_state_t;
    const auto context = context_;
    context_t menu;
    menu.kind = menu_kind_t::pseudocode;
    menu.entity_id = "pseudocode-line:" + std::to_string(line.line_number) + ":" +
        std::to_string(selected_token_begin) + ":" +
        std::to_string(selected_token_end) + ":" +
        std::to_string(source_address.value_or(0));
    menu.generation = aida::analysis_bridge::combine_generation_revision(
        context.workspace->generation(), context.workspace->analysis_revision());
    menu.live_generation = [workspace = context.workspace]() {
        return aida::analysis_bridge::combine_generation_revision(workspace->generation(),
            workspace->analysis_revision());
    };
    menu.validate_identity = [this, selected_line, selected_token_begin,
                              selected_token_end]() {
        const auto current = pseudocode_view::line_selection(context_);
        return current.selected_line == selected_line &&
            current.selected_token_begin == selected_token_begin &&
            current.selected_token_end == selected_token_end
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The selected pseudocode token changed");
    };
    auto copy = [&](const char* id, std::string value) {
        menu.actions[id].invoke = [value = std::move(value)]() {
            aida::qt::clipboard::set_text(QString::fromStdString(value));
            return action_handler_result_t::completed();
        };
    };
    copy("analysis.copy.line", line.text);
    copy("analysis.copy.text", token_text.empty() ? line.text : token_text);
    copy("analysis.export.line", line.text);
    menu.actions["analysis.navigate.back"].invoke = [context]() {
        disasm_view::navigate_back(context);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.forward"].invoke = [context]() {
        disasm_view::navigate_forward(context);
        return action_handler_result_t::completed();
    };
    const auto focus = []() {
        return analysis_bridge::view_focus_hook();
    };
    menu.actions["analysis.navigate.functions"].invoke = [focus]() {
        const auto hook = focus();
        if (hook)
            hook("view.analysis.functions");
        return hook ? action_handler_result_t::completed()
            : action_handler_result_t::failed(
                "The canonical Functions view could not be opened");
    };
    menu.actions["analysis.navigate.structures"].invoke = [focus]() {
        const auto hook = focus();
        if (hook)
            hook("view.types.structures");
        return hook ? action_handler_result_t::completed()
            : action_handler_result_t::failed(
                "The canonical Structures view could not be opened");
    };
    menu.actions["analysis.navigate.types"].invoke = [focus]() {
        const auto hook = focus();
        if (hook)
            hook("view.types.inferred");
        return hook ? action_handler_result_t::completed()
            : action_handler_result_t::failed(
                "The canonical Types view could not be opened");
    };
    if (source_address) {
        const auto source = *source_address;
        copy("analysis.copy.address",
            pseudocode_view::canonical_address_text(source));
        copy("analysis.copy.address_va",
            "0x" + pseudocode_view::canonical_address_text(source));
        menu.actions["analysis.navigate.disassembly"].invoke = [this, source]() {
            Q_EMIT navigateToDisassembly(source);
            return action_handler_result_t::completed();
        };
        menu.actions["analysis.navigate.graph"].invoke = [this, source]() {
            Q_EMIT navigateToGraph(source);
            return action_handler_result_t::completed();
        };
        menu.actions["analysis.navigate.pseudocode"].invoke = [context, source, focus]() {
            pseudocode_view::request_decompile(context, source, false);
            const auto hook = focus();
            if (hook)
                hook("document.pseudocode");
            return action_handler_result_t::completed();
        };
    }
    const auto typed = source_address
        ? pseudocode_view::typed_source_address(context_, *source_address) : std::nullopt;
    if (typed) {
        const auto value = *typed;
        const auto runtime =
            disasm_view::runtime_address(context_, value).value_or(value.value);
        menu.actions["analysis.navigate.hex"].invoke = [context, value]() {
            return pseudocode_view::navigate_to_hex(context, value)
                ? action_handler_result_t::completed()
                : action_handler_result_t::failed(
                    "The selected address is unavailable in the Hex document");
        };
        menu.actions["analysis.modify.rename"].invoke = [context, value]() {
            disasm_view::request_rename_dialog(context, value);
            return action_handler_result_t::completed();
        };
        menu.actions["analysis.modify.comment"].invoke = [context, value]() {
            disasm_view::request_comment_dialog(context, value);
            return action_handler_result_t::completed();
        };
        menu.actions["analysis.navigate.xrefs"].invoke = [context, runtime, focus]() {
            disasm_view::open_xrefs(runtime, context);
            const auto hook = focus();
            if (hook)
                hook("view.analysis.references");
            return action_handler_result_t::completed();
        };
        const auto name = disasm_view::resolve_name(context_, value);
        if (!name.empty())
            copy("analysis.copy.name", name);
        menu.actions["analysis.modify.bookmark"].invoke = [context, value]() {
            return disasm_view::queue_bookmark(context, value, {})
                ? action_handler_result_t::completed()
                : action_handler_result_t::failed(
                    "The workspace rejected the bookmark request");
        };
    }
    menu.actions["analysis.modify.retype"].capability =
        capability_state_t::unavailable(
            "Use Types > Apply Type until canonical type-entry validation is available here");
    if (!rename_candidate.empty()) {
        menu.actions["analysis.modify.rename_local"].invoke =
            [this, rename_candidate = std::move(rename_candidate)]() {
                if (rename_local_handler_)
                    rename_local_handler_(rename_candidate);
                return action_handler_result_t::completed();
            };
    } else {
        menu.actions["analysis.modify.rename_local"].capability =
            capability_state_t::unavailable(
                "Select a function-local identifier token to rename it in pseudocode");
    }
    return menu;
}

}
