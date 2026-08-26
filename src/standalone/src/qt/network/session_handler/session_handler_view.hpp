#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QModelRoleDataSpan>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/network/burp/session_handler.hpp"
#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class QtHumanRequestEditor;
class ReviewedContextBanner;

class MacroListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name = 0, Id, Steps, LastRun, Ok, ColumnCount };

    explicit MacroListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    void adopt(std::vector<aida::burp::session_handler::macro_t> macros);
    const aida::burp::session_handler::macro_t* rowAt(int row) const noexcept;
    const aida::burp::session_handler::macro_t* findById(std::uint64_t id) const noexcept;

private:
    std::vector<aida::burp::session_handler::macro_t> rows_;
};

class SessionRuleListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name = 0, Match, Macro, Active, ColumnCount };

    explicit SessionRuleListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    void adopt(std::vector<aida::burp::session_handler::session_rule_t> rules);
    const aida::burp::session_handler::session_rule_t* rowAt(int row) const noexcept;

private:
    std::vector<aida::burp::session_handler::session_rule_t> rows_;
};

class MacroStepsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Index = 0, Label, Target, Extracts, ColumnCount };

    explicit MacroStepsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    void adopt(const aida::burp::session_handler::macro_t& macro);

private:
    std::vector<aida::burp::session_handler::macro_step_t> steps_;
};

// SessionHandlerView ports session_handler_view.cpp. Run-macro results are
// delivered to the pane through a queued QMetaObject::invokeMethod and stored
// as plain members post-delivery (replacing the run_mtx-protected map read
// during render); the last_run_macro_id == selected fence is preserved.
class SessionHandlerView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit SessionHandlerView(QWidget* parent = nullptr);
    ~SessionHandlerView() override;

    void stageReviewedContext(const network_view::artifact_identity_t& identity,
                              const std::vector<std::uint8_t>& requestBytes);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void ensureInitialized();
    void refreshLists();
    void refreshMacroEditor();
    void refreshExtractedValues();
    void refreshButtons();
    void createMacro();
    void deleteMacro();
    void addRule();
    void deleteRule();
    void renameMacro();
    void runMacroNow();
    void deleteSelectedStep();
    void addStep();
    void queueMacroUpdate(aida::burp::session_handler::macro_t macro);
    void drainStaged();
    void updateMacroEmptyState();
    void updateRuleEmptyState();
    void updateStepsEmptyState();
    void onMacroRunFinished(std::uint64_t macroId, bool ok,
                            std::map<std::string, std::string> values);

    ReviewedContextBanner* banner_ = nullptr;
    MacroListModel* macro_model_ = nullptr;
    QTableView* macro_table_ = nullptr;
    QStackedLayout* macro_stack_ = nullptr;
    widgets::AidaStateView* macro_empty_ = nullptr;
    QLineEdit* new_macro_name_ = nullptr;
    widgets::AidaButton* create_macro_button_ = nullptr;
    widgets::AidaButton* delete_macro_button_ = nullptr;
    SessionRuleListModel* rule_model_ = nullptr;
    QTableView* rule_table_ = nullptr;
    QStackedLayout* rule_stack_ = nullptr;
    widgets::AidaStateView* rule_empty_ = nullptr;
    QLineEdit* new_rule_name_ = nullptr;
    QComboBox* new_rule_match_ = nullptr;
    QLineEdit* new_rule_pattern_ = nullptr;
    QSpinBox* new_rule_status_ = nullptr;
    QLabel* rule_status_label_ = nullptr;
    QLineEdit* new_rule_macro_id_ = nullptr;
    QCheckBox* new_rule_repl_url_ = nullptr;
    QCheckBox* new_rule_repl_headers_ = nullptr;
    QCheckBox* new_rule_repl_body_ = nullptr;
    widgets::AidaButton* add_rule_button_ = nullptr;
    widgets::AidaButton* delete_rule_button_ = nullptr;
    QLineEdit* edit_macro_name_ = nullptr;
    widgets::AidaButton* rename_button_ = nullptr;
    widgets::AidaButton* run_button_ = nullptr;
    MacroStepsModel* steps_model_ = nullptr;
    QTableView* steps_table_ = nullptr;
    QStackedLayout* steps_stack_ = nullptr;
    widgets::AidaStateView* steps_empty_ = nullptr;
    widgets::AidaButton* delete_step_button_ = nullptr;
    QLineEdit* new_step_label_ = nullptr;
    QLineEdit* new_step_host_ = nullptr;
    QLineEdit* new_step_scheme_ = nullptr;
    QSpinBox* new_step_port_ = nullptr;
    QSpinBox* new_step_timeout_ = nullptr;
    QtHumanRequestEditor* step_editor_ = nullptr;
    QLineEdit* new_extract_name_ = nullptr;
    QComboBox* new_extract_from_ = nullptr;
    QLineEdit* new_extract_regex_ = nullptr;
    QSpinBox* new_extract_group_ = nullptr;
    widgets::AidaButton* add_step_button_ = nullptr;
    QLabel* extracted_label_ = nullptr;
    QPlainTextEdit* extracted_view_ = nullptr;
    QTimer* refresh_timer_ = nullptr;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> initialization_requested_{false};
    std::uint64_t selected_macro_id_ = 0;
    std::uint64_t selected_rule_id_ = 0;
    int edit_step_index_ = -1;
    aida::burp::session_handler::macro_t current_macro_{};
    bool current_macro_valid_ = false;
    std::map<std::string, std::string> last_run_values_;
    bool last_run_ok_ = false;
    std::uint64_t last_run_macro_id_ = 0;
    bool hooks_installed_ = false;
};

}
