#pragma once

#include <QPlainTextEdit>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/network/burp/comparer.hpp"

class QScrollBar;

namespace aida::qt::net {

// One decoded comparer slot side: the display text plus the per-byte
// byte-offset -> UTF-16 code-unit endpoint map. diff_block_t
// a_start/a_end/b_start/b_end are BYTE offsets (comparer.hpp:38-41) while
// QTextCursor positions are UTF-16 code-unit offsets (qtextcursor.h:52), so
// the worker builds this map in a single pass over the <= 32 KiB decoded text
// mirroring the vendored decoder (qstringconverter_p.h:198-289 semantics:
// US-ASCII 1:1; valid 2/3-byte -> 1 code unit; valid 4-byte -> 2; any error
// consumes exactly the lead byte and emits one U+FFFD per byte, stateless
// decode path qstringconverter.cpp:636-670; UTF-8 BOM at offset 0 skipped
// qstringconverter.cpp:647-651; QChar::isNonCharacter qchar.h:491-494).
struct comparer_diff_side_t {
    QString text;
    QVector<qint32> cuStart;
    QVector<qint32> cuNext;
    qint32 cuTotal = 0;
};

struct comparer_diff_content_t {
    std::uint64_t generation = 0;
    comparer_diff_side_t sideA;
    comparer_diff_side_t sideB;
    std::vector<aida::burp::comparer::diff_block_t> blocks;
};

void decode_utf8_with_cu_map(const std::vector<std::uint8_t>& bytes,
                             comparer_diff_side_t& out);

// DiffTextEdit is one side of the comparer: read-only, NoWrap
// (qplaintextedit.h:51,100), with the LineNumberGutter in the left margin.
class LineNumberGutter;

class DiffTextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit DiffTextEdit(QWidget* parent = nullptr);

    int gutterWidth() const;
    void gutterPaint(QWidget* gutter, QPaintEvent* event);
    void setLineNumberVisible(bool visible);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void relayoutGutter();

    LineNumberGutter* gutter_ = nullptr;
    bool line_numbers_ = true;
};

// LineNumberGutter: the documented updateRequest gutter hook
// (qplaintextedit.cpp:2948-2959) — paints block numbers for the visible
// blocks via firstVisibleBlock/blockBoundingRect (qplaintextedit.h:233,235).
class LineNumberGutter : public QWidget {
    Q_OBJECT
public:
    explicit LineNumberGutter(DiffTextEdit* editor);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    DiffTextEdit* editor_;
};

// ComparerDiffViewer: two DiffTextEdits with per-diff-generation
// ExtraSelections (stored as FormatRanges painted per visible block,
// qplaintextedit.cpp:2532-2546) replacing the per-line clipper/CalcTextSize
// scan, and two-way Y scroll sync under QSignalBlocker (qobject.h:482,555 —
// the guard-flag port of the legacy sync_scroll_y).
class ComparerDiffViewer : public QWidget {
    Q_OBJECT
public:
    explicit ComparerDiffViewer(QWidget* parent = nullptr);

    void setDiff(const std::shared_ptr<const comparer_diff_content_t>& content);
    void clearDiff();

    DiffTextEdit* editA() const noexcept { return edit_a_; }
    DiffTextEdit* editB() const noexcept { return edit_b_; }

Q_SIGNALS:
    void contextMenuRequested(bool sideA, const QPoint& globalPos);

private:
    void rebuildSelections();

    DiffTextEdit* edit_a_ = nullptr;
    DiffTextEdit* edit_b_ = nullptr;
    std::shared_ptr<const comparer_diff_content_t> content_;
};

}
