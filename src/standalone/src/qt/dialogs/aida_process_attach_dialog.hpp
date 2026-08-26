#pragma once

#include <QAbstractTableModel>
#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/runtime/standalone_driver.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTimer;

namespace aida::qt::dialogs {

class AidaProcessTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        PidColumn = 0,
        NameColumn,
        WindowTitleColumn,
        ColumnCount
    };

    explicit AidaProcessTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void applySnapshot(std::vector<driver_bridge::process_info_t> processes,
                       std::uint64_t epoch);
    void setFilter(const QString& filter);
    const driver_bridge::process_info_t* processAt(int row) const;
    int rowForPid(std::uint32_t pid) const;
    std::uint64_t appliedEpoch() const noexcept { return applied_epoch_; }
    int totalCount() const noexcept { return static_cast<int>(processes_.size()); }
    bool filtering() const noexcept { return !filter_.empty(); }

private:
    void rebuildFiltered();

    std::vector<driver_bridge::process_info_t> processes_;
    std::vector<int> filtered_;
    std::string filter_;
    std::uint64_t applied_epoch_ = 0;
    std::string cached_filter_;
    std::uint64_t cached_filter_epoch_ = 0;
};

class AidaProcessAttachDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    explicit AidaProcessAttachDialog(QWidget* parent = nullptr);
    ~AidaProcessAttachDialog() override;

    void openFresh();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void scheduleRefresh(bool immediate);
    void onRefreshFired();
    void applyPendingIfReady();
    void onAttachClicked();
    void requestClose();
    void updateStatusLine();

    AidaProcessTableModel* model_ = nullptr;
    QLineEdit* filter_edit_ = nullptr;
    QTableView* table_ = nullptr;
    QPushButton* attach_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    QTimer* apply_timer_ = nullptr;
    bool closing_ = false;
};

struct process_attach_hooks_t {
    std::function<void(const char* view_id)> focus_view;
    std::function<void(const std::string& text)> push_output_line;
};

void set_process_attach_hooks(process_attach_hooks_t hooks);
bool process_attach_active();

}
