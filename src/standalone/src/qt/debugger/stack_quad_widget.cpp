#include "qt/debugger/stack_quad_widget.hpp"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

#include <cstring>

#include "core/debugger/debugger_engine.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::debugger {

namespace {
constexpr std::size_t k_stack_bytes = 0x100;
}

StackQuadWidget::StackQuadWidget(QWidget* parent)
    : QAbstractScrollArea(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_StaticContents);
    setFocusPolicy(Qt::StrongFocus);
    setFont(theme::fonts::codeRegular());
    viewport()->setMouseTracking(true);
}

int StackQuadWidget::rowHeight() const {
    const auto& grid = theme::fonts::monoGrid();
    if (grid.valid && grid.line_h > 0.0)
        return (std::max)(1, qRound(grid.line_h));
    return theme::tokens().table.compact_row_h;
}

qreal StackQuadWidget::cellWidth() const {
    const auto& grid = theme::fonts::monoGrid();
    if (grid.valid && grid.cell_w > 0.0)
        return grid.cell_w;
    const qreal w = QFontMetricsF(theme::fonts::codeRegular())
        .horizontalAdvance(u'0');
    return w > 0.0 ? w : static_cast<qreal>(theme::tokens().spacing.sm);
}

qreal StackQuadWidget::contentWidth() const {
    return 2.0 * theme::tokens().spacing.lg + 34.0 * cellWidth();
}

void StackQuadWidget::setRsp(std::uint64_t rsp) {
    if (rsp_ == rsp)
        return;
    rsp_ = rsp;
    if (rsp_ == 0)
        clearBytes();
}

void StackQuadWidget::clearBytes() {
    bytes_.clear();
    base_ = 0;
    selected_row_ = -1;
    updateScrollRange();
    viewport()->update();
}

bool StackQuadWidget::tick() {
    if (rsp_ == 0) {
        if (!bytes_.empty())
            clearBytes();
        return false;
    }
    debugger_engine::request_stack_refresh(rsp_, k_stack_bytes, 220);
    std::uint64_t base = 0;
    auto bytes = debugger_engine::cached_stack_bytes(base);
    if (bytes.empty() || base != rsp_) {
        return false;
    }
    bytes_ = std::move(bytes);
    base_ = base;
    updateScrollRange();
    viewport()->update();
    return true;
}

void StackQuadWidget::updateScrollRange() {
    const int rows = (std::max)(1, static_cast<int>(bytes_.size() / 8));
    verticalScrollBar()->setRange(0,
        (std::max)(0, rows * rowHeight() - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(rowHeight());
    const int overflow = (std::max)(0,
        qRound(contentWidth()) - viewport()->width());
    horizontalScrollBar()->setRange(0, overflow);
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setSingleStep(qRound(cellWidth()) * 4);
}

int StackQuadWidget::rowAtY(int y) const {
    const int rows = (std::max)(1, static_cast<int>(bytes_.size() / 8));
    const int row = (y + verticalScrollBar()->value()) / rowHeight();
    return (row >= 0 && row < rows) ? row : -1;
}

void StackQuadWidget::scrollRowVisible(int row) {
    if (row < 0)
        return;
    const int rh = rowHeight();
    const int top = row * rh;
    const int bottom = top + rh;
    auto* bar = verticalScrollBar();
    if (top < bar->value())
        bar->setValue(top);
    else if (bottom > bar->value() + viewport()->height())
        bar->setValue(bottom - viewport()->height());
}

debugger_interaction::context_t StackQuadWidget::contextForRow(int row) const {
    const std::size_t offset = static_cast<std::size_t>(row) * 8U;
    if (row < 0 || offset + 8U > bytes_.size())
        return {};
    std::uint64_t value = 0;
    std::memcpy(&value, bytes_.data() + offset, sizeof(value));
    return debugger_interaction::capture(debugger_interaction::kind_t::stack_slot,
        rsp_ + static_cast<std::uint64_t>(row) * 8ULL, value, row);
}

void StackQuadWidget::mousePressEvent(QMouseEvent* event) {
    const int row = rowAtY(event->pos().y());
    if (event->button() == Qt::LeftButton) {
        selected_row_ = row;
        if (row >= 0)
            Q_EMIT rowSelected(row);
        viewport()->update();
    } else if (event->button() == Qt::RightButton && row >= 0) {
        selected_row_ = row;
        Q_EMIT contextRowRequested(row, event->globalPosition().toPoint());
        viewport()->update();
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void StackQuadWidget::mouseMoveEvent(QMouseEvent* event) {
    const int row = rowAtY(event->pos().y());
    const std::size_t offset = static_cast<std::size_t>(row) * 8U;
    if (row >= 0 && offset + 8U <= bytes_.size()) {
        std::uint64_t value = 0;
        std::memcpy(&value, bytes_.data() + offset, sizeof(value));
        const std::uint64_t address =
            rsp_ + static_cast<std::uint64_t>(row) * 8ULL;
        const QString text = QString::asprintf("RSP+0x%02llX  [%016llX] = %016llX",
            static_cast<unsigned long long>(row * 8),
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(value));
        if (viewport()->toolTip() != text)
            viewport()->setToolTip(text);
    } else if (!viewport()->toolTip().isEmpty()) {
        viewport()->setToolTip(QString());
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

QPoint StackQuadWidget::selectedRowGlobalPos() const {
    const int rh = rowHeight();
    const int y = selected_row_ * rh - verticalScrollBar()->value() + rh / 2;
    return viewport()->mapToGlobal(
        QPoint(viewport()->width() / 2, y));
}

void StackQuadWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Menu ||
        (event->key() == Qt::Key_F10 &&
            event->modifiers().testFlag(Qt::ShiftModifier))) {
        if (selected_row_ >= 0 &&
            selected_row_ < static_cast<int>(bytes_.size() / 8))
            Q_EMIT contextRowRequested(selected_row_, selectedRowGlobalPos());
        event->accept();
        return;
    }
    const int rows = static_cast<int>(bytes_.size() / 8);
    if (rows > 0) {
        const int page = (std::max)(1, viewport()->height() / rowHeight());
        int next = -1;
        switch (event->key()) {
            case Qt::Key_Up:
                next = selected_row_ > 0 ? selected_row_ - 1 : 0;
                break;
            case Qt::Key_Down:
                next = selected_row_ >= 0 && selected_row_ < rows - 1
                    ? selected_row_ + 1 : rows - 1;
                break;
            case Qt::Key_PageUp:
                next = (std::max)(0,
                    (selected_row_ >= 0 ? selected_row_ : 0) - page);
                break;
            case Qt::Key_PageDown:
                next = (std::min)(rows - 1,
                    (selected_row_ >= 0 ? selected_row_ : 0) + page);
                break;
            case Qt::Key_Home:
                next = 0;
                break;
            case Qt::Key_End:
                next = rows - 1;
                break;
            default:
                break;
        }
        if (next >= 0 && next != selected_row_) {
            selected_row_ = next;
            Q_EMIT rowSelected(next);
            scrollRowVisible(next);
            viewport()->update();
            event->accept();
            return;
        }
        if (next >= 0) {
            event->accept();
            return;
        }
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void StackQuadWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (event->reason() == QContextMenuEvent::Keyboard &&
        selected_row_ >= 0 &&
        selected_row_ < static_cast<int>(bytes_.size() / 8)) {
        Q_EMIT contextRowRequested(selected_row_, selectedRowGlobalPos());
        event->accept();
        return;
    }
    QAbstractScrollArea::contextMenuEvent(event);
}

void StackQuadWidget::focusInEvent(QFocusEvent* event) {
    viewport()->update();
    QAbstractScrollArea::focusInEvent(event);
}

void StackQuadWidget::focusOutEvent(QFocusEvent* event) {
    viewport()->update();
    QAbstractScrollArea::focusOutEvent(event);
}

void StackQuadWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(viewport());
    const auto& t = theme::tokens();
    painter.fillRect(event->rect(), t.bg_base);
    const QFont code_font = theme::fonts::codeRegular();
    painter.setFont(code_font);
    const QFontMetricsF fm(code_font);

    if (rsp_ == 0) {
        painter.setPen(t.text_dim);
        painter.drawText(QRectF(event->rect()), Qt::AlignCenter,
            QStringLiteral("RSP is zero (target not paused)."));
        return;
    }
    if (bytes_.empty() || base_ != rsp_) {
        painter.setPen(t.text_dim);
        painter.drawText(QRectF(event->rect()), Qt::AlignCenter,
            QStringLiteral("Reading stack frame..."));
        return;
    }

    const int row_h = rowHeight();
    const qreal cell_w = cellWidth();
    const qreal margin = t.spacing.lg;
    const qreal value_x = margin + 18.0 * cell_w;
    const int rows = (std::max)(1, static_cast<int>(bytes_.size() / 8));
    const int scroll = verticalScrollBar()->value();
    const int hscroll = horizontalScrollBar()->value();
    const int first = (std::max)(0, scroll / row_h);
    const int last = (std::min)(rows - 1,
        (scroll + static_cast<int>(event->rect().height()) + row_h) / row_h + 1);
    const qreal width = viewport()->width();

    for (int i = first; i <= last; ++i) {
        const qreal ry = static_cast<qreal>(i) * row_h - scroll;
        const std::size_t offset = static_cast<std::size_t>(i) * 8U;
        std::uint64_t value = 0;
        if (offset + 8U <= bytes_.size())
            std::memcpy(&value, bytes_.data() + offset, sizeof(value));
        const std::uint64_t address = rsp_ + static_cast<std::uint64_t>(i) * 8ULL;
        const bool is_top = i == 0;
        const bool is_sel = i == selected_row_;

        if (is_top) {
            painter.fillRect(QRectF(0, ry, width, row_h),
                widgets::with_alpha(t.accent_glow, 0.18));
            painter.fillRect(QRectF(0, ry, 3, row_h), t.accent);
        } else if (is_sel) {
            painter.fillRect(QRectF(0, ry, width, row_h), t.selection);
            painter.fillRect(QRectF(0, ry, 3, row_h), t.selection_strong);
        }

        painter.setPen(is_top ? t.accent_hover : t.text_address);
        painter.drawText(QPointF(margin - hscroll,
                ry + widgets::text_baseline_centered(QRectF(0, ry, 1, row_h), fm)),
            QString::asprintf("%016llX", static_cast<unsigned long long>(address)));
        painter.setPen(is_top ? t.text_primary : t.syn_number);
        painter.drawText(QPointF(value_x - hscroll,
                ry + widgets::text_baseline_centered(QRectF(0, ry, 1, row_h), fm)),
            QString::asprintf("%016llX", static_cast<unsigned long long>(value)));
    }

    if (hasFocus()) {
        const QRectF frame = QRectF(viewport()->rect())
            .adjusted(1, 1, -1, -1);
        painter.setPen(QPen(t.border_focus,
            static_cast<qreal>(t.control.focus_ring)));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(frame);
    }
}

}
