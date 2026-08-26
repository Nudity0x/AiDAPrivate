#include "qt/layout/workspace_persistence.hpp"

#include "qt/docking/dock_host.hpp"
#include "qt/registry/qt_view_registry.hpp"
#include "qt/registry/view_visibility.hpp"

#include "core/infra/executor.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/settings/settings_persistence_service.hpp"
#include "core/ui/task_center.hpp"
#include "helpers/diag_log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <utility>

namespace aida::qt::layout {

namespace {

constexpr int k_dirty_coalesce_ms = 500;
constexpr std::uint64_t k_write_retry_backoff_ms = 5000ULL;

}

WorkspacePersistenceController::WorkspacePersistenceController(
    docking::AidaDockHost* host, registry::qt_view_registry_t* registry, QObject* parent)
    : QObject(parent), host_(host), registry_(registry) {
    dirty_timer_ = new QTimer(this);
    dirty_timer_->setSingleShot(true);
    dirty_timer_->setInterval(k_dirty_coalesce_ms);
    connect(dirty_timer_, &QTimer::timeout, this, &WorkspacePersistenceController::flush_dirty);
    if (host_) {
        connect(host_, &docking::AidaDockHost::layoutDirty,
                this, &WorkspacePersistenceController::mark_dirty);
        visibility_ = host_->visibility();
    }

    operation_task_hooks_t hooks;
    QPointer<WorkspacePersistenceController> guard(this);
    hooks.register_task = [guard](operation_task_registration_t registration) {
        aida::ui::task_center::task_registration_t task;
        task.id = std::move(registration.id);
        task.source = std::move(registration.source);
        task.owner = std::move(registration.owner);
        task.owner_view = std::move(registration.owner_view);
        task.owner_action = std::move(registration.owner_action);
        task.target = std::move(registration.target);
        task.label = std::move(registration.label);
        task.stage = std::move(registration.stage);
        task.affected_entity = std::move(registration.affected_entity);
        task.callbacks.retry = [guard, raw_retry = std::move(registration.retry)]() mutable {
            const bool accepted = raw_retry ? raw_retry() : false;
            if (accepted && guard) {
                const bool posted = QMetaObject::invokeMethod(guard, [] {
                    process_operation_retry();
                }, Qt::QueuedConnection);
                if (!posted)
                    diag::log_tagged("workspace_layout",
                        "operation_retry_delivery_failed context_destroyed=1");
            }
            return accepted;
        };
        return aida::ui::task_center::register_task(std::move(task));
    };
    hooks.update_task = [](const std::string& task_id, operation_task_state_t state,
                           float progress, const std::string& stage,
                           const std::string& result_summary) {
        aida::ui::task_center::task_state_t mapped = aida::ui::task_center::task_state_t::queued;
        switch (state) {
        case operation_task_state_t::running:
            mapped = aida::ui::task_center::task_state_t::running;
            break;
        case operation_task_state_t::completed:
            mapped = aida::ui::task_center::task_state_t::completed;
            break;
        case operation_task_state_t::failed:
            mapped = aida::ui::task_center::task_state_t::failed;
            break;
        case operation_task_state_t::cancelled:
            mapped = aida::ui::task_center::task_state_t::cancelled;
            break;
        }
        static_cast<void>(aida::ui::task_center::update_task(task_id, mapped, progress,
            stage, result_summary));
    };
    set_operation_task_hooks(std::move(hooks));
    set_operation_completion_sink([guard](std::shared_ptr<operation_result_t>) {
        const bool posted = QMetaObject::invokeMethod(guard, [guard] {
            if (guard)
                guard->settle_pending_operation_for_shutdown();
        }, Qt::QueuedConnection);
        if (!posted)
            diag::log_tagged("workspace_layout",
                "operation_completion_delivery_failed context_destroyed=1");
    });

    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, &WorkspacePersistenceController::shutdown, Qt::DirectConnection);
    }
}

WorkspacePersistenceController::~WorkspacePersistenceController() = default;

std::string_view WorkspacePersistenceController::active_preset_name() const noexcept {
    return docking::preset_descriptor(active_).display_name;
}

docking::workspace_identity_t WorkspacePersistenceController::active_identity() const noexcept {
    return {active_user_.empty() ? docking::workspace_identity_kind_t::built_in :
        docking::workspace_identity_kind_t::user, active_, active_user_};
}

std::string WorkspacePersistenceController::active_identity_key() const noexcept {
    try {
        return docking::identity_key(active_, active_user_);
    } catch (...) {
        return "builtin:analysis";
    }
}

std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>>
WorkspacePersistenceController::user_layout_catalog() const noexcept {
    const auto catalog = catalog_snapshot();
    return catalog ? catalog :
        std::make_shared<const std::vector<docking::user_workspace_descriptor_t>>();
}

bool WorkspacePersistenceController::user_layout_catalog_ready() const noexcept {
    return catalog_ready();
}

bool WorkspacePersistenceController::operation_pending() const noexcept {
    return operation_runtime().pending.load(std::memory_order_acquire);
}

std::string WorkspacePersistenceController::operation_status() const {
    return operation_error_.empty() ? operation_status_ : operation_error_;
}

layout_paths_t WorkspacePersistenceController::capture_paths() const {
    try {
        if (!active_user_.empty())
            return named_user_paths(directory_, active_user_);
        return preset_paths(directory_, active_);
    } catch (...) {
        return {};
    }
}

void WorkspacePersistenceController::capture_environment() {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect work = screen->availableGeometry();
    environment_.work_x = static_cast<std::int64_t>(work.x());
    environment_.work_y = static_cast<std::int64_t>(work.y());
    environment_.work_width = static_cast<std::uint64_t>((std::max)(1, work.width()));
    environment_.work_height = static_cast<std::uint64_t>((std::max)(1, work.height()));
    const double dpr = screen->devicePixelRatio();
    environment_.dpi_milli = static_cast<std::uint32_t>((std::clamp)(
        std::llround(dpr * 1000.0), 250LL, 8000LL));
}

WorkspacePersistenceController::capture_result_t
WorkspacePersistenceController::capture_current() {
    capture_result_t result;
    capture_environment();
    const QByteArray xml = host_->capture_state();
    if (xml.isEmpty() || static_cast<std::size_t>(xml.size()) > k_maximum_payload_bytes) {
        diag::log_tagged_critical_fmt("workspace_layout",
            "layout_save_capture_rejected payload_bytes=%lld maximum_bytes=%llu",
            static_cast<long long>(xml.size()),
            static_cast<unsigned long long>(k_maximum_payload_bytes));
        return result;
    }
    auto payloads = std::make_shared<container_payloads_t>();
    payloads->dock_xml = std::string(xml.constData(), static_cast<std::size_t>(xml.size()));
    payloads->surface_json = host_->capture_surface_json();
    result.payloads = std::move(payloads);
    result.environment = environment_;
    return result;
}

void WorkspacePersistenceController::mark_dirty() {
    if (!initialized_ || !persistence_available_ || shutdown_done_)
        return;
    if (operation_pending())
        return;
    dirty_timer_->setInterval(k_dirty_coalesce_ms);
    dirty_timer_->start();
}

void WorkspacePersistenceController::flush_dirty() {
    if (!initialized_ || !persistence_available_ || shutdown_done_)
        return;
    if (operation_pending())
        return;
    const std::uint64_t now_ms = static_cast<std::uint64_t>(GetTickCount64());
    const std::uint64_t committed = committed_generation();
    if (generation_ != 0 && committed >= generation_)
        recovered_from_backup_ = false;
    const std::uint64_t failed = failed_generation();
    if (failed != 0 && failed == generation_) {
        retry_not_before_ms_ = now_ms + k_write_retry_backoff_ms;
        dirty_timer_->start(static_cast<int>(k_write_retry_backoff_ms));
        diag::log_tagged_critical_fmt("workspace_layout",
            "layout_async_save_failed generation=%llu",
            static_cast<unsigned long long>(failed));
        return;
    }
    if (retry_not_before_ms_ != 0 && now_ms < retry_not_before_ms_) {
        dirty_timer_->start(static_cast<int>(retry_not_before_ms_ - now_ms));
        return;
    }
    auto capture = capture_current();
    if (!capture.payloads) {
        retry_not_before_ms_ = now_ms + k_write_retry_backoff_ms;
        dirty_timer_->start(static_cast<int>(k_write_retry_backoff_ms));
        return;
    }
    const std::uint64_t next_generation = (std::max)(generation_, committed) + 1ULL;
    bool queued = false;
    try {
        queued = queue_layout_write(capture_paths(), active_, locked_, next_generation,
            recovered_from_backup_ || preserve_recovery_backup_, capture.environment,
            registry_fingerprint_, capture.payloads->dock_xml, capture.payloads->surface_json);
    } catch (...) {
        queued = false;
    }
    if (queued) {
        generation_ = next_generation;
        retry_not_before_ms_ = 0;
        diag::log_tagged_fmt("workspace_layout",
            "layout_save_queued generation=%llu payload_bytes=%llu",
            static_cast<unsigned long long>(next_generation),
            static_cast<unsigned long long>(capture.payloads->dock_xml.size()));
    } else {
        retry_not_before_ms_ = now_ms + k_write_retry_backoff_ms;
        dirty_timer_->start(static_cast<int>(k_write_retry_backoff_ms));
        diag::log_tagged_critical_fmt("workspace_layout",
            "layout_save_queue_failed payload_bytes=%llu",
            static_cast<unsigned long long>(capture.payloads->dock_xml.size()));
    }
}

void WorkspacePersistenceController::submit_catalog_scan() {
    aida::infra::executor::submission_t catalog_submission;
    catalog_submission.owner_subsystem = "workspace_layout";
    catalog_submission.label = "workspace_layout.catalog_refresh";
    catalog_submission.thread_class = "diagnostics_io";
    catalog_submission.domain = aida::infra::executor::domain_t::diagnostics;
    catalog_submission.priority = 1;
    catalog_submission.generation = generation_;
    catalog_submission.ui_access_policy = "none";
    catalog_submission.failure_policy = "retain_last_known_good";
    catalog_submission.shutdown_policy = "drain";
    const std::filesystem::path user_directory = directory_ / L"user";
    const std::string active_user = active_user_;
    const std::uint64_t refresh_epoch = next_catalog_epoch();
    catalog_submission.body = [user_directory, active_user, refresh_epoch] {
        if (const auto catalog = scan_user_catalog(user_directory, active_user))
            publish_catalog(catalog, refresh_epoch);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(catalog_submission)));
}

bool WorkspacePersistenceController::restore_or_build_default() {
    if (initialized_)
        return true;
    try {
        registry_fingerprint_ = registry_->persistence_fingerprint();
    } catch (...) {
        registry_fingerprint_.clear();
    }
    if (!valid_registry_fingerprint(registry_fingerprint_)) {
        diag::log_tagged_critical("workspace_layout", "view_registry_fingerprint_unavailable");
        initialized_ = true;
        return false;
    }
    if (!workspace_directory(directory_)) {
        diag::log_tagged_critical("workspace_layout", "persistence_path_unavailable");
        initialized_ = true;
        return false;
    }
    active_record_ = active_record_path(directory_);
    legacy_primary_ = legacy_primary_path(directory_);
    const active_record_t record = read_active_record(active_record_);
    if (record.present) {
        active_ = record.preset;
        locked_ = record.locked;
        active_user_ = record.user_name;
    }
    persistence_available_ = true;
    if (visibility_)
        visibility_->set_capture_suspended(true);

    bool loaded_any = false;
    bool loaded_needs_default = false;

    const auto apply_candidate = [&](const char* source, record_metadata_t& metadata,
                                     container_payloads_t& payloads, bool& out_default) {
        out_default = false;
        if (metadata.payload_kind == payload_kind_t::imgui_ini) {
            diag::log_tagged_fmt("workspace_layout",
                "layout_legacy_container source=%s schema=%u classification=imgui_ini action=rebuild_recipe",
                source, metadata.schema);
            out_default = true;
            return true;
        }
        if (!host_->load_surface_json(payloads.surface_json))
            diag::log_tagged_fmt("workspace_layout",
                "layout_surface_json_rejected source=%s action=surface_state_ignored", source);
        host_->preregister_all_docks();
        if (!host_->apply_state(payloads.dock_xml)) {
            diag::log_tagged_critical_fmt("workspace_layout",
                "layout_apply_failed source=%s action=continue_fallback_chain", source);
            return false;
        }
        return true;
    };

    if (!active_user_.empty()) {
        const auto named_paths = named_user_paths(directory_, active_user_);
        record_metadata_t named_metadata;
        container_payloads_t named_payloads;
        bool use_default = false;
        read_result_t named_result = read_layout_with_backup(named_paths, named_metadata,
            named_payloads);
        if (named_result == read_result_t::valid && named_metadata.preset != active_)
            named_result = read_result_t::invalid;
        if (named_result == read_result_t::valid &&
            !apply_candidate("named", named_metadata, named_payloads, use_default))
            named_result = read_result_t::invalid;
        if (named_result == read_result_t::valid) {
            loaded_any = true;
            loaded_needs_default = use_default;
            active_ = named_metadata.preset;
            generation_ = named_metadata.generation;
            locked_ = named_metadata.locked;
            environment_ = named_metadata.environment;
            note_committed_generation(generation_);
            recovered_from_backup_ = false;
            preserve_recovery_backup_ = false;
            diag::log_tagged_fmt("workspace_layout",
                "layout_loaded source=named schema=%u preset_revision=%u generation=%llu saved_unix_ms=%llu registry_match=%d monitor=%lld,%lld,%llux%llu dpi_milli=%u",
                named_metadata.schema, named_metadata.preset_revision,
                static_cast<unsigned long long>(generation_),
                static_cast<unsigned long long>(named_metadata.saved_unix_ms),
                named_metadata.registry_fingerprint == registry_fingerprint_ ? 1 : 0,
                static_cast<long long>(named_metadata.environment.work_x),
                static_cast<long long>(named_metadata.environment.work_y),
                static_cast<unsigned long long>(named_metadata.environment.work_width),
                static_cast<unsigned long long>(named_metadata.environment.work_height),
                named_metadata.environment.dpi_milli);
        } else {
            active_user_.clear();
            if (!write_active_record(directory_, active_record_, active_, locked_))
                persistence_available_ = false;
        }
    }

    if (!loaded_any) {
        record_metadata_t primary_metadata;
        container_payloads_t primary_payloads;
        bool use_default = false;
        read_result_t primary_result = read_layout_payload(preset_paths(directory_, active_).primary,
            primary_metadata, primary_payloads);
        if (primary_result == read_result_t::valid && primary_metadata.preset != active_)
            primary_result = read_result_t::invalid;
        bool migrated_legacy = false;
        if (primary_result == read_result_t::absent &&
            active_ == docking::workspace_preset_t::analysis) {
            record_metadata_t legacy_metadata;
            container_payloads_t legacy_payloads;
            read_result_t legacy_result = read_layout_payload(legacy_primary_,
                legacy_metadata, legacy_payloads);
            if (legacy_result == read_result_t::valid &&
                legacy_metadata.preset == active_) {
                primary_metadata = legacy_metadata;
                primary_payloads = std::move(legacy_payloads);
                primary_result = read_result_t::valid;
                migrated_legacy = true;
            }
        }
        if (primary_result == read_result_t::valid &&
            !apply_candidate(migrated_legacy ? "legacy" : "primary", primary_metadata,
                primary_payloads, use_default))
            primary_result = read_result_t::invalid;
        if (primary_result == read_result_t::valid) {
            loaded_any = true;
            loaded_needs_default = use_default;
            active_ = primary_metadata.preset;
            generation_ = primary_metadata.generation;
            locked_ = primary_metadata.locked;
            environment_ = primary_metadata.environment;
            note_committed_generation(generation_);
            recovered_from_backup_ = false;
            preserve_recovery_backup_ = false;
            if (!primary_metadata.clean_shutdown) {
                record_metadata_t recovery_metadata;
                const bool recovery_available = inspect_layout_file(
                    preset_paths(directory_, active_).backup, recovery_metadata);
                preserve_recovery_backup_ = recovery_available;
                diag::log_tagged_critical_fmt("workspace_layout",
                    "layout_unclean_start policy=use_valid_primary_preserve_last_good recovery_available=%d recovery_generation=%llu recovery_clean=%d",
                    recovery_available ? 1 : 0,
                    static_cast<unsigned long long>(recovery_metadata.generation),
                    recovery_available && recovery_metadata.clean_shutdown ? 1 : 0);
            }
            diag::log_tagged_fmt("workspace_layout",
                "layout_loaded source=%s schema=%u preset_revision=%u generation=%llu clean_shutdown=%d saved_unix_ms=%llu registry_match=%d",
                migrated_legacy ? "legacy" : "primary",
                primary_metadata.schema, primary_metadata.preset_revision,
                static_cast<unsigned long long>(generation_),
                primary_metadata.clean_shutdown ? 1 : 0,
                static_cast<unsigned long long>(primary_metadata.saved_unix_ms),
                primary_metadata.registry_fingerprint == registry_fingerprint_ ? 1 : 0);
        } else {
            const layout_paths_t active_paths = preset_paths(directory_, active_);
            if (primary_result == read_result_t::invalid)
                MoveFileExW(active_paths.primary.c_str(), active_paths.invalid.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            record_metadata_t backup_metadata;
            container_payloads_t backup_payloads;
            bool backup_default = false;
            read_result_t backup_result = read_layout_payload(active_paths.backup,
                backup_metadata, backup_payloads);
            if (backup_result == read_result_t::valid && backup_metadata.preset != active_)
                backup_result = read_result_t::invalid;
            if (backup_result == read_result_t::valid &&
                !apply_candidate("backup", backup_metadata, backup_payloads, backup_default))
                backup_result = read_result_t::invalid;
            if (backup_result == read_result_t::valid) {
                loaded_any = true;
                loaded_needs_default = backup_default;
                active_ = backup_metadata.preset;
                generation_ = backup_metadata.generation;
                locked_ = backup_metadata.locked;
                environment_ = backup_metadata.environment;
                note_committed_generation(generation_);
                recovered_from_backup_ = true;
                preserve_recovery_backup_ = false;
                diag::log_tagged_critical_fmt("workspace_layout",
                    "layout_recovered source=backup schema=%u generation=%llu clean_shutdown=%d",
                    backup_metadata.schema,
                    static_cast<unsigned long long>(generation_),
                    backup_metadata.clean_shutdown ? 1 : 0);
            } else if (primary_result != read_result_t::absent ||
                       backup_result != read_result_t::absent) {
                active_ = docking::workspace_preset_t::safe;
                diag::log_tagged_critical_fmt("workspace_layout",
                    "layout_recovery_safe primary_result=%u backup_result=%u",
                    static_cast<unsigned>(primary_result),
                    static_cast<unsigned>(backup_result));
            } else {
                diag::log_tagged_fmt("workspace_layout",
                    "layout_first_run_default schema=%u", k_schema_version);
            }
        }
    }

    if (loaded_needs_default || !loaded_any)
        host_->build_preset_layout(active_);
    host_->set_layout_locked(locked_);
    if (visibility_) {
        visibility_->synchronize(active_, active_identity_key());
        host_->apply_hub_subview_selections();
        visibility_->set_capture_suspended(false);
        visibility_->capture();
    }
    if (g_sa_settings.workspace.legacy_bottom_visible) {
        static_cast<void>(registry_->open(
            registry_->instance_for(registry::stable_view_id_t("view.output")),
            registry_->context()));
        g_sa_settings.workspace.legacy_bottom_visible = false;
        static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
    }
    submit_catalog_scan();
    initialized_ = true;
    mark_dirty();
    return true;
}

void WorkspacePersistenceController::finish_operation_state() {
    operation_runtime_t& runtime = operation_runtime();
    runtime.pending.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(runtime.result_mutex);
    runtime.active_request.reset();
}

void WorkspacePersistenceController::process_operation_completion() noexcept {
    auto result = take_operation_result();
    if (!result)
        return;
    operation_runtime_t& runtime = operation_runtime();
    if (result->serial != runtime.serial.load(std::memory_order_acquire) || !initialized_) {
        finish_operation_state();
        static_cast<void>(aida::ui::task_center::update_task(result->task_id,
            aida::ui::task_center::task_state_t::cancelled, 1.0f,
            "Discarded stale workspace transaction", "Operation serial changed"));
        Q_EMIT operationFinished(false,
            QStringLiteral("Discarded stale workspace transaction"));
        return;
    }
    if (!result->success) {
        operation_error_ = result->error;
        operation_status_ = result->error;
        finish_operation_state();
        Q_EMIT operationFinished(false, QString::fromStdString(result->error));
        return;
    }
    if (generation_ != result->source_generation) {
        operation_error_ = "The workspace changed after this transaction was captured; run the action again from the current layout.";
        operation_status_ = operation_error_;
        finish_operation_state();
        static_cast<void>(aida::ui::task_center::update_task(result->task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Workspace generation changed before UI application", operation_error_));
        Q_EMIT operationFinished(false, QString::fromStdString(operation_error_));
        return;
    }
    const std::string previous_identity = docking::identity_key(active_, active_user_);
    bool layout_applied = false;
    const std::uint64_t completion_started = ::GetTickCount64();
    std::uint64_t begin_switch_ms = 0;
    std::uint64_t apply_layout_ms = 0;
    std::uint64_t finish_switch_ms = 0;
    std::uint64_t hub_subview_ms = 0;
    std::uint64_t capture_ms = 0;
    if (result->catalog) {
        publish_catalog(result->catalog, result->catalog_epoch);
        Q_EMIT userCatalogChanged();
    }
    if (result->kind == operation_kind_t::delete_user && visibility_)
        visibility_->remove_persisted(
            docking::identity_key(result->target_preset, result->target_user_name));
    if (result->kind == operation_kind_t::set_lock) {
        locked_ = result->target_locked;
        generation_ = (std::max)(generation_, result->saved_generation);
        if (host_)
            host_->set_layout_locked(locked_);
    } else if (result->kind == operation_kind_t::save_user) {
        generation_ = (std::max)(generation_, result->saved_generation);
        active_user_ = result->active_user_name;
        if (visibility_)
            visibility_->clone_persisted(previous_identity,
                docking::identity_key(active_, active_user_));
    } else if (result->kind == operation_kind_t::rename_user) {
        if (visibility_)
            visibility_->rename_persisted(
                docking::identity_key(result->target_preset, result->source_user_name),
                docking::identity_key(result->target_preset, result->target_user_name));
        active_user_ = result->active_user_name;
    } else if (result->kind == operation_kind_t::delete_user && !result->apply_layout) {
        active_user_ = result->active_user_name;
    } else {
        const std::string target_identity = docking::identity_key(result->target_preset,
            result->active_user_name);
        const std::uint64_t phase_started = ::GetTickCount64();
        if (visibility_)
            visibility_->begin_switch(result->target_preset, target_identity);
        begin_switch_ms = ::GetTickCount64() - phase_started;
        const std::uint64_t apply_started = ::GetTickCount64();
        if (!apply_result_layout(*result)) {
            operation_error_ = "The workspace layout could not be applied to the dock surface.";
            operation_status_ = operation_error_;
            if (visibility_)
                visibility_->set_capture_suspended(false);
            runtime.pending.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(runtime.result_mutex);
                runtime.last_failed_request = runtime.active_request;
                runtime.active_request.reset();
            }
            static_cast<void>(aida::ui::task_center::update_task(result->task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "Workspace UI application failed", operation_error_));
            Q_EMIT operationFinished(false, QString::fromStdString(operation_error_));
            return;
        }
        apply_layout_ms = ::GetTickCount64() - apply_started;
        active_ = result->target_preset;
        active_user_ = result->active_user_name;
        locked_ = result->target_locked;
        generation_ = (std::max)(generation_,
            (std::max)(result->saved_generation, result->metadata.generation));
        host_->set_layout_locked(locked_);
        layout_applied = true;
        if (result->kind == operation_kind_t::restore_preset && visibility_)
            visibility_->reset_persisted(result->target_preset, false);
        else if (result->kind == operation_kind_t::reset_all && visibility_)
            visibility_->reset_persisted(result->target_preset, true);
    }
    operation_error_.clear();
    operation_status_ = "Workspace transaction completed";
    mark_dirty();
    runtime.pending.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(runtime.result_mutex);
        runtime.active_request.reset();
        runtime.last_failed_request.reset();
    }
    static_cast<void>(aida::ui::task_center::update_task(result->task_id,
        aida::ui::task_center::task_state_t::completed, 1.0f,
        "Workspace transaction applied",
        std::string(docking::preset_descriptor(result->target_preset).display_name)));
    Q_EMIT operationFinished(true, {});
    Q_EMIT activeWorkspaceChanged();
    if (visibility_) {
        std::uint64_t phase_started = ::GetTickCount64();
        if (layout_applied)
            visibility_->finish_switch(active_, active_identity_key());
        else
            visibility_->synchronize(active_, active_identity_key());
        finish_switch_ms = ::GetTickCount64() - phase_started;
        phase_started = ::GetTickCount64();
        host_->apply_hub_subview_selections();
        hub_subview_ms = ::GetTickCount64() - phase_started;
        phase_started = ::GetTickCount64();
        visibility_->set_capture_suspended(false);
        visibility_->capture();
        capture_ms = ::GetTickCount64() - phase_started;
    }
    if (layout_applied) {
        diag::log_tagged_fmt("workspace_layout",
            "workspace_switch_applied total_ms=%llu begin_switch_ms=%llu apply_layout_ms=%llu finish_switch_ms=%llu hub_subview_ms=%llu capture_ms=%llu",
            static_cast<unsigned long long>(::GetTickCount64() - completion_started),
            static_cast<unsigned long long>(begin_switch_ms),
            static_cast<unsigned long long>(apply_layout_ms),
            static_cast<unsigned long long>(finish_switch_ms),
            static_cast<unsigned long long>(hub_subview_ms),
            static_cast<unsigned long long>(capture_ms));
    }
    process_operation_retry();
}

bool WorkspacePersistenceController::apply_result_layout(operation_result_t& result) {
    if (visibility_)
        visibility_->set_capture_suspended(true);
    const QByteArray rollback = host_->capture_state();
    bool applied = false;
    if (result.use_default || result.payloads.dock_xml.empty()) {
        host_->build_preset_layout(result.target_preset);
        applied = true;
    } else {
        if (!result.payloads.surface_json.empty() &&
            !host_->load_surface_json(result.payloads.surface_json))
            diag::log_tagged("workspace_layout",
                "layout_surface_json_rejected source=operation action=surface_state_ignored");
        host_->preregister_all_docks();
        applied = host_->apply_state(result.payloads.dock_xml);
        if (!applied) {
            diag::log_tagged_critical("workspace_layout",
                "layout_operation_apply_failed action=rollback_previous_state");
            if (!rollback.isEmpty())
                static_cast<void>(host_->apply_state(
                    std::string(rollback.constData(), static_cast<std::size_t>(rollback.size()))));
        }
    }
    return applied;
}

void WorkspacePersistenceController::settle_pending_operation_for_shutdown() noexcept {
    process_operation_completion();
}

void WorkspacePersistenceController::shutdown() {
    if (shutdown_done_)
        return;
    shutdown_done_ = true;
    settle_pending_operation_for_shutdown();
    if (initialized_ && persistence_available_ && !operation_pending()) {
        auto capture = capture_current();
        bool saved = false;
        const std::uint64_t final_generation = (std::max)(generation_,
            committed_generation()) + 1ULL;
        if (capture.payloads) {
            saved = write_generation(capture_paths(), active_, locked_, final_generation, true,
                recovered_from_backup_ || preserve_recovery_backup_, capture.environment,
                registry_fingerprint_, capture.payloads->dock_xml,
                capture.payloads->surface_json);
        }
        if (!saved)
            diag::log_tagged_critical_fmt("workspace_layout",
                "layout_shutdown_save_failed payload_bytes=%llu",
                static_cast<unsigned long long>(
                    capture.payloads ? capture.payloads->dock_xml.size() : 0));
        else
            diag::log_tagged_fmt("workspace_layout",
                "layout_shutdown_save_complete generation=%llu payload_bytes=%llu clean_shutdown=1",
                static_cast<unsigned long long>(final_generation),
                static_cast<unsigned long long>(capture.payloads->dock_xml.size()));
    } else if (operation_pending()) {
        diag::log_tagged_critical("workspace_layout",
            "layout_shutdown_save_skipped pending_transaction_not_settled=1");
    }
    reset_catalog();
    reset_generation_state();
}

docking::workspace_request_result_t WorkspacePersistenceController::submit_with_status(
    operation_request_t request, const QString& target) noexcept {
    try {
        const auto result = submit_operation(std::move(request));
        if (result == docking::workspace_request_result_t::queued) {
            operation_status_ = "Workspace transaction queued";
            operation_error_.clear();
            Q_EMIT operationStarted(target);
        } else if (result == docking::workspace_request_result_t::failed) {
            operation_error_ = last_submit_error();
            operation_status_ = operation_error_;
            Q_EMIT operationFinished(false, QString::fromStdString(operation_error_));
        }
        return result;
    } catch (...) {
        return docking::workspace_request_result_t::failed;
    }
}

docking::workspace_request_result_t WorkspacePersistenceController::set_layout_locked(
    bool locked) noexcept {
    if (!initialized_ || !persistence_available_)
        return docking::workspace_request_result_t::unavailable;
    if (operation_pending())
        return docking::workspace_request_result_t::busy;
    if (locked_ == locked)
        return docking::workspace_request_result_t::unchanged;
    auto capture = capture_current();
    if (!capture.payloads)
        return docking::workspace_request_result_t::failed;
    operation_request_t request;
    request.kind = operation_kind_t::set_lock;
    request.source_generation = generation_;
    request.current_preset = active_;
    request.current_user_name = active_user_;
    request.target_preset = active_;
    request.target_locked = locked;
    request.save_generation = (std::max)(generation_, committed_generation()) + 1ULL;
    request.skip_backup = recovered_from_backup_ || preserve_recovery_backup_;
    request.current_paths = capture_paths();
    request.workspace_directory = directory_;
    request.active_record = active_record_;
    request.environment = capture.environment;
    request.current_payload = capture.payloads;
    request.registry_fingerprint = registry_fingerprint_;
    return submit_with_status(std::move(request), QString::fromStdString(std::string(docking::preset_stable_id(active_))));
}

docking::workspace_request_result_t WorkspacePersistenceController::switch_to(
    docking::workspace_preset_t preset) noexcept {
    if (!initialized_ || !persistence_available_)
        return docking::workspace_request_result_t::unavailable;
    if (operation_pending())
        return docking::workspace_request_result_t::busy;
    if (active_ == preset && active_user_.empty())
        return docking::workspace_request_result_t::unchanged;
    const std::uint64_t capture_started = ::GetTickCount64();
    auto capture = capture_current();
    diag::log_tagged_fmt("workspace_layout",
        "workspace_switch_capture elapsed_ms=%llu",
        static_cast<unsigned long long>(::GetTickCount64() - capture_started));
    if (!capture.payloads)
        return docking::workspace_request_result_t::failed;
    operation_request_t request;
    request.kind = operation_kind_t::switch_preset;
    request.source_generation = generation_;
    request.current_preset = active_;
    request.current_user_name = active_user_;
    request.target_preset = preset;
    request.target_locked = locked_;
    request.save_generation = (std::max)(generation_, committed_generation()) + 1ULL;
    request.skip_backup = recovered_from_backup_ || preserve_recovery_backup_;
    request.current_paths = capture_paths();
    request.target_paths = preset_paths(directory_, preset);
    request.workspace_directory = directory_;
    request.active_record = active_record_;
    request.environment = capture.environment;
    request.current_payload = capture.payloads;
    request.registry_fingerprint = registry_fingerprint_;
    return submit_with_status(std::move(request), QString::fromStdString(std::string(docking::preset_stable_id(preset))));
}

docking::workspace_request_result_t WorkspacePersistenceController::save_user_layout(
    std::string_view name, bool overwrite) noexcept {
    if (!docking::valid_user_layout_name(name))
        return docking::workspace_request_result_t::invalid_name;
    if (!initialized_ || !persistence_available_)
        return docking::workspace_request_result_t::unavailable;
    if (operation_pending())
        return docking::workspace_request_result_t::busy;
    if (!catalog_ready())
        return docking::workspace_request_result_t::unavailable;
    const auto catalog = catalog_snapshot();
    const bool exists = catalog && std::any_of(catalog->begin(), catalog->end(),
        [name](const auto& entry) { return entry.name == name; });
    if (!overwrite && exists)
        return docking::workspace_request_result_t::already_exists;
    if (!exists && catalog && catalog->size() >= k_maximum_named_user_layouts)
        return docking::workspace_request_result_t::unavailable;
    auto capture = capture_current();
    if (!capture.payloads)
        return docking::workspace_request_result_t::failed;
    operation_request_t request;
    try {
        request.target_paths = named_user_paths(directory_, name);
    } catch (...) {
        return docking::workspace_request_result_t::failed;
    }
    request.kind = operation_kind_t::save_user;
    request.source_generation = generation_;
    request.current_preset = active_;
    request.current_user_name = active_user_;
    request.target_user_name.assign(name);
    request.overwrite = overwrite;
    request.target_preset = active_;
    request.target_locked = locked_;
    request.save_generation = (std::max)(generation_, committed_generation()) + 1ULL;
    request.skip_backup = recovered_from_backup_ || preserve_recovery_backup_;
    request.current_paths = capture_paths();
    request.workspace_directory = directory_;
    request.active_record = active_record_;
    request.environment = capture.environment;
    request.current_payload = capture.payloads;
    request.registry_fingerprint = registry_fingerprint_;
    return submit_with_status(std::move(request), QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
}

docking::workspace_request_result_t WorkspacePersistenceController::save_active_user_layout() noexcept {
    return active_user_.empty() ? docking::workspace_request_result_t::unavailable :
        save_user_layout(active_user_, true);
}

docking::workspace_request_result_t WorkspacePersistenceController::load_user_layout(
    std::string_view name) noexcept {
    return load_user_layout_exact(name, 0);
}

docking::workspace_request_result_t WorkspacePersistenceController::load_user_layout_exact(
    std::string_view name, std::uint64_t expected_generation) noexcept {
    if (!docking::valid_user_layout_name(name))
        return docking::workspace_request_result_t::invalid_name;
    if (!initialized_ || !persistence_available_)
        return docking::workspace_request_result_t::unavailable;
    if (operation_pending())
        return docking::workspace_request_result_t::busy;
    if (!catalog_ready())
        return docking::workspace_request_result_t::unavailable;
    const auto catalog = catalog_snapshot();
    const auto selected = catalog ? std::find_if(catalog->begin(), catalog->end(),
        [name](const auto& entry) { return entry.name == name; }) :
        std::vector<docking::user_workspace_descriptor_t>::const_iterator{};
    if (!catalog || selected == catalog->end())
        return docking::workspace_request_result_t::not_found;
    if (expected_generation != 0 && selected->generation != expected_generation)
        return docking::workspace_request_result_t::unavailable;
    auto capture = capture_current();
    if (!capture.payloads)
        return docking::workspace_request_result_t::failed;
    operation_request_t request;
    try {
        request.target_paths = named_user_paths(directory_, name);
    } catch (...) {
        return docking::workspace_request_result_t::failed;
    }
    request.kind = operation_kind_t::load_user;
    request.source_generation = generation_;
    request.current_preset = active_;
    request.current_user_name = active_user_;
    request.target_user_name.assign(name);
    request.expected_user_generation = expected_generation != 0
        ? expected_generation : selected->generation;
    request.target_preset = active_;
    request.target_locked = locked_;
    request.save_generation = (std::max)(generation_, committed_generation()) + 1ULL;
    request.skip_backup = recovered_from_backup_ || preserve_recovery_backup_;
    request.current_paths = capture_paths();
    request.workspace_directory = directory_;
    request.active_record = active_record_;
    request.environment = capture.environment;
    request.current_payload = capture.payloads;
    request.registry_fingerprint = registry_fingerprint_;
    return submit_with_status(std::move(request), QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
}

docking::workspace_request_result_t WorkspacePersistenceController::rename_user_layout(
    std::string_view current_name, std::string_view new_name) noexcept {
    if (!docking::valid_user_layout_name(current_name) || !docking::valid_user_layout_name(new_name))
        return docking::workspace_request_result_t::invalid_name;
    if (current_name == new_name)
        return docking::workspace_request_result_t::unchanged;
    if (!initialized_ || !persistence_available_)
        return docking::workspace_request_result_t::unavailable;
    if (operation_pending())
        return docking::workspace_request_result_t::busy;
    if (!catalog_ready())
        return docking::workspace_request_result_t::unavailable;
    const auto catalog = catalog_snapshot();
    if (!catalog || std::none_of(catalog->begin(), catalog->end(),
            [current_name](const auto& entry) { return entry.name == current_name; }))
        return docking::workspace_request_result_t::not_found;
    if (std::any_of(catalog->begin(), catalog->end(),
            [new_name](const auto& entry) { return entry.name == new_name; }))
        return docking::workspace_request_result_t::already_exists;
    operation_request_t request;
    try {
        request.source_user_paths = named_user_paths(directory_, current_name);
        request.target_paths = named_user_paths(directory_, new_name);
    } catch (...) {
        return docking::workspace_request_result_t::failed;
    }
    request.kind = operation_kind_t::rename_user;
    request.source_generation = generation_;
    request.current_preset = active_;
    const auto source_descriptor = std::find_if(catalog->begin(), catalog->end(),
        [current_name](const auto& entry) { return entry.name == current_name; });
    request.target_preset = source_descriptor->base_preset;
    request.target_locked = locked_;
    request.current_user_name = active_user_;
    request.source_user_name.assign(current_name);
    request.target_user_name.assign(new_name);
    request.expected_user_generation = source_descriptor->generation;
    request.workspace_directory = directory_;
    request.active_record = active_record_;
    request.registry_fingerprint = registry_fingerprint_;
    return submit_with_status(std::move(request), QString::fromUtf8(new_name.data(), static_cast<qsizetype>(new_name.size())));
}

docking::workspace_request_result_t WorkspacePersistenceController::delete_user_layout(
    std::string_view name) noexcept {
    if (!docking::valid_user_layout_name(name))
        return docking::workspace_request_result_t::invalid_name;
    if (!initialized_ || !persistence_available_)
        return docking::workspace_request_result_t::unavailable;
    if (operation_pending())
        return docking::workspace_request_result_t::busy;
    if (!catalog_ready())
        return docking::workspace_request_result_t::unavailable;
    const auto catalog = catalog_snapshot();
    if (!catalog || std::none_of(catalog->begin(), catalog->end(),
            [name](const auto& entry) { return entry.name == name; }))
        return docking::workspace_request_result_t::not_found;
    operation_request_t request;
    try {
        request.target_paths = named_user_paths(directory_, name);
        if (active_user_ == name)
            request.fallback_paths = preset_paths(directory_, active_);
    } catch (...) {
        return docking::workspace_request_result_t::failed;
    }
    request.kind = operation_kind_t::delete_user;
    request.source_generation = generation_;
    request.current_preset = active_;
    const auto target_descriptor = std::find_if(catalog->begin(), catalog->end(),
        [name](const auto& entry) { return entry.name == name; });
    request.target_preset = target_descriptor->base_preset;
    request.target_locked = locked_;
    request.current_user_name = active_user_;
    request.target_user_name.assign(name);
    request.expected_user_generation = target_descriptor->generation;
    request.workspace_directory = directory_;
    request.active_record = active_record_;
    request.registry_fingerprint = registry_fingerprint_;
    return submit_with_status(std::move(request), QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
}

docking::workspace_request_result_t WorkspacePersistenceController::restore_builtin(
    docking::workspace_preset_t preset) noexcept {
    if (!initialized_ || !persistence_available_)
        return docking::workspace_request_result_t::unavailable;
    if (operation_pending())
        return docking::workspace_request_result_t::busy;
    operation_request_t request;
    request.kind = operation_kind_t::restore_preset;
    request.source_generation = generation_;
    request.current_preset = active_;
    request.target_preset = preset;
    request.target_locked = false;
    request.current_user_name = active_user_;
    request.workspace_directory = directory_;
    request.active_record = active_record_;
    request.registry_fingerprint = registry_fingerprint_;
    try {
        request.reset_paths.push_back(preset_paths(directory_, preset));
    } catch (...) {
        return docking::workspace_request_result_t::failed;
    }
    return submit_with_status(std::move(request), QString::fromStdString(std::string(docking::preset_stable_id(preset))));
}

docking::workspace_request_result_t WorkspacePersistenceController::reset_current() noexcept {
    return restore_builtin(active_preset());
}

docking::workspace_request_result_t WorkspacePersistenceController::reset_all() noexcept {
    if (!initialized_ || !persistence_available_)
        return docking::workspace_request_result_t::unavailable;
    if (operation_pending())
        return docking::workspace_request_result_t::busy;
    operation_request_t request;
    request.kind = operation_kind_t::reset_all;
    request.source_generation = generation_;
    request.current_preset = active_;
    request.target_preset = docking::workspace_preset_t::analysis;
    request.target_locked = false;
    request.current_user_name = active_user_;
    request.workspace_directory = directory_;
    request.active_record = active_record_;
    request.registry_fingerprint = registry_fingerprint_;
    try {
        std::size_t count = 0;
        const auto* descriptors = docking::presets(count);
        for (std::size_t index = 0; index < count; ++index)
            request.reset_paths.push_back(preset_paths(directory_, descriptors[index].id));
        request.user_directory = directory_ / L"user";
    } catch (...) {
        return docking::workspace_request_result_t::failed;
    }
    return submit_with_status(std::move(request), QStringLiteral("reset_all"));
}

docking::workspace_request_result_t WorkspacePersistenceController::activate_safe_layout() noexcept {
    return restore_builtin(docking::workspace_preset_t::safe);
}

docking::workspace_request_result_t WorkspacePersistenceController::open_missing_views() noexcept {
    if (!initialized_)
        return docking::workspace_request_result_t::unavailable;
    if (locked_)
        return docking::workspace_request_result_t::unavailable;
    if (operation_pending())
        return docking::workspace_request_result_t::busy;
    host_->build_preset_layout(active_, true);
    mark_dirty();
    return docking::workspace_request_result_t::completed;
}

}
