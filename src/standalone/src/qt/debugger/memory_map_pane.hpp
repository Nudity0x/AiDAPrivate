#pragma once

#include "qt/debugger/debugger_pane_base.hpp"

#include <QWidget>

#include <QPoint>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/debugger/debugger_engine.hpp"
#include "core/debugger/memory_map_view.hpp"

class QLabel;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaLineEdit;
}

namespace aida::qt::debugger {

class MemoryRegionsModel;

// Hero VA map: proportional segment strip over the regions snapshot (colors
// port segment_color/state_color verbatim; protect-token hue). Click selects a
// region (memory_region context + memory_interaction publish); double-click
// jumps the hex view. Keyboard: focusable, Left/Right/Home/End move the
// selected segment, Enter jumps the hex view. paintEvent reads only the
// adopted snapshot.
class VaMapCanvas : public QWidget {
    Q_OBJECT
public:
    explicit VaMapCanvas(QWidget* parent = nullptr);

    void setRegions(
        std::shared_ptr<const std::vector<debugger_engine::memory_region_t>>
            regions,
        const std::vector<int>& filteredIndices);
    int selectedRegion() const noexcept { return selected_region_; }
    void clearSelection();

    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void regionSelected(int regionIndex, QPoint globalPos);
    void regionJumpHex(std::uint64_t base, std::uint64_t size);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    struct segment_t {
        std::uint64_t base = 0;
        std::uint64_t size = 0;
        std::uint32_t protect = 0;
        std::uint32_t state = 0;
        std::uint32_t type = 0;
        int region_index = -1;
        qreal start_x = 0;
        qreal width = 0;
    };

    void rebuildSegments();
    int segmentAtX(qreal x) const;
    QColor segmentColor(std::uint32_t protect, std::uint32_t state) const;

    std::shared_ptr<const std::vector<debugger_engine::memory_region_t>>
        regions_;
    std::vector<int> filtered_;
    std::vector<segment_t> segments_;
    int hovered_segment_ = -1;
    int selected_segment_ = -1;
    int selected_region_ = -1;
};

// Memory map pane: VaMapCanvas hero + stat pods (REGIONS/COMMITTED/RWX/
// ATTACHED) + width-adaptive region table (columns hidden least-essential
// first against the mono-grid content budget) + filter + Refresh (the backend
// memory_map_view::refresh single-flight worker) + Change Protection + Dump
// Region.
class MemoryMapPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit MemoryMapPane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onHidden() override;
    void onSessionTick() override;
    bool hasContentRows() const override;
    bool isContentLoading() const override;
    bool contentError(QString* detail) const override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void pollModel();
    void refreshNow();
    void applyColumnTiers();

    MemoryRegionsModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    VaMapCanvas* canvas_ = nullptr;
    widgets::AidaLineEdit* filter_edit_ = nullptr;
    widgets::AidaButton* refresh_button_ = nullptr;
    QLabel* regions_pod_ = nullptr;
    QLabel* committed_pod_ = nullptr;
    QLabel* rwx_pod_ = nullptr;
    QLabel* attached_pod_ = nullptr;
    QString last_error_;
    QTimer* poll_timer_ = nullptr;
};

}
