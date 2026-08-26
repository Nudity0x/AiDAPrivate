#include "qt/net/qt_ws_editor_view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <utility>

#include "core/infra/executor.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/interaction_context.hpp"
#include "helpers/diag_log.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

const char* k_scheme_items[] = { "ws", "wss" };
const char* k_compose_modes[] = { "Text", "Binary hex", "Raw frame" };

std::vector<std::pair<std::string, std::string>> parseHeaders(const std::string& s)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t p = 0;
    while (p < s.size()) {
        std::size_t eol = s.find('\n', p);
        if (eol == std::string::npos)
            eol = s.size();
        std::string line = s.substr(p, eol - p);
        p = eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const std::size_t cp = line.find(':');
        if (cp == std::string::npos)
            continue;
        const std::string k = line.substr(0, cp);
        const std::string v = line.substr(cp + 1);
        const std::size_t kb = k.find_first_not_of(" \t");
        const std::size_t ke = k.find_last_not_of(" \t");
        const std::size_t vb = v.find_first_not_of(" \t");
        const std::size_t ve = v.find_last_not_of(" \t");
        if (kb == std::string::npos || vb == std::string::npos)
            continue;
        out.emplace_back(k.substr(kb, ke - kb + 1), v.substr(vb, ve - vb + 1));
    }
    return out;
}

bool parseHexPayload(const std::string& src, std::vector<std::uint8_t>& out)
{
    out.clear();
    std::string digits;
    for (char c : src) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            digits.push_back(c);
    }
    if (digits.size() % 2 != 0)
        return false;
    for (std::size_t i = 0; i + 1 < digits.size(); i += 2) {
        const char h = digits[i];
        const char l = digits[i + 1];
        const std::uint8_t hi = static_cast<std::uint8_t>(h <= '9'
            ? h - '0' : (std::tolower(static_cast<unsigned char>(h)) - 'a' + 10));
        const std::uint8_t lo = static_cast<std::uint8_t>(l <= '9'
            ? l - '0' : (std::tolower(static_cast<unsigned char>(l)) - 'a' + 10));
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return true;
}

const char* opcodeName(std::uint8_t op)
{
    switch (op) {
    case 0x0: return "continuation";
    case 0x1: return "text";
    case 0x2: return "binary";
    case 0x8: return "close";
    case 0x9: return "ping";
    case 0xA: return "pong";
    }
    return "?";
}

void submitWsOperation(const char* label, std::function<void()> body)
{
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.ws_view";
    submission.label = label;
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = std::move(body);
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

}

bool wsResolveRetainedFrame(std::uint64_t connection_id, std::uint64_t frame_id,
                            std::vector<std::uint8_t>& bytes,
                            std::string& unavailable_reason)
{
    aida::burp::ws_editor::ws_frame_log_t frame;
    if (!aida::burp::ws_editor::get_frame(connection_id, frame_id, frame)) {
        unavailable_reason = "The WebSocket editor frame was cleared or rolled out of its bounded log.";
        return false;
    }
    bytes = frame.payload;
    unavailable_reason.clear();
    return true;
}

network_view::artifact_identity_t wsFrameArtifactIdentity(
    std::uint64_t connection_id, const aida::burp::ws_editor::ws_frame_log_t& frame)
{
    network_view::artifact_identity_t identity;
    identity.kind = network_view::artifact_kind_t::websocket_editor_frame;
    identity.id = "network.ws_editor." + std::to_string(connection_id) + "." +
        std::to_string(frame.id);
    identity.parent_id = "network.ws_editor." + std::to_string(connection_id);
    identity.source_view_id = "view.network.ws_editor";
    identity.source_id = connection_id;
    identity.timestamp = frame.ts_ms;
    identity.revision = frame.id;
    identity.content_size = frame.payload.size();
    identity.content_hash = http_text::fnv1a64(frame.payload);
    identity.label = std::string(frame.outbound ? "Outbound" : "Inbound") +
        " WebSocket editor frame #" + std::to_string(frame.id);
    return identity;
}

QtWsConnectionModel::QtWsConnectionModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtWsConnectionModel::adopt(std::vector<aida::burp::ws_editor::ws_status_t> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const aida::burp::ws_editor::ws_status_t* QtWsConnectionModel::rowAt(int row) const noexcept
{
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

int QtWsConnectionModel::rowForConnectionId(std::uint64_t id) const noexcept
{
    for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
        if (rows_[static_cast<std::size_t>(row)].id == id)
            return row;
    }
    return -1;
}

int QtWsConnectionModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int QtWsConnectionModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant QtWsConnectionModel::data(const QModelIndex& index, int role) const
{
    const auto* row = rowAt(index.isValid() ? index.row() : -1);
    if (!row)
        return {};
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Id:  return QStringLiteral("[%1]%2")
            .arg(static_cast<quint64>(row->id))
            .arg(row->connected ? QString() : QStringLiteral(" (disconnected)"));
        case Url: return QString::fromStdString(row->url);
        case Sent: return QString::number(static_cast<quint64>(row->frames_sent));
        case Recv: return QString::number(static_cast<quint64>(row->frames_received));
        case Error: return row->last_error.empty() ? QStringLiteral("(none)")
                                                   : QString::fromStdString(row->last_error);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (index.column()) {
        case Error: return t.text_dim;
        default: return row->connected ? t.text_primary : t.text_dim;
        }
    }
    if (role == Qt::ToolTipRole)
        return QString::fromStdString(row->url);
    return {};
}

QVariant QtWsConnectionModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Id:    return QStringLiteral("Id");
    case Url:   return QStringLiteral("URL");
    case Sent:  return QStringLiteral("Sent");
    case Recv:  return QStringLiteral("Recv");
    case Error: return QStringLiteral("Error");
    default: return {};
    }
}

QtWsFrameModel::QtWsFrameModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtWsFrameModel::refresh(std::uint64_t connectionId)
{
    if (connectionId == 0) {
        clearFrames();
        return;
    }
    if (connectionId != connection_id_) {
        connection_id_ = connectionId;
        beginResetModel();
        rows_ = aida::burp::ws_editor::frames(connectionId, 0,
            aida::burp::ws_editor::frame_count(connectionId));
        endResetModel();
        return;
    }
    const std::size_t total = aida::burp::ws_editor::frame_count(connectionId);
    if (total < rows_.size()) {
        beginResetModel();
        rows_ = aida::burp::ws_editor::frames(connectionId, 0, total);
        endResetModel();
        return;
    }
    if (total > rows_.size()) {
        const std::size_t first = rows_.size();
        auto appended = aida::burp::ws_editor::frames(connectionId, first, total - first);
        if (!appended.empty()) {
            beginInsertRows(QModelIndex(), static_cast<int>(first),
                static_cast<int>(first + appended.size() - 1));
            rows_.insert(rows_.end(), appended.begin(), appended.end());
            endInsertRows();
        }
    }
}

void QtWsFrameModel::clearFrames()
{
    connection_id_ = 0;
    if (rows_.empty())
        return;
    beginResetModel();
    rows_.clear();
    endResetModel();
}

const aida::burp::ws_editor::ws_frame_log_t* QtWsFrameModel::rowAt(int row) const noexcept
{
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

int QtWsFrameModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int QtWsFrameModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant QtWsFrameModel::data(const QModelIndex& index, int role) const
{
    const auto* row = rowAt(index.isValid() ? index.row() : -1);
    if (!row)
        return {};
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Dir:  return row->outbound ? QStringLiteral("OUT") : QStringLiteral("IN ");
        case Op:   return QString::fromLatin1(opcodeName(row->opcode));
        case Len:  return QString::number(static_cast<quint64>(row->payload.size()));
        case Time: return QString::number(static_cast<quint64>(row->ts_ms));
        case Preview: return QString::fromStdString(row->preview);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole)
        return row->outbound ? t.warning : t.info;
    return {};
}

QVariant QtWsFrameModel::headerData(int section, Qt::Orientation orientation,
                                    int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Dir:     return QStringLiteral("Dir");
    case Op:      return QStringLiteral("Op");
    case Len:     return QStringLiteral("Len");
    case Time:    return QStringLiteral("Time");
    case Preview: return QStringLiteral("Preview");
    default: return {};
    }
}

QtWsEditorView::QtWsEditorView(QWidget* parent)
    : NetworkPaneBase(parent)
{
    setObjectName(QStringLiteral("view.network.ws_editor"));
    setRequiresTarget(false);

    const auto& t = theme::tokens();
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.xs);

    auto* connectRow = new QHBoxLayout();
    connectRow->setSpacing(t.spacing.xs);
    schemeCombo_ = new QComboBox(content);
    schemeCombo_->setObjectName(QStringLiteral("view.network.ws_editor.scheme"));
    for (const char* item : k_scheme_items)
        schemeCombo_->addItem(QString::fromLatin1(item));
    schemeCombo_->setCurrentIndex(1);
    connectRow->addWidget(schemeCombo_);
    hostEdit_ = new QLineEdit(content);
    hostEdit_->setObjectName(QStringLiteral("view.network.ws_editor.host"));
    hostEdit_->setMaxLength(255);
    hostEdit_->setPlaceholderText(QStringLiteral("host.example.com"));
    connectRow->addWidget(hostEdit_, 1);
    portSpin_ = new QSpinBox(content);
    portSpin_->setObjectName(QStringLiteral("view.network.ws_editor.port"));
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(443);
    connectRow->addWidget(portSpin_);
    pathEdit_ = new QLineEdit(QStringLiteral("/"), content);
    pathEdit_->setObjectName(QStringLiteral("view.network.ws_editor.path"));
    pathEdit_->setMaxLength(1023);
    pathEdit_->setPlaceholderText(QStringLiteral("/socket"));
    connectRow->addWidget(pathEdit_, 1);
    verifyTlsCheck_ = new QCheckBox(QStringLiteral("Verify TLS"), content);
    verifyTlsCheck_->setChecked(true);
    connectRow->addWidget(verifyTlsCheck_);
    connectButton_ = new widgets::AidaButton(QStringLiteral("Connect"), content);
    connectButton_->setObjectName(QStringLiteral("view.network.ws_editor.connect"));
    connectButton_->setKind(widgets::AidaButton::Kind::Primary);
    connectButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    connectRow->addWidget(connectButton_);
    layout->addLayout(connectRow);

    auto* optionsRow = new QHBoxLayout();
    optionsRow->setSpacing(t.spacing.xs);
    originEdit_ = new QLineEdit(content);
    originEdit_->setObjectName(QStringLiteral("view.network.ws_editor.origin"));
    originEdit_->setMaxLength(255);
    originEdit_->setPlaceholderText(QStringLiteral("Origin (optional)"));
    optionsRow->addWidget(originEdit_, 1);
    subprotocolEdit_ = new QLineEdit(content);
    subprotocolEdit_->setObjectName(QStringLiteral("view.network.ws_editor.subprotocol"));
    subprotocolEdit_->setMaxLength(127);
    subprotocolEdit_->setPlaceholderText(QStringLiteral("Sub-protocol"));
    optionsRow->addWidget(subprotocolEdit_, 1);
    layout->addLayout(optionsRow);

    auto* headersLabel = new QLabel(QStringLiteral("Headers (Name: value, one per line)"),
        content);
    headersLabel->setProperty("aidaTone", QStringLiteral("secondary"));
    layout->addWidget(headersLabel);
    headersEdit_ = new QtByteCappedPlainTextEdit(content);
    headersEdit_->setObjectName(QStringLiteral("view.network.ws_editor.headers"));
    headersEdit_->setMaxBytes(4095);
    headersEdit_->setFont(theme::fonts::codeRegular());
    headersEdit_->setMaximumHeight(editor_min_height_lines(headersEdit_, 3));
    layout->addWidget(headersEdit_);

    statusLabel_ = new QLabel(content);
    statusLabel_->setVisible(false);
    layout->addWidget(statusLabel_);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);

    auto* leftPane = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(t.spacing.xs);
    connectionsHeader_ = new QLabel(QStringLiteral("Connections"), leftPane);
    connectionsHeader_->setProperty("aidaTone", QStringLiteral("secondary"));
    leftLayout->addWidget(connectionsHeader_);
    connectionModel_ = new QtWsConnectionModel(leftPane);
    connectionsView_ = new QTableView(leftPane);
    connectionsView_->setObjectName(QStringLiteral("view.network.ws_editor.connections"));
    connectionsView_->verticalHeader()->hide();
    connectionsView_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    connectionsView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    connectionsView_->horizontalHeader()->setStretchLastSection(true);
    connectionsView_->setAlternatingRowColors(true);
    connectionsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    connectionsView_->setSelectionMode(QAbstractItemView::SingleSelection);
    connectionsView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connectionsView_->setModel(connectionModel_);
    leftLayout->addWidget(connectionsView_, 1);

    rightContent_ = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightContent_);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(t.spacing.xs);
    auto* selectedRow = new QHBoxLayout();
    selectedRow->setSpacing(t.spacing.xs);
    selectedLabel_ = new QLabel(rightContent_);
    selectedLabel_->setProperty("aidaTone", QStringLiteral("primary"));
    selectedRow->addWidget(selectedLabel_, 1);
    disconnectButton_ = new widgets::AidaButton(QStringLiteral("Disconnect"), rightContent_);
    disconnectButton_->setKind(widgets::AidaButton::Kind::Destructive);
    disconnectButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    selectedRow->addWidget(disconnectButton_);
    clearButton_ = new widgets::AidaButton(QStringLiteral("Clear log"), rightContent_);
    clearButton_->setKind(widgets::AidaButton::Kind::Secondary);
    clearButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    selectedRow->addWidget(clearButton_);
    pingButton_ = new widgets::AidaButton(QStringLiteral("Ping"), rightContent_);
    pingButton_->setKind(widgets::AidaButton::Kind::Secondary);
    pingButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    selectedRow->addWidget(pingButton_);
    closeButton_ = new widgets::AidaButton(QStringLiteral("Close"), rightContent_);
    closeButton_->setKind(widgets::AidaButton::Kind::Ghost);
    closeButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    selectedRow->addWidget(closeButton_);
    rightLayout->addLayout(selectedRow);

    frameModel_ = new QtWsFrameModel(rightContent_);
    framesView_ = new QTableView(rightContent_);
    framesView_->setModel(frameModel_);
    framesView_->setObjectName(QStringLiteral("view.network.ws_editor.frames"));
    framesView_->verticalHeader()->hide();
    framesView_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    framesView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    framesView_->horizontalHeader()->setSectionResizeMode(QtWsFrameModel::Preview,
        QHeaderView::Stretch);
    framesView_->setAlternatingRowColors(true);
    framesView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    framesView_->setSelectionMode(QAbstractItemView::SingleSelection);
    framesView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    framesView_->setContextMenuPolicy(Qt::CustomContextMenu);
    rightLayout->addWidget(framesView_, 1);

    auto* composeLabel = new QLabel(QStringLiteral("Compose:"), rightContent_);
    composeLabel->setProperty("aidaTone", QStringLiteral("secondary"));
    rightLayout->addWidget(composeLabel);
    auto* composeRow = new QHBoxLayout();
    composeRow->setSpacing(t.spacing.xs);
    composeModeCombo_ = new QComboBox(rightContent_);
    composeModeCombo_->setObjectName(QStringLiteral("view.network.ws_editor.compose_mode"));
    for (const char* mode : k_compose_modes)
        composeModeCombo_->addItem(QString::fromLatin1(mode));
    composeRow->addWidget(composeModeCombo_);
    composeOpcodeSpin_ = new QSpinBox(rightContent_);
    composeOpcodeSpin_->setRange(0, 15);
    composeOpcodeSpin_->setValue(1);
    composeRow->addWidget(composeOpcodeSpin_);
    composeFinCheck_ = new QCheckBox(QStringLiteral("FIN"), rightContent_);
    composeFinCheck_->setChecked(true);
    composeRow->addWidget(composeFinCheck_);
    composeMaskedCheck_ = new QCheckBox(QStringLiteral("MASK"), rightContent_);
    composeMaskedCheck_->setChecked(true);
    composeRow->addWidget(composeMaskedCheck_);
    composeRow->addStretch(1);
    rightLayout->addLayout(composeRow);

    auto* composeHost = new QWidget(rightContent_);
    composeStack_ = new QStackedLayout(composeHost);
    composeStack_->setStackingMode(QStackedLayout::StackOne);
    composeTextEdit_ = new QtByteCappedPlainTextEdit(composeHost);
    composeTextEdit_->setObjectName(QStringLiteral("view.network.ws_editor.compose_text"));
    composeTextEdit_->setMaxBytes(16383);
    composeTextEdit_->setFont(theme::fonts::codeRegular());
    composeStack_->addWidget(composeTextEdit_);
    composeHexEdit_ = new QtByteCappedPlainTextEdit(composeHost);
    composeHexEdit_->setObjectName(QStringLiteral("view.network.ws_editor.compose_hex"));
    composeHexEdit_->setMaxBytes(16383);
    composeHexEdit_->setFont(theme::fonts::codeRegular());
    composeStack_->addWidget(composeHexEdit_);
    rightLayout->addWidget(composeHost, 1);
    sendButton_ = new widgets::AidaButton(QStringLiteral("Send"), rightContent_);
    sendButton_->setObjectName(QStringLiteral("view.network.ws_editor.send"));
    sendButton_->setKind(widgets::AidaButton::Kind::Primary);
    sendButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    rightLayout->addWidget(sendButton_);

    emptyState_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No connection selected"),
        QStringLiteral("Open a connection above, or select one from the list."), splitter);

    splitter->addWidget(leftPane);
    splitter->addWidget(rightContent_);
    splitter->addWidget(emptyState_);
    splitter->setSizes({ 360, 520, 520 });
    layout->addWidget(splitter, 1);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(500);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        refreshConnections();
        refreshFrames();
    });

    connect(connectButton_, &QAbstractButton::clicked, this, &QtWsEditorView::connectNow);
    connect(disconnectButton_, &QAbstractButton::clicked, this,
        &QtWsEditorView::disconnectSelected);
    connect(clearButton_, &QAbstractButton::clicked, this, &QtWsEditorView::clearSelected);
    connect(pingButton_, &QAbstractButton::clicked, this, &QtWsEditorView::pingSelected);
    connect(closeButton_, &QAbstractButton::clicked, this, &QtWsEditorView::closeSelected);
    connect(sendButton_, &QAbstractButton::clicked, this, &QtWsEditorView::sendNow);
    connect(connectionsView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex&, const QModelIndex&) { onConnectionSelected(); });
    connect(framesView_, &QWidget::customContextMenuRequested, this,
        &QtWsEditorView::showFrameContext);
    framesView_->viewport()->installEventFilter(this);
    connect(composeModeCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        composeStack_->setCurrentIndex(index == 0 ? 0 : 1);
        const bool raw = index == 2;
        composeOpcodeSpin_->setVisible(raw);
        composeFinCheck_->setVisible(raw);
        composeMaskedCheck_->setVisible(raw);
    });

    composeOpcodeSpin_->setVisible(false);
    composeFinCheck_->setVisible(false);
    composeMaskedCheck_->setVisible(false);
    onConnectionSelected();
    setContent(content);
}

void QtWsEditorView::onPaneShown()
{
    refreshConnections();
    refreshFrames();
    refreshTimer_->start();
}

void QtWsEditorView::onPaneHidden()
{
    refreshTimer_->stop();
}

void QtWsEditorView::connectNow()
{
    aida::burp::ws_editor::ws_connection_config_t config;
    config.scheme = k_scheme_items[schemeCombo_->currentIndex()];
    config.host = hostEdit_->text().toStdString();
    config.port = static_cast<std::uint16_t>(portSpin_->value());
    config.path = pathEdit_->text().toStdString();
    config.origin = originEdit_->text().toStdString();
    config.subprotocol = subprotocolEdit_->text().toStdString();
    config.verify_tls = verifyTlsCheck_->isChecked();
    config.headers = parseHeaders(headersEdit_->toPlainText().toStdString());
    ::diag::log_tagged_fmt("ws_v", "connect scheme=%s host=%s port=%d path=%s",
        config.scheme.c_str(), config.host.c_str(), config.port, config.path.c_str());
    QPointer<QtWsEditorView> guard(this);
    submitWsOperation("ws.connect", [guard, config]() {
        const std::uint64_t id = aida::burp::ws_editor::connect(config);
        QString text;
        QString kind;
        if (id != 0) {
            ::diag::log_tagged_fmt("ws_v", "connected id=%llu",
                static_cast<unsigned long long>(id));
            kind = QStringLiteral("ok");
            text = QStringLiteral("Connected id=%1").arg(static_cast<quint64>(id));
        } else {
            const std::string error = aida::burp::ws_editor::last_error();
            ::diag::log_tagged_fmt("ws_v", "connect_failed err='%s'", error.c_str());
            kind = QStringLiteral("error");
            text = QString::fromStdString(error);
        }
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(),
            [guard, text, kind]() {
                auto* self = guard.data();
                if (!self)
                    return;
                self->last_action_ = text;
                self->last_action_kind_ = kind;
                self->statusLabel_->setText(text);
                set_label_tone(self->statusLabel_,
                    kind == QStringLiteral("error") ? "error" : "success");
                self->statusLabel_->setVisible(!text.isEmpty());
            }, Qt::QueuedConnection);
    });
}

void QtWsEditorView::refreshConnections()
{
    auto connections = aida::burp::ws_editor::list_connections();
    connectionsHeader_->setText(QStringLiteral("Connections (%1)")
        .arg(static_cast<quint64>(connections.size())));
    connectionModel_->adopt(std::move(connections));
    if (selectedConnectionId_ != 0) {
        const int row = connectionModel_->rowForConnectionId(selectedConnectionId_);
        if (row >= 0)
            connectionsView_->setCurrentIndex(connectionModel_->index(row, 0));
    }
    onConnectionSelected();
}

void QtWsEditorView::refreshFrames()
{
    if (selectedConnectionId_ == 0) {
        frameModel_->clearFrames();
        return;
    }
    frameModel_->refresh(selectedConnectionId_);
}

void QtWsEditorView::onConnectionSelected()
{
    const QModelIndex current = connectionsView_->selectionModel()->currentIndex();
    const auto* connection = connectionModel_->rowAt(current.isValid() ? current.row() : -1);
    selectedConnectionId_ = connection ? connection->id : 0;
    const bool have = connection != nullptr;
    rightContent_->setVisible(have);
    emptyState_->setVisible(!have);
    if (!have) {
        frameModel_->clearFrames();
        return;
    }
    selectedLabel_->setText(QStringLiteral("%1 [id=%2]")
        .arg(QString::fromStdString(connection->url))
        .arg(static_cast<quint64>(connection->id)));
    refreshFrames();
}

void QtWsEditorView::disconnectSelected()
{
    if (selectedConnectionId_ == 0)
        return;
    const std::uint64_t connectionId = selectedConnectionId_;
    ::diag::log_tagged_fmt("ws_v", "disconnect id=%llu",
        static_cast<unsigned long long>(connectionId));
    submitWsOperation("ws.disconnect", [connectionId] {
        aida::burp::ws_editor::disconnect(connectionId);
    });
}

void QtWsEditorView::clearSelected()
{
    if (selectedConnectionId_ == 0)
        return;
    const std::uint64_t connectionId = selectedConnectionId_;
    ::diag::log_tagged_fmt("ws_v", "clear_frames id=%llu",
        static_cast<unsigned long long>(connectionId));
    submitWsOperation("ws_editor.clear_frames", [connectionId] {
        aida::burp::ws_editor::clear_frames(connectionId);
    });
    selectedFrameId_ = 0;
}

void QtWsEditorView::pingSelected()
{
    if (selectedConnectionId_ == 0)
        return;
    const std::uint64_t connectionId = selectedConnectionId_;
    ::diag::log_tagged_fmt("ws_v", "send_ping id=%llu",
        static_cast<unsigned long long>(connectionId));
    submitWsOperation("ws.send_ping", [connectionId] {
        aida::burp::ws_editor::send_ping(connectionId, {});
    });
}

void QtWsEditorView::closeSelected()
{
    if (selectedConnectionId_ == 0)
        return;
    const std::uint64_t connectionId = selectedConnectionId_;
    ::diag::log_tagged_fmt("ws_v", "send_close id=%llu",
        static_cast<unsigned long long>(connectionId));
    submitWsOperation("ws.send_close", [connectionId] {
        aida::burp::ws_editor::send_close(connectionId, 1000, "user_close");
    });
}

void QtWsEditorView::sendNow()
{
    if (selectedConnectionId_ == 0)
        return;
    const std::uint64_t connectionId = selectedConnectionId_;
    const int mode = composeModeCombo_->currentIndex();
    const std::string text = composeTextEdit_->toPlainText().toStdString();
    const std::string hex = composeHexEdit_->toPlainText().toStdString();
    const int opcode = composeOpcodeSpin_->value() & 0xF;
    const bool fin = composeFinCheck_->isChecked();
    const bool masked = composeMaskedCheck_->isChecked();
    ::diag::log_tagged_fmt("ws_v", "send_frame id=%llu mode=%d payload_len=%zu",
        static_cast<unsigned long long>(connectionId), mode,
        mode == 0 ? text.size() : hex.size());
    submitWsOperation("ws.send_frame",
        [connectionId, mode, text, hex, opcode, fin, masked]() {
            if (mode == 0) {
                aida::burp::ws_editor::send_text(connectionId, text);
            } else if (mode == 1) {
                std::vector<std::uint8_t> binary;
                if (parseHexPayload(hex, binary))
                    aida::burp::ws_editor::send_binary(connectionId, binary);
            } else {
                std::vector<std::uint8_t> binary;
                parseHexPayload(hex, binary);
                aida::burp::ws_editor::send_raw_frame(connectionId,
                    static_cast<std::uint8_t>(opcode), fin, masked, binary);
            }
        });
}

bool QtWsEditorView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == framesView_->viewport() && event->type() == QEvent::ContextMenu) {
        auto* contextEvent = static_cast<QContextMenuEvent*>(event);
        if (contextEvent->reason() == QContextMenuEvent::Keyboard) {
            const QModelIndex current = framesView_->selectionModel()->currentIndex();
            if (!current.isValid())
                return false;
            const QRect rect = framesView_->visualRect(current);
            frameContextForIndex(current,
                framesView_->viewport()->mapToGlobal(rect.center()),
                aida::ui::context_menu_open_origin_t::menu_key);
            return true;
        }
    }
    return NetworkPaneBase::eventFilter(watched, event);
}

void QtWsEditorView::showFrameContext(const QPoint& viewportPos)
{
    const QModelIndex index = framesView_->indexAt(viewportPos);
    if (index.isValid())
        framesView_->setCurrentIndex(index);
    frameContextForIndex(index, framesView_->viewport()->mapToGlobal(viewportPos),
        aida::ui::context_menu_open_origin_t::pointer);
}

void QtWsEditorView::frameContextForIndex(const QModelIndex& index, const QPoint& globalPos,
                                          aida::ui::context_menu_open_origin_t origin)
{
    const auto* frame = frameModel_->rowAt(index.isValid() ? index.row() : -1);
    if (!frame || selectedConnectionId_ == 0)
        return;
    selectedFrameId_ = frame->id;
    exchange_context_host().show(framesView_, globalPos,
        wsFrameArtifactIdentity(selectedConnectionId_, *frame), {},
        origin == aida::ui::context_menu_open_origin_t::pointer
            ? network_view::exchange_context_origin_t::pointer
            : network_view::exchange_context_origin_t::menu_key);
}

}
