#include "explorer_views.hpp"

#include "task_center.hpp"
#include "toast_notification.hpp"
#include "ui_thread_dispatcher.hpp"
#include "../../helpers/globals.h"
#include "../session/analysis_session.hpp"
#include "../settings/standalone_settings.hpp"
#include "../infra/executor.hpp"
#include "qt/programming/programming_host_hooks.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

namespace aida::ui::explorer_views {
namespace {

std::string path_key(const std::string& path);


std::filesystem::path path_from_utf8(const std::string& value) {
#if defined(__cpp_char8_t)
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
#else
    return std::filesystem::u8path(value);
#endif
}

std::string path_to_utf8(const std::filesystem::path& value) {
    const auto encoded = value.generic_u8string();
#if defined(__cpp_char8_t)
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
    return encoded;
#endif
}

struct file_operation_state_t {
    file_operation_t operation = file_operation_t::new_file;
    std::string source;
    std::string clipboard_path;
    std::vector<file_operation_target_t> sources;
    std::vector<file_operation_target_t> clipboard_targets;
    bool source_directory = false;
    bool clipboard_directory = false;
    bool clipboard_cut = false;
    bool name_dialog_open = false;
    bool delete_dialog_open = false;
    bool name_dialog_requested = false;
    bool delete_dialog_requested = false;
    bool operation_pending = false;
    std::uint64_t generation = 0;
    std::uint64_t task_id = 0;
    std::shared_ptr<std::atomic<bool>> dispatch_failed;
    char name[260] = {};
    std::string validation_error;
    std::string operation_error;
};

file_operation_state_t& file_operations() {
    static file_operation_state_t value;
    return value;
}

void drain_dispatch_failure(file_operation_state_t& state) {
    if (!state.dispatch_failed ||
        !state.dispatch_failed->exchange(false, std::memory_order_acq_rel))
        return;
    state.operation_pending = false;
    state.task_id = 0;
    state.dispatch_failed.reset();
    state.operation_error = "The filesystem worker completed, but its result could not return to the UI owner";
    toast_notification::push(
        "Project Explorer result publication failed: " + state.operation_error,
        toast_notification::toast_type_t::error, 6.0f);
}

bool path_inside_roots(const std::filesystem::path& candidate,
    const std::vector<std::string>& roots, bool allow_root) {
    const std::string key = path_key(path_to_utf8(candidate.lexically_normal()));
    if (key.empty())
        return false;
    for (const auto& root_value : roots) {
        const std::string root = path_key(root_value);
        if (root.empty())
            continue;
        if (key == root)
            return allow_root;
        if (key.size() > root.size() && key.compare(0, root.size(), root) == 0 &&
            (key[root.size()] == '/' || key[root.size()] == '\\'))
            return true;
    }
    return false;
}

bool path_inside_root(const std::filesystem::path& candidate, bool allow_root) {
    return path_inside_roots(candidate, file_browser::roots, allow_root);
}

}

namespace {

const char* operation_label(file_operation_t operation) {
    switch (operation) {
    case file_operation_t::new_file: return "Create file";
    case file_operation_t::new_folder: return "Create folder";
    case file_operation_t::rename: return "Rename item";
    case file_operation_t::cut: return "Cut item";
    case file_operation_t::copy: return "Copy item";
    case file_operation_t::paste: return "Paste item";
    case file_operation_t::duplicate: return "Duplicate item";
    case file_operation_t::remove: return "Delete item";
    case file_operation_t::open_with: return "Open with";
    case file_operation_t::terminal_here: return "Open terminal here";
    }
    return "Workspace operation";
}

}

bool bounded_copy_tree(const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::shared_ptr<std::atomic<bool>>& cancelled, std::string& detail,
    std::uint64_t& total_bytes, std::size_t& entry_count) {
    namespace fs = std::filesystem;
    constexpr std::uint64_t maximum_bytes = 1024ULL * 1024ULL * 1024ULL;
    constexpr std::size_t maximum_entries = 100000;
    std::error_code error;
    if (fs::exists(destination, error) || error) {
        detail = error ? error.message() : "The destination already exists";
        return false;
    }
    if (fs::is_regular_file(source, error)) {
        if (error) {
            detail = error.message();
            return false;
        }
        const auto size = fs::file_size(source, error);
        if (error || size > maximum_bytes - total_bytes || entry_count >= maximum_entries) {
            detail = error ? error.message() : "The file exceeds the 1 GiB duplicate limit";
            return false;
        }
        if (!fs::copy_file(source, destination, fs::copy_options::none, error)) {
            detail = error ? error.message() : "The file copy did not complete";
            return false;
        }
        total_bytes += size;
        ++entry_count;
        return true;
    }
    if (!fs::is_directory(source, error) || error) {
        detail = error ? error.message() : "The source is not a regular file or directory";
        return false;
    }
    fs::create_directory(destination, error);
    if (error) {
        detail = error.message();
        return false;
    }
    if (++entry_count > maximum_entries) {
        detail = "The selection exceeds the 100,000-entry copy limit";
        std::error_code cleanup_error;
        fs::remove_all(destination, cleanup_error);
        return false;
    }
    for (fs::recursive_directory_iterator iterator(source,
            fs::directory_options::skip_permission_denied, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (cancelled->load(std::memory_order_acquire)) {
            detail = "The copy was cancelled";
            error = std::make_error_code(std::errc::operation_canceled);
            break;
        }
        if (++entry_count > maximum_entries) {
            detail = "The directory exceeds the 100,000-entry duplicate limit";
            error = std::make_error_code(std::errc::file_too_large);
            break;
        }
        const fs::path relative = iterator->path().lexically_relative(source);
        const fs::path target = destination / relative;
        if (iterator->is_symlink(error) || iterator->is_other(error)) {
            detail = "Workspace copies reject symbolic links and special filesystem entries";
            error = std::make_error_code(std::errc::operation_not_supported);
            break;
        }
        if (iterator->is_directory(error)) {
            fs::create_directory(target, error);
        } else if (iterator->is_regular_file(error)) {
            const auto size = iterator->file_size(error);
            if (!error && (size > maximum_bytes - total_bytes)) {
                detail = "The directory exceeds the 1 GiB duplicate limit";
                error = std::make_error_code(std::errc::file_too_large);
                break;
            }
            if (!error) {
                total_bytes += size;
                fs::copy_file(iterator->path(), target, fs::copy_options::none, error);
            }
        }
    }
    if (error) {
        if (detail.empty())
            detail = error.message();
        std::error_code cleanup_error;
        fs::remove_all(destination, cleanup_error);
        return false;
    }
    return true;
}

file_operation_result_t submit_file_operation(file_operation_t operation,
    std::filesystem::path source, std::filesystem::path destination,
    bool source_directory) {
    auto& state = file_operations();
    drain_dispatch_failure(state);
    if (state.operation_pending)
        return {false, "Another Project Explorer filesystem operation is already running"};
    const bool source_may_be_root = operation == file_operation_t::new_file ||
        operation == file_operation_t::new_folder;
    if (!path_inside_root(source, source_may_be_root))
        return {false, "The source is outside the open Project Explorer roots"};
    if (!destination.empty() && !path_inside_root(destination, false))
        return {false, "The destination is outside the open Project Explorer roots"};
    state.operation_pending = true;
    state.operation_error.clear();
    const std::uint64_t generation = ++state.generation;
    const std::vector<std::string> roots = file_browser::roots;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto dispatch_failed = std::make_shared<std::atomic<bool>>(false);
    state.dispatch_failed = dispatch_failed;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "project_explorer";
    submission.label = "project_explorer.file_operation";
    submission.thread_class = "bounded_filesystem_io";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 3;
    submission.generation = generation;
    submission.cancel_hook = [cancelled] {
        cancelled->store(true, std::memory_order_release);
    };
    submission.body = [operation, source = std::move(source), destination = std::move(destination),
            roots, source_directory, cancelled, dispatch_failed, generation]() mutable {
        bool succeeded = false;
        std::string detail;
        namespace fs = std::filesystem;
        try {
            std::error_code error;
            const fs::file_status source_status = fs::symlink_status(source, error);
            fs::path canonical_source;
            if (error || fs::is_symlink(source_status) || fs::is_other(source_status))
                detail = error ? error.message() :
                    "Workspace mutations reject symbolic links and special filesystem entries";
            else
                canonical_source = fs::weakly_canonical(source, error);
            if (detail.empty() && (error || !path_inside_roots(canonical_source, roots,
                    operation == file_operation_t::new_file ||
                    operation == file_operation_t::new_folder)))
                detail = error ? error.message() : "The resolved source escaped the workspace root";
            else if (detail.empty() && !destination.empty()) {
                error.clear();
                const fs::path canonical_parent = fs::weakly_canonical(destination.parent_path(), error);
                if (error || !path_inside_roots(canonical_parent, roots, true)) {
                    detail = error ? error.message() : "The resolved destination escaped the workspace root";
                } else {
                    destination = canonical_parent / destination.filename();
                    if (fs::exists(destination, error) || error)
                        detail = error ? error.message() : "The destination already exists";
                    else if (source_directory) {
                        const std::string source_key = path_key(path_to_utf8(canonical_source));
                        const std::string destination_key = path_key(path_to_utf8(destination));
                        if (destination_key.size() > source_key.size() &&
                            destination_key.compare(0, source_key.size(), source_key) == 0 &&
                            (destination_key[source_key.size()] == '/' ||
                             destination_key[source_key.size()] == '\\'))
                            detail = "A folder cannot be copied or moved inside itself";
                    }
                }
            }
            if (!detail.empty()) {
            }
            else if (operation == file_operation_t::new_file) {
                const HANDLE file = CreateFileW(destination.c_str(), GENERIC_WRITE,
                    0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                succeeded = file != INVALID_HANDLE_VALUE;
                if (succeeded)
                    CloseHandle(file);
                detail = succeeded ? "File created" :
                    "The new file could not be created without replacing an existing item";
            } else if (operation == file_operation_t::new_folder) {
                succeeded = fs::create_directory(destination, error);
                detail = succeeded ? "Folder created" : (error ? error.message() : "The folder already exists");
            } else if (operation == file_operation_t::rename) {
                fs::rename(canonical_source, destination, error);
                succeeded = !error;
                detail = succeeded ? "Item renamed" : error.message();
            } else if (operation == file_operation_t::duplicate || operation == file_operation_t::copy) {
                std::uint64_t copied_bytes = 0;
                std::size_t copied_entries = 0;
                succeeded = bounded_copy_tree(canonical_source, destination, cancelled, detail,
                    copied_bytes, copied_entries);
                if (succeeded) detail = "Item copied";
            } else if (operation == file_operation_t::cut) {
                fs::rename(canonical_source, destination, error);
                succeeded = !error;
                detail = succeeded ? "Item moved" :
                    std::string("Move failed; cross-volume cut is not performed implicitly: ") + error.message();
            } else if (operation == file_operation_t::remove) {
                const auto removed = fs::remove_all(canonical_source, error);
                succeeded = !error && removed > 0;
                detail = succeeded ? "Item deleted" : (error ? error.message() : "The item no longer exists");
            } else if (operation == file_operation_t::open_with) {
                const auto launched = reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr,
                    L"openas", canonical_source.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                succeeded = launched > 32;
                detail = succeeded ? "Open With launched" : "Windows could not open the Open With chooser";
            }
        } catch (const std::exception& exception) {
            detail = exception.what();
        } catch (...) {
            detail = "The filesystem operation failed with a non-standard exception";
        }
        aida::ui_thread::post_options_t options;
        options.subsystem = "project_explorer";
        options.label = "file_operation_result";
        options.phase = "worker_result";
        options.owner = "project_explorer.file_operation";
        options.priority = aida::ui_thread::priority_t::normal;
        const auto posted = aida::ui_thread::post([generation, succeeded,
                detail = std::move(detail)]() mutable {
            auto& current = file_operations();
            if (current.generation != generation)
                return;
            current.operation_pending = false;
            current.task_id = 0;
            current.dispatch_failed.reset();
            current.operation_error = succeeded ? std::string{} : detail;
            const bool clears_selection = succeeded &&
                (current.operation == file_operation_t::remove ||
                 current.operation == file_operation_t::rename ||
                 current.clipboard_cut);
            if (succeeded && current.clipboard_cut &&
                path_key(current.clipboard_path) == path_key(current.source)) {
                current.clipboard_path.clear();
                current.clipboard_cut = false;
                current.clipboard_directory = false;
            }
            if (clears_selection) {
                file_browser::selected_paths.clear();
                file_browser::selection_anchor_path.clear();
                file_browser::selected_idx = -1;
                ++file_browser::selection_revision;
            }
            file_browser::needs_refresh = true;
            if (!succeeded) {
                toast_notification::push(
                    "Project Explorer filesystem operation failed: " + current.operation_error,
                    toast_notification::toast_type_t::error, 6.0f);
            }
        }, std::move(options));
        if (posted != aida::ui_thread::enqueue_result_t::accepted)
            dispatch_failed->store(true, std::memory_order_release);
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        state.operation_pending = false;
        state.operation_error = "The filesystem worker could not be scheduled: " + submitted.reject_reason;
        return {false, state.operation_error};
    }
    state.task_id = submitted.task_id;
    task_center::task_registration_t registration;
    registration.id = "project-explorer-file-operation-" + std::to_string(generation);
    registration.source = "Project Explorer";
    registration.owner = "project_explorer";
    registration.owner_view = "view.project_explorer";
    registration.owner_action = operation_label(operation);
    registration.target = state.source;
    registration.label = operation_label(operation);
    registration.stage = "Queued";
    registration.cancellation_is_safe = operation == file_operation_t::copy ||
        operation == file_operation_t::duplicate;
    if (registration.cancellation_is_safe)
        registration.callbacks.cancel = [task = submitted.task_id] {
            return aida::infra::executor::cancel(task);
        };
    if (!task_center::register_executor_job(submitted.task_id,
            std::move(registration))) {
        toast_notification::push(
            "Project Explorer work could not register in Background Tasks: "
            "the bounded filesystem worker is running, but Task Center rejected its registration",
            toast_notification::toast_type_t::error, 6.0f);
    }
    return {true, "The filesystem operation was queued"};
}

file_operation_result_t submit_batch_file_operation(file_operation_t operation,
    std::vector<file_operation_target_t> targets, const std::filesystem::path& destination_directory = {}) {
    auto& state = file_operations();
    drain_dispatch_failure(state);
    if (state.operation_pending)
        return {false, "Another Project Explorer filesystem operation is already running"};
    if (targets.empty()) return {false, "Select at least one Project Explorer item"};
    if (targets.size() > 100000)
        return {false, "Project Explorer batch operations are limited to 100,000 selected items"};
    const std::vector<std::string> roots = file_browser::roots;
    for (const auto& target : targets) {
        if (!path_inside_root(path_from_utf8(target.path), false))
            return {false, "Every selected item must remain inside an open Project Explorer root"};
    }
    if (!destination_directory.empty() && !path_inside_root(destination_directory, true))
        return {false, "The paste destination is outside the open Project Explorer roots"};
    state.operation_pending = true;
    state.operation_error.clear();
    state.sources = targets;
    const std::uint64_t generation = ++state.generation;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto dispatch_failed = std::make_shared<std::atomic<bool>>(false);
    state.dispatch_failed = dispatch_failed;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "project_explorer";
    submission.label = "project_explorer.batch_file_operation";
    submission.thread_class = "bounded_filesystem_io";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 3;
    submission.generation = generation;
    submission.cancel_hook = [cancelled] { cancelled->store(true, std::memory_order_release); };
    submission.body = [operation, targets = std::move(targets), destination_directory,
            roots, cancelled, dispatch_failed, generation]() mutable {
        bool succeeded = false;
        bool partial = false;
        std::string detail;
        namespace fs = std::filesystem;
        std::vector<std::pair<fs::path, fs::path>> completed;
        std::size_t permanently_completed = 0;
        std::uint64_t total_bytes = 0;
        std::size_t entry_count = 0;
        const auto recover_completed = [&]() {
            bool proven = true;
            for (auto iterator = completed.rbegin(); iterator != completed.rend(); ++iterator) {
                std::error_code recovery_error;
                if (operation == file_operation_t::copy)
                    fs::remove_all(iterator->second, recovery_error);
                else if (operation == file_operation_t::cut)
                    fs::rename(iterator->second, iterator->first, recovery_error);
                if (recovery_error) proven = false;
            }
            return proven;
        };
        try {
            completed.reserve(targets.size());
            for (const auto& target : targets) {
                if (cancelled->load(std::memory_order_acquire)) {
                    detail = "The batch filesystem operation was cancelled";
                    break;
                }
                std::error_code error;
                const fs::path requested = path_from_utf8(target.path);
                const fs::file_status status = fs::symlink_status(requested, error);
                if (error || fs::is_symlink(status) || fs::is_other(status)) {
                    detail = error ? error.message() :
                        "Workspace mutations reject symbolic links and special filesystem entries";
                    break;
                }
                const fs::path source = fs::weakly_canonical(requested, error);
                if (error || !path_inside_roots(source, roots, false)) {
                    detail = error ? error.message() : "A selected item escaped its retained workspace root";
                    break;
                }
                if (operation == file_operation_t::remove) {
                    const auto removed = fs::remove_all(source, error);
                    if (error || removed == 0) {
                        detail = error ? error.message() : "A selected item no longer exists";
                        partial = !completed.empty();
                        break;
                    }
                    ++permanently_completed;
                    continue;
                }
                const fs::path parent = fs::weakly_canonical(destination_directory, error);
                if (error || !path_inside_roots(parent, roots, true)) {
                    detail = error ? error.message() : "The resolved paste destination escaped its workspace root";
                    break;
                }
                const fs::path destination = parent / source.filename();
                if (fs::exists(destination, error) || error) {
                    detail = error ? error.message() : "A paste destination already exists: " + destination.u8string();
                    break;
                }
                if (target.directory) {
                    const std::string source_key = path_key(source.u8string());
                    const std::string destination_key = path_key(destination.u8string());
                    if (destination_key.size() > source_key.size() &&
                        destination_key.compare(0, source_key.size(), source_key) == 0 &&
                        (destination_key[source_key.size()] == '/' || destination_key[source_key.size()] == '\\')) {
                        detail = "A folder cannot be copied or moved inside itself";
                        break;
                    }
                }
                if (operation == file_operation_t::copy) {
                    if (!bounded_copy_tree(source, destination, cancelled, detail,
                            total_bytes, entry_count))
                        break;
                } else {
                    fs::rename(source, destination, error);
                    if (error) {
                        detail = "Move failed; cross-volume cut is not performed implicitly: " + error.message();
                        break;
                    }
                }
                completed.emplace_back(source, destination);
            }
            succeeded = operation == file_operation_t::remove
                ? permanently_completed == targets.size()
                : completed.size() == targets.size();
            if (!succeeded && (operation == file_operation_t::copy ||
                    operation == file_operation_t::cut)) {
                if (!recover_completed()) {
                    partial = true;
                    detail += operation == file_operation_t::cut
                        ? "; one or more completed moves could not be rolled back"
                        : "; one or more copied destinations could not be cleaned up";
                }
            }
            if (succeeded)
                detail = operation == file_operation_t::remove
                    ? "Selected items deleted" : operation == file_operation_t::cut
                        ? "Selected items moved" : "Selected items copied";
            else if (operation == file_operation_t::remove && permanently_completed != 0)
                partial = true;
        } catch (const std::exception& exception) {
            detail = exception.what();
            if (!completed.empty() || permanently_completed != 0) {
                if (operation == file_operation_t::remove)
                    partial = permanently_completed != 0;
                else if (!recover_completed()) {
                    partial = true;
                    detail += operation == file_operation_t::cut
                        ? "; one or more completed moves could not be rolled back"
                        : "; one or more copied destinations could not be cleaned up";
                }
            }
        } catch (...) {
            detail = "The batch filesystem operation failed with a non-standard exception";
            if (!completed.empty() || permanently_completed != 0) {
                if (operation == file_operation_t::remove)
                    partial = permanently_completed != 0;
                else if (!recover_completed()) {
                    partial = true;
                    detail += operation == file_operation_t::cut
                        ? "; one or more completed moves could not be rolled back"
                        : "; one or more copied destinations could not be cleaned up";
                }
            }
        }
        aida::ui_thread::post_options_t options;
        options.subsystem = "project_explorer";
        options.label = "batch_file_operation_result";
        options.phase = "worker_result";
        options.owner = "project_explorer.file_operation";
        const auto posted = aida::ui_thread::post([generation, succeeded, partial,
                detail = std::move(detail)]() mutable {
            auto& current = file_operations();
            if (current.generation != generation) return;
            current.operation_pending = false;
            current.task_id = 0;
            current.dispatch_failed.reset();
            current.operation_error = succeeded ? std::string{} : detail;
            const bool clears_selection = succeeded &&
                (current.operation == file_operation_t::remove || current.clipboard_cut);
            if (succeeded && current.clipboard_cut) {
                current.clipboard_targets.clear();
                current.clipboard_path.clear();
                current.clipboard_cut = false;
            }
            if (clears_selection) {
                file_browser::selected_paths.clear();
                file_browser::selection_anchor_path.clear();
                file_browser::selected_idx = -1;
                ++file_browser::selection_revision;
            }
            file_browser::needs_refresh = true;
            if (!succeeded) {
                toast_notification::push(
                    std::string(partial
                        ? "Project Explorer batch operation completed only partially: "
                        : "Project Explorer batch operation failed: ") + current.operation_error,
                    toast_notification::toast_type_t::error, 6.0f);
            }
        }, std::move(options));
        if (posted != aida::ui_thread::enqueue_result_t::accepted)
            dispatch_failed->store(true, std::memory_order_release);
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        state.operation_pending = false;
        state.operation_error = "The batch filesystem worker could not be scheduled: " + submitted.reject_reason;
        return {false, state.operation_error};
    }
    state.task_id = submitted.task_id;
    task_center::task_registration_t registration;
    registration.id = "project-explorer-batch-operation-" + std::to_string(generation);
    registration.source = "Project Explorer";
    registration.owner = "project_explorer";
    registration.owner_view = "view.project_explorer";
    registration.owner_action = operation_label(operation);
    registration.target = std::to_string(state.sources.size()) + " selected items";
    registration.label = operation_label(operation);
    registration.stage = "Queued";
    registration.cancellation_is_safe = operation == file_operation_t::copy;
    if (registration.cancellation_is_safe)
        registration.callbacks.cancel = [task = submitted.task_id] {
            return aida::infra::executor::cancel(task);
        };
    if (!task_center::register_executor_job(submitted.task_id, std::move(registration))) {
        toast_notification::push(
            "Project Explorer batch work could not register in Background Tasks: "
            "the bounded filesystem worker is running, but Task Center rejected its registration",
            toast_notification::toast_type_t::error, 6.0f);
    }
    return {true, "The batch filesystem operation was queued"};
}

namespace {

std::string path_key(const std::string& path) {
    try {
        std::string normalized = path_to_utf8(path_from_utf8(path).lexically_normal());
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return normalized;
    } catch (...) {
        return {};
    }
}

std::string path_leaf(const std::string& path) {
    try {
        const std::string value = path_to_utf8(path_from_utf8(path).filename());
        return value.empty() ? path : value;
    } catch (...) {
        return path;
    }
}

file_operation_result_t request_search_scope_impl(const std::string& path, bool directory) {
    if (path.empty())
        return {false, "Select a Project Explorer file or folder first"};
    const std::filesystem::path selected = path_from_utf8(path);
    const std::filesystem::path scope = directory ? selected : selected.parent_path();
    if (scope.empty() || !path_inside_root(scope, true))
        return {false, "The retained search scope is outside the open Project Explorer roots"};
    aida::qt::programming::host::set_workspace_search_scope(
        path_to_utf8(scope.lexically_normal()));
    if (!aida::qt::programming::host::open_or_focus_view("view.workspace_search"))
        return {false, "The Workspace Search view host is unavailable"};
    return {true, {}};
}

void request_recent_open(const std::string& path) {
    file_browser::pending_open_path = path;
    file_browser::pending_open_filename = path_leaf(path);
    file_browser::pending_open_should_open = true;
    file_browser::pending_open_modal_visible = true;
}

const std::vector<std::string>& recent_workspace_paths() {
	static std::string source;
	static std::vector<std::string> paths;
	if (source == g_sa_settings.recent_workspaces_json)
		return paths;
	source = g_sa_settings.recent_workspaces_json;
	paths.clear();
	if (source.empty())
		return paths;
	const auto json = nlohmann::json::parse(source, nullptr, false);
    if (json.is_discarded() || !json.is_array())
        return paths;
    std::unordered_set<std::string> seen;
    for (const auto& value : json) {
        if (!value.is_string())
            continue;
        std::string path = value.get<std::string>();
        if (path.empty() || !seen.insert(path_key(path)).second)
            continue;
        paths.push_back(std::move(path));
        if (paths.size() == 32)
            break;
    }
    return paths;
}

bool session_is_open(const std::string& path) {
    const std::string expected = path_key(path);
    for (std::size_t index = 0; index < analysis_session::session_count(); ++index) {
        const auto session = analysis_session::session_handle_at(index);
        if (session && path_key(session->path) == expected)
            return true;
    }
    return false;
}

const std::string* previous_closed_session(const std::vector<std::string>& paths) {
    const auto found = std::find_if(paths.begin(), paths.end(), [](const std::string& path) {
        return !session_is_open(path);
    });
    return found == paths.end() ? nullptr : &*found;
}
}

file_operation_result_t submit_confirmed_file_operation(file_operation_t operation,
    std::filesystem::path source, std::filesystem::path destination,
    bool source_directory) {
    return submit_file_operation(operation, std::move(source), std::move(destination),
        source_directory);
}

file_operation_result_t submit_confirmed_batch_file_operation(file_operation_t operation,
    std::vector<file_operation_target_t> targets) {
    return submit_batch_file_operation(operation, std::move(targets));
}

file_operation_capability_t file_operation_capability(file_operation_t operation,
    const std::string& path, bool directory) {
    try {
    const auto& state = file_operations();
    if (state.operation_pending)
        return {false, "Another Project Explorer filesystem operation is already running"};
    if (file_browser::roots.empty())
        return {false, "Open a Project Explorer root first"};
    if ((!path.empty() && path_key(path).empty()) ||
        std::any_of(file_browser::roots.begin(), file_browser::roots.end(),
            [](const std::string& root) { return path_key(root).empty(); }))
        return {false, "The selected path or workspace root is not valid UTF-8 and cannot be converted safely"};
    if (operation == file_operation_t::paste) {
        if (state.clipboard_targets.empty())
            return {false, "Cut or copy a Project Explorer item first"};
        if (state.clipboard_cut) {
            for (const auto& staged_target : state.clipboard_targets) {
                const std::string staged = path_key(staged_target.path);
                if (staged.empty())
                    return {false, "A staged clipboard path is not valid UTF-8 and cannot be converted safely"};
                for (const auto& tab : file_tabs::tabs) {
                    const std::string document = path_key(tab.filepath);
                    if (document == staged || (staged_target.directory &&
                        document.size() > staged.size() &&
                        document.compare(0, staged.size(), staged) == 0 &&
                        (document[staged.size()] == '/' || document[staged.size()] == '\\')))
                        return {false, "Close editor documents under every cut path before moving the selection"};
                }
            }
        }
        const std::filesystem::path target = directory
            ? path_from_utf8(path) : path_from_utf8(path).parent_path();
        return path_inside_root(target, true)
            ? file_operation_capability_t{true, {}}
            : file_operation_capability_t{false, "Select a destination inside an open Project Explorer root"};
    }
    if (path.empty())
        return {false, "Select a file or folder first"};
    const bool allow_root = operation == file_operation_t::new_file ||
        operation == file_operation_t::new_folder || operation == file_operation_t::terminal_here;
    if (!path_inside_root(path_from_utf8(path), allow_root))
        return {false, allow_root ? "The selected destination is outside the open Project Explorer roots" :
            "Workspace roots cannot be renamed, moved, copied, duplicated, or deleted"};
    if (operation == file_operation_t::open_with && directory)
        return {false, "Open With is available for files; use Terminal Here for folders"};
    if (operation == file_operation_t::rename || operation == file_operation_t::cut ||
        operation == file_operation_t::remove) {
        const std::string selected = path_key(path);
        for (const auto& tab : file_tabs::tabs) {
            const std::string document = path_key(tab.filepath);
            const bool affected = document == selected || (directory &&
                document.size() > selected.size() &&
                document.compare(0, selected.size(), selected) == 0 &&
                (document[selected.size()] == '/' || document[selected.size()] == '\\'));
            if (!affected)
                continue;
            if (tab.dirty)
                return {false, "Save or close the modified editor document before changing its path"};
            if (tab.save_in_progress || tab.load_in_progress)
                return {false, "Wait for the editor document operation to finish before changing its path"};
            return {false, "Close editor documents under this path before renaming, moving, or deleting it"};
        }
    }
    return {true, {}};
    } catch (const std::exception&) {
        return {false, "The selected path is not valid UTF-8 or cannot be represented by the native filesystem"};
    } catch (...) {
        return {false, "The selected path could not be converted to a native filesystem path"};
    }
}

file_operation_result_t request_search_scope(const std::string& path, bool directory) {
    return request_search_scope_impl(path, directory);
}

file_operation_result_t request_file_operation(file_operation_t operation,
    const std::string& path, bool directory) {
    try {
    const auto capability = file_operation_capability(operation, path, directory);
    if (!capability.enabled)
        return {false, capability.reason};
    auto& state = file_operations();
    state.operation = operation;
    state.source = path_to_utf8(path_from_utf8(path).lexically_normal());
    state.source_directory = directory;
    state.sources = {{state.source, directory}};
    state.validation_error.clear();
    state.operation_error.clear();
    if (operation == file_operation_t::cut || operation == file_operation_t::copy) {
        state.clipboard_path = state.source;
        state.clipboard_targets = {{state.source, directory}};
        state.clipboard_cut = operation == file_operation_t::cut;
        state.clipboard_directory = directory;
        return {true, operation == file_operation_t::cut
            ? "The item is ready to move; choose Paste in a destination folder"
            : "The item is ready to copy; choose Paste in a destination folder"};
    }
    if (operation == file_operation_t::paste) {
        const std::filesystem::path destination_directory = directory
            ? path_from_utf8(path) : path_from_utf8(path).parent_path();
        if (state.clipboard_targets.size() > 1)
            return submit_batch_file_operation(state.clipboard_cut
                ? file_operation_t::cut : file_operation_t::copy,
                state.clipboard_targets, destination_directory);
        const std::filesystem::path source = path_from_utf8(state.clipboard_targets.front().path);
        state.source = path_to_utf8(source);
        state.source_directory = state.clipboard_targets.front().directory;
        return submit_file_operation(state.clipboard_cut
            ? file_operation_t::cut : file_operation_t::copy,
            source, destination_directory / source.filename(), state.source_directory);
    }
    if (operation == file_operation_t::open_with ||
        operation == file_operation_t::terminal_here) {
        if (operation == file_operation_t::terminal_here) {
            const std::filesystem::path selected = path_from_utf8(state.source);
            const std::string directory_path = path_to_utf8(directory
                ? selected : selected.parent_path());
            const auto terminal = aida::qt::programming::host::terminal_new_at(directory_path);
            if (!terminal.succeeded)
                return {false, terminal.detail};
            const bool opened = aida::qt::programming::host::open_or_focus_view("view.terminal");
            return opened ? file_operation_result_t{true, "Integrated terminal opened"}
                : file_operation_result_t{false, "The Terminal view host is unavailable"};
        }
        return submit_file_operation(operation, path_from_utf8(state.source), {}, directory);
    }
    if (operation == file_operation_t::remove) {
        state.delete_dialog_open = true;
        state.delete_dialog_requested = true;
        return {true, "Review the exact deletion target before continuing"};
    }
    std::string proposed;
    if (operation == file_operation_t::rename)
        proposed = path_to_utf8(path_from_utf8(path).filename());
    else if (operation == file_operation_t::duplicate) {
        const auto source = path_from_utf8(path);
        proposed = path_to_utf8(source.stem()) + " copy" + path_to_utf8(source.extension());
    }
    std::snprintf(state.name, sizeof(state.name), "%s", proposed.c_str());
    state.name_dialog_open = true;
    state.name_dialog_requested = true;
    return {true, "Enter and validate the destination name"};
    } catch (const std::exception&) {
        return {false, "The selected path is not valid UTF-8 or cannot be represented by the native filesystem"};
    } catch (...) {
        return {false, "The selected path could not be converted to a native filesystem path"};
    }
}

file_operation_capability_t file_operation_capability(file_operation_t operation,
    const std::vector<file_operation_target_t>& targets) {
    if (targets.empty()) return {false, "Select a file or folder first"};
    if (targets.size() == 1)
        return file_operation_capability(operation, targets.front().path,
            targets.front().directory);
    if (targets.size() > 100000)
        return {false, "Project Explorer batch operations are limited to 100,000 selected items"};
    if (operation != file_operation_t::copy && operation != file_operation_t::cut &&
        operation != file_operation_t::remove)
        return {false, "This action requires exactly one selected Project Explorer item"};
    std::vector<std::string> keys;
    keys.reserve(targets.size());
    for (const auto& target : targets) {
        const auto capability = file_operation_capability(operation,
            target.path, target.directory);
        if (!capability.enabled) return capability;
        keys.push_back(path_key(target.path));
    }
    const std::unordered_set<std::string> selected_keys(keys.begin(), keys.end());
    for (const auto& key : keys) {
        std::size_t boundary = key.size();
        while (boundary > 0) {
            boundary = key.find_last_of("/\\", boundary - 1);
            if (boundary == std::string::npos) break;
            const std::string ancestor = key.substr(0, boundary);
            if (!ancestor.empty() && selected_keys.find(ancestor) != selected_keys.end())
                return {false, "The selection contains both a folder and one of its descendants; deselect the descendant"};
            if (boundary == 0) break;
        }
    }
    return {true, {}};
}

file_operation_result_t request_file_operation(file_operation_t operation,
    const std::vector<file_operation_target_t>& targets) {
    const auto capability = file_operation_capability(operation, targets);
    if (!capability.enabled) return {false, capability.reason};
    if (targets.size() == 1)
        return request_file_operation(operation, targets.front().path,
            targets.front().directory);
    auto& state = file_operations();
    state.operation = operation;
    state.sources = targets;
    state.source = std::to_string(targets.size()) + " selected items";
    state.validation_error.clear();
    state.operation_error.clear();
    if (operation == file_operation_t::copy || operation == file_operation_t::cut) {
        state.clipboard_targets = targets;
        state.clipboard_path = targets.front().path;
        state.clipboard_directory = targets.front().directory;
        state.clipboard_cut = operation == file_operation_t::cut;
        return {true, operation == file_operation_t::cut
            ? "The selected items are ready to move; choose Paste in a destination folder"
            : "The selected items are ready to copy; choose Paste in a destination folder"};
    }
    state.delete_dialog_open = true;
    state.delete_dialog_requested = true;
    return {true, "Review the exact selected deletion set before continuing"};
}

bool can_restore_previous_session() {
	const auto& paths = recent_workspace_paths();
    return previous_closed_session(paths) != nullptr;
}

bool request_restore_previous_session() {
	const auto& paths = recent_workspace_paths();
    const std::string* path = previous_closed_session(paths);
    if (!path)
        return false;
    request_recent_open(*path);
    return true;
}

}
