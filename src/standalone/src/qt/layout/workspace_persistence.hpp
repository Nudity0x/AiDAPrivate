#pragma once

#include "qt/layout/workspace_operations.hpp"
#include "qt/docking/preset_recipes.hpp"

#include <QObject>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class QTimer;

namespace aida::qt::registry {
class qt_view_registry_t;
class ViewVisibilityController;
}

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::layout {

class WorkspacePersistenceController : public QObject {
    Q_OBJECT
public:
    WorkspacePersistenceController(docking::AidaDockHost* host,
                                   registry::qt_view_registry_t* registry,
                                   QObject* parent = nullptr);
    ~WorkspacePersistenceController() override;

    bool restore_or_build_default();

    docking::workspace_preset_t active_preset() const noexcept { return active_; }
    std::string_view active_preset_name() const noexcept;
    docking::workspace_identity_t active_identity() const noexcept;
    std::string active_identity_key() const noexcept;
    std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>>
        user_layout_catalog() const noexcept;
    bool user_layout_catalog_ready() const noexcept;
    bool layout_locked() const noexcept { return locked_; }
    bool operation_pending() const noexcept;
    std::string operation_status() const;
    const std::string& registry_fingerprint() const noexcept { return registry_fingerprint_; }
    bool persistence_available() const noexcept { return persistence_available_; }

    docking::workspace_request_result_t set_layout_locked(bool locked) noexcept;
    docking::workspace_request_result_t switch_to(docking::workspace_preset_t preset) noexcept;
    docking::workspace_request_result_t save_user_layout(std::string_view name,
                                                         bool overwrite = false) noexcept;
    docking::workspace_request_result_t save_active_user_layout() noexcept;
    docking::workspace_request_result_t load_user_layout(std::string_view name) noexcept;
    docking::workspace_request_result_t load_user_layout_exact(std::string_view name,
        std::uint64_t expected_generation) noexcept;
    docking::workspace_request_result_t rename_user_layout(std::string_view current_name,
                                                           std::string_view new_name) noexcept;
    docking::workspace_request_result_t delete_user_layout(std::string_view name) noexcept;
    docking::workspace_request_result_t restore_builtin(docking::workspace_preset_t preset) noexcept;
    docking::workspace_request_result_t reset_current() noexcept;
    docking::workspace_request_result_t reset_all() noexcept;
    docking::workspace_request_result_t activate_safe_layout() noexcept;
    docking::workspace_request_result_t open_missing_views() noexcept;

    void settle_pending_operation_for_shutdown() noexcept;

public Q_SLOTS:
    void mark_dirty();
    void shutdown();

Q_SIGNALS:
    void operationStarted(const QString& target);
    void operationFinished(bool success, const QString& detail);
    void activeWorkspaceChanged();
    void userCatalogChanged();

private:
    struct capture_result_t {
        std::shared_ptr<const container_payloads_t> payloads;
        layout_environment_t environment;
    };

    void process_operation_completion() noexcept;
    docking::workspace_request_result_t submit_with_status(operation_request_t request,
                                                           const QString& target) noexcept;
    void flush_dirty();
    capture_result_t capture_current();
    layout_paths_t capture_paths() const;
    void capture_environment();
    bool apply_result_layout(operation_result_t& result);
    void submit_catalog_scan();
    void finish_operation_state();

    docking::AidaDockHost* host_ = nullptr;
    registry::qt_view_registry_t* registry_ = nullptr;
    registry::ViewVisibilityController* visibility_ = nullptr;
    QTimer* dirty_timer_ = nullptr;
    bool initialized_ = false;
    bool persistence_available_ = false;
    bool recovered_from_backup_ = false;
    bool preserve_recovery_backup_ = false;
    bool shutdown_done_ = false;
    docking::workspace_preset_t active_ = docking::workspace_preset_t::analysis;
    std::string active_user_;
    bool locked_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t retry_not_before_ms_ = 0;
    layout_environment_t environment_;
    std::filesystem::path directory_;
    std::filesystem::path active_record_;
    std::filesystem::path legacy_primary_;
    std::string registry_fingerprint_;
    std::string operation_status_;
    std::string operation_error_;
};

}
