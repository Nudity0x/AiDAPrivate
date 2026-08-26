#pragma once

#include <QModelIndex>
#include <QVariant>
#include <QVector>

#include <memory>
#include <vector>

#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"

class QLabel;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaSearchField;
class AidaStateView;
class AidaToggleSwitch;
class AidaButton;
}

namespace aida::qt::net {

class ConnectionsModel : public SnapshotTableModel<network_view::connection_entry_t> {
public:
    enum Column { Pid = 0, Proto, State, Local, Remote, ColumnCount };

    explicit ConnectionsModel(QObject* parent = nullptr);

    void setFilter(const QString& filter);
    QString filter() const { return filter_; }
    const network_view::connection_entry_t* visibleRowAt(int row) const noexcept;
    int visibleIndexForKey(quint32 pid, quint8 protocol, quint16 localPort,
                           quint16 remotePort) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const network_view::connection_entry_t& row, int column, int role) const override;
    void onRowsAdopted() override;

private:
    void rebuildVisible();

    QString filter_;
    QVector<int> visible_;
};

class ConnectionsPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit ConnectionsPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void onSnapshot(std::shared_ptr<const std::vector<network_view::connection_entry_t>> snapshot);
    void updateEmptyState();

    ConnectionsModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* tableStack_ = nullptr;
    widgets::AidaStateView* emptyView_ = nullptr;
    widgets::AidaSearchField* search_ = nullptr;
    widgets::AidaToggleSwitch* autoRefresh_ = nullptr;
    widgets::AidaButton* refreshButton_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QTimer* pendingPoll_ = nullptr;
    quint64 generation_ = 0;
    bool driverSettled_ = false;
};

}
