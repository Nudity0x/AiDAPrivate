#pragma once

#include <QLabel>

namespace aida::qt::widgets {

class AidaKbdChip : public QLabel
{
    Q_OBJECT
public:
    explicit AidaKbdChip(QWidget* parent = nullptr);
    explicit AidaKbdChip(const QString& text, QWidget* parent = nullptr);
};

}
