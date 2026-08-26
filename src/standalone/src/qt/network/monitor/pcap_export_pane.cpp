#include "qt/network/monitor/pcap_export_pane.hpp"

#include <QAbstractButton>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <cstdio>
#include <mutex>
#include <string>

#include "core/network/network_view.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_pill.hpp"

namespace aida::qt::net {

PcapExportPane::PcapExportPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.pcap"));
    setRequiresTarget(true);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* titleLabel = new QLabel("PCAP Export", content);
    titleLabel->setProperty("aidaTone", QStringLiteral("titleAccent"));
    layout->addWidget(titleLabel);

    auto* pathRow = new QHBoxLayout();
    pathRow->setSpacing(t.spacing.sm);
    pathRow->addWidget(new QLabel("Output File:", content));
    pathEdit_ = new QLineEdit(content);
    pathEdit_->setPlaceholderText("C:\\path\\to\\capture.pcap");
    pathEdit_->setMaxLength(511);
    pathRow->addWidget(pathEdit_, 1);
    browseButton_ = new widgets::AidaButton("Browse...", content);
    browseButton_->setKind(widgets::AidaButton::Kind::Secondary);
    browseButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    pathRow->addWidget(browseButton_);
    layout->addLayout(pathRow);

    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(t.spacing.sm);
    filterRow->addWidget(new QLabel("Filter PID:", content));
    filterPid_ = new QSpinBox(content);
    filterPid_->setRange(0, 0x7FFFFFFF);
    filterRow->addWidget(filterPid_);
    auto* note = new QLabel("(0 = all)", content);
    note->setProperty("aidaTone", QStringLiteral("dim"));
    filterRow->addWidget(note);
    filterRow->addWidget(new QLabel("Protocol:", content));
    filterProtocol_ = new QComboBox(content);
    filterProtocol_->addItems({"All", "TCP", "UDP"});
    filterRow->addWidget(filterProtocol_);
    filterRow->addStretch(1);
    layout->addLayout(filterRow);

    availableLabel_ = new QLabel(content);
    availableLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    layout->addWidget(availableLabel_);

    auto* exportRow = new QHBoxLayout();
    exportRow->setSpacing(t.spacing.sm);
    exportButton_ = new widgets::AidaButton("Export to PCAP", content);
    exportButton_->setKind(widgets::AidaButton::Kind::Primary);
    exportButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    exportRow->addWidget(exportButton_);
    writingPill_ = new widgets::AidaPill("Writing PCAP file...", widgets::AidaSemantic::Accent, content);
    writingPill_->setVisible(false);
    exportRow->addWidget(writingPill_);
    exportRow->addStretch(1);
    layout->addLayout(exportRow);

    errorPill_ = new widgets::AidaPill(QString(), widgets::AidaSemantic::Error, content);
    errorPill_->setVisible(false);
    layout->addWidget(errorPill_);
    donePill_ = new widgets::AidaPill(QString(), widgets::AidaSemantic::Success, content);
    donePill_->setVisible(false);
    layout->addWidget(donePill_);

    auto* separator = new QFrame(content);
    separator->setFrameShape(QFrame::HLine);
    separator->setProperty("aidaRole", QStringLiteral("separator"));
    layout->addWidget(separator);

    auto* harTitle = new QLabel("Export Proxy History", content);
    harTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    layout->addWidget(harTitle);
    proxyCountLabel_ = new QLabel(content);
    proxyCountLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    layout->addWidget(proxyCountLabel_);

    auto* harRow = new QHBoxLayout();
    harRow->setSpacing(t.spacing.sm);
    harButton_ = new widgets::AidaButton("Export Proxy as HAR", content);
    harButton_->setKind(widgets::AidaButton::Kind::Secondary);
    harButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    harRow->addWidget(harButton_);
    harRow->addStretch(1);
    layout->addLayout(harRow);
    harErrorPill_ = new widgets::AidaPill(QString(), widgets::AidaSemantic::Error, content);
    harErrorPill_->setVisible(false);
    layout->addWidget(harErrorPill_);
    harDonePill_ = new widgets::AidaPill(QString(), widgets::AidaSemantic::Success, content);
    harDonePill_->setVisible(false);
    layout->addWidget(harDonePill_);
    layout->addStretch(1);

    connect(browseButton_, &QAbstractButton::clicked, this, [this] {
        static const char k_pcapFilter[] =
            "Packet Capture (*.pcap)\0*.pcap\0"
            "All files (*.*)\0*.*\0\0";
        const auto picked = dialogs::save_file(this, "Save PCAP", k_pcapFilter, "pcap",
            pathEdit_->text());
        if (picked)
            pathEdit_->setText(QString::fromStdString(*picked));
    });
    connect(exportButton_, &QAbstractButton::clicked, this, [this] {
        std::snprintf(network_view::g_state.pcap_path, sizeof(network_view::g_state.pcap_path),
            "%s", pathEdit_->text().toUtf8().constData());
        network_view::g_state.pcap_filter_pid =
            static_cast<std::uint32_t>(filterPid_->value());
        network_view::g_state.pcap_filter_protocol = static_cast<std::uint8_t>(
            filterProtocol_->currentIndex() == 1 ? 6 : filterProtocol_->currentIndex() == 2 ? 17 : 0);
        network_view::start_pcap_export();
        refreshState();
    });
    connect(harButton_, &QAbstractButton::clicked, this, [this] {
        static const char k_harFilter[] =
            "HTTP Archive (*.har)\0*.har\0"
            "JSON (*.json)\0*.json\0"
            "All files (*.*)\0*.*\0\0";
        const auto picked = dialogs::save_file(this, "Export HAR", k_harFilter, "har");
        if (!picked)
            return;
        network_view::start_har_export(*picked);
        refreshState();
    });

    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(250);
    connect(progressTimer_, &QTimer::timeout, this, [this] {
        refreshProgress();
    });

    setContent(content);
    refreshState();
}

void PcapExportPane::onPaneShown() {
    network_view::request_driver_available_snapshot(true);
    network_view::request_proxy_runtime_snapshot();
    refreshState();
    progressTimer_->start();
}

void PcapExportPane::onPaneHidden() {
    progressTimer_->stop();
}

void PcapExportPane::refreshState() {
    if (refreshing_)
        return;
    refreshing_ = true;
    struct refresh_reset_t { bool& f; ~refresh_reset_t() { f = false; } } refresh_reset{ refreshing_ };
    refreshProgress();
    const bool writing = network_view::g_state.pcap_writing.load(std::memory_order_acquire);
    const bool harWriting = network_view::g_state.har_writing.load(std::memory_order_acquire);
    const std::size_t captureCount = network_view::capture_buffered_count();
    availableLabel_->setText(QStringLiteral("Captured packets available: %1")
        .arg(static_cast<quint64>(captureCount)));
    const auto proxySnapshot = network_view::proxy_runtime_snapshot();
    proxyCountLabel_->setText(QStringLiteral("Proxy exchanges available: %1")
        .arg(static_cast<quint64>(proxySnapshot ? proxySnapshot->history.size() : 0)));
    exportButton_->setEnabled(!writing && !pathEdit_->text().isEmpty() && captureCount > 0);
    harButton_->setEnabled(!harWriting && proxySnapshot && !proxySnapshot->history.empty());
    harButton_->setText(harWriting ? "Exporting HAR..." : "Export Proxy as HAR");
    writingPill_->setVisible(writing);
}

void PcapExportPane::refreshProgress() {
    const bool writing = network_view::g_state.pcap_writing.load(std::memory_order_acquire);
    const bool harWriting = network_view::g_state.har_writing.load(std::memory_order_acquire);
    if (writing || harWriting) {
        refreshState();
        return;
    }
    std::string lastError;
    std::string lastPath;
    std::string harError;
    std::string harPath;
    {
        std::lock_guard<std::mutex> lock(network_view::g_state.pcap_error_mutex);
        lastError = network_view::g_state.pcap_last_error;
        lastPath = network_view::g_state.pcap_last_path;
    }
    {
        std::lock_guard<std::mutex> lock(network_view::g_state.har_status_mutex);
        harError = network_view::g_state.har_last_error;
        harPath = network_view::g_state.har_last_path;
    }
    const std::uint32_t written =
        network_view::g_state.pcap_written_count.load(std::memory_order_acquire);
    const std::uint32_t harWritten =
        network_view::g_state.har_written_count.load(std::memory_order_acquire);

    errorPill_->setVisible(!lastError.empty());
    if (!lastError.empty())
        errorPill_->setText(QString::fromStdString(lastError));
    const bool showDone = written > 0 && !lastPath.empty();
    donePill_->setVisible(showDone);
    if (showDone)
        donePill_->setText(QStringLiteral("Exported %1 packets to %2")
            .arg(written).arg(QString::fromStdString(lastPath)));

    harErrorPill_->setVisible(!harError.empty());
    if (!harError.empty())
        harErrorPill_->setText(QString::fromStdString(harError));
    const bool showHarDone = harWritten > 0 && !harPath.empty() && harError.empty();
    harDonePill_->setVisible(showHarDone);
    if (showHarDone)
        harDonePill_->setText(QStringLiteral("Exported %1 exchanges to %2")
            .arg(harWritten).arg(QString::fromStdString(harPath)));

    if (!writing && !harWriting && progressTimer_->isActive() && isVisible())
        refreshState();
}

}
