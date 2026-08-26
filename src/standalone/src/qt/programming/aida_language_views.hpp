#pragma once

#include <QAbstractTableModel>
#include <QDialog>
#include <QObject>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/editor/programming_language_service.hpp"
#include "core/debugger/source_debug_service.hpp"
#include "core/ui/task_center.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::ui {
enum class context_menu_open_origin_t : std::uint8_t;
}

namespace aida::qt::bridge {
class AidaDialog;
}

namespace aida::qt::widgets {
class AidaStateView;
class AidaViewHeader;
}

namespace aida::qt::programming {

class AidaOutputPane;

// GUI-thread poller over the (Qt-free) programming_language_service: 250 ms
// QTimer runs begin_frame() (the service's own try-lock-driven pump) and reads
// the published capability slots + workspace index, emitting change signals on
// generation/request-id transitions only.
class AidaLanguageServiceBridge : public QObject {
    Q_OBJECT
public:
    static AidaLanguageServiceBridge& instance();
    static bool exists() noexcept;

    void install();
    void shutdown();

    aida::editor::language_service::query_snapshot_t latestResult(
        std::initializer_list<aida::editor::language_service::capability_kind_t> kinds) const;
    aida::editor::language_service::query_snapshot_t result(
        aida::editor::language_service::capability_kind_t kind) const;
    std::shared_ptr<const code_index::published_index_t> indexSnapshot() const {
        return index_snapshot_;
    }
    QString indexStatus() const { return index_status_; }
    source_debug_service::snapshot_ptr sourceDebugSnapshot() const { return source_debug_; }

Q_SIGNALS:
    void resultChanged(int capability_kind);
    void indexChanged();
    void sourceDebugChanged();

private:
    explicit AidaLanguageServiceBridge(QObject* parent = nullptr);
    void onTick();

    QTimer* timer_ = nullptr;
    std::shared_ptr<const code_index::published_index_t> index_snapshot_;
    QString index_status_;
    source_debug_service::snapshot_ptr source_debug_;
    std::uint64_t index_generation_ = 0;
    std::uint64_t source_debug_generation_ = 0;
    struct slot_state_t {
        std::uint64_t request_id = 0;
        std::uint64_t request_generation = 0;
    };
    slot_state_t slots_[16] = {};
};

// Workspace Text Index status line (verbatim text from the retired
// programming_language_views render_index_status).
class AidaIndexStatusStrip : public QWidget {
    Q_OBJECT
public:
    explicit AidaIndexStatusStrip(QWidget* parent = nullptr);
    void refresh();

private:
    QLabel* status_ = nullptr;
    QLabel* stats_ = nullptr;
};

class AidaOutlineModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int { symbol = 0, kind, line, count };

    explicit AidaOutlineModel(QObject* parent = nullptr);
    void setSnapshot(aida::editor::language_service::query_snapshot_t snapshot);
    void setFilter(const QString& filter);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const aida::editor::language_service::symbol_t* rowAt(int row) const noexcept;
    aida::editor::language_service::query_snapshot_t snapshot() const noexcept {
        return snapshot_;
    }

private:
    void rebuildVisible();

    aida::editor::language_service::query_snapshot_t snapshot_;
    std::string filter_;
    std::vector<std::size_t> visible_;
};

class AidaOutlineView : public QWidget {
    Q_OBJECT
public:
    explicit AidaOutlineView(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onResultChanged(int kind);
    void refreshPresentation();
    void openRowContext(const QModelIndex& index,
                        aida::ui::context_menu_open_origin_t origin,
                        const QPoint& global_pos);
    void activateRow(const QModelIndex& index);

    AidaOutlineModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaViewHeader* header_ = nullptr;
    AidaIndexStatusStrip* strip_ = nullptr;
    QLineEdit* filter_edit_ = nullptr;
    QPushButton* rebuild_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QLabel* truncated_label_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    QWidget* content_ = nullptr;
    aida::editor::language_service::query_snapshot_t shown_;
};

// References / provider-results table: 7 payload modes in the documented
// priority order, per-mode column sets, in-model filter.
class AidaReferencesModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Mode : std::uint8_t {
        none, locations, symbols, completions, information, diagnostics,
        proposed_edits, code_actions
    };

    explicit AidaReferencesModel(QObject* parent = nullptr);
    void setSnapshot(aida::editor::language_service::query_snapshot_t snapshot);
    void setFilter(const QString& filter);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    Mode mode() const noexcept { return mode_; }
    aida::editor::language_service::location_t locationAt(int row) const;
    aida::editor::language_service::query_snapshot_t snapshot() const noexcept {
        return snapshot_;
    }

private:
    void rebuildVisible();

    aida::editor::language_service::query_snapshot_t snapshot_;
    Mode mode_ = Mode::none;
    std::string filter_;
    std::vector<std::size_t> visible_;
};

class AidaReferencesView : public QWidget {
    Q_OBJECT
public:
    explicit AidaReferencesView(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onResultChanged(int kind);
    void refreshPresentation();
    void submitQuery();
    void openRowContext(const QModelIndex& index,
                        aida::ui::context_menu_open_origin_t origin,
                        const QPoint& global_pos);
    void activateRow(const QModelIndex& index);

    AidaReferencesModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaViewHeader* header_ = nullptr;
    AidaIndexStatusStrip* strip_ = nullptr;
    QLineEdit* query_edit_ = nullptr;
    QPushButton* find_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QLabel* summary_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    QWidget* content_ = nullptr;
    aida::editor::language_service::query_snapshot_t shown_;
    std::uint64_t adopted_request_id_ = 0;
};

class AidaRenameDialog : public QDialog {
    Q_OBJECT
public:
    explicit AidaRenameDialog(QWidget* parent = nullptr);
    void openFor(const std::string& identifier);

private:
    void requestEdits();
    void validateInput();

    QLineEdit* identifier_ = nullptr;
    QLineEdit* replacement_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* request_button_ = nullptr;
    bool submit_error_active_ = false;
};

void open_rename_dialog(QWidget* parent);

class AidaSourceDebugTaskModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int { operation = 0, stage, target, progress, count };

    explicit AidaSourceDebugTaskModel(QObject* parent = nullptr);
    void setSnapshot(aida::ui::task_center::immutable_snapshot_ptr snapshot);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    aida::ui::task_center::immutable_snapshot_ptr snapshot_;
    std::vector<const aida::ui::task_center::task_snapshot_t*> rows_;
    std::uint64_t generation_ = 0;
};

class AidaSourceDebugConsole : public QWidget {
    Q_OBJECT
public:
    explicit AidaSourceDebugConsole(QWidget* parent = nullptr);

private:
    void onTick();
    void refreshStatus();
    void refreshActions();

    widgets::AidaViewHeader* header_ = nullptr;
    QPushButton* mixed_button_ = nullptr;
    QPushButton* rebind_button_ = nullptr;
    QPushButton* problems_button_ = nullptr;
    QPushButton* output_button_ = nullptr;
    QLabel* status_line_ = nullptr;
    QLabel* pending_label_ = nullptr;
    QLabel* error_label_ = nullptr;
    QLabel* location_label_ = nullptr;
    QLabel* location_detail_ = nullptr;
    QLabel* bind_hint_ = nullptr;
    AidaSourceDebugTaskModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    AidaOutputPane* output_pane_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QTimer* timer_ = nullptr;
    std::uint64_t task_generation_ = 0;
};

}
