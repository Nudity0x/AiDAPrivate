#pragma once

#include <QAbstractTableModel>
#include <QObject>
#include <QString>
#include <QStyledItemDelegate>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <string>

#include "core/ui/task_center.hpp"

class QHBoxLayout;
class QLabel;
class QPushButton;
class QSplitter;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::ui {
enum class context_menu_open_origin_t : std::uint8_t;
}

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::widgets {
class AidaPill;
class AidaStateView;
}

namespace aida::qt::programming {

// GUI-thread owner of the task_center publish/observe loop: a 100 ms QTimer
// submits task_center::refresh() to the executor (the backend's single-flight
// makes duplicates harmless) and re-reads the atomic published snapshot; the
// GUI thread NEVER enumerates taskflow/analysis locks.
class AidaTaskCenterController : public QObject {
    Q_OBJECT
public:
    static AidaTaskCenterController& instance();
    static bool exists() noexcept;

    void install(docking::AidaDockHost* host);
    docking::AidaDockHost* host() const noexcept { return host_; }

    aida::ui::task_center::immutable_snapshot_ptr current() const noexcept { return snapshot_; }
    std::uint64_t generation() const noexcept { return generation_; }

    void stageDiagnosticSelection(const std::string& diagnostic_id);
    std::string consumeDiagnosticSelection();

Q_SIGNALS:
    void snapshotChanged(quint64 generation);

private:
    explicit AidaTaskCenterController(QObject* parent = nullptr);
    void onTick();

    docking::AidaDockHost* host_ = nullptr;
    aida::ui::task_center::immutable_snapshot_ptr snapshot_;
    std::uint64_t generation_ = 0;
    QTimer* timer_ = nullptr;
    std::string staged_diagnostic_selection_;
};

const char* task_state_name(aida::ui::task_center::task_state_t value) noexcept;
const char* diagnostic_severity_name(aida::ui::task_center::diagnostic_severity_t value) noexcept;
QString task_duration_text(std::uint64_t milliseconds);
bool task_state_active(aida::ui::task_center::task_state_t value) noexcept;

class AidaTaskTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int {
        task = 0, owner, target, state, progress, elapsed, action, count
    };

    explicit AidaTaskTableModel(QObject* parent = nullptr);
    void setSnapshot(aida::ui::task_center::immutable_snapshot_ptr snapshot);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const aida::ui::task_center::task_snapshot_t* rowAt(int row) const noexcept;
    int rowForTaskId(const std::string& id) const noexcept;
    std::uint64_t snapshotGeneration() const noexcept { return generation_; }

private:
    aida::ui::task_center::immutable_snapshot_ptr snapshot_;
    std::uint64_t generation_ = 0;
};

class AidaTaskRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit AidaTaskRowDelegate(AidaTaskTableModel* model, QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option, const QModelIndex& index) override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

Q_SIGNALS:
    void cancelRequested(const QString& task_id);
    void retryRequested(const QString& task_id);
    void focusRequested(const QString& task_id);

private:
    enum class cell_action_t : std::uint8_t { none, cancel, retry, focus };
    QRect actionRect(const QStyleOptionViewItem& option) const;
    cell_action_t actionFor(const aida::ui::task_center::task_snapshot_t& task) const noexcept;

    AidaTaskTableModel* model_ = nullptr;
};

class AidaTaskCenterView : public QWidget {
    Q_OBJECT
public:
    explicit AidaTaskCenterView(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onSnapshotChanged(quint64 generation);
    void refreshBadges();
    void reapplySelection();
    void openTaskContext(const QModelIndex& index,
                         aida::ui::context_menu_open_origin_t origin,
                         const QPoint& global_pos);
    void updateStatePage();

    AidaTaskTableModel* model_ = nullptr;
    AidaTaskRowDelegate* delegate_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    QWidget* content_ = nullptr;
    widgets::AidaPill* running_pill_ = nullptr;
    widgets::AidaPill* queued_pill_ = nullptr;
    widgets::AidaPill* cancelling_pill_ = nullptr;
    widgets::AidaPill* failed_pill_ = nullptr;
    widgets::AidaPill* interrupted_pill_ = nullptr;
    widgets::AidaPill* partial_pill_ = nullptr;
    QString selected_task_id_;
};

class AidaDiagnosticsTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int {
        severity = 0, summary, owner, target, age, status, count
    };

    explicit AidaDiagnosticsTableModel(QObject* parent = nullptr);
    void setSnapshot(aida::ui::task_center::immutable_snapshot_ptr snapshot);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const aida::ui::task_center::diagnostic_snapshot_t* rowAt(int row) const noexcept;
    int rowForDiagnosticId(const std::string& id) const noexcept;
    std::uint64_t snapshotGeneration() const noexcept { return generation_; }

private:
    aida::ui::task_center::immutable_snapshot_ptr snapshot_;
    std::uint64_t generation_ = 0;
};

class AidaDiagnosticsView : public QWidget {
    Q_OBJECT
public:
    explicit AidaDiagnosticsView(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onSnapshotChanged(quint64 generation);
    void refreshDetails();
    void reapplySelection();
    void openDiagnosticContext(const QModelIndex& index,
                               aida::ui::context_menu_open_origin_t origin,
                               const QPoint& global_pos);
    void updateStatePage();

    AidaDiagnosticsTableModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaPill* badge_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    QWidget* content_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* details_ = nullptr;
    QLabel* id_label_ = nullptr;
    QPushButton* focus_button_ = nullptr;
    QPushButton* log_button_ = nullptr;
    QPushButton* retry_button_ = nullptr;
    QPushButton* acknowledge_button_ = nullptr;
    QString selected_diagnostic_id_;
    QString summary_variant_;
};

}
