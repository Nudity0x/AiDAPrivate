#pragma once

#include "qt/debugger/debugger_pane_base.hpp"
#include "qt/debugger/debugger_snapshot_store.hpp"

#include "core/debugger/debugger_engine.hpp"

class QLabel;
class QSpinBox;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaLineEdit;
}

namespace aida::qt::debugger {

class StringsModel;

// Strings pane: Scan/Cancel drives find_strings_async (the long scan with the
// cancel token); a 250ms poll reads the GUI-safe progress atomics
// (strings_pages_scanned/strings_found_so_far) and the generation snapshot.
class StringsPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit StringsPane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onHidden() override;
    bool hasContentRows() const override;
    bool isContentLoading() const override;

private:
    void poll();
    void toggleScan();
    void jumpToSelectedHex();

    StringsModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    widgets::AidaLineEdit* filter_edit_ = nullptr;
    QSpinBox* min_length_spin_ = nullptr;
    widgets::AidaButton* scan_button_ = nullptr;
    QLabel* progress_label_ = nullptr;
    QTimer* timer_ = nullptr;
    SnapshotStore<debugger_engine::string_ref_t> snapshots_;
};

}
