#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace aida::analysis {
class analysis_workspace_t;
}

namespace aida::qt::analysis {

// Qt-free engine-facing hook surface for the analysis domain. The Qt bootstrap
// installs these hooks; engine-side call sites (application_ui_runtime.cpp,
// qt/disasm/aida_disasm_view.cpp, qt/graph/cfg_scene_controller.cpp,
// memory_scanner_view.cpp, debugger_view.cpp, safe_headless_runtime_bridges.cpp)
// call the hooks instead of the removed ImGui view namespaces. Every hook may
// be empty; callers treat empty as "the analysis UI is not available".

struct staged_dissector_target_t {
    std::uint64_t address = 0;
    std::uint32_t target_pid = 0;
    std::uint64_t target_epoch = 0;
    std::uint64_t process_creation_time_100ns = 0;
    std::uint64_t source_generation = 0;
    bool live_process = false;
    std::string source_view;
    std::string source_identity;
    std::function<bool(std::string&)> validate;
};

struct analysis_host_hooks_t {
    // view.types catalog: prefill the apply-type strip and focus Structures.
    // Returns false with `error` when the address is not mapped.
    std::function<bool(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
        std::uint64_t runtime_address, std::string& error)> stage_type_application;

    // Focus a types-hub subview (0 = structures .. 6 = dissector).
    std::function<void(int types_subview)> activate_types_subview;

    // Open the source-reconstruction dialog for the workspace.
    std::function<void(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace)>
        open_source_reconstruction;

    // Submit an xref query and focus the References view. `query_to` selects
    // XRefs To (true) vs XRefs From (false).
    std::function<bool(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
        std::uint64_t runtime_address, bool query_to, std::string& error)>
        submit_xref_query;

    // Proximity drill (analysis.proximity.drill): drills the selected node.
    // Returns true when a drill was executed.
    std::function<bool()> proximity_drill_selected;
    // Returns true when a drillable node is selected; otherwise fills reason.
    std::function<bool(std::string& reason)> proximity_drill_capability;

    // Reconstructed-structure actions (types.reconstruction.*).
    std::function<bool(std::string& detail)> recon_copy_declaration;
    std::function<bool()> recon_has_current_structure;
    std::function<bool(std::string& detail)> recon_declare_and_apply;

    // Scanner/debugger -> Structure Dissector staged-target handoff.
    std::function<bool(staged_dissector_target_t context, std::string& error)>
        stage_dissector_target;
};

void install_analysis_host_hooks(analysis_host_hooks_t hooks);
analysis_host_hooks_t& analysis_host_hooks();
bool analysis_host_hooks_installed() noexcept;

}
