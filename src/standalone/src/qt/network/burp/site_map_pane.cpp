#include "qt/network/burp/site_map_pane.hpp"

#include "core/network/burp/scope.hpp"

#include <QAbstractItemView>
#include <QEvent>
#include <QFont>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QWidget>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabBar>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QStackedLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>

#include <algorithm>
#include <functional>
#include <string>

#include "core/network/burp/site_map.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/shell_host_contract.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/network/shared/event_bus_bridge.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/network/shared/http_highlighter.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_search_field.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

QString hostExpansionKey(const aida::burp::sitemap::site_map_node_t& node) {
    return QString::fromStdString(node.host) + QStringLiteral("|HOST|") +
        QString::number(node.port);
}

QString pathExpansionKey(const aida::burp::sitemap::site_map_node_t& node) {
    return QString::fromStdString(node.host) + QStringLiteral("|") +
        QString::fromStdString(node.path);
}

}

SiteMapTreeModel::SiteMapTreeModel(QObject* parent)
    : QAbstractItemModel(parent) {}

void SiteMapTreeModel::adopt(
    std::shared_ptr<const aida::burp::sitemap::site_map_tree_snapshot_t> snapshot) {
    beginResetModel();
    snapshot_ = std::move(snapshot);
    queryRevision_ = snapshot_ ? snapshot_->query_revision : 0;
    endResetModel();
}

const aida::burp::sitemap::site_map_node_t* SiteMapTreeModel::nodeFor(
    const QModelIndex& index) const noexcept {
    if (!index.isValid())
        return nullptr;
    return static_cast<const aida::burp::sitemap::site_map_node_t*>(index.internalPointer());
}

QModelIndex SiteMapTreeModel::indexForKey(const QString& key) const {
    if (!snapshot_)
        return {};
    std::function<QModelIndex(const aida::burp::sitemap::site_map_node_t*, int)>
        walk = [&](const aida::burp::sitemap::site_map_node_t* node, int row) -> QModelIndex {
        if (!node)
            return {};
        const QString nodeKey = node->is_host ? hostExpansionKey(*node) : pathExpansionKey(*node);
        const QModelIndex self = createIndex(row, 0,
            const_cast<aida::burp::sitemap::site_map_node_t*>(node));
        if (nodeKey == key)
            return self;
        for (int childRow = 0; childRow < static_cast<int>(node->children.size()); ++childRow) {
            const QModelIndex found = walk(node->children[static_cast<std::size_t>(childRow)].get(), childRow);
            if (found.isValid())
                return found;
        }
        return {};
    };
    for (int hostRow = 0; hostRow < static_cast<int>(snapshot_->hosts.size()); ++hostRow) {
        const QModelIndex found = walk(snapshot_->hosts[static_cast<std::size_t>(hostRow)].get(), hostRow);
        if (found.isValid())
            return found;
    }
    return {};
}

QModelIndex SiteMapTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (!snapshot_ || row < 0 || column != 0)
        return {};
    if (!parent.isValid()) {
        if (row >= static_cast<int>(snapshot_->hosts.size()))
            return {};
        return createIndex(row, 0,
            const_cast<aida::burp::sitemap::site_map_node_t*>(snapshot_->hosts[static_cast<std::size_t>(row)].get()));
    }
    const auto* node = static_cast<const aida::burp::sitemap::site_map_node_t*>(parent.internalPointer());
    if (!node || row >= static_cast<int>(node->children.size()))
        return {};
    return createIndex(row, 0,
        const_cast<aida::burp::sitemap::site_map_node_t*>(node->children[static_cast<std::size_t>(row)].get()));
}

QModelIndex SiteMapTreeModel::parent(const QModelIndex& child) const {
    const auto* node = nodeFor(child);
    if (!node || !snapshot_)
        return {};

    QVector<QPair<const aida::burp::sitemap::site_map_node_t*, int>> path;
    std::function<bool(const aida::burp::sitemap::site_map_node_t*, int)> walk =
        [&](const aida::burp::sitemap::site_map_node_t* current, int row) -> bool {
        path.append({ current, row });
        if (current == node)
            return true;
        for (int childRow = 0; childRow < static_cast<int>(current->children.size()); ++childRow) {
            if (walk(current->children[static_cast<std::size_t>(childRow)].get(), childRow))
                return true;
        }
        path.removeLast();
        return false;
    };
    bool found = false;
    for (int hostRow = 0; hostRow < static_cast<int>(snapshot_->hosts.size()) && !found; ++hostRow)
        found = walk(snapshot_->hosts[static_cast<std::size_t>(hostRow)].get(), hostRow);
    if (!found || path.size() < 2)
        return {};
    const auto& parentEntry = path.at(path.size() - 2);
    return createIndex(parentEntry.second, 0,
        const_cast<aida::burp::sitemap::site_map_node_t*>(parentEntry.first));
}

int SiteMapTreeModel::rowCount(const QModelIndex& parent) const {
    if (!snapshot_)
        return 0;
    if (!parent.isValid())
        return static_cast<int>(snapshot_->hosts.size());
    const auto* node = static_cast<const aida::burp::sitemap::site_map_node_t*>(parent.internalPointer());
    return node ? static_cast<int>(node->children.size()) : 0;
}

int SiteMapTreeModel::columnCount(const QModelIndex&) const {
    return 1;
}

QVariant SiteMapTreeModel::data(const QModelIndex& index, int role) const {
    const auto* node = nodeFor(index);
    if (!node)
        return {};
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        return QString::fromStdString(node->display);
    }
    if (role == Qt::ForegroundRole)
        return node->in_scope ? t.text_primary : t.text_dim;
    return {};
}

QVariant SiteMapTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section != 0)
        return {};
    return QStringLiteral("Site");
}

SiteMapExchangesModel::SiteMapExchangesModel(QObject* parent)
    : SnapshotTableModel(parent) {}

int SiteMapExchangesModel::rowCount(const QModelIndex& parent) const {
    return SnapshotTableModel::rowCount(parent);
}

int SiteMapExchangesModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant SiteMapExchangesModel::cellData(const aida::burp::sitemap::exchange_row_t& row,
                                         int column, int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Id:     return QString::number(static_cast<quint64>(row.id));
        case Method: return QString::fromStdString(row.method);
        case Path:   return QString::fromStdString(row.path);
        case Status: return row.status_code;
        case Size:   return QString::number(static_cast<quint64>(row.response_size));
        case Time:   return QStringLiteral("%1 ms").arg(static_cast<quint64>(row.latency_ms));
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (column) {
        case Method: return QString::fromStdString(row.method);
        case Path:   return QString::fromStdString(row.path);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Status) {
            switch (status_code_semantic(row.status_code)) {
            case net_semantic_t::success: return t.success;
            case net_semantic_t::warning: return t.warning;
            case net_semantic_t::error:   return t.error;
            default: return t.text_primary;
            }
        }
        if (column == Id || column == Size || column == Time)
            return t.text_dim;
        return t.text_primary;
    }
    return {};
}

QVariant SiteMapExchangesModel::headerData(int section, Qt::Orientation orientation,
                                           int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Id:     return QStringLiteral("#");
    case Method: return QStringLiteral("Method");
    case Path:   return QStringLiteral("Path");
    case Status: return QStringLiteral("Status");
    case Size:   return QStringLiteral("Size");
    case Time:   return QStringLiteral("Time");
    default: return {};
    }
}

ExchangeDetailWidget::ExchangeDetailWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    detailTabs_ = new QTabBar(this);
    detailTabs_->addTab("Request");
    detailTabs_->addTab("Response");
    detailTabs_->addTab("Meta");
    layout->addWidget(detailTabs_);
    auto* stackHost = new QWidget(this);
    detailStack_ = new QStackedLayout(stackHost);
    detailStack_->setStackingMode(QStackedLayout::StackOne);
    layout->addWidget(stackHost, 1);

    const QFont mono = theme::fonts::codeRegular();

    auto* requestPage = new QWidget(stackHost);
    auto* requestLayout = new QVBoxLayout(requestPage);
    requestLayout->setContentsMargins(0, 0, 0, 0);
    requestTabs_ = new QTabBar(requestPage);
    requestTabs_->addTab("Raw");
    requestTabs_->addTab("Headers");
    requestTabs_->addTab("Body");
    requestLayout->addWidget(requestTabs_);
    auto* requestStackHost = new QWidget(requestPage);
    requestStack_ = new QStackedLayout(requestStackHost);
    requestStack_->setStackingMode(QStackedLayout::StackOne);
    requestLayout->addWidget(requestStackHost, 1);
    requestRaw_ = new QPlainTextEdit(requestStackHost);
    requestRaw_->setReadOnly(true);
    requestRaw_->setFont(mono);
    attach_http_highlighter(requestRaw_);
    requestStack_->addWidget(requestRaw_);
    auto* requestHeadersScroll = new QScrollArea(requestStackHost);
    requestHeadersScroll->setWidgetResizable(true);
    requestHeaders_ = new QWidget(requestHeadersScroll);
    requestHeadersScroll->setWidget(requestHeaders_);
    requestStack_->addWidget(requestHeadersScroll);
    requestBody_ = new QPlainTextEdit(requestStackHost);
    requestBody_->setReadOnly(true);
    requestBody_->setFont(mono);
    requestStack_->addWidget(requestBody_);
    detailStack_->addWidget(requestPage);

    auto* responsePage = new QWidget(stackHost);
    auto* responseLayout = new QVBoxLayout(responsePage);
    responseLayout->setContentsMargins(0, 0, 0, 0);
    responseTabs_ = new QTabBar(responsePage);
    responseTabs_->addTab("Raw");
    responseTabs_->addTab("Headers");
    responseTabs_->addTab("Body");
    responseLayout->addWidget(responseTabs_);
    auto* responseStackHost = new QWidget(responsePage);
    responseStack_ = new QStackedLayout(responseStackHost);
    responseStack_->setStackingMode(QStackedLayout::StackOne);
    responseLayout->addWidget(responseStackHost, 1);
    responseRaw_ = new QPlainTextEdit(responseStackHost);
    responseRaw_->setReadOnly(true);
    responseRaw_->setFont(mono);
    attach_http_highlighter(responseRaw_);
    responseStack_->addWidget(responseRaw_);
    auto* responseHeadersScroll = new QScrollArea(responseStackHost);
    responseHeadersScroll->setWidgetResizable(true);
    responseHeaders_ = new QWidget(responseHeadersScroll);
    responseHeadersScroll->setWidget(responseHeaders_);
    responseStack_->addWidget(responseHeadersScroll);
    responseBody_ = new QPlainTextEdit(responseStackHost);
    responseBody_->setReadOnly(true);
    responseBody_->setFont(mono);
    responseStack_->addWidget(responseBody_);
    detailStack_->addWidget(responsePage);

    metaPage_ = new QWidget(stackHost);
    detailStack_->addWidget(metaPage_);

    connect(detailTabs_, &QTabBar::currentChanged, detailStack_, &QStackedLayout::setCurrentIndex);
    connect(requestTabs_, &QTabBar::currentChanged, requestStack_, &QStackedLayout::setCurrentIndex);
    connect(responseTabs_, &QTabBar::currentChanged, responseStack_, &QStackedLayout::setCurrentIndex);
}

void ExchangeDetailWidget::showExchange(std::uint64_t exchangeId) {
    exchangeId_ = exchangeId;
    if (detailCacheId_ != exchangeId) {
        hasCurrent_ = aida::burp::sitemap::find_exchange(exchangeId, current_);
        detailCacheId_ = hasCurrent_ ? exchangeId : 0;
    } else {
        hasCurrent_ = aida::burp::sitemap::find_exchange(exchangeId, current_);
        if (!hasCurrent_)
            detailCacheId_ = 0;
    }
    rebuild();
}

void ExchangeDetailWidget::clearExchange() {
    exchangeId_ = 0;
    detailCacheId_ = 0;
    hasCurrent_ = false;
    rebuild();
}

namespace {

void fillHeaderList(QWidget* container,
                    const std::vector<std::pair<std::string, std::string>>& headers) {
    if (auto* existing = container->layout()) {
        QLayoutItem* item;
        while ((item = existing->takeAt(0)) != nullptr) {
            if (item->widget())
                item->widget()->deleteLater();
            delete item;
        }
    } else {
        const auto& t = theme::tokens();
        auto* form = new QFormLayout(container);
        form->setContentsMargins(t.spacing.xs, t.spacing.xs, t.spacing.xs, t.spacing.xs);
        form->setSpacing(t.spacing.xxs);
    }
    auto* form = qobject_cast<QFormLayout*>(container->layout());
    if (!form)
        return;
    for (const auto& header : headers) {
        auto* name = new QLabel(QString::fromStdString(header.first) + ":", container);
        name->setProperty("aidaTone", QStringLiteral("secondary"));
        auto* value = new QLabel(QString::fromStdString(header.second), container);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(name, value);
    }
}

}

void ExchangeDetailWidget::rebuild() {
    if (!hasCurrent_) {
        requestRaw_->clear();
        requestBody_->clear();
        responseRaw_->clear();
        responseBody_->clear();
        return;
    }
    const auto& e = current_;

    QString raw = QStringLiteral("%1 %2%3%4 HTTP/1.1\n")
        .arg(e.method.empty() ? "GET" : e.method.c_str())
        .arg(QString::fromStdString(e.path))
        .arg(e.query.empty() ? QString() : QStringLiteral("?"))
        .arg(QString::fromStdString(e.query));
    bool hasHost = false;
    for (const auto& header : e.req_headers) {
        std::string lowered = header.first;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        hasHost = hasHost || lowered == "host";
        raw += QString::fromStdString(header.first) + ": " + QString::fromStdString(header.second) + "\n";
    }
    if (!hasHost)
        raw += "Host: " + QString::fromStdString(e.host) + "\n";
    raw += "\n";
    if (!e.req_body.empty()) {
        const std::size_t shown = std::min<std::size_t>(e.req_body.size(), 65536);
        raw += QString::fromUtf8(reinterpret_cast<const char*>(e.req_body.data()),
            static_cast<qsizetype>(shown));
        if (shown < e.req_body.size())
            raw += "\n... (truncated)";
    }
    requestRaw_->setPlainText(raw);
    fillHeaderList(requestHeaders_, e.req_headers);
    requestBody_->setPlainText(e.req_body.empty() ? QStringLiteral("(empty)")
        : QString::fromUtf8(reinterpret_cast<const char*>(e.req_body.data()),
            static_cast<qsizetype>(std::min<std::size_t>(e.req_body.size(), 65536))));

    QString responseRawText = QStringLiteral("HTTP/1.1 %1 %2\n")
        .arg(e.status_code)
        .arg(QString::fromStdString(e.reason_phrase));
    for (const auto& header : e.resp_headers)
        responseRawText += QString::fromStdString(header.first) + ": " +
            QString::fromStdString(header.second) + "\n";
    responseRawText += "\n";
    if (!e.resp_body.empty()) {
        const std::size_t shown = std::min<std::size_t>(e.resp_body.size(), 65536);
        responseRawText += QString::fromUtf8(reinterpret_cast<const char*>(e.resp_body.data()),
            static_cast<qsizetype>(shown));
        if (shown < e.resp_body.size())
            responseRawText += "\n... (truncated)";
    }
    responseRaw_->setPlainText(responseRawText);
    fillHeaderList(responseHeaders_, e.resp_headers);
    responseBody_->setPlainText(e.resp_body.empty() ? QStringLiteral("(empty)")
        : QString::fromUtf8(reinterpret_cast<const char*>(e.resp_body.data()),
            static_cast<qsizetype>(std::min<std::size_t>(e.resp_body.size(), 65536))));

    if (auto* existing = metaPage_->layout()) {
        QLayoutItem* item;
        while ((item = existing->takeAt(0)) != nullptr) {
            if (item->widget())
                item->widget()->deleteLater();
            delete item;
        }
    } else {
        auto* form = new QFormLayout(metaPage_);
        const auto& t = theme::tokens();
        form->setContentsMargins(t.spacing.sm, t.spacing.sm, t.spacing.sm, t.spacing.sm);
    }
    auto* form = qobject_cast<QFormLayout*>(metaPage_->layout());
    if (form) {
        form->addRow("ID:", new QLabel(QString::number(static_cast<quint64>(e.id)), metaPage_));
        form->addRow("Host:", new QLabel(QString::fromStdString(e.host), metaPage_));
        form->addRow("Port:", new QLabel(QString::number(e.port), metaPage_));
        form->addRow("Scheme:", new QLabel(QString::fromStdString(e.scheme), metaPage_));
        form->addRow("Status:", new QLabel(QStringLiteral("%1 %2")
            .arg(e.status_code).arg(QString::fromStdString(e.reason_phrase)), metaPage_));
        form->addRow("Latency:", new QLabel(QStringLiteral("%1 ms")
            .arg(static_cast<quint64>(e.latency_ms)), metaPage_));
        form->addRow("TLS / ALPN:", new QLabel(QStringLiteral("%1 / %2")
            .arg(QString::fromStdString(e.tls_version))
            .arg(QString::fromStdString(e.alpn)), metaPage_));
        form->addRow("WebSocket / HTTP2:", new QLabel(QStringLiteral("%1 / %2")
            .arg(e.is_websocket ? "yes" : "no").arg(e.is_h2 ? "yes" : "no"), metaPage_));
        form->addRow("Sizes:", new QLabel(QStringLiteral("Request %1 bytes  Response %2 bytes")
            .arg(static_cast<quint64>(e.req_body.size()))
            .arg(static_cast<quint64>(e.resp_body.size())), metaPage_));
    }
}

SiteMapPane::SiteMapPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.site_map"));
    setRequiresTarget(false);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding_compact, t.panel.padding_compact,
        t.panel.padding_compact, t.panel.padding_compact);
    layout->setSpacing(t.spacing.xs);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(t.spacing.sm);
    headerLabel_ = new QLabel("Site map", content);
    headerLabel_->setProperty("aidaTone", QStringLiteral("title"));
    headerRow->addWidget(headerLabel_);
    countLabel_ = new QLabel(content);
    countLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    headerRow->addWidget(countLabel_);
    headerRow->addStretch(1);
    layout->addLayout(headerRow);

    filterEdit_ = new widgets::AidaSearchField("Filter host or path...", content);
    filterEdit_->setMaxLength(256);
    layout->addWidget(filterEdit_);

    limitBanner_ = new widgets::AidaNotice("View limit reached",
        "View limit reached; filter to narrow the site map.", widgets::AidaSemantic::Warning, content);
    limitBanner_->setVisible(false);
    layout->addWidget(limitBanner_);
    errorBanner_ = new widgets::AidaNotice("Site-map index", QString(), widgets::AidaSemantic::Error, content);
    errorBanner_->setVisible(false);
    layout->addWidget(errorBanner_);

    mainSplitter_ = new QSplitter(Qt::Horizontal, content);
    mainSplitter_->setOpaqueResize(true);
    mainSplitter_->setChildrenCollapsible(false);

    treeModel_ = new SiteMapTreeModel(content);
    tree_ = new QTreeView(mainSplitter_);
    tree_->setUniformRowHeights(true);
    tree_->setHeaderHidden(true);
    tree_->setModel(treeModel_);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    mainSplitter_->addWidget(tree_);

    rightSplitter_ = new QSplitter(Qt::Vertical, mainSplitter_);
    rightSplitter_->setOpaqueResize(true);
    rightSplitter_->setChildrenCollapsible(false);

    exchangesModel_ = new SiteMapExchangesModel(content);
    auto* exchangesHost = new QWidget(rightSplitter_);
    exchangesStack_ = new QStackedLayout(exchangesHost);
    exchangesStack_->setStackingMode(QStackedLayout::StackOne);
    exchangesStack_->setContentsMargins(0, 0, 0, 0);
    exchanges_ = new QTableView(exchangesHost);
    exchanges_->setObjectName(QStringLiteral("aida.view.network.site_map.exchanges"));
    exchanges_->verticalHeader()->hide();
    exchanges_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    exchanges_->horizontalHeader()->setDefaultSectionSize(table_column_width_chars(exchanges_, 7));
    exchanges_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    exchanges_->horizontalHeader()->setStretchLastSection(true);
    exchanges_->setSelectionBehavior(QAbstractItemView::SelectRows);
    exchanges_->setSelectionMode(QAbstractItemView::SingleSelection);
    exchanges_->setAlternatingRowColors(true);
    exchanges_->setShowGrid(false);
    exchanges_->setModel(exchangesModel_);
    exchanges_->setContextMenuPolicy(Qt::CustomContextMenu);
    exchangesStack_->addWidget(exchanges_);
    exchangesEmptyView_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No exchanges"),
        QStringLiteral("Select a host in the tree; its recorded requests appear here."),
        exchangesHost);
    exchangesEmptyView_->setObjectName(QStringLiteral("aida.view.network.site_map.empty"));
    exchangesStack_->addWidget(exchangesEmptyView_);
    rightSplitter_->addWidget(exchangesHost);

    detail_ = new ExchangeDetailWidget(rightSplitter_);
    rightSplitter_->addWidget(detail_);
    rightSplitter_->setStretchFactor(0, 9);
    rightSplitter_->setStretchFactor(1, 11);
    mainSplitter_->addWidget(rightSplitter_);
    mainSplitter_->setStretchFactor(0, 8);
    mainSplitter_->setStretchFactor(1, 17);
    layout->addWidget(mainSplitter_, 1);
    updateExchangesEmptyState();

    connect(filterEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filterDebounce_->setProperty("pendingFilter", text);
        filterDebounce_->start();
    });
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex&, const QModelIndex&) {
            onTreeSelectionChanged();
        });
    connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        if (const auto* node = treeModel_->nodeFor(index))
            expandedKeys_.insert(node->is_host ? hostExpansionKey(*node) : pathExpansionKey(*node));
    });
    connect(tree_, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        if (const auto* node = treeModel_->nodeFor(index))
            expandedKeys_.remove(node->is_host ? hostExpansionKey(*node) : pathExpansionKey(*node));
    });
    connect(tree_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = tree_->indexAt(pos);
        if (index.isValid())
            tree_->setCurrentIndex(index);
        if (index.isValid())
            showNodeContext(index, tree_->viewport()->mapToGlobal(pos),
                aida::ui::context_menu_open_origin_t::pointer);
    });
    tree_->viewport()->installEventFilter(this);
    connect(exchanges_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* row = exchangesModel_->rowAt(current.isValid() ? current.row() : -1);
            selectedExchangeId_ = row ? row->id : 0;
            aida::burp::sitemap::set_selected_exchange_id(selectedExchangeId_);
            if (row)
                detail_->showExchange(row->id);
            else
                detail_->clearExchange();
        });
    connect(exchanges_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = exchanges_->indexAt(pos);
        if (index.isValid())
            exchanges_->setCurrentIndex(index);
        if (index.isValid())
            showExchangeContext(index, exchanges_->viewport()->mapToGlobal(pos),
                aida::ui::context_menu_open_origin_t::pointer);
    });
    exchanges_->viewport()->installEventFilter(this);

    exchangeCoalesce_ = new QTimer(this);
    exchangeCoalesce_->setSingleShot(true);
    exchangeCoalesce_->setInterval(100);
    connect(exchangeCoalesce_, &QTimer::timeout, this, [this] {
        if (!selectedHost_.isEmpty())
            reloadExchanges();
    });

    filterDebounce_ = new QTimer(this);
    filterDebounce_->setSingleShot(true);
    filterDebounce_->setInterval(150);
    connect(filterDebounce_, &QTimer::timeout, this, [this] {
        aida::burp::sitemap::set_tree_filter(
            filterEdit_->property("pendingFilter").toString().toStdString());
    });

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(250);
    connect(pollTimer_, &QTimer::timeout, this, &SiteMapPane::pollTree);

    if (auto* bridge = NetworkEventBusBridge::instance()) {
        connect(bridge, &NetworkEventBusBridge::exchangeObserved, this,
            &SiteMapPane::onExchangeObserved);
    }

    setContent(content);
}

void SiteMapPane::onPaneShown() {
    aida::burp::sitemap::request_tree_rebuild();
    pollTimer_->start();
    pollTree();
}

void SiteMapPane::onPaneHidden() {
    pollTimer_->stop();
    filterDebounce_->stop();
}

bool SiteMapPane::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::ContextMenu)
        return NetworkPaneBase::eventFilter(watched, event);
    auto* contextEvent = static_cast<QContextMenuEvent*>(event);
    if (contextEvent->reason() != QContextMenuEvent::Keyboard)
        return NetworkPaneBase::eventFilter(watched, event);
    if (watched == tree_->viewport()) {
        const QModelIndex current = tree_->selectionModel()->currentIndex();
        if (!current.isValid())
            return false;
        showNodeContext(current, tree_->viewport()->mapToGlobal(
            tree_->visualRect(current).center()),
            aida::ui::context_menu_open_origin_t::menu_key);
        return true;
    }
    if (watched == exchanges_->viewport()) {
        const QModelIndex current = exchanges_->selectionModel()->currentIndex();
        if (!current.isValid())
            return false;
        showExchangeContext(current, exchanges_->viewport()->mapToGlobal(
            exchanges_->visualRect(current).center()),
            aida::ui::context_menu_open_origin_t::menu_key);
        return true;
    }
    return NetworkPaneBase::eventFilter(watched, event);
}

void SiteMapPane::pollTree() {
    const std::uint64_t revision = aida::burp::sitemap::tree_snapshot_revision();
    const std::uint64_t total = aida::burp::sitemap::total_exchanges();
    if (revision != adoptedRevision_) {
        const auto snapshot = aida::burp::sitemap::tree_snapshot();
        adoptedRevision_ = revision;
        treeModel_->adopt(snapshot);
        for (const QString& key : expandedKeys_) {
            const QModelIndex index = treeModel_->indexForKey(key);
            if (index.isValid())
                tree_->setExpanded(index, true);
        }
        limitBanner_->setVisible(snapshot && snapshot->limited);
        const QString error = snapshot ? QString::fromStdString(snapshot->error) : QString();
        errorBanner_->setVisible(!error.isEmpty());
        errorBanner_->setMessage(error);
        countLabel_->setText(QStringLiteral("%1 exchanges").arg(static_cast<quint64>(total)));
        if (snapshot && snapshot->hosts.empty() && total != 0)
            aida::burp::sitemap::request_tree_rebuild();
    }
    if (total != lastExchangeCount_) {
        lastExchangeCount_ = total;
        countLabel_->setText(QStringLiteral("%1 exchanges").arg(static_cast<quint64>(total)));
    }
}

void SiteMapPane::onExchangeObserved(const aida::burp::exchange_observed_t& exchange) {
    if (!selectedHost_.isEmpty() &&
        exchange.host == selectedHost_.toStdString() && exchange.port == selectedPort_) {
        exchangeCoalesce_->start();
    }
}

void SiteMapPane::onTreeSelectionChanged() {
    const QModelIndex current = tree_->selectionModel()->currentIndex();
    const auto* node = treeModel_->nodeFor(current);
    if (!node) {
        return;
    }
    selectedHost_ = QString::fromStdString(node->host);
    selectedPort_ = node->port;
    selectedTls_ = node->tls;
    selectedPath_ = node->is_host ? QString() : QString::fromStdString(node->path);
    reloadExchanges();
}

void SiteMapPane::reloadExchanges() {
    if (selectedHost_.isEmpty()) {
        exchangesModel_->clearRows();
        detail_->clearExchange();
        updateExchangesEmptyState();
        return;
    }
    const auto rows = aida::burp::sitemap::exchange_rows_for(
        selectedHost_.toStdString(), selectedPort_, selectedTls_, selectedPath_.toStdString());
    ++exchangeGeneration_;
    exchangesModel_->adopt(std::make_shared<const QVector<aida::burp::sitemap::exchange_row_t>>(
        rows.begin(), rows.end()), exchangeGeneration_);
    updateExchangesEmptyState();
}

void SiteMapPane::updateExchangesEmptyState() {
    if (!exchangesStack_ || !exchangesEmptyView_ || !exchanges_ || !exchangesModel_)
        return;
    exchangesStack_->setCurrentWidget(exchangesModel_->rowCount() == 0
        ? static_cast<QWidget*>(exchangesEmptyView_) : static_cast<QWidget*>(exchanges_));
}

void SiteMapPane::showNodeContext(const QModelIndex& index, const QPoint& globalPos,
                                  aida::ui::context_menu_open_origin_t origin) {
    const auto* node = treeModel_->nodeFor(index);
    if (!node)
        return;
    const bool isHost = node->is_host;
    const std::string scheme = node->tls ? "https" : "http";
    aida::ui::application_ui::retained_entity_context_t context;
    context.owner_id = isHost ? "network.site_map.host" : "network.site_map.path";
    context.entity_id = scheme + "://" + node->host + ":" + std::to_string(node->port) + node->path;
    context.entity_generation = node->last_seen_ms;
    context.active_view = aida::ui::stable_view_id_t("view.network.site_map");
    const auto retained_last_seen = node->last_seen_ms;
    const auto retained_requests = node->total_requests;
    const auto retainedHost = node->host;
    const auto retainedPort = node->port;
    const auto retainedPath = node->path;
    const auto retainedTls = node->tls;
    context.validate_identity = [retainedHost, retainedPort, retainedPath, retainedTls,
                                 retained_last_seen, retained_requests] {
        const auto snapshot = aida::burp::sitemap::tree_snapshot();
        if (!snapshot)
            return aida::ui::capability_state_t::unavailable(
                "The site-map node was removed or replaced; select it again");
        for (const auto& host : snapshot->hosts) {
            if (host->host == retainedHost && host->port == retainedPort && host->tls == retainedTls) {
                if (retainedPath.empty())
                    return host->last_seen_ms == retained_last_seen &&
                            host->total_requests == retained_requests
                        ? aida::ui::capability_state_t::available()
                        : aida::ui::capability_state_t::unavailable(
                            "The site-map host was removed or replaced; select it again");
                std::function<bool(const aida::burp::sitemap::site_map_node_t*)> walk =
                    [&](const aida::burp::sitemap::site_map_node_t* current) -> bool {
                    if (!current)
                        return false;
                    if (current->path == retainedPath)
                        return current->last_seen_ms == retained_last_seen &&
                            current->total_requests == retained_requests;
                    for (const auto& child : current->children) {
                        if (walk(child.get()))
                            return true;
                    }
                    return false;
                };
                return walk(host.get())
                    ? aida::ui::capability_state_t::available()
                    : aida::ui::capability_state_t::unavailable(
                        "The site-map path was removed or replaced; select it again");
            }
        }
        return aida::ui::capability_state_t::unavailable(
            "The site-map node was removed or replaced; select it again");
    };
    const auto add = [&context](const char* id, std::function<aida::ui::action_handler_result_t()> invoke) {
        aida::ui::application_ui::retained_entity_action_t action;
        action.action_id = id;
        action.capability = aida::ui::capability_state_t::available();
        action.invoke = std::move(invoke);
        context.actions.push_back(std::move(action));
    };
    add(isHost ? "network.site_map.host.include" : "network.site_map.path.include",
        [scheme, retainedHost, retainedPort, retainedPath] {
            aida::burp::scope::add_include_rule(scheme, retainedHost, retainedPort, retainedPath);
            return aida::ui::action_handler_result_t::completed();
        });
    add(isHost ? "network.site_map.host.exclude" : "network.site_map.path.exclude",
        [scheme, retainedHost, retainedPort, retainedPath] {
            aida::burp::scope::add_exclude_rule(scheme, retainedHost, retainedPort, retainedPath);
            return aida::ui::action_handler_result_t::completed();
        });
    if (!isHost) {
        const std::string retainedUrl = context.entity_id;
        add("network.site_map.copy_url", [retainedUrl] {
            clipboard::set_text(QString::fromStdString(retainedUrl));
            return aida::ui::action_handler_result_t::completed();
        });
    }
    documents::show_retained_entity_menu(context, origin, globalPos, tree_);
}

void SiteMapPane::showExchangeContext(const QModelIndex& index, const QPoint& globalPos,
                                      aida::ui::context_menu_open_origin_t origin) {
    const auto* row = exchangesModel_->rowAt(index.isValid() ? index.row() : -1);
    if (!row)
        return;
    network_view::artifact_identity_t requestIdentity;
    network_view::artifact_identity_t responseIdentity;
    std::string reasonText;
    static_cast<void>(network_view::make_sitemap_artifact(row->id,
        network_view::artifact_kind_t::sitemap_request, requestIdentity, reasonText));
    static_cast<void>(network_view::make_sitemap_artifact(row->id,
        network_view::artifact_kind_t::sitemap_response, responseIdentity, reasonText));
    exchange_context_host().show(exchanges_, globalPos, std::move(requestIdentity),
        std::move(responseIdentity),
        origin == aida::ui::context_menu_open_origin_t::pointer
            ? network_view::exchange_context_origin_t::pointer
            : network_view::exchange_context_origin_t::menu_key);
}

}
