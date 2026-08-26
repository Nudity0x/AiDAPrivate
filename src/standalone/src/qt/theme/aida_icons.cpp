#include "aida_icons.hpp"

#include <QColor>
#include <QFile>
#include <QHash>
#include <QList>
#include <QPainter>
#include <QSvgRenderer>

#include "helpers/diag_log.hpp"

namespace aida::qt::theme::icons {

namespace {

constexpr int kCacheCapacity = 64;

QHash<QString, QIcon> s_cache;
QList<QString> s_lru;

QString resourcePath(const QString& name)
{
    return QStringLiteral(":/icons/") + name + QStringLiteral(".svg");
}

QIcon renderTinted(const QString& name, QRgb color, int size, qreal dpr)
{
    QFile file(resourcePath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        diag::log_tagged_fmt("qt_icons", "icon_open_failed name=%s", name.toUtf8().constData());
        return QIcon();
    }
    QByteArray bytes = file.readAll();
    const QString hex = QColor::fromRgba(color).name(QColor::HexRgb);
    bytes.replace("#FF00FF", hex.toLatin1());
    bytes.replace("#ff00ff", hex.toLatin1());
    bytes.replace("#D8DEE8", hex.toLatin1());
    bytes.replace("#d8dee8", hex.toLatin1());

    QSvgRenderer renderer(bytes);
    if (!renderer.isValid()) {
        diag::log_tagged_fmt("qt_icons", "icon_invalid_svg name=%s", name.toUtf8().constData());
        return QIcon();
    }

    const qreal scale = dpr > 0.0 ? dpr : 1.0;
    QSize actualSize = renderer.defaultSize();
    if (!actualSize.isNull())
        actualSize.scale(QSize(size, size) * scale, Qt::KeepAspectRatio);
    if (actualSize.isEmpty())
        actualSize = QSize(qRound(size * scale), qRound(size * scale));

    QPixmap pixmap(actualSize);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        renderer.render(&painter);
    }
    pixmap.setDevicePixelRatio(scale);

    QIcon result;
    result.addPixmap(pixmap);
    return result;
}

void cacheInsert(const QString& key, const QIcon& icon)
{
    if (s_cache.size() >= kCacheCapacity) {
        if (!s_lru.isEmpty()) {
            const QString oldest = s_lru.takeFirst();
            s_cache.remove(oldest);
        }
    }
    s_cache.insert(key, icon);
    s_lru.append(key);
}

}

QStringList available()
{
    return {
        QStringLiteral("files-empty"),
        QStringLiteral("copy"),
        QStringLiteral("history"),
        QStringLiteral("spinner"),
        QStringLiteral("search"),
        QStringLiteral("settings"),
        QStringLiteral("power-cord"),
        QStringLiteral("function"),
        QStringLiteral("clear-x"),
        QStringLiteral("check"),
        QStringLiteral("padlock"),
        QStringLiteral("caret-down"),
        QStringLiteral("caret-right"),
        QStringLiteral("caret-up"),
        QStringLiteral("caret-left"),
        QStringLiteral("chevron-down"),
        QStringLiteral("chevron-right"),
        QStringLiteral("severity-info"),
        QStringLiteral("severity-warning"),
        QStringLiteral("severity-error"),
        QStringLiteral("checkbox-checked"),
        QStringLiteral("checkbox-unchecked"),
        QStringLiteral("checkbox-partial"),
        QStringLiteral("radio-checked"),
        QStringLiteral("radio-unchecked"),
        QStringLiteral("logo"),
    };
}

QIcon icon(const QString& name)
{
    return QIcon(resourcePath(name));
}

QIcon tinted(const QString& name, QRgb color, int size, qreal dpr)
{
    const QString key = QStringLiteral("%1#%2@%3x%4")
        .arg(name)
        .arg(color, 8, 16, QLatin1Char('0'))
        .arg(size)
        .arg(dpr);

    const auto it = s_cache.constFind(key);
    if (it != s_cache.constEnd()) {
        s_lru.removeAll(key);
        s_lru.append(key);
        return it.value();
    }

    QIcon result = renderTinted(name, color, size, dpr);
    if (!result.isNull())
        cacheInsert(key, result);
    return result;
}

void clearCache()
{
    s_cache.clear();
    s_lru.clear();
}

}
