#pragma once

#include "core/network/network_view.hpp"

class QWidget;

namespace aida::qt::net {

// Aggregate factory for the plan-11 Burp attack/tool panes (intruder,
// repeater, sequencer, comparer, jwt_lab, match_replace, session_handler,
// collaborator, websocket capture, decoder, scripting, network fuzzer,
// offensive). Returns nullptr for tabs outside that set so the network pane
// factory's switch can chain: monitor cases, then create_network_editor_pane
// (the editors wave), then this. The composition wires the call into
// createNetworkPane.
QWidget* create_network_attack_pane(network_view::sub_tab_t tab, QWidget* parent = nullptr);

}
