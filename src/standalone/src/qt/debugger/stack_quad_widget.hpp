#pragma once

#include <QAbstractScrollArea>

#include <QPoint>

#include <cstdint>
#include <vector>

#include "core/debugger/debugger_interaction_context.hpp"

class QContextMenuEvent;

namespace aida::qt::debugger {

// Stack quad view over the engine's cached stack bytes (220ms cadence via the
// owning pane's tick -> debugger_engine::request_stack_refresh). Custom paint;
// paintEvent reads only the local byte copy.
class StackQuadWidget : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit StackQuadWidget(QWidget* parent = nullptr);

    bool tick();
    void setRsp(std::uint64_t rsp);
    std::uint64_t rsp() const noexcept { return rsp_; }
    void clearBytes();

    debugger_interaction::context_t contextForRow(int row) const;

Q_SIGNALS:
    void rowSelected(int row);
    void contextRowRequested(int row, QPoint globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void updateScrollRange();
    int rowAtY(int y) const;
    int rowHeight() const;
    qreal cellWidth() const;
    qreal contentWidth() const;
    void scrollRowVisible(int row);
    QPoint selectedRowGlobalPos() const;

    std::uint64_t rsp_ = 0;
    std::uint64_t base_ = 0;
    std::vector<std::uint8_t> bytes_;
    int selected_row_ = -1;
};

}
