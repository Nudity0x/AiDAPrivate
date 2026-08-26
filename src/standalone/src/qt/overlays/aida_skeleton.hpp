#pragma once

#include <QWidget>

#include "qt/theme/aida_tokens.hpp"

class QPainter;
class QVariantAnimation;

namespace aida::qt::overlays {

class AidaSkeletonBlock : public QWidget {
    Q_OBJECT
public:
    enum class Kind {
        Block,
        TextLine,
        Paragraph,
        Avatar,
        Card,
        TableRows
    };

    explicit AidaSkeletonBlock(QWidget* parent = nullptr);
    AidaSkeletonBlock(Kind kind, QWidget* parent = nullptr);
    ~AidaSkeletonBlock() override;

    void setKind(Kind kind);
    Kind kind() const noexcept { return kind_; }

    void setLineCount(int lines);
    void setTableShape(int columns, int rows);
    void setRadius(qreal radius);
    void setSweepEnabled(bool enabled);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void paintBlock(QPainter& painter, const QRectF& rect, qreal radius);
    void paintTextLine(QPainter& painter, const QPointF& origin, qreal width, qreal height);
    void startSweep();
    void stopSweep();

    Kind kind_ = Kind::Block;
    int line_count_ = 4;
    int table_columns_ = 4;
    int table_rows_ = 8;
    qreal radius_ = theme::tokens().radius.md;
    qreal phase_ = 0.0;
    bool sweep_enabled_ = true;
    QVariantAnimation* sweep_ = nullptr;
};

}
