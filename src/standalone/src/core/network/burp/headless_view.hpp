#pragma once

#include <string>

namespace aida {
namespace burp {
namespace headless_view {

// Lifecycle surface of the Camoufox headless view. The implementation lives in
// the Qt port (qt/net/qt_headless_browser_view.cpp, QtHeadlessBrowserController
// with identical semantics and headless_v log tags); these free functions
// remain the call surface for burp_module.cpp and the Test Lab, which are
// outside the view-slice edit surface. The ImGui render entry point is gone.
bool initialize();
void shutdown();
std::string last_error();

}
}
}
