#pragma once

#include <QDialog>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/analysis/struct_dissector.hpp"
#include "core/disasm/disasm_view.hpp"

class QDialogButtonBox;
class QLabel;
class QTimer;

namespace aida::qt::widgets {
class AidaNotice;
}

namespace aida::qt::analysis {

// Write-review status (verbatim from struct_dissector_view.hpp).
enum class qt_write_review_status_t : std::uint8_t {
    review,
    queued,
    running,
    succeeded,
    failed,
    cancelled,
    stale
};

// The reviewed-write record (verbatim field set).
struct qt_write_review_t {
    bool visible = false;
    bool open_requested = false;
    qt_write_review_status_t status = qt_write_review_status_t::review;
    std::uint64_t serial = 0;
    std::uint32_t pid = 0;
    std::uint64_t address = 0;
    std::uint64_t workspace_generation = 0;
    std::uint64_t structure_identity = 0;
    std::uint64_t base_address = 0;
    int structure_index = -1;
    int field_index = -1;
    std::string structure_name;
    std::string field_name;
    std::vector<std::uint8_t> old_bytes;
    std::vector<std::uint8_t> new_bytes;
    std::string error;
    bool mutation_may_remain = false;
    std::weak_ptr<aida::analysis::analysis_workspace_t> workspace;
    std::shared_ptr<std::atomic<bool>> cancellation;
};

// Modal "Review Structure Field Write" (07 sec. 6.2, DRIVER WRITE safety
// invariant - VERBATIM semantics): shows PID/structure.field/exact range/old
// bytes/new bytes + both notices; pre-write exact-range re-read mismatch aborts;
// post-write readback mismatch auto-rolls back; "Stage undo" stages the
// inverse write through the same modal. open(), never exec().
class QtWriteReviewDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtWriteReviewDialog(QWidget* parent = nullptr);

    // Stage a reviewed write (ports stage_write_review). Shows the dialog.
    bool stage(const disasm_view::workspace_context_t& context,
               int structure_index, int field_index,
               const struct_dissector::field_def_t& field,
               const struct_dissector::live_value_t& value,
               std::uint64_t base_address, const char* text, std::string& error);

    static QtWriteReviewDialog* activeInstance() noexcept { return active_instance_; }

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void pollWorker();

private:
    void rebuildContent();
    void onConfirm();
    void onCancel();
    void stageUndo();
    bool submitWrite();
    void publishResult(bool wrote, const std::string& result_error,
                       bool mutation_may_remain, std::uint64_t serial,
                       std::uint64_t workspace_generation,
                       std::uint64_t structure_identity, std::uint64_t base_address,
                       int structure_index, int field_index,
                       const std::vector<std::uint8_t>& new_bytes,
                       const std::string& task_id, std::uint32_t pid,
                       bool cancelled);

    qt_write_review_t review_;
    std::atomic<std::uint64_t> write_serial_{0};
    std::uint64_t running_serial_ = 0;
    QTimer* timer_ = nullptr;
    QLabel* pid_value_ = nullptr;
    QLabel* field_value_ = nullptr;
    QLabel* range_value_ = nullptr;
    QLabel* old_bytes_ = nullptr;
    QLabel* new_bytes_ = nullptr;
    QLabel* status_label_ = nullptr;
    widgets::AidaNotice* error_notice_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    static QtWriteReviewDialog* active_instance_;
};

}
