#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>

#include "qt/analysis/qt_binary_map_types.hpp"

class QComboBox;
class QLabel;
class QPushButton;
class QSplitter;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaSearchField;
class AidaStateView;
}

namespace aida::qt::analysis {

class QtAddressSpaceCanvas;
class QtBinaryMapListModel;
class QtFunctionHeatmapWidget;
class QtSectionStripWidget;
class QtWorkspaceContext;

// Binary Map host view (07 sec. 6), view.analysis.binary_map. QSplitter between
// the canvas column (section strip / VA canvas / heatmap) and the grouped list.
class QtBinaryMapView : public QWidget {
    Q_OBJECT
public:
    explicit QtBinaryMapView(QWidget* parent = nullptr);

    QtBinaryMapListModel* listModel() const noexcept { return list_model_; }

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebindContext(QtWorkspaceContext* context);
    void pollRefreshFlags();
    void refreshPresentation();
    void rebuildList();
    void showRowMenu(const QPoint& global_pos, int view_row);
    void exportSnapshot();
    void sendToChat();

    QtWorkspaceContext* context_ = nullptr;
    std::shared_ptr<QtBinaryMapViewState> state_;
    QSplitter* splitter_ = nullptr;
    QWidget* left_host_ = nullptr;
    QtSectionStripWidget* section_strip_ = nullptr;
    QtAddressSpaceCanvas* canvas_ = nullptr;
    QtFunctionHeatmapWidget* heatmap_ = nullptr;
    QLabel* legend_label_ = nullptr;
    QLabel* live_stats_ = nullptr;
    QTableView* list_ = nullptr;
    QtBinaryMapListModel* list_model_ = nullptr;
    widgets::AidaSearchField* filter_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* subtitle_label_ = nullptr;
    QComboBox* mode_combo_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QPushButton* export_button_ = nullptr;
    QPushButton* chat_button_ = nullptr;
    QPushButton* copy_button_ = nullptr;
    QTimer* timer_ = nullptr;
    QMetaObject::Connection context_connection_;
    std::shared_ptr<const aida::binary_map::map_t> last_map_;
    std::shared_ptr<const qt_binary_map_live_snapshot_t> last_live_;
};

}
