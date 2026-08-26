#pragma once

#include <QObject>
#include <QString>

#include <cstddef>
#include <memory>
#include <vector>

#include "core/network/intercept/diagnostics.hpp"
#include "core/network/intercept/instrumentation_provider.hpp"
#include "core/network/network_view.hpp"

namespace aida::qt::net {

// NetworkMonitorBridge is the long-lived GUI-affinity QObject that owns the
// worker->GUI delivery for the network monitor streams. The backend sinks in
// network_view.cpp fire on worker threads; the bridge's sink lambdas repost
// the immutable payloads onto the GUI thread with queued invokeMethod functors
// (qmetaobject.cpp:1642-1657) and emit the Qt signals there. Receivers are
// GUI-thread widgets; signal delivery is same-thread direct, and Qt drops the
// connection automatically when a receiver is destroyed. The bridge is created
// by install_network_domain and lives until the network domain shuts down,
// which happens after the workers stop (network_view::shutdown precedes Qt
// object teardown in the ordered shutdown).
class NetworkMonitorBridge : public QObject {
    Q_OBJECT
public:
    explicit NetworkMonitorBridge(QObject* parent = nullptr);
    ~NetworkMonitorBridge() override;

    NetworkMonitorBridge(const NetworkMonitorBridge&) = delete;
    NetworkMonitorBridge& operator=(const NetworkMonitorBridge&) = delete;

    void attachSinks();
    void detachSinks();

    static NetworkMonitorBridge* instance() noexcept { return instance_; }
    static void registerInstance(NetworkMonitorBridge* bridge) noexcept { instance_ = bridge; }
    static void clearInstance(NetworkMonitorBridge* bridge) noexcept {
        if (instance_ == bridge)
            instance_ = nullptr;
    }

Q_SIGNALS:
    void connectionSnapshot(
        std::shared_ptr<const std::vector<network_view::connection_entry_t>> snapshot);
    void captureBatch(
        std::shared_ptr<const std::vector<network_view::packet_entry_t>> batch,
        quint64 trimmedFromFront);
    void captureCleared();
    void dnsBatch(
        std::shared_ptr<const std::vector<network_view::dns_entry_t>> batch,
        quint64 trimmedFromFront);
    void bandwidthSnapshot(
        std::shared_ptr<const std::vector<network_view::bw_entry_t>> snapshot);
    void proxySnapshot(
        std::shared_ptr<const network_view::proxy_runtime_snapshot_t> snapshot);
    void interceptSnapshot(
        std::shared_ptr<const network_view::intercept_runtime_snapshot_t> snapshot);
    void keylogSnapshot(
        std::shared_ptr<const network_view::keylog_runtime_snapshot_t> snapshot);
    void filtersChanged();
    void certDiagnosticsResult(bool success,
        cert_intercept::process_diagnostics_t report,
        std::vector<cert_intercept::provider_status_t> providers, QString status);
    void certHandoffResult(bool success, QString status);

private:
    static inline NetworkMonitorBridge* instance_ = nullptr;
};

}
