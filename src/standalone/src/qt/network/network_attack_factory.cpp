#include "qt/network/network_attack_factory.hpp"

#include "qt/network/collaborator/collaborator_view.hpp"
#include "qt/network/comparer/comparer_view.hpp"
#include "qt/network/decoder/decoder_pane.hpp"
#include "qt/network/fuzzer/fuzzer_pane.hpp"
#include "qt/network/intruder/intruder_view.hpp"
#include "qt/network/jwt_lab/jwt_lab_view.hpp"
#include "qt/network/match_replace/match_replace_view.hpp"
#include "qt/network/offensive/offensive_pane.hpp"
#include "qt/network/repeater/repeater_pane.hpp"
#include "qt/network/scripting/scripting_pane.hpp"
#include "qt/network/sequencer/sequencer_view.hpp"
#include "qt/network/session_handler/session_handler_view.hpp"
#include "qt/network/websocket/ws_pane.hpp"

namespace aida::qt::net {

QWidget* create_network_attack_pane(network_view::sub_tab_t tab, QWidget* parent) {
    using sub_tab_t = network_view::sub_tab_t;
    switch (tab) {
    case sub_tab_t::repeater:  return new RepeaterPane(parent);
    case sub_tab_t::fuzzer:    return new FuzzerPane(parent);
    case sub_tab_t::offensive: return new OffensivePane(parent);
    case sub_tab_t::websocket: return new WsPane(parent);
    case sub_tab_t::scripting: return new ScriptingPane(parent);
    case sub_tab_t::decoder:   return new DecoderPane(parent);
    case sub_tab_t::intruder:  return new IntruderView(parent);
    case sub_tab_t::collab:    return new CollaboratorView(parent);
    case sub_tab_t::sequencer: return new SequencerView(parent);
    case sub_tab_t::comparer:  return new ComparerView(parent);
    case sub_tab_t::jwt:       return new JwtLabView(parent);
    case sub_tab_t::mr:        return new MatchReplaceView(parent);
    case sub_tab_t::session:   return new SessionHandlerView(parent);
    default:
        return nullptr;
    }
}

}
