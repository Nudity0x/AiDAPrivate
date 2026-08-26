#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QModelRoleDataSpan>
#include <QVariant>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qt/network/network_pane_base.hpp"
#include "qt/network/websocket/ws_frame_store.hpp"

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

// WsFramesModel is the paged capture-frame model (plan 11 section 13.1).
// rowCount is the materialized row count; canFetchMore is true while the
// store's append_serial exceeds the last materialized serial; fetchMore pulls
// the next 128-frame page in one beginInsertRows batch (contract
// qabstractitemmodel.cpp:2889-2914; default no-ops/contract
// qabstractitemmodel.cpp:2427-2446; the view triggers fetching at the
// scrollbar maximum qabstractitemview.cpp:2893-2894). Eviction (the store's
// dropped_total advancing) and Clear reset the model; selection is restored
// by frame identity (timestamp, exchange_id, payload hash) instead of the
// legacy fragile deque index.
class WsFramesModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Dir = 0, Host, Opcode, Size, Preview, ColumnCount };
    static constexpr std::size_t kPageSize = 128;

    explicit WsFramesModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    void setFilterText(const QString& text);
    void clearAll();
    bool resyncAfterEviction();

    const WsFrameStore::entry_t* frameAt(int row) const noexcept;
    std::uint64_t lastSeenSerial() const noexcept { return last_seen_serial_; }
    std::uint64_t lastDroppedTotal() const noexcept { return last_dropped_total_; }

private:
    bool matchesFilter(const WsFrameStore::entry_t& frame) const;
    void refilter();

    std::vector<WsFrameStore::entry_ptr_t> materialized_;
    std::vector<int> visible_;
    QString filter_;
    std::uint64_t last_seen_serial_ = 0;
    std::uint64_t last_dropped_total_ = 0;
};

class WsPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit WsPane(QWidget* parent = nullptr);
    ~WsPane() override;

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void fetchLiveEdge();
    void updateDetail();
    void updateEmptyState();
    void openContextMenu(const QPoint& viewportPos);
    void openContextForSelection(network_view::exchange_context_origin_t origin);
    void clearFrames();

    WsFramesModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* table_stack_ = nullptr;
    widgets::AidaStateView* empty_view_ = nullptr;
    QLineEdit* filter_edit_ = nullptr;
    widgets::AidaButton* clear_button_ = nullptr;
    QCheckBox* auto_scroll_ = nullptr;
    QPlainTextEdit* detail_ = nullptr;
    QTimer* live_timer_ = nullptr;
    std::uint64_t selected_timestamp_ = 0;
    std::uint64_t selected_exchange_id_ = 0;
    std::uint64_t selected_payload_hash_ = 0;
    bool has_selection_ = false;
};

}
