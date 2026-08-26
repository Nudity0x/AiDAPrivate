#include "qt/ai/qt_ai_chat_composer.hpp"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QSignalBlocker>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::ai {

AidaChatComposer::AidaChatComposer(QWidget* parent) : QPlainTextEdit(parent) {
    setObjectName(QStringLiteral("aida.ai.chat.composer"));
    setPlaceholderText(QStringLiteral("Ask AiDA about the active workspace..."));
    setFont(theme::fonts::body());
    setTabChangesFocus(false);
    connect(this, &QPlainTextEdit::textChanged, this, &AidaChatComposer::enforceCap);
}

void AidaChatComposer::appendInjected(const QString& text) {
    if (text.isEmpty())
        return;
    const int cap = k_max_chars - 1;
    QString current = toPlainText();
    const int cur = current.size();
    if (cur + text.size() >= cap)
        return;
    if (cur > 0 && cur + 2 < cap)
        current += QStringLiteral("\n\n");
    current += text;
    {
        const QSignalBlocker blocker(this);
        setPlainText(current);
    }
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);
}

void AidaChatComposer::clearComposer() {
    const QSignalBlocker blocker(this);
    clear();
}

QSize AidaChatComposer::sizeHint() const {
    const auto& t = theme::tokens();
    const QFontMetricsF fm(font());
    const int h = qRound(fm.lineSpacing() * 2.2) + t.panel.padding;
    return QSize(theme::scale_logical(t.shell.min_panel_w, 2.5), h);
}

QSize AidaChatComposer::minimumSizeHint() const {
    const auto& t = theme::tokens();
    const QFontMetricsF fm(font());
    return QSize(theme::scale_logical(t.shell.min_panel_w, 1.25),
                 qRound(fm.lineSpacing()) + t.spacing.lg);
}

void AidaChatComposer::keyPressEvent(QKeyEvent* event) {
    const bool return_key = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (return_key && (event->modifiers() & Qt::ControlModifier)) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }
    if (return_key) {
        event->accept();
        Q_EMIT submitRequested();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

void AidaChatComposer::enforceCap() {
    if (document()->characterCount() <= k_max_chars)
        return;
    const QSignalBlocker blocker(this);
    QTextCursor cursor = textCursor();
    cursor.setPosition(k_max_chars);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
}

}
