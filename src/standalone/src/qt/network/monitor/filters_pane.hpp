#pragma once

#include <QModelIndex>
#include <QVariant>

#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"

class QComboBox;
class QKeyEvent;
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

class FiltersModel : public SnapshotTableModel<network_view::filter_entry_t> {
public:
    enum Column { Rule = 0, ActionCol, Direction, Protocol, Pid, Port, Ip, ColumnCount };

    explicit FiltersModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const network_view::filter_entry_t& row, int column, int role) const override;
};

class FiltersPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit FiltersPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void refreshFromStore();
    void syncDraftToState();
    void refreshButtons();
    void removeSelected();
    void updateEmptyState();

    FiltersModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* tableStack_ = nullptr;
    widgets::AidaStateView* emptyView_ = nullptr;
    QComboBox* actionCombo_ = nullptr;
    QComboBox* directionCombo_ = nullptr;
    QComboBox* protocolCombo_ = nullptr;
    QLineEdit* pidEdit_ = nullptr;
    QLineEdit* portEdit_ = nullptr;
    QLineEdit* ipEdit_ = nullptr;
    widgets::AidaButton* addButton_ = nullptr;
    widgets::AidaButton* clearAllButton_ = nullptr;
    widgets::AidaButton* removeButton_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QTimer* statePoll_ = nullptr;
    bool driverSettled_ = false;
    quint64 generation_ = 0;
};

}
