#pragma once

namespace aida::qt {
namespace bridge {
class ActionBridge;
class MenuBridge;
}
namespace docking {
class AidaDockHost;
}
}

namespace aida::qt::scanner {

void install_scanner_domain(docking::AidaDockHost* host, bridge::MenuBridge* menus,
	bridge::ActionBridge* actions);

}
