#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace analysis_session { struct session_summary_t; }

namespace loading_binary_overlay {

enum class completion_action_t : unsigned int {
    none = 0,
    switch_to_disassembly = 1,
    switch_to_disassembly_or_hex = 2,
};

enum class phase_t : unsigned int {
    idle = 0,
    loading = 1,
    awaiting_analysis = 2,
    awaiting_pdb_decision = 3,
    loading_pdb = 4,
    finalizing = 5,
    complete = 6
};

namespace detail {

struct state_t {
    std::string session_id;
    std::string path;
    std::string filename;
    completion_action_t action = completion_action_t::none;
    std::atomic<bool> completion_applied{false};
    std::atomic<bool> cancellation_requested{false};
    std::atomic<float> visual_progress{0.f};
    std::atomic<float> visual_alpha{0.f};
    std::chrono::steady_clock::time_point tracked_at = std::chrono::steady_clock::now();
};

std::mutex& registry_mutex();
std::unordered_map<std::string, std::shared_ptr<state_t>>& registry();
std::string derive_filename(const std::string& path);
const char* phase_name(phase_t phase);
std::shared_ptr<state_t> selected_state();
float progress_for(const analysis_session::session_summary_t& summary);
std::string label_for(const analysis_session::session_summary_t& summary);

}

void set_view_focus_hook(std::function<void(const char* view_id)> hook);

void track_session(const std::string& session_id, const std::string& path,
                   completion_action_t action);
void release_session(const std::string& session_id);
phase_t current_phase();
const char* current_phase_name();
bool is_active();
bool is_waiting_for_user_decision();
void log_state(const char* reason);
bool cancel_queued_load(const char* reason);
bool is_load_ready_for_tools();
bool is_blocking_views();
void begin_load(const std::string& path,
                completion_action_t action = completion_action_t::switch_to_disassembly);
void poll_completion();

}
