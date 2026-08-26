#pragma once

#include <QPointer>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/analysis/struct_dissector.hpp"
#include "core/disasm/disasm_view.hpp"
#include "qt/analysis/qt_analysis_host_hooks.hpp"

class QAbstractTableModel;
class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QSplitter;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaSearchField;
class AidaStateView;
}

namespace aida::qt::analysis {

class QtDissectorFieldModel;
class QtDissectorStructureModel;
class QtWriteReviewDialog;
class QtEnumManagerDialog;
class QtDissectorLayoutDialog;

// Edit-target kinds (verbatim from struct_dissector_view::edit_target_t).
enum class dissector_edit_target_t : int {
    none = 0,
    field_name,
    field_size,
    field_comment,
    struct_name,
    array_count,
    nested_target,
    pointer_target,
    enum_reference,
    bitfield,
    field_alignment,
};

// Staged-target handoff store (cross-view: scanner/debugger stage targets for
// the dissector). Domain-level store, replacing struct_dissector_view::g_staged_target.
struct qt_staged_target_state_t {
    std::optional<staged_dissector_target_t> context;
    std::string status;
    bool stale = false;
};

qt_staged_target_state_t& staged_dissector_target_store();

// Structure Dissector view (07 sec. 6.2), types-hub page 6 (view.types.dissector,
// alias view.types.live_inspector). The engine (struct_dissector.hpp) is
// ZERO-CHANGE; g_ui becomes widget members; the draw-only scrollbars die (sec. 10).
class QtStructDissectorView : public QWidget {
    Q_OBJECT
public:
    explicit QtStructDissectorView(QWidget* parent = nullptr);
    ~QtStructDissectorView() override;

    static QtStructDissectorView* activeInstance() noexcept { return active_instance_; }

    // Scanner/debugger staged-target handoff (analysis_host_hooks).
    bool stageTarget(staged_dissector_target_t context, std::string& error);

    // Cross-module entry points preserved from struct_dissector_view.hpp.
    bool stageWriteReview(int structure_index, int field_index,
                          const struct_dissector::field_def_t& field,
                          const struct_dissector::live_value_t& value,
                          std::uint64_t base_address, const char* text,
                          std::string& error);
    void openEnumManager(std::uint64_t selected_enum_id = 0);
    void openLayoutConfig();
    bool applyCatalogUndo();
    bool applyCatalogRedo();

    // Catalog edit helpers (verbatim apply_catalog_edit semantics); public for
    // the dissector dialogs and context menus.
    bool catalogEdit(const char* label, const std::function<bool()>& mutation);
    int catalogIndexEdit(const char* label, const std::function<int()>& mutation);
    const std::string& operationStatus() const noexcept { return operation_status_; }
    void resetFieldSelection() {
        selected_field_ = -1;
        editing_field_ = -1;
    }
    void setPendingInsertIndex(int index) { pending_insert_index_ = index; }
    // Retained-identity inline field edit entry (context-menu path).
    void openFieldEdit(int target, int field_index, std::string seed,
                       std::uint64_t structure_id, std::uint64_t structure_revision,
                       std::uint64_t field_id);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void pollEngine();
    void rebuildStructureList();
    void refreshFieldModel();
    void refreshPresentation();
    void showStructureMenu(const QPoint& global_pos, int view_row);
    void showFieldMenu(const QPoint& global_pos, int view_row);
    void showInlineEdit(int target, int field_index, const std::string& seed);
    void confirmRemoveField(std::uint64_t structure_id, std::uint64_t structure_revision,
                            std::uint64_t field_id);
    void applyBaseAddress();
    void commitInlineEdit();
    void addField();
    void publishFieldSelection(int field_index);
    void refreshStagedStrip();

    QtDissectorStructureModel* struct_model_ = nullptr;
    QtDissectorFieldModel* field_model_ = nullptr;
    QTableView* struct_table_ = nullptr;
    QTableView* field_table_ = nullptr;
    widgets::AidaSearchField* struct_filter_ = nullptr;
    QLineEdit* addr_edit_ = nullptr;
    QLineEdit* rename_edit_ = nullptr;
    QLineEdit* new_name_edit_ = nullptr;
    QLineEdit* field_name_edit_ = nullptr;
    QLineEdit* field_offset_edit_ = nullptr;
    QComboBox* field_type_combo_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QLabel* validation_label_ = nullptr;
    QLabel* operation_label_ = nullptr;
    QLabel* staged_label_ = nullptr;
    QWidget* staged_strip_ = nullptr;
    QTimer* timer_ = nullptr;
    QSplitter* splitter_ = nullptr;

    // Widget-member UI state (was struct_dissector_view::g_ui; 07 sec. 1.3).
    int selected_field_ = -1;
    int editing_field_ = -1;
    bool edit_value_focus_requested_ = false;
    std::uint64_t edit_value_structure_id_ = 0;
    std::uint64_t edit_value_structure_revision_ = 0;
    std::uint64_t edit_value_field_id_ = 0;
    std::uint64_t edit_base_address_ = 0;
    std::string edit_value_buf_;
    int pending_insert_index_ = -1;
    int add_type_ = 0;
    bool table_focused_ = false;
    bool list_focused_ = false;
    std::uint64_t context_refresh_seq_ = 0;
    std::uint64_t context_base_address_ = 0;
    std::uint32_t context_target_pid_ = 0;
    std::uint64_t pending_remove_structure_id_ = 0;
    std::uint64_t pending_remove_structure_revision_ = 0;
    std::uint64_t pending_remove_field_id_ = 0;
    int edit_target_ = 0;
    int edit_target_field_ = -1;
    std::uint64_t edit_structure_id_ = 0;
    std::uint64_t edit_structure_revision_ = 0;
    std::uint64_t edit_field_id_ = 0;
    std::string operation_status_;
    bool operation_error_ = false;
    std::uint64_t validation_structure_id_ = 0;
    std::uint64_t validation_revision_ = 0;
    struct_dissector::layout_validation_t validation_;
    std::uint64_t last_schema_revision_ = 0;
    std::uint64_t last_completed_seq_ = 0;
    int last_active_struct_ = -2;
    bool addr_seeded_ = false;
    float auto_refresh_accum_ = 0.f;

    QtWriteReviewDialog* write_review_ = nullptr;
    QtEnumManagerDialog* enum_manager_ = nullptr;
    QtDissectorLayoutDialog* layout_dialog_ = nullptr;
    QDialog* inline_edit_dialog_ = nullptr;
    QDialog* remove_dialog_ = nullptr;
    static QtStructDissectorView* active_instance_;
};

}
