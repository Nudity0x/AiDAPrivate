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

#include "core/network/burp/match_replace.hpp"
#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStackedLayout;
class QTableView;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class BoundedPlainTextEdit;
class BurpOperationRunner;
class ReviewedContextBanner;

// MatchReplaceRulesModel sits over the shared_ptr<const vector<rule_t>>
// publication; the requestRulesRefresh CAS port fetches the list on an
// executor and publishes via queued delivery (the row-entrance animation of
// the legacy paint path is dropped per plan 11 section 19).
class MatchReplaceRulesModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Label = 0, Target, Match, Hits, Active, ColumnCount };

    explicit MatchReplaceRulesModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    void adopt(std::shared_ptr<const std::vector<aida::burp::match_replace::rule_t>> rules);
    const aida::burp::match_replace::rule_t* rowAt(int row) const noexcept;
    const aida::burp::match_replace::rule_t* findById(std::uint64_t id) const noexcept;
    std::shared_ptr<const std::vector<aida::burp::match_replace::rule_t>> rules() const;

private:
    std::shared_ptr<const std::vector<aida::burp::match_replace::rule_t>> rules_ =
        std::make_shared<const std::vector<aida::burp::match_replace::rule_t>>();
};

class MatchReplaceView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit MatchReplaceView(QWidget* parent = nullptr);
    ~MatchReplaceView() override;

    void stageReviewedContext(const network_view::artifact_identity_t& identity,
                              bool responseTarget);

protected:
    void onPaneShown() override;

private:
    void submitInitialize();
    void submitRuleChange(int operation, aida::burp::match_replace::rule_t rule,
                          std::vector<aida::burp::match_replace::rule_t> reviewed,
                          int delta = 0);
    void requestRulesRefresh();
    void loadIntoEditor(const aida::burp::match_replace::rule_t& rule);
    void addRule();
    void saveRule();
    void openReview(int operation);
    void moveSelected(int delta);
    void runTest();
    void refreshEditorButtons();
    void updateEmptyState();
    void drainStaged();

    ReviewedContextBanner* banner_ = nullptr;
    QLabel* op_status_label_ = nullptr;
    widgets::AidaButton* retry_init_button_ = nullptr;
    QComboBox* new_target_ = nullptr;
    QComboBox* new_scheme_ = nullptr;
    QCheckBox* new_regex_ = nullptr;
    QCheckBox* new_ci_ = nullptr;
    QCheckBox* new_active_ = nullptr;
    widgets::AidaButton* add_button_ = nullptr;
    QLineEdit* new_label_ = nullptr;
    QLineEdit* new_match_ = nullptr;
    QLineEdit* new_replace_ = nullptr;
    QLineEdit* new_host_filter_ = nullptr;
    MatchReplaceRulesModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* table_stack_ = nullptr;
    widgets::AidaStateView* empty_view_ = nullptr;
    QLineEdit* edit_label_ = nullptr;
    QComboBox* edit_target_ = nullptr;
    QComboBox* edit_scheme_ = nullptr;
    QCheckBox* edit_regex_ = nullptr;
    QCheckBox* edit_ci_ = nullptr;
    QCheckBox* edit_active_ = nullptr;
    QLineEdit* edit_match_ = nullptr;
    QLineEdit* edit_replace_ = nullptr;
    QLineEdit* edit_host_filter_ = nullptr;
    widgets::AidaButton* save_button_ = nullptr;
    widgets::AidaButton* delete_button_ = nullptr;
    widgets::AidaButton* up_button_ = nullptr;
    widgets::AidaButton* down_button_ = nullptr;
    widgets::AidaButton* clear_all_button_ = nullptr;
    BoundedPlainTextEdit* test_sample_ = nullptr;
    widgets::AidaButton* run_test_button_ = nullptr;
    QPlainTextEdit* test_result_ = nullptr;

    BurpOperationRunner* runner_ = nullptr;
    bool initialized_ = false;
    bool initialization_requested_ = false;
    std::uint64_t selected_id_ = 0;
    std::atomic<bool> refresh_pending_{false};
    bool hooks_installed_ = false;
};

}
