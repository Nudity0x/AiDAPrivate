#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "core/analysis/binary_map.hpp"
#include "qt/theme/aida_tokens.hpp"

class QTimer;

namespace aida::qt::analysis {

// Function heatmap grid (07 sec. 6.3). Pin rings, hover pulse rings, click select,
// double-click jump, right-click menu signals.
class QtFunctionHeatmapWidget : public QWidget {
    Q_OBJECT
public:
    explicit QtFunctionHeatmapWidget(QWidget* parent = nullptr);

    void setFunctions(const std::shared_ptr<const aida::binary_map::map_t>& map,
                      quint64 selected_va);
    QSize sizeHint() const override {
        const auto& t = theme::tokens();
        return {8 * t.control.height_lg, 3 * t.control.height_lg};
    }
    QSize minimumSizeHint() const override {
        const auto& t = theme::tokens();
        return {3 * t.control.height_lg, t.control.height_lg + t.spacing.xl};
    }

Q_SIGNALS:
    void functionClicked(quint64 va);
    void functionDoubleClicked(quint64 va);
    void functionMenuRequested(quint64 va, const QPoint& global_pos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRectF cellRect(int index) const;
    int hitCell(const QPointF& pos) const;
    int columnCount() const;
    void seedHover(int hit);
    void showFunctionTooltip(const QPoint& global_pos, int index);
    void tickPulses();

    std::shared_ptr<const aida::binary_map::map_t> map_;
    quint64 selected_va_ = 0;
    int max_xrefs_ = 1;
    int hover_ = -1;
    // Hover pulse rings (binary_map_view.hpp:73-75/:2365-2373 port). One
    // shared 16 ms timer runs only while any pulse is non-settled (S8).
    QTimer* pulse_timer_ = nullptr;
    std::unordered_map<quint64, qreal> pulses_;
};

}
