#include "qt/network/monitor/capture_pane.hpp"

#include <QAbstractAnimation>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QEvent>
#include <QFont>
#include <QItemSelectionModel>
#include <QWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>

#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/shell_host_contract.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/network/shared/monitor_bridge.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

CaptureModel::CaptureModel(QObject* parent)
    : RingTableModel(8192, parent) {
    setFilter([this](const network_view::packet_entry_t& entry) {
        if (filterPid_ != 0 && entry.pid != filterPid_)
            return false;
        if (filterProtocol_ != 0 && entry.protocol != filterProtocol_)
            return false;
        if (filterPort_ != 0 && entry.src_port != filterPort_ && entry.dst_port != filterPort_)
            return false;
        if (!filterText_.isEmpty()) {
            const std::string src = format_ip(entry.src_addr, 2) + ":" + std::to_string(entry.src_port);
            const std::string dst = format_ip(entry.dst_addr, 2) + ":" + std::to_string(entry.dst_port);
            const std::string all = src + " " + dst + " " + entry.protocol_label + " " + entry.summary;
            if (!filter_text_match(filterText_, all))
                return false;
        }
        return true;
    });
}

void CaptureModel::applyFilterSpec(quint32 pid, quint16 port, quint8 protocol,
                                   const QString& text) {
    filterPid_ = pid;
    filterPort_ = port;
    filterProtocol_ = protocol;
    filterText_ = text;
    refilter();
}

int CaptureModel::visibleIndexForKey(quint64 timestamp, quint32 pid, quint16 srcPort,
                                     quint16 dstPort, quint32 payloadSize) const noexcept {
    const auto& ringData = ringRef();
    for (int row = 0; row < rowCount(); ++row) {
        const auto* entry = visibleRowAt(row);
        if (entry && entry->timestamp == timestamp && entry->pid == pid &&
            entry->src_port == srcPort && entry->dst_port == dstPort &&
            entry->payload_size == payloadSize)
            return row;
    }
    static_cast<void>(ringData);
    return -1;
}

int CaptureModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant CaptureModel::cellData(const network_view::packet_entry_t& row, int column,
                                int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Index: return static_cast<quint64>(row.timestamp);
        case Time:  return QString::fromStdString(format_timestamp(row.timestamp));
        case Src:   return QStringLiteral("%1:%2")
            .arg(QString::fromStdString(format_ip(row.src_addr, 2))).arg(row.src_port);
        case Dst:   return QStringLiteral("%1:%2")
            .arg(QString::fromStdString(format_ip(row.dst_addr, 2))).arg(row.dst_port);
        case Proto: return QString::fromStdString(row.protocol_label);
        case Info:  return QString::fromStdString(capture_row_info_text(row.summary));
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        if (column == Info)
            return QString::fromStdString(capture_row_info_text(row.summary));
        return {};
    }
    if (role == Qt::ForegroundRole) {
        switch (column) {
        case Time: return t.text_dim;
        case Proto: {
            switch (protocol_label_semantic(row.protocol_label)) {
            case net_semantic_t::info:    return t.info;
            case net_semantic_t::success: return t.success;
            case net_semantic_t::warning: return t.warning;
            case net_semantic_t::accent:  return t.accent;
            default: return t.text_dim;
            }
        }
        case Info: {
            const auto& summary = row.summary;
            static const char* k_methods[] = { "GET ", "POST ", "PUT ", "DELETE ", "PATCH ",
                "HEAD ", "OPTIONS " };
            for (const char* method : k_methods) {
                const std::size_t len = std::strlen(method);
                if (summary.size() >= len && summary.compare(0, len, method) == 0)
                    return http_method_color(method);
            }
            return t.text_secondary;
        }
        default: return t.text_secondary;
        }
    }
    return {};
}

QVariant CaptureModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Index: return QStringLiteral("#");
    case Time:  return QStringLiteral("Time");
    case Src:   return QStringLiteral("Src");
    case Dst:   return QStringLiteral("Dst");
    case Proto: return QStringLiteral("Proto");
    case Info:  return QStringLiteral("Info");
    default: return {};
    }
}

CaptureLiveBadge::CaptureLiveBadge(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.network.capture.live_badge"));
    pulse_ = theme::motion::loop(theme::tokens().motion.hero, this);
    pulse_->setStartValue(0.45);
    pulse_->setEndValue(1.0);
    connect(pulse_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        pulseValue_ = value.toReal();
        update();
    });
}

void CaptureLiveBadge::setState(bool running, bool starting, bool stopping,
                                double packetsPerSecond) {
    running_ = running;
    starting_ = starting;
    stopping_ = stopping;
    rate_ = packetsPerSecond;
    if (running && !theme::AidaMotion::reducedMotion() &&
        pulse_->state() != QAbstractAnimation::Running)
        pulse_->start();
    else if ((!running || theme::AidaMotion::reducedMotion()) &&
        pulse_->state() == QAbstractAnimation::Running)
        pulse_->stop();
    updateGeometry();
    update();
}

QSize CaptureLiveBadge::sizeHint() const {
    const auto& t = theme::tokens();
    const QFontMetricsF fm(font());
    const qreal textWidth = fm.horizontalAdvance(QStringLiteral("LIVE  -  0000.0 pkt/s"));
    return QSize(qCeil(textWidth + t.spacing.xxl + t.spacing.xxl + t.spacing.xs),
        t.row.compact);
}

void CaptureLiveBadge::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto& t = theme::tokens();
    const QRectF face = rect().adjusted(1, 1, -1, -1);
    const QColor base = running_ ? t.error : t.text_secondary;
    QColor fill = base;
    fill.setAlphaF(0.18);
    QColor border = base;
    border.setAlphaF(0.55);
    painter.setBrush(fill);
    painter.setPen(QPen(border, 1.0));
    painter.drawRoundedRect(face, face.height() / 2.0, face.height() / 2.0);

    const qreal dotY = face.center().y();
    const qreal dotX = face.left() + t.spacing.md;
    if (running_) {
        QColor halo = t.error;
        halo.setAlphaF(0.35 * pulseValue_);
        painter.setPen(Qt::NoPen);
        painter.setBrush(halo);
        painter.drawEllipse(QPointF(dotX, dotY), 6.0, 6.0);
        painter.setBrush(t.error);
        painter.drawEllipse(QPointF(dotX, dotY), 4.0, 4.0);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(t.text_dim);
        painter.drawEllipse(QPointF(dotX, dotY), 4.0, 4.0);
    }

    QString text;
    if (running_)
        text = QStringLiteral("LIVE  -  %1 pkt/s").arg(rate_, 0, 'f', 1);
    else if (starting_ || stopping_)
        text = starting_ ? QStringLiteral("STARTING") : QStringLiteral("STOPPING");
    else
        text = QStringLiteral("PAUSED");
    painter.setPen(running_ ? t.error : t.text_secondary);
    painter.drawText(QRectF(face.left() + t.spacing.xxl, face.top(),
        face.width() - t.spacing.xxl - t.spacing.xs, face.height()),
        Qt::AlignVCenter | Qt::AlignLeft, text);
}

CaptureDetailWidget::CaptureDetailWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding_compact, t.panel.padding_compact,
        t.panel.padding_compact, t.panel.padding_compact);
    layout->setSpacing(t.spacing.xs);

    titleLabel_ = new QLabel(this);
    titleLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    layout->addWidget(titleLabel_);
    metaLabel_ = new QLabel(this);
    metaLabel_->setProperty("aidaTone", QStringLiteral("secondary"));
    layout->addWidget(metaLabel_);
    summaryLabel_ = new QLabel(this);
    summaryLabel_->setWordWrap(true);
    layout->addWidget(summaryLabel_);

    copyButton_ = new widgets::AidaButton("Copy Payload", this);
    copyButton_->setKind(widgets::AidaButton::Kind::Ghost);
    copyButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    copyButton_->setVisible(false);
    connect(copyButton_, &QAbstractButton::clicked, this, [this] {
        if (!currentPayloadText_.isEmpty())
            clipboard::set_text(currentPayloadText_);
    });
    layout->addWidget(copyButton_, 0, Qt::AlignLeft);

    hexView_ = new QPlainTextEdit(this);
    hexView_->setReadOnly(true);
    hexView_->setFont(theme::fonts::codeRegular());
    layout->addWidget(hexView_, 1);
    clearPacket();
}

void CaptureDetailWidget::showPacket(const network_view::packet_entry_t& packet, int packetIndex) {
    titleLabel_->setText(QStringLiteral("Packet #%1  -  %2")
        .arg(packetIndex + 1)
        .arg(QString::fromStdString(packet.protocol_label)));
    set_label_tone(titleLabel_, "titleAccent");
    metaLabel_->setText(QStringLiteral("%1:%2 -> %3:%4  -  PID: %5  -  %6 bytes  -  %7")
        .arg(QString::fromStdString(format_ip(packet.src_addr, 2))).arg(packet.src_port)
        .arg(QString::fromStdString(format_ip(packet.dst_addr, 2))).arg(packet.dst_port)
        .arg(packet.pid)
        .arg(packet.payload_size)
        .arg(packet.direction == 0 ? "Inbound" : "Outbound"));
    summaryLabel_->setText(QString::fromStdString(packet.summary));
    summaryLabel_->setVisible(!packet.summary.empty());

    if (packet.payload.empty()) {
        hexView_->setPlainText(QString());
        hexView_->setVisible(false);
        copyButton_->setVisible(false);
        currentPayloadText_.clear();
        return;
    }
    const std::size_t displaySize = std::min(packet.payload.size(), static_cast<std::size_t>(4096));
    QString hex;
    hex.reserve(static_cast<qsizetype>(displaySize * 4 + 64));
    for (std::size_t off = 0; off < displaySize; off += 16) {
        char line[128];
        int pos = snprintf(line, sizeof(line), "%04X  ", static_cast<unsigned>(off));
        const std::size_t end = std::min(off + 16, displaySize);
        for (std::size_t j = off; j < off + 16; j++) {
            if (j < end)
                pos += snprintf(line + pos, sizeof(line) - static_cast<std::size_t>(pos), "%02X ",
                    static_cast<unsigned>(packet.payload[j]));
            else
                pos += snprintf(line + pos, sizeof(line) - static_cast<std::size_t>(pos), "   ");
            if (j == off + 7)
                pos += snprintf(line + pos, sizeof(line) - static_cast<std::size_t>(pos), " ");
        }
        pos += snprintf(line + pos, sizeof(line) - static_cast<std::size_t>(pos), " |");
        for (std::size_t j = off; j < end; j++) {
            const char c = static_cast<char>(packet.payload[j]);
            line[pos++] = (c >= 32 && c < 127) ? c : '.';
        }
        line[pos++] = '|';
        line[pos] = '\0';
        hex += QString::fromLatin1(line);
        hex += u'\n';
    }
    if (displaySize < packet.payload.size())
        hex += QStringLiteral("... %1 more bytes").arg(packet.payload.size() - displaySize);
    hexView_->setPlainText(hex);
    hexView_->setVisible(true);
    copyButton_->setVisible(true);
    currentPayloadText_ = QString::fromStdString(payload_display_text(packet.payload, 262144));
}

void CaptureDetailWidget::clearPacket() {
    titleLabel_->setText("Select a packet to view details");
    set_label_tone(titleLabel_, "dim");
    metaLabel_->clear();
    summaryLabel_->clear();
    hexView_->clear();
    hexView_->setVisible(false);
    copyButton_->setVisible(false);
    currentPayloadText_.clear();
}

CapturePane::CapturePane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.capture"));
    setRequiresTarget(true);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(t.spacing.sm);
    startStopButton_ = new widgets::AidaButton("Start Capture", content);
    startStopButton_->setKind(widgets::AidaButton::Kind::Primary);
    startStopButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    topRow->addWidget(startStopButton_);

    badge_ = new CaptureLiveBadge(content);
    topRow->addWidget(badge_);

    statusLabel_ = new QLabel(content);
    statusLabel_->setProperty("aidaTone", QStringLiteral("secondary"));
    topRow->addWidget(statusLabel_, 1);
    layout->addLayout(topRow);

    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(t.spacing.sm);
    filterRow->addWidget(new QLabel("PID:", content));
    filterPid_ = new QSpinBox(content);
    filterPid_->setRange(0, 0x7FFFFFFF);
    filterPid_->setSpecialValueText(QStringLiteral("0"));
    filterRow->addWidget(filterPid_);
    filterRow->addWidget(new QLabel("Port:", content));
    filterPort_ = new QSpinBox(content);
    filterPort_->setRange(0, 65535);
    filterRow->addWidget(filterPort_);
    filterRow->addWidget(new QLabel("Proto:", content));
    filterProtocol_ = new QComboBox(content);
    filterProtocol_->addItems({"All", "TCP", "UDP"});
    filterRow->addWidget(filterProtocol_);
    clearButton_ = new widgets::AidaButton("Clear", content);
    clearButton_->setKind(widgets::AidaButton::Kind::Secondary);
    clearButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    filterRow->addWidget(clearButton_);
    filterText_ = new QLineEdit(content);
    filterText_->setPlaceholderText("Filter packets...");
    filterText_->setMaxLength(128);
    filterRow->addWidget(filterText_, 1);
    countLabel_ = new QLabel("0 packets", content);
    countLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    filterRow->addWidget(countLabel_);
    autoScroll_ = new QCheckBox("Auto-scroll", content);
    autoScroll_->setChecked(true);
    filterRow->addWidget(autoScroll_);
    layout->addLayout(filterRow);

    splitter_ = new QSplitter(Qt::Vertical, content);
    splitter_->setOpaqueResize(true);
    splitter_->setChildrenCollapsible(false);

    model_ = new CaptureModel(content);
    auto* tableHost = new QWidget(splitter_);
    tableStack_ = new QStackedLayout(tableHost);
    tableStack_->setStackingMode(QStackedLayout::StackOne);
    tableStack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.capture.table"));
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
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    tableStack_->addWidget(table_);
    emptyView_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No packets captured"),
        QStringLiteral("Start Capture to record driver-observed traffic, or adjust the filters."),
        tableHost);
    emptyView_->setObjectName(QStringLiteral("aida.view.network.capture.empty"));
    emptyView_->setActionLabel(QStringLiteral("Start Capture"));
    connect(emptyView_, &widgets::AidaStateView::actionTriggered, this, [this] {
        if (startStopButton_->isEnabled())
            startStopButton_->click();
    });
    tableStack_->addWidget(emptyView_);
    splitter_->addWidget(tableHost);

    detail_ = new CaptureDetailWidget(splitter_);
    splitter_->addWidget(detail_);
    splitter_->setStretchFactor(0, 13);
    splitter_->setStretchFactor(1, 7);
    layout->addWidget(splitter_, 1);
    updateEmptyState();

    connect(startStopButton_, &QAbstractButton::clicked, this, [] {
        const bool running = network_view::g_state.cap_running.load(std::memory_order_acquire);
        aida::ui::application_ui::execute_action(
            running ? "network.capture.stop" : "network.capture.start",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(clearButton_, &QAbstractButton::clicked, this, [this] {
        clearCapture();
    });
    const auto filterChanged = [this] {
        model_->applyFilterSpec(static_cast<quint32>(filterPid_->value()),
            static_cast<quint16>(filterPort_->value()),
            filterProtocol_->currentIndex() == 1 ? 6 : filterProtocol_->currentIndex() == 2 ? 17 : 0,
            filterText_->text());
        network_view::g_state.cap_filter_pid = static_cast<std::uint32_t>(filterPid_->value());
        network_view::g_state.cap_filter_port = static_cast<std::uint16_t>(filterPort_->value());
        network_view::g_state.cap_filter_protocol = static_cast<std::uint8_t>(
            filterProtocol_->currentIndex() == 1 ? 6 : filterProtocol_->currentIndex() == 2 ? 17 : 0);
    };
    connect(filterPid_, &QSpinBox::valueChanged, this, filterChanged);
    connect(filterPort_, &QSpinBox::valueChanged, this, filterChanged);
    connect(filterProtocol_, &QComboBox::currentIndexChanged, this, filterChanged);
    connect(filterText_, &QLineEdit::textChanged, this, filterChanged);

    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            selected_ = current;
            updateDetail();
        });
    connect(table_, &QWidget::customContextMenuRequested, this, &CapturePane::openContextMenu);
    table_->viewport()->installEventFilter(this);
    connect(model_, &QAbstractItemModel::rowsInserted, this, [this] { updateEmptyState(); });
    connect(model_, &QAbstractItemModel::rowsRemoved, this, [this] { updateEmptyState(); });
    connect(model_, &QAbstractItemModel::modelReset, this, [this] { updateEmptyState(); });

    if (auto* bridge = NetworkMonitorBridge::instance()) {
        connect(bridge, &NetworkMonitorBridge::captureBatch, this, &CapturePane::onBatch);
        connect(bridge, &NetworkMonitorBridge::captureCleared, this, [this] {
            model_->clearRows();
            selected_ = {};
            updateDetail();
        });
    }

    rateTimer_ = new QTimer(this);
    rateTimer_->setInterval(250);
    connect(rateTimer_, &QTimer::timeout, this, [this] {
        refreshButtons();
        const std::uint64_t buffered = network_view::capture_buffered_count();
        countLabel_->setText(QStringLiteral("%1 packets").arg(static_cast<quint64>(buffered)));
        statusLabel_->setText(QString::fromStdString(network_view::capture_control_status_text()));
    });
    rateClock_.start();

    setContent(content);
    refreshButtons();
    updateDetail();
}

void CapturePane::onPaneShown() {
    network_view::request_driver_available_snapshot(true);
    const auto snapshot = network_view::capture_packets_snapshot();
    if (snapshot && !snapshot->empty() && model_->ringCount() == 0)
        model_->appendBatch(snapshot);
    rateTimer_->start();
    refreshButtons();
}

void CapturePane::onPaneHidden() {
    rateTimer_->stop();
}

void CapturePane::onBatch(
    std::shared_ptr<const std::vector<network_view::packet_entry_t>> batch,
    quint64 trimmedFromFront) {
    if (!batch || batch->empty())
        return;
    const bool atBottom = table_->verticalScrollBar()->value() >=
        table_->verticalScrollBar()->maximum() - 4;
    model_->appendBatch(batch, static_cast<std::size_t>(trimmedFromFront));
    rateWindowCount_ += batch->size();
    const qint64 elapsedMs = rateClock_.isValid() ? rateClock_.elapsed() : 0;
    if (elapsedMs >= 250) {
        const double seconds = elapsedMs / 1000.0;
        const double rate = seconds > 0.0 ? rateWindowCount_ / seconds : 0.0;
        rateEma_ = rateEma_ * 0.65 + rate * 0.35;
        rateWindowCount_ = 0;
        rateClock_.restart();
    }
    if (selected_.isValid()) {
        const auto* row = model_->visibleRowAt(selected_.row());
        if (!row) {
            selected_ = {};
            updateDetail();
        }
    }
    if (autoScroll_->isChecked() && atBottom && model_->rowCount() > 0)
        table_->scrollTo(model_->index(model_->rowCount() - 1, 0));
    refreshButtons();
}

void CapturePane::clearCapture() {
    {
        std::lock_guard<std::mutex> lock(network_view::g_state.cap_mutex);
        network_view::g_state.captured_packets.clear();
    }
    model_->clearRows();
    selected_ = {};
    updateDetail();
}

void CapturePane::updateDetail() {
    if (!selected_.isValid()) {
        detail_->clearPacket();
        return;
    }
    const auto* row = model_->visibleRowAt(selected_.row());
    if (!row) {
        detail_->clearPacket();
        return;
    }
    detail_->showPacket(*row, selected_.row());
}

void CapturePane::updateEmptyState() {
    if (!tableStack_ || !emptyView_ || !table_ || !model_)
        return;
    tableStack_->setCurrentWidget(model_->rowCount() == 0
        ? static_cast<QWidget*>(emptyView_) : static_cast<QWidget*>(table_));
}

void CapturePane::openContextMenu(const QPoint& viewportPos) {
    const QModelIndex index = table_->indexAt(viewportPos);
    if (index.isValid())
        table_->setCurrentIndex(index);
    if (!index.isValid())
        return;
    showContextForPacket(index.row(), table_->viewport()->mapToGlobal(viewportPos),
        aida::ui::context_menu_open_origin_t::pointer);
}

bool CapturePane::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_->viewport() && event->type() == QEvent::ContextMenu) {
        auto* contextEvent = static_cast<QContextMenuEvent*>(event);
        if (contextEvent->reason() == QContextMenuEvent::Keyboard) {
            const QModelIndex current = table_->selectionModel()->currentIndex();
            if (!current.isValid())
                return false;
            const auto* row = model_->visibleRowAt(current.row());
            if (!row)
                return false;
            const QRect rect = table_->visualRect(current);
            const QPoint globalPos = table_->viewport()->mapToGlobal(rect.center());
            showContextForPacket(current.row(), globalPos,
                aida::ui::context_menu_open_origin_t::menu_key);
            return true;
        }
    }
    return NetworkPaneBase::eventFilter(watched, event);
}

void CapturePane::showContextForPacket(int visibleRow, const QPoint& globalPos,
                                       aida::ui::context_menu_open_origin_t origin) {
    const auto* packet = model_->visibleRowAt(visibleRow);
    if (!packet)
        return;
    const auto packetCopy = *packet;
    const auto retained_timestamp = packetCopy.timestamp;
    const auto retained_size = packetCopy.payload.size();

    aida::ui::application_ui::retained_entity_context_t context;
    context.owner_id = "network.capture.packet";
    context.entity_id = std::to_string(packetCopy.timestamp) + ":" +
        std::to_string(visibleRow);
    context.entity_generation = packetCopy.timestamp;
    context.active_view = aida::ui::stable_view_id_t("view.network.capture");
    context.validate_identity = [retained_timestamp, retained_size] {
        const auto live = network_view::capture_packets_snapshot();
        if (!live)
            return aida::ui::capability_state_t::unavailable(
                "The bounded capture buffer advanced; select a current packet");
        const auto found = std::find_if(live->begin(), live->end(),
            [&](const network_view::packet_entry_t& candidate) {
                return candidate.timestamp == retained_timestamp &&
                    candidate.payload.size() == retained_size;
            });
        return found != live->end()
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The captured packet was replaced; select it again");
    };
    const auto add = [&context](const char* id, bool enabled, const char* reason,
                        std::function<aida::ui::action_handler_result_t()> invoke) {
        aida::ui::application_ui::retained_entity_action_t action;
        action.action_id = id;
        action.capability = enabled
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(reason);
        action.invoke = std::move(invoke);
        context.actions.push_back(std::move(action));
    };
    const std::string summary = packetCopy.summary;
    add("network.capture.copy_summary", !summary.empty(),
        "The captured packet has no summary", [summary] {
            clipboard::set_text(QString::fromStdString(summary));
            return aida::ui::action_handler_result_t::completed();
        });
    const std::string src = format_ip(packetCopy.src_addr, 2) + ":" +
        std::to_string(packetCopy.src_port);
    const std::string dst = format_ip(packetCopy.dst_addr, 2) + ":" +
        std::to_string(packetCopy.dst_port);
    add("network.capture.copy_source", true, "", [src] {
        clipboard::set_text(QString::fromStdString(src));
        return aida::ui::action_handler_result_t::completed();
        });
    add("network.capture.copy_destination", true, "", [dst] {
        clipboard::set_text(QString::fromStdString(dst));
        return aida::ui::action_handler_result_t::completed();
        });
    const std::string payload = payload_display_text(packetCopy.payload, 262144);
    add("network.capture.copy_payload", !packetCopy.payload.empty(),
        "The captured packet has no payload", [payload] {
            clipboard::set_text(QString::fromStdString(payload));
            return aida::ui::action_handler_result_t::completed();
        });
    network_view::artifact_identity_t packetIdentity;
    packetIdentity.kind = network_view::artifact_kind_t::packet;
    packetIdentity.id = "network.packet." + std::to_string(packetCopy.timestamp) + "." +
        std::to_string(visibleRow);
    packetIdentity.source_view_id = "view.network.capture";
    packetIdentity.source_id = static_cast<std::uint64_t>(visibleRow + 1);
    packetIdentity.timestamp = packetCopy.timestamp;
    packetIdentity.content_size = packetCopy.payload.size();
    packetIdentity.content_hash = network_view::artifact_content_hash(packetCopy.payload);
    packetIdentity.label = packetCopy.protocol_label + " packet";
    add("network.capture.send_comparer", !packetCopy.payload.empty(),
        "The captured packet has no payload", [packetIdentity] {
            std::string reason;
            return network_view::send_artifact_to_comparer(packetIdentity, reason)
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(reason);
        });
    add("network.capture.send_chat", !packetCopy.payload.empty(),
        "The captured packet has no payload", [packetIdentity] {
            std::string reason;
            return network_view::add_artifact_to_chat(packetIdentity, reason)
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(reason);
        });
    add("network.capture.assign_agent", !packetCopy.payload.empty(),
        "The captured packet has no payload", [packetIdentity] {
            std::string reason;
            return network_view::assign_artifact_to_agent(packetIdentity, reason)
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(reason);
        });
    const auto pid = packetCopy.pid;
    add("network.capture.filter_pid", pid != 0,
        "The captured packet has no process identity", [this, pid] {
            filterPid_->setValue(static_cast<int>(pid));
            return aida::ui::action_handler_result_t::completed();
        });
    const auto protocol = packetCopy.protocol;
    add("network.capture.filter_protocol", protocol != 0,
        "The captured packet has no protocol discriminator", [this, protocol] {
            filterProtocol_->setCurrentIndex(protocol == 6 ? 1 : protocol == 17 ? 2 : 0);
            return aida::ui::action_handler_result_t::completed();
        });
    add("network.capture.toggle_follow", true, "", [this] {
            autoScroll_->setChecked(!autoScroll_->isChecked());
            return aida::ui::action_handler_result_t::completed();
        });
    add("network.capture.send_repeater", false,
        "A raw driver packet is not necessarily a complete HTTP request; use Proxy history for safe request reconstruction", {});
    add("network.capture.replay", false,
        "Raw packet replay has no capability-backed human handler in this view", {});

    documents::show_retained_entity_menu(context, origin, globalPos, table_);
}

void CapturePane::refreshButtons() {
    const bool running = network_view::g_state.cap_running.load(std::memory_order_acquire);
    const bool startPending = network_view::g_state.cap_start_pending.load(std::memory_order_acquire);
    const bool stopPending = network_view::g_state.cap_stop_pending.load(std::memory_order_acquire);
    const bool driverOk = network_view::driver_available_snapshot();
    startStopButton_->setText(running ? "Stop Capture" : "Start Capture");
    startStopButton_->setKind(running ? widgets::AidaButton::Kind::Destructive
                                      : widgets::AidaButton::Kind::Primary);
    startStopButton_->setEnabled(driverOk && !startPending && !stopPending);
    badge_->setState(running, startPending, stopPending, rateEma_);
}

}
