#pragma once

#include "core/network/network_view.hpp"

class QWidget;

namespace aida::qt::net {

// Factory seam for the Wave-4 network editors slice (plan 12): returns the Qt
// pane for the nine in-scope sub-tabs (scanner, recon, api, ws_edit, h2_edit,
// csp, browser, reports, headless) or nullptr for tabs owned by other slices.
// The network pane factory (qt/network/network_pane_factory.cpp) calls this
// from its default branch before falling back to nullptr.
QWidget* create_network_editor_pane(network_view::sub_tab_t tab, QWidget* parent = nullptr);

// Domain lifetime: creates the GUI-affinity headless controller (the Camoufox
// bridge-state mirror and worker target) ahead of first use so the lifecycle
// entry points (headless_view::initialize/shutdown from burp_module.cpp and
// the Test Lab) always have a delivery target. Called once from
// install_network_domain on the GUI thread. Shutdown detaches the instance.
void install_network_editors_domain();
void shutdown_network_editors_domain();

}
