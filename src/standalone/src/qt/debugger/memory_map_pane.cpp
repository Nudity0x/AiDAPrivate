#include "qt/debugger/memory_map_pane.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_view.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/scanner/memory_interaction_context.hpp"

#include "qt/debugger/debugger_models.hpp"
#include "qt/debugger/debugger_mutation_queue.hpp"
#include "qt/debugger/debugger_selection_bridge.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::debugger {

VaMapCanvas::VaMapCanvas(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(3 * theme::tokens().panel.header_h);
}

QSize VaMapCanvas::minimumSizeHint() const {
    return {2 * static_cast<int>(theme::tokens().shell.min_panel_w),
        3 * theme::tokens().panel.header_h};
}

void VaMapCanvas::setRegions(
    std::shared_ptr<const std::vector<debugger_engine::memory_region_t>>
        regions,
    const std::vector<int>& filteredIndices) {
    regions_ = std::move(regions);
    filtered_ = filteredIndices;
    rebuildSegments();
    update();
}

void VaMapCanvas::clearSelection() {
    selected_segment_ = -1;
    selected_region_ = -1;
    update();
}

void VaMapCanvas::rebuildSegments() {
    segments_.clear();
    if (!regions_)
        return;
    std::uint64_t total = 0;
    for (const int idx : filtered_) {
        if (idx < 0 || static_cast<std::size_t>(idx) >= regions_->size())
            continue;
        total += (*regions_)[static_cast<std::size_t>(idx)].size;
    }
    if (total == 0)
        return;
    const qreal pad = theme::tokens().panel.padding;
    const qreal strip_w = width() - pad * 2;
    double cumulative = 0.0;
    segments_.reserve(filtered_.size());
    for (const int idx : filtered_) {
        if (idx < 0 || static_cast<std::size_t>(idx) >= regions_->size())
            continue;
        const auto& region = (*regions_)[static_cast<std::size_t>(idx)];
        const double frac = static_cast<double>(region.size) /
            static_cast<double>(total);
        segment_t segment;
        segment.base = region.base;
        segment.size = region.size;
        segment.protect = region.protect;
        segment.state = region.state;
        segment.type = region.type;
        segment.region_index = idx;
        segment.start_x = pad + static_cast<qreal>(cumulative) * strip_w;
        segment.width = static_cast<qreal>(frac) * strip_w;
        segments_.push_back(segment);
        cumulative += frac;
    }
}

QColor VaMapCanvas::segmentColor(std::uint32_t protect,
                                 std::uint32_t state) const {
    const auto& t = theme::tokens();
    const bool exec = (protect & 0xF0) != 0;
    const bool write = (protect & 0x04) || (protect & 0x08) ||
        (protect & 0x40) || (protect & 0x80);
    const bool read = (protect & 0x02) || (protect & 0x20) ||
        (protect & 0x04) || (protect & 0x40);
    qreal r_w = exec ? 1.0 : 0.0;
    qreal g_w = (write && !exec) ? 1.0 : (write ? 0.55 : 0.0);
    qreal b_w = (read && !exec && !write) ? 1.0 : (read ? 0.45 : 0.0);
    const qreal sum = r_w + g_w + b_w;
    if (sum < 0.0001) {
        r_w = g_w = b_w = 0.6;
    } else {
        r_w /= sum;
        g_w /= sum;
        b_w /= sum;
    }
    const int rr = std::clamp(static_cast<int>(t.error.red() * r_w +
        t.warning.red() * g_w + t.info.red() * b_w), 0, 255);
    const int gg = std::clamp(static_cast<int>(t.error.green() * r_w +
        t.warning.green() * g_w + t.info.green() * b_w), 0, 255);
    const int bb = std::clamp(static_cast<int>(t.error.blue() * r_w +
        t.warning.blue() * g_w + t.info.blue() * b_w), 0, 255);
    qreal alpha = 1.0;
    if (state == 0x2000)
        alpha = 0.55;
    else if (state == 0x10000)
        alpha = 0.18;
    QColor out(rr, gg, bb);
    out.setAlphaF(alpha);
    return out;
}

int VaMapCanvas::segmentAtX(qreal x) const {
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        if (x >= segments_[i].start_x &&
            x <= segments_[i].start_x + segments_[i].width)
            return static_cast<int>(i);
    }
    return -1;
}

void VaMapCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const int segment = segmentAtX(event->pos().x());
        if (segment >= 0) {
            selected_segment_ = segment;
            selected_region_ = segments_[static_cast<std::size_t>(segment)]
                .region_index;
            Q_EMIT regionSelected(selected_region_,
                event->globalPosition().toPoint());
            update();
        }
    }
    QWidget::mousePressEvent(event);
}

void VaMapCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && selected_segment_ >= 0 &&
        regions_) {
        const auto& segment = segments_[static_cast<std::size_t>(
            selected_segment_)];
        Q_EMIT regionJumpHex(segment.base, segment.size);
    }
    QWidget::mouseDoubleClickEvent(event);
}

void VaMapCanvas::mouseMoveEvent(QMouseEvent* event) {
    const int segment = segmentAtX(event->pos().x());
    if (segment != hovered_segment_) {
        hovered_segment_ = segment;
        update();
    }
    if (segment >= 0 && regions_) {
        const auto& s = segments_[static_cast<std::size_t>(segment)];
        const auto& region = (*regions_)[static_cast<std::size_t>(
            s.region_index)];
        const QString tip = region.module_name.empty()
            ? QString::asprintf("0x%016llX - 0x%016llX\n%s | %s | %s\n%s",
                static_cast<unsigned long long>(region.base),
                static_cast<unsigned long long>(region.base + region.size),
                debugger_engine::format_protect(region.protect).c_str(),
                memory_map_view::detail::format_state(region.state).c_str(),
                memory_map_view::detail::format_type(region.type).c_str(),
                memory_map_view::detail::format_size(region.size).c_str())
            : QString::asprintf("%s\n0x%016llX - 0x%016llX\n%s | %s | %s\n%s",
                region.module_name.c_str(),
                static_cast<unsigned long long>(region.base),
                static_cast<unsigned long long>(region.base + region.size),
                debugger_engine::format_protect(region.protect).c_str(),
                memory_map_view::detail::format_state(region.state).c_str(),
                memory_map_view::detail::format_type(region.type).c_str(),
                memory_map_view::detail::format_size(region.size).c_str());
        setToolTip(tip);
    } else {
        setToolTip(QString());
    }
    QWidget::mouseMoveEvent(event);
}

void VaMapCanvas::keyPressEvent(QKeyEvent* event) {
    const int count = static_cast<int>(segments_.size());
    if (count > 0) {
        int next = -1;
        switch (event->key()) {
            case Qt::Key_Left:
                next = selected_segment_ > 0 ? selected_segment_ - 1 : 0;
                break;
            case Qt::Key_Right:
                next = selected_segment_ < 0 ? 0
                    : (std::min)(count - 1, selected_segment_ + 1);
                break;
            case Qt::Key_Home:
                next = 0;
                break;
            case Qt::Key_End:
                next = count - 1;
                break;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (selected_segment_ >= 0) {
                    const auto& segment = segments_[static_cast<std::size_t>(
                        selected_segment_)];
                    Q_EMIT regionJumpHex(segment.base, segment.size);
                    event->accept();
                }
                return;
            default:
                break;
        }
        if (next >= 0 && next != selected_segment_) {
            selected_segment_ = next;
            selected_region_ = segments_[static_cast<std::size_t>(next)]
                .region_index;
            const auto& segment = segments_[static_cast<std::size_t>(next)];
            Q_EMIT regionSelected(selected_region_,
                mapToGlobal(QPoint(qRound(segment.start_x +
                    segment.width * 0.5), height() / 2)));
            update();
            event->accept();
            return;
        }
        if (next >= 0) {
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void VaMapCanvas::focusInEvent(QFocusEvent* event) {
    update();
    QWidget::focusInEvent(event);
}

void VaMapCanvas::focusOutEvent(QFocusEvent* event) {
    update();
    QWidget::focusOutEvent(event);
}

void VaMapCanvas::resizeEvent(QResizeEvent* event) {
    rebuildSegments();
    QWidget::resizeEvent(event);
}

void VaMapCanvas::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    const auto& t = theme::tokens();
    painter.fillRect(event->rect(), t.bg_base);
    const qreal pad = t.panel.padding;
    const QFont tick_font = theme::fonts::codeRegular();
    const QFontMetricsF fm(tick_font);
    const qreal tick_gap = t.spacing.xs;
    const qreal tick_len = t.spacing.xs;
    const qreal tick_zone = tick_gap + tick_len + tick_gap + fm.height() +
        tick_gap;
    const QRectF strip(pad, pad, width() - pad * 2,
        (std::max)(0.0, height() - pad - tick_zone));

    painter.setPen(QPen(t.border_subtle, 1.0));
    painter.setBrush(widgets::with_alpha(t.panel_header, 0.6));
    painter.drawRoundedRect(strip, t.radius.md, t.radius.md);

    if (segments_.empty()) {
        painter.setPen(t.text_dim);
        painter.setFont(theme::fonts::body());
        painter.drawText(QRectF(rect()), Qt::AlignCenter,
            QStringLiteral("No regions to map. Refresh while attached."));
        if (hasFocus())
            widgets::paint_focus_ring(painter, strip, t.radius.md, 0.95);
        return;
    }

    painter.save();
    painter.setClipRect(strip);
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        const auto& segment = segments_[i];
        const qreal seg_w = (std::max)(segment.width, 1.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(segmentColor(segment.protect, segment.state));
        painter.drawRect(QRectF(segment.start_x, strip.top() + 1, seg_w,
            strip.height() - 2));
        if (static_cast<int>(i) == hovered_segment_) {
            painter.setBrush(widgets::with_alpha(t.hover_wash, 0.18));
            painter.drawRect(QRectF(segment.start_x, strip.top() + 1, seg_w,
                strip.height() - 2));
            painter.setPen(QPen(t.accent_hover, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(segment.start_x, strip.top(), seg_w,
                strip.height()));
        }
        if (static_cast<int>(i) == selected_segment_) {
            painter.setPen(QPen(t.accent, 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(segment.start_x, strip.top(), seg_w,
                strip.height()));
        }
    }
    painter.restore();

    painter.setFont(tick_font);
    painter.setPen(t.text_dim);
    const std::uint64_t low_va = segments_.front().base;
    const std::uint64_t high_va =
        segments_.back().base + segments_.back().size;
    const QString low_tick = QString::asprintf("%012llX",
        static_cast<unsigned long long>(low_va));
    const QString high_tick = QString::asprintf("%012llX",
        static_cast<unsigned long long>(high_va));
    const qreal tick_label_w = (std::max)(fm.horizontalAdvance(low_tick),
        fm.horizontalAdvance(high_tick));
    const qreal min_stride = tick_label_w + t.spacing.xl;
    const int capacity = 1 + static_cast<int>(strip.width() /
        (std::max)(min_stride, 1.0));
    const int ticks = std::clamp(capacity, 2, 6);
    for (int ti = 0; ti < ticks; ++ti) {
        const qreal frac = static_cast<qreal>(ti) /
            static_cast<qreal>(ticks - 1);
        const qreal tx = strip.left() + frac * strip.width();
        painter.drawLine(QPointF(tx, strip.bottom() + tick_gap),
            QPointF(tx, strip.bottom() + tick_gap + tick_len));
        const std::uint64_t va = low_va + static_cast<std::uint64_t>(
            static_cast<double>(high_va - low_va) * frac);
        const QString label = QString::asprintf("%012llX",
            static_cast<unsigned long long>(va));
        const qreal label_w = fm.horizontalAdvance(label);
        qreal lx = tx - label_w * 0.5;
        lx = std::clamp(lx, strip.left(), strip.right() - label_w);
        painter.drawText(QPointF(lx,
            strip.bottom() + tick_gap + tick_len + tick_gap + fm.ascent()),
            label);
    }

    if (hasFocus())
        widgets::paint_focus_ring(painter, strip, t.radius.md, 0.95);
}

MemoryMapPane::MemoryMapPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.memory_map"));
    setOwnerViewId("view.debug.memory_map");
    setEmptyTargetText(QStringLiteral("No memory map target"),
        QStringLiteral(
            "Attach to a process to enumerate its memory map."));
    setEmptyContentText(QStringLiteral("No memory regions"),
        QStringLiteral(
            "Refresh enumerates the attached target's VAD regions into the "
            "map and table."));
    setLoadingText(QStringLiteral("Enumerating memory regions"),
        QStringLiteral(
            "The engine is walking the attached target's virtual address "
            "space."));
    setErrorText(QStringLiteral("Memory-map refresh failed"),
        QStringLiteral(
            "The memory-map refresh worker failed; press Refresh to retry."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);
    filter_edit_ = new widgets::AidaLineEdit(
        QStringLiteral("Filter modules or info..."), bar);
    filter_edit_->setObjectName(QStringLiteral("aida.view.debug.memory_map.filter"));
    filter_edit_->setMaxLength(63);
    bar_layout->addWidget(filter_edit_, 1);
    refresh_button_ = new widgets::AidaButton(QStringLiteral("Refresh"), bar);
    refresh_button_->setObjectName(
        QStringLiteral("aida.view.debug.memory_map.refresh"));
    refresh_button_->setKind(widgets::AidaButton::Kind::Secondary);
    refresh_button_->setToolTip(QStringLiteral(
        "Re-enumerate the attached target's memory regions"));
    connect(refresh_button_, &widgets::AidaButton::clicked, this,
        &MemoryMapPane::refreshNow);
    bar_layout->addWidget(refresh_button_);
    setToolBar(bar);

    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(tokens.panel.padding_compact,
        tokens.spacing.xs, tokens.panel.padding_compact, tokens.spacing.xs);
    body_layout->setSpacing(tokens.spacing.sm);

    canvas_ = new VaMapCanvas(body);
    canvas_->setObjectName(QStringLiteral("aida.view.debug.memory_map.hero"));
    body_layout->addWidget(canvas_, 0);

    auto* pods = new QWidget(body);
    auto* pods_layout = new QHBoxLayout(pods);
    pods_layout->setContentsMargins(0, 0, 0, 0);
    pods_layout->setSpacing(tokens.status_bar.item_gap);
    const auto make_pod = [pods, pods_layout](const QString& id,
                                              const QString& label) {
        auto* widget = new QLabel(pods);
        widget->setObjectName(QStringLiteral("aida.view.debug.memory_map.") + id);
        widget->setProperty("aidaVariant", QStringLiteral("secondary"));
        widget->setText(label);
        pods_layout->addWidget(widget);
        return widget;
    };
    regions_pod_ = make_pod(QStringLiteral("pod_regions"), QString());
    committed_pod_ = make_pod(QStringLiteral("pod_committed"), QString());
    rwx_pod_ = make_pod(QStringLiteral("pod_rwx"), QString());
    attached_pod_ = make_pod(QStringLiteral("pod_attached"), QString());
    body_layout->addWidget(pods, 0);

    model_ = new MemoryRegionsModel(this);
    view_ = new QTableView(body);
    view_->setObjectName(QStringLiteral("aida.view.debug.memory_map.table"));
    wireTable(view_, model_);
    body_layout->addWidget(view_, 1);
    setContent(body);

    connect(filter_edit_, &QLineEdit::textChanged, this, [this] {
        model_->setFilter(filter_edit_->text());
        if (auto regions = memory_map_view::regions_snapshot()) {
            std::vector<int> filtered;
            const auto& all = *regions;
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (memory_map_view::detail::match_filter(all[i],
                        filter_edit_->text().toStdString().c_str()))
                    filtered.push_back(static_cast<int>(i));
            }
            canvas_->setRegions(regions, filtered);
        }
    });
    connect(canvas_, &VaMapCanvas::regionSelected, this,
        [this](int regionIndex, const QPoint& globalPos) {
            (void)globalPos;
            if (regionIndex < 0)
                return;
            auto regions = memory_map_view::regions_snapshot();
            if (!regions ||
                regionIndex >= static_cast<int>(regions->size()))
                return;
            const auto& region = (*regions)[static_cast<std::size_t>(
                regionIndex)];
            selection_bridge::publish_context(debugger_interaction::capture(
                debugger_interaction::kind_t::memory_region, region.base, 0,
                regionIndex, 0, region.size, region.module_name, region.info));
            memory_interaction::runtime_t runtime;
            runtime.driver_loaded = driver_bridge::is_loaded();
            runtime.live_attached = driver_bridge::attached_pid() != 0;
            runtime.target_pid = driver_bridge::attached_pid();
            memory_interaction::select(
                memory_interaction::capture_memory_range(runtime, region.base,
                    region.size, regionIndex, region.module_name));
        });
    connect(canvas_, &VaMapCanvas::regionJumpHex, this,
        [](std::uint64_t base, std::uint64_t size) {
            debugger_view::jump_to_hex(base,
                size != 0 ? static_cast<std::size_t>(size) : 256u);
        });
    connect(&DebuggerMutationQueue::instance(),
        &DebuggerMutationQueue::protectionChangeCompleted, this,
        [this](bool verified, const QString&) {
            if (verified)
                refreshNow();
        });

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(250);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, &MemoryMapPane::pollModel);

    applyColumnTiers();
}

void MemoryMapPane::onShown() {
    poll_timer_->start();
    if (driver_bridge::attached_pid() != 0)
        memory_map_view::refresh();
    pollModel();
}

void MemoryMapPane::onHidden() {
    poll_timer_->stop();
}

void MemoryMapPane::onSessionTick() {
    pollModel();
    updateOverlayState();
}

bool MemoryMapPane::hasContentRows() const {
    return model_ && (model_->rowCount() > 0 ||
        (filter_edit_ && !filter_edit_->text().isEmpty()));
}

bool MemoryMapPane::isContentLoading() const {
    return memory_map_view::refresh_in_flight();
}

bool MemoryMapPane::contentError(QString* detail) const {
    if (last_error_.isEmpty())
        return false;
    if (detail)
        *detail = last_error_;
    return true;
}

void MemoryMapPane::resizeEvent(QResizeEvent* event) {
    applyColumnTiers();
    DebuggerPaneBase::resizeEvent(event);
}

void MemoryMapPane::applyColumnTiers() {
    if (!view_ || !model_)
        return;
    const auto& t = theme::tokens();
    const auto& grid = theme::fonts::monoGrid();
    const qreal cell_w = grid.valid && grid.cell_w > 0.0
        ? grid.cell_w
        : QFontMetricsF(theme::fonts::codeRegular()).horizontalAdvance(u'0');
    const auto& cols = model_->columns();
    int fixed_total = 0;
    for (const auto& column : cols)
        if (!column.stretch)
            fixed_total += column.width;
    const int info_min = qRound(12.0 * cell_w);
    const int slack = 2 * t.panel.padding_compact + t.spacing.md;
    int needed = fixed_total;
    const int available = width() - info_min - slack;
    const int hide_order[] = {MemoryRegionsModel::Type,
        MemoryRegionsModel::Module, MemoryRegionsModel::State,
        MemoryRegionsModel::Size};
    bool hidden[MemoryRegionsModel::ColumnCount] = {};
    for (const int column : hide_order) {
        if (needed <= available)
            break;
        hidden[column] = true;
        needed -= cols[static_cast<std::size_t>(column)].width;
    }
    bool changed = false;
    for (int column = 0; column < MemoryRegionsModel::ColumnCount; ++column) {
        const bool want = hidden[column];
        if (view_->isColumnHidden(column) != want) {
            view_->setColumnHidden(column, want);
            changed = true;
        }
    }
    if (changed) {
        const QModelIndex current = view_->currentIndex();
        if (current.isValid() && view_->isColumnHidden(current.column()))
            view_->setCurrentIndex(model_->index(current.row(),
                MemoryRegionsModel::Address));
    }
}

void MemoryMapPane::pollModel() {
    refresh_button_->setEnabled(!memory_map_view::refresh_in_flight());
    refresh_button_->setLoading(memory_map_view::refresh_in_flight());
    std::shared_ptr<const std::vector<debugger_engine::memory_region_t>>
        regions;
    std::uint64_t generation = 0;
    std::string error;
    if (memory_map_view::snapshot_state(regions, generation, error)) {
        const QString error_text = QString::fromStdString(error);
        if (error_text != last_error_) {
            last_error_ = error_text;
            updateOverlayState();
        }
    }

    model_->applyRegions(regions, generation);
    std::vector<int> filtered;
    if (regions) {
        const auto filter = filter_edit_->text().toStdString();
        for (std::size_t i = 0; i < regions->size(); ++i) {
            if (memory_map_view::detail::match_filter((*regions)[i],
                    filter.c_str()))
                filtered.push_back(static_cast<int>(i));
        }
    }
    canvas_->setRegions(regions, filtered);

    regions_pod_->setText(QStringLiteral("REGIONS %1")
        .arg(regions ? static_cast<qulonglong>(regions->size()) : 0));
    committed_pod_->setText(QStringLiteral("COMMITTED %1")
        .arg(QString::fromStdString(memory_map_view::detail::format_size(
            model_->committedBytes()))));
    rwx_pod_->setText(QStringLiteral("RWX %1").arg(model_->rwxCount()));
    const quint32 pid = driver_bridge::attached_pid();
    attached_pod_->setText(pid != 0
        ? QStringLiteral("ATTACHED PID %1").arg(pid)
        : QStringLiteral("ATTACHED %1").arg(QStringLiteral("\u2014")));
}

void MemoryMapPane::refreshNow() {
    if (!memory_map_view::refresh_in_flight())
        memory_map_view::refresh();
}

}
