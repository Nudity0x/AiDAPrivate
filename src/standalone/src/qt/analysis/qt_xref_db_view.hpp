#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>

#include "qt/analysis/qt_xref_model.hpp"

class QComboBox;
class QLabel;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaLineEdit;
class AidaSearchField;
class AidaStateView;
}

namespace aida::qt::analysis {

class QtWorkspaceContext;

// Cross References view (07 sec. 7.1): address query + To/From mode, two-worker
// serial fencing (query -> filter), S1-S5 model rules.
class QtXrefDbView : public QWidget {
    Q_OBJECT
public:
    explicit QtXrefDbView(QWidget* parent = nullptr);

    // Engine-hook entry point (analysis_host_hooks submit_xref_query).
    bool queryAddress(std::uint64_t runtime_address, bool query_to);

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebindContext(QtWorkspaceContext* context);
    void submitQuery();
    void requestFilter();
    void adoptResults();
    void refreshPresentation();
    void showRowMenu(const QPoint& global_pos, int view_row);

    QtWorkspaceContext* context_ = nullptr;
    std::shared_ptr<QtXrefViewState> state_;
    QtXrefModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaLineEdit* address_edit_ = nullptr;
    widgets::AidaSearchField* filter_edit_ = nullptr;
    QComboBox* mode_combo_ = nullptr;
    QLabel* status_label_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QTimer* filter_debounce_ = nullptr;
    QMetaObject::Connection poller_connection_;
    QMetaObject::Connection context_connection_;
};

}
