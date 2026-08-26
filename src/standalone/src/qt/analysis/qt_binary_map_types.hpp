#pragma once

#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/analysis/binary_map.hpp"
#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/infra/event_bus.hpp"
#include "core/runtime/standalone_driver.hpp"

namespace aida::qt::analysis {

// Ported verbatim from binary_map_view.hpp (view-domain types).
enum class qt_binary_map_display_mode_t : int {
    auto_detect = 0,
    static_only,
    live_only
};

enum class qt_binary_map_active_mode_t : int {
    none = 0,
    pe_static,
    live_process,
    merged
};

struct qt_binary_map_live_region_t {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::uint32_t state = 0;
    std::uint32_t protect = 0;
    std::uint32_t type = 0;
    std::string module_name;
    std::string module_path;
    std::string section_name;
    std::string info;
    std::uint32_t owner_tid = 0;
    bool is_image = false;
    bool is_mapped = false;
    bool is_private = false;
    bool is_stack = false;
    bool is_heap = false;
    bool is_committed = false;
    bool is_reserved = false;
    bool is_guard = false;
    bool is_noaccess = false;
};

struct qt_binary_map_live_target_binding_t {
    aida::analysis::process_identity_t process;
    std::optional<aida::analysis::module_identity_t> module;
    std::uint64_t workspace_generation = 0;
    std::uint64_t refresh_serial = 0;

    bool valid() const noexcept {
        return process.pid != 0 && process.creation_time_100ns != 0 && module &&
            module->base != 0 && module->size != 0 && workspace_generation != 0 &&
            refresh_serial != 0;
    }
};

struct qt_binary_map_live_snapshot_t {
    std::vector<qt_binary_map_live_region_t> regions;
    std::vector<driver_bridge::module_info_t> modules;
    std::vector<driver_bridge::thread_info_t> threads;
    std::uint64_t process_heap = 0;
    std::uint64_t total_committed = 0;
    std::uint64_t total_reserved = 0;
    std::uint32_t rwx_count = 0;
    std::uint32_t pid = 0;
    std::string process_name;
    std::int64_t generated_unix = 0;
    std::uint64_t enum_elapsed_ms = 0;
    qt_binary_map_live_target_binding_t target_binding;
};

// Per-binary binary-map state (07 sec. 1.3/sec. 6.1); replaces workspace_states().
// GUI-thread owner except where atomics are annotated by the port.
struct QtBinaryMapViewState {
    QtBinaryMapViewState() = default;
    QtBinaryMapViewState(const QtBinaryMapViewState&) = delete;
    QtBinaryMapViewState& operator=(const QtBinaryMapViewState&) = delete;
    ~QtBinaryMapViewState();

    std::mutex mutex;
    std::shared_ptr<const aida::binary_map::map_t> map =
        std::make_shared<aida::binary_map::map_t>();
    // Defaults match the ImGui view's initialize() overrides
    // (binary_map_view.hpp:2750-2755), not the engine's smaller defaults.
    aida::binary_map::map_options_t opts = [] {
        aida::binary_map::map_options_t o;
        o.max_functions = 200;
        o.max_globals = 60;
        o.max_callees_per_function = 5;
        o.max_chars = 16384;
        o.include_imports = true;
        o.include_exports = true;
        return o;
    }();
    std::shared_ptr<const std::string> rendered_text =
        std::make_shared<std::string>();
    std::set<std::string> collapsed_groups;
    QString filter;
    std::string filter_lower;
    const qt_binary_map_live_snapshot_t* filtered_live_identity = nullptr;
    std::string filtered_live_query;
    std::vector<int> filtered_live_indices;
    std::string last_error;
    std::string live_last_error;
    std::atomic<bool> has_map{false};
    std::atomic<bool> refreshing{false};
    std::atomic<bool> refresh_requested{false};
    std::atomic<std::uint64_t> refresh_serial{0};
    std::atomic<bool> export_pending{false};
    std::atomic<std::uint64_t> selected_va{0};
    std::string selected_entity_id;
    float left_split = 0.58f;
    bool initialized = false;
    bool auto_refreshed_once = false;
    std::string last_binary_identity_path;
    std::uint64_t last_binary_identity_base = 0;
    std::uint32_t last_binary_identity_size = 0;
    std::uint64_t hover_function_va = 0;
    qt_binary_map_display_mode_t mode_pref =
        qt_binary_map_display_mode_t::auto_detect;
    std::atomic<int> active_mode_atomic{0};
    std::shared_ptr<const qt_binary_map_live_snapshot_t> live =
        std::make_shared<qt_binary_map_live_snapshot_t>();
    std::atomic<bool> live_refreshing{false};
    std::atomic<bool> live_refresh_requested{false};
    std::atomic<std::uint64_t> live_refresh_serial{0};
    std::atomic<std::int64_t> live_last_refresh_unix{0};
    std::atomic<std::uint64_t> live_selected_base{0};
    std::atomic<int> live_hover_index{-1};
    float canvas_zoom = 1.f;
    double canvas_offset_norm = 0.0;
    bool canvas_dragging = false;
    float canvas_drag_anchor = 0.f;
    double canvas_drag_offset_start = 0.0;
    bool change_protect_open = false;
    bool change_protect_popup_requested = false;
    bool refresh_after_pin_requested = false;
    std::atomic<bool> change_protect_pending{false};
    std::uint64_t change_protect_addr = 0;
    std::uint64_t change_protect_size = 0;
    int change_protect_choice = 0;
    std::uint32_t change_protect_old = 0;
    qt_binary_map_live_target_binding_t change_protect_binding;
    std::weak_ptr<aida::analysis::analysis_workspace_t> workspace;
    std::string binary_id;
    std::atomic<std::uint64_t> workspace_generation{0};

    // Event subscriptions (binary_map_view.hpp:1573-1660 port). Installed by
    // bm_install_event_subscriptions from the owning QtWorkspaceContext;
    // released by ~QtBinaryMapViewState (handles are not RAII).
    aida::events::subscription_handle_t sub_binary_loaded_;
    aida::events::subscription_handle_t sub_process_created_;
    aida::events::subscription_handle_t sub_process_exited_;
};

}
