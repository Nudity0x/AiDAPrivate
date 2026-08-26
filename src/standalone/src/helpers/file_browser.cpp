
#include <windows.h>
#include <shlobj.h>
#ifdef small
#undef small
#endif

#include "globals.h"
#include "standalone_settings.hpp"
#include "../core/settings/settings_persistence_service.hpp"
#include "diag_log.hpp"
#include "../core/infra/executor.hpp"
#include "../core/infra/taskflow_runtime.hpp"
#include "../core/ui/ui_thread_dispatcher.hpp"
#include "qt/explorer/exchange/open_dispatch.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <sstream>
#include <atomic>
#include <limits>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <cstring>
#include <exception>

namespace fs = std::filesystem;

extern HWND g_hwnd;

namespace {

fs::path explorer_path_from_utf8(std::string_view value)
{
#if defined(__cpp_char8_t)
	const auto* begin = reinterpret_cast<const char8_t*>(value.data());
	return fs::path(std::u8string(begin, begin + value.size()));
#else
	return fs::u8path(value.begin(), value.end());
#endif
}

std::string explorer_path_to_utf8(const fs::path& value)
{
	const auto encoded = value.generic_u8string();
#if defined(__cpp_char8_t)
	return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
	return encoded;
#endif
}

std::string normalized_explorer_path(const std::string& input)
{
	std::string value = explorer_path_to_utf8(
		explorer_path_from_utf8(input).lexically_normal());
	while (value.size() > 1 && value.back() == '/')
		value.pop_back();
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

}

namespace {

constexpr std::size_t k_explorer_entry_limit = 250000;
constexpr std::size_t k_explorer_string_budget = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t k_explorer_publish_batch = 512;

struct explorer_index_control_t {
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::shared_ptr<std::atomic<std::size_t>> pending_publications;
    std::uint64_t task_id = 0;
    std::uint64_t generation = 0;
};

explorer_index_control_t& explorer_index_control()
{
    static explorer_index_control_t value;
    return value;
}

std::uint64_t explorer_identity(std::string_view value, std::uint64_t seed = 14695981039346656037ULL)
{
    std::uint64_t hash = seed;
    for (const char character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

bool publish_explorer_batch(std::uint64_t generation,
    std::shared_ptr<const std::vector<FileBrowserEntry>> batch,
    std::size_t directory_count, bool final, bool cancelled,
    bool truncated, std::string error, std::string selected_path,
    std::shared_ptr<std::unordered_set<std::string>> matched_selected_paths,
    int matched_primary_index, std::string matched_primary_path,
    std::string retained_anchor_path, std::uint64_t retained_selection_revision,
    std::shared_ptr<std::atomic<std::size_t>> pending_publications)
{
    bool posted = false;
    for (int attempt = 0; attempt < 100 && !posted; ++attempt) {
        posted = aida::ui_thread::post(
        [generation, batch, directory_count, final, cancelled,
         truncated, error, selected_path, matched_selected_paths,
         matched_primary_index, matched_primary_path,
         retained_anchor_path, retained_selection_revision, pending_publications]() mutable {
            struct publication_guard_t {
                std::shared_ptr<std::atomic<std::size_t>> pending;
                ~publication_guard_t() {
                    if (pending) pending->fetch_sub(1, std::memory_order_acq_rel);
                }
            } publication_guard{pending_publications};
            if (generation != file_browser::index_generation)
                return;
            if (batch) {
                file_browser::entries.insert(file_browser::entries.end(),
                    batch->begin(), batch->end());
                if (file_browser::selected_idx < 0 && !selected_path.empty()) {
                    for (std::size_t index = file_browser::entries.size() - batch->size();
                         index < file_browser::entries.size(); ++index) {
                        if (normalized_explorer_path(file_browser::entries[index].full_path) == selected_path) {
                            file_browser::selected_idx = static_cast<int>(index);
                            break;
                        }
                    }
                }
            }
            file_browser::indexed_directory_count = directory_count;
            file_browser::indexed_entry_count = file_browser::entries.size();
            file_browser::index_truncated = truncated;
            if (!error.empty())
                file_browser::index_error = std::move(error);
            if (final) {
                file_browser::index_state = cancelled
                    ? file_browser::index_state_t::cancelled
                    : (file_browser::entries.empty() && !file_browser::index_error.empty()
                        ? file_browser::index_state_t::error
                        : file_browser::index_state_t::ready);
                const bool selection_unchanged = file_browser::selection_revision ==
                    retained_selection_revision;
                if (matched_selected_paths && selection_unchanged)
                    file_browser::selected_paths.swap(*matched_selected_paths);
                if (selection_unchanged) {
                    file_browser::selected_idx = matched_primary_index >= 0 &&
                        static_cast<std::size_t>(matched_primary_index) < file_browser::entries.size()
                        ? matched_primary_index : -1;
                    file_browser::selection_anchor_path =
                        file_browser::selected_paths.find(retained_anchor_path) !=
                            file_browser::selected_paths.end()
                        ? retained_anchor_path : matched_primary_path;
                }
                if (file_browser::selected_paths.size() < 100000)
                    file_browser::selection_error.clear();
                auto& control = explorer_index_control();
                if (control.generation == generation)
                    control.task_id = 0;
            }
        }, "file_browser", "index_snapshot", final ? "worker_final" : "worker_batch");
        if (!posted)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!posted)
    {
        if (pending_publications)
            pending_publications->fetch_sub(1, std::memory_order_acq_rel);
        diag::log_tagged_fmt("file_browser",
            "index_snapshot_dispatch_failed generation=%llu final=%d entries=%zu",
            static_cast<unsigned long long>(generation), final ? 1 : 0,
            batch ? batch->size() : 0U);
    }
    return posted;
}

}

void file_browser::refresh(const std::string& dir)
{
    if (!aida::ui_thread::is_owner_thread()) {
        const bool routed = aida::ui_thread::post([dir]() {
            file_browser::refresh(dir);
        }, "file_browser", "refresh", "entry");
        if (!routed)
            diag::log_tagged_fmt("file_browser", "refresh denied dir=%s reason=ui_affinity_route_failed", dir.c_str());
        return;
    }
    if (!aida::ui_thread::require_owner("file_browser", "refresh", "entry"))
        return;

    std::string root = dir;
    if (root.empty() && current_dir.empty() && roots.empty()) {
        char buf[MAX_PATH] = {};
        GetCurrentDirectoryA(MAX_PATH, buf);
        root = buf;
    }
    if (!root.empty()) {
        roots = {fs::path(root).lexically_normal().string()};
        expanded_paths.clear();
        expanded_paths.insert(normalized_explorer_path(roots.front()));
    }
    else if (roots.empty() && !current_dir.empty()) {
        roots = {fs::path(current_dir).lexically_normal().string()};
        expanded_paths.insert(normalized_explorer_path(roots.front()));
    }
    if (roots.empty())
        return;
    current_dir = roots.front();
    strncpy_s(path_buf, sizeof(path_buf), current_dir.c_str(), _TRUNCATE);

    for (const auto& entry : entries)
        if (entry.is_dir && entry.expanded)
            expanded_paths.insert(normalized_explorer_path(entry.full_path));
    std::string selected_path = normalized_explorer_path(pending_reveal_path);
    pending_reveal_path.clear();
    if (selected_path.empty() && selected_idx >= 0 && static_cast<std::size_t>(selected_idx) < entries.size())
        selected_path = normalized_explorer_path(entries[static_cast<std::size_t>(selected_idx)].full_path);
    auto selected_keys = std::make_shared<const std::unordered_set<std::string>>(selected_paths);
    if (!selected_path.empty() && selected_keys->find(selected_path) == selected_keys->end()) {
        auto completed_keys = std::make_shared<std::unordered_set<std::string>>(*selected_keys);
        completed_keys->insert(selected_path);
        selected_keys = std::static_pointer_cast<const std::unordered_set<std::string>>(completed_keys);
    }
    const std::string retained_anchor_path = selection_anchor_path;
    const std::uint64_t retained_selection_revision = selection_revision;

    auto& control = explorer_index_control();
    if (control.cancelled)
        control.cancelled->store(true, std::memory_order_release);
    if (control.task_id != 0)
        aida::infra::executor::cancel(control.task_id);
    control.cancelled = std::make_shared<std::atomic<bool>>(false);
    control.pending_publications = std::make_shared<std::atomic<std::size_t>>(0);
    control.generation = ++index_generation;
    control.task_id = 0;

    entries.clear();
    selected_idx = -1;
    needs_refresh = false;
    index_state = index_state_t::loading;
    index_error.clear();
    indexed_directory_count = 0;
    indexed_entry_count = 0;
    index_truncated = false;

    auto roots_copy = roots;
    if (roots_copy.size() > 8)
        roots_copy.resize(8);
    const auto expanded_copy = expanded_paths;
    const auto cancel = control.cancelled;
    const auto pending_publications = control.pending_publications;
    const std::uint64_t generation = control.generation;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "file_browser";
    submission.label = "file_browser.project_index";
    submission.thread_class = "blocking_directory_io";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 3;
    submission.generation = generation;
    submission.cancel_hook = [cancel]() {
        cancel->store(true, std::memory_order_release);
    };
    submission.body = [roots_copy, expanded_copy, cancel, pending_publications, generation,
                       selected_path = std::move(selected_path), selected_keys,
                       retained_anchor_path, retained_selection_revision]() mutable {
        std::size_t directory_count = 0;
        bool truncated = false;
        auto matched_selected_paths = std::make_shared<std::unordered_set<std::string>>();
        matched_selected_paths->reserve(selected_keys->size());
        int matched_primary_index = -1;
        std::string matched_primary_path;
        try {
        std::vector<FileBrowserEntry> batch;
        batch.reserve(k_explorer_publish_batch);
        std::size_t total_entries = 0;
        std::size_t total_string_bytes = 0;
        std::string errors;
        const bool multiple_roots = roots_copy.size() > 1;
        auto append_error = [&errors](const std::string& path, const std::string& detail) {
            if (errors.size() >= 4096) return;
            if (!errors.empty()) errors.append("; ");
            errors.append(path).append(": ").append(detail);
            if (errors.size() > 4096) errors.resize(4096);
        };

        auto flush = [&](bool final) {
            while (pending_publications->load(std::memory_order_acquire) >= 8 &&
                   !cancel->load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            pending_publications->fetch_add(1, std::memory_order_acq_rel);
            auto immutable = std::make_shared<const std::vector<FileBrowserEntry>>(std::move(batch));
            const bool published = publish_explorer_batch(generation, std::move(immutable), directory_count,
                final, cancel->load(std::memory_order_acquire), truncated,
                final ? errors : std::string{}, selected_path,
                final ? matched_selected_paths : nullptr,
                final ? matched_primary_index : -1,
                final ? matched_primary_path : std::string{},
                retained_anchor_path, retained_selection_revision,
                pending_publications);
            batch.clear();
            batch.reserve(k_explorer_publish_batch);
            if (!published) {
                cancel->store(true, std::memory_order_release);
                append_error("Project index", "UI publication remained unavailable after bounded retries");
            }
            return published;
        };
        auto append = [&](FileBrowserEntry entry) {
            const std::size_t bytes = entry.name.size() + entry.full_path.size();
            if (total_entries >= k_explorer_entry_limit ||
                total_string_bytes + bytes > k_explorer_string_budget) {
                truncated = true;
                return false;
            }
            total_string_bytes += bytes;
            const std::string key = normalized_explorer_path(entry.full_path);
            if (selected_keys->find(key) != selected_keys->end()) {
                matched_selected_paths->insert(key);
                if (matched_primary_index < 0 || key == selected_path) {
                    matched_primary_index = static_cast<int>(total_entries);
                    matched_primary_path = key;
                }
            }
            ++total_entries;
            batch.push_back(std::move(entry));
            if (batch.size() >= k_explorer_publish_batch && !flush(false))
                return false;
            return true;
        };

        std::function<void(const std::string&, int, std::uint64_t, std::uint64_t)> scan;
        scan = [&](const std::string& directory, int depth,
                   std::uint64_t root_id, std::uint64_t parent_id) {
            if (cancel->load(std::memory_order_acquire) || truncated)
                return;
            if (depth > 256) {
                truncated = true;
                append_error(directory, "maximum Explorer nesting depth (256) reached");
                return;
            }
            ++directory_count;
            std::error_code ec;
            std::vector<FileBrowserEntry> directories;
            std::vector<FileBrowserEntry> files;
            std::size_t local_string_bytes = 0;
            const std::size_t remaining_entries = k_explorer_entry_limit - total_entries;
            const std::size_t remaining_string_bytes = k_explorer_string_budget - total_string_bytes;
            fs::directory_iterator iterator(fs::path(directory),
                fs::directory_options::skip_permission_denied, ec);
            if (ec) {
                append_error(directory, ec.message());
                return;
            }
            const fs::directory_iterator end;
            while (iterator != end) {
                if (cancel->load(std::memory_order_acquire) || truncated)
                    return;
                const fs::directory_entry item = *iterator;
                const std::string name = item.path().filename().string();
                if (!name.empty() && name.front() != '.') {
                    FileBrowserEntry entry;
                    entry.full_path = item.path().lexically_normal().string();
                    entry.name = name;
                    entry.depth = depth;
                    entry.root_id = root_id;
                    entry.parent_id = parent_id;
                    entry.generation = generation;
                    const std::string key = normalized_explorer_path(entry.full_path);
                    entry.entry_id = explorer_identity(key, root_id);
                    const std::size_t entry_string_bytes = entry.name.size() + entry.full_path.size();
                    if (directories.size() + files.size() >= remaining_entries ||
                        local_string_bytes + entry_string_bytes > remaining_string_bytes) {
                        truncated = true;
                        break;
                    }
                    std::error_code type_error;
                    const bool directory_entry = item.is_directory(type_error) && !type_error;
                    const bool symlink_entry = directory_entry && item.is_symlink(type_error) && !type_error;
                    if (directory_entry && !symlink_entry) {
                        local_string_bytes += entry_string_bytes;
                        entry.is_dir = true;
                        entry.expanded = expanded_copy.find(key) != expanded_copy.end();
                        directories.push_back(std::move(entry));
                    } else if (item.is_regular_file(type_error) && !type_error) {
                        local_string_bytes += entry_string_bytes;
                        files.push_back(std::move(entry));
                    }
                }
                iterator.increment(ec);
                if (ec) {
                    append_error(directory, ec.message());
                    break;
                }
            }
            const auto by_name = [](const FileBrowserEntry& left, const FileBrowserEntry& right) {
                return _stricmp(left.name.c_str(), right.name.c_str()) < 0;
            };
            std::sort(directories.begin(), directories.end(), by_name);
            std::sort(files.begin(), files.end(), by_name);
            for (auto& child : directories) {
                const bool expanded = child.expanded;
                const std::string child_path = child.full_path;
                const std::uint64_t child_id = child.entry_id;
                if (!append(std::move(child))) return;
                if (expanded)
                    scan(child_path, depth + 1, root_id, child_id);
            }
            for (auto& child : files)
                if (!append(std::move(child))) return;
        };

        for (const auto& root_path : roots_copy) {
            if (cancel->load(std::memory_order_acquire) || truncated)
                break;
            const std::string root_key = normalized_explorer_path(root_path);
            const std::uint64_t root_id = explorer_identity(root_key);
            std::error_code root_error;
            if (!fs::is_directory(fs::path(root_path), root_error) || root_error) {
                append_error(root_path, root_error ? root_error.message() : "not a directory");
                continue;
            }
            bool scan_root = true;
            if (multiple_roots) {
                FileBrowserEntry root_entry;
                root_entry.name = fs::path(root_path).filename().string();
                if (root_entry.name.empty()) root_entry.name = root_path;
                root_entry.full_path = root_path;
                root_entry.is_dir = true;
                root_entry.expanded = expanded_copy.find(root_key) != expanded_copy.end();
                scan_root = root_entry.expanded;
                root_entry.depth = 0;
                root_entry.root_id = root_id;
                root_entry.entry_id = root_id;
                root_entry.generation = generation;
                root_entry.is_root = true;
                if (!append(std::move(root_entry))) break;
            }
            if (scan_root)
                scan(root_path, multiple_roots ? 1 : 0, root_id, multiple_roots ? root_id : 0);
        }
        flush(true);
        } catch (const std::exception& exception) {
            while (pending_publications->load(std::memory_order_acquire) >= 8 &&
                   !cancel->load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            pending_publications->fetch_add(1, std::memory_order_acq_rel);
            auto empty = std::make_shared<const std::vector<FileBrowserEntry>>();
            publish_explorer_batch(generation, std::move(empty), directory_count, true,
                cancel->load(std::memory_order_acquire), truncated,
                std::string("Project index failed: ") + exception.what(), selected_path,
                matched_selected_paths, matched_primary_index, matched_primary_path,
                retained_anchor_path, retained_selection_revision,
                pending_publications);
        } catch (...) {
            while (pending_publications->load(std::memory_order_acquire) >= 8 &&
                   !cancel->load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            pending_publications->fetch_add(1, std::memory_order_acq_rel);
            auto empty = std::make_shared<const std::vector<FileBrowserEntry>>();
            publish_explorer_batch(generation, std::move(empty), directory_count, true,
                cancel->load(std::memory_order_acquire), truncated,
                "Project index failed with an unknown filesystem error.", selected_path,
                matched_selected_paths, matched_primary_index, matched_primary_path,
                retained_anchor_path, retained_selection_revision,
                pending_publications);
        }
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        index_state = index_state_t::error;
        index_error = "The project index worker could not be scheduled: " + submitted.reject_reason;
        control.cancelled.reset();
        diag::log_tagged_fmt("file_browser", "index_submit_failed generation=%llu reason=%s",
            static_cast<unsigned long long>(generation), submitted.reject_reason.c_str());
        return;
    }
    control.task_id = submitted.task_id;
}

bool file_browser::reveal_path(const std::string& path)
{
    if (path.empty() || roots.empty())
        return false;
    const fs::path candidate = explorer_path_from_utf8(path).lexically_normal();
    const std::string candidate_key = normalized_explorer_path(
        explorer_path_to_utf8(candidate));
    bool inside_root = false;
    for (const auto& root_value : roots) {
        const fs::path root = explorer_path_from_utf8(root_value).lexically_normal();
        const std::string root_key = normalized_explorer_path(explorer_path_to_utf8(root));
        if (candidate_key == root_key || (candidate_key.size() > root_key.size() &&
            candidate_key.compare(0, root_key.size(), root_key) == 0 &&
            (candidate_key[root_key.size()] == '/' || candidate_key[root_key.size()] == '\\'))) {
            inside_root = true;
            fs::path parent = candidate.parent_path();
            while (!parent.empty()) {
                expanded_paths.insert(normalized_explorer_path(explorer_path_to_utf8(parent)));
                if (normalized_explorer_path(explorer_path_to_utf8(parent)) == root_key)
                    break;
                const fs::path next = parent.parent_path();
                if (next == parent)
                    break;
                parent = next;
            }
            break;
        }
    }
    if (!inside_root)
        return false;
    pending_reveal_path = explorer_path_to_utf8(candidate);
    selected_paths.clear();
    selected_paths.insert(normalized_explorer_path(pending_reveal_path));
    selection_anchor_path = normalized_explorer_path(pending_reveal_path);
    selected_idx = -1;
    ++selection_revision;
    needs_refresh = true;
    return true;
}

void file_browser::set_roots(std::vector<std::string> requested_roots)
{
    if (!aida::ui_thread::is_owner_thread()) {
        const bool routed = aida::ui_thread::post(
            [requested_roots = std::move(requested_roots)]() mutable {
                file_browser::set_roots(std::move(requested_roots));
            }, "file_browser", "set_roots", "entry");
        if (!routed)
            diag::log_tagged("file_browser", "set_roots denied reason=ui_affinity_route_failed");
        return;
    }
    if (!aida::ui_thread::require_owner("file_browser", "set_roots", "entry"))
        return;
    std::vector<std::string> normalized;
    std::unordered_set<std::string> seen;
    normalized.reserve(requested_roots.size());
    for (auto& root : requested_roots) {
        if (normalized.size() >= 8) break;
        if (root.empty()) continue;
        root = fs::path(root).lexically_normal().string();
        if (seen.insert(normalized_explorer_path(root)).second)
            normalized.push_back(std::move(root));
    }
    roots = std::move(normalized);
    std::unordered_set<std::string> retained_expansions;
    retained_expansions.reserve((std::min)(expanded_paths.size() + roots.size(), k_explorer_entry_limit));
    for (const auto& root : roots) {
        const std::string key = normalized_explorer_path(root);
        retained_expansions.insert(key);
        const std::string prefix = key + "/";
        for (const auto& expanded : expanded_paths) {
            if (retained_expansions.size() >= k_explorer_entry_limit) break;
            if (expanded.compare(0, prefix.size(), prefix) == 0)
                retained_expansions.insert(expanded);
        }
    }
    expanded_paths = std::move(retained_expansions);
    current_dir = roots.empty() ? std::string{} : roots.front();
    needs_refresh = true;
}

bool file_browser::set_workspace_root(const std::string& path, std::string* error)
{
    try {
        if (path.empty()) {
            if (error) *error = "Select a folder before setting the workspace root";
            return false;
        }
        if (path.size() > 32768) {
            if (error) *error = "The selected workspace-root path exceeds the supported bound";
            return false;
        }
        const std::string normalized = explorer_path_to_utf8(
            explorer_path_from_utf8(path).lexically_normal());
        if (normalized.empty()) {
            if (error) *error = "The selected workspace-root path is invalid";
            return false;
        }
        const std::vector<std::string> previous_roots = roots;
        const std::string previous_setting = g_sa_settings.workspace.root_path;
        selected_paths.clear();
        selection_anchor_path.clear();
        selection_error.clear();
        selected_idx = -1;
        ++selection_revision;
        pending_reveal_path.clear();
        refresh(normalized);
        g_sa_settings.workspace.root_path = normalized;
        const auto requested = aida::settings_persistence::request_save(g_sa_settings);
        if (!aida::settings_persistence::accepted(requested)) {
            g_sa_settings.workspace.root_path = previous_setting;
            set_roots(previous_roots);
            if (error) *error = "Settings persistence rejected the workspace-root transaction; the previous roots were restored";
            return false;
        }
        if (error) error->clear();
        return true;
    } catch (const std::exception&) {
        if (error) *error = "The selected workspace-root path is not valid UTF-8 or cannot be represented natively";
        return false;
    } catch (...) {
        if (error) *error = "The selected workspace-root path could not be converted safely";
        return false;
    }
}

bool file_browser::binary_analysis_candidate(const std::string& path)
{
    if (path.empty()) return false;
    std::string extension;
    try {
        extension = explorer_path_to_utf8(explorer_path_from_utf8(path).extension());
    } catch (...) {
        return false;
    }
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    static constexpr const char* extensions[] = {
        ".exe", ".dll", ".sys", ".efi", ".scr", ".cpl", ".ocx", ".ax",
        ".drv", ".mui", ".tsp", ".node", ".bin", ".lib", ".obj", ".o",
        ".a", ".so", ".dylib", ".elf", ".out", ".com", ".ko", ".kext",
        ".dmp", ".pdb", ".rom", ".img", ".uefi", ".class", ".jar", ".pyc",
        ".pyo", ".rar", ".zip", ".7z", ".tar", ".gz", ".bz2", ".xz",
        ".cab", ".iso", ".apk", ".ipa"
    };
    return std::find(std::begin(extensions), std::end(extensions), extension) !=
        std::end(extensions);
}

void file_browser::cancel_refresh()
{
    auto& control = explorer_index_control();
    if (control.cancelled)
        control.cancelled->store(true, std::memory_order_release);
    if (control.task_id != 0)
        aida::infra::executor::cancel(control.task_id);
    needs_refresh = false;
    if (index_state == index_state_t::loading)
        index_state = index_state_t::cancelled;
}


void file_browser::toggle_dir(int idx)
{
    if (idx < 0 || static_cast<std::size_t>(idx) >= entries.size()) return;
    auto& ent = entries[static_cast<std::size_t>(idx)];
    if (!ent.is_dir) return;

    ent.expanded = !ent.expanded;
    const std::string key = normalized_explorer_path(ent.full_path);
    if (ent.expanded)
        expanded_paths.insert(key);
    else
        expanded_paths.erase(key);
    needs_refresh = true;
}


namespace file_browser {


void open_path(const std::string& path)
{
    aida::qt::explorer::open_path(path);
}

}

void file_browser::open_file(int idx)
{
    if (idx < 0 || static_cast<std::size_t>(idx) >= entries.size()) return;
    auto& ent = entries[static_cast<std::size_t>(idx)];
    if (ent.is_dir) return;
    file_browser::request_open_confirmation(ent.full_path);
}

namespace file_browser {


void request_open_confirmation(const std::string& path)
{
    if (path.empty()) return;
    if (!aida::ui_thread::is_owner_thread()) {
        const bool routed = aida::ui_thread::post([path]() {
            file_browser::request_open_confirmation(path);
        }, "file_browser", "request_open_confirmation", "entry");
        if (!routed)
            diag::log_tagged_fmt("file_open", "explorer_confirm_denied path=%s reason=ui_affinity_route_failed", path.c_str());
        return;
    }
    if (!aida::ui_thread::require_owner("file_browser", "request_open_confirmation", "entry"))
        return;

    std::error_code ec;
    if (fs::is_directory(path, ec) && !ec) {
        open_path(path);
        return;
    }

    pending_open_path = path;
    size_t sl = path.find_last_of("/\\");
    pending_open_filename = (sl != std::string::npos) ? path.substr(sl + 1) : path;
    pending_open_should_open = true;
    pending_open_modal_visible = true;
    diag::log_tagged_fmt("file_open", "explorer_confirm_requested path=%s", path.c_str());
}

void record_recent_workspace(const std::string& path)
{
    if (path.empty()) return;

    std::vector<std::string> list;
    if (!g_sa_settings.recent_workspaces_json.empty()) {
        auto j = nlohmann::json::parse(g_sa_settings.recent_workspaces_json,
                                       nullptr, false);
        if (!j.is_discarded() && j.is_array()) {
            for (auto& el : j) {
                if (el.is_string()) {
                    list.push_back(el.get<std::string>());
                }
            }
        }
    }

    auto same_path = [&](const std::string& a, const std::string& b) -> bool {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i];
            char cb = b[i];
            if (ca == '/') ca = '\\';
            if (cb == '/') cb = '\\';
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return true;
    };

    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const std::string& s) { return same_path(s, path); }),
               list.end());
    list.insert(list.begin(), path);
    if (list.size() > 20) list.resize(20);

    nlohmann::json out = nlohmann::json::array();
    for (auto& s : list) out.push_back(s);
    g_sa_settings.recent_workspaces_json = out.dump();
}


namespace watcher_detail {

struct watcher_t {
    std::atomic<bool>       running{false};
    std::atomic<bool>       stop{false};
    std::atomic<bool>       has_change{false};
    std::atomic<bool>       worker_done{true};
    std::atomic<uint64_t>   retry_after_ms{0};
    std::string             watched_dir;
    HANDLE                  wake_event = nullptr;
    std::mutex              mtx;
};

struct watcher_manager_t {
    std::unordered_map<std::string, std::shared_ptr<watcher_t>> active;
    std::vector<std::shared_ptr<watcher_t>> retiring;
};

inline watcher_manager_t& g_watchers()
{
    static watcher_manager_t value;
    return value;
}

inline uint64_t now_ms()
{
    return ::GetTickCount64();
}

inline bool utf8_to_wide(const std::string& in, std::wstring& out)
{
    out.clear();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, in.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    if (::MultiByteToWideChar(CP_UTF8, 0, in.c_str(), -1, out.data(), n) <= 0) {
        out.clear();
        return false;
    }
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return true;
}

inline void close_completed_wake_event_locked(watcher_t& w)
{
    if (w.wake_event && w.worker_done.load(std::memory_order_acquire)) {
        ::CloseHandle(w.wake_event);
        w.wake_event = nullptr;
    }
}

inline void request_stop(watcher_t& w)
{
    if (!w.running.load(std::memory_order_acquire)) return;
    w.stop.store(true, std::memory_order_release);
    if (w.wake_event) ::SetEvent(w.wake_event);
}

inline bool is_noise_basename(const std::wstring& bn)
{
    if (bn.empty()) return true;
    if (bn.size() >= 14) {
        const wchar_t* tail = bn.c_str() + bn.size() - 14;
        if (_wcsicmp(tail, L"aida_debug.log") == 0) return true;
    }
    if (bn.size() >= 4) {
        const wchar_t* ext = bn.c_str() + bn.size() - 4;
        if (_wcsicmp(ext, L".log") == 0) return true;
        if (_wcsicmp(ext, L".tmp") == 0) return true;
    }
    if (bn.size() >= 1 && bn[0] == L'.') return true;
    return false;
}

inline void watcher_thread(std::shared_ptr<watcher_t> watcher, std::string dir, HANDLE wake_event)
{
    watcher_t& w = *watcher;

    std::wstring wdir;
    if (!utf8_to_wide(dir, wdir) || wdir.empty()) {
        diag::log_tagged("file_browser_watcher", "thread_exit empty_dir");
        w.retry_after_ms.store(now_ms() + 5000ull, std::memory_order_release);
        w.worker_done.store(true, std::memory_order_release);
        w.running.store(false, std::memory_order_release);
        return;
    }

    HANDLE h = ::CreateFileW(wdir.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        w.retry_after_ms.store(now_ms() + 5000ull, std::memory_order_release);
        diag::log_tagged_fmt("file_browser_watcher",
            "thread_exit CreateFileW failed err=%lu dir=%s",
            static_cast<unsigned long>(err), dir.c_str());
        w.worker_done.store(true, std::memory_order_release);
        w.running.store(false, std::memory_order_release);
        return;
    }

    diag::log_tagged_fmt("file_browser_watcher",
        "thread_started dir=%s", dir.c_str());

    constexpr DWORD kBufSize = 32768;
    std::vector<uint8_t> buf(kBufSize);

    uint64_t last_signal_ms = 0;

    while (!w.stop.load(std::memory_order_acquire)) {
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) break;

        DWORD bytes_returned = 0;
        BOOL ok = ::ReadDirectoryChangesW(
            h,
            buf.data(),
            kBufSize,
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_SIZE,
            &bytes_returned,
            &ov,
            nullptr);
        if (!ok) {
            DWORD err = ::GetLastError();
            w.retry_after_ms.store(now_ms() + 5000ull, std::memory_order_release);
            diag::log_tagged_fmt("file_browser_watcher",
                "ReadDirectoryChangesW failed err=%lu",
                static_cast<unsigned long>(err));
            ::CloseHandle(ov.hEvent);
            break;
        }

        HANDLE waits[2] = { ov.hEvent, wake_event };
        DWORD wait_count = wake_event ? 2u : 1u;
        DWORD waited = ::WaitForMultipleObjects(wait_count, waits, FALSE, INFINITE);

        if (waited == WAIT_OBJECT_0) {
            DWORD transferred = 0;
            if (::GetOverlappedResult(h, &ov, &transferred, FALSE) && transferred > 0) {
                bool has_meaningful = false;
                DWORD off = 0;
                const uint8_t* p = buf.data();
                while (off + sizeof(FILE_NOTIFY_INFORMATION) <= transferred) {
                    const FILE_NOTIFY_INFORMATION* fni =
                        reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p + off);
                    USHORT name_chars = static_cast<USHORT>(fni->FileNameLength / sizeof(WCHAR));
                    std::wstring bn(fni->FileName, name_chars);
                    if (!is_noise_basename(bn)) {
                        has_meaningful = true;
                        break;
                    }
                    if (fni->NextEntryOffset == 0) break;
                    off += fni->NextEntryOffset;
                }

                if (has_meaningful) {
                    uint64_t stamp_ms = now_ms();
                    if (stamp_ms - last_signal_ms >= 500ull) {
                        last_signal_ms = stamp_ms;
                        w.has_change.store(true, std::memory_order_release);
                    }
                }
            }
        } else {
            ::CancelIoEx(h, &ov);
            DWORD tmp = 0;
            ::GetOverlappedResult(h, &ov, &tmp, TRUE);
        }
        ::CloseHandle(ov.hEvent);
    }

    ::CloseHandle(h);
    diag::log_tagged_fmt("file_browser_watcher",
        "thread_exit dir=%s", dir.c_str());
    w.worker_done.store(true, std::memory_order_release);
    w.running.store(false, std::memory_order_release);
}

inline void start_watcher(const std::shared_ptr<watcher_t>& watcher)
{
    watcher_t& w = *watcher;
    std::lock_guard<std::mutex> lk(w.mtx);
    const uint64_t stamp_ms = now_ms();
    if (w.running.load(std::memory_order_acquire)) return;
    const uint64_t retry_after_ms = w.retry_after_ms.load(std::memory_order_acquire);
    if (retry_after_ms != 0 && stamp_ms < retry_after_ms) return;
    close_completed_wake_event_locked(w);
    w.wake_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!w.wake_event) return;
    w.stop.store(false, std::memory_order_release);
    w.has_change.store(false, std::memory_order_release);
    HANDLE we = w.wake_event;
    const std::string cap = w.watched_dir;
    w.worker_done.store(false, std::memory_order_release);
    w.running.store(true, std::memory_order_release);
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "file_browser_watcher";
    sub.label = "file_browser_watcher.watch";
    sub.thread_class = "long_lived_service";
    sub.domain = aida::infra::executor::domain_t::service;
    sub.priority = 3;
    sub.cancel_hook = [watcher]() {
        request_stop(*watcher);
    };
    sub.body = [watcher, cap, we]() { watcher_thread(watcher, cap, we); };
    if (aida::infra::executor::submit(std::move(sub)).submitted) {
        w.retry_after_ms.store(0, std::memory_order_release);
        diag::log_tagged_fmt("file_browser_watcher", "ensure_running_for dir=%s", cap.c_str());
    }
    else {
        if (w.wake_event) {
            ::CloseHandle(w.wake_event);
            w.wake_event = nullptr;
        }
        w.stop.store(false, std::memory_order_release);
        w.running.store(false, std::memory_order_release);
        w.worker_done.store(true, std::memory_order_release);
        w.retry_after_ms.store(stamp_ms + 5000ull, std::memory_order_release);
        diag::log_tagged_fmt("file_browser_watcher", "executor_submit_failed dir=%s",
            cap.c_str());
    }
}

inline void ensure_running_for(const std::vector<std::string>& roots)
{
    auto& manager = g_watchers();
    std::unordered_set<std::string> requested;
    for (const auto& root : roots) {
        if (requested.size() >= 8) break;
        if (!root.empty()) requested.insert(normalized_explorer_path(root));
    }

    for (auto iterator = manager.active.begin(); iterator != manager.active.end();) {
        if (requested.find(iterator->first) != requested.end()) {
            ++iterator;
            continue;
        }
        request_stop(*iterator->second);
        manager.retiring.push_back(iterator->second);
        iterator = manager.active.erase(iterator);
    }
    for (const auto& root : roots) {
        if (root.empty()) continue;
        const std::string key = normalized_explorer_path(root);
        if (requested.find(key) == requested.end()) continue;
        auto found = manager.active.find(key);
        if (found == manager.active.end()) {
            auto watcher = std::make_shared<watcher_t>();
            watcher->watched_dir = root;
            found = manager.active.emplace(key, std::move(watcher)).first;
        }
        start_watcher(found->second);
    }
    for (auto iterator = manager.retiring.begin(); iterator != manager.retiring.end();) {
        auto& watcher = **iterator;
        if (!watcher.worker_done.load(std::memory_order_acquire)) {
            ++iterator;
            continue;
        }
        std::lock_guard<std::mutex> lock(watcher.mtx);
        close_completed_wake_event_locked(watcher);
        iterator = manager.retiring.erase(iterator);
    }
}

inline bool consume_change()
{
    auto& manager = g_watchers();
    bool changed = false;
    for (const auto& item : manager.active)
        changed = item.second->has_change.exchange(false, std::memory_order_acq_rel) || changed;
    return changed;
}

}


void tick_watcher()
{
    watcher_detail::ensure_running_for(roots.empty()
        ? std::vector<std::string>{current_dir} : roots);
    if (watcher_detail::consume_change()) {
        needs_refresh = true;
    }
}

}
