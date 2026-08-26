#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QModelRoleDataSpan>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/tools/script_engine.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStackedLayout;
class QSyntaxHighlighter;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class BoundedPlainTextEdit;

struct script_row_t {
    std::string name;
    std::string path;
    bool enabled = true;
    bool loaded = false;
};

// ScriptLibraryModel backs the Script Library table (name, UNLOADED/ENABLED/
// PAUSED state text, filename; render_scripting network_view.cpp:10843-10931
// pre-migration). All mutations happen on the GUI thread inside the fenced
// operation completion slots.
class ScriptLibraryModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name = 0, State, File, ColumnCount };

    explicit ScriptLibraryModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    const script_row_t* rowAt(int row) const noexcept;
    int indexOfName(const std::string& name) const;
    void upsertLoaded(const std::string& name, const std::string& path, int& rowOut);
    bool markUnloaded(const std::string& name);
    bool setEnabled(const std::string& name, bool enabled);

private:
    std::vector<script_row_t> rows_;
};

// ScriptingPane ports render_scripting plus the request_script_* operation
// functions and the script runtime snapshot machinery
// (network_view.cpp:6209-7200 pre-migration). Operation fencing is the exact
// legacy contract: operation_pending_ CAS single-flight, then serial =
// operation_serial_.fetch_add(1)+1, task-center registration, executor
// diagnostics/bounded_task body, terminal update, and a queued completion
// delivered to the pane that is dropped when the serial no longer matches
// (QMetaObject::invokeMethod Qt::QueuedConnection replaces
// post_network_ui_completion; receiver-destroyed drop qobject.cpp:201-202).
class ScriptingPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit ScriptingPane(QWidget* parent = nullptr);
    ~ScriptingPane() override;

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    struct runtime_snapshot_t {
        bool initialized = false;
        std::size_t hook_count = 0;
        std::vector<script_engine::log_entry> log;
    };

    std::string registerNetworkOperation(const char* action, const char* label,
                                         std::string target);
    static void finishNetworkOperation(const std::string& id, bool success,
                                       std::string stage, std::string summary);

    void requestRuntimeSnapshot(bool force = false);
    void requestScriptLoad(std::string path);
    void requestScriptUnload(std::string name);
    void requestScriptToggle(std::string name, bool enable);
    void requestScriptEvaluate(std::string source);
    void requestScriptConsole(std::string command);
    void requestScriptLogClear();
    void requestScriptOpen(std::string path);

    void loadScriptFromDialog();
    void applySnapshot(const std::shared_ptr<const runtime_snapshot_t>& snapshot);
    void rebuildLogText(const std::shared_ptr<const runtime_snapshot_t>& snapshot);
    void refreshLibraryMeta();
    void refreshEditorMeta();
    void refreshButtons();
    void updateEmptyState();

    ScriptLibraryModel* library_model_ = nullptr;
    QTableView* library_table_ = nullptr;
    QStackedLayout* library_stack_ = nullptr;
    widgets::AidaStateView* library_empty_ = nullptr;
    QLabel* library_meta_ = nullptr;
    QLabel* engine_dot_ = nullptr;
    widgets::AidaButton* load_button_ = nullptr;
    widgets::AidaButton* unload_button_ = nullptr;
    widgets::AidaButton* toggle_button_ = nullptr;
    widgets::AidaButton* open_button_ = nullptr;
    BoundedPlainTextEdit* editor_ = nullptr;
    QLabel* editor_meta_ = nullptr;
    widgets::AidaButton* run_button_ = nullptr;
    widgets::AidaButton* clear_editor_button_ = nullptr;
    widgets::AidaButton* copy_button_ = nullptr;
    QLineEdit* console_input_ = nullptr;
    widgets::AidaButton* exec_button_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QLabel* log_status_ = nullptr;
    QLabel* op_status_ = nullptr;
    QSyntaxHighlighter* log_highlighter_ = nullptr;
    QCheckBox* auto_scroll_ = nullptr;
    widgets::AidaButton* clear_log_button_ = nullptr;

    std::atomic<bool> operation_pending_{false};
    std::atomic<bool> open_pending_{false};
    std::atomic<std::uint64_t> operation_serial_{0};
    std::atomic<bool> snapshot_pending_{false};
    std::atomic<std::uint64_t> snapshot_requested_ms_{0};
    std::shared_ptr<const runtime_snapshot_t> runtime_snapshot_;
    std::size_t applied_log_size_ = 0;
    std::uint64_t applied_log_last_wall_seconds_ = 0;
    std::uint32_t applied_log_last_repeat_ = 0;

    QTimer* snapshot_timer_ = nullptr;
    std::uint64_t operation_sequence_ = 0;
};

}
