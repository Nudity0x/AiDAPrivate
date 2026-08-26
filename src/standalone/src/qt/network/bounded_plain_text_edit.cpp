#include "qt/network/bounded_plain_text_edit.hpp"

#include <QTextCursor>

namespace aida::qt::net {

BoundedPlainTextEdit::BoundedPlainTextEdit(QWidget* parent)
    : QPlainTextEdit(parent) {
    connect(this, &QPlainTextEdit::textChanged, this,
            &BoundedPlainTextEdit::enforceCap);
}

BoundedPlainTextEdit::BoundedPlainTextEdit(qsizetype maxBytes, QWidget* parent)
    : BoundedPlainTextEdit(parent) {
    setMaxBytes(maxBytes);
}

void BoundedPlainTextEdit::setMaxBytes(qsizetype maxBytes) {
    max_bytes_ = maxBytes;
    enforceCap();
}

void BoundedPlainTextEdit::enforceCap() {
    if (enforcing_ || max_bytes_ <= 0)
        return;
    const QString text = toPlainText();
    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() <= max_bytes_)
        return;
    enforcing_ = true;
    qsizetype cut = max_bytes_;
    while (cut > 0 && (static_cast<unsigned char>(utf8.at(cut)) & 0xC0) == 0x80)
        --cut;
    setPlainText(QString::fromUtf8(utf8.constData(), cut));
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);
    enforcing_ = false;
    Q_EMIT capHit();
}

}
