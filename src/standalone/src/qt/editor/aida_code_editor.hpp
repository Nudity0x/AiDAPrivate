#pragma once

#include <QAbstractScrollArea>
#include <QElapsedTimer>

#include <cstdint>
#include <memory>
#include <string>

class QTimer;
class QVariantAnimation;
class QTextLayout;

namespace aida::qt::editor {

class AidaCodeDocument;
class AidaCodeDocumentRegistry;
class AidaCodeEditorHeader;
class AidaCodeFindOverlay;
class AidaCodeGotoOverlay;
class AidaCodeAutocompletePopup;

class AidaCodeEditor : public QAbstractScrollArea {
    Q_OBJECT
public:
    AidaCodeEditor(AidaCodeDocumentRegistry* registry, quint64 document_id,
                   QWidget* parent = nullptr);
    ~AidaCodeEditor() override;

    quint64 documentId() const noexcept { return document_id_; }
    void setDocumentId(quint64 document_id);
    AidaCodeDocument* document() const noexcept;

    void reloadContent();
    void ensureCaretVisible();
    QPoint autocompleteAnchor() const;
    qreal rowHeight() const noexcept { return row_h_; }

Q_SIGNALS:
    void focusGained(quint64 document_id);
    void fileDropRequested(const QString& path);

protected:
    void paintEvent(QPaintEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private Q_SLOTS:
    void onBlinkTick();
    void onGhostDebounce();

private:
    struct layout_entry_t {
        quint64 hash = 0;
        std::shared_ptr<QTextLayout> layout;
    };

    void bindSignals();
    void refreshMetrics();
    void updateScrollbars();
    void scheduleScrollTo(float target_y);
    void screenToLineCol(const QPointF& pos, int& out_line, int& out_col);
    qreal textX0() const;
    qreal gutterWidth() const;
    qreal sourceGutterWidth() const;
    qreal foldGutterWidth() const;
    qreal lineNumberGutterWidth() const;
    int gutterDigits() const;
    qreal diffHeaderHeight() const;
    qreal cellsForColumn(const std::string& text, int column) const;
    int columnForCells(const std::string& text, qreal cells) const;
    qreal lineCellWidth(const std::string& text) const;
    QRect caretViewportRect() const;
    void repositionOverlays();
    bool minimapVisible() const;
    qreal minimapWidth() const;
    QTextLayout* layoutForLine(int line, const std::string& text);
    void invalidateLayoutCache();

    void paintGutter(QPainter& painter, int first_row, int last_row, qreal row_h);
    void paintTextLines(QPainter& painter, int first_row, int last_row, qreal row_h,
                        qreal char_w, const QRectF& clip);
    void paintSelection(QPainter& painter, int first_row, int last_row, qreal row_h,
                        qreal char_w);
    void paintFindMatches(QPainter& painter, int first_row, int last_row, qreal row_h,
                          qreal char_w);
    void paintIndentGuides(QPainter& painter, int first_row, int last_row, qreal row_h,
                           qreal char_w);
    void paintBracketMatch(QPainter& painter, qreal row_h, qreal char_w);
    void paintCaret(QPainter& painter, qreal row_h, qreal char_w);
    void paintGhostText(QPainter& painter, qreal row_h, qreal char_w);
    void paintMinimap(QPainter& painter, const QRectF& clip);
    void paintCurrentLine(QPainter& painter, qreal row_h);
    void paintDiffReview(QPainter& painter, const QRectF& clip);
    bool handleDiffMousePress(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    void ensureDiffRowVisible(int hunk_index);

    void handleTextInput(const QString& text);
    void moveCaret(int new_line, int new_col, bool shift);
    void pageStep(bool up);
    void acceptAutocomplete(int index);
    void updateAutocompletePopup();
    void hideAutocomplete();
    void updateFindOverlayGeometry();
    void updateGotoOverlayGeometry();
    void showFindOverlay(bool replace_mode);
    void showGotoOverlay();
    bool overlaysHaveFocus() const;
    void applyFocusState();
    void triggerGhostIfIdle();

    AidaCodeDocumentRegistry* registry_ = nullptr;
    quint64 document_id_ = 0;
    AidaCodeEditorHeader* header_ = nullptr;
    AidaCodeFindOverlay* find_overlay_ = nullptr;
    AidaCodeGotoOverlay* goto_overlay_ = nullptr;
    AidaCodeAutocompletePopup* autocomplete_popup_ = nullptr;
    QTimer* blink_timer_ = nullptr;
    QTimer* ghost_debounce_ = nullptr;
    QVariantAnimation* ghost_in_ = nullptr;
    QVariantAnimation* ghost_absorb_ = nullptr;
    QVariantAnimation* minimap_hover_anim_ = nullptr;
    qreal char_w_ = 8.0;
    qreal row_h_ = 16.0;
    qreal ascent_ = 0.0;
    QHash<qint32, layout_entry_t> layout_cache_;
    QElapsedTimer click_clock_;
    int diff_content_rows_ = 0;
    bool dragging_minimap_ = false;
    bool mouse_selecting_ = false;
    bool in_find_overlay_key_ = false;
    bool signals_bound_ = false;
};

}
