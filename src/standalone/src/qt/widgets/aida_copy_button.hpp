#pragma once

#include "aida_button.hpp"

namespace aida::qt::widgets {

class AidaCopyButton : public AidaButton
{
    Q_OBJECT
public:
    explicit AidaCopyButton(QWidget* parent = nullptr);

    void flashCopied();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void paintCheck(QPainter& p, const QRectF& face, qreal t01);
};

}
