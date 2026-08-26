#include "qt/network/monitor/connections_pane.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QLineEdit>
#include <QHeaderView>
#include <QLabel>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "qt/network/shared/monitor_bridge.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_search_field.hpp"
#include "qt/widgets/aida_state_view.hpp"
#include "qt/widgets/aida_toggle.hpp"

namespace aida::qt::net {

ConnectionsModel::ConnectionsModel(QObject* parent)
    : SnapshotTableModel(parent) {}

void ConnectionsModel::setFilter(const QString& filter) {
    if (filter_ == filter)
        return;
    filter_ = filter;
    if (!rows())
        return;
    beginResetModel();
    rebuildVisible();
    endResetModel();
}

const network_view::connection_entry_t* ConnectionsModel::visibleRowAt(int row) const noexcept {
    if (row < 0 || row >= visible_.size())
        return nullptr;
    return rowAt(visible_.at(row));
}

int ConnectionsModel::visibleIndexForKey(quint32 pid, quint8 protocol, quint16 localPort,
                                         quint16 remotePort) const noexcept {
    const auto* rowsData = rows();
    if (!rowsData)
        return -1;
    for (int visibleRow = 0; visibleRow < visible_.size(); ++visibleRow) {
        const auto& entry = rowsData->at(visible_.at(visibleRow));
        if (entry.pid == pid && entry.protocol == protocol &&
            entry.local_port == localPort && entry.remote_port == remotePort)
            return visibleRow;
    }
    return -1;
}

int ConnectionsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : visible_.size();
}

int ConnectionsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ConnectionsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* row = visibleRowAt(index.row());
    if (!row)
        return {};
    return cellData(*row, index.column(), role);
}

void ConnectionsModel::multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const {
    if (!index.isValid()) {
        for (auto& roleData : roleDataSpan)
            roleData.clearData();
        return;
    }
    const auto* row = visibleRowAt(index.row());
    if (!row) {
        for (auto& roleData : roleDataSpan)
            roleData.clearData();
        return;
    }
    for (auto& roleData : roleDataSpan)
        roleData.setData(cellData(*row, index.column(), roleData.role()));
}

QVariant ConnectionsModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Pid:    return QStringLiteral("PID");
    case Proto:  return QStringLiteral("Proto");
    case State:  return QStringLiteral("State");
    case Local:  return QStringLiteral("Local");
    case Remote: return QStringLiteral("Remote");
    default: return {};
    }
}

QVariant ConnectionsModel::cellData(const network_view::connection_entry_t& row, int column,
                                    int role) const {
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Pid:   return static_cast<quint32>(row.pid);
        case Proto: return QString::fromLatin1(protocol_name(row.protocol));
        case State: return QString::fromLatin1(tcp_state_name(row.state));
        case Local: return QStringLiteral("%1:%2")
            .arg(QString::fromStdString(format_ip(row.local_addr, row.address_family)))
            .arg(row.local_port);
        case Remote: return QStringLiteral("%1:%2")
            .arg(QString::fromStdString(format_ip(row.remote_addr, row.address_family)))
            .arg(row.remote_port);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole && column == State) {
        const auto& t = theme::tokens();
        switch (tcp_state_semantic(row.state)) {
        case net_semantic_t::success: return t.success;
        case net_semantic_t::warning: return t.warning;
        case net_semantic_t::error:   return t.error;
        case net_semantic_t::info:    return t.info;
        case net_semantic_t::accent:  return t.accent;
        case net_semantic_t::neutral: default: return t.text_secondary;
        }
    }
    return {};
}

void ConnectionsModel::onRowsAdopted() {
    rebuildVisible();
}

void ConnectionsModel::rebuildVisible() {
    visible_.clear();
    const auto* rowsData = rows();
    if (!rowsData)
        return;
    visible_.reserve(rowsData->size());
    for (int i = 0; i < rowsData->size(); ++i) {
        const auto& connection = rowsData->at(i);
        if (connection.pid == 0 && connection.protocol == 0 &&
            connection.local_port == 0 && connection.remote_port == 0)
            continue;
        if (!filter_.isEmpty()) {
            const std::string local = format_ip(connection.local_addr, connection.address_family) +
                ":" + std::to_string(connection.local_port);
            const std::string remote = format_ip(connection.remote_addr, connection.address_family) +
                ":" + std::to_string(connection.remote_port);
            const std::string searchable = std::to_string(connection.pid) + " " +
                protocol_name(connection.protocol) + " " + tcp_state_name(connection.state) +
                " " + local + " " + remote;
            if (!filter_text_match(filter_, searchable))
                continue;
        }
        visible_.push_back(i);
    }
}

ConnectionsPane::ConnectionsPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.connections"));
    setRequiresTarget(true);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(t.spacing.sm);
    search_ = new widgets::AidaSearchField("Filter by PID, host, port...", content);
    search_->setMaxLength(128);
    toolbar->addWidget(search_, 1);

    autoRefresh_ = new widgets::AidaToggleSwitch(content);
    autoRefresh_->setChecked(true);
    toolbar->addWidget(autoRefresh_);

    refreshButton_ = new widgets::AidaButton("Refresh", content);
    refreshButton_->setKind(widgets::AidaButton::Kind::Secondary);
    refreshButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    toolbar->addWidget(refreshButton_);

    countLabel_ = new QLabel("0 connections", content);
    countLabel_->setProperty("aidaTone", QStringLiteral("secondary"));
    toolbar->addWidget(countLabel_);
    layout->addLayout(toolbar);

    model_ = new ConnectionsModel(content);
    auto* tableHost = new QWidget(content);
    tableStack_ = new QStackedLayout(tableHost);
    tableStack_->setStackingMode(QStackedLayout::StackOne);
    tableStack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.connections.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    table_->horizontalHeader()->setDefaultSectionSize(table_column_width_chars(table_, 9));
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setModel(model_);
    tableStack_->addWidget(table_);
    emptyView_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No active connections"),
        QStringLiteral("Connections will appear once the kernel driver enumerates them."),
        tableHost);
    emptyView_->setObjectName(QStringLiteral("aida.view.network.connections.empty"));
    emptyView_->setActionLabel(QStringLiteral("Refresh"));
    connect(emptyView_, &widgets::AidaStateView::actionTriggered, this, [this] {
        if (refreshButton_->isEnabled())
            refreshButton_->click();
    });
    tableStack_->addWidget(emptyView_);
    layout->addWidget(tableHost, 1);

    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        model_->setFilter(text);
        updateEmptyState();
    });
    connect(autoRefresh_, &QAbstractButton::toggled, this, [](bool checked) {
        network_view::g_state.conn_auto_refresh_enabled.store(checked, std::memory_order_release);
        network_view::g_state.conn_cv.notify_all();
    });
    connect(refreshButton_, &QAbstractButton::clicked, this, [this] {
        if (network_view::driver_available_snapshot()) {
            refreshButton_->setEnabled(false);
            refreshButton_->setText("Refreshing...");
            pendingPoll_->start();
            network_view::request_connection_refresh();
        }
    });

    if (auto* bridge = NetworkMonitorBridge::instance()) {
        connect(bridge, &NetworkMonitorBridge::connectionSnapshot, this,
            &ConnectionsPane::onSnapshot);
    }

    pendingPoll_ = new QTimer(this);
    pendingPoll_->setInterval(150);
    connect(pendingPoll_, &QTimer::timeout, this, [this] {
        driverSettled_ = true;
        const bool pending =
            network_view::g_state.conn_refresh_pending.load(std::memory_order_acquire);
        refreshButton_->setEnabled(!pending && network_view::driver_available_snapshot());
        refreshButton_->setText(pending ? "Refreshing..." : "Refresh");
        updateEmptyState();
        if (!pending)
            pendingPoll_->stop();
    });

    setContent(content);
    updateEmptyState();
}

void ConnectionsPane::onPaneShown() {
    network_view::g_state.conn_pane_visible.store(true, std::memory_order_release);
    network_view::g_state.conn_cv.notify_all();
    network_view::request_driver_available_snapshot(true);
    driverSettled_ = network_view::driver_available_snapshot();
    network_view::request_connection_refresh();
    pendingPoll_->start();
    updateEmptyState();
}

void ConnectionsPane::onPaneHidden() {
    network_view::g_state.conn_pane_visible.store(false, std::memory_order_release);
    pendingPoll_->stop();
}

void ConnectionsPane::onSnapshot(
    std::shared_ptr<const std::vector<network_view::connection_entry_t>> snapshot) {
    quint32 selPid = 0;
    quint16 selLocal = 0, selRemote = 0;
    quint8 selProto = 0;
    bool hadSelection = false;
    const auto current = table_->selectionModel()->currentIndex();
    if (current.isValid()) {
        const auto* row = model_->visibleRowAt(current.row());
        if (row) {
            selPid = row->pid;
            selProto = row->protocol;
            selLocal = row->local_port;
            selRemote = row->remote_port;
            hadSelection = true;
        }
    }
    auto qtRows = std::make_shared<QVector<network_view::connection_entry_t>>();
    if (snapshot) {
        qtRows->reserve(static_cast<qsizetype>(snapshot->size()));
        for (const auto& entry : *snapshot)
            qtRows->push_back(entry);
    }
    ++generation_;
    model_->adopt(std::move(qtRows), generation_);
    countLabel_->setText(QStringLiteral("%1 connections").arg(model_->rowCount()));
    if (hadSelection) {
        const int row = model_->visibleIndexForKey(selPid, selProto, selLocal, selRemote);
        if (row >= 0)
            table_->setCurrentIndex(model_->index(row, 0));
    }
    updateEmptyState();
    const bool pending =
        network_view::g_state.conn_refresh_pending.load(std::memory_order_acquire);
    refreshButton_->setEnabled(!pending && network_view::driver_available_snapshot());
    refreshButton_->setText(pending ? "Refreshing..." : "Refresh");
}

void ConnectionsPane::updateEmptyState() {
    const bool empty = model_->rowCount() == 0;
    if (empty) {
        const bool driverOk = network_view::driver_available_snapshot();
        const bool refreshPending =
            network_view::g_state.conn_refresh_pending.load(std::memory_order_acquire);
        if (!driverOk && driverSettled_) {
            emptyView_->setState(widgets::AidaStateView::State::Error);
            emptyView_->setTitle(QStringLiteral("Kernel driver not attached"));
            emptyView_->setMessage(QStringLiteral(
                "Some features are unavailable until the driver is loaded."));
            emptyView_->setActionLabel(QString());
        } else if (driverOk && refreshPending) {
            emptyView_->setState(widgets::AidaStateView::State::Loading);
            emptyView_->setTitle(QStringLiteral("Refreshing connections"));
            emptyView_->setMessage(QStringLiteral(
                "Waiting for the kernel driver to enumerate connections."));
            emptyView_->setActionLabel(QString());
        } else {
            emptyView_->setState(widgets::AidaStateView::State::Empty);
            emptyView_->setTitle(QStringLiteral("No active connections"));
            emptyView_->setMessage(QStringLiteral(
                "Connections will appear once the kernel driver enumerates them."));
            emptyView_->setActionLabel(QStringLiteral("Refresh"));
        }
    }
    tableStack_->setCurrentWidget(empty
        ? static_cast<QWidget*>(emptyView_) : static_cast<QWidget*>(table_));
}

}
