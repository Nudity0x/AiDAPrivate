#pragma once

#include <QSyntaxHighlighter>

class QPlainTextEdit;
class QTextDocument;

namespace aida::qt::net {

class HttpMessageHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit HttpMessageHighlighter(QTextDocument* parent);

protected:
    void highlightBlock(const QString& text) override;

private:
    void formatSpan(const QString& text, int start, int end, const QTextCharFormat& format);
};

HttpMessageHighlighter* attach_http_highlighter(QPlainTextEdit* edit);

}

