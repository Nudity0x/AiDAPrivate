#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/analysis/decrypt_oracle.hpp"

class QAbstractTableModel;
class QLabel;
class QProgressBar;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaLineEdit;
class AidaStateView;
}

namespace aida::qt::analysis {

// Decrypt Oracle view (07 sec. 7.2), hosted as scan-hub page 3 (view.memory.decrypt).
// The engine (decrypt_oracle.hpp) is ZERO-CHANGE; the view polls the published
// snapshot on a view-local 66 ms timer (S11).
class QtDecryptOracleView : public QWidget {
    Q_OBJECT
public:
    explicit QtDecryptOracleView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void pollEngine();
    void refreshPresentation();
    void copySelectedString();
    void updateSummary(
        const std::shared_ptr<const std::vector<decrypt_oracle::decrypted_string_t>>&
            results);

    QAbstractTableModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaLineEdit* address_edit_ = nullptr;
    widgets::AidaLineEdit* size_edit_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* stat_found_ = nullptr;
    QLabel* stat_avg_conf_ = nullptr;
    QLabel* stat_high_conf_ = nullptr;
    QLabel* stat_total_len_ = nullptr;
    QTimer* timer_ = nullptr;
    std::shared_ptr<const std::vector<decrypt_oracle::decrypted_string_t>>
        summary_source_;
    int selected_row_ = -1;
};

}
