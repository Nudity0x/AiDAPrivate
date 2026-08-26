#include "qt/debugger/disasm_slice_widget.hpp"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

#include <cmath>
#include <cstring>

#include "core/debugger/debugger_engine.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::debugger {

DisasmSliceWidget::DisasmSliceWidget(QWidget* parent)
    : QAbstractScrollArea(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_StaticContents);
    setFocusPolicy(Qt::StrongFocus);
    setFont(theme::fonts::codeRegular());
    viewport()->setFont(font());
    viewport()->setMouseTracking(true);
    pulse_anim_ = new QVariantAnimation(this);
    pulse_anim_->setStartValue(0.0);
    pulse_anim_->setKeyValueAt(0.5, 1.0);
    pulse_anim_->setEndValue(0.0);
    pulse_anim_->setDuration(theme::tokens().motion.xl);
    pulse_anim_->setLoopCount(-1);
    connect(pulse_anim_, &QVariantAnimation::valueChanged, this,
        [this](const QVariant& value) {
            pulse_ = 0.55 + 0.45 * value.toReal();
            if (rip_row_ >= 0)
                viewport()->update();
        });
}

int DisasmSliceWidget::rowHeight() const {
    const auto& grid = theme::fonts::monoGrid();
    if (grid.valid && grid.line_h > 0.0)
        return (std::max)(1, qRound(grid.line_h));
    return theme::tokens().table.compact_row_h;
}

void DisasmSliceWidget::setRip(std::uint64_t rip) {
    if (rip_ == rip)
        return;
    rip_ = rip;
    if (rip == 0) {
        clearRows();
    }
}

void DisasmSliceWidget::clearRows() {
    rows_.clear();
    window_base_ = 0;
    rip_row_ = -1;
    selected_row_ = -1;
    anchor_rip_ = 0;
    updatePulse();
    updateScrollRange();
    viewport()->update();
}

bool DisasmSliceWidget::tick() {
    if (rip_ == 0) {
        if (!rows_.empty())
            clearRows();
        return false;
    }
    debugger_engine::request_disasm_refresh(rip_, 220);
    std::uint64_t base = 0;
    auto buf = debugger_engine::cached_disasm_window(base);
    if (buf.empty() || base == 0) {
        if (!rows_.empty())
            clearRows();
        return false;
    }
    if (base == window_base_ && !rows_.empty() && anchor_rip_ == rip_)
        return false;
    window_base_ = base;
    rebuildRows();
    return true;
}

void DisasmSliceWidget::updatePulse() {
    if (rip_row_ >= 0 && isVisible() &&
        !theme::AidaMotion::reducedMotion()) {
        if (pulse_anim_->state() != QAbstractAnimation::Running)
            pulse_anim_->start();
    } else {
        if (pulse_anim_->state() == QAbstractAnimation::Running)
            pulse_anim_->stop();
        pulse_ = 1.0;
    }
}

void DisasmSliceWidget::rebuildRows() {
    std::uint64_t base = 0;
    const auto buf = debugger_engine::cached_disasm_window(base);
    rows_.clear();
    rows_.reserve(64);
    std::size_t offset = 0;
    if (rip_ > base && rip_ < base + buf.size()) {
        offset = static_cast<std::size_t>(rip_ - base);
        if (offset > 0x40)
            offset = static_cast<std::size_t>(rip_ - base) - 0x40;
        else
            offset = 0;
    }
    std::uint64_t cursor_va = base + offset;
    std::size_t cursor = offset;
    while (cursor < buf.size() &&
           static_cast<int>(rows_.size()) < k_max_rows) {
        const int remaining = static_cast<int>(buf.size() - cursor);
        if (remaining <= 0)
            break;
        AsmInstr ins = zydis_decode_one(buf.data() + cursor, remaining, cursor_va);
        decoded_row_t row;
        row.addr = cursor_va;
        row.len = ins.len > 0 ? ins.len : 1;
        row.ins = ins;
        rows_.push_back(row);
        cursor += static_cast<std::size_t>(row.len);
        cursor_va += static_cast<std::uint64_t>(row.len);
    }

    rip_row_ = -1;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].addr == rip_) {
            rip_row_ = static_cast<int>(i);
            break;
        }
    }
    updateScrollRange();
    if (rip_row_ >= 0 && anchor_rip_ != rip_) {
        const int target = (std::max)(0,
            static_cast<int>(rip_row_ * rowHeight() -
                viewport()->height() * 0.35));
        verticalScrollBar()->setValue(target);
        anchor_rip_ = rip_;
    }
    updatePulse();
    viewport()->update();
}

void DisasmSliceWidget::updateScrollRange() {
    const int content_h = static_cast<int>(rows_.size()) * rowHeight();
    verticalScrollBar()->setRange(0,
        (std::max)(0, content_h - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(rowHeight());
}

int DisasmSliceWidget::rowAtY(int y) const {
    const int row = (y + verticalScrollBar()->value()) / rowHeight();
    return (row >= 0 && row < static_cast<int>(rows_.size())) ? row : -1;
}

void DisasmSliceWidget::scrollRowVisible(int row) {
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

void DisasmSliceWidget::selectRow(int row, bool openMenu,
                                  const QPoint& globalPos) {
    selected_row_ = row;
    if (row >= 0) {
        if (openMenu)
            Q_EMIT contextRowRequested(row, globalPos);
        else
            Q_EMIT rowSelected(row);
    }
    viewport()->update();
}

debugger_interaction::context_t DisasmSliceWidget::contextForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return {};
    const auto& item = rows_[static_cast<std::size_t>(row)];
    return debugger_interaction::capture(
        debugger_interaction::kind_t::instruction, item.addr,
        item.ins.branch_target, row, 0,
        static_cast<std::uint64_t>(item.len), item.ins.mnem, item.ins.ops);
}

void DisasmSliceWidget::mousePressEvent(QMouseEvent* event) {
    const int row = rowAtY(event->pos().y());
    if (event->button() == Qt::LeftButton) {
        selectRow(row, false, event->globalPosition().toPoint());
    } else if (event->button() == Qt::RightButton && row >= 0) {
        selectRow(row, true, event->globalPosition().toPoint());
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void DisasmSliceWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    const int row = rowAtY(event->pos().y());
    if (event->button() == Qt::LeftButton && row >= 0) {
        const auto& item = rows_[static_cast<std::size_t>(row)];
        if (item.ins.branch_target != 0)
            Q_EMIT branchFollowRequested(item.ins.branch_target);
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void DisasmSliceWidget::mouseMoveEvent(QMouseEvent* event) {
    const int row = rowAtY(event->pos().y());
    if (row >= 0) {
        const auto& item = rows_[static_cast<std::size_t>(row)];
        const QString text = QStringLiteral("%1  %2 %3")
            .arg(QString::asprintf("%016llX",
                static_cast<unsigned long long>(item.addr)),
                QString::fromLatin1(item.ins.mnem),
                QString::fromLatin1(item.ins.ops));
        if (viewport()->toolTip() != text)
            viewport()->setToolTip(text);
    } else if (!viewport()->toolTip().isEmpty()) {
        viewport()->setToolTip(QString());
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

QPoint DisasmSliceWidget::selectedRowGlobalPos() const {
    const int rh = rowHeight();
    const int y = selected_row_ * rh - verticalScrollBar()->value() + rh / 2;
    return viewport()->mapToGlobal(
        QPoint(viewport()->width() / 2, y));
}

void DisasmSliceWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Menu ||
        (event->key() == Qt::Key_F10 &&
            event->modifiers().testFlag(Qt::ShiftModifier))) {
        if (selected_row_ >= 0 &&
            selected_row_ < static_cast<int>(rows_.size()))
            selectRow(selected_row_, true, selectedRowGlobalPos());
        event->accept();
        return;
    }
    const int rows = static_cast<int>(rows_.size());
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
                next = rip_row_ >= 0 ? rip_row_ : 0;
                break;
            case Qt::Key_End:
                next = rows - 1;
                break;
            default:
                break;
        }
        if (next >= 0 && next != selected_row_) {
            selectRow(next, false, QPoint());
            scrollRowVisible(next);
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

void DisasmSliceWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (event->reason() == QContextMenuEvent::Keyboard &&
        selected_row_ >= 0 &&
        selected_row_ < static_cast<int>(rows_.size())) {
        selectRow(selected_row_, true, selectedRowGlobalPos());
        event->accept();
        return;
    }
    QAbstractScrollArea::contextMenuEvent(event);
}

void DisasmSliceWidget::showEvent(QShowEvent* event) {
    updatePulse();
    QAbstractScrollArea::showEvent(event);
}

void DisasmSliceWidget::hideEvent(QHideEvent* event) {
    if (pulse_anim_->state() == QAbstractAnimation::Running)
        pulse_anim_->stop();
    QAbstractScrollArea::hideEvent(event);
}

void DisasmSliceWidget::focusInEvent(QFocusEvent* event) {
    viewport()->update();
    QAbstractScrollArea::focusInEvent(event);
}

void DisasmSliceWidget::focusOutEvent(QFocusEvent* event) {
    viewport()->update();
    QAbstractScrollArea::focusOutEvent(event);
}

void DisasmSliceWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(viewport());
    const auto& t = theme::tokens();
    const QRectF clip = event->rect();
    painter.fillRect(clip, t.bg_base);

    const QFont code_font = theme::fonts::codeRegular();
    painter.setFont(code_font);
    const QFontMetricsF fm(code_font);
    const qreal cell_w = theme::fonts::monoGrid().cell_w > 0.0
        ? theme::fonts::monoGrid().cell_w : fm.horizontalAdvance(u'0');

    if (rip_ == 0) {
        painter.setPen(t.text_dim);
        painter.drawText(QRectF(clip), Qt::AlignCenter,
            QStringLiteral("RIP is zero (target not paused at a valid instruction)."));
        return;
    }
    if (rows_.empty()) {
        painter.setPen(t.text_dim);
        painter.drawText(QRectF(clip), Qt::AlignCenter,
            QStringLiteral("Fetching instruction stream..."));
        return;
    }

    const int row_h = rowHeight();
    const qreal width = viewport()->width();
    const int scroll = verticalScrollBar()->value();
    const int first = (std::max)(0, scroll / row_h);
    const int last = (std::min)(static_cast<int>(rows_.size()) - 1,
        (scroll + static_cast<int>(clip.height()) + row_h) / row_h + 1);
    const qreal addr_x = 2 * cell_w;
    const qreal bytes_x = addr_x + 17 * cell_w;
    const qreal mnem_x = bytes_x + 26 * cell_w;
    const qreal ops_x = mnem_x + 9 * cell_w;
    const qreal ops_avail = width - ops_x - cell_w;

    for (int i = first; i <= last; ++i) {
        const auto& row = rows_[static_cast<std::size_t>(i)];
        const qreal ry = static_cast<qreal>(i) * row_h - scroll;
        const bool is_rip = row.addr == rip_;
        const bool is_sel = i == selected_row_;

        if (is_rip) {
            painter.fillRect(QRectF(0, ry, width, row_h),
                widgets::with_alpha(t.accent_glow, 0.30));
            painter.fillRect(QRectF(0, ry, 3, row_h), t.accent);
            const QPointF tri[3] = {
                {6, ry + row_h * 0.5 - 4},
                {6, ry + row_h * 0.5 + 4},
                {12, ry + row_h * 0.5},
            };
            painter.setPen(Qt::NoPen);
            painter.setBrush(widgets::with_alpha(t.accent, pulse_));
            painter.drawPolygon(tri, 3);
        } else if (is_sel) {
            painter.fillRect(QRectF(0, ry, width, row_h), t.selection);
            painter.fillRect(QRectF(0, ry, 3, row_h), t.selection_strong);
        }

        painter.setPen(is_rip ? t.accent_hover : t.text_address);
        painter.drawText(QPointF(addr_x,
                ry + widgets::text_baseline_centered(QRectF(0, ry, 1, row_h), fm)),
            QString::asprintf("%016llX", static_cast<unsigned long long>(row.addr)));

        char bytes_buf[40] = {};
        char* bp = bytes_buf;
        const int blen = row.ins.len > 8 ? 8 : row.ins.len;
        for (int b = 0; b < blen; ++b) {
            bp += std::snprintf(bp,
                sizeof(bytes_buf) - static_cast<std::size_t>(bp - bytes_buf),
                "%02X ", static_cast<unsigned>(row.ins.raw[b]));
        }
        if (row.ins.len > 8)
            std::snprintf(bp, sizeof(bytes_buf) -
                static_cast<std::size_t>(bp - bytes_buf), "+");
        painter.setPen(widgets::with_alpha(t.text_dim, 0.85));
        painter.drawText(QPointF(bytes_x,
                ry + widgets::text_baseline_centered(QRectF(0, ry, 1, row_h), fm)),
            QString::fromLatin1(bytes_buf));

        QColor mnemonic;
        if (row.ins.is_call) mnemonic = t.syn_function;
        else if (row.ins.is_branch) mnemonic = t.warning;
        else if (row.ins.is_ret) mnemonic = t.error;
        else if (row.ins.is_priv) mnemonic = t.accent;
        else if (row.ins.is_nop) mnemonic = t.text_dim;
        else mnemonic = t.syn_keyword;
        painter.setPen(mnemonic);
        painter.drawText(QPointF(mnem_x,
                ry + widgets::text_baseline_centered(QRectF(0, ry, 1, row_h), fm)),
            QString::fromLatin1(row.ins.mnem));

        if (row.ins.ops[0] != 0 && ops_avail > cell_w * 4.0) {
            const QString ops = QString::fromLatin1(row.ins.ops);
            painter.setPen(t.text_primary);
            painter.drawText(QPointF(ops_x,
                    ry + widgets::text_baseline_centered(
                        QRectF(0, ry, 1, row_h), fm)),
                fm.horizontalAdvance(ops) > ops_avail
                    ? fm.elidedText(ops, Qt::ElideRight, ops_avail)
                    : ops);
        }
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
