#pragma once

#include <QPointer>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "qt/analysis/qt_analysis_list_model.hpp"

class QLabel;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaSearchField;
class AidaStateView;
}

namespace aida::qt::analysis {

class QtWorkspaceContext;
struct QtAnalysisListState;

// Generic analysis list view (imports/exports/names/strings/segments/
// local_types), 07 sec. 3. One instance per domain, hosted as a singleton dock.
class QtAnalysisListView : public QWidget {
    Q_OBJECT
public:
    QtAnalysisListView(analysis_list_domain_t domain, QWidget* parent = nullptr);

    QtAnalysisListModel* model() const noexcept { return model_; }
    void copySelectedName();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebindContext(QtWorkspaceContext* context);
    void scheduleRebuild();
    void adoptSnapshot(std::shared_ptr<const analysis_list_snapshot_t> snapshot,
                       std::uint64_t serial);
    void refreshPresentation();
    void showRowMenu(const QPoint& global_pos, int view_row);
    std::size_t remapSelection(const analysis_list_snapshot_t& snapshot) const;

    analysis_list_domain_t domain_;
    QtWorkspaceContext* context_ = nullptr;
    QtAnalysisListState* state_ = nullptr;
    QtAnalysisListModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaSearchField* filter_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* refreshing_label_ = nullptr;
    QTimer* filter_debounce_ = nullptr;
    QMetaObject::Connection context_connection_;
    QMetaObject::Connection poller_connection_;
};

}
