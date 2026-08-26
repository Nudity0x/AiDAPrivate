#include "qt/editor/aida_hex_editor.hpp"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpressionValidator>
#include <QScrollBar>
#include <QShowEvent>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>

#include "core/ai/entity_evidence_handoff.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/editor/aida_hex_document.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_theme_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::editor {

namespace {

using widgets::with_alpha;

struct hex_row_geometry_t {
    qreal address_x = 0.0;
    qreal hex_x = 0.0;
    qreal ascii_x = 0.0;
    qreal content_w = 0.0;
};

hex_row_geometry_t hexRowGeometry(qreal char_w)
{
    const auto& spacing = theme::tokens().spacing;
    hex_row_geometry_t geometry;
    geometry.address_x = spacing.md;
    geometry.hex_x = geometry.address_x + 16.0 * char_w + spacing.xxl;
    geometry.ascii_x = geometry.hex_x + 16.0 * 3.0 * char_w + spacing.lg;
    geometry.content_w = geometry.ascii_x + 16.0 * char_w + spacing.md;
    return geometry;
}

std::optional<aida::analysis::address_t> normalized_address_for_file_offset(
    const disasm_view::workspace_context_t& context, std::uint64_t offset)
{
    if (!context.workspace)
        return {};
    const auto image = context.workspace->normalized_image();
    if (!image)
        return {};
    const auto& identity = context.workspace->identity();
    for (const auto& mapping : image->address_mappings) {
        if (mapping.source_space != aida::analysis::address_space_id_t::file_offset ||
            mapping.size == 0 || offset < mapping.source_start)
            continue;
        const std::uint64_t delta = offset - mapping.source_start;
        if (delta >= mapping.size)
            continue;
        if (mapping.target_start >
            (std::numeric_limits<std::uint64_t>::max)() - delta)
            return {};
        aida::analysis::address_t address;
        address.space = mapping.target_space;
        address.value = mapping.target_start + delta;
        address.architecture = identity.architecture();
        address.mode = image->architecture_mode;
        return address;
    }
    return {};
}

std::optional<std::uint64_t> display_address_for_file_offset(
    const disasm_view::workspace_context_t& context, std::uint64_t offset)
{
    auto address = normalized_address_for_file_offset(context, offset);
    if (address) {
        if (address->space == aida::analysis::address_space_id_t::virtual_address)
            return address->value;
        if (address->space == aida::analysis::address_space_id_t::relative_virtual) {
            const auto image = context.workspace ? context.workspace->normalized_image() : nullptr;
            if (!image)
                return {};
            if (image->image_base >
                (std::numeric_limits<std::uint64_t>::max)() - address->value)
                return {};
            return image->image_base + address->value;
        }
    }
    if (context.image) {
        auto rva = context.image->file_offset_to_rva(offset);
        if (rva) {
            auto va = context.image->rva_to_va(rva.value());
            if (va)
                return va.value();
        }
    }
    return {};
}

std::optional<std::uint64_t> file_offset_for_normalized_address(
    const disasm_view::workspace_context_t& context, std::uint64_t value)
{
    if (!context.workspace)
        return {};
    const auto image = context.workspace->normalized_image();
    if (!image)
        return {};
    const auto checked_mapping_offset = [&](aida::analysis::address_space_id_t target_space,
                                            std::uint64_t target_value) -> std::optional<std::uint64_t> {
        for (const auto& mapping : image->address_mappings) {
            if (mapping.source_space != aida::analysis::address_space_id_t::file_offset ||
                mapping.target_space != target_space || mapping.size == 0 ||
                target_value < mapping.target_start)
                continue;
            const std::uint64_t delta = target_value - mapping.target_start;
            if (delta >= mapping.size)
                continue;
            if (mapping.source_start >
                (std::numeric_limits<std::uint64_t>::max)() - delta)
                return {};
            return mapping.source_start + delta;
        }
        return {};
    };
    if (auto offset = checked_mapping_offset(
            aida::analysis::address_space_id_t::virtual_address, value))
        return offset;
    if (image->image_base <= value) {
        const std::uint64_t rva = value - image->image_base;
        if (auto offset = checked_mapping_offset(
                aida::analysis::address_space_id_t::relative_virtual, rva))
            return offset;
    }
    return checked_mapping_offset(aida::analysis::address_space_id_t::relative_virtual, value);
}

}

AidaHexEditor::AidaHexEditor(QWidget* parent) : QAbstractScrollArea(parent)
{
    setObjectName(QStringLiteral("aida.document.hex.primary"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_StaticContents);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    registry_ = &AidaHexDocumentRegistry::instance();

    for (int value = 0; value < 256; ++value) {
        char encoded[4]{};
        std::snprintf(encoded, sizeof(encoded), "%02X", value);
        hex_cells_[value] = QString::fromLatin1(encoded);
    }

    toolbar_ = new QWidget(this);
    toolbar_->setObjectName(QStringLiteral("aida.hex.toolbar"));
    toolbar_->setFixedHeight(theme::tokens().toolbar.height);
    auto* toolbar_layout = new QHBoxLayout(toolbar_);
    toolbar_layout->setContentsMargins(theme::tokens().toolbar.padding_x,
        theme::tokens().toolbar.padding_y, theme::tokens().toolbar.padding_x,
        theme::tokens().toolbar.padding_y);
    toolbar_layout->setSpacing(theme::tokens().toolbar.group_gap);
    auto* search_button = new widgets::AidaButton(QStringLiteral("Search"), toolbar_);
    search_button->setKind(widgets::AidaButton::Kind::Ghost);
    search_button->setControlSize(widgets::AidaButton::ControlSize::Small);
    toolbar_layout->addWidget(search_button);
    auto* goto_button = new widgets::AidaButton(QStringLiteral("Go to"), toolbar_);
    goto_button->setKind(widgets::AidaButton::Kind::Ghost);
    goto_button->setControlSize(widgets::AidaButton::ControlSize::Small);
    toolbar_layout->addWidget(goto_button);
    auto* copy_button = new widgets::AidaButton(QStringLiteral("Copy selection"), toolbar_);
    copy_button->setKind(widgets::AidaButton::Kind::Ghost);
    copy_button->setControlSize(widgets::AidaButton::ControlSize::Small);
    copy_button->setToolTip(QStringLiteral("Copy the selected byte range"));
    toolbar_layout->addWidget(copy_button);
    source_label_ = new QLabel(toolbar_);
    toolbar_layout->addWidget(source_label_, 1);
    setViewportMargins(0, toolbar_->height(), 0, 0);
    toolbar_->setGeometry(0, 0, width(), toolbar_->height());
    toolbar_->show();
    connect(search_button, &QAbstractButton::clicked, this, [this] { showSearchOverlay(); });
    connect(goto_button, &QAbstractButton::clicked, this, [this] { showGotoOverlay(); });
    connect(copy_button, &QAbstractButton::clicked, this, [this] { copySelection(); });

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        prefetchVisibleWindow();
        viewport()->update();
    });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        viewport()->update();
    });
    connect(&theme::AidaThemeController::instance(), &theme::AidaThemeController::themeChanged,
        this, [this] {
            refreshMetrics();
            updateScrollbars();
            viewport()->update();
        });

    refreshMetrics();
}

AidaHexEditor::~AidaHexEditor() = default;

void AidaHexEditor::refreshMetrics()
{
    const int pixel_size = std::clamp(static_cast<int>(g_sa_settings.editor_font_size), 8, 48);
    QFont font = theme::fonts::code(400, pixel_size);
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontInfo info(font);
    if (!info.fixedPitch())
        font = theme::fonts::codeRegular();
    const QFontMetricsF fm(font);
    char_w_ = fm.horizontalAdvance(QChar(u'X'));
    row_h_ = fm.lineSpacing();
    ascent_ = fm.ascent();
    setFont(font);
    viewport()->setFont(font);
}

void AidaHexEditor::setContext(const disasm_view::workspace_context_t& context)
{
    context_ = context;
    bindDocument(context_ ? registry_->stateFor(context_) : nullptr);
    updateScrollbars();
    viewport()->update();
}

void AidaHexEditor::refreshContext()
{
    setContext(disasm_view::capture_selected_workspace());
}

void AidaHexEditor::showEvent(QShowEvent* event)
{
    QAbstractScrollArea::showEvent(event);
    refreshContext();
}

std::shared_ptr<AidaHexDocument> AidaHexEditor::document() const
{
    return document_;
}

void AidaHexEditor::bindDocument(const std::shared_ptr<AidaHexDocument>& document)
{
    if (document_ == document)
        return;
    if (document_)
        document_->disconnect(this);
    document_ = document;
    document_error_.clear();
    if (document_) {
        document_error_ = QString::fromStdString(document_->lastError());
        connect(document_.get(), &AidaHexDocument::stateChanged, this,
                &AidaHexEditor::onDocumentStateChanged);
        connect(document_.get(), &AidaHexDocument::searchStateChanged, this,
                &AidaHexEditor::onDocumentStateChanged);
        connect(document_.get(), &AidaHexDocument::liveStateChanged, this,
                &AidaHexEditor::onDocumentStateChanged);
        connect(document_.get(), &AidaHexDocument::errorChanged, this,
                &AidaHexEditor::onDocumentStateChanged);
    }
}

void AidaHexEditor::onDocumentStateChanged()
{
    document_error_ = document_ ? QString::fromStdString(document_->lastError())
        : QString();
    if (document_ && context_ && source_label_) {
        const bool live = [&] {
            std::lock_guard<std::mutex> lock(document_->mutex);
            return document_->source_kind == hex_source_kind_t::live_memory;
        }();
        const QString name = QString::fromStdString(document_->sourceName(context_));
        source_label_->setText(name + (live ? QStringLiteral("  [Live memory]")
            : QStringLiteral("  [Workspace image]")));
    }
    updateScrollbars();
    applyScrollToOffset();
    viewport()->update();
}

void AidaHexEditor::updateScrollbars()
{
    if (!document_ || !context_) {
        verticalScrollBar()->setRange(0, 0);
        horizontalScrollBar()->setRange(0, 0);
        return;
    }
    const std::uint64_t byte_count = document_->byteCount(context_);
    const std::uint64_t row_count64 = (byte_count + 15) / 16;
    const int row_count = row_count64 > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())
        ? (std::numeric_limits<int>::max)() : static_cast<int>(row_count64);
    const int page_rows = std::max(1, static_cast<int>(viewport()->height() / row_h_));
    verticalScrollBar()->setRange(0, std::max(0, row_count - page_rows));
    verticalScrollBar()->setSingleStep(1);
    verticalScrollBar()->setPageStep(page_rows);
    const qreal content_w = hexRowGeometry(char_w_).content_w;
    const qreal text_w = viewport()->width();
    horizontalScrollBar()->setRange(0, std::max(0, static_cast<int>(content_w - text_w)));
    horizontalScrollBar()->setSingleStep(std::max(1, static_cast<int>(char_w_)));
    horizontalScrollBar()->setPageStep(std::max(1, static_cast<int>(text_w)));
}

void AidaHexEditor::prefetchVisibleWindow()
{
    if (!document_ || !context_)
        return;
    const std::uint64_t byte_count = document_->byteCount(context_);
    if (byte_count == 0)
        return;
    const int first_row = std::max(0, verticalScrollBar()->value());
    const int last_row = first_row + std::max(1, verticalScrollBar()->pageStep());
    const std::uint64_t view_begin = static_cast<std::uint64_t>(first_row) * 16;
    const std::uint64_t view_end = (std::min)(byte_count,
        static_cast<std::uint64_t>(last_row) * 16 + 16);
    if (view_begin < view_end)
        document_->ensureWindow(context_, view_begin, view_end);
}

void AidaHexEditor::applyScrollToOffset()
{
    if (!document_)
        return;
    std::uint64_t offset = 0;
    {
        std::lock_guard<std::mutex> lock(document_->mutex);
        if (document_->scroll_to_offset == (std::numeric_limits<std::uint64_t>::max)())
            return;
        offset = document_->scroll_to_offset;
        document_->scroll_to_offset = (std::numeric_limits<std::uint64_t>::max)();
    }
    verticalScrollBar()->setValue(static_cast<int>(offset / 16));
}

void AidaHexEditor::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    const auto& t = theme::tokens();
    const auto& spacing = t.spacing;
    painter.fillRect(event->rect(), t.bg_base);
    if (!context_ || !document_) {
        painter.setPen(t.text_dim);
        painter.drawText(QPointF(spacing.md, spacing.xxl),
            QStringLiteral("No workspace selected"));
        painter.drawText(QPointF(spacing.md, spacing.xxl + row_h_),
            QStringLiteral("Open or attach to a target to inspect its bytes."));
        return;
    }
    if (document_->live_loading.load(std::memory_order_acquire)) {
        painter.setPen(t.text_dim);
        painter.drawText(QPointF(spacing.md, spacing.xxl),
            QStringLiteral("Reading target bytes"));
        painter.drawText(QPointF(spacing.md, spacing.xxl + row_h_),
            QStringLiteral("A bounded live-memory read is in progress."));
        return;
    }

    const bool live_source = [&] {
        std::lock_guard<std::mutex> lock(document_->mutex);
        return document_->source_kind == hex_source_kind_t::live_memory;
    }();
    if (!live_source)
        document_->requestPatchRefresh(context_);

    const std::uint64_t byte_count = document_->byteCount(context_);
    if (byte_count == 0) {
        if (!document_error_.isEmpty()) {
            painter.setPen(t.error);
            painter.drawText(QPointF(spacing.md, spacing.xxl),
                QStringLiteral("Hex source error"));
            painter.setPen(t.text_dim);
            const int text_w = (std::max)(1, viewport()->width() - spacing.md * 2);
            painter.drawText(QPointF(spacing.md, spacing.xxl + row_h_),
                painter.fontMetrics().elidedText(document_error_, Qt::ElideRight, text_w));
            return;
        }
        painter.setPen(t.text_dim);
        painter.drawText(QPointF(spacing.md, spacing.xxl), QStringLiteral("No bytes available"));
        painter.drawText(QPointF(spacing.md, spacing.xxl + row_h_),
            live_source ? QStringLiteral("The selected memory snapshot does not contain readable bytes.")
                        : QStringLiteral("The workspace provider has not published readable bytes."));
        return;
    }

    const hex_row_geometry_t geometry = hexRowGeometry(char_w_);
    const qreal row_count_f = static_cast<qreal>(byte_count + 15) / 16.0;
    const int total_rows = row_count_f > static_cast<qreal>((std::numeric_limits<int>::max)())
        ? (std::numeric_limits<int>::max)() : static_cast<int>(row_count_f);
    const int first_row = std::clamp(verticalScrollBar()->value(), 0, total_rows - 1);
    const int last_row = std::min(first_row + verticalScrollBar()->pageStep() + 1,
        total_rows - 1);
    if (first_row > last_row)
        return;

    prefetchVisibleWindow();

    struct row_snapshot_t {
        std::array<std::uint8_t, 16> bytes{};
        std::array<bool, 16> patched{};
        int count = 0;
    };
    std::vector<row_snapshot_t> rows(static_cast<std::size_t>(last_row - first_row + 1));
    std::int64_t selection_begin = -1;
    std::int64_t selection_end = -1;
    std::int64_t search_match = -1;
    std::uint64_t search_match_len = 0;
    std::uint64_t live_base = 0;
    {
        std::unique_lock<std::mutex> lock(document_->mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            painter.setPen(t.text_dim);
            painter.drawText(QPointF(spacing.md, spacing.xxl),
                QStringLiteral("Hex data is updating..."));
            return;
        }
        selection_begin = document_->ui.sel_start;
        selection_end = document_->ui.sel_end;
        search_match = document_->ui.search_match;
        search_match_len = document_->ui.search_match_len;
        live_base = document_->live_base;
        const bool snapshot_live = document_->source_kind == hex_source_kind_t::live_memory;
        if (snapshot_live == live_source) {
            for (int row = first_row; row <= last_row; ++row) {
                const std::uint64_t row_offset = static_cast<std::uint64_t>(row) * 16;
                if (row_offset >= byte_count)
                    break;
                row_snapshot_t& snap = rows[static_cast<std::size_t>(row - first_row)];
                const std::size_t row_byte_count = static_cast<std::size_t>((std::min)(
                    byte_count - row_offset, static_cast<std::uint64_t>(snap.bytes.size())));
                for (std::size_t column = 0; column < row_byte_count; ++column) {
                    const std::uint64_t offset = row_offset + column;
                    if (live_source) {
                        if (offset >= document_->live_bytes.size())
                            break;
                        snap.bytes[column] =
                            document_->live_bytes[static_cast<std::size_t>(offset)];
                    } else {
                        if (offset < document_->window_offset)
                            break;
                        const auto relative = offset - document_->window_offset;
                        if (relative >= document_->window.size())
                            break;
                        snap.bytes[column] = document_->patchedByte(offset,
                            document_->window[static_cast<std::size_t>(relative)],
                            &snap.patched[column]);
                    }
                    snap.count = static_cast<int>(column) + 1;
                }
            }
        }
    }
    const auto selection_low = (std::min)(selection_begin, selection_end);
    const auto selection_high = (std::max)(selection_begin, selection_end);

    painter.setPen(QPen(with_alpha(t.border_subtle, 1.0), 1));
    const qreal hscroll = horizontalScrollBar()->value();
    painter.drawLine(QPointF(geometry.hex_x - spacing.md - hscroll, 0),
        QPointF(geometry.hex_x - spacing.md - hscroll, viewport()->height()));
    painter.drawLine(QPointF(geometry.ascii_x - spacing.sm - hscroll, 0),
        QPointF(geometry.ascii_x - spacing.sm - hscroll, viewport()->height()));

    for (int row = first_row; row <= last_row; ++row) {
        const row_snapshot_t& snap = rows[static_cast<std::size_t>(row - first_row)];
        if (snap.count == 0)
            continue;
        const std::uint64_t row_offset = static_cast<std::uint64_t>(row) * 16;
        const qreal row_y = (row - verticalScrollBar()->value()) * row_h_;

        std::uint64_t display_address = live_source ? live_base + row_offset : row_offset;
        if (!live_source) {
            if (auto translated = display_address_for_file_offset(context_, row_offset))
                display_address = *translated;
        }
        char address[32]{};
        std::snprintf(address, sizeof(address), "%016llX",
            static_cast<unsigned long long>(display_address));
        painter.setPen(t.text_address);
        painter.drawText(QPointF(geometry.address_x - hscroll, row_y + ascent_),
            QString::fromLatin1(address));

        char ascii[17]{};
        for (int column = 0; column < snap.count; ++column) {
            const std::uint64_t offset = row_offset + static_cast<std::uint64_t>(column);
            const std::uint8_t value = snap.bytes[static_cast<std::size_t>(column)];
            const bool patched = snap.patched[static_cast<std::size_t>(column)];
            const bool selected = selection_begin >= 0 &&
                static_cast<std::int64_t>(offset) >= selection_low &&
                static_cast<std::int64_t>(offset) <= selection_high;
            const bool is_match = search_match >= 0 && search_match_len > 0 &&
                static_cast<std::int64_t>(offset) >= search_match &&
                static_cast<std::int64_t>(offset) < search_match +
                    static_cast<std::int64_t>(search_match_len);
            const qreal cell_x = geometry.hex_x + column * 3.0 * char_w_ - hscroll;
            const qreal ascii_cell_x = geometry.ascii_x + column * char_w_ - hscroll;
            if (selected) {
                const bool next_selected = column + 1 < snap.count &&
                    static_cast<std::int64_t>(offset + 1) >= selection_low &&
                    static_cast<std::int64_t>(offset + 1) <= selection_high;
                painter.fillRect(QRectF(cell_x, row_y,
                    (next_selected ? 3.0 : 2.0) * char_w_, row_h_), t.selection);
                painter.fillRect(QRectF(ascii_cell_x, row_y,
                    (next_selected ? 2.0 : 1.0) * char_w_, row_h_), t.selection);
            } else if (is_match) {
                painter.fillRect(QRectF(cell_x, row_y, 2.0 * char_w_, row_h_),
                    with_alpha(t.info, 0.45));
                painter.fillRect(QRectF(ascii_cell_x, row_y, char_w_, row_h_),
                    with_alpha(t.info, 0.45));
            }
            if (selection_end >= 0 && static_cast<std::int64_t>(offset) == selection_end) {
                painter.setPen(QPen(t.accent, 1));
                painter.drawRect(QRectF(cell_x, row_y, 2.0 * char_w_, row_h_ - 1.0));
                painter.drawRect(QRectF(ascii_cell_x, row_y, char_w_, row_h_ - 1.0));
            }
            painter.setPen(patched ? t.warning : t.text_primary);
            painter.drawText(QPointF(cell_x, row_y + ascent_),
                hex_cells_[value]);
            ascii[column] = value >= 0x20 && value <= 0x7E ? static_cast<char>(value) : '.';
        }
        painter.setPen(t.text_secondary);
        painter.drawText(QPointF(geometry.ascii_x - hscroll, row_y + ascent_),
            QString::fromLatin1(ascii, snap.count));
    }
}

bool AidaHexEditor::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::Resize) {
        if (toolbar_)
            toolbar_->setGeometry(0, 0, width(), toolbar_->height());
        updateScrollbars();
    }
    return QAbstractScrollArea::viewportEvent(event);
}

std::int64_t AidaHexEditor::cellAt(const QPointF& pos) const
{
    if (!document_)
        return -1;
    const hex_row_geometry_t geometry = hexRowGeometry(char_w_);
    const std::int64_t row = static_cast<std::int64_t>(pos.y() / row_h_) +
        verticalScrollBar()->value();
    const qreal rel_x = pos.x() + horizontalScrollBar()->value();
    if (rel_x < geometry.hex_x)
        return -1;
    const std::int64_t column = static_cast<std::int64_t>((rel_x - geometry.hex_x) /
        (3.0 * char_w_));
    if (column < 0 || column >= 16)
        return -1;
    const std::int64_t offset = row * 16 + column;
    return offset;
}

void AidaHexEditor::ensureByteVisible(std::int64_t offset)
{
    const std::int64_t row = offset / 16;
    const int first = verticalScrollBar()->value();
    const int page = std::max(1, verticalScrollBar()->pageStep());
    if (row < first)
        verticalScrollBar()->setValue(static_cast<int>(row));
    else if (row >= first + page)
        verticalScrollBar()->setValue(static_cast<int>(row - page + 1));
}

void AidaHexEditor::moveByteCursor(std::int64_t offset, bool extend)
{
    if (!document_ || !context_)
        return;
    const std::uint64_t byte_count = document_->byteCount(context_);
    if (byte_count == 0)
        return;
    offset = std::clamp<std::int64_t>(offset, 0, static_cast<std::int64_t>(byte_count) - 1);
    {
        std::lock_guard<std::mutex> lock(document_->mutex);
        if (!extend || document_->ui.sel_start < 0)
            document_->ui.sel_start = offset;
        document_->ui.sel_end = offset;
    }
    ensureByteVisible(offset);
    viewport()->update();
}

void AidaHexEditor::keyPressEvent(QKeyEvent* event)
{
    if (!document_ || !context_) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const bool shift = event->modifiers() & Qt::ShiftModifier;

    if (event->key() == Qt::Key_Escape) {
        bool dismissed = false;
        {
            std::lock_guard<std::mutex> lock(document_->mutex);
            if (document_->ui.goto_visible) {
                document_->ui.goto_visible = false;
                dismissed = true;
            } else if (document_->ui.search_visible) {
                document_->ui.search_visible = false;
                dismissed = true;
            }
        }
        if (dismissed) {
            if (goto_overlay_) goto_overlay_->hide();
            if (search_overlay_) search_overlay_->hide();
            event->accept();
            return;
        }
    }
    if (ctrl && event->key() == Qt::Key_C) {
        copySelection();
        event->accept();
        return;
    }

    std::int64_t cursor = -1;
    {
        std::lock_guard<std::mutex> lock(document_->mutex);
        cursor = document_->ui.sel_end;
    }
    if (cursor < 0)
        cursor = 0;
    const std::int64_t page_bytes =
        static_cast<std::int64_t>(std::max(1, verticalScrollBar()->pageStep())) * 16;

    switch (event->key()) {
    case Qt::Key_Left:  moveByteCursor(cursor - 1, shift);  event->accept(); return;
    case Qt::Key_Right: moveByteCursor(cursor + 1, shift);  event->accept(); return;
    case Qt::Key_Up:    moveByteCursor(cursor - 16, shift); event->accept(); return;
    case Qt::Key_Down:  moveByteCursor(cursor + 16, shift); event->accept(); return;
    case Qt::Key_PageUp:   moveByteCursor(cursor - page_bytes, shift); event->accept(); return;
    case Qt::Key_PageDown: moveByteCursor(cursor + page_bytes, shift); event->accept(); return;
    case Qt::Key_Home:
        moveByteCursor(ctrl ? 0 : cursor - (cursor % 16), shift);
        event->accept();
        return;
    case Qt::Key_End:
        if (ctrl) {
            moveByteCursor((std::numeric_limits<std::int64_t>::max)(), shift);
        } else {
            moveByteCursor(cursor - (cursor % 16) + 15, shift);
        }
        event->accept();
        return;
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

bool AidaHexEditor::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == goto_edit_ || watched == search_edit_) &&
        event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            if (document_) {
                std::lock_guard<std::mutex> lock(document_->mutex);
                document_->ui.goto_visible = false;
                document_->ui.search_visible = false;
            }
            if (goto_overlay_) goto_overlay_->hide();
            if (search_overlay_) search_overlay_->hide();
            setFocus(Qt::OtherFocusReason);
            return true;
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

void AidaHexEditor::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ShiftModifier) {
        QCoreApplication::sendEvent(horizontalScrollBar(), event);
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void AidaHexEditor::mousePressEvent(QMouseEvent* event)
{
    if (!document_ || !context_) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const std::int64_t offset = cellAt(event->position());
        const std::int64_t byte_count =
            static_cast<std::int64_t>(document_->byteCount(context_));
        if (offset < 0 || offset >= byte_count) {
            QAbstractScrollArea::mousePressEvent(event);
            return;
        }
        std::lock_guard<std::mutex> lock(document_->mutex);
        if (event->modifiers() & Qt::ShiftModifier) {
            if (document_->ui.sel_start >= 0)
                document_->ui.sel_end = offset;
        } else {
            document_->ui.sel_start = document_->ui.sel_end = offset;
        }
        selecting_ = true;
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void AidaHexEditor::mouseMoveEvent(QMouseEvent* event)
{
    if (selecting_ && document_ && (event->buttons() & Qt::LeftButton)) {
        const std::int64_t offset = cellAt(event->position());
        if (offset >= 0) {
            const std::int64_t clamped = std::clamp<std::int64_t>(offset, 0,
                (std::max)(static_cast<std::int64_t>(document_->byteCount(context_)) - 1,
                    std::int64_t{0}));
            std::lock_guard<std::mutex> lock(document_->mutex);
            document_->ui.sel_end = clamped;
            viewport()->update();
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void AidaHexEditor::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        selecting_ = false;
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void AidaHexEditor::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (document_ && context_ && event->button() == Qt::LeftButton) {
        const std::int64_t offset = cellAt(event->position());
        if (offset >= 0) {
            openDisassemblyAt(static_cast<std::uint64_t>(offset));
            event->accept();
            return;
        }
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void AidaHexEditor::openDisassemblyAt(std::uint64_t offset)
{
    if (!context_)
        return;
    if (auto translated = normalized_address_for_file_offset(context_, offset)) {
        disasm_view::goto_address(*translated, context_);
        Q_EMIT openDisassemblyRequested();
    }
}

void AidaHexEditor::copyByte(std::uint8_t value)
{
    char encoded[4]{};
    std::snprintf(encoded, sizeof(encoded), "%02X", value);
    aida::qt::clipboard::set_text(QString::fromLatin1(encoded));
}

void AidaHexEditor::copySelection()
{
    if (!document_ || !context_)
        return;
    std::int64_t begin = -1;
    std::int64_t end = -1;
    std::vector<std::uint8_t> live_bytes;
    {
        std::lock_guard<std::mutex> lock(document_->mutex);
        begin = document_->ui.sel_start;
        end = document_->ui.sel_end;
        if (document_->source_kind == hex_source_kind_t::live_memory)
            live_bytes = document_->live_bytes;
    }
    if (begin < 0 || end < 0)
        return;
    if (begin > end)
        std::swap(begin, end);
    const std::uint64_t count = static_cast<std::uint64_t>(end - begin + 1);
    if (count > (1ULL << 20))
        return;
    std::vector<std::uint8_t> selected;
    if (!live_bytes.empty()) {
        if (static_cast<std::uint64_t>(end) >= live_bytes.size())
            return;
        using byte_difference_t = std::vector<std::uint8_t>::difference_type;
        if (end >= static_cast<std::int64_t>(std::numeric_limits<byte_difference_t>::max()))
            return;
        const auto first = static_cast<byte_difference_t>(begin);
        const auto last = static_cast<byte_difference_t>(end + 1);
        selected.assign(live_bytes.begin() + first, live_bytes.begin() + last);
    } else {
        auto bytes = context_.workspace->provider().read_vector(static_cast<std::uint64_t>(begin),
            count, 1ULL << 20, context_.workspace->cancellation_token());
        if (!bytes)
            return;
        selected = bytes.take_value();
    }
    std::string text;
    text.reserve(selected.size() * 3);
    char encoded[4]{};
    for (std::uint8_t byte : selected) {
        if (!text.empty())
            text.push_back(' ');
        std::snprintf(encoded, sizeof(encoded), "%02X", byte);
        text.append(encoded);
    }
    aida::qt::clipboard::set_text(QString::fromStdString(text));
}

void AidaHexEditor::copyAddress(std::uint64_t address)
{
    char text[24]{};
    std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(address));
    aida::qt::clipboard::set_text(QString::fromLatin1(text));
}

void AidaHexEditor::contextMenuEvent(QContextMenuEvent* event)
{
    if (!document_ || !context_)
        return;
    if (event->reason() == QContextMenuEvent::Mouse) {
        const std::int64_t offset = cellAt(event->pos());
        if (offset >= 0) {
            std::lock_guard<std::mutex> lock(document_->mutex);
            document_->ui.sel_start = document_->ui.sel_end = offset;
            document_->ui.context_offset = offset;
            document_->ui.context_generation = context_.publication
                ? context_.publication->generation : 0;
            document_->ui.context_overlay_revision = context_.workspace->overlay_revision();
            std::uint8_t value = 0;
            bool live_source = false;
            std::uint64_t live_base = 0;
            {
                live_source = document_->source_kind == hex_source_kind_t::live_memory;
                live_base = document_->live_base;
                if (live_source) {
                    if (static_cast<std::uint64_t>(offset) < document_->live_bytes.size())
                        value = document_->live_bytes[static_cast<std::size_t>(offset)];
                } else if (document_->ensureWindow(context_,
                        static_cast<std::uint64_t>(offset),
                        static_cast<std::uint64_t>(offset) + 1)) {
                    const auto relative = static_cast<std::uint64_t>(offset) -
                        document_->window_offset;
                    if (relative < document_->window.size())
                        value = document_->window[static_cast<std::size_t>(relative)];
                }
            }
            document_->ui.context_value = value;
            document_->ui.context_live = live_source;
            document_->ui.context_valid = true;
        }
    }
    openContextMenu(event->globalPos(), event->reason() == QContextMenuEvent::Keyboard);
}

void AidaHexEditor::openContextMenu(const QPoint& global_pos, bool keyboard)
{
    if (!document_ || !context_)
        return;
    const auto context = context_;
    auto state = document_;
    hex_ui_state_t snapshot;
    std::uint64_t byte_count = 0;
    std::uint64_t live_base = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        snapshot.context_offset = state->ui.context_offset;
        snapshot.context_generation = state->ui.context_generation;
        snapshot.context_overlay_revision = state->ui.context_overlay_revision;
        snapshot.context_value = state->ui.context_value;
        snapshot.context_live = state->ui.context_live;
        snapshot.context_valid = state->ui.context_valid;
        snapshot.sel_start = state->ui.sel_start;
        snapshot.sel_end = state->ui.sel_end;
        byte_count = snapshot.context_live
            ? static_cast<std::uint64_t>(state->live_bytes.size())
            : context.workspace->provider().size();
        live_base = state->live_base;
    }
    const bool generation_current = snapshot.context_valid && context.publication &&
        snapshot.context_generation == context.publication->generation;
    const bool overlay_current = snapshot.context_live ||
        snapshot.context_overlay_revision == context.workspace->overlay_revision();
    const bool current = generation_current && overlay_current &&
        snapshot.context_offset < byte_count;
    const std::uint64_t selection_size = snapshot.sel_start >= 0 && snapshot.sel_end >= 0
        ? static_cast<std::uint64_t>((std::max)(snapshot.sel_start, snapshot.sel_end) -
            (std::min)(snapshot.sel_start, snapshot.sel_end)) + 1U : 0U;
    const std::uint64_t display_address = snapshot.context_live
        ? live_base + snapshot.context_offset
        : display_address_for_file_offset(context, snapshot.context_offset)
            .value_or(snapshot.context_offset);
    const auto typed = snapshot.context_live
        ? disasm_view::typed_address(context, display_address)
        : normalized_address_for_file_offset(context, snapshot.context_offset);
    const std::uint64_t selection_begin = snapshot.sel_start >= 0 && snapshot.sel_end >= 0
        ? static_cast<std::uint64_t>((std::min)(snapshot.sel_start, snapshot.sel_end))
        : snapshot.context_offset;
    const auto patch_address = snapshot.context_live
        ? std::optional<aida::analysis::address_t>{}
        : normalized_address_for_file_offset(context, selection_begin);
    const bool can_stage_overlay = current && !snapshot.context_live && typed.has_value();
    const bool can_stage_selection = current && !snapshot.context_live &&
        patch_address.has_value() && selection_size != 0 && selection_size <= 64U * 1024U;
    const auto context_pid = snapshot.context_live ? driver_bridge::attached_pid() : 0UL;

    aida::ui::application_ui::retained_entity_context_t retained;
    retained.owner_id = "hex.byte";
    retained.entity_id = std::to_string(snapshot.context_offset);
    retained.entity_generation = snapshot.context_generation;
    retained.active_view = aida::ui::stable_view_id_t("document.hex");
    retained.validate_identity = [state, context, snapshot, byte_count, context_pid]() {
        if (!context.publication || context.publication->generation != snapshot.context_generation)
            return aida::ui::capability_state_t::unavailable("The analysis publication changed; reopen the menu.");
        if (snapshot.context_live && driver_bridge::attached_pid() != context_pid)
            return aida::ui::capability_state_t::unavailable("The live Hex target process changed; reopen the menu.");
        if (!snapshot.context_live && context.workspace->overlay_revision() != snapshot.context_overlay_revision)
            return aida::ui::capability_state_t::unavailable("The overlay revision changed; reopen the menu.");
        if (snapshot.context_offset >= byte_count)
            return aida::ui::capability_state_t::unavailable("The selected byte is outside the current source.");
        std::lock_guard<std::mutex> lock(state->mutex);
        const bool same = state->ui.context_valid && state->ui.context_offset == snapshot.context_offset &&
            state->ui.context_generation == snapshot.context_generation &&
            state->ui.sel_start == snapshot.sel_start && state->ui.sel_end == snapshot.sel_end;
        return same ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable("The byte selection changed; reopen the menu.");
    };
    auto add = [&](const char* id, bool enabled, const char* reason, auto invoke) {
        retained.actions.push_back({id, enabled ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(reason), invoke});
    };
    add("hex.copy_byte", current, "The byte selection is stale; select it again.", [this, snapshot]() {
        copyByte(snapshot.context_value);
        return aida::ui::action_handler_result_t::completed();
    });
    add("hex.copy_selection", current && selection_size != 0 && selection_size <= (1ULL << 20),
        !current ? "The byte selection is stale; select it again."
            : selection_size > (1ULL << 20) ? "Clipboard selection is limited to 1 MiB."
            : "No current byte range is selected.", [this]() {
            copySelection();
            return aida::ui::action_handler_result_t::completed();
        });
    add("hex.copy_address", current, "The byte selection is stale.", [this, display_address]() {
        copyAddress(display_address);
        return aida::ui::action_handler_result_t::completed();
    });
    add("hex.open_disassembly", current && typed.has_value(),
        !current ? "The byte selection is stale; select it again."
            : "The selected byte has no current mapped analysis address.", [typed, context]() {
            disasm_view::goto_address(*typed, context);
            return aida::ui::action_handler_result_t::completed();
        });
    add("hex.stage_zero_overlay", can_stage_overlay,
        !current ? "The byte selection is stale; select it again."
            : snapshot.context_live ? "Live memory writes require the reviewed Patches view."
            : "The selected byte has no current mapped analysis address.", [typed, context]() {
            if (!typed)
                return aida::ui::action_handler_result_t::failed(
                    "The retained byte no longer has a mapped workspace address.");
            auto original = disasm_view::read_bytes(context, *typed, 1);
            if (!original)
                return aida::ui::action_handler_result_t::failed(
                    original.error().stable_code() + ": " + original.error().message);
            std::string error;
            if (!disasm_view::open_exact_static_patch_review(context, *typed,
                    original.value(), std::vector<std::uint8_t>{0x00U},
                    "Hex editor zero overlay",
                    context.workspace->generation(),
                    context.workspace->analysis_revision(),
                    context.workspace->overlay_revision(), &error))
                return aida::ui::action_handler_result_t::failed(error);
            return aida::ui::action_handler_result_t::completed();
        });
    add("hex.stage_patch_overlay", can_stage_selection,
        !current ? "The byte selection is stale; select it again."
            : snapshot.context_live ? "Live memory writes require the reviewed Debugger Patches view."
            : selection_size > 64U * 1024U ? "Interactive overlay review is limited to 64 KiB."
            : "Select a fully mapped provider-backed byte range.",
        [patch_address, selection_size, context]() {
            if (!patch_address)
                return aida::ui::action_handler_result_t::failed(
                    "The retained selection no longer has a mapped workspace address.");
            std::string error;
            return disasm_view::open_static_patch_review(context, *patch_address,
                selection_size, disasm_view::static_patch_mode_t::bytes, &error)
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(error);
        });
    add("hex.stage_nop_overlay", can_stage_selection,
        !current ? "The byte selection is stale; select it again."
            : snapshot.context_live ? "Live memory writes require the reviewed Debugger Patches view."
            : selection_size > 64U * 1024U ? "Interactive overlay review is limited to 64 KiB."
            : "Select a fully mapped provider-backed byte range.",
        [patch_address, selection_size, context]() {
            if (!patch_address)
                return aida::ui::action_handler_result_t::failed(
                    "The retained selection no longer has a mapped workspace address.");
            std::string error;
            return disasm_view::open_static_patch_review(context, *patch_address,
                selection_size, disasm_view::static_patch_mode_t::nop_fill, &error)
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(error);
        });
    char display_address_text[24]{};
    char byte_text[4]{};
    std::snprintf(display_address_text, sizeof(display_address_text), "0x%016llX",
        static_cast<unsigned long long>(display_address));
    std::snprintf(byte_text, sizeof(byte_text), "%02X", snapshot.context_value);
    aida::automation_ui::entity_evidence::snapshot_t evidence;
    evidence.workspace_id = snapshot.context_live
        ? "pid:" + std::to_string(context_pid)
        : context.workspace->identity().binary_id().to_hex();
    evidence.source_view_id = "document.hex";
    evidence.source_kind = snapshot.context_live ? "live_hex_byte" : "static_hex_byte";
    evidence.entity_id = retained.entity_id;
    evidence.display_label = std::string("Hex byte ") + display_address_text;
    evidence.excerpt = "Source: " + std::string(snapshot.context_live
        ? "live process" : "static analysis") +
        "\nAddress: " + display_address_text +
        "\nFile/source offset: " + std::to_string(snapshot.context_offset) +
        "\nValue: " + byte_text +
        "\nSelection bytes: " + std::to_string(selection_size) +
        "\nOverlay revision: " + std::to_string(snapshot.context_overlay_revision) +
        "\nPublication generation: " + std::to_string(snapshot.context_generation);
    evidence.address = display_address;
    evidence.revision = snapshot.context_overlay_revision;
    evidence.generation = snapshot.context_generation;
    evidence.sensitive = snapshot.context_live;
    evidence.return_to_source = [state, context, snapshot, byte_count,
        context_pid](std::string& reason) {
        if (!context.publication || context.publication->generation != snapshot.context_generation ||
            (!snapshot.context_live &&
                context.workspace->overlay_revision() != snapshot.context_overlay_revision) ||
            (snapshot.context_live && driver_bridge::attached_pid() != context_pid) ||
            snapshot.context_offset >= byte_count) {
            reason = "The Hex source, publication, or overlay changed; capture the byte again.";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->ui.sel_start = snapshot.sel_start;
            state->ui.sel_end = snapshot.sel_end;
            state->ui.context_offset = snapshot.context_offset;
            state->ui.context_generation = snapshot.context_generation;
            state->ui.context_overlay_revision = snapshot.context_overlay_revision;
            state->ui.context_value = snapshot.context_value;
            state->ui.context_live = snapshot.context_live;
            state->ui.context_valid = true;
        }
        reason.clear();
        return true;
    };
    aida::automation_ui::entity_evidence::append_actions(retained,
        std::move(evidence), current
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The retained Hex byte or its publication changed; select it again."));
    documents::show_retained_entity_menu(retained,
        keyboard ? aida::ui::context_menu_open_origin_t::menu_key
                 : aida::ui::context_menu_open_origin_t::pointer,
        global_pos, this);
}

void AidaHexEditor::updateOverlayGeometry()
{
    const auto& spacing = theme::tokens().spacing;
    const int top = toolbar_ ? toolbar_->height() + spacing.xxs : spacing.xxs;
    if (goto_overlay_) {
        const int w = (std::min)(360, (std::max)(120, width() - spacing.sm * 2));
        goto_overlay_->setGeometry(spacing.sm, top, w, goto_overlay_->sizeHint().height());
    }
    if (search_overlay_) {
        const int w = (std::min)(560, (std::max)(120, width() - spacing.sm * 2));
        search_overlay_->setGeometry(spacing.sm, top, w, search_overlay_->sizeHint().height());
    }
}

void AidaHexEditor::showGotoOverlay()
{
    if (!document_)
        return;
    if (!goto_overlay_) {
        goto_overlay_ = new QFrame(this);
        goto_overlay_->setObjectName(QStringLiteral("aida.hex.goto_overlay"));
        goto_overlay_->setProperty("aidaRole", QStringLiteral("panel"));
        goto_overlay_->setFrameShape(QFrame::StyledPanel);
        const auto& spacing = theme::tokens().spacing;
        auto* layout = new QHBoxLayout(goto_overlay_);
        layout->setContentsMargins(spacing.sm, spacing.xs, spacing.sm, spacing.xs);
        layout->setSpacing(spacing.xs);
        goto_edit_ = new widgets::AidaLineEdit(
            QStringLiteral("File offset or virtual address"), goto_overlay_);
        goto_edit_->setMinimumWidth(goto_edit_->fontMetrics().horizontalAdvance(
            QStringLiteral("0xDDDDDDDDDDDDDDDD")) +
            2 * theme::tokens().table.cell_pad_x + spacing.lg);
        goto_edit_->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("(0[xX])?[0-9a-fA-F]*")), goto_edit_));
        layout->addWidget(goto_edit_, 1);
        auto* go = new widgets::AidaButton(QStringLiteral("Go"), goto_overlay_);
        go->setKind(widgets::AidaButton::Kind::Primary);
        go->setControlSize(widgets::AidaButton::ControlSize::Small);
        layout->addWidget(go);
        auto* close = new widgets::AidaButton(QStringLiteral("Close"), goto_overlay_);
        close->setKind(widgets::AidaButton::Kind::Ghost);
        close->setControlSize(widgets::AidaButton::ControlSize::Small);
        layout->addWidget(close);
        connect(go, &QAbstractButton::clicked, this, &AidaHexEditor::submitGoto);
        connect(goto_edit_, &QLineEdit::returnPressed, this, &AidaHexEditor::submitGoto);
        connect(close, &QAbstractButton::clicked, this, [this] {
            std::lock_guard<std::mutex> lock(document_->mutex);
            document_->ui.goto_visible = false;
            goto_overlay_->hide();
        });
        goto_edit_->installEventFilter(this);
    }
    {
        std::lock_guard<std::mutex> lock(document_->mutex);
        document_->ui.goto_visible = true;
        document_->ui.goto_buf[0] = '\0';
    }
    goto_edit_->clear();
    updateOverlayGeometry();
    goto_overlay_->show();
    goto_edit_->setFocus(Qt::ShortcutFocusReason);
}

void AidaHexEditor::submitGoto()
{
    if (!document_ || !context_)
        return;
    const std::string text = goto_edit_->text().toStdString();
    if (auto value = hex_parse_u64(text)) {
        std::uint64_t offset = *value;
        bool live_source = false;
        std::uint64_t live_base = 0;
        std::uint64_t source_size = context_.workspace->provider().size();
        {
            std::lock_guard<std::mutex> lock(document_->mutex);
            live_source = document_->source_kind == hex_source_kind_t::live_memory;
            live_base = document_->live_base;
            if (live_source)
                source_size = static_cast<std::uint64_t>(document_->live_bytes.size());
        }
        if (live_source && *value >= live_base)
            offset = *value - live_base;
        else if (!live_source) {
            if (auto normalized_offset = file_offset_for_normalized_address(context_, *value))
                offset = *normalized_offset;
            else if (auto typed = disasm_view::typed_address(context_, *value))
                offset = disasm_view::provider_offset(context_, *typed).value_or(offset);
        }
        if (offset < source_size) {
            std::lock_guard<std::mutex> lock(document_->mutex);
            document_->scroll_to_offset = offset;
        }
        applyScrollToOffset();
        viewport()->update();
    }
    {
        std::lock_guard<std::mutex> lock(document_->mutex);
        document_->ui.goto_visible = false;
    }
    goto_overlay_->hide();
    setFocus(Qt::OtherFocusReason);
}

void AidaHexEditor::showSearchOverlay()
{
    if (!document_)
        return;
    if (!search_overlay_) {
        search_overlay_ = new QFrame(this);
        search_overlay_->setObjectName(QStringLiteral("aida.hex.search_overlay"));
        search_overlay_->setProperty("aidaRole", QStringLiteral("panel"));
        search_overlay_->setFrameShape(QFrame::StyledPanel);
        const auto& spacing = theme::tokens().spacing;
        auto* layout = new QHBoxLayout(search_overlay_);
        layout->setContentsMargins(spacing.sm, spacing.xs, spacing.sm, spacing.xs);
        layout->setSpacing(spacing.xs);
        search_edit_ = new widgets::AidaLineEdit(QStringLiteral("Bytes or text"), search_overlay_);
        search_edit_->setMinimumWidth(search_edit_->fontMetrics().horizontalAdvance(
            QStringLiteral("00 11 22 33 44 55 66 77")) +
            2 * theme::tokens().table.cell_pad_x + spacing.lg);
        layout->addWidget(search_edit_, 1);
        auto* hex_toggle = new widgets::AidaButton(QStringLiteral("Hex"), search_overlay_);
        hex_toggle->setCheckable(true);
        hex_toggle->setChecked(true);
        hex_toggle->setToolTip(QStringLiteral("Interpret the input as hex bytes (off = plain text)"));
        layout->addWidget(hex_toggle);
        auto* find = new widgets::AidaButton(QStringLiteral("Find"), search_overlay_);
        find->setControlSize(widgets::AidaButton::ControlSize::Small);
        layout->addWidget(find);
        auto* prev = new widgets::AidaButton(QStringLiteral("Previous"), search_overlay_);
        prev->setControlSize(widgets::AidaButton::ControlSize::Small);
        layout->addWidget(prev);
        auto* next = new widgets::AidaButton(QStringLiteral("Next"), search_overlay_);
        next->setControlSize(widgets::AidaButton::ControlSize::Small);
        layout->addWidget(next);
        auto* close = new widgets::AidaButton(QStringLiteral("Close"), search_overlay_);
        close->setKind(widgets::AidaButton::Kind::Ghost);
        close->setControlSize(widgets::AidaButton::ControlSize::Small);
        layout->addWidget(close);
        connect(hex_toggle, &QAbstractButton::toggled, this, [this](bool checked) {
            std::lock_guard<std::mutex> lock(document_->mutex);
            document_->ui.search_hex = checked;
        });
        connect(find, &QAbstractButton::clicked, this, &AidaHexEditor::submitSearch);
        connect(search_edit_, &QLineEdit::returnPressed, this, &AidaHexEditor::submitSearch);
        connect(prev, &QAbstractButton::clicked, this, [this] {
            if (document_) document_->stepSearchResult(-1);
        });
        connect(next, &QAbstractButton::clicked, this, [this] {
            if (document_) document_->stepSearchResult(1);
        });
        connect(close, &QAbstractButton::clicked, this, [this] {
            std::lock_guard<std::mutex> lock(document_->mutex);
            document_->ui.search_visible = false;
            search_overlay_->hide();
        });
        search_edit_->installEventFilter(this);
    }
    {
        std::lock_guard<std::mutex> lock(document_->mutex);
        document_->ui.search_visible = !document_->ui.search_visible;
        if (!document_->ui.search_visible) {
            search_overlay_->hide();
            return;
        }
    }
    const QByteArray current = [&] {
        std::lock_guard<std::mutex> lock(document_->mutex);
        return QByteArray(document_->ui.search_buf);
    }();
    search_edit_->setText(QString::fromUtf8(current));
    updateOverlayGeometry();
    search_overlay_->show();
    search_edit_->setFocus(Qt::ShortcutFocusReason);
}

void AidaHexEditor::submitSearch()
{
    if (!document_ || !context_)
        return;
    {
        std::lock_guard<std::mutex> lock(document_->mutex);
        const QByteArray utf8 = search_edit_->text().toUtf8();
        std::snprintf(document_->ui.search_buf, sizeof(document_->ui.search_buf), "%s",
            utf8.constData());
    }
    document_->startSearch(context_);
    viewport()->update();
}

}
