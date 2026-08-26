#pragma once

#include <QPlainTextEdit>

#include <QtGlobal>

namespace aida::qt::net {

// QPlainTextEdit subclass enforcing the legacy fixed-buffer byte caps.
// QPlainTextEdit has no maxLength property (that is QLineEdit-only,
// qlineedit.h:33,68-69), so the byte cap is enforced on textChanged
// (qplaintextedit.h:39 USER-prop NOTIFY): when the UTF-8 payload exceeds the
// cap the document is truncated at a UTF-8 code-point boundary and the cursor
// is restored to the end via QTextCursor::movePosition(End)
// (qtextcursor.h:61-90,92). setPlainText resets the undo history
// (qtextdocument.cpp:1263-1282); that cap-hit edge behavior is accepted per
// plan 11 section 3.4.
class BoundedPlainTextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit BoundedPlainTextEdit(QWidget* parent = nullptr);
    explicit BoundedPlainTextEdit(qsizetype maxBytes, QWidget* parent = nullptr);

    void setMaxBytes(qsizetype maxBytes);
    qsizetype maxBytes() const noexcept { return max_bytes_; }

Q_SIGNALS:
    void capHit();

private:
    void enforceCap();

    qsizetype max_bytes_ = 0;
    bool enforcing_ = false;
};

}
