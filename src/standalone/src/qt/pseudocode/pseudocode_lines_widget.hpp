#pragma once

#include "core/disasm/disasm_view.hpp"
#include "core/ui/analysis_context_menu.hpp"
#include "core/workbench/adapters/pseudocode_document.hpp"

#include <QAbstractScrollArea>
#include <QRectF>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace aida::qt::pseudocode {

class PseudocodeLinesWidget : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit PseudocodeLinesWidget(QWidget* parent = nullptr);

    void setContext(const disasm_view::workspace_context_t& context);
    void reload();
    void copyAll();
    void set_rename_local_handler(std::function<void(std::string old_name)> handler);

Q_SIGNALS:
    void navigateToDisassembly(quint64 address);
    void navigateToGraph(quint64 address);
    void lineSelectionChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    bool viewportEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct token_span_t {
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
        QRectF rect;
        QString text;
        std::optional<std::uint64_t> address;
    };
    struct line_layout_t {
        aida::workbench::pseudocode_document::pseudocode_line_view_t line;
        aida::workbench::pseudocode_document::pseudocode_page_t page;
        QVector<token_span_t> spans;
        qreal text_width = 0.0;
    };

    void updateScrollbars();
    void updateGutterMetrics();
    int lineAt(const QPointF& pos) const;
    qreal contentX(const QPointF& viewport_pos) const;
    void ensureLineVisible(int line_index);
    void moveLineSelection(int line_index);
    void applyLineSelection(const line_layout_t& layout, std::uint32_t token_begin,
                            std::uint32_t token_end,
                            std::optional<std::uint64_t> source_address);
    std::optional<std::uint64_t> selectionSourceAddress();
    bool buildLineLayout(int line_index, line_layout_t& output);
    void openLineContextMenu(int line_index,
                             aida::ui::context_menu_open_origin_t origin,
                             const QPoint& global_pos);
    aida::ui::analysis_context_menu::context_t makeLineContextMenu(
        const aida::workbench::pseudocode_document::pseudocode_line_view_t& line,
        std::optional<std::uint64_t> source_address, std::string token_text,
        int selected_line, std::uint32_t selected_token_begin,
        std::uint32_t selected_token_end, std::string rename_candidate);
    aida::workbench::pseudocode_document::pseudocode_document_model_t* model() const;
    QColor tokenColor(aida::analysis::decompiler_document_token_kind_t kind) const;

    disasm_view::workspace_context_t context_;
    std::uint32_t total_lines_ = 0;
    int selected_line_ = -1;
    std::uint32_t selected_token_begin_ = 0;
    std::uint32_t selected_token_end_ = 0;
    int hover_line_ = -1;
    qreal row_height_ = 0.0;
    qreal gutter_width_ = 0.0;
    qreal gutter_text_pad_ = 0.0;
    qreal text_pad_ = 0.0;
    qreal char_width_ = 1.0;
    qreal max_text_width_ = 0.0;
    std::function<void(std::string old_name)> rename_local_handler_;
};

}
