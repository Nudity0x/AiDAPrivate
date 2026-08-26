#pragma once

#include <QAbstractListModel>
#include <QAbstractTableModel>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/ui/programming_tasks.hpp"
#include "core/ui/task_center.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QSplitter;
class QTableView;
class QTimer;
class QToolButton;

namespace aida::ui {
enum class context_menu_open_origin_t : std::uint8_t;
}

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::widgets {
class AidaNotice;
}

namespace aida::qt::programming {

// GUI-thread controller over the (Qt-free) aida::ui::programming_tasks model:
// 250 ms tick drives the backend's ensure_initialized (dispatch-failure and
// persistence completion/rollback arms), caches the catalog snapshot DTO, and
// opens the configuration/run-review/delete dialogs at the call sites.
class AidaProgrammingTasksController : public QObject {
    Q_OBJECT
public:
    static AidaProgrammingTasksController& instance();
    static bool exists() noexcept;

    void install(docking::AidaDockHost* host, QWidget* dialog_parent);
    docking::AidaDockHost* host() const noexcept { return host_; }

    const aida::ui::programming_tasks::catalog_snapshot_t& catalog() const noexcept {
        return catalog_;
    }
    std::uint64_t observedGeneration() const noexcept { return observed_generation_; }

    aida::ui::programming_tasks::operation_result_t beginEdit(int index,
        aida::ui::programming_tasks::configuration_draft_t& draft);
    aida::ui::programming_tasks::operation_result_t beginCreate(
        aida::ui::programming_tasks::configuration_draft_t& draft);
    aida::ui::programming_tasks::operation_result_t beginDuplicate(int index,
        aida::ui::programming_tasks::configuration_draft_t& draft);
    aida::ui::programming_tasks::operation_result_t saveDraft(
        const aida::ui::programming_tasks::configuration_draft_t& draft);
    void discardDraft();
    aida::ui::programming_tasks::operation_result_t revertDraft(
        aida::ui::programming_tasks::configuration_draft_t& draft);
    aida::ui::programming_tasks::operation_result_t deleteSelected();

    aida::ui::programming_tasks::operation_result_t selectConfiguration(int index,
        bool persist);
    void setChannel(const std::string& channel);

    void openConfigurationEditor();
    void openDeleteReview();
    void openRunReview();
    void presentPendingRunReview();

    std::string scriptActionError() const { return script_action_error_; }
    void clearScriptActionError() { script_action_error_.clear(); }
    void setScriptActionError(std::string error) { script_action_error_ = std::move(error); }

Q_SIGNALS:
    void catalogChanged();
    void editorStateChanged();
    void channelSelectionChanged();

private:
    explicit AidaProgrammingTasksController(QObject* parent = nullptr);
    void onTick();
    void pullCatalog();

    docking::AidaDockHost* host_ = nullptr;
    QWidget* dialog_parent_ = nullptr;
    aida::ui::programming_tasks::catalog_snapshot_t catalog_;
    std::uint64_t observed_generation_ = 0;
    std::string observed_error_;
    bool observed_editor_dirty_ = false;
    bool observed_editor_save_in_flight_ = false;
    int observed_editor_selected_ = -2;
    bool observed_editor_creating_ = false;
    std::string observed_validation_;
    QTimer* timer_ = nullptr;
    std::string script_action_error_;
    std::optional<aida::ui::programming_tasks::configuration_draft_t> pending_draft_;
    QPointer<QDialog> config_dialog_;
    QPointer<QDialog> delete_dialog_;
    QPointer<QDialog> review_dialog_;
};

// The strip embedded at the top of the Output view: configuration combo,
// Run.../Configure.../Cancel/Problems(n) and the task-output channel combo.
class AidaTaskControlsStrip : public QWidget {
    Q_OBJECT
public:
    explicit AidaTaskControlsStrip(QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void refresh();
    void rebuildConfigurationCombo();
    void rebuildChannelCombo();
    void updateCompactMode();

    QComboBox* configuration_combo_ = nullptr;
    QPushButton* run_button_ = nullptr;
    QPushButton* configure_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* problems_button_ = nullptr;
    QComboBox* channel_combo_ = nullptr;
    QToolButton* channel_button_ = nullptr;
    QMenu* channel_menu_ = nullptr;
    QLabel* error_label_ = nullptr;
    QLabel* loading_label_ = nullptr;
    bool updating_combos_ = false;
    bool compact_ = false;
};

class AidaConfigurationListModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AidaConfigurationListModel(QObject* parent = nullptr);
    void setCatalog(aida::ui::programming_tasks::catalog_snapshot_t catalog);
    void setFilter(const QString& filter);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    const aida::ui::programming_tasks::configuration_t* rowAt(int row) const noexcept;
    int rowForId(const std::string& id) const noexcept;
    int sourceIndex(int view_row) const noexcept;

private:
    aida::ui::programming_tasks::catalog_snapshot_t catalog_;
    std::string filter_;
    std::vector<int> visible_;
};

class AidaScriptRunsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int { script = 0, state, stage, actions, count };

    explicit AidaScriptRunsModel(QObject* parent = nullptr);
    void setSnapshot(aida::ui::task_center::immutable_snapshot_ptr snapshot);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const aida::ui::task_center::task_snapshot_t* rowAt(int row) const noexcept;
    std::uint64_t snapshotGeneration() const noexcept { return generation_; }

private:
    aida::ui::task_center::immutable_snapshot_ptr snapshot_;
    std::vector<const aida::ui::task_center::task_snapshot_t*> rows_;
    std::uint64_t generation_ = 0;
};

class AidaAutomationScriptsView : public QWidget {
    Q_OBJECT
public:
    explicit AidaAutomationScriptsView(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh();
    void refreshDetail();
    void openConfigurationContext(int catalog_index,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos);
    void openRunContext(const QModelIndex& index,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos);
    void runSelected();
    void editSelected();
    void duplicateSelected();

    AidaConfigurationListModel* catalog_model_ = nullptr;
    QListView* catalog_view_ = nullptr;
    QLineEdit* filter_edit_ = nullptr;
    QPushButton* new_button_ = nullptr;
    QPushButton* reload_button_ = nullptr;
    QLabel* loading_label_ = nullptr;
    widgets::AidaNotice* error_strip_ = nullptr;
    QLabel* stale_hint_ = nullptr;
    widgets::AidaNotice* script_error_strip_ = nullptr;
    QLabel* detail_name_ = nullptr;
    QLabel* detail_meta_ = nullptr;
    QPushButton* detail_run_ = nullptr;
    QPushButton* detail_edit_ = nullptr;
    QPushButton* detail_duplicate_ = nullptr;
    QLabel* prop_source_ = nullptr;
    QLabel* prop_scope_ = nullptr;
    QLabel* prop_command_ = nullptr;
    QLabel* prop_workdir_ = nullptr;
    QLabel* prop_output_ = nullptr;
    QLabel* prop_matcher_ = nullptr;
    AidaScriptRunsModel* runs_model_ = nullptr;
    QTableView* runs_table_ = nullptr;
    QSplitter* splitter_ = nullptr;
    std::string selected_run_id_;
    std::uint64_t selected_run_generation_ = 0;
};

class AidaTaskConfigurationDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    explicit AidaTaskConfigurationDialog(QWidget* parent = nullptr);
    void openAtSelection();
    void openForDraft(const aida::ui::programming_tasks::configuration_draft_t& draft);
    void reject() override;

private:
    void onSelectionChanged(int row);
    void loadDraft(const aida::ui::programming_tasks::configuration_draft_t& draft);
    aida::ui::programming_tasks::configuration_draft_t currentDraft() const;
    void refreshButtons();
    void markDirty();

    AidaConfigurationListModel* list_model_ = nullptr;
    QListView* list_ = nullptr;
    QLineEdit* name_ = nullptr;
    QPlainTextEdit* command_ = nullptr;
    QLineEdit* cwd_ = nullptr;
    QLineEdit* channel_ = nullptr;
    QComboBox* kind_ = nullptr;
    QComboBox* matcher_ = nullptr;
    QLabel* origin_label_ = nullptr;
    QPushButton* open_source_button_ = nullptr;
    QLabel* creating_label_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* discard_button_ = nullptr;
    QPushButton* delete_button_ = nullptr;
    QPushButton* revert_button_ = nullptr;
    QPushButton* reload_button_ = nullptr;
    QPushButton* close_button_ = nullptr;
    QPushButton* add_button_ = nullptr;
    bool loading_fields_ = false;
};

class AidaDeleteConfigurationDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    explicit AidaDeleteConfigurationDialog(QWidget* parent = nullptr);

private:
    QLabel* error_ = nullptr;
    QPushButton* delete_button_ = nullptr;
};

class AidaRunReviewDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    explicit AidaRunReviewDialog(QWidget* parent = nullptr);
    void openForPending();

private:
    QLabel* title_label_ = nullptr;
    QLabel* command_ = nullptr;
    QLabel* workdir_ = nullptr;
    QLabel* channel_ = nullptr;
    QLabel* matcher_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* run_button_ = nullptr;
};

}
