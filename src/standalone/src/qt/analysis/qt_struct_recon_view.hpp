#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "core/analysis/struct_recon_engine.hpp"

class QAbstractTableModel;
class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::analysis {

// Structure Reconstruction view (07 sec. 6.4): view.types.inferred (types-hub page
// 5) and view.types.struct_recon (standalone document). The engine
// (struct_recon_engine.hpp/struct_monitor.hpp) is ZERO-CHANGE.
class QtStructReconView : public QWidget {
    Q_OBJECT
public:
    explicit QtStructReconView(QWidget* parent = nullptr);

    // Action-surface entry points (analysis_host_hooks recon_*).
    bool hasCurrentStructure() const;
    bool copyCurrentDeclaration(std::string& detail);
    bool declareAndApplyCurrent(std::string& detail);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void pollEngine();
    void refreshPresentation();
    void showFieldMenu(const QPoint& global_pos, int view_row);
    void showDeclarationPreview();
    void showDeclareApplyReview();
    void showRetainedFieldEdit(int kind, int field_index);
    void updateDetailPanel();
    std::optional<std::pair<int, int>> resolveRetainedEditBinding() const;
    void startMonitors(int stop_live);
    void snapshotReconstruct();
    void hwMonitor();
    void exportDeclaration();
    void saveStruct();
    void loadAll();
    void refreshValues();

    QAbstractTableModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QLineEdit* addr_edit_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QLineEdit* size_edit_ = nullptr;
    QLabel* info_label_ = nullptr;
    QLabel* operation_label_ = nullptr;
    QLabel* live_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* snapshot_button_ = nullptr;
    QPushButton* hw_button_ = nullptr;
    QPushButton* live_button_ = nullptr;
    QPushButton* export_button_ = nullptr;
    QPushButton* apply_button_ = nullptr;
    QPushButton* ai_button_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* load_button_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QWidget* detail_panel_ = nullptr;
    QLabel* detail_title_ = nullptr;
    QLabel* detail_offset_ = nullptr;
    QLabel* detail_size_ = nullptr;
    QLabel* detail_type_ = nullptr;
    QLabel* detail_array_ = nullptr;
    QLabel* detail_conf_ = nullptr;
    QLabel* detail_heat_ = nullptr;
    QLabel* detail_name_ = nullptr;
    QAbstractTableModel* vtable_model_ = nullptr;
    QTableView* vtable_table_ = nullptr;
    QLabel* vtable_header_ = nullptr;
    QAbstractTableModel* access_model_ = nullptr;
    QTableView* access_table_ = nullptr;
    QLabel* access_header_ = nullptr;
    QSplitter* splitter_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QTimer* timer_ = nullptr;
    QDialog* preview_dialog_ = nullptr;
    QDialog* overlay_review_dialog_ = nullptr;
    QDialog* field_edit_dialog_ = nullptr;

    // View-local UI state (was struct_recon_view::s_state; 07 sec. 1.3).
    int selected_field_ = -1;
    std::string operation_status_;
    bool operation_error_ = false;
    bool operation_pending_ = false;
    std::uint64_t operation_generation_ = 0;
    std::uint64_t operation_overlay_revision_ = 0;
    bool vtable_expanded_ = true;
    int context_field_ = -1;
    std::uint64_t context_base_ = 0;
    std::uint64_t context_offset_ = 0;
    int context_size_ = 0;
    std::string context_name_;
    std::string context_struct_name_;

    // Retained-edit review state (verbatim from struct_recon_view local_state_t).
    enum class retained_edit_kind_t : std::uint8_t { none, rename, retype, live_value };
    retained_edit_kind_t retained_edit_kind_ = retained_edit_kind_t::none;
    std::shared_ptr<const struct_recon::reconstructed_struct_t> retained_edit_snapshot_;
    std::weak_ptr<aida::analysis::analysis_workspace_t> retained_edit_workspace_;
    std::shared_ptr<const aida::analysis::analysis_publication_t>
        retained_edit_publication_;
    std::string retained_edit_workspace_id_;
    std::uint64_t retained_edit_workspace_generation_ = 0;
    std::uint64_t retained_edit_analysis_revision_ = 0;
    std::uint32_t retained_edit_target_pid_ = 0;
    std::uint64_t retained_edit_field_hash_ = 0;
    std::uint64_t retained_edit_structure_id_ = 0;
    std::uint64_t retained_edit_structure_revision_ = 0;
    std::uint64_t retained_edit_field_id_ = 0;
    std::uint64_t retained_edit_schema_revision_ = 0;
    std::uint64_t retained_edit_base_ = 0;
    std::uint64_t retained_edit_refresh_sequence_ = 0;
    int retained_edit_field_index_ = -1;
    std::string retained_edit_text_;
    int retained_edit_type_ = 0;
    std::shared_ptr<const struct_recon::reconstructed_struct_t> last_snapshot_;

    // Declare-and-apply overlay review state (retained; verbatim).
    std::shared_ptr<const struct_recon::reconstructed_struct_t>
        retained_overlay_structure_;
    std::string retained_overlay_workspace_id_;
    std::uint64_t retained_overlay_generation_ = 0;
    std::uint64_t retained_overlay_analysis_revision_ = 0;
    std::uint64_t retained_overlay_revision_ = 0;
    std::uint64_t retained_overlay_base_ = 0;
    std::weak_ptr<aida::analysis::analysis_workspace_t> retained_overlay_workspace_;
    std::shared_ptr<const aida::analysis::analysis_publication_t>
        retained_overlay_publication_;
};

}
