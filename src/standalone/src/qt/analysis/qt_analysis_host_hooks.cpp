#include "qt/analysis/qt_analysis_host_hooks.hpp"

namespace aida::qt::analysis {

analysis_host_hooks_t& analysis_host_hooks()
{
    static analysis_host_hooks_t value;
    return value;
}

void install_analysis_host_hooks(analysis_host_hooks_t hooks)
{
    analysis_host_hooks() = std::move(hooks);
}

bool analysis_host_hooks_installed() noexcept
{
    const analysis_host_hooks_t& hooks = analysis_host_hooks();
    return static_cast<bool>(hooks.stage_type_application) ||
        static_cast<bool>(hooks.open_source_reconstruction);
}

}
