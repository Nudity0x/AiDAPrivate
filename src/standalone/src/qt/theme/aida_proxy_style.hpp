#pragma once

#include <QProxyStyle>

namespace aida::qt::theme {

class AidaProxyStyle : public QProxyStyle {
    Q_OBJECT
public:
    explicit AidaProxyStyle(QStyle* base = nullptr);
    explicit AidaProxyStyle(const QString& key);

    int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
        const QWidget* widget = nullptr) const override;
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
        const QWidget* widget = nullptr, QStyleHintReturn* returnData = nullptr) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
        const QWidget* widget = nullptr) const override;
};

}
