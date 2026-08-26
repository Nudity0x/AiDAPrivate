#pragma once

#include <cstddef>
#include <string>
#include <vector>

enum class bottom_tab_t : int;

namespace aida::qt::docking {
class AidaDockHost;
}

// Qt-free facade surface consumed by engine-side call sites
// (application_ui_runtime.cpp action handlers, explorer_views.cpp file
// operations) replacing the retired core/ui/output_views.hpp +
// programming_language_views.hpp free functions. Implemented by the Qt
// programming domain; every function is GUI-thread and safe to call before
// the domain installs (capability queries then return conservative defaults,
// operations fail with a stable reason).
namespace aida::qt::programming::host {

void install(docking::AidaDockHost* host);

struct operation_result_t {
    bool succeeded = false;
    std::string detail;
};

operation_result_t copy_all(bottom_tab_t tab);
operation_result_t clear(bottom_tab_t tab);
operation_result_t select_all(bottom_tab_t tab);
operation_result_t toggle_follow(bottom_tab_t tab);
operation_result_t focus_filter(bottom_tab_t tab);
operation_result_t export_all(bottom_tab_t tab);
operation_result_t terminal_new();
operation_result_t terminal_new_at(const std::string& working_directory);
operation_result_t terminal_close();
operation_result_t terminal_restart();
operation_result_t terminal_next();
operation_result_t terminal_previous();
operation_result_t terminal_split_vertical();
operation_result_t terminal_split_horizontal();
operation_result_t terminal_unsplit();
operation_result_t terminal_focus_search();
operation_result_t terminal_paste();
bool has_content(bottom_tab_t tab);
bool supports_filter(bottom_tab_t tab) noexcept;
bool follows_tail(bottom_tab_t tab);
bool source_available(bottom_tab_t tab) noexcept;
std::size_t terminal_session_count() noexcept;
bool terminal_is_split() noexcept;

void open_rename_dialog();
void shutdown_terminal();
bool open_or_focus_view(const char* view_id);

void set_workspace_search_scope(const std::string& path);
std::vector<std::string> workspace_search_scope();
void clear_workspace_search_scope();

}
