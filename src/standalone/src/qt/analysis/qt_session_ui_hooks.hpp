#pragma once

#include <functional>
#include <memory>
#include <string>

namespace aida::analysis {
class analysis_workspace_t;
}

// Qt-free engine-facing hook surface. The Qt bootstrap installs these hooks;
// engine-side call sites (core/session/analysis_session.cpp) invoke them. Every
// hook may be empty; callers treat empty as "the session UI is not available".
namespace aida::session_ui_hooks {

struct workbench_hook_result_t {
    bool ok = true;
    unsigned int code = 0;
    unsigned long long subject = 0;
};

struct hooks_t {
    std::function<void(const std::string& session_id, const std::string& path)>
        track_loading_session;
    std::function<void(const std::string& session_id)> release_loading_session;
    std::function<workbench_hook_result_t(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace)>
        attach_workbench_workspace;
    std::function<workbench_hook_result_t(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace)>
        close_workbench_workspace;
};

void install_hooks(hooks_t hooks);
hooks_t& current_hooks();
bool hooks_installed() noexcept;

}
