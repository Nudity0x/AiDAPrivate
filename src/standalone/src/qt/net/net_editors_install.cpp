#include "qt/net/net_editors_install.hpp"

#include "qt/net/qt_api_view.hpp"
#include "qt/net/qt_browser_launcher_view.hpp"
#include "qt/net/qt_csp_view.hpp"
#include "qt/net/qt_h2_editor_view.hpp"
#include "qt/net/qt_headless_browser_view.hpp"
#include "qt/net/qt_recon_view.hpp"
#include "qt/net/qt_report_view.hpp"
#include "qt/net/qt_scanner_view.hpp"
#include "qt/net/qt_ws_editor_view.hpp"

namespace aida::qt::net {

namespace {

QtHeadlessBrowserController* g_headless_controller = nullptr;

}

QWidget* create_network_editor_pane(network_view::sub_tab_t tab, QWidget* parent)
{
    using sub_tab_t = network_view::sub_tab_t;
    switch (tab) {
    case sub_tab_t::scanner:  return new QtScannerView(parent);
    case sub_tab_t::recon:    return new QtReconView(parent);
    case sub_tab_t::api:      return new QtApiView(parent);
    case sub_tab_t::ws_edit:  return new QtWsEditorView(parent);
    case sub_tab_t::h2_edit:  return new QtH2EditorView(parent);
    case sub_tab_t::csp:      return new QtCspView(parent);
    case sub_tab_t::browser:  return new QtBrowserLauncherView(parent);
    case sub_tab_t::reports:  return new QtReportView(parent);
    case sub_tab_t::headless: return new QtHeadlessBrowserView(parent);
    default:
        return nullptr;
    }
}

void install_network_editors_domain()
{
    if (g_headless_controller == nullptr) {
        g_headless_controller = new QtHeadlessBrowserController();
        QtHeadlessBrowserController::registerInstance(g_headless_controller);
        g_headless_controller->scheduleStatusPoll();
        g_headless_controller->scheduleInstallProbe(false);
    }
}

void shutdown_network_editors_domain()
{
    if (g_headless_controller != nullptr) {
        QtHeadlessBrowserController::clearInstance(g_headless_controller);
        delete g_headless_controller;
        g_headless_controller = nullptr;
    }
}

}
