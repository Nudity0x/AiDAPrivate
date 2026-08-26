#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QStyledItemDelegate>
#include <QVariant>
#include <QVector>

#include <memory>
#include <vector>

#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"

class QLabel;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class BandwidthModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Pid = 0, Name, BytesIn, BytesOut, RateIn, RateOut, Trend, ColumnCount };
    static constexpr int SparklineRole = Qt::UserRole + 1;

    explicit BandwidthModel(QObject* parent = nullptr);

    void adoptSnapshot(std::shared_ptr<const std::vector<network_view::bw_entry_t>> snapshot);
    const network_view::bw_entry_t* rowAt(int row) const noexcept;
    int rowForPid(quint32 pid) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QVariant cellData(const network_view::bw_entry_t& row, int column, int role) const;

    QVector<network_view::bw_entry_t> rows_;
    QVector<quint32> pids_;
};

class SparklineDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit SparklineDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view,
                 const QStyleOptionViewItem& option, const QModelIndex& index) override;
};

class BandwidthPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit BandwidthPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void onSnapshot(std::shared_ptr<const std::vector<network_view::bw_entry_t>> snapshot);
    void refreshButtons();
    void updateStateView();

    BandwidthModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* tableStack_ = nullptr;
    widgets::AidaStateView* emptyView_ = nullptr;
    widgets::AidaButton* startStopButton_ = nullptr;
    QLabel* pendingLabel_ = nullptr;
    QTimer* statePoll_ = nullptr;
    bool driverSettled_ = false;
};

}
