#include "aida_card.hpp"
#include <algorithm>

#include <QHash>
#include <QImage>
#include <QPainter>

#include "../theme/aida_tokens.hpp"
#include "aida_paint_utils.hpp"

namespace aida::qt::widgets {

namespace {
    constexpr qreal kShadowStrength = 0.30;
    constexpr int kShadowPasses = 5;
    constexpr qreal kPassFalloff[kShadowPasses] = { 0.46, 0.26, 0.15, 0.08, 0.04 };

    qreal shadow_margin_for()
    {
        return qreal(aida::qt::theme::tokens().panel.padding);
    }

    QHash<quint64, QPixmap>& shadow_cache()
    {
        static QHash<quint64, QPixmap> cache;
        return cache;
    }

    quint64 shadow_key(qreal radius, qreal dpr)
    {
        const quint64 r = quint64(quint32(qHashBits(&radius, sizeof(radius))));
        const quint64 d = quint64(quint32(qHashBits(&dpr, sizeof(dpr))));
        return (r << 32) | d;
    }
}

QPixmap make_shadow_9patch(qreal radius, qreal dpr)
{
    const auto& t = aida::qt::theme::tokens();
    const qreal margin = shadow_margin_for();
    const qreal offset_y = qreal(t.spacing.xs);
    const qreal inner = radius * 2.0;
    const qreal logical_size = margin * 2.0 + inner;
    const int device_size = qCeil(logical_size * dpr);

    QImage image(QSize(device_size, device_size), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int pass = kShadowPasses - 1; pass >= 0; --pass) {
            const qreal expand = qreal(pass) * 2.2;
            QColor shade(t.title_bar);
            shade.setAlphaF(clamp01(kShadowStrength * kPassFalloff[pass]));
            p.setPen(Qt::NoPen);
            p.setBrush(shade);
            p.drawRoundedRect(QRectF(margin - expand, margin - expand + offset_y,
                inner + expand * 2.0, inner + expand * 2.0),
                radius + expand, radius + expand);
        }
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setBrush(Qt::black);
        p.drawRoundedRect(QRectF(margin, margin + offset_y, inner, inner), radius, radius);
    }

    QPixmap pm = QPixmap::fromImage(image);
    pm.setDevicePixelRatio(dpr);
    return pm;
}

AidaCard::AidaCard(QWidget* parent)
    : QFrame(parent)
    , card_radius_(qreal(aida::qt::theme::tokens().radius.modal))
{
    setObjectName(QStringLiteral("aida.card"));
    setFrameShape(QFrame::StyledPanel);
    setProperty("aidaRole", QStringLiteral("card"));
}

void AidaCard::setCardRadius(qreal radius)
{
    if (card_radius_ == radius)
        return;
    card_radius_ = radius;
    update();
}

void AidaCard::setShadowVisible(bool visible)
{
    if (shadow_visible_ == visible)
        return;
    shadow_visible_ = visible;
    update();
}

void AidaCard::clearShadowCache()
{
    shadow_cache().clear();
}

QSize AidaCard::sizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    const QSize base = QFrame::sizeHint();
    const int m = qCeil(shadow_margin_for());
    const int w = base.width() >= 0 ? base.width() : t.panel.overlay_margin * 5;
    const int h = base.height() >= 0 ? base.height() : t.panel.overlay_margin * 3;
    return QSize(w + m * 2, h + m * 2);
}

QPixmap AidaCard::shadowPixmap() const
{
    const qreal dpr = (std::max)(devicePixelRatioF(), qreal(1.0));
    const quint64 key = shadow_key(card_radius_, dpr);
    auto& cache = shadow_cache();
    const auto found = cache.constFind(key);
    if (found != cache.constEnd())
        return found.value();
    const QPixmap pm = make_shadow_9patch(card_radius_, dpr);
    cache.insert(key, pm);
    return pm;
}

void AidaCard::paintShadow(QPainter& p, const QRectF& face)
{
    const QPixmap pm = shadowPixmap();
    const qreal dpr = (std::max)(pm.devicePixelRatio(), qreal(1.0));
    const qreal logical = pm.width() / dpr;
    const qreal margin = shadow_margin_for();
    const qreal inner = logical - margin * 2.0;

    const qreal src_w = pm.width();
    const qreal m_dev = margin * dpr;
    const qreal inner_dev = src_w - m_dev * 2.0;

    const QRectF dst(face.left() - margin, face.top() - margin,
        face.width() + margin * 2.0, face.height() + margin * 2.0);

    const qreal sx[3] = { 0.0, m_dev, src_w - m_dev };
    const qreal sy[3] = { 0.0, m_dev, src_w - m_dev };
    const qreal sw[3] = { m_dev, inner_dev, m_dev };
    const qreal sh[3] = { m_dev, inner_dev, m_dev };
    const qreal dx[3] = { dst.left(), dst.left() + margin, dst.right() - margin };
    const qreal dy[3] = { dst.top(), dst.top() + margin, dst.bottom() - margin };
    const qreal dw[3] = { margin, dst.width() - margin * 2.0, margin };
    const qreal dh[3] = { margin, dst.height() - margin * 2.0, margin };

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (row == 1 && col == 1)
                continue;
            if (dw[col] <= 0.0 || dh[row] <= 0.0)
                continue;
            p.drawPixmap(QRectF(dx[col], dy[row], dw[col], dh[row]), pm,
                QRectF(sx[col], sy[row], sw[col], sh[row]));
        }
    }
}

void AidaCard::paintEvent(QPaintEvent* event)
{
    if (shadow_visible_) {
        QPainter p(this);
        const qreal margin = shadow_margin_for();
        const QRectF face = QRectF(rect()).adjusted(margin, margin, -margin, -margin);
        if (face.width() > 1.0 && face.height() > 1.0)
            paintShadow(p, face);
    }
    QFrame::paintEvent(event);
}

}
