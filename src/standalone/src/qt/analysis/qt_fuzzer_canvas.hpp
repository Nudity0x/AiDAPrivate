#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/analysis/fuzzer_engine.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::analysis {

// Fuzzer canvas (07 sec. 7.5): one custom paint surface carrying the exec graph,
// the coverage heatmap and the strategy bars. WA_OpaquePaintEvent; paintEvent
// never locks (data arrives as shared_ptr<const> snapshots).
class QtFuzzerCanvas : public QWidget {
    Q_OBJECT
public:
    explicit QtFuzzerCanvas(QWidget* parent = nullptr);

    void setSnapshot(std::shared_ptr<const fuzzer_engine::render_snapshot_t> snapshot,
                     bool running);
    void setScanPhase(qreal phase);

    QSize sizeHint() const override {
        const auto& tokens = theme::tokens();
        return {13 * tokens.control.height_lg,
            6 * tokens.control.height_lg + tokens.spacing.md + tokens.spacing.xxs};
    }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct geometry_t {
        QRectF graph;
        QRectF heatmap;
        QRectF bars;
    };

    geometry_t layoutZones(const QRectF& bounds) const;
    void paintGraph(QPainter& painter, const QRectF& zone);
    void paintHeatmap(QPainter& painter, const QRectF& zone);
    void paintStrategyBars(QPainter& painter, const QRectF& zone);

    std::shared_ptr<const fuzzer_engine::render_snapshot_t> snapshot_;
    bool running_ = false;
    qreal scan_phase_ = 0.0;
    int hover_graph_index_ = -1;
    int strategy_unique_[6] = {};
    qreal strategy_efficacy_[6] = {};
};

}
