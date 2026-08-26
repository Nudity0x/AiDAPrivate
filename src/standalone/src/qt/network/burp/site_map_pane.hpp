#pragma once

#include <QAbstractItemModel>
#include <QModelIndex>
#include <QSet>
#include <QVariant>

#include <memory>
#include <vector>

#include "core/network/burp/burp_events.hpp"
#include "core/network/network_view.hpp"
#include "qt/network/burp/site_map_bridge.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"

class QContextMenuEvent;
class QLabel;
class QPlainTextEdit;
class QSplitter;
class QTabBar;
class QTableView;
class QTimer;
class QTreeView;
class QStackedLayout;

namespace aida::qt::widgets {
class AidaNotice;
class AidaSearchField;
class AidaStateView;
}

namespace aida::qt::net {

class SiteMapTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit SiteMapTreeModel(QObject* parent = nullptr);

    void adopt(std::shared_ptr<const aida::burp::sitemap::site_map_tree_snapshot_t> snapshot);
    const aida::burp::sitemap::site_map_node_t* nodeFor(const QModelIndex& index) const noexcept;
    QModelIndex indexForKey(const QString& key) const;
    std::uint64_t queryRevision() const noexcept { return queryRevision_; }

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    std::shared_ptr<const aida::burp::sitemap::site_map_tree_snapshot_t> snapshot_;
    std::uint64_t queryRevision_ = 0;
};

class SiteMapExchangesModel : public SnapshotTableModel<aida::burp::sitemap::exchange_row_t> {
public:
    enum Column { Id = 0, Method, Path, Status, Size, Time, ColumnCount };

    explicit SiteMapExchangesModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const aida::burp::sitemap::exchange_row_t& row, int column, int role) const override;
};

class ExchangeDetailWidget : public QWidget {
    Q_OBJECT
public:
    explicit ExchangeDetailWidget(QWidget* parent = nullptr);

    void showExchange(std::uint64_t exchangeId);
    void clearExchange();

private:
    void rebuild();

    std::uint64_t exchangeId_ = 0;
    std::uint64_t detailCacheId_ = 0;
    aida::burp::exchange_observed_t current_;
    bool hasCurrent_ = false;
    QTabBar* requestTabs_ = nullptr;
    QStackedLayout* requestStack_ = nullptr;
    QTabBar* responseTabs_ = nullptr;
    QStackedLayout* responseStack_ = nullptr;
    QTabBar* detailTabs_ = nullptr;
    QStackedLayout* detailStack_ = nullptr;
    QPlainTextEdit* requestRaw_ = nullptr;
    QPlainTextEdit* requestBody_ = nullptr;
    QPlainTextEdit* responseRaw_ = nullptr;
    QPlainTextEdit* responseBody_ = nullptr;
    QWidget* requestHeaders_ = nullptr;
    QWidget* responseHeaders_ = nullptr;
    QWidget* metaPage_ = nullptr;
};

class SiteMapPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit SiteMapPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void pollTree();
    void onExchangeObserved(const aida::burp::exchange_observed_t& exchange);
    void onTreeSelectionChanged();
    void reloadExchanges();
    void updateExchangesEmptyState();
    void showNodeContext(const QModelIndex& index, const QPoint& globalPos,
                         aida::ui::context_menu_open_origin_t origin);
    void showExchangeContext(const QModelIndex& index, const QPoint& globalPos,
                             aida::ui::context_menu_open_origin_t origin);

    widgets::AidaSearchField* filterEdit_ = nullptr;
    widgets::AidaNotice* limitBanner_ = nullptr;
    widgets::AidaNotice* errorBanner_ = nullptr;
    QTreeView* tree_ = nullptr;
    SiteMapTreeModel* treeModel_ = nullptr;
    QTableView* exchanges_ = nullptr;
    QStackedLayout* exchangesStack_ = nullptr;
    widgets::AidaStateView* exchangesEmptyView_ = nullptr;
    SiteMapExchangesModel* exchangesModel_ = nullptr;
    ExchangeDetailWidget* detail_ = nullptr;
    QSplitter* mainSplitter_ = nullptr;
    QSplitter* rightSplitter_ = nullptr;
    QLabel* headerLabel_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    QTimer* filterDebounce_ = nullptr;
    QTimer* exchangeCoalesce_ = nullptr;
    std::uint64_t adoptedRevision_ = 0;
    QSet<QString> expandedKeys_;
    QString selectedHost_;
    quint16 selectedPort_ = 0;
    bool selectedTls_ = false;
    QString selectedPath_;
    quint64 selectedExchangeId_ = 0;
    std::uint64_t lastExchangeCount_ = 0;
    std::uint64_t exchangeGeneration_ = 0;
};

}
