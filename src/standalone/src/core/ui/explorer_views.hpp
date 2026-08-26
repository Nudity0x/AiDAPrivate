#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aida::ui::explorer_views {

enum class file_operation_t : std::uint8_t {
    new_file,
    new_folder,
    rename,
    cut,
    copy,
    paste,
    duplicate,
    remove,
    open_with,
    terminal_here
};

struct file_operation_capability_t {
    bool enabled = false;
    std::string reason;
};

struct file_operation_result_t {
    bool accepted = false;
    std::string detail;
};

struct file_operation_target_t {
    std::string path;
    bool directory = false;
};

void render_project_explorer();
void render_workspace_search();
bool can_restore_previous_session();
bool request_restore_previous_session();
file_operation_capability_t file_operation_capability(file_operation_t operation,
    const std::string& path, bool directory);
file_operation_result_t request_file_operation(file_operation_t operation,
    const std::string& path, bool directory);
file_operation_capability_t file_operation_capability(file_operation_t operation,
    const std::vector<file_operation_target_t>& targets);
file_operation_result_t request_file_operation(file_operation_t operation,
    const std::vector<file_operation_target_t>& targets);
file_operation_result_t submit_confirmed_file_operation(file_operation_t operation,
    std::filesystem::path source, std::filesystem::path destination, bool source_directory);
file_operation_result_t submit_confirmed_batch_file_operation(file_operation_t operation,
    std::vector<file_operation_target_t> targets);
file_operation_result_t request_search_scope(const std::string& path, bool directory);
void render_global_file_operation_dialogs();

}
