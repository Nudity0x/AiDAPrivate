#pragma once

namespace aida::qt::docking {
class AidaDockHost;
}
namespace aida::qt::bridge {
class ActionBridge;
class MenuBridge;
}

namespace aida::qt::debugger {

// Composition entry point (mirrors documents::install_document_domain /
// analysis::install_analysis_domain). Called by the shell composition root
// (AidaChromeComposer::installDomainRegistrations) once the dock host and the
// W2.2 bridges exist. Installs the 17 debugger view factories, constructs the
// session controller / action bridge / mutation queue, and wires the backend
// host-UI hooks (core/debugger/debugger_view.cpp stays Qt-free).
void install_debugger_domain(docking::AidaDockHost* host,
                             bridge::MenuBridge* menus,
                             bridge::ActionBridge* actions);

}
