#include "qt/layout/workspace_operations.hpp"

#include "core/infra/executor.hpp"
#include "helpers/diag_log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <exception>
#include <system_error>
#include <utility>

namespace aida::qt::layout {

namespace {

operation_task_hooks_t& task_hooks() noexcept {
    static operation_task_hooks_t value;
    return value;
}

std::function<void(std::shared_ptr<operation_result_t>)>& completion_sink() noexcept {
    static std::function<void(std::shared_ptr<operation_result_t>)> value;
    return value;
}

void task_update(const std::string& task_id, operation_task_state_t state, float progress,
                 const std::string& stage, const std::string& result_summary = {}) {
    if (task_hooks().update_task)
        task_hooks().update_task(task_id, state, progress, stage, result_summary);
}

std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>>& catalog_publication() noexcept {
    static std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>> value =
        std::make_shared<const std::vector<docking::user_workspace_descriptor_t>>();
    return value;
}

std::atomic<std::uint64_t>& catalog_epoch_storage() noexcept {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

std::atomic<std::uint64_t>& published_catalog_epoch() noexcept {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

std::atomic<bool>& catalog_ready_storage() noexcept {
    static std::atomic<bool> value{false};
    return value;
}

std::mutex& catalog_publication_mutex() noexcept {
    static std::mutex value;
    return value;
}

std::string& submit_error_storage() noexcept {
    static std::string value;
    return value;
}

}

operation_runtime_t& operation_runtime() noexcept {
    static operation_runtime_t value;
    return value;
}

std::uint64_t next_catalog_epoch() noexcept {
    return catalog_epoch_storage().fetch_add(1, std::memory_order_acq_rel) + 1ULL;
}

void publish_catalog(
    std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>> catalog,
    std::uint64_t epoch) noexcept {
    if (!catalog)
        return;
    std::lock_guard<std::mutex> lock(catalog_publication_mutex());
    if (epoch < published_catalog_epoch().load(std::memory_order_acquire))
        return;
    std::atomic_store_explicit(&catalog_publication(), std::move(catalog),
        std::memory_order_release);
    published_catalog_epoch().store(epoch, std::memory_order_release);
    catalog_ready_storage().store(true, std::memory_order_release);
}

std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>> catalog_snapshot() noexcept {
    return std::atomic_load_explicit(&catalog_publication(), std::memory_order_acquire);
}

bool catalog_ready() noexcept {
    return catalog_ready_storage().load(std::memory_order_acquire);
}

void reset_catalog() noexcept {
    const std::uint64_t reset_epoch = next_catalog_epoch();
    publish_catalog(std::make_shared<const std::vector<docking::user_workspace_descriptor_t>>(),
        reset_epoch);
    catalog_ready_storage().store(false, std::memory_order_release);
}

void set_operation_task_hooks(operation_task_hooks_t hooks) {
    task_hooks() = std::move(hooks);
}

void set_operation_completion_sink(
    std::function<void(std::shared_ptr<operation_result_t> result)> sink) {
    completion_sink() = std::move(sink);
}

const std::string& last_submit_error() noexcept {
    static const std::string empty;
    return submit_error_storage().empty() ? empty : submit_error_storage();
}

void execute_operation(const operation_request_t& request, operation_result_t& result) noexcept {
    result.kind = request.kind;
    result.serial = request.serial;
    result.source_generation = request.source_generation;
    result.catalog_epoch = request.catalog_epoch;
    result.target_preset = request.target_preset;
    result.target_locked = request.target_locked;
    result.source_user_name = request.source_user_name;
    result.target_user_name = request.target_user_name;
    result.task_id = request.task_id;
    const auto fail = [&result](std::string message) {
        result.success = false;
        result.error = std::move(message);
    };
    try {
        if (request.kind == operation_kind_t::set_lock ||
            request.kind == operation_kind_t::switch_preset ||
            request.kind == operation_kind_t::save_user ||
            request.kind == operation_kind_t::load_user) {
            if (!request.current_payload) {
                fail("The immutable current layout snapshot is unavailable.");
                return;
            }
            const docking::workspace_preset_t saved_preset = request.current_preset;
            if (!write_generation(request.current_paths, saved_preset, request.target_locked,
                    request.save_generation, false, request.skip_backup, request.environment,
                    request.registry_fingerprint, request.current_payload->dock_xml,
                    request.current_payload->surface_json)) {
                fail("The current layout could not be written atomically.");
                return;
            }
            result.saved_generation = request.save_generation;
            if (request.kind == operation_kind_t::save_user) {
                const DWORD attributes = GetFileAttributesW(request.target_paths.primary.c_str());
                if (!request.overwrite && attributes != INVALID_FILE_ATTRIBUTES) {
                    fail("A named user workspace with this exact name already exists.");
                    return;
                }
                const std::uint64_t named_generation = request.save_generation + 1ULL;
                if (!write_generation(request.target_paths, saved_preset, request.target_locked,
                        named_generation, false, false, request.environment,
                        request.registry_fingerprint, request.current_payload->dock_xml,
                        request.current_payload->surface_json)) {
                    fail("The named user layout could not be written atomically.");
                    return;
                }
                result.saved_generation = named_generation;
                result.active_user_name = request.target_user_name;
                if (!write_active_record(request.workspace_directory,
                        request.active_record, saved_preset, request.target_locked,
                        request.target_user_name)) {
                    fail("The active named workspace record could not be replaced atomically.");
                    return;
                }
            }
        }

        if (request.kind == operation_kind_t::switch_preset ||
            request.kind == operation_kind_t::load_user) {
            const read_result_t loaded = read_layout_with_backup(request.target_paths,
                result.metadata, result.payloads);
            if (request.kind == operation_kind_t::load_user &&
                loaded == read_result_t::valid && request.expected_user_generation != 0 &&
                result.metadata.generation != request.expected_user_generation) {
                fail("The named user workspace changed after it was selected.");
                return;
            }
            if (loaded == read_result_t::valid &&
                request.kind == operation_kind_t::switch_preset &&
                result.metadata.preset != request.target_preset) {
                fail("The saved layout belongs to a different workspace preset.");
                return;
            }
            if (loaded != read_result_t::valid) {
                if (request.kind == operation_kind_t::load_user) {
                    fail(loaded == read_result_t::absent
                        ? "The named user layout does not exist."
                        : "The named user layout is invalid or unreadable.");
                    return;
                }
                result.use_default = true;
                result.payloads = {};
                result.metadata = {};
                result.metadata.preset = request.target_preset;
                result.metadata.locked = false;
            } else {
                result.target_preset = result.metadata.preset;
                result.target_locked = result.metadata.locked;
                if (result.metadata.payload_kind == payload_kind_t::imgui_ini) {
                    result.use_default = true;
                    result.payloads = {};
                }
            }
            if (!write_active_record(request.workspace_directory,
                    request.active_record, result.target_preset, result.target_locked,
                    request.kind == operation_kind_t::load_user
                        ? std::string_view(request.target_user_name) : std::string_view{})) {
                fail("The active workspace record could not be replaced atomically.");
                return;
            }
            result.active_user_name = request.kind == operation_kind_t::load_user
                ? request.target_user_name : std::string{};
        } else if (request.kind == operation_kind_t::set_lock) {
            if (!write_active_record(request.workspace_directory,
                    request.active_record, request.current_preset, request.target_locked,
                    request.current_user_name)) {
                fail("The active workspace lock record could not be replaced atomically.");
                return;
            }
            result.active_user_name = request.current_user_name;
        } else if (request.kind == operation_kind_t::rename_user) {
            if (GetFileAttributesW(request.source_user_paths.primary.c_str()) == INVALID_FILE_ATTRIBUTES) {
                fail("The named user workspace no longer exists.");
                return;
            }
            if (GetFileAttributesW(request.target_paths.primary.c_str()) != INVALID_FILE_ATTRIBUTES) {
                fail("A named user workspace with the new exact name already exists.");
                return;
            }
            record_metadata_t source_metadata;
            if (!inspect_layout_file(request.source_user_paths.primary, source_metadata) ||
                source_metadata.generation != request.expected_user_generation) {
                fail("The named user workspace changed after it was selected.");
                return;
            }
            if (!MoveFileExW(request.source_user_paths.primary.c_str(),
                    request.target_paths.primary.c_str(), MOVEFILE_WRITE_THROUGH)) {
                fail("The named user workspace could not be renamed atomically.");
                return;
            }
            const auto move_optional = [](const std::filesystem::path& source,
                const std::filesystem::path& target, bool& moved) noexcept {
                moved = false;
                if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    const DWORD error = GetLastError();
                    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
                }
                moved = MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
                return moved;
            };
            bool backup_moved = false;
            bool invalid_moved = false;
            if (!move_optional(request.source_user_paths.backup, request.target_paths.backup,
                    backup_moved) ||
                !move_optional(request.source_user_paths.invalid, request.target_paths.invalid,
                    invalid_moved)) {
                if (invalid_moved)
                    static_cast<void>(MoveFileExW(request.target_paths.invalid.c_str(),
                        request.source_user_paths.invalid.c_str(), MOVEFILE_WRITE_THROUGH));
                if (backup_moved)
                    static_cast<void>(MoveFileExW(request.target_paths.backup.c_str(),
                        request.source_user_paths.backup.c_str(), MOVEFILE_WRITE_THROUGH));
                static_cast<void>(MoveFileExW(request.target_paths.primary.c_str(),
                    request.source_user_paths.primary.c_str(), MOVEFILE_WRITE_THROUGH));
                fail("The named user workspace companion files could not be renamed exactly.");
                return;
            }
            result.active_user_name = request.current_user_name == request.source_user_name
                ? request.target_user_name : request.current_user_name;
            if (!write_active_record(request.workspace_directory,
                    request.active_record, request.current_preset, request.target_locked,
                    result.active_user_name)) {
                static_cast<void>(MoveFileExW(request.target_paths.primary.c_str(),
                    request.source_user_paths.primary.c_str(), MOVEFILE_WRITE_THROUGH));
                if (invalid_moved)
                    static_cast<void>(MoveFileExW(request.target_paths.invalid.c_str(),
                        request.source_user_paths.invalid.c_str(), MOVEFILE_WRITE_THROUGH));
                if (backup_moved)
                    static_cast<void>(MoveFileExW(request.target_paths.backup.c_str(),
                        request.source_user_paths.backup.c_str(), MOVEFILE_WRITE_THROUGH));
                fail("The active workspace record could not follow the rename.");
                return;
            }
        } else if (request.kind == operation_kind_t::delete_user) {
            record_metadata_t target_metadata;
            if (!inspect_layout_file(request.target_paths.primary, target_metadata) ||
                target_metadata.generation != request.expected_user_generation) {
                fail("The named user workspace changed after it was selected.");
                return;
            }
            const bool deleting_active = request.current_user_name == request.target_user_name;
            result.active_user_name = deleting_active ? std::string{} : request.current_user_name;
            if (deleting_active) {
                const read_result_t fallback = read_layout_with_backup(request.fallback_paths,
                    result.metadata, result.payloads);
                if (fallback == read_result_t::io_failure) {
                    fail("The built-in fallback workspace could not be read safely.");
                    return;
                }
                if (fallback == read_result_t::valid &&
                    result.metadata.preset != request.target_preset) {
                    fail("The built-in fallback belongs to a different workspace preset.");
                    return;
                }
                result.apply_layout = true;
                result.target_preset = request.target_preset;
                result.use_default = fallback != read_result_t::valid ||
                    result.metadata.payload_kind == payload_kind_t::imgui_ini;
                result.target_locked = fallback == read_result_t::valid
                    ? result.metadata.locked : false;
                if (result.use_default) {
                    result.payloads = {};
                    result.metadata = {};
                    result.metadata.preset = request.target_preset;
                    result.metadata.locked = false;
                }
            }
            if (!write_active_record(request.workspace_directory,
                    request.active_record, deleting_active ? request.target_preset : request.current_preset,
                    deleting_active ? result.target_locked : request.target_locked,
                    result.active_user_name)) {
                fail("The active workspace record could not be prepared for deletion.");
                return;
            }
            if (!remove_file_exact(request.target_paths.primary) ||
                !remove_file_exact(request.target_paths.backup) ||
                !remove_file_exact(request.target_paths.invalid)) {
                static_cast<void>(write_active_record(request.workspace_directory,
                    request.active_record, request.current_preset, request.target_locked,
                    request.current_user_name));
                fail("The named user workspace could not be removed exactly.");
                return;
            }
        } else if (request.kind == operation_kind_t::restore_preset ||
                   request.kind == operation_kind_t::reset_all) {
            std::vector<std::filesystem::path> user_files;
            if (request.kind == operation_kind_t::reset_all) {
                std::error_code error;
                if (std::filesystem::exists(request.user_directory, error)) {
                    std::filesystem::directory_iterator cursor(request.user_directory, error);
                    const std::filesystem::directory_iterator end;
                    while (!error && cursor != end) {
                        const bool regular = cursor->is_regular_file(error);
                        if (error || !regular) {
                            fail("The named user workspace set is invalid or exceeds its exact bound.");
                            return;
                        }
                        if (managed_user_layout_artifact(cursor->path())) {
                            if (user_files.size() >= k_maximum_named_user_layouts * 3U) {
                                fail("The named user workspace set exceeds its exact bound.");
                                return;
                            }
                            user_files.push_back(cursor->path());
                        }
                        cursor.increment(error);
                    }
                }
                if (error) {
                    fail("The user workspace directory could not be enumerated exactly.");
                    return;
                }
            }
            for (const auto& paths : request.reset_paths) {
                if (!remove_file_exact(paths.primary) || !remove_file_exact(paths.backup) ||
                    !remove_file_exact(paths.invalid)) {
                    fail("A saved workspace layout could not be removed exactly.");
                    return;
                }
            }
            if (request.kind == operation_kind_t::reset_all) {
                for (const auto& path : user_files) {
                    if (!remove_file_exact(path)) {
                        fail("A saved user workspace could not be removed exactly.");
                        return;
                    }
                }
            }
            if (!write_active_record(request.workspace_directory,
                    request.active_record, request.target_preset, false)) {
                fail("The reset workspace record could not be replaced atomically.");
                return;
            }
            result.use_default = true;
            result.target_locked = false;
            result.active_user_name.clear();
        }
        result.catalog = scan_user_catalog(request.workspace_directory / L"user",
            result.active_user_name);
        if (!result.catalog) {
            fail("The named user workspace catalog could not be refreshed exactly.");
            return;
        }
        result.success = true;
    } catch (const std::exception& exception) {
        fail(exception.what());
    } catch (...) {
        fail("The workspace operation failed with an unknown error.");
    }
}

docking::workspace_request_result_t submit_operation(operation_request_t request) noexcept {
    if (!valid_registry_fingerprint(request.registry_fingerprint)) {
        submit_error_storage() = "The view registry fingerprint is unavailable.";
        return docking::workspace_request_result_t::failed;
    }
    operation_runtime_t& runtime = operation_runtime();
    bool expected = false;
    if (!runtime.pending.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel))
        return docking::workspace_request_result_t::busy;
    if (request.catalog_epoch == 0)
        request.catalog_epoch = next_catalog_epoch();
    request.serial = runtime.serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    request.task_id = "workspace.layout." + std::to_string(request.serial);
    auto immutable_request = std::make_shared<const operation_request_t>(std::move(request));
    {
        std::lock_guard<std::mutex> lock(runtime.result_mutex);
        runtime.active_request = immutable_request;
    }
    if (task_hooks().register_task) {
        operation_task_registration_t registration;
        registration.id = immutable_request->task_id;
        registration.source = "workspace_layout";
        registration.owner = "Workspace Layout";
        registration.owner_view = "view.background_tasks";
        registration.owner_action = "Apply workspace transaction";
        registration.target = std::string(
            docking::preset_descriptor(immutable_request->target_preset).display_name);
        registration.label = "Workspace layout transaction";
        registration.stage = "Queued serialized persistence phase";
        registration.affected_entity = std::string(
            docking::preset_descriptor(immutable_request->target_preset).stable_id);
        registration.retry = [serial = immutable_request->serial] {
            operation_runtime_t& current = operation_runtime();
            std::lock_guard<std::mutex> lock(current.result_mutex);
            if (!current.last_failed_request || current.last_failed_request->serial != serial ||
                current.pending.load(std::memory_order_acquire))
                return false;
            current.retry_requested.store(serial, std::memory_order_release);
            return true;
        };
        if (!task_hooks().register_task(std::move(registration))) {
            runtime.pending.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(runtime.result_mutex);
                runtime.active_request.reset();
            }
            submit_error_storage() = "Task Center rejected the workspace transaction.";
            return docking::workspace_request_result_t::failed;
        }
    }
    auto result = std::make_shared<operation_result_t>();
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "workspace_layout";
    submission.label = "workspace_layout.transaction";
    submission.thread_class = "diagnostics_io";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 2;
    submission.generation = immutable_request->serial;
    submission.ui_access_policy = "none";
    submission.failure_policy = "retain_last_known_good";
    submission.shutdown_policy = "drain";
    submission.body = [immutable_request, result] {
        task_update(immutable_request->task_id, operation_task_state_t::running, 0.1f,
            "Executing atomic persistence phase");
        execute_operation(*immutable_request, *result);
        if (result->success) {
            task_update(result->task_id, operation_task_state_t::running, 0.9f,
                "Persistence complete; awaiting UI layout application");
        } else {
            task_update(result->task_id, operation_task_state_t::failed, 1.0f,
                "Workspace transaction failed", result->error);
        }
        operation_runtime_t& current = operation_runtime();
        {
            std::lock_guard<std::mutex> lock(current.result_mutex);
            current.result = result;
            if (!result->success)
                current.last_failed_request = immutable_request;
        }
        if (completion_sink())
            completion_sink()(result);
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        runtime.pending.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(runtime.result_mutex);
            runtime.last_failed_request = immutable_request;
            runtime.active_request.reset();
        }
        submit_error_storage() = "Workspace executor rejected the transaction: " +
            submitted.reject_reason;
        task_update(immutable_request->task_id, operation_task_state_t::failed, 1.0f,
            "Workspace executor rejected the transaction", submitted.reject_reason);
        return docking::workspace_request_result_t::failed;
    }
    submit_error_storage().clear();
    return docking::workspace_request_result_t::queued;
}

std::shared_ptr<operation_result_t> take_operation_result() noexcept {
    operation_runtime_t& runtime = operation_runtime();
    std::lock_guard<std::mutex> lock(runtime.result_mutex);
    return std::move(runtime.result);
}

void process_operation_retry() noexcept {
    operation_runtime_t& runtime = operation_runtime();
    const std::uint64_t retry_serial = runtime.retry_requested.exchange(0,
        std::memory_order_acq_rel);
    if (retry_serial == 0 || runtime.pending.load(std::memory_order_acquire))
        return;
    std::shared_ptr<const operation_request_t> failed;
    {
        std::lock_guard<std::mutex> lock(runtime.result_mutex);
        if (runtime.last_failed_request && runtime.last_failed_request->serial == retry_serial)
            failed = runtime.last_failed_request;
    }
    if (!failed)
        return;
    operation_request_t retry = *failed;
    retry.serial = 0;
    retry.catalog_epoch = 0;
    retry.task_id.clear();
    static_cast<void>(submit_operation(std::move(retry)));
}

bool queue_layout_write(const layout_paths_t& paths, docking::workspace_preset_t preset,
                        bool locked, std::uint64_t generation, bool skip_backup,
                        const layout_environment_t& environment,
                        std::string_view registry_fingerprint,
                        std::string_view payload, std::string_view surface) {
    if (payload.empty() || payload.size() > k_maximum_payload_bytes || generation == 0 ||
        surface.size() > k_maximum_surface_bytes ||
        !valid_registry_fingerprint(registry_fingerprint) ||
        payload.find("<QtAdvancedDockingSystem") == std::string_view::npos)
        return false;
    auto immutable_payload = std::make_shared<const std::string>(payload);
    auto immutable_surface = std::make_shared<const std::string>(surface);
    std::string immutable_fingerprint(registry_fingerprint);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "workspace_layout";
    submission.label = "workspace_layout_atomic_save";
    submission.thread_class = "diagnostics_io";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 1;
    submission.generation = generation;
    submission.diagnostic_id = "workspace_layout.save";
    submission.ui_access_policy = "none";
    submission.failure_policy = "retain_last_known_good";
    submission.shutdown_policy = "drain";
    submission.body = [paths, preset, locked, immutable_payload, immutable_surface, generation,
        skip_backup, environment, registry_fingerprint = std::move(immutable_fingerprint)]() {
        write_generation(paths, preset, locked, generation, false, skip_backup, environment,
            registry_fingerprint, *immutable_payload, *immutable_surface);
    };
    return aida::infra::executor::submit(std::move(submission)).submitted;
}

}
