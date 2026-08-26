#pragma once

#include <QByteArray>
#include <QPlainTextEdit>
#include <QSize>
#include <QString>
#include <QSyntaxHighlighter>
#include <QWidget>

#include <cstdint>
#include <string>
#include <vector>

#include "qt/net/http_text_utils.hpp"

class QButtonGroup;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QMimeData;
class QPushButton;
class QSplitter;
class QStackedLayout;
class QToolButton;

namespace aida::qt::net {

// Hard byte-capacity plain-text edit. QPlainTextEdit documents are unbounded,
// so the cap is enforced explicitly in three layers: keyPressEvent rejects
// printable input past the cap, insertFromMimeData truncates pasted text to
// the remaining capacity, and a contentsChanged safety net truncates at the
// byte cap on a codepoint boundary for the paths the first two miss (IME,
// multi-byte UTF-8 overshoot). Net invariant: the document never holds more
// than maxBytes UTF-8 bytes, so a downstream char[Cap] destination can never
// overflow. (insertFromMimeData is virtual, qplaintextedit.h:224;
// characterCount is a QTextDocument property, qtextdocument.h:243.)
class QtByteCappedPlainTextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit QtByteCappedPlainTextEdit(QWidget* parent = nullptr);

    void setMaxBytes(qsizetype maxBytes);
    qsizetype maxBytes() const noexcept { return max_bytes_; }

Q_SIGNALS:
    void overCapacity();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void insertFromMimeData(const QMimeData* source) override;

private:
    void enforceCap();

    qsizetype max_bytes_ = (1 << 20);
    bool enforcing_ = false;
};

// HTTP grammar highlighter. Block states: 0 = request-line, 1 = header lines,
// 2 = body (reached after the blank separator line). Grammar::FullRequest
// starts at state 0; Grammar::HeadersOnly starts at state 1 (pretty headers
// pane). highlightBlock formats relative to the current block
// (qsyntaxhighlighter.cpp:186-203) and chains to the next block only while
// the state changed (:140-148,205-246). One highlighter per document
// (:288-289).
class HttpRequestHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    enum class Grammar { FullRequest, HeadersOnly };

    struct Colors {
        QColor keyword;
        QColor string;
        QColor type;
        QColor error;
        QColor text;
    };

    explicit HttpRequestHighlighter(Grammar grammar, QTextDocument* parent);

    void setColors(const Colors& colors);

protected:
    void highlightBlock(const QString& text) override;

private:
    Grammar grammar_;
    Colors colors_;
};

// QtHumanRequestEditor is THE shared HTTP request editor widget (raw/pretty
// modes, bounded capacity, binary/oversize read-only fallback, revision-fenced
// pretty validation, search with match navigation). GUI-thread only; all
// validation and highlighting run synchronously on the GUI thread (inputs are
// bounded to maxBytes; QSyntaxHighlighter is synchronous GUI-thread by design,
// qsyntaxhighlighter.cpp:119-123,295-312).
class QtHumanRequestEditor : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Raw, Pretty };

    struct Config {
        QString stableId = QStringLiteral("request-editor");
        qsizetype maxBytes = (1 << 20);
        bool editable = true;
        bool allowEmpty = false;
    };

    explicit QtHumanRequestEditor(QWidget* parent = nullptr);

    void setConfig(const Config& cfg);
    void setAuthority(const QString& identity, const QString& authorityUtf8);
    void setAuthority(const QString& identity, const QByteArray& authorityBytes);
    QString authority() const;
    QString identity() const { return identity_; }
    Mode mode() const noexcept { return mode_; }
    void setMode(Mode mode);
    bool isEditable() const noexcept { return config_.editable; }
    void setEditable(bool editable);
    bool isOversized() const noexcept { return oversized_; }
    bool isBinary() const noexcept { return binary_; }
    QString errorString() const { return error_; }
    bool isDirty() const noexcept { return raw_dirty_; }
    void markClean();
    bool hasUnappliedPretty() const noexcept { return pretty_dirty_; }
    quint64 prettyRevision() const noexcept { return pretty_revision_; }
    bool isValid() const;
    void setSearchQuery(const QString& query);
    QSize sizeHint() const override;

Q_SIGNALS:
    void authorityChanged();
    void validityChanged(bool valid, const QString& error);
    void dirtyChanged(bool dirty);
    void hasUnappliedPrettyChanged(bool unapplied);
    void modeChanged(aida::qt::net::QtHumanRequestEditor::Mode mode);
    void searchChanged();
    void overCapacity();

private:
    void buildUi();
    void loadBytes(const QByteArray& bytes);
    void assignRawDocument(const QString& text);
    void assignPrettyDocuments(const http_text::parsed_request_t& parsed);
    void onRawTextChanged();
    void onPrettyChanged(bool bodyEdited);
    void markPrettyChanged();
    void schedulePrettyValidation();
    void validatePrettyNow();
    void applyPretty();
    void discardPretty();
    void recomputeMatches();
    void applyMatchSelections();
    void selectActiveMatch();
    void stepMatch(int direction);
    QString currentSource() const;
    QPlainTextEdit* visibleEdit() const;
    bool editableEffective() const;
    QString effectiveError() const;
    void refreshChrome();
    void refreshValidity();
    void setRawDirty(bool dirty);
    void setPrettyDirty(bool dirty);
    void setError(const QString& error);

    Config config_;
    QString identity_;
    QByteArray pushed_bytes_;
    std::string authority_;
    QString error_;
    QString line_ending_ = QStringLiteral("\r\n");
    QString pretty_candidate_;
    Mode mode_ = Mode::Raw;
    bool parsed_ = false;
    bool oversized_ = false;
    bool binary_ = false;
    bool raw_dirty_ = false;
    bool pretty_dirty_ = false;
    bool valid_ = false;
    bool validation_pending_ = false;
    QString last_validity_error_;
    quint64 pretty_revision_ = 0;
    quint64 validated_pretty_revision_ = 0;
    QString search_query_;
    std::vector<qsizetype> matches_;
    int active_match_ = -1;

    QPushButton* raw_button_ = nullptr;
    QPushButton* pretty_button_ = nullptr;
    QButtonGroup* mode_group_ = nullptr;
    QLineEdit* search_edit_ = nullptr;
    QToolButton* prev_button_ = nullptr;
    QToolButton* next_button_ = nullptr;
    QLabel* match_label_ = nullptr;
    QLabel* dirty_label_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    QtByteCappedPlainTextEdit* raw_edit_ = nullptr;
    QLineEdit* request_line_edit_ = nullptr;
    QtByteCappedPlainTextEdit* headers_edit_ = nullptr;
    QtByteCappedPlainTextEdit* body_edit_ = nullptr;
    QSplitter* pretty_splitter_ = nullptr;
    QWidget* pretty_footer_ = nullptr;
    QPushButton* apply_button_ = nullptr;
    QPushButton* discard_button_ = nullptr;
    QLabel* pretty_error_label_ = nullptr;
    QLabel* notice_label_ = nullptr;
    HttpRequestHighlighter* raw_highlighter_ = nullptr;
    HttpRequestHighlighter* headers_highlighter_ = nullptr;
};

}
