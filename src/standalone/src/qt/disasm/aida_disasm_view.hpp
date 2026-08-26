#pragma once

#include "core/disasm/disasm_view.hpp"
#include "core/ui/analysis_context_menu.hpp"
#include "qt/analysis_bridge/revision_poller.hpp"
#include "qt/disasm/disasm_painter.hpp"
#include "qt/theme/disasm_theme_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

#include <QAbstractScrollArea>
#include <QPointer>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QFocusEvent;
class QPainter;

namespace aida::qt::analysis_bridge {
struct view_hooks_t;
}

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::disasm {

class DisasmRowCache;
class DisasmToolbar;
class DisasmGotoStrip;
namespace dialogs {
class AidaDisasmXrefPopup;
}

class AidaDisasmNavBand : public QWidget {
    Q_OBJECT
public:
    explicit AidaDisasmNavBand(QWidget* parent = nullptr);

    void set_model(const std::shared_ptr<const aida::analysis::analysis_snapshot_t>& snapshot,
                   std::size_t range_first, std::size_t range_second,
                   DisasmRowCache* rows);
    void set_selection(const std::optional<aida::analysis::address_t>& selection);
    void invalidate_colors();
    void set_theme_revision(quint64 revision);

Q_SIGNALS:
    void navigateToIndex(std::size_t instruction_index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void rebuild_colors();

    std::shared_ptr<const aida::analysis::analysis_snapshot_t> snapshot_;
    std::size_t range_first_ = 0;
    std::size_t range_second_ = 0;
    DisasmRowCache* rows_ = nullptr;
    std::optional<aida::analysis::address_t> selection_;
    std::vector<QColor> colors_;
    const void* colors_instructions_ = nullptr;
    std::size_t colors_range_first_ = 0;
    std::size_t colors_range_second_ = 0;
    std::size_t colors_marker_step_ = 0;
    quint64 colors_revision_ = 0;
    quint64 colors_theme_revision_ = 0;
};

class AidaDisasmView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit AidaDisasmView(QWidget* parent = nullptr);
    explicit AidaDisasmView(std::string presentation_key, QWidget* parent = nullptr);
    ~AidaDisasmView() override;

    std::string presentation_key() const { return presentation_key_; }
    disasm_view::workspace_context_t context() const { return context_; }
    void refreshContext();

    static void ensure_dialog_hooks_installed();

public Q_SLOTS:
    void showGotoStrip();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private Q_SLOTS:
    void onRevisionChanged(quint64 combined, quint64 overlayRevision);
    void onUiSerialChanged(quint64 serial);
    void onRowCacheRevision(quint64 revision);
    void onXrefResults(aida::analysis::address_t address,
                       std::vector<disasm_view::xref_popup_entry_t> results,
                       QString error);
    void onExportStatus(QString status, QString error);

private:
    struct metadata_row_t {
        QString text;
        disasm_view::metadata_line_kind_t kind = disasm_view::metadata_line_kind_t::comment;
    };

    void recaptureContext();
    void recomputeTextMetrics();
    void applyHorizontalExtent();
    analysis_bridge::revision_sample_t sampleRevisions();
    void syncFromModel();
    void syncUiState();
    void syncPresentation();
    void syncMetadata();
    void syncBookmarks();
    void syncSelection();
    void updateScrollbars();
    void updateMargins();
    void applyScrollRestore();
    void applyScrollToSelection();
    void prefetchVisible();
    void paintListing(QPainter& painter, QPaintEvent* event);
    disasm_frame_geometry_t frameGeometry(const QFont& code_font);
    std::size_t instructionRowCount() const;
    std::optional<std::size_t> selectedInstructionRow() const;
    void moveSelectionBy(std::ptrdiff_t delta, bool extend_range);
    void selectInstructionRow(std::size_t row, bool extend_range);
    void ensureInstructionRowVisible(std::size_t row);
    void clearRangeSelection();
    bool rangeContains(std::size_t instruction_row) const;
    void copyRangeToClipboard();
    void followSelectionTarget();
    std::optional<std::size_t> rowAt(const QPointF& pos) const;
    std::optional<aida::analysis::instruction_record_t> instructionAtRow(
        std::size_t row) const;
    std::optional<std::size_t> instructionRowFor(
        const aida::analysis::address_t& address) const;
    void openInstructionMenu(const aida::analysis::instruction_record_t& instruction,
                             const std::optional<disasm_view::formatted_instruction_t>& formatted,
                             aida::ui::context_menu_open_origin_t origin,
                             const QPoint& global_pos);
    void openMetadataMenu(std::size_t line_index,
                          aida::ui::context_menu_open_origin_t origin,
                          const QPoint& global_pos);
    aida::ui::analysis_context_menu::context_t makeInstructionMenu(
        const aida::analysis::instruction_record_t& instruction,
        const std::optional<disasm_view::formatted_instruction_t>& formatted);
    std::optional<aida::ui::analysis_context_menu::context_t> makeMetadataMenu(
        std::size_t line_index);
    void openXrefPopup(const aida::analysis::address_t& address);
    void showEventState();
    void clearNotices();
    void addNotice(const QString& id, const QString& title, const QString& message,
                   widgets::AidaSemantic kind, const QString& action_label,
                   std::function<void()> action);

    std::string presentation_key_;
    disasm_view::workspace_context_t context_;
    DisasmRowCache* row_cache_ = nullptr;
    analysis_bridge::AidaRevisionPoller* poller_ = nullptr;
    DisasmToolbar* toolbar_ = nullptr;
    DisasmGotoStrip* goto_strip_ = nullptr;
    QWidget* notices_ = nullptr;
    AidaDisasmNavBand* nav_band_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    std::shared_ptr<analysis_bridge::view_hooks_t> hooks_;

    std::vector<metadata_row_t> metadata_lines_;
    std::optional<aida::analysis::address_t> selection_;
    std::optional<std::size_t> range_anchor_;
    std::optional<std::size_t> range_extent_;
    std::shared_ptr<const std::vector<disasm_view::bookmark_t>> bookmarks_;
    const void* bookmarks_publication_ = nullptr;
    std::uint64_t bookmarks_overlay_revision_ = 0;
    int hover_row_ = -1;
    bool banner_selected_all_ = false;
    std::size_t banner_selected_line_ = 0;
    bool drag_selecting_ = false;
    bool paint_show_bytes_ = true;
    int paint_addr_format_ = 0;
    int row_height_ = 16;
    qreal char_width_ = 8.0;
    qreal content_width_max_ = 0.0;
    prefix_width_cache_t prefix_cache_;
    theme::AidaDisasmTheme theme_;
    quint64 theme_revision_ = 0;
    quint64 last_publication_generation_ = 0;
    quint64 last_analysis_revision_ = 0;
    quint64 last_overlay_revision_ = 0;
    QPointer<dialogs::AidaDisasmXrefPopup> xref_popup_;
};

}
