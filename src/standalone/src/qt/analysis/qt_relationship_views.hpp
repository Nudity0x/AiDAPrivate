#pragma once

#include <QPointer>
#include <QWidget>

#include <cstdint>
#include <memory>

#include "qt/analysis/qt_proximity_model.hpp"
#include "qt/analysis/qt_segment_registers_model.hpp"

class QComboBox;
class QLabel;
class QProgressBar;
class QTableView;
class QTimer;
class QToolButton;

namespace aida::qt::widgets {
class AidaSearchField;
class AidaStateView;
}

namespace aida::qt::analysis {

class QtWorkspaceContext;

// Segment Registers view (07 sec. 5.1): incremental 2048-instruction chunk scan
// driven by a GUI-thread QTimer(0) while visible.
class QtSegmentRegistersView : public QWidget {
    Q_OBJECT
public:
    explicit QtSegmentRegistersView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebindContext(QtWorkspaceContext* context);
    void resetIfNeeded();
    void pumpChunk();
    void publishRows();
    void refreshPresentation();
    void showRowMenu(const QPoint& global_pos, int view_row);
    QString instructionText(aida::analysis::entity_id_t instruction_id);

    QtWorkspaceContext* context_ = nullptr;
    QtSegmentRegistersState* state_ = nullptr;
    QtSegmentRegistersModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaSearchField* filter_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTimer* pump_ = nullptr;
    QTimer* filter_debounce_ = nullptr;
    QMetaObject::Connection poller_connection_;
    QMetaObject::Connection context_connection_;
    std::size_t published_rows_ = 0;
    QHash<quint64, QString> instruction_text_cache_;
};

// Proximity Browser view (07 sec. 5.2): incremental 1024-relation BFS chunk pump,
// history navigation, node/depth limits.
class QtProximityBrowserView : public QWidget {
    Q_OBJECT
public:
    explicit QtProximityBrowserView(QWidget* parent = nullptr);

    // Public API preserved for external callers (07 sec. 5.2).
    bool drillSelected();
    bool drillCapable() const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebindContext(QtWorkspaceContext* context);
    void resetIfNeeded();
    void pumpChunk();
    void publishNodes();
    void refreshPresentation();
    void showNodeMenu(const QPoint& global_pos, int view_row);
    void navigateRoot(std::uint64_t root, bool record_history);
    std::uint64_t defaultRoot() const;
    void useSelection();

    QtWorkspaceContext* context_ = nullptr;
    QtProximityState* state_ = nullptr;
    QtProximityModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaSearchField* filter_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QToolButton* back_button_ = nullptr;
    QToolButton* forward_button_ = nullptr;
    QToolButton* use_selection_button_ = nullptr;
    QComboBox* depth_combo_ = nullptr;
    QComboBox* limit_combo_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTimer* pump_ = nullptr;
    QTimer* filter_debounce_ = nullptr;
    QMetaObject::Connection poller_connection_;
    QMetaObject::Connection context_connection_;
};

}
