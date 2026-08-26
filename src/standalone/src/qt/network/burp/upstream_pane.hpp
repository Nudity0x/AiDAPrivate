#pragma once

#include <QModelIndex>
#include <QVariant>

#include <memory>
#include <vector>

#include "core/network/burp/upstream_chain.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QStackedLayout;
class QTableView;
class QVBoxLayout;
class QWidget;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class UpstreamModel : public SnapshotTableModel<aida::burp::upstream::upstream_chain_t> {
public:
    enum Column { Label = 0, Hops, Active, ColumnCount };

    explicit UpstreamModel(QObject* parent = nullptr);

    std::uint64_t activeChainId = 0;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const aida::burp::upstream::upstream_chain_t& row, int column,
                      int role) const override;
};

class HopRowWidget : public QWidget {
    Q_OBJECT
public:
    explicit HopRowWidget(QWidget* parent = nullptr);

    aida::burp::upstream::upstream_hop_t toHop() const;
    bool hasHost() const;

Q_SIGNALS:
    void moveUpRequested(HopRowWidget* row);
    void moveDownRequested(HopRowWidget* row);
    void removeRequested(HopRowWidget* row);

private:
    QComboBox* typeCombo_ = nullptr;
    QLineEdit* hostEdit_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QLineEdit* userEdit_ = nullptr;
    QLineEdit* passEdit_ = nullptr;
};

class UpstreamPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit UpstreamPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;

private:
    void refreshFromStore();
    void addHopRow();
    void saveChain();
    void testSelectedChain();
    void applyTestResult(std::uint64_t chainId, bool success, QString detail,
                         std::uint64_t serial);
    void updateActionBar();

    QLineEdit* labelEdit_ = nullptr;
    widgets::AidaButton* addHopButton_ = nullptr;
    widgets::AidaButton* saveChainButton_ = nullptr;
    QWidget* hopsContainer_ = nullptr;
    QVBoxLayout* hopsLayout_ = nullptr;
    UpstreamModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaButton* activateButton_ = nullptr;
    widgets::AidaButton* deactivateButton_ = nullptr;
    widgets::AidaButton* testButton_ = nullptr;
    widgets::AidaButton* removeButton_ = nullptr;
    QLineEdit* testHostEdit_ = nullptr;
    QSpinBox* testPortSpin_ = nullptr;
    QLabel* testResultLabel_ = nullptr;
    QStackedLayout* tableStack_ = nullptr;
    widgets::AidaStateView* emptyView_ = nullptr;
    quint64 generation_ = 0;
    quint64 testSerial_ = 0;
    bool testPending_ = false;
};

}
