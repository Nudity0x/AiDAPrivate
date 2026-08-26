#pragma once

#include <QFrame>
#include <QPixmap>

namespace aida::qt::widgets {

QPixmap make_shadow_9patch(qreal radius, qreal dpr);

class AidaCard : public QFrame
{
    Q_OBJECT
public:
    explicit AidaCard(QWidget* parent = nullptr);

    void setCardRadius(qreal radius);
    qreal cardRadius() const { return card_radius_; }

    void setShadowVisible(bool visible);
    bool isShadowVisible() const { return shadow_visible_; }

    static void clearShadowCache();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPixmap shadowPixmap() const;
    void paintShadow(QPainter& p, const QRectF& face);

    qreal card_radius_;
    bool shadow_visible_ = true;
};

}
