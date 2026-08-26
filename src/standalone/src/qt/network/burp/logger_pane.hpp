#pragma once

#include <QModelIndex>
#include <QVariant>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/network/burp/burp_logger.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"

namespace aida::ui {
enum class context_menu_open_origin_t : std::uint8_t;
}

class QContextMenuEvent;
class QLineEdit;
class QLabel;
class QSpinBox;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class LoggerModel : public SnapshotTableModel<aida::burp::logger::log_row_t> {
public:
    enum Column { Id = 0, Time, Method, Url, Status, Length, Latency, Source, ColumnCount };

    explicit LoggerModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const aida::burp::logger::log_row_t& row, int column, int role) const override;
};

class LoggerPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit LoggerPane(QWidget* parent = nullptr);
    ~LoggerPane() override;

protected:
    void onPaneShown() override;
    void onPaneHidden() override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onFilterEdited();
    void pollGeneration();
    void submitQuery();
    void applyQueryResult(std::shared_ptr<const std::vector<aida::burp::logger::log_row_t>> rows,
                          std::uint64_t filterSerial, std::uint64_t generation,
                          std::size_t total, std::size_t capacity);
    void exportCsv();
    void clearLog();
    void showContextForRow(int row, const QPoint& globalPos,
                           aida::ui::context_menu_open_origin_t origin);

    LoggerModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* tableStack_ = nullptr;
    widgets::AidaStateView* emptyView_ = nullptr;
    QLineEdit* methodEdit_ = nullptr;
    QLineEdit* hostEdit_ = nullptr;
    QLineEdit* urlEdit_ = nullptr;
    QSpinBox* statusMin_ = nullptr;
    QSpinBox* statusMax_ = nullptr;
    QLineEdit* sourceEdit_ = nullptr;
    QLineEdit* mimeEdit_ = nullptr;
    QSpinBox* limitSpin_ = nullptr;
    widgets::AidaButton* clearButton_ = nullptr;
    QLineEdit* exportPathEdit_ = nullptr;
    widgets::AidaButton* exportButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    QTimer* filterDebounce_ = nullptr;
    std::uint64_t filterSerial_ = 0;
    std::uint64_t queryGeneration_ = 0;
    std::uint64_t lastSeenGeneration_ = 0;
    std::uint64_t inFlightSerial_ = 0;
    std::size_t totalRows_ = 0;
    std::size_t capacity_ = 0;
};

}
