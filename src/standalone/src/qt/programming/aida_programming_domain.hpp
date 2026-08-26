#pragma once

namespace aida::qt::docking {
class AidaDockHost;
}
namespace aida::qt::bridge {
class ActionBridge;
class MenuBridge;
}

namespace aida::qt::programming {

// Composition entry point (mirrors debugger::install_debugger_domain).
// Called by the shell composition root (AidaChromeComposer::installDomainRegistrations)
// once the dock host and the W2.2 bridges exist. Installs the twelve view
// factories, constructs the controllers, installs the backend host hooks, and
// wires the host facade.
void install_programming_domain(docking::AidaDockHost* host,
                                bridge::MenuBridge* menus,
                                bridge::ActionBridge* actions);

}
