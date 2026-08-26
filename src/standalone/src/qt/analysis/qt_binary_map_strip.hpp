#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>

#include "core/analysis/binary_map.hpp"
#include "qt/theme/aida_tokens.hpp"

class QVariantAnimation;

namespace aida::qt::analysis {

// Section strip + entropy sub-row (07 sec. 6.2). WA_OpaquePaintEvent; paintEvent
// reads only the published snapshot pointer.
class QtSectionStripWidget : public QWidget {
    Q_OBJECT
public:
    explicit QtSectionStripWidget(QWidget* parent = nullptr);

    void setSections(
        const std::shared_ptr<const aida::binary_map::map_t>& map);
    QSize sizeHint() const override {
        const auto& t = theme::tokens();
        return {3 * static_cast<int>(t.shell.min_panel_w) + t.spacing.section,
            stripHeight() + t.spacing.xs + t.spacing.md};
    }
    QSize minimumSizeHint() const override {
        const auto& t = theme::tokens();
        return {static_cast<int>(t.shell.min_panel_w) + t.spacing.xxl,
            stripHeight()};
    }

Q_SIGNALS:
    void jumpToAddress(quint64 va);
    void openHex(quint64 va, std::size_t size);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    int hitSection(const QPointF& pos) const;
    QRectF sectionRect(std::size_t index) const;
    void showSectionTooltip(const QPoint& global_pos, int index);
    static int stripHeight() {
        const auto& t = theme::tokens();
        return t.toolbar.height + t.spacing.xxs;
    }

    std::shared_ptr<const aida::binary_map::map_t> map_;
    QVariantAnimation* reveal_ = nullptr;
    qreal reveal_value_ = 1.0;
    int hover_ = -1;
    qreal strip_height_ = static_cast<qreal>(stripHeight());
};

}
