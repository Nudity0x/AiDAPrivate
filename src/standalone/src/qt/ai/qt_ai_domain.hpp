#pragma once

#include <string>

namespace aida::qt {
class AidaEventBusBridge;
namespace bridge {
class ActionBridge;
class MenuBridge;
class InteractionContextProvider;
}
namespace docking { class AidaDockHost; }
}

namespace aida::qt::ai {

class AidaChatController;

struct ai_domain_t {
    docking::AidaDockHost* host = nullptr;
    bridge::ActionBridge* actions = nullptr;
    bridge::MenuBridge* menus = nullptr;
    bridge::InteractionContextProvider* context = nullptr;
    AidaEventBusBridge* events = nullptr;
    AidaChatController* controller = nullptr;
};

ai_domain_t& ai_domain();

void open_ai_view(const std::string& view_id);

}
