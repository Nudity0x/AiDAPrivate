#pragma once

#include "qt/layout/layout_container.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aida::qt::layout {

enum class operation_kind_t : std::uint8_t {
    set_lock,
    switch_preset,
    save_user,
    load_user,
    rename_user,
    delete_user,
    restore_preset,
    reset_all
};

struct operation_request_t {
    operation_kind_t kind = operation_kind_t::switch_preset;
    std::uint64_t serial = 0;
    std::uint64_t source_generation = 0;
    docking::workspace_preset_t current_preset = docking::workspace_preset_t::analysis;
    docking::workspace_preset_t target_preset = docking::workspace_preset_t::analysis;
    bool target_locked = false;
    std::uint64_t save_generation = 0;
    std::uint64_t expected_user_generation = 0;
    std::uint64_t catalog_epoch = 0;
    bool skip_backup = false;
    layout_paths_t current_paths;
    layout_paths_t target_paths;
    layout_paths_t source_user_paths;
    layout_paths_t fallback_paths;
    std::filesystem::path active_record;
    std::filesystem::path workspace_directory;
    std::filesystem::path user_directory;
    std::string current_user_name;
    std::string source_user_name;
    std::string target_user_name;
    std::string registry_fingerprint;
    bool overwrite = false;
    std::vector<layout_paths_t> reset_paths;
    std::shared_ptr<const container_payloads_t> current_payload;
    layout_environment_t environment;
    std::string task_id;
};

struct operation_result_t {
    operation_kind_t kind = operation_kind_t::switch_preset;
    std::uint64_t serial = 0;
    std::uint64_t source_generation = 0;
    docking::workspace_preset_t target_preset = docking::workspace_preset_t::analysis;
    bool target_locked = false;
    bool success = false;
    bool use_default = false;
    bool apply_layout = false;
    std::uint64_t saved_generation = 0;
    record_metadata_t metadata;
    container_payloads_t payloads;
    std::string active_user_name;
    std::string source_user_name;
    std::string target_user_name;
    std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>> catalog;
    std::uint64_t catalog_epoch = 0;
    std::string error;
    std::string task_id;
};

enum class operation_task_state_t : std::uint8_t {
    running,
    completed,
    failed,
    cancelled
};

struct operation_task_registration_t {
    std::string id;
    std::string source;
    std::string owner;
    std::string owner_view;
    std::string owner_action;
    std::string target;
    std::string label;
    std::string stage;
    std::string affected_entity;
    std::function<bool()> retry;
};

struct operation_task_hooks_t {
    std::function<bool(operation_task_registration_t registration)> register_task;
    std::function<void(const std::string& task_id, operation_task_state_t state,
                       float progress, const std::string& stage,
                       const std::string& result_summary)> update_task;
};

struct operation_runtime_t {
    std::atomic<bool> pending{false};
    std::atomic<std::uint64_t> serial{0};
    std::mutex result_mutex;
    std::shared_ptr<operation_result_t> result;
    std::shared_ptr<const operation_request_t> active_request;
    std::shared_ptr<const operation_request_t> last_failed_request;
    std::atomic<std::uint64_t> retry_requested{0};
};

operation_runtime_t& operation_runtime() noexcept;

void set_operation_task_hooks(operation_task_hooks_t hooks);
void set_operation_completion_sink(
    std::function<void(std::shared_ptr<operation_result_t> result)> sink);

void execute_operation(const operation_request_t& request, operation_result_t& result) noexcept;
docking::workspace_request_result_t submit_operation(operation_request_t request) noexcept;
const std::string& last_submit_error() noexcept;
std::shared_ptr<operation_result_t> take_operation_result() noexcept;
void process_operation_retry() noexcept;

std::uint64_t next_catalog_epoch() noexcept;
void publish_catalog(
    std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>> catalog,
    std::uint64_t epoch) noexcept;
std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>> catalog_snapshot() noexcept;
bool catalog_ready() noexcept;
void reset_catalog() noexcept;

bool queue_layout_write(const layout_paths_t& paths, docking::workspace_preset_t preset,
                        bool locked, std::uint64_t generation, bool skip_backup,
                        const layout_environment_t& environment,
                        std::string_view registry_fingerprint,
                        std::string_view payload, std::string_view surface);

}
