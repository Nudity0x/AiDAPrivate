#include "qt/network/monitor/bandwidth_pane.hpp"

#include <QAbstractButton>
#include <QStyledItemDelegate>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QPainter>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <algorithm>

#include "qt/network/shared/monitor_bridge.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

BandwidthModel::BandwidthModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void BandwidthModel::adoptSnapshot(
    std::shared_ptr<const std::vector<network_view::bw_entry_t>> snapshot) {
    if (!snapshot)
        return;
    const auto& incoming = *snapshot;

    QVector<bool> surviving(static_cast<std::size_t>(rows_.size()), false);
    QVector<quint32> changedPids;
    QVector<network_view::bw_entry_t> appended;
    for (const auto& entry : incoming) {
        const int existing = rowForPid(entry.pid);
        if (existing >= 0) {
            surviving[static_cast<std::size_t>(existing)] = true;
            rows_[existing] = entry;
            changedPids.push_back(entry.pid);
        } else {
            appended.push_back(entry);
        }
    }

    QVector<int> removed;
    for (int row = 0; row < rows_.size(); ++row) {
        if (!surviving[static_cast<std::size_t>(row)])
            removed.push_back(row);
    }
    for (int i = removed.size() - 1; i >= 0; --i) {
        const int row = removed.at(i);
        beginRemoveRows(QModelIndex(), row, row);
        rows_.removeAt(row);
        pids_.removeAt(row);
        endRemoveRows();
    }

    if (!appended.isEmpty()) {
        beginInsertRows(QModelIndex(), rows_.size(),
            rows_.size() + appended.size() - 1);
        for (const auto& entry : appended) {
            pids_.push_back(entry.pid);
            rows_.push_back(entry);
        }
        endInsertRows();
    }

    QVector<int> changedRows;
    changedRows.reserve(changedPids.size());
    for (const quint32 pid : changedPids) {
        const int row = rowForPid(pid);
        if (row >= 0)
            changedRows.push_back(row);
    }
    if (!changedRows.isEmpty()) {
        std::sort(changedRows.begin(), changedRows.end());
        int spanStart = 0;
        for (int i = 1; i <= changedRows.size(); ++i) {
            if (i == changedRows.size() || changedRows.at(i) != changedRows.at(i - 1) + 1) {
                const int top = changedRows.at(spanStart);
                const int bottom = changedRows.at(i - 1);
                Q_EMIT dataChanged(index(top, 0), index(bottom, ColumnCount - 1));
                spanStart = i;
            }
        }
    }
}

const network_view::bw_entry_t* BandwidthModel::rowAt(int row) const noexcept {
    if (row < 0 || row >= rows_.size())
        return nullptr;
    return &rows_.at(row);
}

int BandwidthModel::rowForPid(quint32 pid) const noexcept {
    for (int row = 0; row < pids_.size(); ++row) {
        if (pids_.at(row) == pid)
            return row;
    }
    return -1;
}

int BandwidthModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

int BandwidthModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant BandwidthModel::cellData(const network_view::bw_entry_t& row, int column, int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Pid:      return static_cast<quint32>(row.pid);
        case Name:     return row.process_name.empty() ? QStringLiteral("-")
            : QString::fromStdString(row.process_name);
        case BytesIn:  return QString::fromStdString(format_bytes(row.bytes_in));
        case BytesOut: return QString::fromStdString(format_bytes(row.bytes_out));
        case RateIn:   return QString::fromStdString(format_rate(row.rate_in));
        case RateOut:  return QString::fromStdString(format_rate(row.rate_out));
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (column) {
        case BytesIn:  return t.info;
        case BytesOut: return t.warning;
        default:       return t.text_secondary;
        }
    }
    if (role == SparklineRole && column == Trend) {
        const int historyCount = (std::min)(row.history_index, 64);
        if (historyCount > 1) {
            QVector<float> ordered(static_cast<std::size_t>(historyCount));
            for (int hi = 0; hi < historyCount; ++hi) {
                int idx = (row.history_index - historyCount + hi) % 64;
                if (idx < 0) idx += 64;
                ordered[static_cast<std::size_t>(hi)] = row.rate_history[idx];
            }
            return QVariant::fromValue(ordered);
        }
        return {};
    }
    return {};
}

QVariant BandwidthModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* row = rowAt(index.row());
    if (!row)
        return {};
    return cellData(*row, index.column(), role);
}

void BandwidthModel::multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const {
    if (!index.isValid()) {
        for (auto& roleData : roleDataSpan)
            roleData.clearData();
        return;
    }
    const auto* row = rowAt(index.row());
    if (!row) {
        for (auto& roleData : roleDataSpan)
            roleData.clearData();
        return;
    }
    for (auto& roleData : roleDataSpan)
        roleData.setData(cellData(*row, index.column(), roleData.role()));
}

QVariant BandwidthModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Pid:      return QStringLiteral("PID");
    case Name:     return QStringLiteral("Process");
    case BytesIn:  return QStringLiteral("In");
    case BytesOut: return QStringLiteral("Out");
    case RateIn:   return QStringLiteral("In Rate");
    case RateOut:  return QStringLiteral("Out Rate");
    case Trend:    return QStringLiteral("Trend");
    default: return {};
    }
}

SparklineDelegate::SparklineDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

void SparklineDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    const QVariant value = index.data(BandwidthModel::SparklineRole);
    if (!value.canConvert<QVector<float>>()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    const QVector<float> ordered = value.value<QVector<float>>();
    if (ordered.size() < 2) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    painter->save();
    painter->setClipRect(option.rect);
    const auto& t = theme::tokens();
    const QRectF area = option.rect.adjusted(t.spacing.xs, t.spacing.xs,
        -t.spacing.xs, -t.spacing.xs);

    float maxValue = 0.0001f;
    int peakIndex = 0;
    for (int i = 0; i < ordered.size(); ++i) {
        if (ordered.at(i) > maxValue) {
            maxValue = ordered.at(i);
            peakIndex = i;
        }
    }

    QPolygonF fill;
    const qreal step = area.width() / static_cast<qreal>(ordered.size() - 1);
    for (int i = 0; i < ordered.size(); ++i) {
        const qreal x = area.left() + step * static_cast<qreal>(i);
        const qreal y = area.bottom() - (ordered.at(i) / maxValue) * area.height();
        fill.append(QPointF(x, y));
    }
    QPolygonF fillArea = fill;
    fillArea.append(QPointF(area.right(), area.bottom()));
    fillArea.append(QPointF(area.left(), area.bottom()));
    QColor fillColor = t.accent;
    fillColor.setAlphaF(0.30);
    painter->setPen(Qt::NoPen);
    painter->setBrush(fillColor);
    painter->drawPolygon(fillArea);

    painter->setPen(QPen(t.accent, 1.25));
    painter->setBrush(Qt::NoBrush);
    painter->drawPolyline(fill);

    const qreal peakX = area.left() + step * static_cast<qreal>(peakIndex);
    const qreal peakY = area.bottom() - (ordered.at(peakIndex) / maxValue) * area.height();
    painter->setPen(Qt::NoPen);
    painter->setBrush(t.warning);
    painter->drawEllipse(QPointF(peakX, peakY), 2.5, 2.5);
    QColor peakRing = t.warning;
    peakRing.setAlphaF(0.55);
    painter->setPen(QPen(peakRing, 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(QPointF(peakX, peakY), 4.5, 4.5);
    painter->restore();
}

bool SparklineDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view,
                                  const QStyleOptionViewItem& option, const QModelIndex& index) {
    const QVariant value = index.data(BandwidthModel::SparklineRole);
    if (!value.canConvert<QVector<float>>())
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    const QVector<float> ordered = value.value<QVector<float>>();
    if (ordered.size() < 2)
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    const auto& t = theme::tokens();
    const QRectF area = option.rect.adjusted(t.spacing.xs, t.spacing.xs,
        -t.spacing.xs, -t.spacing.xs);
    const qreal step = area.width() / static_cast<qreal>(ordered.size() - 1);
    int hover = static_cast<int>((event->pos().x() - area.left()) / step + 0.5);
    hover = (std::max)(0, (std::min)(hover, static_cast<int>(ordered.size()) - 1));
    QToolTip::showText(event->globalPos(),
        QString::fromStdString(format_rate(ordered.at(hover))), view);
    return true;
}

BandwidthPane::BandwidthPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.bandwidth"));
    setRequiresTarget(true);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(t.spacing.sm);
    startStopButton_ = new widgets::AidaButton("Start Monitoring", content);
    startStopButton_->setKind(widgets::AidaButton::Kind::Primary);
    startStopButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    toolbar->addWidget(startStopButton_);
    pendingLabel_ = new QLabel("Applying driver state...", content);
    pendingLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    pendingLabel_->hide();
    toolbar->addWidget(pendingLabel_);
    toolbar->addStretch(1);
    layout->addLayout(toolbar);

    model_ = new BandwidthModel(content);
    auto* tableHost = new QWidget(content);
    tableStack_ = new QStackedLayout(tableHost);
    tableStack_->setStackingMode(QStackedLayout::StackOne);
    tableStack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.bandwidth.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h + t.spacing.xs);
    table_->horizontalHeader()->setDefaultSectionSize(table_column_width_chars(table_, 9));
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setModel(model_);
    table_->setItemDelegateForColumn(BandwidthModel::Trend, new SparklineDelegate(table_));
    table_->setMouseTracking(true);
    tableStack_->addWidget(table_);
    emptyView_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("Bandwidth monitor idle"),
        QStringLiteral("Click Start Monitoring above to track per-process bandwidth."),
        tableHost);
    emptyView_->setObjectName(QStringLiteral("aida.view.network.bandwidth.empty"));
    emptyView_->setActionLabel(QStringLiteral("Start Monitoring"));
    connect(emptyView_, &widgets::AidaStateView::actionTriggered, this, [this] {
        if (startStopButton_->isEnabled())
            startStopButton_->click();
    });
    tableStack_->addWidget(emptyView_);
    layout->addWidget(tableHost, 1);

    connect(startStopButton_, &QAbstractButton::clicked, this, [this] {
        const bool polling = network_view::g_state.bw_polling.load(std::memory_order_acquire);
        network_view::request_bandwidth_control(!polling);
        refreshButtons();
        updateStateView();
    });

    if (auto* bridge = NetworkMonitorBridge::instance())
        connect(bridge, &NetworkMonitorBridge::bandwidthSnapshot, this, &BandwidthPane::onSnapshot);

    statePoll_ = new QTimer(this);
    statePoll_->setInterval(250);
    connect(statePoll_, &QTimer::timeout, this, [this] {
        driverSettled_ = true;
        refreshButtons();
        updateStateView();
    });

    setContent(content);
    refreshButtons();
    updateStateView();
}

void BandwidthPane::onPaneShown() {
    network_view::request_driver_available_snapshot(true);
    driverSettled_ = network_view::driver_available_snapshot();
    statePoll_->start();
    refreshButtons();
    updateStateView();
}

void BandwidthPane::onPaneHidden() {
    statePoll_->stop();
}

void BandwidthPane::onSnapshot(
    std::shared_ptr<const std::vector<network_view::bw_entry_t>> snapshot) {
    model_->adoptSnapshot(std::move(snapshot));
    updateStateView();
}

void BandwidthPane::updateStateView() {
    const bool empty = model_->rowCount() == 0;
    if (empty) {
        const bool polling = network_view::g_state.bw_polling.load(std::memory_order_acquire);
        const bool pending =
            network_view::g_state.bw_control_pending.load(std::memory_order_acquire);
        const bool driverOk = network_view::driver_available_snapshot();
        if (!driverOk && driverSettled_) {
            emptyView_->setState(widgets::AidaStateView::State::Error);
            emptyView_->setTitle(QStringLiteral("Kernel driver not attached"));
            emptyView_->setMessage(QStringLiteral(
                "Per-process bandwidth metrics require the kernel driver; load it to collect telemetry."));
            emptyView_->setActionLabel(QString());
        } else if (pending) {
            emptyView_->setState(widgets::AidaStateView::State::Loading);
            emptyView_->setTitle(QStringLiteral("Applying driver state"));
            emptyView_->setMessage(QStringLiteral(
                "Waiting for the kernel driver to apply the monitoring change."));
            emptyView_->setActionLabel(QString());
        } else if (polling) {
            emptyView_->setState(widgets::AidaStateView::State::Loading);
            emptyView_->setTitle(QStringLiteral("Collecting metrics"));
            emptyView_->setMessage(QStringLiteral(
                "Per-process bandwidth statistics will appear shortly."));
            emptyView_->setActionLabel(QString());
        } else {
            emptyView_->setState(widgets::AidaStateView::State::Empty);
            emptyView_->setTitle(QStringLiteral("Bandwidth monitor idle"));
            emptyView_->setMessage(QStringLiteral(
                "Click Start Monitoring above to track per-process bandwidth."));
            emptyView_->setActionLabel(QStringLiteral("Start Monitoring"));
        }
    }
    tableStack_->setCurrentWidget(empty
        ? static_cast<QWidget*>(emptyView_) : static_cast<QWidget*>(table_));
}

void BandwidthPane::refreshButtons() {
    const bool polling = network_view::g_state.bw_polling.load(std::memory_order_acquire);
    const bool pending = network_view::g_state.bw_control_pending.load(std::memory_order_acquire);
    const bool driverOk = network_view::driver_available_snapshot();
    startStopButton_->setText(polling ? "Stop Monitoring" : "Start Monitoring");
    startStopButton_->setKind(polling ? widgets::AidaButton::Kind::Destructive
                                      : widgets::AidaButton::Kind::Primary);
    startStopButton_->setEnabled(driverOk && !pending);
    pendingLabel_->setVisible(pending);
}

}
