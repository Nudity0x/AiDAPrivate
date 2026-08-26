#pragma once

#include <QModelIndex>
#include <QVariant>

#include <memory>

#include "core/network/network_view.hpp"
#include "core/network/ssl_keylog.hpp"
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
class AidaNotice;
class AidaStateView;
}

namespace aida::qt::net {

class KeylogModel : public SnapshotTableModel<ssl_keylog::keylog_entry> {
public:
    enum Column { Time = 0, Label, ClientRandom, Secret, ColumnCount };

    explicit KeylogModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const ssl_keylog::keylog_entry& row, int column, int role) const override;
};

class KeylogPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit KeylogPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;

private:
    void onSnapshot(std::shared_ptr<const network_view::keylog_runtime_snapshot_t> snapshot);
    void refreshButtons();
    void syncFormToState();
    void updateDetailRow();
    void updateEmptyState();

    KeylogModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* tableStack_ = nullptr;
    widgets::AidaStateView* emptyView_ = nullptr;
    QLineEdit* exePath_ = nullptr;
    QLineEdit* argsEdit_ = nullptr;
    QLineEdit* watchPath_ = nullptr;
    widgets::AidaButton* browseExeButton_ = nullptr;
    widgets::AidaButton* launchButton_ = nullptr;
    widgets::AidaButton* watchFileButton_ = nullptr;
    widgets::AidaButton* watchTypedButton_ = nullptr;
    widgets::AidaButton* stopButton_ = nullptr;
    widgets::AidaButton* clearButton_ = nullptr;
    QLabel* watchingLabel_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QLabel* detailLabel_ = nullptr;
    widgets::AidaButton* copyClientRandom_ = nullptr;
    widgets::AidaButton* copySecret_ = nullptr;
    widgets::AidaButton* copyLine_ = nullptr;
    QTimer* pendingPoll_ = nullptr;
    widgets::AidaNotice* errorNotice_ = nullptr;
    quint64 generation_ = 0;
    bool watching_ = false;
    bool watchStartPending_ = false;
    bool lastStartWasLaunch_ = false;
};

}
