#pragma once

#include "application_action_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aida::ui::programming_tasks {

struct operation_result_t {
    bool succeeded = false;
    std::string detail;
};

enum class configuration_kind_t : std::uint8_t { task, launch, test };
enum class configuration_origin_t : std::uint8_t { user, project };

struct configuration_t {
    std::string id;
    std::string source_id;
    std::string name;
    std::string command;
    std::string cwd;
    std::string output_channel;
    std::string problem_matcher;
    configuration_kind_t kind = configuration_kind_t::task;
    configuration_origin_t origin = configuration_origin_t::user;
};

struct resolved_configuration_t {
    configuration_t source;
    std::string command;
    std::string cwd;
    std::string channel;
};

struct configuration_draft_t {
    std::string name;
    std::string command;
    std::string cwd;
    std::string channel;
    int kind = 0;
    int matcher = 0;
};

struct catalog_snapshot_t {
    std::vector<configuration_t> configurations;
    std::string selected_id;
    std::vector<std::string> channels;
    std::string selected_channel;
    std::string project_root;
    std::string configuration_error;
    bool loading = false;
    bool editor_dirty = false;
    bool editor_save_in_flight = false;
    bool editor_creating = false;
    int editor_selected = -1;
    std::string editor_validation_error;
    std::uint64_t configuration_generation = 0;
    std::uint64_t catalog_fingerprint = 0;
    std::size_t active_run_count = 0;
    std::size_t problem_count = 0;
};

struct host_ui_hooks_t {
    std::function<void()> present_configuration_editor;
    std::function<void()> present_run_review;
    std::function<void(const char* view_id)> open_or_focus_view;
};

void install_host_ui_hooks(host_ui_hooks_t hooks);

void tick();
catalog_snapshot_t catalog_snapshot();

operation_result_t begin_edit(int index, configuration_draft_t& draft);
operation_result_t begin_create(configuration_draft_t& draft);
operation_result_t begin_duplicate(int index, configuration_draft_t& draft);
operation_result_t save_draft(const configuration_draft_t& draft);
void discard_draft();
operation_result_t revert_draft(configuration_draft_t& draft);
operation_result_t delete_selected_configuration();
void note_draft_edited();

operation_result_t select_configuration(int index, bool persist_selection);
int configuration_index(std::string_view id);
void set_selected_channel(const std::string& channel);

std::optional<resolved_configuration_t> pending_run_snapshot();
void clear_pending_run();
operation_result_t start_run(const resolved_configuration_t& configuration);
operation_result_t open_project_configuration_file();

std::uint64_t configuration_fingerprint(const configuration_t& configuration);
std::uint64_t configuration_catalog_fingerprint();
capability_state_t validate_configuration_identity(const configuration_t& retained,
    std::uint64_t generation, std::uint64_t catalog_fingerprint,
    const std::string& project_root);
enum class configuration_run_gate_t : std::uint8_t { run, delete_ };
std::string configuration_run_gate_reason(const configuration_t& retained,
                                          configuration_run_gate_t gate);
std::string redacted_command(std::string value);
std::string kind_name(configuration_kind_t value);

operation_result_t request_run_selected();
operation_result_t request_run_selected_for_file(const std::string& path, bool launch);
operation_result_t request_test_selected_for_file(const std::string& path);
operation_result_t request_cancel_active();
operation_result_t request_retry_last();
operation_result_t open_configurations();
operation_result_t reload_configurations();
bool has_active_run();
std::size_t problem_count();
std::string run_unavailable_reason();
std::string run_for_file_unavailable_reason(const std::string& path, bool launch);
std::string test_for_file_unavailable_reason(const std::string& path);
std::string cancel_unavailable_reason();
std::string retry_unavailable_reason();

}
