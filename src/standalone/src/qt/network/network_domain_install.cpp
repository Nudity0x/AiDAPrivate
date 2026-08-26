#include "qt/network/network_domain_install.hpp"

#include <QCoreApplication>
#include <QObject>

#include <string>

#include "core/network/network_view.hpp"
#include "helpers/diag_log.hpp"
#include "qt/net/net_editors_install.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/network/burp/cookie_jar_pane.hpp"
#include "qt/network/burp/logger_pane.hpp"
#include "qt/network/burp/project_pane.hpp"
#include "qt/network/burp/scope_pane.hpp"
#include "qt/network/burp/site_map_pane.hpp"
#include "qt/network/burp/upstream_pane.hpp"
#include "qt/network/monitor/bandwidth_pane.hpp"
#include "qt/network/monitor/capture_pane.hpp"
#include "qt/network/monitor/connections_pane.hpp"
#include "qt/network/monitor/dns_pane.hpp"
#include "qt/network/monitor/filters_pane.hpp"
#include "qt/network/monitor/intercept_pane.hpp"
#include "qt/network/monitor/keylog_pane.hpp"
#include "qt/network/monitor/pcap_export_pane.hpp"
#include "qt/network/monitor/proxy_pane.hpp"
#include "qt/network/network_hub_widget.hpp"
#include "qt/network/network_pane_factory.hpp"
#include "qt/network/shared/event_bus_bridge.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/network/shared/exchange_review_dialog.hpp"
#include "qt/network/shared/monitor_bridge.hpp"
#include "qt/registry/qt_view_registry.hpp"

namespace aida::qt::net {

namespace {

QObject* g_domainRoot = nullptr;

}

void install_network_domain(docking::AidaDockHost* host) {
    diag::log_tagged("network", "network_domain_install_begin");

    network_view::initialize();
    install_network_editors_domain();

    if (!g_domainRoot)
        g_domainRoot = new QObject();
    auto* monitorBridge = new NetworkMonitorBridge(g_domainRoot);
    monitorBridge->attachSinks();
    new NetworkEventBusBridge(g_domainRoot);
    auto* reviewHost = new ExchangeReviewHost(g_domainRoot);
    reviewHost->installHooks();
    exchange_context_host();
    InterceptPane::installDropReviewDisplay();

    register_network_hub_content_factory([](QWidget* parent) -> QWidget* {
        return new NetworkHubWidget(parent);
    });

    network_view::set_open_view_handler([host](const char* viewId) -> std::string {
        if (!host || !viewId || viewId[0] == '\0')
            return "The view host is unavailable";
        const auto result = host->open_or_focus(
            registry::stable_view_id_t(viewId));
        return result.ok() ? std::string() : result.detail;
    });
    network_view::set_clipboard_text_handler([](const std::string& text) {
        clipboard::set_text(QString::fromStdString(text));
    });
    network_view::set_save_file_dialog_handler(
        [](const char* title, const char* filterPairs, const char* defaultExtension,
           const std::string& initialName) -> std::string {
            const auto picked = dialogs::save_file(nullptr,
                QString::fromLatin1(title), filterPairs,
                QString::fromLatin1(defaultExtension ? defaultExtension : ""),
                QString::fromStdString(initialName));
            return picked ? *picked : std::string();
        });

    if (host) {
        const auto install = [host](network_view::sub_tab_t tab) {
            const char* id = network_view_id_for_tab(tab);
            const auto result = host->install_view_factory(
                registry::stable_view_id_t(id),
                [tab](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
                    return createNetworkPane(tab, parent);
                });
            if (!result.ok())
                diag::log_tagged_fmt("network",
                    "network_view_factory_install_failed view=%s status=%d detail=%s",
                    id, static_cast<int>(result.status), result.detail.c_str());
        };
        install(network_view::sub_tab_t::connections);
        install(network_view::sub_tab_t::capture);
        install(network_view::sub_tab_t::intercept);
        install(network_view::sub_tab_t::proxy);
        install(network_view::sub_tab_t::dns);
        install(network_view::sub_tab_t::filters);
        install(network_view::sub_tab_t::bandwidth);
        install(network_view::sub_tab_t::keylog);
        install(network_view::sub_tab_t::pcap_export);
        install(network_view::sub_tab_t::sitemap);
        install(network_view::sub_tab_t::scope);
        install(network_view::sub_tab_t::cookies);
        install(network_view::sub_tab_t::logger);
        install(network_view::sub_tab_t::upstream);

        const auto projectResult = host->install_view_factory(
            registry::stable_view_id_t("view.network.project"),
            [](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
                return new ProjectPane(parent);
            });
        if (!projectResult.ok())
            diag::log_tagged_fmt("network",
                "network_view_factory_install_failed view=view.network.project status=%d detail=%s",
                static_cast<int>(projectResult.status), projectResult.detail.c_str());
    }

    diag::log_tagged("network", "network_domain_install_complete");
}

void shutdown_network_domain() {
    diag::log_tagged("network", "network_domain_shutdown_begin");
    if (auto* bridge = NetworkMonitorBridge::instance())
        bridge->detachSinks();
    network_view::set_open_view_handler(nullptr);
    network_view::set_clipboard_text_handler(nullptr);
    network_view::set_save_file_dialog_handler(nullptr);
    network_view::set_exchange_context_display(nullptr);
    network_view::set_exchange_review_display(nullptr);
    network_view::set_exchange_remove_receipt_display(nullptr);
    network_view::set_intercept_drop_review_display(nullptr);
    network_view::shutdown();
    shutdown_network_editors_domain();
    delete g_domainRoot;
    g_domainRoot = nullptr;
    diag::log_tagged("network", "network_domain_shutdown_complete");
}

}
