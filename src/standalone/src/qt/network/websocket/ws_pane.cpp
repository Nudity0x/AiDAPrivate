#include "qt/network/websocket/ws_pane.hpp"

#include <QAction>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>

#include "helpers/diag_log.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

network_view::artifact_identity_t websocket_artifact_identity(
    const WsFrameStore::entry_t& frame) {
    network_view::artifact_identity_t identity;
    identity.kind = network_view::artifact_kind_t::websocket_frame;
    identity.id = "network.websocket." + std::to_string(frame.exchange_id) + "." +
        std::to_string(frame.timestamp) + (frame.is_outbound ? ".out" : ".in");
    identity.parent_id = "network.exchange." + std::to_string(frame.exchange_id);
    identity.source_view_id = "view.network.websocket";
    identity.source_id = frame.exchange_id;
    identity.timestamp = frame.timestamp;
    identity.content_size = frame.payload.size();
    identity.content_hash = network_view::artifact_content_hash(frame.payload);
    identity.label = std::string(frame.is_outbound ? "Outbound" : "Inbound") +
        " WebSocket frame";
    identity.target_host = frame.host;
    identity.target_port = frame.port;
    return identity;
}

}

WsFramesModel::WsFramesModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int WsFramesModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

int WsFramesModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant WsFramesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* frame = frameAt(index.row());
    if (!frame)
        return {};
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Dir: return frame->is_outbound ? QStringLiteral("\xE2\x86\x91")
                                            : QStringLiteral("\xE2\x86\x93");
        case Host: return QStringLiteral("%1:%2")
            .arg(QString::fromStdString(frame->host))
            .arg(static_cast<unsigned>(frame->port));
        case Opcode: return QStringLiteral("0x%1")
            .arg(static_cast<unsigned>(frame->opcode), 2, 16, QLatin1Char('0')).toUpper();
        case Size: return QString::number(static_cast<qulonglong>(frame->payload.size()));
        case Preview: return frame->preview.empty()
            ? QStringLiteral("(empty)") : QString::fromStdString(frame->preview);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        const auto& t = theme::tokens();
        switch (index.column()) {
        case Dir: return frame->is_outbound ? t.warning : t.info;
        case Host:
        case Size: return t.text_primary;
        case Opcode:
        case Preview:
        default: return t.text_dim;
        }
    }
    return {};
}

QVariant WsFramesModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Dir: return QStringLiteral("Dir");
    case Host: return QStringLiteral("Host");
    case Opcode: return QStringLiteral("Opcode");
    case Size: return QStringLiteral("Size");
    case Preview: return QStringLiteral("Preview");
    default: return {};
    }
}

void WsFramesModel::multiData(const QModelIndex& index,
                              QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

bool WsFramesModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid())
        return false;
    return WsFrameStore::instance().appendSerial() > last_seen_serial_;
}

void WsFramesModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid())
        return;
    auto& store = WsFrameStore::instance();
    const auto result = store.fetchAfter(last_seen_serial_, kPageSize);
    if (result.dropped_total != last_dropped_total_) {
        resyncAfterEviction();
        return;
    }
    if (result.entries.empty())
        return;
    std::vector<int> newVisible;
    newVisible.reserve(result.entries.size());
    for (const auto& entry : result.entries) {
        const int row = static_cast<int>(materialized_.size());
        materialized_.push_back(entry);
        if (matchesFilter(*entry))
            newVisible.push_back(row);
    }
    last_seen_serial_ = result.head_serial;
    if (!newVisible.empty()) {
        beginInsertRows(QModelIndex(), static_cast<int>(visible_.size()),
            static_cast<int>(visible_.size()) + static_cast<int>(newVisible.size()) - 1);
        visible_.insert(visible_.end(), newVisible.begin(), newVisible.end());
        endInsertRows();
    }
}

void WsFramesModel::setFilterText(const QString& text) {
    if (filter_ == text)
        return;
    filter_ = text;
    refilter();
}

void WsFramesModel::clearAll() {
    if (materialized_.empty() && visible_.empty())
        return;
    beginResetModel();
    materialized_.clear();
    visible_.clear();
    endResetModel();
}

bool WsFramesModel::resyncAfterEviction() {
    auto& store = WsFrameStore::instance();
    const std::uint64_t dropped = store.droppedTotal();
    if (dropped == last_dropped_total_)
        return false;
    beginResetModel();
    materialized_.clear();
    visible_.clear();
    const std::uint64_t head = store.appendSerial();
    const std::size_t available = store.size();
    const std::uint64_t rebase = head > static_cast<std::uint64_t>(available)
        ? head - static_cast<std::uint64_t>(available) : 0;
    const auto result = store.fetchAfter(rebase, available == 0 ? 1 : available);
    last_dropped_total_ = result.dropped_total;
    last_seen_serial_ = result.head_serial;
    for (const auto& entry : result.entries) {
        const int row = static_cast<int>(materialized_.size());
        materialized_.push_back(entry);
        if (matchesFilter(*entry))
            visible_.push_back(row);
    }
    endResetModel();
    return true;
}

const WsFrameStore::entry_t* WsFramesModel::frameAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(visible_.size()))
        return nullptr;
    const int index = visible_[static_cast<std::size_t>(row)];
    if (index < 0 || index >= static_cast<int>(materialized_.size()))
        return nullptr;
    return materialized_[static_cast<std::size_t>(index)].get();
}

bool WsFramesModel::matchesFilter(const WsFrameStore::entry_t& frame) const {
    if (filter_.isEmpty())
        return true;
    const std::string needle = filter_.toStdString();
    return frame.host.find(needle) != std::string::npos ||
        frame.preview.find(needle) != std::string::npos;
}

void WsFramesModel::refilter() {
    beginResetModel();
    visible_.clear();
    visible_.reserve(materialized_.size());
    for (std::size_t i = 0; i < materialized_.size(); ++i) {
        if (matchesFilter(*materialized_[i]))
            visible_.push_back(static_cast<int>(i));
    }
    endResetModel();
}

WsPane::WsPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.websocket"));
    const auto& t = theme::tokens();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(t.spacing.sm);
    filter_edit_ = new QLineEdit(content);
    filter_edit_->setPlaceholderText(QStringLiteral("Filter..."));
    filter_edit_->setMaxLength(127);
    toolbar->addWidget(filter_edit_, 1);
    clear_button_ = new widgets::AidaButton(QStringLiteral("Clear"), content);
    clear_button_->setKind(widgets::AidaButton::Kind::Secondary);
    clear_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    clear_button_->setToolTip(QStringLiteral("Discard all retained WebSocket frames"));
    toolbar->addWidget(clear_button_);
    auto_scroll_ = new QCheckBox(QStringLiteral("Auto-scroll"), content);
    auto_scroll_->setChecked(true);
    auto_scroll_->setToolTip(QStringLiteral("Keep the latest frame in view as the stream grows"));
    toolbar->addWidget(auto_scroll_);
    layout->addLayout(toolbar);

    auto* splitter = new QSplitter(Qt::Vertical, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);
    model_ = new WsFramesModel(splitter);
    auto* tableHost = new QWidget(splitter);
    table_stack_ = new QStackedLayout(tableHost);
    table_stack_->setStackingMode(QStackedLayout::StackOne);
    table_stack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.websocket.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setModel(model_);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_stack_->addWidget(table_);
    empty_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No WebSocket frames"),
        QStringLiteral("Frames appear here while the proxy observes WebSocket traffic."),
        tableHost);
    empty_view_->setObjectName(QStringLiteral("aida.view.network.websocket.empty"));
    connect(empty_view_, &widgets::AidaStateView::actionTriggered, this, [] {
        (void)network_view::open_view("view.network.proxy");
    });
    table_stack_->addWidget(empty_view_);
    splitter->addWidget(tableHost);
    detail_ = new QPlainTextEdit(splitter);
    detail_->setReadOnly(true);
    detail_->setFont(theme::fonts::codeRegular());
    detail_->setPlaceholderText(QStringLiteral("Select a frame to inspect its payload"));
    splitter->addWidget(detail_);
    splitter->setStretchFactor(0, 11);
    splitter->setStretchFactor(1, 9);
    layout->addWidget(splitter, 1);
    updateEmptyState();

    auto* menuAction = new QAction(content);
    menuAction->setShortcut(QKeySequence(Qt::Key_Menu));
    menuAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(menuAction, &QAction::triggered, this, [this] {
        openContextForSelection(network_view::exchange_context_origin_t::menu_key);
    });
    content->addAction(menuAction);
    auto* shiftF10Action = new QAction(content);
    shiftF10Action->setShortcut(QKeySequence(QStringLiteral("Shift+F10")));
    shiftF10Action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(shiftF10Action, &QAction::triggered, this, [this] {
        openContextForSelection(network_view::exchange_context_origin_t::shift_f10);
    });
    content->addAction(shiftF10Action);

    connect(filter_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        model_->setFilterText(text);
    });
    connect(clear_button_, &QAbstractButton::clicked, this, [this] { clearFrames(); });
    connect(table_, &QWidget::customContextMenuRequested, this, &WsPane::openContextMenu);
    connect(model_, &QAbstractItemModel::rowsInserted, this, [this] { updateEmptyState(); });
    connect(model_, &QAbstractItemModel::modelReset, this, [this] { updateEmptyState(); });
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* frame = model_->frameAt(current.isValid() ? current.row() : -1);
            if (frame) {
                selected_timestamp_ = frame->timestamp;
                selected_exchange_id_ = frame->exchange_id;
                selected_payload_hash_ = network_view::artifact_content_hash(frame->payload);
                has_selection_ = true;
            } else {
                has_selection_ = false;
            }
            updateDetail();
        });
    connect(model_, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex&, int, int) {
        if (auto_scroll_->isChecked() && model_->rowCount() > 0)
            table_->verticalScrollBar()->setValue(table_->verticalScrollBar()->maximum());
    });
    connect(model_, &QAbstractItemModel::modelReset, this, [this] {
        if (!has_selection_)
            return;
        for (int row = 0; row < model_->rowCount(); ++row) {
            const auto* frame = model_->frameAt(row);
            if (frame && frame->timestamp == selected_timestamp_ &&
                frame->exchange_id == selected_exchange_id_ &&
                network_view::artifact_content_hash(frame->payload) == selected_payload_hash_) {
                table_->setCurrentIndex(model_->index(row, 0));
                break;
            }
        }
    });

    live_timer_ = new QTimer(this);
    live_timer_->setInterval(100);
    connect(live_timer_, &QTimer::timeout, this, [this] {
        fetchLiveEdge();
        updateEmptyState();
    });

    set_ws_frame_view_hooks({
        [pane = QPointer<WsPane>(this)](const std::string& host) {
            if (!pane)
                return;
            QMetaObject::invokeMethod(pane.data(), [pane, host] {
                pane->filter_edit_->setText(QString::fromStdString(host));
            }, Qt::QueuedConnection);
        },
        [pane = QPointer<WsPane>(this)]() {
            if (!pane)
                return;
            QMetaObject::invokeMethod(pane.data(), [pane] {
                pane->auto_scroll_->setChecked(!pane->auto_scroll_->isChecked());
            }, Qt::QueuedConnection);
        }
    });
    const auto pendingFilter = take_pending_ws_filter_host();
    if (pendingFilter.first)
        filter_edit_->setText(QString::fromStdString(pendingFilter.second));
    if (take_pending_ws_follow_toggle())
        auto_scroll_->setChecked(!auto_scroll_->isChecked());

    setContent(content);
}

WsPane::~WsPane() {
    set_ws_frame_view_hooks({});
}

void WsPane::onPaneShown() {
    network_view::request_proxy_runtime_snapshot(true);
    updateEmptyState();
    fetchLiveEdge();
    live_timer_->start();
}

void WsPane::onPaneHidden() {
    live_timer_->stop();
}

void WsPane::fetchLiveEdge() {
    if (model_->resyncAfterEviction())
        return;
    if (model_->canFetchMore(QModelIndex()))
        model_->fetchMore(QModelIndex());
}

void WsPane::updateDetail() {
    const QModelIndex current = table_->currentIndex();
    const auto* frame = model_->frameAt(current.isValid() ? current.row() : -1);
    if (!frame) {
        detail_->clear();
        return;
    }
    const auto& payload = frame->payload;
    const std::size_t displaySize = (std::min)(payload.size(),
        static_cast<std::size_t>(64 * 1024));
    QString text;
    text.reserve(static_cast<qsizetype>(displaySize * 5 + 64));
    for (std::size_t off = 0; off < displaySize; off += 16) {
        char line[128];
        int pos = snprintf(line, sizeof(line), "%04zx  ", off);
        for (std::size_t j = 0; j < 16; j++) {
            if (off + j < displaySize)
                pos += snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), "%02x ",
                    static_cast<unsigned>(payload[off + j]));
            else
                pos += snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), "   ");
        }
        pos += snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), " ");
        for (std::size_t j = 0; j < 16 && off + j < displaySize; j++) {
            uint8_t c = payload[off + j];
            line[pos++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
        }
        line[pos] = '\0';
        text += QString::fromLatin1(line);
        text += QLatin1Char('\n');
    }
    if (displaySize < payload.size())
        text += QStringLiteral("... (%1 more bytes)\n")
            .arg(static_cast<qulonglong>(payload.size() - displaySize));
    detail_->setPlainText(text);
}

void WsPane::openContextMenu(const QPoint& viewportPos) {
    const QModelIndex index = table_->indexAt(viewportPos);
    if (index.isValid())
        table_->setCurrentIndex(index);
    if (!index.isValid())
        return;
    const auto* frame = model_->frameAt(index.row());
    if (!frame)
        return;
    exchange_context_host().show(table_, table_->viewport()->mapToGlobal(viewportPos),
        websocket_artifact_identity(*frame), {},
        network_view::exchange_context_origin_t::pointer);
}

void WsPane::openContextForSelection(network_view::exchange_context_origin_t origin) {
    const QModelIndex current = table_->currentIndex();
    const auto* frame = model_->frameAt(current.isValid() ? current.row() : -1);
    if (!frame)
        return;
    exchange_context_host().show(table_,
        table_->viewport()->mapToGlobal(table_->visualRect(current).center()),
        websocket_artifact_identity(*frame), {}, origin);
}

void WsPane::updateEmptyState() {
    if (!table_stack_ || !empty_view_ || !table_ || !model_)
        return;
    if (model_->rowCount() > 0) {
        table_stack_->setCurrentWidget(table_);
        return;
    }
    const auto snapshot = network_view::proxy_runtime_snapshot();
    if (!snapshot) {
        empty_view_->setState(widgets::AidaStateView::State::Loading);
        empty_view_->setTitle(QStringLiteral("Checking proxy state"));
        empty_view_->setMessage(QStringLiteral(
            "Waiting for the proxy runtime snapshot."));
        empty_view_->setActionLabel(QString());
    } else if (!snapshot->stats.running) {
        empty_view_->setState(widgets::AidaStateView::State::Error);
        empty_view_->setTitle(QStringLiteral("Proxy not running"));
        empty_view_->setMessage(QStringLiteral(
            "WebSocket frames are captured by the local proxy; start the proxy to observe traffic."));
        empty_view_->setActionLabel(QStringLiteral("Open Proxy"));
    } else {
        empty_view_->setState(widgets::AidaStateView::State::Loading);
        empty_view_->setTitle(QStringLiteral("Listening for WebSocket frames"));
        empty_view_->setMessage(QStringLiteral(
            "Frames appear here while the proxy observes WebSocket traffic."));
        empty_view_->setActionLabel(QString());
    }
    table_stack_->setCurrentWidget(empty_view_);
}

void WsPane::clearFrames() {
    auto& store = WsFrameStore::instance();
    const std::size_t previous = store.size();
    store.clear();
    has_selection_ = false;
    model_->clearAll();
    detail_->clear();
    diag::log_tagged_fmt("network", "ws_frames_cleared prev=%zu", previous);
}

}
