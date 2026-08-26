#pragma once

#include <QModelIndex>
#include <QVariant>
#include <QVector>

#include <memory>
#include <vector>

#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class DnsModel : public RingTableModel<network_view::dns_entry_t> {
public:
    enum Column { Pid = 0, Type, Domain, Address, RCode, Ttl, ColumnCount };

    explicit DnsModel(QObject* parent = nullptr);

    void setFilterText(const QString& text);
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const network_view::dns_entry_t& row, int column, int role) const override;

private:
    QString filterText_;
};

class DnsPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit DnsPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void onBatch(std::shared_ptr<const std::vector<network_view::dns_entry_t>> batch,
                 quint64 trimmedFromFront);
    void refreshButtons();
    void updateEmptyStateForCurrentFilter();

    DnsModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* tableStack_ = nullptr;
    widgets::AidaStateView* emptyView_ = nullptr;
    widgets::AidaButton* startStopButton_ = nullptr;
    widgets::AidaButton* refreshButton_ = nullptr;
    QLineEdit* filterText_ = nullptr;
    QCheckBox* autoScroll_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QTimer* statePoll_ = nullptr;
    bool driverSettled_ = false;
    bool bulkLoaded_ = false;
};

}
