#pragma once

#include "core/disasm/disasm_view.hpp"

#include <QAbstractScrollArea>

#include <cstdint>
#include <memory>
#include <string>

class QFrame;
class QLabel;
class QLineEdit;
class QTimer;

namespace aida::ui::application_ui {
struct retained_entity_context_t;
}

namespace aida::qt::editor {

class AidaHexDocument;
class AidaHexDocumentRegistry;

class AidaHexEditor : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit AidaHexEditor(QWidget* parent = nullptr);
    ~AidaHexEditor() override;

    void setContext(const disasm_view::workspace_context_t& context);
    void refreshContext();
    std::shared_ptr<AidaHexDocument> document() const;

public Q_SLOTS:
    void showGotoOverlay();
    void showSearchOverlay();
    void copySelection();
    void copyAddress(std::uint64_t address);

Q_SIGNALS:
    void openDisassemblyRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void onDocumentStateChanged();

private:
    void bindDocument(const std::shared_ptr<AidaHexDocument>& document);
    void refreshMetrics();
    void updateScrollbars();
    void applyScrollToOffset();
    void prefetchVisibleWindow();
    std::int64_t cellAt(const QPointF& pos) const;
    void moveByteCursor(std::int64_t offset, bool extend);
    void ensureByteVisible(std::int64_t offset);
    void openContextMenu(const QPoint& global_pos, bool keyboard);
    void copyByte(std::uint8_t value);
    void openDisassemblyAt(std::uint64_t offset);
    void submitGoto();
    void submitSearch();
    void updateOverlayGeometry();

    disasm_view::workspace_context_t context_;
    std::shared_ptr<AidaHexDocument> document_;
    AidaHexDocumentRegistry* registry_ = nullptr;
    QString document_error_;
    QWidget* toolbar_ = nullptr;
    QLabel* source_label_ = nullptr;
    QFrame* goto_overlay_ = nullptr;
    QFrame* search_overlay_ = nullptr;
    QLineEdit* goto_edit_ = nullptr;
    QLineEdit* search_edit_ = nullptr;
    QString hex_cells_[256];
    qreal char_w_ = 8.0;
    qreal row_h_ = 16.0;
    qreal ascent_ = 0.0;
    bool selecting_ = false;
};

}
