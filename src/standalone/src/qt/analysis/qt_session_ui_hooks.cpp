#include "qt/analysis/qt_session_ui_hooks.hpp"

namespace aida::session_ui_hooks {

hooks_t& current_hooks()
{
    static hooks_t value;
    return value;
}

void install_hooks(hooks_t hooks)
{
    current_hooks() = std::move(hooks);
}

bool hooks_installed() noexcept
{
    const hooks_t& hooks = current_hooks();
    return static_cast<bool>(hooks.attach_workbench_workspace) ||
        static_cast<bool>(hooks.close_workbench_workspace) ||
        static_cast<bool>(hooks.track_loading_session) ||
        static_cast<bool>(hooks.release_loading_session);
}

}
