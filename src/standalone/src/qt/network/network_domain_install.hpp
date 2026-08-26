#pragma once

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::net {

// Installs the network domain: boots the network runtime backend
// (network_view::initialize, idempotent), creates the monitor/event bridges
// and review hosts with GUI lifetime, wires the backend display hooks
// (open_view / clipboard / save-file dialog / exchange context / review
// dialogs / intercept drop review), and registers the in-scope view factories
// (view.network.* hub members + view.network.project) into the dock host.
// Called once from the Qt boot composition after the dock host exists.
void install_network_domain(docking::AidaDockHost* host);

// Cooperative domain shutdown: stops the monitor workers via
// network_view::shutdown() and detaches the sinks (called from the ordered
// shutdown before the Qt object tree dies).
void shutdown_network_domain();

}
