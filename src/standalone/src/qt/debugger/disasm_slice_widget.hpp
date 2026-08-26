#pragma once

#include <QAbstractScrollArea>

#include <QElapsedTimer>
#include <QPoint>
#include <QVariantAnimation>

#include <cstdint>
#include <vector>

#include "core/debugger/debugger_interaction_context.hpp"
#include "core/disasm/zydis_disasm.hpp"

class QContextMenuEvent;

namespace aida::qt::debugger {

// Live disasm slice at RIP (ports render_cpu_disasm_slice). Zydis decode of the
// engine's cached disasm window runs GUI-side on a local byte copy (same as the
// ImGui path); the window fetch is worker-side via
// debugger_engine::request_disasm_refresh driven by the owning pane's 220ms
// tick. paintEvent never locks or blocks: it paints only the decoded row cache.
class DisasmSliceWidget : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit DisasmSliceWidget(QWidget* parent = nullptr);

    // 220ms pane tick: request + decode. Returns true when the row cache is
    // fresh this tick.
    bool tick();

    void setRip(std::uint64_t rip);
    std::uint64_t rip() const noexcept { return rip_; }

    void clearRows();

    debugger_interaction::context_t contextForRow(int row) const;

Q_SIGNALS:
    void rowSelected(int row);
    void branchFollowRequested(std::uint64_t target);
    void contextRowRequested(int row, QPoint globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    struct decoded_row_t {
        std::uint64_t addr = 0;
        int len = 1;
        AsmInstr ins{};
    };

    void rebuildRows();
    int rowAtY(int y) const;
    int rowHeight() const;
    void updateScrollRange();
    void selectRow(int row, bool openMenu, const QPoint& globalPos);
    void scrollRowVisible(int row);
    void updatePulse();
    QPoint selectedRowGlobalPos() const;

    std::uint64_t rip_ = 0;
    std::uint64_t window_base_ = 0;
    std::uint64_t anchor_rip_ = 0;
    std::vector<decoded_row_t> rows_;
    int selected_row_ = -1;
    int rip_row_ = -1;
    QVariantAnimation* pulse_anim_ = nullptr;
    qreal pulse_ = 1.0;

    static constexpr int k_max_rows = 256;
};

}
