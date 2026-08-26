#pragma once

#include <QIcon>
#include <QRgb>
#include <QString>
#include <QStringList>

namespace aida::qt::theme::icons {

QStringList available();

QIcon icon(const QString& name);
QIcon tinted(const QString& name, QRgb color, int size, qreal dpr);

void clearCache();

}
