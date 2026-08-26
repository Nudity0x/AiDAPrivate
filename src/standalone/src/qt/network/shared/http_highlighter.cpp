#include "qt/network/shared/http_highlighter.hpp"

#include <QPlainTextEdit>
#include <QTextCharFormat>
#include <QTextDocument>

#include "qt/network/shared/network_format.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::net {

namespace {

QTextCharFormat make_format(const QColor& color, bool bold = false) {
    QTextCharFormat format;
    format.setForeground(color);
    if (bold)
        format.setFontWeight(QFont::DemiBold);
    return format;
}

}

HttpMessageHighlighter::HttpMessageHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent) {}

void HttpMessageHighlighter::formatSpan(const QString& text, int start, int end,
                                        const QTextCharFormat& format) {
    if (end > start)
        setFormat(start, end - start, format);
}

void HttpMessageHighlighter::highlightBlock(const QString& text) {
    const auto& t = theme::tokens();
    const int state = previousBlockState();
    if (state == 1) {
        setCurrentBlockState(1);
        return;
    }
    if (currentBlock().blockNumber() == 0) {
        const int firstSpace = text.indexOf(QLatin1Char(' '));
        if (firstSpace <= 0) {
            setFormat(0, text.size(), make_format(t.text_secondary));
            setCurrentBlockState(0);
            return;
        }
        const QString head = text.left(firstSpace);
        if (head.startsWith(QLatin1String("HTTP/"))) {
            const int secondSpace = text.indexOf(QLatin1Char(' '), firstSpace + 1);
            const int codeEnd = secondSpace < 0 ? text.size() : secondSpace;
            setFormat(0, firstSpace, make_format(t.text_dim));
            bool ok = false;
            const int code = text.mid(firstSpace + 1, codeEnd - firstSpace - 1).toInt(&ok);
            setFormat(firstSpace + 1, codeEnd - firstSpace - 1,
                make_format(ok ? status_code_color(code) : t.text_primary, true));
            formatSpan(text, codeEnd, text.size(), make_format(t.text_secondary));
        } else {
            const int lastSpace = text.lastIndexOf(QLatin1Char(' '));
            setFormat(0, firstSpace, make_format(http_method_color(
                head.toLatin1().constData()), true));
            const int targetEnd = lastSpace > firstSpace ? lastSpace : text.size();
            formatSpan(text, firstSpace + 1, targetEnd, make_format(t.syn_function));
            if (lastSpace > firstSpace)
                formatSpan(text, lastSpace, text.size(), make_format(t.text_dim));
        }
        setCurrentBlockState(0);
        return;
    }
    if (text.isEmpty()) {
        setCurrentBlockState(1);
        return;
    }
    const int colon = text.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        setFormat(0, colon, make_format(t.syn_type));
        formatSpan(text, colon + 1, text.size(), make_format(t.text_primary));
    } else {
        setFormat(0, text.size(), make_format(t.text_primary));
    }
    setCurrentBlockState(0);
}

HttpMessageHighlighter* attach_http_highlighter(QPlainTextEdit* edit) {
    if (!edit || !edit->document())
        return nullptr;
    if (edit->document()->findChild<HttpMessageHighlighter*>())
        return edit->document()->findChild<HttpMessageHighlighter*>();
    return new HttpMessageHighlighter(edit->document());
}

}

