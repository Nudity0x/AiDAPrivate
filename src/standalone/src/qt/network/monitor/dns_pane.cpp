#include "qt/network/monitor/dns_pane.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QScrollBar>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include "qt/network/shared/monitor_bridge.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

DnsModel::DnsModel(QObject* parent)
    : RingTableModel(8192, parent) {
    setFilter([this](const network_view::dns_entry_t& entry) {
        if (filterText_.isEmpty())
            return true;
        const std::string searchable = entry.domain + " " + entry.resolved_addr + " " +
            std::to_string(entry.pid);
        return filter_text_match(filterText_, searchable);
    });
}

void DnsModel::setFilterText(const QString& text) {
    filterText_ = text;
    refilter();
}

int DnsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant DnsModel::cellData(const network_view::dns_entry_t& row, int column, int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Pid:   return static_cast<quint32>(row.pid);
        case Type:  return QString::fromLatin1(dns_query_type_name(row.query_type));
        case Domain: return QString::fromStdString(row.domain);
        case Address: return QString::fromStdString(row.resolved_addr);
        case RCode: return static_cast<quint32>(row.response_code);
        case Ttl:   return static_cast<quint32>(row.ttl);
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (column) {
        case Domain:  return QString::fromStdString(row.domain);
        case Address: return QString::fromStdString(row.resolved_addr);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (column) {
        case Type:    return t.info;
        case Domain:  return t.accent;
        case RCode:   return row.response_code == 0 ? t.success : t.error;
        case Ttl:     return t.text_dim;
        default:      return t.text_secondary;
        }
    }
    return {};
}

QVariant DnsModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Pid:     return QStringLiteral("PID");
    case Type:    return QStringLiteral("Type");
    case Domain:  return QStringLiteral("Domain");
    case Address: return QStringLiteral("Address");
    case RCode:   return QStringLiteral("RCode");
    case Ttl:     return QStringLiteral("TTL");
    default: return {};
    }
}

DnsPane::DnsPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.dns"));
    setRequiresTarget(true);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(t.spacing.sm);
    startStopButton_ = new widgets::AidaButton("Start DNS Monitor", content);
    startStopButton_->setKind(widgets::AidaButton::Kind::Primary);
    startStopButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    toolbar->addWidget(startStopButton_);

    refreshButton_ = new widgets::AidaButton("Refresh", content);
    refreshButton_->setKind(widgets::AidaButton::Kind::Secondary);
    refreshButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    toolbar->addWidget(refreshButton_);

    filterText_ = new QLineEdit(content);
    filterText_->setPlaceholderText("Filter by domain or address...");
    filterText_->setMaxLength(128);
    toolbar->addWidget(filterText_, 1);

    autoScroll_ = new QCheckBox("Auto-scroll", content);
    autoScroll_->setChecked(true);
    toolbar->addWidget(autoScroll_);

    countLabel_ = new QLabel(content);
    countLabel_->setProperty("aidaTone", QStringLiteral("secondary"));
    toolbar->addWidget(countLabel_);
    layout->addLayout(toolbar);

    model_ = new DnsModel(content);
    auto* tableHost = new QWidget(content);
    tableStack_ = new QStackedLayout(tableHost);
    tableStack_->setStackingMode(QStackedLayout::StackOne);
    tableStack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.dns.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    table_->horizontalHeader()->setDefaultSectionSize(table_column_width_chars(table_, 8));
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setModel(model_);
    tableStack_->addWidget(table_);
    emptyView_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("DNS monitor idle"),
        QStringLiteral("Click Start DNS Monitor to begin tracking queries."),
        tableHost);
    emptyView_->setObjectName(QStringLiteral("aida.view.network.dns.empty"));
    emptyView_->setActionLabel(QStringLiteral("Start DNS Monitor"));
    connect(emptyView_, &widgets::AidaStateView::actionTriggered, this, [this] {
        if (startStopButton_->isEnabled())
            startStopButton_->click();
    });
    tableStack_->addWidget(emptyView_);
    layout->addWidget(tableHost, 1);

    connect(startStopButton_, &QAbstractButton::clicked, this, [this] {
        const bool polling = network_view::g_state.dns_polling.load(std::memory_order_acquire);
        if (!polling) {
            network_view::g_state.dns_polling.store(true, std::memory_order_release);
            network_view::g_state.dns_cv.notify_all();
        } else {
            network_view::g_state.dns_polling.store(false, std::memory_order_release);
        }
        refreshButtons();
        updateEmptyStateForCurrentFilter();
    });
    connect(refreshButton_, &QAbstractButton::clicked, this, [this] {
        if (network_view::driver_available_snapshot()) {
            network_view::request_dns_refresh();
            refreshButtons();
            updateEmptyStateForCurrentFilter();
        }
    });
    connect(filterText_, &QLineEdit::textChanged, this, [this](const QString& text) {
        model_->setFilterText(text);
        updateEmptyStateForCurrentFilter();
    });

    if (auto* bridge = NetworkMonitorBridge::instance())
        connect(bridge, &NetworkMonitorBridge::dnsBatch, this, &DnsPane::onBatch);

    statePoll_ = new QTimer(this);
    statePoll_->setInterval(250);
    connect(statePoll_, &QTimer::timeout, this, [this] {
        driverSettled_ = true;
        refreshButtons();
        updateEmptyStateForCurrentFilter();
    });

    setContent(content);
    refreshButtons();
    updateEmptyStateForCurrentFilter();
}

void DnsPane::updateEmptyStateForCurrentFilter() {
    const bool empty = model_->rowCount() == 0;
    const bool polling = network_view::g_state.dns_polling.load(std::memory_order_acquire);
    const bool driverOk = network_view::driver_available_snapshot();
    const bool refreshPending =
        network_view::g_state.dns_refresh_pending.load(std::memory_order_acquire);
    if (empty) {
        if (!driverOk && driverSettled_) {
            emptyView_->setState(widgets::AidaStateView::State::Error);
            emptyView_->setTitle(QStringLiteral("Kernel driver not attached"));
            emptyView_->setMessage(QStringLiteral(
                "DNS monitoring requires the kernel driver; load it to observe queries."));
            emptyView_->setActionLabel(QString());
        } else if (driverOk && refreshPending) {
            emptyView_->setState(widgets::AidaStateView::State::Loading);
            emptyView_->setTitle(QStringLiteral("Refreshing DNS observations"));
            emptyView_->setMessage(QStringLiteral(
                "Waiting for the kernel driver to answer the refresh request."));
            emptyView_->setActionLabel(QString());
        } else {
            emptyView_->setState(widgets::AidaStateView::State::Empty);
            emptyView_->setTitle(polling
                ? QStringLiteral("Listening for DNS queries")
                : QStringLiteral("DNS monitor idle"));
            emptyView_->setMessage(polling
                ? QStringLiteral("Resolved queries will appear here as the kernel observes DNS traffic.")
                : QStringLiteral("Click Start DNS Monitor to begin tracking queries."));
            emptyView_->setActionLabel(polling ? QString() : QStringLiteral("Start DNS Monitor"));
        }
    }
    tableStack_->setCurrentWidget(empty
        ? static_cast<QWidget*>(emptyView_) : static_cast<QWidget*>(table_));
}

void DnsPane::onPaneShown() {
    network_view::request_driver_available_snapshot(true);
    driverSettled_ = network_view::driver_available_snapshot();
    statePoll_->start();
    if (!bulkLoaded_) {
        bulkLoaded_ = true;
        const auto snapshot = network_view::dns_entries_snapshot();
        if (snapshot && !snapshot->empty())
            model_->appendBatch(snapshot);
    }
    refreshButtons();
    updateEmptyStateForCurrentFilter();
}

void DnsPane::onPaneHidden() {
    statePoll_->stop();
}

void DnsPane::onBatch(std::shared_ptr<const std::vector<network_view::dns_entry_t>> batch,
                      quint64 trimmedFromFront) {
    if (!batch || batch->empty())
        return;
    const bool atBottom = table_->verticalScrollBar()->value() >=
        table_->verticalScrollBar()->maximum() - 4;
    model_->appendBatch(batch, static_cast<std::size_t>(trimmedFromFront));
    countLabel_->setText(QStringLiteral("%1 observations").arg(model_->rowCount()));
    if (autoScroll_->isChecked() && atBottom && model_->rowCount() > 0)
        table_->scrollTo(model_->index(model_->rowCount() - 1, 0));
    updateEmptyStateForCurrentFilter();
}

void DnsPane::refreshButtons() {
    const bool polling = network_view::g_state.dns_polling.load(std::memory_order_acquire);
    const bool driverOk = network_view::driver_available_snapshot();
    startStopButton_->setText(polling ? "Stop DNS Monitor" : "Start DNS Monitor");
    startStopButton_->setKind(polling ? widgets::AidaButton::Kind::Destructive
                                      : widgets::AidaButton::Kind::Primary);
    startStopButton_->setEnabled(driverOk);
    const bool refreshPending =
        network_view::g_state.dns_refresh_pending.load(std::memory_order_acquire);
    refreshButton_->setEnabled(driverOk && !refreshPending);
    refreshButton_->setText(refreshPending ? "Refreshing..." : "Refresh");
}

}
