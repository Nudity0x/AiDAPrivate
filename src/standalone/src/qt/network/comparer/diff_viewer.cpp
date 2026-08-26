#include "qt/network/comparer/diff_viewer.hpp"

#include <QHBoxLayout>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::net {

namespace {

bool utf8_is_continuation(std::uint8_t byte) {
    return (byte & 0xC0) == 0x80;
}

bool utf8_is_noncharacter(char32_t ucs4) {
    return ucs4 >= 0xfdd0 && (ucs4 <= 0xfdef || (ucs4 & 0xfffe) == 0xfffe);
}

}

void decode_utf8_with_cu_map(const std::vector<std::uint8_t>& bytes,
                             comparer_diff_side_t& out) {
    const std::size_t n = bytes.size();
    out.cuStart.fill(0, static_cast<qsizetype>(n));
    out.cuNext.fill(0, static_cast<qsizetype>(n));
    out.text.clear();
    out.text.reserve(static_cast<qsizetype>(n));

    qint32 cu = 0;
    std::size_t i = 0;
    if (n >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        out.cuStart[0] = out.cuNext[0] = 0;
        out.cuStart[1] = out.cuNext[1] = 0;
        out.cuStart[2] = out.cuNext[2] = 0;
        i = 3;
    }
    while (i < n) {
        const std::uint8_t b = bytes[i];
        out.cuStart[static_cast<qsizetype>(i)] = cu;
        if (b < 0x80) {
            out.text += QLatin1Char(static_cast<char>(b));
            out.cuNext[static_cast<qsizetype>(i)] = cu + 1;
            ++i;
            ++cu;
            continue;
        }
        if (b <= 0xC1 || b >= 0xF5) {
            out.text += QChar::ReplacementCharacter;
            out.cuNext[static_cast<qsizetype>(i)] = cu + 1;
            ++i;
            ++cu;
            continue;
        }
        const int needed = b < 0xE0 ? 2 : (b < 0xF0 ? 3 : 4);
        const char32_t min_uc = needed == 2 ? 0x80 : (needed == 3 ? 0x800 : 0x10000);
        char32_t uc = b & (needed == 2 ? 0x1F : (needed == 3 ? 0x0F : 0x07));
        bool error = false;
        for (int k = 0; k < needed - 1; ++k) {
            if (i + 1 + static_cast<std::size_t>(k) >= n) {
                error = true;
                break;
            }
            const std::uint8_t continuation = bytes[i + 1 + static_cast<std::size_t>(k)];
            if (!utf8_is_continuation(continuation)) {
                error = true;
                break;
            }
            uc = (uc << 6) | (continuation & 0x3F);
        }
        if (!error) {
            if (uc < min_uc || (uc - 0xD800u) < 2048u || uc > 0x10FFFF ||
                utf8_is_noncharacter(uc))
                error = true;
        }
        if (error) {
            out.text += QChar::ReplacementCharacter;
            out.cuNext[static_cast<qsizetype>(i)] = cu + 1;
            ++i;
            ++cu;
            continue;
        }
        const qint32 width = needed == 4 ? 2 : 1;
        if (needed == 4) {
            const char32_t high = 0xD800 + ((uc - 0x10000) >> 10);
            const char32_t low = 0xDC00 + ((uc - 0x10000) & 0x3FF);
            out.text += QChar(static_cast<char16_t>(high));
            out.text += QChar(static_cast<char16_t>(low));
        } else {
            out.text += QChar(static_cast<char16_t>(uc));
        }
        for (int k = 0; k < needed; ++k) {
            out.cuStart[static_cast<qsizetype>(i + static_cast<std::size_t>(k))] = cu;
            out.cuNext[static_cast<qsizetype>(i + static_cast<std::size_t>(k))] = cu + width;
        }
        i += static_cast<std::size_t>(needed);
        cu += width;
    }
    out.cuTotal = cu;
}

DiffTextEdit::DiffTextEdit(QWidget* parent)
    : QPlainTextEdit(parent) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFont(theme::fonts::codeRegular());
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    gutter_ = new LineNumberGutter(this);
    relayoutGutter();
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this](int) {
        relayoutGutter();
    });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        if (dy)
            gutter_->scroll(0, dy);
        else
            gutter_->update(0, rect.y(), gutter_->width(), rect.height());
        if (rect.contains(viewport()->rect()))
            relayoutGutter();
    });
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        relayoutGutter();
        gutter_->update();
    });
}

int DiffTextEdit::gutterWidth() const {
    if (!line_numbers_)
        return 0;
    const int digits = qMax(2, QString::number(qMax(1, blockCount())).length());
    return theme::tokens().spacing.md +
        fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void DiffTextEdit::relayoutGutter() {
    setViewportMargins(gutterWidth(), 0, 0, 0);
    const QRect cr = contentsRect();
    gutter_->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
}

void DiffTextEdit::setLineNumberVisible(bool visible) {
    if (line_numbers_ == visible)
        return;
    line_numbers_ = visible;
    gutter_->setVisible(visible);
    relayoutGutter();
}

void DiffTextEdit::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    relayoutGutter();
}

void DiffTextEdit::gutterPaint(QWidget* gutter, QPaintEvent* event) {
    QPainter painter(gutter);
    const auto& t = theme::tokens();
    painter.fillRect(event->rect(), t.panel_header);
    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(t.text_lineno);
            painter.drawText(0, top, gutter->width() - t.spacing.xs,
                static_cast<int>(blockBoundingRect(block).height()),
                Qt::AlignRight | Qt::AlignVCenter, QString::number(blockNumber + 1));
        }
        block = block.next();
        top = bottom;
        bottom = block.isValid()
            ? top + static_cast<int>(blockBoundingRect(block).height()) : top;
        ++blockNumber;
    }
}

LineNumberGutter::LineNumberGutter(DiffTextEdit* editor)
    : QWidget(editor), editor_(editor) {}

QSize LineNumberGutter::sizeHint() const {
    return QSize(editor_->gutterWidth(), 0);
}

void LineNumberGutter::paintEvent(QPaintEvent* event) {
    editor_->gutterPaint(this, event);
}

ComparerDiffViewer::ComparerDiffViewer(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::tokens().spacing.sm);
    edit_a_ = new DiffTextEdit(this);
    edit_a_->setPlaceholderText(QStringLiteral("Comparer input A"));
    edit_b_ = new DiffTextEdit(this);
    edit_b_->setPlaceholderText(QStringLiteral("Comparer input B"));
    layout->addWidget(edit_a_, 1);
    layout->addWidget(edit_b_, 1);

    edit_a_->setContextMenuPolicy(Qt::CustomContextMenu);
    edit_b_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(edit_a_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        Q_EMIT contextMenuRequested(true, edit_a_->viewport()->mapToGlobal(pos));
    });
    connect(edit_b_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        Q_EMIT contextMenuRequested(false, edit_b_->viewport()->mapToGlobal(pos));
    });

    connect(edit_a_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        const QSignalBlocker blocker(edit_b_->verticalScrollBar());
        edit_b_->verticalScrollBar()->setValue(value);
    });
    connect(edit_b_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        const QSignalBlocker blocker(edit_a_->verticalScrollBar());
        edit_a_->verticalScrollBar()->setValue(value);
    });
}

void ComparerDiffViewer::setDiff(
    const std::shared_ptr<const comparer_diff_content_t>& content) {
    content_ = content;
    if (!content_) {
        edit_a_->clear();
        edit_b_->clear();
        return;
    }
    edit_a_->setPlainText(content_->sideA.text);
    edit_b_->setPlainText(content_->sideB.text);
    rebuildSelections();
}

void ComparerDiffViewer::clearDiff() {
    content_.reset();
    edit_a_->clear();
    edit_b_->clear();
    edit_a_->setExtraSelections({});
    edit_b_->setExtraSelections({});
}

void ComparerDiffViewer::rebuildSelections() {
    if (!content_) {
        edit_a_->setExtraSelections({});
        edit_b_->setExtraSelections({});
        return;
    }
    const auto& t = theme::tokens();
    QColor insertColor = t.success;
    insertColor.setAlphaF(0.30);
    QColor deleteColor = t.error;
    deleteColor.setAlphaF(0.30);
    QColor replaceColor = t.warning;
    replaceColor.setAlphaF(0.30);

    QList<QTextEdit::ExtraSelection> selectionsA;
    QList<QTextEdit::ExtraSelection> selectionsB;
    const auto& sideA = content_->sideA;
    const auto& sideB = content_->sideB;
    for (const auto& block : content_->blocks) {
        if (block.kind == aida::burp::comparer::diff_block_t::kind_t::equal)
            continue;
        QColor color;
        switch (block.kind) {
            case aida::burp::comparer::diff_block_t::kind_t::insert:  color = insertColor; break;
            case aida::burp::comparer::diff_block_t::kind_t::delete_: color = deleteColor; break;
            case aida::burp::comparer::diff_block_t::kind_t::replace: color = replaceColor; break;
            default: break;
        }
        const auto spanFor = [](const comparer_diff_side_t& side, std::size_t start,
                                std::size_t end, qint32& cuStartOut, qint32& cuEndOut) {
            const qint32 n = static_cast<qint32>(side.cuStart.size());
            cuStartOut = start < static_cast<std::size_t>(n)
                ? side.cuStart[static_cast<qsizetype>(start)] : side.cuTotal;
            cuEndOut = end == 0 ? 0
                : end <= static_cast<std::size_t>(n)
                    ? side.cuNext[static_cast<qsizetype>(end - 1)] : side.cuTotal;
        };
        qint32 aCuStart = 0;
        qint32 aCuEnd = 0;
        spanFor(sideA, block.a_start, block.a_end, aCuStart, aCuEnd);
        if (aCuEnd > aCuStart) {
            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(color);
            selection.cursor = QTextCursor(edit_a_->document());
            selection.cursor.setPosition(aCuStart, QTextCursor::MoveAnchor);
            selection.cursor.setPosition(aCuEnd, QTextCursor::KeepAnchor);
            selectionsA.push_back(selection);
        }
        qint32 bCuStart = 0;
        qint32 bCuEnd = 0;
        spanFor(sideB, block.b_start, block.b_end, bCuStart, bCuEnd);
        if (bCuEnd > bCuStart) {
            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(color);
            selection.cursor = QTextCursor(edit_b_->document());
            selection.cursor.setPosition(bCuStart, QTextCursor::MoveAnchor);
            selection.cursor.setPosition(bCuEnd, QTextCursor::KeepAnchor);
            selectionsB.push_back(selection);
        }
    }
    edit_a_->setExtraSelections(selectionsA);
    edit_b_->setExtraSelections(selectionsB);
}

}
