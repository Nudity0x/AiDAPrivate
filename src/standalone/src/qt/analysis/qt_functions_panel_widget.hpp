#pragma once

#include <QPointer>
#include <QWidget>

#include <cstdint>
#include <memory>

#include "qt/analysis/qt_functions_model.hpp"

class QLabel;
class QStyledItemDelegate;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaProgressBar;
class AidaSearchField;
class AidaStateView;
}

namespace aida::qt::analysis {

class QtWorkspaceContext;

// Functions panel (07 sec. 4): adaptive columns, 4-counter revision polling,
// dual-stage worker pipeline (projection -> filter/sort), S1-S5 model rules.
class QtFunctionsPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit QtFunctionsPanelWidget(QWidget* parent = nullptr);

    void copySelectedName();

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebindContext(QtWorkspaceContext* context);
    void refreshFromWorkspace();
    void submitFilterSort();
    void adoptPresentation(std::shared_ptr<const qt_functions_presentation_t> presentation,
                           std::uint64_t serial);
    void applyAdaptiveColumns();
    void refreshPresentation();
    void showFunctionMenu(const QPoint& global_pos, int view_row);

    QtWorkspaceContext* context_ = nullptr;
    std::shared_ptr<QtFunctionsPanelState> state_;
    QtFunctionsModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStyledItemDelegate* name_delegate_ = nullptr;
    widgets::AidaSearchField* filter_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    widgets::AidaProgressBar* loading_ = nullptr;
    QLabel* count_label_ = nullptr;
    QTimer* filter_debounce_ = nullptr;
    QMetaObject::Connection poller_connection_;
    QMetaObject::Connection context_connection_;
    bool compact_mode_ = false;
};

}
