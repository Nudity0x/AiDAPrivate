#pragma once

#include <QModelIndex>
#include <QVariant>

#include <memory>
#include <vector>

#include "core/network/burp/scope.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"

class QComboBox;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QStackedLayout;
class QTableView;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class ScopeRulesModel : public SnapshotTableModel<aida::burp::scope::rule_t> {
public:
    enum Column { Kind = 0, Proto, Host, Path, Port, ColumnCount };

    explicit ScopeRulesModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const aida::burp::scope::rule_t& row, int column, int role) const override;
};

class ScopePane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit ScopePane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void refreshFromStore();
    void addRuleFromForm();
    void adoptStagedDraft();
    void toggleSelected();
    void removeSelected();
    void runUrlTest();
    void updateActionBar();
    void updateEmptyState();
    void openContextForRow(int row, const QPoint& globalPos);

    ScopeRulesModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* tableStack_ = nullptr;
    widgets::AidaStateView* emptyView_ = nullptr;
    QComboBox* kindCombo_ = nullptr;
    QLineEdit* protoEdit_ = nullptr;
    QLineEdit* hostEdit_ = nullptr;
    QLineEdit* portEdit_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    widgets::AidaButton* addButton_ = nullptr;
    widgets::AidaButton* toggleButton_ = nullptr;
    widgets::AidaButton* removeButton_ = nullptr;
    QLineEdit* testUrlEdit_ = nullptr;
    widgets::AidaButton* checkButton_ = nullptr;
    QLabel* testResultLabel_ = nullptr;
    widgets::AidaButton* clearAllButton_ = nullptr;
    widgets::AidaButton* reloadButton_ = nullptr;
    quint64 generation_ = 0;
};

}
