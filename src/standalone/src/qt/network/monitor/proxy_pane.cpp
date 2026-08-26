#include "qt/network/monitor/proxy_pane.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QDialog>
#include <QEvent>
#include <QFont>
#include <QItemSelectionModel>
#include <QWidget>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedLayout>
#include <QStringList>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <algorithm>
#include <cstring>

#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/toast_notification.hpp"
#include "qt/bridge/aida_dialog.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/network/shared/http_highlighter.hpp"
#include "qt/network/shared/monitor_bridge.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_pill.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

ProxyHistoryModel::ProxyHistoryModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void ProxyHistoryModel::adoptHistory(const std::vector<mitm_proxy::http_exchange>& history) {
    QVector<std::uint64_t> incomingIds;
    incomingIds.reserve(static_cast<qsizetype>(history.size()));
    for (const auto& exchange : history)
        incomingIds.push_back(exchange.id);

    QVector<int> removed;
    for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
        if (std::find(incomingIds.begin(), incomingIds.end(), rows_[row].id) == incomingIds.end())
            removed.push_back(row);
    }
    for (int i = removed.size() - 1; i >= 0; --i) {
        beginRemoveRows(QModelIndex(), removed.at(i), removed.at(i));
        rows_.erase(rows_.begin() + removed.at(i));
        endRemoveRows();
    }

    QVector<int> changedRows;
    QVector<mitm_proxy::http_exchange> appended;
    for (const auto& exchange : history) {
        const int existing = rowForExchangeId(exchange.id);
        if (existing < 0) {
            if (matchesFilter(exchange))
                appended.push_back(exchange);
            continue;
        }
        const auto& current = rows_[static_cast<std::size_t>(existing)];
        const bool changed = current.response.status_code != exchange.response.status_code ||
            current.latency_ms != exchange.latency_ms ||
            current.response_size != exchange.response_size ||
            current.state != exchange.state ||
            current.raw_response.size() != exchange.raw_response.size();
        rows_[static_cast<std::size_t>(existing)] = exchange;
        if (changed && matchesFilter(exchange))
            changedRows.push_back(existing);
    }
    if (!appended.isEmpty()) {
        beginInsertRows(QModelIndex(), static_cast<int>(rows_.size()),
            static_cast<int>(rows_.size() + appended.size()) - 1);
        for (auto& exchange : appended)
            rows_.push_back(std::move(exchange));
        endInsertRows();
    }
    for (const int row : changedRows)
        Q_EMIT dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

const mitm_proxy::http_exchange* ProxyHistoryModel::rowAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

int ProxyHistoryModel::rowForExchangeId(std::uint64_t id) const noexcept {
    for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
        if (rows_[static_cast<std::size_t>(row)].id == id)
            return row;
    }
    return -1;
}

void ProxyHistoryModel::setFilter(const QString& filter) {
    if (filter_ == filter)
        return;
    filter_ = filter;
}

bool ProxyHistoryModel::matchesFilter(const mitm_proxy::http_exchange& exchange) const {
    if (filter_.isEmpty())
        return true;
    const std::string searchable = exchange.target_host + " " + exchange.request.method + " " +
        exchange.request.uri;
    return filter_text_match(filter_, searchable);
}

int ProxyHistoryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int ProxyHistoryModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ProxyHistoryModel::cellData(const mitm_proxy::http_exchange& row, int column,
                                     int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Id:     return QString::number(static_cast<quint64>(row.id));
        case Method: return QString::fromStdString(row.request.method);
        case Host:   return QString::fromStdString(row.target_host);
        case Path:   return QString::fromStdString(row.request.uri);
        case Status:
            if (row.response.status_code > 0)
                return row.response.status_code;
            if (row.state == mitm_proxy::http_exchange::state_t::dropped)
                return QStringLiteral("DROP");
            if (row.state == mitm_proxy::http_exchange::state_t::error)
                return QStringLiteral("ERR");
            return QStringLiteral("...");
        case Time:  return QStringLiteral("%1ms").arg(static_cast<quint64>(row.latency_ms));
        case Size:  return QString::fromStdString(format_bytes(row.response_size));
        case Tls:   return row.is_tls ? QStringLiteral("TLS") : QStringLiteral("-");
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (column) {
        case Host: return QString::fromStdString(row.target_host);
        case Path: return QString::fromStdString(row.request.uri);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (column) {
        case Method: return http_method_color(row.request.method.c_str());
        case Status:
            if (row.response.status_code > 0) {
                switch (status_code_semantic(row.response.status_code)) {
                case net_semantic_t::success: return t.success;
                case net_semantic_t::info:    return t.info;
                case net_semantic_t::warning: return t.warning;
                case net_semantic_t::error:   return t.error;
                default: return t.text_secondary;
                }
            }
            if (row.state == mitm_proxy::http_exchange::state_t::dropped ||
                row.state == mitm_proxy::http_exchange::state_t::error)
                return t.error;
            return t.text_dim;
        case Tls:   return row.is_tls ? t.success : t.text_dim;
        case Time:  return t.text_secondary;
        default:    return t.text_secondary;
        }
    }
    return {};
}

QVariant ProxyHistoryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* row = rowAt(index.row());
    if (!row)
        return {};
    return cellData(*row, index.column(), role);
}

void ProxyHistoryModel::multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const {
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

QVariant ProxyHistoryModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Id:     return QStringLiteral("#");
    case Method: return QStringLiteral("Method");
    case Host:   return QStringLiteral("Host");
    case Path:   return QStringLiteral("Path");
    case Status: return QStringLiteral("Status");
    case Time:   return QStringLiteral("Time");
    case Size:   return QStringLiteral("Size");
    case Tls:    return QStringLiteral("TLS");
    default: return {};
    }
}

ProxySparkline::ProxySparkline(QWidget* parent)
    : QWidget(parent) {
    clock_.start();
    setMinimumHeight(theme::tokens().row.compact);
}

void ProxySparkline::sample(std::uint64_t totalRequests) {
    if (!hasSample_) {
        hasSample_ = true;
        lastTotal_ = totalRequests;
        clock_.restart();
        return;
    }
    const float dt = clock_.elapsed() / 1000.0f;
    if (dt < 0.5f)
        return;
    const std::uint64_t diff = totalRequests >= lastTotal_ ? totalRequests - lastTotal_ : 0;
    const float rate = dt > 0.f ? static_cast<float>(diff) / dt : 0.f;
    values_[head_] = rate;
    head_ = (head_ + 1) % kSampleCount;
    lastTotal_ = totalRequests;
    clock_.restart();
    update();
}

QSize ProxySparkline::sizeHint() const {
    const auto& t = theme::tokens();
    return QSize(t.grid * 24, t.row.compact);
}

void ProxySparkline::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto& t = theme::tokens();
    float ordered[kSampleCount];
    float maxValue = 0.0001f;
    for (int i = 0; i < kSampleCount; i++) {
        const int idx = (head_ + i) % kSampleCount;
        ordered[i] = values_[idx];
        if (ordered[i] > maxValue)
            maxValue = ordered[i];
    }
    const QRectF area = rect().adjusted(1, 4, -1, -4);
    QPolygonF line;
    for (int i = 0; i < kSampleCount; ++i) {
        const qreal x = area.left() + area.width() * static_cast<qreal>(i) / (kSampleCount - 1);
        const qreal y = area.bottom() - (ordered[i] / maxValue) * area.height();
        line.append(QPointF(x, y));
    }
    QPolygonF fillArea = line;
    fillArea.append(QPointF(area.right(), area.bottom()));
    fillArea.append(QPointF(area.left(), area.bottom()));
    QColor fill = t.accent;
    fill.setAlphaF(0.30);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawPolygon(fillArea);
    painter.setPen(QPen(t.accent, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(line);
}

ProxyPane::ProxyPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.proxy"));
    setRequiresTarget(false);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* controlRow = new QHBoxLayout();
    controlRow->setSpacing(t.spacing.sm);
    controlRow->addWidget(new QLabel("Bind:", content));
    bindAddrEdit_ = new QLineEdit(QString::fromStdString(network_view::g_state.proxy_bind_addr), content);
    bindAddrEdit_->setMaxLength(63);
    bindAddrEdit_->setMaximumWidth(field_width_chars(bindAddrEdit_, 18));
    controlRow->addWidget(bindAddrEdit_);
    controlRow->addWidget(new QLabel("Port:", content));
    bindPortSpin_ = new QSpinBox(content);
    bindPortSpin_->setRange(1, 65535);
    bindPortSpin_->setValue(network_view::g_state.proxy_port);
    controlRow->addWidget(bindPortSpin_);
    decodeTlsCheck_ = new QCheckBox("TLS MITM", content);
    decodeTlsCheck_->setChecked(network_view::g_state.proxy_decode_tls);
    controlRow->addWidget(decodeTlsCheck_);
    startStopButton_ = new widgets::AidaButton("Start Proxy", content);
    startStopButton_->setKind(widgets::AidaButton::Kind::Primary);
    startStopButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controlRow->addWidget(startStopButton_);
    clearHistoryButton_ = new widgets::AidaButton("Clear History", content);
    clearHistoryButton_->setKind(widgets::AidaButton::Kind::Secondary);
    clearHistoryButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controlRow->addWidget(clearHistoryButton_);
    filterEdit_ = new QLineEdit(content);
    filterEdit_->setPlaceholderText("Filter requests...");
    filterEdit_->setMaxLength(127);
    filterEdit_->setText(QString::fromStdString(network_view::g_state.proxy_filter_text));
    controlRow->addWidget(filterEdit_, 1);
    layout->addLayout(controlRow);

    auto* readinessRow = new QHBoxLayout();
    readinessRow->setSpacing(t.spacing.sm);
    auto mkPill = [content](const QString& text, const char* variant, const char* toolTip) {
        auto* label = new QLabel(text, content);
        label->setProperty("aidaVariant", QString::fromLatin1(variant));
        label->setToolTip(QString::fromLatin1(toolTip));
        return label;
    };
    runningPill_ = mkPill("Proxy stopped", "warning",
        "Whether the local intercepting proxy is listening");
    readinessRow->addWidget(runningPill_);
    caPill_ = mkPill("AiDA CA not trusted", "warning",
        "Whether the AiDA interception CA is trusted by the controlled browser");
    readinessRow->addWidget(caPill_);
    controlledPill_ = mkPill("Controlled ready", "neutral",
        "Whether the controlled Camoufox browser is running under this proxy");
    readinessRow->addWidget(controlledPill_);
    auto* camoufoxPill = mkPill("Camoufox only", "success",
        "Camoufox is the only supported browser for privacy-safe interception");
    readinessRow->addWidget(camoufoxPill);
    auto* webrtcPill = mkPill("WebRTC blocked", "success",
        "WebRTC is disabled in the controlled browser to prevent IP leaks");
    readinessRow->addWidget(webrtcPill);
    auto* quicPill = mkPill("QUIC disabled", "info",
        "QUIC/HTTP3 is blocked so traffic stays on the intercepted TCP path");
    readinessRow->addWidget(quicPill);
    sparkline_ = new ProxySparkline(content);
    sparkline_->setToolTip(QStringLiteral("Requests per second (rolling window)"));
    readinessRow->addWidget(sparkline_);
    statsLabel_ = new QLabel(content);
    statsLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    readinessRow->addWidget(statsLabel_, 1);
    layout->addLayout(readinessRow);

    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(t.spacing.sm);
    prepareBrowserButton_ = new widgets::AidaButton("Prepare controlled browser", content);
    prepareBrowserButton_->setKind(widgets::AidaButton::Kind::Secondary);
    prepareBrowserButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionRow->addWidget(prepareBrowserButton_);
    repairTrustButton_ = new widgets::AidaButton("Repair trust", content);
    repairTrustButton_->setKind(widgets::AidaButton::Kind::Secondary);
    repairTrustButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionRow->addWidget(repairTrustButton_);
    camoufoxControlsButton_ = new widgets::AidaButton("Open Camoufox controls", content);
    camoufoxControlsButton_->setKind(widgets::AidaButton::Kind::Secondary);
    camoufoxControlsButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionRow->addWidget(camoufoxControlsButton_);
    legacyRow_ = new QWidget(content);
    auto* legacyLayout = new QHBoxLayout(legacyRow_);
    legacyLayout->setContentsMargins(0, 0, 0, 0);
    legacyLayout->setSpacing(t.spacing.sm);
    legacyPill_ = new QLabel(legacyRow_);
    legacyPill_->setProperty("aidaVariant", QStringLiteral("warning"));
    legacyLayout->addWidget(legacyPill_);
    legacyRevertButton_ = new widgets::AidaButton("Revert legacy patches", legacyRow_);
    legacyRevertButton_->setKind(widgets::AidaButton::Kind::Secondary);
    legacyRevertButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    legacyLayout->addWidget(legacyRevertButton_);
    legacyRow_->setVisible(false);
    actionRow->addWidget(legacyRow_);
    actionRow->addStretch(1);
    layout->addLayout(actionRow);

    auto* certRow = new QHBoxLayout();
    certRow->setSpacing(t.spacing.sm);
    certRow->addWidget(new QLabel("Target PID:", content));
    certPidSpin_ = new QSpinBox(content);
    certPidSpin_->setRange(0, 0x7FFFFFFF);
    certRow->addWidget(certPidSpin_);
    certDiagButton_ = new widgets::AidaButton("Diagnose target", content);
    certDiagButton_->setKind(widgets::AidaButton::Kind::Secondary);
    certDiagButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    certRow->addWidget(certDiagButton_);
    certStatusLabel_ = new QLabel(content);
    certStatusLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    certRow->addWidget(certStatusLabel_, 1);
    layout->addLayout(certRow);

    certTierLabel_ = new QLabel(content);
    certTierLabel_->setProperty("aidaTone", QStringLiteral("secondary"));
    certTierLabel_->setVisible(false);
    layout->addWidget(certTierLabel_);
    certFindingsLabel_ = new QLabel(content);
    certFindingsLabel_->setWordWrap(true);
    certFindingsLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    certFindingsLabel_->setVisible(false);
    layout->addWidget(certFindingsLabel_);
    certProvidersLabel_ = new QLabel(content);
    certProvidersLabel_->setWordWrap(true);
    certProvidersLabel_->setProperty("aidaTone", QStringLiteral("secondary"));
    certProvidersLabel_->setVisible(false);
    layout->addWidget(certProvidersLabel_);

    auto* handoffRow = new QHBoxLayout();
    handoffRow->setSpacing(t.spacing.sm);
    handoffButton_ = new widgets::AidaButton("Generate handoff", content);
    handoffButton_->setKind(widgets::AidaButton::Kind::Secondary);
    handoffButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    handoffButton_->setVisible(false);
    handoffRow->addWidget(handoffButton_);
    handoffStatusLabel_ = new QLabel(content);
    handoffStatusLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    handoffRow->addWidget(handoffStatusLabel_, 1);
    layout->addLayout(handoffRow);

    splitter_ = new QSplitter(Qt::Vertical, content);
    splitter_->setOpaqueResize(true);
    splitter_->setChildrenCollapsible(false);

    model_ = new ProxyHistoryModel(splitter_);
    auto* tableHost = new QWidget(splitter_);
    tableStack_ = new QStackedLayout(tableHost);
    tableStack_->setStackingMode(QStackedLayout::StackOne);
    tableStack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.proxy.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    table_->horizontalHeader()->setDefaultSectionSize(table_column_width_chars(table_, 7));
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setModel(model_);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    tableStack_->addWidget(table_);
    emptyView_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No proxy history"),
        QStringLiteral("Start the proxy and browse through it to record HTTP exchanges."),
        tableHost);
    emptyView_->setObjectName(QStringLiteral("aida.view.network.proxy.empty"));
    emptyView_->setActionLabel(QStringLiteral("Start Proxy"));
    connect(emptyView_, &widgets::AidaStateView::actionTriggered, this, [this] {
        if (startStopButton_->isEnabled())
            startStopButton_->click();
    });
    tableStack_->addWidget(emptyView_);
    connect(model_, &QAbstractItemModel::rowsInserted, this, [this] { updateEmptyState(); });
    connect(model_, &QAbstractItemModel::rowsRemoved, this, [this] { updateEmptyState(); });
    connect(model_, &QAbstractItemModel::modelReset, this, [this] { updateEmptyState(); });
    splitter_->addWidget(tableHost);

    auto* detail = new QWidget(splitter_);
    auto* detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(0, t.spacing.xs, 0, 0);
    detailLayout->setSpacing(t.spacing.xs);
    detailTitle_ = new QLabel(detail);
    detailTitle_->setProperty("aidaTone", QStringLiteral("title"));
    detailLayout->addWidget(detailTitle_);
    detailMeta_ = new QLabel(detail);
    detailMeta_->setProperty("aidaTone", QStringLiteral("secondary"));
    detailLayout->addWidget(detailMeta_);
    auto* detailButtons = new QHBoxLayout();
    detailButtons->setSpacing(t.spacing.sm);
    sendRepeaterButton_ = new widgets::AidaButton("Send to Repeater", detail);
    sendRepeaterButton_->setKind(widgets::AidaButton::Kind::Secondary);
    sendRepeaterButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    detailButtons->addWidget(sendRepeaterButton_);
    copyUrlButton_ = new widgets::AidaButton("Copy URL", detail);
    copyUrlButton_->setKind(widgets::AidaButton::Kind::Ghost);
    copyUrlButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    detailButtons->addWidget(copyUrlButton_);
    detailButtons->addStretch(1);
    detailLayout->addLayout(detailButtons);
    auto* payloadSplit = new QSplitter(Qt::Horizontal, detail);
    payloadSplit->setOpaqueResize(true);
    requestView_ = new QPlainTextEdit(payloadSplit);
    requestView_->setReadOnly(true);
    requestView_->setFont(theme::fonts::codeRegular());
    requestView_->setPlaceholderText(QStringLiteral("Request"));
    attach_http_highlighter(requestView_);
    responseView_ = new QPlainTextEdit(payloadSplit);
    responseView_->setReadOnly(true);
    responseView_->setFont(theme::fonts::codeRegular());
    responseView_->setPlaceholderText(QStringLiteral("Response"));
    attach_http_highlighter(responseView_);
    payloadSplit->addWidget(requestView_);
    payloadSplit->addWidget(responseView_);
    detailLayout->addWidget(payloadSplit, 1);
    splitter_->addWidget(detail);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 1);
    layout->addWidget(splitter_, 1);
    updateEmptyState();

    connect(startStopButton_, &QAbstractButton::clicked, this, [this] {
        std::snprintf(network_view::g_state.proxy_bind_addr,
            sizeof(network_view::g_state.proxy_bind_addr), "%s",
            bindAddrEdit_->text().toUtf8().constData());
        network_view::g_state.proxy_port = bindPortSpin_->value();
        network_view::g_state.proxy_decode_tls = decodeTlsCheck_->isChecked();
        const auto snapshot = network_view::proxy_runtime_snapshot();
        const bool running = snapshot && snapshot->stats.running;
        aida::ui::application_ui::execute_action(
            running ? "network.proxy.stop" : "network.proxy.start",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(clearHistoryButton_, &QAbstractButton::clicked, this, [] {
        aida::ui::application_ui::execute_action("network.proxy.history.clear",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(filterEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        std::snprintf(network_view::g_state.proxy_filter_text,
            sizeof(network_view::g_state.proxy_filter_text), "%s", text.toUtf8().constData());
        model_->setFilter(text);
        network_view::request_proxy_runtime_snapshot(true);
    });
    connect(prepareBrowserButton_, &QAbstractButton::clicked, this, [] {
        network_view::open_view("view.network.browser");
    });
    connect(camoufoxControlsButton_, &QAbstractButton::clicked, this, [] {
        network_view::open_view("view.network.browser");
        toast_notification::push("Camoufox is the only supported browser.",
            toast_notification::toast_type_t::info);
    });
    connect(repairTrustButton_, &QAbstractButton::clicked, this, [] {
        aida::ui::application_ui::execute_action("network.proxy.ca_trust_repair",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(legacyRevertButton_, &QAbstractButton::clicked, this, [this] {
        revertLegacyPatches();
    });
    connect(certDiagButton_, &QAbstractButton::clicked, this, [this] {
        const auto snapshot = network_view::proxy_runtime_snapshot();
        if (certPidSpin_->value() <= 0) {
            hasReport_ = false;
            certStatusLabel_->setText("Select a live PID before diagnostics");
            return;
        }
        cert_intercept::diagnostic_context_t context;
        context.proxy_running = snapshot && snapshot->stats.running;
        context.ca_trusted = snapshot && snapshot->ca_installed;
        context.controlled_browser = snapshot && snapshot->controlled_browser_running;
        context.proxy_endpoint = std::string(network_view::g_state.proxy_bind_addr) + ":" +
            std::to_string(network_view::g_state.proxy_port);
        network_view::request_certificate_diagnostics(
            static_cast<std::uint32_t>(certPidSpin_->value()), std::move(context));
        certStatusLabel_->setText("Diagnosing...");
        refreshButtons();
    });
    connect(handoffButton_, &QAbstractButton::clicked, this, [this] {
        if (!hasReport_)
            return;
        network_view::request_certificate_handoff(report_, providers_,
            std::string(network_view::g_state.proxy_bind_addr) + ":" +
                std::to_string(network_view::g_state.proxy_port));
        handoffStatusLabel_->setText("Generating...");
        refreshButtons();
    });
    connect(sendRepeaterButton_, &QAbstractButton::clicked, this, [this] {
        const auto* exchange = model_->rowAt(table_->selectionModel()->currentIndex().isValid()
            ? table_->selectionModel()->currentIndex().row() : -1);
        if (!exchange)
            return;
        std::string unavailable;
        network_view::execute_retained_exchange_toolbar_action("network.exchange.repeater",
            network_view::exchange_artifact_identity(*exchange, network_view::artifact_kind_t::request),
            network_view::exchange_artifact_identity(*exchange, network_view::artifact_kind_t::response),
            unavailable);
    });
    connect(copyUrlButton_, &QAbstractButton::clicked, this, [this] {
        const auto* exchange = model_->rowAt(table_->selectionModel()->currentIndex().isValid()
            ? table_->selectionModel()->currentIndex().row() : -1);
        if (!exchange)
            return;
        std::string unavailable;
        network_view::execute_retained_exchange_toolbar_action("network.exchange.copy_url",
            network_view::exchange_artifact_identity(*exchange, network_view::artifact_kind_t::request),
            network_view::exchange_artifact_identity(*exchange, network_view::artifact_kind_t::response),
            unavailable);
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* exchange = model_->rowAt(current.isValid() ? current.row() : -1);
            selectedExchangeId_ = exchange ? exchange->id : 0;
            if (exchange) {
                network_view::publish_network_selection(
                    network_view::exchange_artifact_identity(*exchange,
                        network_view::artifact_kind_t::request), true);
            } else {
                network_view::clear_stale_network_selection("view.network.proxy");
            }
            updateDetail();
        });
    connect(table_, &QWidget::customContextMenuRequested, this, [this](const QPoint& viewportPos) {
        const QModelIndex index = table_->indexAt(viewportPos);
        if (index.isValid())
            table_->setCurrentIndex(index);
        if (index.isValid())
            showContextForRow(index.row(), table_->viewport()->mapToGlobal(viewportPos),
                aida::ui::context_menu_open_origin_t::pointer);
    });
    table_->viewport()->installEventFilter(this);

    if (auto* bridge = NetworkMonitorBridge::instance()) {
        connect(bridge, &NetworkMonitorBridge::proxySnapshot, this, &ProxyPane::onSnapshot);
        connect(bridge, &NetworkMonitorBridge::certDiagnosticsResult, this,
            &ProxyPane::onCertDiagnostics);
        connect(bridge, &NetworkMonitorBridge::certHandoffResult, this, &ProxyPane::onCertHandoff);
    }

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(250);
    connect(pollTimer_, &QTimer::timeout, this, [this] {
        network_view::request_proxy_runtime_snapshot();
        refreshButtons();
    });

    setContent(content);
    refreshButtons();
    updateDetail();
}

void ProxyPane::onPaneShown() {
    network_view::request_driver_available_snapshot(true);
    network_view::request_proxy_runtime_snapshot(true);
    pollTimer_->start();
    refreshButtons();
}

bool ProxyPane::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_->viewport() && event->type() == QEvent::ContextMenu) {
        auto* contextEvent = static_cast<QContextMenuEvent*>(event);
        if (contextEvent->reason() == QContextMenuEvent::Keyboard) {
            const QModelIndex current = table_->selectionModel()->currentIndex();
            if (!current.isValid())
                return false;
            const auto* row = model_->rowAt(current.row());
            if (!row)
                return false;
            const QRect rect = table_->visualRect(current);
            showContextForRow(current.row(),
                table_->viewport()->mapToGlobal(rect.center()),
                aida::ui::context_menu_open_origin_t::menu_key);
            return true;
        }
    }
    return NetworkPaneBase::eventFilter(watched, event);
}

void ProxyPane::onSnapshot(
    std::shared_ptr<const network_view::proxy_runtime_snapshot_t> snapshot) {
    if (!snapshot)
        return;
    model_->adoptHistory(snapshot->history);
    sparkline_->sample(snapshot->stats.total_requests);
    bypassActive_ = snapshot->bypass_active;
    bypassCount_ = snapshot->bypass_count;
    refreshRuntime();
    refreshButtons();
    updateDetail();
}

void ProxyPane::onCertDiagnostics(bool success, cert_intercept::process_diagnostics_t report,
                                  std::vector<cert_intercept::provider_status_t> providers,
                                  QString status) {
    hasReport_ = success;
    report_ = std::move(report);
    providers_ = std::move(providers);
    certStatusLabel_->setText(status);

    certTierLabel_->setVisible(success);
    certFindingsLabel_->setVisible(success);
    certProvidersLabel_->setVisible(success && !providers_.empty());
    if (success) {
        certTierLabel_->setText(QStringLiteral("Recommended tier: %1")
            .arg(QString::fromStdString(report_.recommended_tier)));
        QString findings;
        int shown = 0;
        for (const auto& finding : report_.findings) {
            if (shown++ >= 3)
                break;
            findings += QStringLiteral("%1: %2\n%3\n")
                .arg(QString::fromStdString(cert_intercept::to_string(finding.severity)))
                .arg(QString::fromStdString(finding.title))
                .arg(QString::fromStdString(finding.next_action));
        }
        certFindingsLabel_->setText(findings.trimmed());
        if (!providers_.empty()) {
            QStringList names;
            for (const auto& provider : providers_)
                names << QStringLiteral("%1:%2")
                    .arg(QString::fromStdString(provider.descriptor.provider_id))
                    .arg(QString::fromStdString(cert_intercept::to_string(provider.state)));
            certProvidersLabel_->setText(QStringLiteral("Providers: %1").arg(names.join("  ")));
        }
    }
    refreshButtons();
}

void ProxyPane::onCertHandoff(bool success, QString status) {
    handoffStatusLabel_->setText(status);
    handoffStatusLabel_->setVisible(true);
    refreshButtons();
}

void ProxyPane::refreshRuntime() {
    const auto snapshot = network_view::proxy_runtime_snapshot();
    const bool running = snapshot && snapshot->stats.running;
    const bool caInstalled = snapshot && snapshot->ca_installed;
    const bool controlledRunning = snapshot && snapshot->controlled_browser_running;

    runningPill_->setText(running
        ? QStringLiteral("Proxy running  %1:%2")
            .arg(QString::fromStdString(std::string(network_view::g_state.proxy_bind_addr)))
            .arg(network_view::g_state.proxy_port)
        : "Proxy stopped");
    set_label_variant(runningPill_, running ? "success" : "warning");
    caPill_->setText(caInstalled ? "AiDA CA trusted" : "AiDA CA not trusted");
    set_label_variant(caPill_, caInstalled ? "success" : "warning");
    controlledPill_->setText(controlledRunning ? "Controlled active" : "Controlled ready");
    set_label_variant(controlledPill_, controlledRunning ? "success" : "neutral");

    if (running && snapshot) {
        statsLabel_->setText(QStringLiteral("%1 req  %2 active  In %3  Out %4")
            .arg(static_cast<quint64>(snapshot->stats.total_requests))
            .arg(snapshot->stats.active_connections)
            .arg(QString::fromStdString(format_bytes(snapshot->stats.total_bytes_in)))
            .arg(QString::fromStdString(format_bytes(snapshot->stats.total_bytes_out))));
    } else {
        statsLabel_->clear();
    }
    sparkline_->setVisible(running);

    legacyRow_->setVisible(bypassActive_);
    if (bypassActive_)
        legacyPill_->setText(QStringLiteral("Legacy cleanup  -  %1 patches").arg(bypassCount_));
}

void ProxyPane::refreshButtons() {
    const auto snapshot = network_view::proxy_runtime_snapshot();
    const bool running = snapshot && snapshot->stats.running;
    const bool pending = network_view::proxy_operation_pending();
    startStopButton_->setText(pending ? (running ? "Stopping..." : "Starting...")
        : running ? "Stop" : "Start Proxy");
    startStopButton_->setKind(running ? widgets::AidaButton::Kind::Destructive
                                      : widgets::AidaButton::Kind::Primary);
    startStopButton_->setEnabled(!pending);
    clearHistoryButton_->setEnabled(!pending && snapshot && !snapshot->history.empty());
    repairTrustButton_->setEnabled(!pending);
    const bool certPending = network_view::cert_diagnostics_pending();
    const bool handoffPending = network_view::cert_handoff_pending();
    certDiagButton_->setEnabled(!certPending);
    certDiagButton_->setText(certPending ? "Diagnosing..." : "Diagnose target");
    const bool canHandoff = hasReport_ &&
        (report_.primary == cert_intercept::classification_t::true_pinning ||
         report_.primary == cert_intercept::classification_t::app_specific_tls_stack);
    handoffButton_->setVisible(canHandoff);
    handoffButton_->setEnabled(canHandoff && !handoffPending);
    handoffButton_->setText(handoffPending ? "Generating..." : "Generate handoff");
}

void ProxyPane::updateEmptyState() {
    if (!tableStack_ || !emptyView_ || !table_ || !model_)
        return;
    tableStack_->setCurrentWidget(model_->rowCount() == 0
        ? static_cast<QWidget*>(emptyView_) : static_cast<QWidget*>(table_));
}

void ProxyPane::updateDetail() {
    const auto* exchange = model_->rowAt(table_->selectionModel()->currentIndex().isValid()
        ? table_->selectionModel()->currentIndex().row() : -1);
    if (!exchange) {
        detailTitle_->clear();
        detailMeta_->clear();
        requestView_->clear();
        responseView_->clear();
        return;
    }
    detailTitle_->setText(QStringLiteral("%1  %2")
        .arg(QString::fromStdString(exchange->request.method))
        .arg(QString::fromStdString(exchange->request.uri)));
    detailMeta_->setText(QStringLiteral("%1:%2  %3  %4ms  req=%5  resp=%6")
        .arg(QString::fromStdString(exchange->target_host))
        .arg(exchange->target_port)
        .arg(exchange->is_tls ? "TLS" : "Plain")
        .arg(static_cast<quint64>(exchange->latency_ms))
        .arg(QString::fromStdString(format_bytes(exchange->raw_request.size())))
        .arg(QString::fromStdString(format_bytes(exchange->response_size))));
    requestView_->setPlainText(QString::fromStdString(payload_display_text(exchange->raw_request)));
    responseView_->setPlainText(QString::fromStdString(payload_display_text(exchange->raw_response)));
}

void ProxyPane::showContextForRow(int row, const QPoint& globalPos,
                                  aida::ui::context_menu_open_origin_t origin) {
    const auto* exchange = model_->rowAt(row);
    if (!exchange)
        return;
    selectedExchangeId_ = exchange->id;
    network_view::publish_network_selection(
        network_view::exchange_artifact_identity(*exchange, network_view::artifact_kind_t::request),
        true);
    exchange_context_host().show(table_, globalPos,
        network_view::exchange_artifact_identity(*exchange, network_view::artifact_kind_t::request),
        network_view::exchange_artifact_identity(*exchange, network_view::artifact_kind_t::response),
        origin == aida::ui::context_menu_open_origin_t::pointer
            ? network_view::exchange_context_origin_t::pointer
            : network_view::exchange_context_origin_t::menu_key);
}

void ProxyPane::revertLegacyPatches() {
    if (bypassCount_ == 0)
        return;
    auto* dialog = new bridge::AidaDialog(this);
    dialog->setWindowTitle("Review Legacy Patch Reversion");
    auto* layout = new QVBoxLayout(dialog);
    auto* body = new QLabel(QStringLiteral("Revert %1 live certificate bypass patches?").arg(bypassCount_), dialog);
    layout->addWidget(body);
    auto* note = new QLabel("The worker will restore reviewed process memory and verify that no bypass remains active.", dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("Confirm Revert");
    layout->addWidget(buttons);
    const std::size_t reviewedCount = bypassCount_;
    connect(buttons, &QDialogButtonBox::accepted, dialog, [dialog, reviewedCount] {
        network_view::request_legacy_bypass_revert(reviewedCount);
        dialog->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

}
