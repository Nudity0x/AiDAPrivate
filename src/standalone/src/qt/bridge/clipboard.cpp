#include "qt/bridge/clipboard.hpp"

#include <QClipboard>
#include <QGuiApplication>

namespace aida::qt::clipboard {

void set_text(const QString& text) {
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return;
    clipboard->setText(text, QClipboard::Clipboard);
}

QString text() {
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};
    QString value = clipboard->text(QClipboard::Clipboard);
    value.remove(u'\r');
    return value;
}

bool has_text() {
    QClipboard* clipboard = QGuiApplication::clipboard();
    return clipboard && !clipboard->text(QClipboard::Clipboard).isEmpty();
}

}
