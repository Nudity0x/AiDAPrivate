#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QModelRoleDataSpan>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/network/burp/comparer.hpp"
#include "qt/network/comparer/diff_viewer.hpp"
#include "qt/network/network_pane_base.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::net {

// ComparerSlotsModel feeds the A/B selector combos from comparer::list_slots()
// snapshots (bounded registry copies, refreshed on a 500 ms visibility-gated
// timer and after each slot op; the legacy path read list_slots() every
// frame). The slot id rides in Qt::UserRole.
class ComparerSlotsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Label = 0, Id, Size, Source, ColumnCount };

    explicit ComparerSlotsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    void refreshFromEngine();
    const aida::burp::comparer::slot_t* slotAt(int row) const noexcept;
    const aida::burp::comparer::slot_t* findSlot(std::uint64_t id) const noexcept;
    std::vector<aida::burp::comparer::slot_t> slots() const;

private:
    std::vector<aida::burp::comparer::slot_t> slots_;
};

// ComparerView ports comparer_view.cpp (the TU has no external callers and is
// deleted by this wave). ensureDiff ports the generation-fenced compute
// contract verbatim: published-triple adoption, same-triple pending coalesce,
// fetch_add(1)+1 generation, feature_worker body with the verbatim get_slot
// error strings and the 32 KiB publication truncation, double-checked
// generation before publish, and the stale-selection reset with the legacy
// error line.
class ComparerView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit ComparerView(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    struct diff_publication_t {
        std::uint64_t generation = 0;
        std::uint64_t slot_a = 0;
        std::uint64_t slot_b = 0;
        int mode_idx = -1;
        bool succeeded = false;
        std::string error;
        std::shared_ptr<const comparer_diff_content_t> content;
        aida::burp::comparer::diff_stats_t stats;
    };

    void ensureDiff();
    void adoptPublication(const std::shared_ptr<const diff_publication_t>& publication);
    void refreshSlots();
    void pruneSelection(const std::vector<aida::burp::comparer::slot_t>& slots);
    void addSlotFromClipboard();
    void addSlotFromFile();
    void addSlotFromText();
    void clearSlots();
    void openSlotContextMenu(bool sideA, const QPoint& globalPos);
    void openRemoveReview(std::uint64_t slotId);
    std::uint64_t selectedId(bool sideA) const;
    void setSelectedId(bool sideA, std::uint64_t id);
    void syncComboSelections();
    void updateStatsStrip();

    QLineEdit* label_edit_ = nullptr;
    widgets::AidaButton* paste_button_ = nullptr;
    QLineEdit* file_edit_ = nullptr;
    widgets::AidaButton* add_file_button_ = nullptr;
    widgets::AidaButton* clear_button_ = nullptr;
    QLineEdit* paste_edit_ = nullptr;
    widgets::AidaButton* add_text_button_ = nullptr;
    ComparerSlotsModel* slots_model_ = nullptr;
    QComboBox* combo_a_ = nullptr;
    QComboBox* combo_b_ = nullptr;
    QComboBox* mode_combo_ = nullptr;
    widgets::AidaButton* swap_button_ = nullptr;
    QLabel* stats_label_ = nullptr;
    ComparerDiffViewer* viewer_ = nullptr;
    QTimer* slots_timer_ = nullptr;

    std::uint64_t selected_a_ = 0;
    std::uint64_t selected_b_ = 0;
    int mode_idx_ = 3;
    bool cached_valid_ = false;
    std::string diff_error_;
    aida::burp::comparer::diff_stats_t cached_stats_{};
    std::atomic<std::uint64_t> diff_generation_{0};
    std::atomic<bool> diff_pending_{false};
    std::uint64_t requested_a_ = 0;
    std::uint64_t requested_b_ = 0;
    int requested_mode_ = -1;
    std::shared_ptr<const diff_publication_t> publication_;
    std::uint64_t applied_generation_ = 0;
    bool slots_refreshing_ = false;
};

}
