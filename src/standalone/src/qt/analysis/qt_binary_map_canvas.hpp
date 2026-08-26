#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/runtime/standalone_driver.hpp"
#include "qt/analysis/qt_binary_map_types.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::analysis {

// VA address-space canvas with zoom/pan (07 sec. 6.4). The transform math and the
// /zoom pan-speed quirk are ported verbatim.
class QtAddressSpaceCanvas : public QWidget {
    Q_OBJECT
public:
    explicit QtAddressSpaceCanvas(QWidget* parent = nullptr);

    void setContent(
        std::shared_ptr<const qt_binary_map_live_snapshot_t> snapshot,
        quint64 selected_base);
    QSize sizeHint() const override {
        const auto& tokens = theme::tokens();
        return {4 * static_cast<int>(tokens.shell.min_panel_w) + tokens.spacing.section,
            5 * tokens.control.height_lg + tokens.spacing.xl};
    }
    QSize minimumSizeHint() const override {
        const auto& tokens = theme::tokens();
        return {static_cast<int>(tokens.shell.min_panel_w) + 2 * tokens.spacing.section,
            3 * tokens.control.height_lg};
    }

Q_SIGNALS:
    void regionSelected(quint64 base);
    void regionDoubleClicked(quint64 base);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct transform_t {
        std::uint64_t va_min = 0;
        std::uint64_t va_max = 1;
        double full_span = 1.0;
        double visible_span = 1.0;
        double offset_norm = 0.0;
        std::uint64_t view_low = 0;
        std::uint64_t view_high = 1;
        QRectF canvas;
    };

    transform_t computeTransform() const;
    void applyZoomSteps(double delta_steps, double anchor_frac);
    void panByFrac(double frac);
    int regionAt(const QPointF& pos, const transform_t& transform) const;
    void showRegionTooltip(const QPoint& global_pos, int index);

    std::shared_ptr<const qt_binary_map_live_snapshot_t> snapshot_;
    quint64 selected_base_ = 0;
    float zoom_ = 1.f;
    double offset_norm_ = 0.0;
    bool dragging_ = false;
    qreal drag_anchor_ = 0.0;
    double drag_offset_start_ = 0.0;
    int hover_ = -1;
};

}
