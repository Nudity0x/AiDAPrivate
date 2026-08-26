#include "qt/network/shared/monitor_bridge.hpp"

#include <QMetaObject>
#include <Qt>

#include <utility>

namespace aida::qt::net {

NetworkMonitorBridge::NetworkMonitorBridge(QObject* parent)
    : QObject(parent) {
    registerInstance(this);
}

NetworkMonitorBridge::~NetworkMonitorBridge() {
    clearInstance(this);
    detachSinks();
}

void NetworkMonitorBridge::attachSinks() {
    network_view::set_connection_snapshot_sink(
        [this](std::shared_ptr<const std::vector<network_view::connection_entry_t>> snapshot) {
            QMetaObject::invokeMethod(this, [this, snapshot = std::move(snapshot)]() mutable {
                Q_EMIT connectionSnapshot(std::move(snapshot));
            }, Qt::QueuedConnection);
        });
    network_view::set_capture_batch_sink(
        [this](std::shared_ptr<const std::vector<network_view::packet_entry_t>> batch,
               std::size_t trimmed) {
            QMetaObject::invokeMethod(this,
                [this, batch = std::move(batch), trimmed]() mutable {
                    Q_EMIT captureBatch(std::move(batch),
                        static_cast<quint64>(trimmed));
                }, Qt::QueuedConnection);
        });
    network_view::set_dns_batch_sink(
        [this](std::shared_ptr<const std::vector<network_view::dns_entry_t>> batch,
               std::size_t trimmed) {
            QMetaObject::invokeMethod(this,
                [this, batch = std::move(batch), trimmed]() mutable {
                    Q_EMIT dnsBatch(std::move(batch), static_cast<quint64>(trimmed));
                }, Qt::QueuedConnection);
        });
    network_view::set_bandwidth_snapshot_sink(
        [this](std::shared_ptr<const std::vector<network_view::bw_entry_t>> snapshot) {
            QMetaObject::invokeMethod(this, [this, snapshot = std::move(snapshot)]() mutable {
                Q_EMIT bandwidthSnapshot(std::move(snapshot));
            }, Qt::QueuedConnection);
        });
    network_view::set_proxy_snapshot_sink(
        [this](std::shared_ptr<const network_view::proxy_runtime_snapshot_t> snapshot) {
            QMetaObject::invokeMethod(this, [this, snapshot = std::move(snapshot)]() mutable {
                Q_EMIT proxySnapshot(std::move(snapshot));
            }, Qt::QueuedConnection);
        });
    network_view::set_intercept_snapshot_sink(
        [this](std::shared_ptr<const network_view::intercept_runtime_snapshot_t> snapshot) {
            QMetaObject::invokeMethod(this, [this, snapshot = std::move(snapshot)]() mutable {
                Q_EMIT interceptSnapshot(std::move(snapshot));
            }, Qt::QueuedConnection);
        });
    network_view::set_keylog_snapshot_sink(
        [this](std::shared_ptr<const network_view::keylog_runtime_snapshot_t> snapshot) {
            QMetaObject::invokeMethod(this, [this, snapshot = std::move(snapshot)]() mutable {
                Q_EMIT keylogSnapshot(std::move(snapshot));
            }, Qt::QueuedConnection);
        });
    network_view::set_filters_changed_sink([this] {
        QMetaObject::invokeMethod(this, [this] {
            Q_EMIT filtersChanged();
        }, Qt::QueuedConnection);
    });
    network_view::set_cert_diagnostics_sink(
        [this](bool success, cert_intercept::process_diagnostics_t report,
               std::vector<cert_intercept::provider_status_t> providers, std::string status) {
            QMetaObject::invokeMethod(this,
                [this, success, report = std::move(report), providers = std::move(providers),
                 status = QString::fromStdString(status)]() mutable {
                    Q_EMIT certDiagnosticsResult(success, std::move(report),
                        std::move(providers), status);
                }, Qt::QueuedConnection);
        });
    network_view::set_cert_handoff_sink([this](bool success, std::string status) {
        QMetaObject::invokeMethod(this,
            [this, success, status = QString::fromStdString(status)]() mutable {
                Q_EMIT certHandoffResult(success, status);
            }, Qt::QueuedConnection);
    });
}

void NetworkMonitorBridge::detachSinks() {
    network_view::set_connection_snapshot_sink(nullptr);
    network_view::set_capture_batch_sink(nullptr);
    network_view::set_dns_batch_sink(nullptr);
    network_view::set_bandwidth_snapshot_sink(nullptr);
    network_view::set_proxy_snapshot_sink(nullptr);
    network_view::set_intercept_snapshot_sink(nullptr);
    network_view::set_keylog_snapshot_sink(nullptr);
    network_view::set_filters_changed_sink(nullptr);
    network_view::set_cert_diagnostics_sink(nullptr);
    network_view::set_cert_handoff_sink(nullptr);
}

}
