#include "qt/disasm/aida_disasm_view.hpp"

#include "qt/disasm/dialogs/disasm_comment_dialog.hpp"
#include "qt/disasm/dialogs/disasm_rebase_dialog.hpp"
#include "qt/disasm/dialogs/disasm_rename_dialog.hpp"
#include "qt/disasm/dialogs/disasm_xref_popup.hpp"
#include "qt/disasm/dialogs/static_patch_review_dialog.hpp"
#include "qt/disasm/disasm_row_cache.hpp"
#include "qt/disasm/disasm_toolbar.hpp"
#include "qt/analysis/qt_analysis_host_hooks.hpp"
#include "qt/analysis_bridge/disasm_workspace_model.hpp"
#include "qt/analysis_bridge/gui_post.hpp"
#include "qt/analysis_bridge/revision_combine.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_theme_controller.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

#include "core/ai/standalone_chat.hpp"
#include "core/analysis/auto_comment_store.hpp"
#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/debugger/debugger_interaction_context.hpp"
#include "core/debugger/debugger_view.hpp"
#include "core/disasm/file_metadata_banner.hpp"
#include "core/disasm/pseudocode_view.hpp"
#include "core/editor/hex_view.hpp"
#include "core/scanner/aob_generator.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "helpers/globals.h"

#include <QAbstractButton>
#include <QApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

#include <QMouseEvent>

namespace aida::qt::disasm {

namespace {

constexpr qreal k_bytes_columns = 31.0;
constexpr qreal k_min_bytes_viewport_columns = 88.0;

std::uint64_t context_generation(const disasm_view::workspace_context_t& context)
{
    if (!context.workspace)
        return 0;
    return aida::analysis_bridge::combine_generation_revision(
        context.workspace->generation(), context.workspace->analysis_revision());
}

}

AidaDisasmNavBand::AidaDisasmNavBand(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.disasm.nav_band"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFixedHeight(theme::tokens().spacing.sm);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral(
        "Listing map: one tick per instruction (calls, jumps, returns highlighted). "
        "Click to navigate; the accent marker tracks the current address."));
    setAccessibleName(QStringLiteral("Disassembly overview band"));
}

void AidaDisasmNavBand::set_model(
    const std::shared_ptr<const aida::analysis::analysis_snapshot_t>& snapshot,
    std::size_t range_first, std::size_t range_second, DisasmRowCache* rows)
{
    snapshot_ = snapshot;
    range_first_ = range_first;
    range_second_ = range_second;
    rows_ = rows;
    update();
}

void AidaDisasmNavBand::set_selection(
    const std::optional<aida::analysis::address_t>& selection)
{
    selection_ = selection;
    update();
}

void AidaDisasmNavBand::invalidate_colors()
{
    colors_instructions_ = nullptr;
    update();
}

void AidaDisasmNavBand::set_theme_revision(quint64 revision)
{
    colors_theme_revision_ = revision;
    update();
}

void AidaDisasmNavBand::rebuild_colors()
{
    colors_.clear();
    if (!snapshot_ || range_second_ <= range_first_)
        return;
    const auto& instructions = snapshot_->instructions;
    const std::size_t navigation_count = range_second_ - range_first_;
    const std::size_t marker_budget = (std::max)(std::size_t{1},
        static_cast<std::size_t>(width()));
    const std::size_t marker_step = (std::max)(std::size_t{1},
        (navigation_count + marker_budget - 1) / marker_budget);
    const auto& theme = theme::disasm_theme_snapshot();
    std::unordered_map<std::size_t, std::string> marker_texts;
    if (rows_) {
        for (std::size_t offset = 0; offset < navigation_count; offset += marker_step) {
            const auto& marker = instructions[range_first_ + offset];
            if ((marker.flow_flags & (aida::analysis::flow_return |
                    aida::analysis::flow_call | aida::analysis::flow_branch)) != 0)
                continue;
            const auto formatted = rows_->find(marker.id);
            if (formatted)
                marker_texts.emplace(offset, formatted->text);
        }
    }
    colors_.reserve((navigation_count + marker_step - 1) / marker_step);
    for (std::size_t offset = 0; offset < navigation_count; offset += marker_step) {
        const auto& marker = instructions[range_first_ + offset];
        QColor color = theme::tokens().text_dim;
        if ((marker.flow_flags & aida::analysis::flow_return) != 0)
            color = theme.mnem_ret;
        else if ((marker.flow_flags & aida::analysis::flow_call) != 0)
            color = theme.mnem_call;
        else if ((marker.flow_flags & aida::analysis::flow_branch) != 0)
            color = theme.mnem_branch;
        else {
            const auto text = marker_texts.find(offset);
            if (text != marker_texts.end()) {
                const auto& value = text->second;
                if (value.size() >= 3 &&
                    (value[0] == 'n' || value[0] == 'N') &&
                    (value[1] == 'o' || value[1] == 'O') &&
                    (value[2] == 'p' || value[2] == 'P'))
                    color = theme.mnem_nop;
            }
        }
        colors_.push_back(color);
    }
    colors_instructions_ = static_cast<const void*>(&snapshot_->instructions);
    colors_range_first_ = range_first_;
    colors_range_second_ = range_second_;
    colors_marker_step_ = marker_step;
    colors_revision_ = rows_ ? rows_->revision() : 0;
    colors_theme_revision_ = theme::disasm_theme_revision();
}

void AidaDisasmNavBand::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    const auto& theme = theme::disasm_theme_snapshot();
    painter.fillRect(event->rect(), theme::tokens().bg_elevated);
    if (!snapshot_ || range_second_ <= range_first_)
        return;
    const auto& instructions = snapshot_->instructions;
    const std::size_t navigation_count = range_second_ - range_first_;
    const qreal band_width = (std::max)(1.0, static_cast<qreal>(width()));
    const std::size_t marker_step = (std::max)(std::size_t{1},
        (navigation_count + static_cast<std::size_t>(band_width) - 1) /
            static_cast<std::size_t>(band_width));
    if (colors_instructions_ != static_cast<const void*>(&instructions) ||
        colors_range_first_ != range_first_ || colors_range_second_ != range_second_ ||
        colors_marker_step_ != marker_step ||
        colors_revision_ != (rows_ ? rows_->revision() : 0) ||
        colors_theme_revision_ != theme::disasm_theme_revision())
        rebuild_colors();
    const qreal dpr = (std::max)(1.0, devicePixelRatioF());
    const auto snap = [dpr](qreal value) {
        return std::floor(value * dpr + 0.5) / dpr;
    };
    const qreal marker_w = 1.0 / dpr;
    const qreal body_h = static_cast<qreal>(height()) - 2.0;
    std::size_t color_index = 0;
    for (std::size_t offset = 0;
         offset < navigation_count && color_index < colors_.size();
         offset += colors_marker_step_, ++color_index) {
        const qreal x = (static_cast<qreal>(offset) /
            static_cast<qreal>(navigation_count)) * band_width;
        const qreal snapped = snap(x);
        painter.fillRect(QRectF(snapped, 1.0,
            (std::max)(marker_w, snap((std::min)(x + 1.0, band_width)) - snapped),
            body_h), theme::disasm_with_alpha(colors_[color_index], 0.9));
    }
    if (selection_) {
        const auto selected = std::lower_bound(
            instructions.begin() + static_cast<std::ptrdiff_t>(range_first_),
            instructions.begin() + static_cast<std::ptrdiff_t>(range_second_),
            *selection_,
            [](const aida::analysis::instruction_record_t& item,
               const aida::analysis::address_t& address) { return item.address < address; });
        if (selected != instructions.begin() +
                static_cast<std::ptrdiff_t>(range_second_) &&
            selected->address == *selection_) {
            const auto selected_offset = static_cast<std::size_t>(std::distance(
                instructions.begin() + static_cast<std::ptrdiff_t>(range_first_),
                selected));
            const qreal x = (static_cast<qreal>(selected_offset) /
                static_cast<qreal>(navigation_count)) * band_width;
            const qreal half = static_cast<qreal>(theme::tokens().spacing.xxs) * 0.5;
            painter.fillRect(QRectF(snap(x) - half, 0.0, half * 2.0 + marker_w,
                static_cast<qreal>(height())), theme::tokens().accent);
        }
    }
}

void AidaDisasmNavBand::mousePressEvent(QMouseEvent* event)
{
    if (!snapshot_ || range_second_ <= range_first_ ||
        event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const std::size_t navigation_count = range_second_ - range_first_;
    const qreal normalized = (std::clamp)(
        event->position().x() / (std::max)(1.0, static_cast<qreal>(width())),
        0.0, 0.999999);
    const auto offset = (std::min)(navigation_count - 1,
        static_cast<std::size_t>(normalized * static_cast<qreal>(navigation_count)));
    Q_EMIT navigateToIndex(range_first_ + offset);
}

AidaDisasmView::AidaDisasmView(QWidget* parent)
    : AidaDisasmView(std::string(), parent)
{
}

AidaDisasmView::AidaDisasmView(std::string presentation_key, QWidget* parent)
    : QAbstractScrollArea(parent), presentation_key_(std::move(presentation_key))
{
    setObjectName(presentation_key_.empty() || presentation_key_ == "primary"
        ? QStringLiteral("aida.document.disassembly")
        : QStringLiteral("aida.document.disassembly.") +
            QString::fromStdString(presentation_key_));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_StaticContents);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    bridge::InteractionContextProvider::attach_scope(this,
        QStringLiteral("scope.document.disassembly"),
        aida::ui::focus_scope_kind_t::document);

    theme_ = theme::disasm_theme_snapshot();
    theme_revision_ = theme::disasm_theme_revision();
    recomputeTextMetrics();
    setAccessibleName(QStringLiteral("Disassembly listing"));

    toolbar_ = new DisasmToolbar(this);
    goto_strip_ = new DisasmGotoStrip(this);
    notices_ = new QWidget(this);
    notices_->setObjectName(QStringLiteral("aida.disasm.notices"));
    notices_->setLayout(new QVBoxLayout(notices_));
    notices_->layout()->setContentsMargins(0, 0, 0, 0);
    notices_->layout()->setSpacing(theme::tokens().spacing.xxs);
    nav_band_ = new AidaDisasmNavBand(this);
    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.disasm.state_view"));
    state_view_->hide();

    row_cache_ = new DisasmRowCache(this);
    row_cache_->set_fence([this](quint64 generation, quint64 analysis_revision,
                                 quint64 overlay_revision) {
        return context_.publication &&
            context_.publication->generation == generation &&
            context_.publication->analysis_revision == analysis_revision &&
            context_.publication->overlay_revision == overlay_revision;
    });
    poller_ = new analysis_bridge::AidaRevisionPoller(this);
    poller_->set_source([this] { return sampleRevisions(); });

    connect(toolbar_, &DisasmToolbar::actionInvoked, this, [this](const QString& id) {
        static_cast<void>(aida::ui::application_ui::execute_action(id.toUtf8().constData(),
            aida::ui::action_invocation_source_t::toolbar));
    });
    connect(toolbar_, &DisasmToolbar::showBytesChanged, this, [this](bool show) {
        if (!context_.view)
            return;
        {
            std::lock_guard<std::mutex> lock(context_.view->mutex);
            context_.view->show_bytes = show;
        }
        context_.view->ui_serial.fetch_add(1, std::memory_order_acq_rel);
        prefix_cache_.valid = false;
        content_width_max_ = 0.0;
        viewport()->update();
    });
    connect(toolbar_, &DisasmToolbar::addrFormatChanged, this, [this](int format) {
        if (!context_.view || format < 0 || format > 2)
            return;
        {
            std::lock_guard<std::mutex> lock(context_.view->mutex);
            context_.view->addr_format = static_cast<disasm_view::addr_format_t>(format);
        }
        context_.view->ui_serial.fetch_add(1, std::memory_order_acq_rel);
        prefix_cache_.valid = false;
        content_width_max_ = 0.0;
        viewport()->update();
    });
    connect(toolbar_, &DisasmToolbar::sectionChanged, this, [this](int section) {
        if (!context_.view)
            return;
        {
            std::lock_guard<std::mutex> lock(context_.view->mutex);
            context_.view->active_section = section;
        }
        analysis_bridge::update_section_filter_mirror(context_, section);
        context_.view->ui_serial.fetch_add(1, std::memory_order_acq_rel);
        prefix_cache_.valid = false;
        content_width_max_ = 0.0;
        updateScrollbars();
        viewport()->update();
    });
    connect(goto_strip_, &DisasmGotoStrip::submitted, this, [this](const QString& text) {
        if (auto value = disasm_view::parse_address_text(context_, text.toStdString()))
            disasm_view::goto_address(*value, context_);
        goto_strip_->hide_strip();
        viewport()->setFocus();
    });
    connect(goto_strip_, &DisasmGotoStrip::closed, this, [this] {
        updateMargins();
        viewport()->setFocus();
    });
    connect(nav_band_, &AidaDisasmNavBand::navigateToIndex, this,
        [this](std::size_t instruction_index) {
            if (!context_.publication || !context_.publication->snapshot)
                return;
            const auto& instructions = context_.publication->snapshot->instructions;
            if (instruction_index >= instructions.size())
                return;
            disasm_view::goto_address(disasm_view::runtime_address(context_,
                instructions[instruction_index].address).value_or(
                    instructions[instruction_index].address.value), context_);
        });
    connect(row_cache_, &DisasmRowCache::revisionChanged, this,
        &AidaDisasmView::onRowCacheRevision);
    connect(row_cache_, &DisasmRowCache::xrefResults, this,
        &AidaDisasmView::onXrefResults);
    connect(row_cache_, &DisasmRowCache::exportStatusChanged, this,
        &AidaDisasmView::onExportStatus);
    connect(poller_, &analysis_bridge::AidaRevisionPoller::revisionChanged, this,
        &AidaDisasmView::onRevisionChanged);
    connect(poller_, &analysis_bridge::AidaRevisionPoller::uiSerialChanged, this,
        &AidaDisasmView::onUiSerialChanged);
    connect(poller_, &analysis_bridge::AidaRevisionPoller::sourceInvalidated, this,
        [this] {
            recaptureContext();
            syncFromModel();
        });
    connect(&theme::AidaThemeController::instance(), &theme::AidaThemeController::themeChanged,
        this, [this] {
            theme::refresh_disasm_theme_tokens();
            theme_ = theme::disasm_theme_snapshot();
            theme_revision_ = theme::disasm_theme_revision();
            prefix_cache_.valid = false;
            content_width_max_ = 0.0;
            recomputeTextMetrics();
            nav_band_->invalidate_colors();
            nav_band_->set_theme_revision(theme_revision_);
            updateMargins();
            updateScrollbars();
            viewport()->update();
        });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (!context_.view)
            return;
        std::lock_guard<std::mutex> lock(context_.view->mutex);
        context_.view->target_scroll_y = static_cast<float>(value) *
            static_cast<float>(row_height_);
        context_.view->scroll_restore_pending = false;
    });

    ensure_dialog_hooks_installed();
    recaptureContext();
    updateMargins();
    updateScrollbars();
    syncFromModel();
}

AidaDisasmView::~AidaDisasmView()
{
    poller_->set_polling(false);
    if (row_cache_)
        row_cache_->unbind();
    if (context_.view)
        analysis_bridge::clear_delivery_targets(context_.view);
}

void AidaDisasmView::ensure_dialog_hooks_installed()
{
    static std::once_flag once;
    std::call_once(once, [] {
        disasm_view::set_rename_dialog_hook([](const disasm_view::workspace_context_t& context,
                                               const aida::analysis::address_t& address) {
            auto* dialog = new dialogs::AidaDisasmRenameDialog(context, address,
                QApplication::activeWindow());
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->open();
        });
        disasm_view::set_comment_dialog_hook([](const disasm_view::workspace_context_t& context,
                                                const aida::analysis::address_t& address) {
            auto* dialog = new dialogs::AidaDisasmCommentDialog(context, address,
                QApplication::activeWindow());
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->open();
        });
        disasm_view::set_rebase_dialog_hook([](const disasm_view::workspace_context_t& context) {
            auto* dialog = new dialogs::AidaDisasmRebaseDialog(context,
                QApplication::activeWindow());
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->open();
        });
        disasm_view::set_static_patch_review_hook(
            [](const disasm_view::workspace_context_t& context,
               disasm_view::static_patch_init_t init) {
                static QPointer<dialogs::AidaStaticPatchReviewDialog> active;
                if (active) {
                    active->reconfigure(context, std::move(init));
                    active->raise();
                    active->activateWindow();
                    return true;
                }
                auto* dialog = new dialogs::AidaStaticPatchReviewDialog(context,
                    std::move(init), QApplication::activeWindow());
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                active = dialog;
                dialog->open();
                return true;
            });
    });
}

void AidaDisasmView::refreshContext()
{
    recaptureContext();
    syncFromModel();
    applyScrollRestore();
}

void AidaDisasmView::recaptureContext()
{
    const auto previous_view = context_.view;
    context_ = presentation_key_.empty()
        ? disasm_view::capture_selected_workspace()
        : disasm_view::capture_selected_workspace(presentation_key_);
    if (context_.view == previous_view)
        return;
    if (context_.view) {
        row_cache_->bind(context_.view);
        hooks_ = std::make_shared<analysis_bridge::view_hooks_t>();
        hooks_->show_goto = [guard = QPointer<AidaDisasmView>(this)] {
            if (guard)
                guard->showGotoStrip();
        };
        hooks_->show_xref_popup = [guard = QPointer<AidaDisasmView>(this)](
                                      const aida::analysis::address_t& address) {
            if (guard)
                guard->openXrefPopup(address);
        };
        analysis_bridge::set_view_hooks(context_.view, hooks_);
    } else {
        row_cache_->unbind();
        hooks_.reset();
    }
}

void AidaDisasmView::recomputeTextMetrics()
{
    const QFontMetricsF metrics(theme::fonts::codeRegular());
    row_height_ = (std::max)(1, static_cast<int>(std::ceil(metrics.lineSpacing())));
    char_width_ = (std::max)(1.0, metrics.horizontalAdvance(u'0'));
    prefix_cache_.valid = false;
}

void AidaDisasmView::applyHorizontalExtent()
{
    const qreal available = static_cast<qreal>((std::max)(1, viewport()->width()));
    const int extent = content_width_max_ > available
        ? static_cast<int>(std::ceil(content_width_max_ - available)) +
            theme::tokens().spacing.sm
        : 0;
    if (horizontalScrollBar()->maximum() != extent)
        horizontalScrollBar()->setRange(0, extent);
}

analysis_bridge::revision_sample_t AidaDisasmView::sampleRevisions()
{
    analysis_bridge::revision_sample_t sample;
    const auto workspace = aida::analysis::workspace_registry().selected_for_ui();
    if (!workspace || workspace->closed())
        return sample;
    if (context_.workspace != workspace)
        recaptureContext();
    if (!context_.workspace)
        return sample;
    sample.workspace = context_.workspace.get();
    sample.generation = context_.workspace->generation();
    sample.analysis_revision = context_.workspace->analysis_revision();
    sample.overlay_revision = context_.workspace->overlay_revision();
    sample.view_revision = context_.workspace->view_state().revision;
    sample.ui_serial = context_.view
        ? context_.view->ui_serial.load(std::memory_order_acquire) : 0;
    sample.valid = true;
    return sample;
}

void AidaDisasmView::onRevisionChanged(quint64, quint64)
{
    recaptureContext();
    syncFromModel();
}

void AidaDisasmView::onUiSerialChanged(quint64)
{
    syncUiState();
    syncSelection();
    syncPresentation();
    syncMetadata();
    toolbar_->refresh_actions();
    updateScrollbars();
    applyScrollRestore();
    viewport()->update();
}

void AidaDisasmView::onRowCacheRevision(quint64)
{
    nav_band_->invalidate_colors();
    viewport()->update();
    prefetchVisible();
}

void AidaDisasmView::onXrefResults(aida::analysis::address_t,
                                   std::vector<disasm_view::xref_popup_entry_t> results,
                                   QString error)
{
    if (xref_popup_) {
        if (!error.isEmpty())
            xref_popup_->set_error(error);
        else
            xref_popup_->set_results(std::move(results));
    }
}

void AidaDisasmView::onExportStatus(QString, QString)
{
    syncUiState();
}

void AidaDisasmView::syncFromModel()
{
    if (!context_) {
        showEventState();
        return;
    }
    if (context_.publication) {
        if (context_.publication->generation != last_publication_generation_ ||
            context_.publication->analysis_revision != last_analysis_revision_ ||
            context_.publication->overlay_revision != last_overlay_revision_) {
            last_publication_generation_ = context_.publication->generation;
            last_analysis_revision_ = context_.publication->analysis_revision;
            last_overlay_revision_ = context_.publication->overlay_revision;
            row_cache_->clearAll();
            prefix_cache_.valid = false;
            content_width_max_ = 0.0;
            range_anchor_.reset();
            range_extent_.reset();
        }
    }
    if (context_.publication && context_.publication->snapshot) {
        file_metadata_banner::refresh(context_);
    }
    syncMetadata();
    syncBookmarks();
    syncSelection();
    syncUiState();
    syncPresentation();
    updateScrollbars();
    toolbar_->refresh_actions();
    showEventState();
    prefetchVisible();
    viewport()->update();
}

void AidaDisasmView::syncMetadata()
{
    metadata_lines_.clear();
    if (!context_.view)
        return;
    std::lock_guard<std::mutex> lock(context_.view->mutex);
    metadata_lines_.reserve(context_.view->metadata_lines.size());
    for (const auto& line : context_.view->metadata_lines)
        metadata_lines_.push_back({QString::fromStdString(line.text), line.kind});
}

void AidaDisasmView::syncBookmarks()
{
    if (!context_.view)
        return;
    const void* publication_key = static_cast<const void*>(context_.publication.get());
    const std::uint64_t overlay_revision = context_.publication
        ? context_.publication->overlay_revision : 0;
    if (bookmarks_ && bookmarks_publication_ == publication_key &&
        bookmarks_overlay_revision_ == overlay_revision)
        return;
    bookmarks_ = std::make_shared<std::vector<disasm_view::bookmark_t>>(
        disasm_view::bookmark_snapshot(context_));
    bookmarks_publication_ = publication_key;
    bookmarks_overlay_revision_ = overlay_revision;
}

void AidaDisasmView::syncSelection()
{
    if (!context_.view)
        return;
    bool scroll_to_selection = false;
    {
        std::lock_guard<std::mutex> lock(context_.view->mutex);
        selection_ = context_.view->selection;
        scroll_to_selection = context_.view->scroll_to_selection;
        if (scroll_to_selection)
            context_.view->scroll_to_selection = false;
    }
    nav_band_->set_selection(selection_);
    if (scroll_to_selection && selection_)
        applyScrollToSelection();
}

void AidaDisasmView::syncPresentation()
{
    if (!context_.view)
        return;
    disasm_view::addr_format_t format;
    bool show_bytes;
    int section;
    {
        std::lock_guard<std::mutex> lock(context_.view->mutex);
        format = context_.view->addr_format;
        show_bytes = context_.view->show_bytes;
        section = context_.view->active_section;
    }
    toolbar_->set_addr_format(format);
    toolbar_->set_show_bytes(show_bytes);
    paint_show_bytes_ = show_bytes;
    paint_addr_format_ = static_cast<int>(format);
    const auto can_rebase = context_.workspace &&
        context_.workspace->target_kind() == aida::analysis::target_kind_t::static_file &&
        context_.image;
    toolbar_->set_can_rebase(can_rebase);
    std::vector<std::string> section_names;
    std::vector<int> section_true_indices;
    if (context_.image) {
        for (std::size_t index = 0; index < context_.image->sections().size(); ++index) {
            const auto& section_record = context_.image->sections()[index];
            if (section_record.executable) {
                section_names.push_back(section_record.name);
                section_true_indices.push_back(static_cast<int>(index));
            }
        }
    }
    toolbar_->set_sections(section_names, section_true_indices, section);
}

void AidaDisasmView::syncUiState()
{
    clearNotices();
    if (!context_)
        return;
    const auto progress = context_.workspace->progress();
    const bool progress_incomplete = progress.total_units == 0 ||
        progress.completed_units < progress.total_units;
    if (((progress.readiness == aida::analysis::workspace_readiness_t::analyzing ||
          progress.readiness == aida::analysis::workspace_readiness_t::partial) &&
         progress_incomplete) ||
        progress.readiness == aida::analysis::workspace_readiness_t::cancelling) {
        const float fraction = progress.total_units == 0 ? 0.0f :
            static_cast<float>(progress.completed_units) /
            static_cast<float>(progress.total_units);
        const QString message = QStringLiteral("%1 of %2 units · %3%")
            .arg(progress.completed_units)
            .arg(progress.total_units)
            .arg(static_cast<int>((std::clamp)(fraction, 0.0f, 1.0f) * 100.0f));
        addNotice(QStringLiteral("analysis_progress"),
            QString::fromStdString(progress.phase), message,
            widgets::AidaSemantic::Info,
            progress.readiness == aida::analysis::workspace_readiness_t::cancelling
                ? QString() : QStringLiteral("Cancel"),
            [workspace = context_.workspace] {
                workspace->request_cancel();
            });
    }
    if (progress.error) {
        addNotice(QStringLiteral("analysis_error"), QStringLiteral("Analysis failed"),
            QString::fromStdString(progress.error->stable_code() + ": " +
                progress.error->message),
            widgets::AidaSemantic::Error, QString(), {});
    }
    std::string mutation_error;
    std::string derived_publication_error;
    bool derived_publication_pending = false;
    std::string format_error;
    std::string export_error;
    std::string export_status;
    {
        const auto mutations = disasm_view::mutation_state(context_);
        mutation_error = mutations.error;
        derived_publication_error = mutations.derived_publication_error;
        derived_publication_pending = mutations.derived_publication_pending;
    }
    {
        std::lock_guard<std::mutex> lock(context_.view->mutex);
        format_error = context_.view->format_error;
        export_error = context_.view->export_error;
        export_status = context_.view->export_status;
    }
    if (!mutation_error.empty())
        addNotice(QStringLiteral("overlay_error"), QStringLiteral("Overlay update failed"),
            QString::fromStdString(mutation_error), widgets::AidaSemantic::Error,
            QString(), {});
    if (!derived_publication_error.empty()) {
        addNotice(QStringLiteral("overlay_derived_publication_error"),
            QStringLiteral("Overlay committed; presentation refresh pending"),
            QString::fromStdString(derived_publication_error),
            widgets::AidaSemantic::Warning,
            derived_publication_pending ? QString() : QStringLiteral("Retry"),
            [context = context_] {
                static_cast<void>(disasm_view::queue_overlay_presentation_retry(context));
            });
    }
    if (!format_error.empty()) {
        addNotice(QStringLiteral("format_error"), QStringLiteral("Formatting failed"),
            QString::fromStdString(format_error), widgets::AidaSemantic::Error,
            QStringLiteral("Retry"), [this] {
                if (!context_.view)
                    return;
                {
                    std::lock_guard<std::mutex> lock(context_.view->mutex);
                    context_.view->format_error.clear();
                    context_.view->formatted.clear();
                    context_.view->formatted_revision.fetch_add(1, std::memory_order_acq_rel);
                    context_.view->pending_format_pages.clear();
                }
                context_.view->ui_serial.fetch_add(1, std::memory_order_acq_rel);
                row_cache_->unbind();
                row_cache_->bind(context_.view);
                prefetchVisible();
            });
    }
    if (!export_error.empty()) {
        addNotice(QStringLiteral("export_error"), QStringLiteral("Export failed"),
            QString::fromStdString(export_error), widgets::AidaSemantic::Error,
            QString(), {});
    } else if (!export_status.empty()) {
        addNotice(QStringLiteral("export_status"), QStringLiteral("Listing export"),
            QString::fromStdString(export_status), widgets::AidaSemantic::Info,
            QString(), {});
    }
    updateMargins();
}

void AidaDisasmView::clearNotices()
{
    auto* layout = notices_->layout();
    while (auto* item = layout->takeAt(0)) {
        if (auto* widget = item->widget())
            delete widget;
        delete item;
    }
}

void AidaDisasmView::addNotice(const QString& id, const QString& title,
                               const QString& message, widgets::AidaSemantic kind,
                               const QString& action_label, std::function<void()> action)
{
    auto* notice = new widgets::AidaNotice(title, message, kind, notices_);
    notice->setObjectName(QStringLiteral("aida.disasm.notice.") + id);
    if (!action_label.isEmpty() && action) {
        notice->setActionLabel(action_label);
        QObject::connect(notice->actionButton(), static_cast<void (QAbstractButton::*)(bool)>(&QAbstractButton::clicked), notices_,
            [action = std::move(action)] { action(); });
    }
    notices_->layout()->addWidget(notice);
    notice->show();
}

void AidaDisasmView::showEventState()
{
    const auto range = context_ ? disasm_view::instruction_range(context_) : std::nullopt;
    const bool have_records = context_ && context_.publication &&
        context_.publication->snapshot && range && range->first < range->second;
    if (context_ && have_records) {
        state_view_->hide();
        return;
    }
    if (!context_) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No workspace selected"));
        state_view_->setMessage(QStringLiteral(
            "Open or attach to a target to inspect its disassembly."));
        state_view_->setActionLabel(QString());
        state_view_->show();
        return;
    }
    const auto progress = context_.workspace->progress();
    if (progress.readiness == aida::analysis::workspace_readiness_t::failed) {
        state_view_->setState(widgets::AidaStateView::State::Error);
        state_view_->setTitle(QStringLiteral("Disassembly unavailable"));
        state_view_->setMessage(QStringLiteral(
            "Analysis failed before any instruction records were published."));
        state_view_->setActionLabel(QString());
    } else if (progress.readiness == aida::analysis::workspace_readiness_t::analyzing ||
               progress.readiness == aida::analysis::workspace_readiness_t::partial ||
               progress.readiness == aida::analysis::workspace_readiness_t::cancelling) {
        state_view_->setState(widgets::AidaStateView::State::Loading);
        state_view_->setTitle(QStringLiteral("Preparing disassembly"));
        state_view_->setMessage(QStringLiteral(
            "Instruction records will appear as soon as analysis publishes them."));
        state_view_->setActionLabel(QString());
    } else {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No instructions available"));
        state_view_->setMessage(QStringLiteral(
            "This target has no instruction records at the current readiness level."));
        state_view_->setActionLabel(QString());
    }
    state_view_->show();
    state_view_->raise();
}

void AidaDisasmView::updateMargins()
{
    const int toolbar_h = toolbar_->sizeHint().height();
    const int goto_h = goto_strip_->strip_visible() ? goto_strip_->sizeHint().height() : 0;
    int notices_h = 0;
    if (notices_->layout()->count() > 0)
        notices_h = (std::max)(0, notices_->sizeHint().height());
    const int nav_h = theme::tokens().spacing.sm;
    nav_band_->setFixedHeight(nav_h);
    const int top = toolbar_h + goto_h + notices_h + nav_h;
    setViewportMargins(0, top, 0, 0);
    toolbar_->setGeometry(0, 0, width(), toolbar_h);
    if (goto_h > 0)
        goto_strip_->setGeometry(0, toolbar_h, width(), goto_h);
    if (notices_h > 0)
        notices_->setGeometry(0, toolbar_h + goto_h, width(), notices_h);
    else
        notices_->setGeometry(0, toolbar_h + goto_h, width(), 0);
    nav_band_->setGeometry(0, toolbar_h + goto_h + notices_h, width(), nav_h);
    state_view_->setGeometry(0, top, width(), (std::max)(0, height() - top));
    if (state_view_->isVisible())
        state_view_->raise();
}

void AidaDisasmView::updateScrollbars()
{
    const auto range = context_ && context_.publication && context_.publication->snapshot
        ? disasm_view::instruction_range(context_) : std::nullopt;
    const std::size_t count_size = range ? range->second - range->first : 0;
    const std::size_t metadata_count = metadata_lines_.size();
    const std::size_t total_size = count_size >
            static_cast<std::size_t>((std::numeric_limits<int>::max)()) - metadata_count
        ? static_cast<std::size_t>((std::numeric_limits<int>::max)())
        : count_size + metadata_count;
    const int visible_rows = (std::max)(1,
        viewport()->height() / row_height_);
    const int maximum = total_size > static_cast<std::size_t>(visible_rows)
        ? static_cast<int>(total_size - static_cast<std::size_t>(visible_rows)) : 0;
    verticalScrollBar()->setRange(0, maximum);
    verticalScrollBar()->setSingleStep(3);
    verticalScrollBar()->setPageStep(visible_rows);
    horizontalScrollBar()->setSingleStep((std::max)(1,
        static_cast<int>(char_width_)));
    horizontalScrollBar()->setPageStep((std::max)(1,
        viewport()->width() - static_cast<int>(char_width_ * 4.0)));
    applyHorizontalExtent();
    if (range) {
        nav_band_->set_model(context_.publication->snapshot, range->first, range->second,
            row_cache_);
        nav_band_->invalidate_colors();
    }
}

void AidaDisasmView::applyScrollRestore()
{
    if (!context_.view)
        return;
    float target = 0.0f;
    {
        std::lock_guard<std::mutex> lock(context_.view->mutex);
        if (!context_.view->scroll_restore_pending)
            return;
        target = context_.view->target_scroll_y;
        context_.view->scroll_restore_pending = false;
    }
    verticalScrollBar()->setValue(static_cast<int>(target /
        static_cast<float>(row_height_)));
}

void AidaDisasmView::applyScrollToSelection()
{
    if (!selection_)
        return;
    const auto row = instructionRowFor(*selection_);
    if (row)
        verticalScrollBar()->setValue(static_cast<int>(metadata_lines_.size() + *row));
}

std::optional<std::size_t> AidaDisasmView::instructionRowFor(
    const aida::analysis::address_t& address) const
{
    if (!context_.publication || !context_.publication->snapshot)
        return std::nullopt;
    const auto range = disasm_view::instruction_range(context_);
    if (!range)
        return std::nullopt;
    const auto& instructions = context_.publication->snapshot->instructions;
    auto found = std::lower_bound(
        instructions.begin() + static_cast<std::ptrdiff_t>(range->first),
        instructions.begin() + static_cast<std::ptrdiff_t>(range->second), address,
        [](const aida::analysis::instruction_record_t& instruction,
           const aida::analysis::address_t& value) {
            return instruction.address < value;
        });
    if (found == instructions.begin() + static_cast<std::ptrdiff_t>(range->second) ||
        found->address != address)
        return std::nullopt;
    return static_cast<std::size_t>(std::distance(
        instructions.begin() + static_cast<std::ptrdiff_t>(range->first), found));
}

std::size_t AidaDisasmView::instructionRowCount() const
{
    if (!context_ || !context_.publication || !context_.publication->snapshot)
        return 0;
    const auto range = disasm_view::instruction_range(context_);
    if (!range || range->second <= range->first)
        return 0;
    const auto count = range->second - range->first;
    return count > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        ? static_cast<std::size_t>((std::numeric_limits<int>::max)()) : count;
}

std::optional<std::size_t> AidaDisasmView::selectedInstructionRow() const
{
    if (!selection_)
        return std::nullopt;
    return instructionRowFor(*selection_);
}

bool AidaDisasmView::rangeContains(std::size_t instruction_row) const
{
    if (!range_anchor_ || !range_extent_)
        return false;
    const auto lo = (std::min)(*range_anchor_, *range_extent_);
    const auto hi = (std::max)(*range_anchor_, *range_extent_);
    return instruction_row >= lo && instruction_row <= hi;
}

void AidaDisasmView::clearRangeSelection()
{
    if (!range_anchor_ && !range_extent_)
        return;
    range_anchor_.reset();
    range_extent_.reset();
    viewport()->update();
}

void AidaDisasmView::ensureInstructionRowVisible(std::size_t row)
{
    const auto total = instructionRowCount();
    if (total == 0)
        return;
    row = (std::min)(row, total - 1);
    const int target = static_cast<int>(metadata_lines_.size() + row);
    const int first = verticalScrollBar()->value();
    const int visible = (std::max)(1,
        viewport()->height() / row_height_);
    if (target < first)
        verticalScrollBar()->setValue(target);
    else if (target >= first + visible)
        verticalScrollBar()->setValue(target - visible + 1);
}

void AidaDisasmView::selectInstructionRow(std::size_t row, bool extend_range)
{
    const auto total = instructionRowCount();
    if (total == 0 || !context_.publication || !context_.publication->snapshot)
        return;
    row = (std::min)(row, total - 1);
    const auto range = disasm_view::instruction_range(context_);
    if (!range)
        return;
    const auto& instructions = context_.publication->snapshot->instructions;
    const auto& instruction = instructions[range->first + row];
    if (extend_range) {
        const auto anchor = range_anchor_ ? range_anchor_
            : selectedInstructionRow();
        range_anchor_ = anchor.value_or(row);
        range_extent_ = row;
        banner_selected_all_ = false;
    } else {
        clearRangeSelection();
    }
    disasm_view::select_address(instruction.address, context_, false);
    selection_ = instruction.address;
    banner_selected_all_ = false;
    nav_band_->set_selection(selection_);
    ensureInstructionRowVisible(row);
    viewport()->update();
}

void AidaDisasmView::moveSelectionBy(std::ptrdiff_t delta, bool extend_range)
{
    const auto total = instructionRowCount();
    if (total == 0)
        return;
    const auto current = selectedInstructionRow();
    std::size_t target;
    if (!current)
        target = delta < 0 ? total - 1 : 0;
    else if (delta < 0)
        target = *current > static_cast<std::size_t>(-delta)
            ? *current - static_cast<std::size_t>(-delta) : 0;
    else
        target = (std::min)(*current + static_cast<std::size_t>(delta), total - 1);
    selectInstructionRow(target, extend_range);
}

void AidaDisasmView::followSelectionTarget()
{
    if (!selection_ || !context_.publication || !context_.publication->snapshot)
        return;
    const auto row = instructionRowFor(*selection_);
    if (!row)
        return;
    const auto range = disasm_view::instruction_range(context_);
    if (!range)
        return;
    const auto& instructions = context_.publication->snapshot->instructions;
    const auto& instruction = instructions[range->first + *row];
    if (instruction.target_fact_count == 0 ||
        instruction.target_fact_begin >= context_.publication->snapshot->target_facts.size())
        return;
    const auto& target =
        context_.publication->snapshot->target_facts[instruction.target_fact_begin];
    if (target.direct)
        disasm_view::goto_address(disasm_view::runtime_address(context_,
            target.target).value_or(target.target.value), context_);
}

void AidaDisasmView::copyRangeToClipboard()
{
    if (!range_anchor_ || !range_extent_ || !context_.publication ||
        !context_.publication->snapshot)
        return;
    const auto range = disasm_view::instruction_range(context_);
    if (!range)
        return;
    const auto total = instructionRowCount();
    if (total == 0)
        return;
    const auto lo = (std::min)((std::min)(*range_anchor_, *range_extent_), total - 1);
    const auto hi = (std::min)((std::max)(*range_anchor_, *range_extent_), total - 1);
    const auto& instructions = context_.publication->snapshot->instructions;
    QString text;
    for (std::size_t row = lo; row <= hi; ++row) {
        const auto& instruction = instructions[range->first + row];
        const auto address = QString::fromStdString(disasm_view::address_label(context_,
            instruction.address,
            static_cast<disasm_view::addr_format_t>(paint_addr_format_)));
        const auto formatted = row_cache_->find(instruction.id);
        if (!text.isEmpty())
            text.push_back(u'\n');
        text += address;
        if (formatted && !formatted->text.empty()) {
            text += QStringLiteral("  ");
            text += QString::fromStdString(formatted->text);
        }
    }
    aida::qt::clipboard::set_text(text);
}

void AidaDisasmView::prefetchVisible()
{
    if (!context_ || !context_.publication || !context_.publication->snapshot)
        return;
    const auto range = disasm_view::instruction_range(context_);
    if (!range)
        return;
    const std::size_t count_size = range->second - range->first;
    const int first_row = verticalScrollBar()->value();
    const int visible_rows = viewport()->height() / row_height_ + 2;
    const std::size_t visible_start = static_cast<std::size_t>((std::max)(0, first_row));
    const std::size_t visible_end = static_cast<std::size_t>((std::max)(0,
        first_row + visible_rows));
    const std::size_t metadata_count = metadata_lines_.size();
    const std::size_t instruction_start = visible_start > metadata_count
        ? visible_start - metadata_count : 0;
    const std::size_t instruction_end = visible_end > metadata_count
        ? visible_end - metadata_count : 0;
    const std::size_t page_begin = range->first + (std::min)(count_size, instruction_start);
    const std::size_t page_end = (std::min)(range->second,
        range->first + (std::min)(count_size, instruction_end));
    if (page_begin < page_end)
        disasm_view::request_format_range(context_, page_begin, page_end);
}

disasm_frame_geometry_t AidaDisasmView::frameGeometry(const QFont& code_font)
{
    const QFontMetricsF metrics(code_font);
    const qreal char_width = (std::max)(1.0, metrics.horizontalAdvance(u'0'));
    disasm_frame_geometry_t geometry;
    geometry.row_height = static_cast<qreal>(row_height_);
    geometry.char_width = char_width;
    geometry.draw_bytes = paint_show_bytes_ &&
        static_cast<qreal>(viewport()->width()) >= char_width * k_min_bytes_viewport_columns;
    geometry.prefix_width = DisasmPainter::prefix_width_for(context_, code_font,
        metrics, paint_addr_format_, devicePixelRatioF(), prefix_cache_);
    geometry.gutter_width = (std::max)(char_width * 2.5,
        static_cast<qreal>(theme::tokens().spacing.xl));
    geometry.bytes_width = char_width * k_bytes_columns;
    geometry.prefix_x = geometry.gutter_width + char_width * 0.5;
    geometry.bytes_x = geometry.prefix_x + geometry.prefix_width + char_width * 1.5;
    geometry.instruction_x = geometry.draw_bytes
        ? geometry.bytes_x + geometry.bytes_width + char_width
        : geometry.bytes_x;
    geometry.text_baseline_dy = (geometry.row_height - metrics.height()) * 0.5 +
        metrics.ascent();
    return geometry;
}

void AidaDisasmView::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    painter.fillRect(event->rect(), theme::tokens().bg_base);
    if (context_ && context_.publication && context_.publication->snapshot)
        paintListing(painter, event);
    if (hasFocus()) {
        painter.setPen(QPen(theme::tokens().border_focus,
            static_cast<qreal>(theme::tokens().control.focus_ring)));
        painter.setBrush(Qt::NoBrush);
        const qreal inset = static_cast<qreal>(theme::tokens().control.focus_ring) * 0.5;
        painter.drawRect(QRectF(viewport()->rect()).adjusted(inset, inset, -inset, -inset));
    }
}

void AidaDisasmView::paintListing(QPainter& painter, QPaintEvent* event)
{
    const auto range = disasm_view::instruction_range(context_);
    if (!range)
        return;
    const auto& snapshot = *context_.publication->snapshot;
    const auto& instructions = snapshot.instructions;
    const std::size_t count_size = range->second - range->first;
    const std::size_t metadata_count = metadata_lines_.size();
    const std::size_t total_size = count_size >
            static_cast<std::size_t>((std::numeric_limits<int>::max)()) - metadata_count
        ? static_cast<std::size_t>((std::numeric_limits<int>::max)())
        : count_size + metadata_count;
    if (total_size == 0)
        return;
    const auto code_font = theme::fonts::codeRegular();
    painter.setFont(code_font);
    DisasmPainter listing_painter(painter, theme_, code_font, devicePixelRatioF());
    const qreal char_width = listing_painter.char_width();
    const int addr_format = paint_addr_format_;
    const qreal available_width = static_cast<qreal>(viewport()->width());

    const int first_row = verticalScrollBar()->value();
    const int visible_rows = viewport()->height() / row_height_ + 2;
    const std::size_t first = static_cast<std::size_t>((std::max)(0, first_row));
    const std::size_t last = (std::min)(total_size,
        static_cast<std::size_t>(first_row + visible_rows));
    const auto& clip = event->rect();
    const std::size_t clip_first = (std::max)(first,
        first + static_cast<std::size_t>(
            (std::max)(0, clip.top()) / row_height_));
    const std::size_t clip_last = (std::min)(last,
        first + static_cast<std::size_t>(
            (std::max)(0, clip.bottom()) / row_height_) + 1);

    const disasm_frame_geometry_t geometry = frameGeometry(code_font);

    const auto first_instruction = instructions.begin() +
        static_cast<std::ptrdiff_t>(range->first);
    const auto banner_section = disasm_view::section_for(context_, first_instruction->address);
    const QString banner_section_name = QString::fromStdString(
        banner_section ? banner_section->name : ".text");
    const QString banner_address = QString::fromStdString(disasm_view::address_label(
        context_, first_instruction->address,
        static_cast<disasm_view::addr_format_t>(addr_format)));

    const qreal x_offset = -static_cast<qreal>(horizontalScrollBar()->value());
    qreal max_content_x = 0.0;
    painter.save();
    painter.setClipRect(event->rect(), Qt::IntersectClip);
    painter.translate(x_offset, 0.0);
    for (std::size_t virtual_row = clip_first; virtual_row < clip_last; ++virtual_row) {
        const qreal row_y = (static_cast<qreal>(virtual_row) -
            static_cast<qreal>(first_row)) * static_cast<qreal>(row_height_);
        const qreal row_width = (std::max)(available_width - x_offset,
            geometry.instruction_x + 96.0 * char_width);
        const QRectF row_rect(0.0, row_y, row_width,
            static_cast<qreal>(row_height_));
        const bool hovered = hover_row_ >= 0 &&
            static_cast<std::size_t>(hover_row_) == virtual_row;
        if (virtual_row < metadata_count) {
            const auto& line = metadata_lines_[virtual_row];
            max_content_x = (std::max)(max_content_x, geometry.instruction_x +
                listing_painter.text_width(line.text));
            listing_painter.paint_metadata_row(row_rect, geometry, banner_section_name,
                banner_address, line.text, line.kind,
                banner_selected_all_, hovered);
            continue;
        }
        const std::size_t row_index = virtual_row - metadata_count;
        const std::size_t index = range->first + row_index;
        if (index >= range->second)
            break;
        const auto& instruction = instructions[index];
        const auto formatted = row_cache_->find(instruction.id);
        const bool selected = selection_ && *selection_ == instruction.address;
        bool bookmarked = false;
        if (bookmarks_) {
            const auto mark = std::lower_bound(bookmarks_->begin(), bookmarks_->end(),
                instruction.address.value,
                [](const disasm_view::bookmark_t& item, std::uint64_t value) {
                    return item.addr < value;
                });
            bookmarked = mark != bookmarks_->end() &&
                mark->addr == instruction.address.value;
        }
        const auto instruction_section = disasm_view::section_for(context_,
            instruction.address);
        const QString section = QString::fromStdString(
            instruction_section ? instruction_section->name : ".text");
        const QString address = QString::fromStdString(disasm_view::address_label(context_,
            instruction.address, static_cast<disasm_view::addr_format_t>(addr_format)));
        const QString bytes = formatted
            ? QString::fromStdString(formatted->bytes) : QString();
        const QString name = QString::fromStdString(
            disasm_view::resolve_name(context_, instruction.address));
        const QString user_comment = QString::fromStdString(
            disasm_view::comment(context_, instruction.address));
        const QString generated_comment = QString::fromStdString(
            disasm_view::auto_comment(context_, instruction.address));
        const auto function = std::lower_bound(snapshot.functions.begin(),
            snapshot.functions.end(), instruction.address,
            [](const aida::analysis::function_record_t& item,
               const aida::analysis::address_t& value) { return item.start < value; });
        const bool function_start = function != snapshot.functions.end() &&
            function->start == instruction.address;
        const auto runtime = disasm_view::runtime_address(context_, instruction.address)
            .value_or(instruction.address.value);
        const bool range_selected = rangeContains(row_index);
        listing_painter.paint_instruction_row(row_rect, geometry, section, address, bytes,
            formatted ? &*formatted : nullptr, runtime, name, function_start, user_comment,
            generated_comment, selected, range_selected, hovered, bookmarked);
        if (formatted && !formatted->error.empty()) {
            max_content_x = (std::max)(max_content_x, geometry.instruction_x +
                listing_painter.text_width(QString::fromStdString(formatted->error)));
        } else if (formatted) {
            max_content_x = (std::max)(max_content_x, geometry.instruction_x +
                listing_painter.char_width() * static_cast<qreal>(
                    formatted->text.size() + 32U) +
                listing_painter.text_width(QString::fromStdString(
                    user_comment.toStdString())));
        } else {
            max_content_x = (std::max)(max_content_x, geometry.instruction_x +
                listing_painter.char_width() * 48.0);
        }
        if (instruction.target_fact_count != 0 &&
            instruction.target_fact_begin < snapshot.target_facts.size()) {
            const auto& target = snapshot.target_facts[instruction.target_fact_begin];
            if (target.direct) {
                const auto target_instruction = std::lower_bound(
                    instructions.begin() + static_cast<std::ptrdiff_t>(range->first),
                    instructions.begin() + static_cast<std::ptrdiff_t>(range->second),
                    target.target,
                    [](const aida::analysis::instruction_record_t& item,
                       const aida::analysis::address_t& value) {
                        return item.address < value;
                    });
                if (target_instruction != instructions.begin() +
                        static_cast<std::ptrdiff_t>(range->second) &&
                    target_instruction->address == target.target) {
                    const auto target_index = static_cast<std::size_t>(std::distance(
                        instructions.begin(), target_instruction));
                    const qreal row_h = static_cast<qreal>(row_height_);
                    const qreal target_y_unclamped = (static_cast<qreal>(target_index) -
                        static_cast<qreal>(index)) * row_h + row_y + row_h * 0.5;
                    const qreal target_y = (std::clamp)(target_y_unclamped, 0.0,
                        static_cast<qreal>(viewport()->height()));
                    const qreal source_y = row_y + row_h * 0.5;
                    listing_painter.queue_flow_arrow(0.0, source_y, target_y,
                        target_y_unclamped < source_y);
                }
            }
        }
    }
    painter.restore();
    painter.fillRect(QRectF(0.0, 0.0, geometry.gutter_width,
        static_cast<qreal>(viewport()->height())), theme_.gutter_bg);
    listing_painter.flush_flow_arrows(QRectF(viewport()->rect()),
        available_width, geometry.gutter_width);
    if (max_content_x > content_width_max_) {
        content_width_max_ = max_content_x;
        applyHorizontalExtent();
    }
}

void AidaDisasmView::scrollContentsBy(int dx, int dy)
{
    if (dy != 0)
        viewport()->scroll(0, dy * row_height_);
    if (dx != 0)
        viewport()->scroll(dx, 0);
    prefetchVisible();
    nav_band_->update();
}

std::optional<std::size_t> AidaDisasmView::rowAt(const QPointF& pos) const
{
    if (pos.y() < 0.0)
        return std::nullopt;
    const auto row = static_cast<std::size_t>(pos.y() /
            static_cast<qreal>(row_height_)) +
        static_cast<std::size_t>(verticalScrollBar()->value());
    return row;
}

std::optional<aida::analysis::instruction_record_t> AidaDisasmView::instructionAtRow(
    std::size_t row) const
{
    if (!context_.publication || !context_.publication->snapshot)
        return std::nullopt;
    if (row < metadata_lines_.size())
        return std::nullopt;
    const auto range = disasm_view::instruction_range(context_);
    if (!range)
        return std::nullopt;
    const std::size_t index = range->first + (row - metadata_lines_.size());
    if (index >= range->second)
        return std::nullopt;
    return context_.publication->snapshot->instructions[index];
}

void AidaDisasmView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    const auto row = rowAt(event->position());
    if (!row) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    if (*row < metadata_lines_.size()) {
        banner_selected_all_ = true;
        banner_selected_line_ = *row;
        selection_.reset();
        clearRangeSelection();
        if (context_.view) {
            std::lock_guard<std::mutex> lock(context_.view->mutex);
            context_.view->selection.reset();
        }
        viewport()->update();
        return;
    }
    const auto instruction = instructionAtRow(*row);
    if (!instruction) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    const std::size_t instruction_row = *row - metadata_lines_.size();
    if ((event->modifiers() & Qt::ShiftModifier) != 0) {
        const auto anchor = range_anchor_ ? range_anchor_ : selectedInstructionRow();
        range_anchor_ = anchor.value_or(instruction_row);
        range_extent_ = instruction_row;
    } else {
        clearRangeSelection();
        range_anchor_ = instruction_row;
        range_extent_ = instruction_row;
    }
    drag_selecting_ = true;
    disasm_view::select_address(instruction->address, context_, false);
    selection_ = instruction->address;
    banner_selected_all_ = false;
    nav_band_->set_selection(selection_);
    viewport()->update();
}

void AidaDisasmView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && drag_selecting_) {
        drag_selecting_ = false;
        if (range_anchor_ && range_extent_ && *range_anchor_ == *range_extent_)
            clearRangeSelection();
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void AidaDisasmView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseDoubleClickEvent(event);
        return;
    }
    const auto row = rowAt(event->position());
    if (!row || *row < metadata_lines_.size())
        return;
    const auto instruction = instructionAtRow(*row);
    if (!instruction)
        return;
    selection_ = instruction->address;
    followSelectionTarget();
}

void AidaDisasmView::mouseMoveEvent(QMouseEvent* event)
{
    const auto row = rowAt(event->position());
    if (drag_selecting_ && (event->buttons() & Qt::LeftButton)) {
        const auto total = instructionRowCount();
        if (total != 0) {
            const std::size_t raw = row.value_or(metadata_lines_.size());
            const std::size_t clamped = (std::min)(raw,
                total - 1 + metadata_lines_.size());
            if (clamped >= metadata_lines_.size()) {
                const std::size_t extent = clamped - metadata_lines_.size();
                if (!range_anchor_)
                    range_anchor_ = extent;
                if (!range_extent_ || *range_extent_ != extent) {
                    range_extent_ = extent;
                    viewport()->update();
                }
            }
        }
    }
    const int hover = row ? static_cast<int>(*row) : -1;
    if (hover != hover_row_) {
        hover_row_ = hover;
        viewport()->update();
    }
    if (row && *row >= metadata_lines_.size()) {
        const auto instruction = instructionAtRow(*row);
        if (instruction) {
            const auto formatted = row_cache_->find(instruction->id);
            if (formatted && !formatted->text.empty() && !formatted->tokens.empty()) {
                const auto geometry = frameGeometry(theme::fonts::codeRegular());
                const qreal char_width = geometry.char_width;
                const auto column = static_cast<std::int64_t>(std::floor(
                    (event->position().x() +
                        static_cast<qreal>(horizontalScrollBar()->value()) -
                        geometry.instruction_x) / char_width));
                QString hovered_token;
                for (const auto& token : formatted->tokens) {
                    if (column >= static_cast<std::int64_t>(token.offset) &&
                        column < static_cast<std::int64_t>(token.offset + token.length)) {
                        hovered_token = QString::fromStdString(formatted->text.substr(
                            token.offset, token.length));
                        break;
                    }
                }
                if (!hovered_token.isEmpty()) {
                    QToolTip::showText(event->globalPosition().toPoint(), hovered_token,
                        viewport());
                    QAbstractScrollArea::mouseMoveEvent(event);
                    return;
                }
            }
        }
    }
    QToolTip::hideText();
    QAbstractScrollArea::mouseMoveEvent(event);
}

void AidaDisasmView::contextMenuEvent(QContextMenuEvent* event)
{
    if (!context_ || !context_.publication || !context_.publication->snapshot)
        return;
    const auto origin = event->reason() == QContextMenuEvent::Keyboard
        ? aida::ui::context_menu_open_origin_t::menu_key
        : aida::ui::context_menu_open_origin_t::pointer;
    QPoint global_pos = event->globalPos();
    if (origin == aida::ui::context_menu_open_origin_t::menu_key) {
        const auto row = rowAt(event->pos());
        if (row) {
            const qreal row_h = static_cast<qreal>(row_height_);
            const qreal local_y = (static_cast<qreal>(*row) -
                static_cast<qreal>(verticalScrollBar()->value())) * row_h;
            global_pos = viewport()->mapToGlobal(QPoint(0,
                static_cast<int>(local_y + row_h)));
        }
    }
    const auto row = rowAt(event->pos());
    if (!row)
        return;
    if (*row < metadata_lines_.size()) {
        banner_selected_line_ = *row;
        openMetadataMenu(*row, origin, global_pos);
        return;
    }
    const auto instruction = instructionAtRow(*row);
    if (!instruction)
        return;
    if (!selection_ || *selection_ != instruction->address) {
        disasm_view::select_address(instruction->address, context_, false);
        selection_ = instruction->address;
    }
    openInstructionMenu(*instruction, row_cache_->find(instruction->id), origin,
        global_pos);
}

void AidaDisasmView::keyPressEvent(QKeyEvent* event)
{
    const bool copy = (event->modifiers() & Qt::ControlModifier) &&
        (event->key() == Qt::Key_C || event->key() == Qt::Key_Insert);
    if (copy && context_ && context_.publication && context_.publication->snapshot) {
        if (range_anchor_ && range_extent_) {
            copyRangeToClipboard();
            event->accept();
            return;
        }
        if (banner_selected_all_) {
            const auto menu = makeMetadataMenu(banner_selected_line_);
            if (menu) {
                aida::ui::analysis_context_menu::execute_shortcut(std::move(*menu),
                    "analysis.copy.metadata");
                event->accept();
                return;
            }
        }
        if (selection_) {
            const auto& instructions = context_.publication->snapshot->instructions;
            const auto found = std::lower_bound(instructions.begin(), instructions.end(),
                *selection_,
                [](const aida::analysis::instruction_record_t& instruction,
                   const aida::analysis::address_t& address) {
                    return instruction.address < address;
                });
            if (found != instructions.end() && found->address == *selection_) {
                aida::ui::analysis_context_menu::execute_shortcut(
                    makeInstructionMenu(*found,
                        disasm_view::formatted_instruction(context_, found->id)),
                    "analysis.copy.line");
                event->accept();
                return;
            }
        }
    }
    if (event->key() == Qt::Key_Escape) {
        if (range_anchor_ || range_extent_) {
            clearRangeSelection();
            event->accept();
            return;
        }
        disasm_view::navigate_back(context_);
        event->accept();
        return;
    }
    const bool has_listing = context_ && context_.publication &&
        context_.publication->snapshot && instructionRowCount() != 0;
    if (has_listing) {
        const Qt::KeyboardModifiers mods = event->modifiers();
        const bool extend = (mods & Qt::ShiftModifier) != 0;
        const bool plain = (mods & (Qt::ControlModifier | Qt::AltModifier)) == 0;
        const bool ctrl = (mods & Qt::ControlModifier) != 0 &&
            (mods & Qt::AltModifier) == 0;
        const int page = (std::max)(1,
            viewport()->height() / row_height_);
        switch (event->key()) {
        case Qt::Key_Up:
            if (plain || extend) {
                moveSelectionBy(-1, extend);
                event->accept();
                return;
            }
            break;
        case Qt::Key_Down:
            if (plain || extend) {
                moveSelectionBy(1, extend);
                event->accept();
                return;
            }
            break;
        case Qt::Key_PageUp:
            if (plain || extend) {
                moveSelectionBy(-static_cast<std::ptrdiff_t>(page), extend);
                event->accept();
                return;
            }
            break;
        case Qt::Key_PageDown:
            if (plain || extend) {
                moveSelectionBy(static_cast<std::ptrdiff_t>(page), extend);
                event->accept();
                return;
            }
            break;
        case Qt::Key_Home:
            if (plain || ctrl) {
                selectInstructionRow(0, extend);
                event->accept();
                return;
            }
            break;
        case Qt::Key_End:
            if (plain || ctrl) {
                selectInstructionRow(instructionRowCount() - 1, extend);
                event->accept();
                return;
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (plain && selection_) {
                followSelectionTarget();
                event->accept();
                return;
            }
            break;
        default:
            break;
        }
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void AidaDisasmView::focusInEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusInEvent(event);
    viewport()->update();
}

void AidaDisasmView::focusOutEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusOutEvent(event);
    viewport()->update();
}

void AidaDisasmView::showEvent(QShowEvent* event)
{
    QAbstractScrollArea::showEvent(event);
    poller_->set_polling(true);
    recaptureContext();
    syncFromModel();
    applyScrollRestore();
}

void AidaDisasmView::hideEvent(QHideEvent* event)
{
    QAbstractScrollArea::hideEvent(event);
    poller_->set_polling(false);
}

void AidaDisasmView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateMargins();
    updateScrollbars();
    prefetchVisible();
}

void AidaDisasmView::showGotoStrip()
{
    goto_strip_->show_strip();
    updateMargins();
}

void AidaDisasmView::openXrefPopup(const aida::analysis::address_t& address)
{
    if (!xref_popup_) {
        auto* popup = new dialogs::AidaDisasmXrefPopup(context_, address,
            QApplication::activeWindow());
        popup->setAttribute(Qt::WA_DeleteOnClose);
        connect(popup, &dialogs::AidaDisasmXrefPopup::navigateRequested, this,
            [this](quint64 address_value) {
                disasm_view::goto_address(address_value, context_);
            });
        connect(popup, &QObject::destroyed, this, [this] {
            xref_popup_ = nullptr;
        });
        xref_popup_ = popup;
    }
    xref_popup_->set_scanning(true);
    xref_popup_->open();
    xref_popup_->raise();
    xref_popup_->activateWindow();
}

void AidaDisasmView::openInstructionMenu(
    const aida::analysis::instruction_record_t& instruction,
    const std::optional<disasm_view::formatted_instruction_t>& formatted,
    aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos)
{
    aida::ui::analysis_context_menu::open(
        makeInstructionMenu(instruction, formatted), origin, global_pos, this);
}

void AidaDisasmView::openMetadataMenu(std::size_t line_index,
                                      aida::ui::context_menu_open_origin_t origin,
                                      const QPoint& global_pos)
{
    auto menu = makeMetadataMenu(line_index);
    if (menu)
        aida::ui::analysis_context_menu::open(std::move(*menu), origin, global_pos, this);
}

aida::ui::analysis_context_menu::context_t AidaDisasmView::makeInstructionMenu(
    const aida::analysis::instruction_record_t& instruction,
    const std::optional<disasm_view::formatted_instruction_t>& formatted)
{
    using namespace aida::ui::analysis_context_menu;
    using aida::ui::action_check_state_t;
    using aida::ui::action_handler_result_t;
    using aida::ui::capability_state_t;
    const auto context = context_;
    context_t menu;
    menu.kind = menu_kind_t::instruction;
    menu.entity_id = "instruction:" + std::to_string(instruction.id) + ":" +
        std::to_string(instruction.address.value);
    menu.generation = context_generation(context);
    menu.live_generation = [context]() { return context_generation(context); };
    const std::optional<aida::analysis::address_t> expected = instruction.address;
    const auto view_state = context.view;
    menu.validate_identity = [view_state, expected]() {
        if (!view_state)
            return aida::ui::capability_state_t::unavailable(
                "The selected disassembly instruction changed");
        std::lock_guard<std::mutex> lock(view_state->mutex);
        return view_state->selection && *view_state->selection == expected
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The selected disassembly instruction changed");
    };
    const auto runtime = disasm_view::runtime_address(context, instruction.address)
        .value_or(instruction.address.value);
    disasm_view::addr_format_t format;
    bool show_bytes;
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        format = context.view->addr_format;
        show_bytes = context.view->show_bytes;
    }
    const auto address = disasm_view::address_label(context, instruction.address, format);
    const auto text = formatted ? formatted->text : std::string();
    const auto bytes = formatted ? formatted->bytes : std::string();
    const auto name = disasm_view::resolve_name(context, instruction.address);
    auto copy = [&menu](const char* id, std::string value, const char* reason) {
        action_slot_t slot;
        if (value.empty())
            slot.capability = capability_state_t::unavailable(reason);
        slot.invoke = [value = std::move(value)]() {
            aida::qt::clipboard::set_text(QString::fromStdString(value));
            return action_handler_result_t::completed();
        };
        menu.actions.emplace(id, std::move(slot));
    };
    auto unavailable = [&menu](const char* id, std::string reason) {
        action_slot_t slot;
        slot.capability = capability_state_t::unavailable(reason);
        slot.invoke = [reason = std::move(reason)]() {
            return action_handler_result_t::failed(reason);
        };
        menu.actions.insert_or_assign(id, std::move(slot));
    };
    const auto focus = [](const char* view_id) {
        const auto hook = analysis_bridge::view_focus_hook();
        if (hook)
            hook(view_id);
        return true;
    };
    unavailable("analysis.navigate.disassembly_side",
        "Independent side documents require per-instance disassembly presentation state");
    unavailable("analysis.navigate.follow",
        "The selected instruction has no direct resolved target");
    unavailable("analysis.navigate.callers",
        "The selected instruction is not inside a discovered function");
    unavailable("analysis.navigate.callees",
        "The current cross-reference provider does not expose a filtered outgoing-call view");
    unavailable("analysis.navigate.pseudocode",
        "The selected instruction is not inside a discovered function");
    unavailable("analysis.function.decompile",
        "The selected instruction is not inside a discovered function");
    unavailable("analysis.modify.retype",
        "The selected address cannot be staged in the canonical Types apply workflow");
    unavailable("analysis.modify.patch",
        "Reviewed runtime patching requires a process-backed workspace");
    unavailable("analysis.modify.assemble",
        "No assembler provider is registered; use reviewed Patch Bytes with explicitly assembled bytes");
    unavailable("analysis.modify.nop",
        "Reviewed NOP staging requires a process-backed instruction with a known byte length");
    unavailable("analysis.modify.bookmark",
        "The selected address is already bookmarked");
    unavailable("analysis.modify.remove_bookmark",
        "The selected address is not bookmarked");
    unavailable("analysis.debug.breakpoint",
        "Breakpoint definitions require a process-backed debugger workspace");
    unavailable("analysis.debug.hardware_breakpoint",
        "Mode-specific breakpoint preselection is not exposed; stage the address, then choose Add HW Exec in Breakpoints");
    copy("analysis.copy.line", address + "  " + text,
        "The instruction address is unavailable");
    copy("analysis.copy.text", text, "The instruction is not formatted");
    copy("analysis.copy.instruction", text, "The instruction is not formatted");
    copy("analysis.copy.address", address, "The address is unavailable");
    copy("analysis.copy.bytes", bytes, "The instruction bytes are not formatted");
    copy("analysis.copy.name", name, "The selected address has no symbol name");
    const std::string address_va = disasm_view::address_label(context, instruction.address,
        disasm_view::addr_format_t::va);
    const std::string address_rva = disasm_view::address_label(context, instruction.address,
        disasm_view::addr_format_t::rva);
    const auto file_offset = disasm_view::provider_offset(context, instruction.address);
    copy("analysis.copy.address_va", address_va, "The selected virtual address is unavailable");
    copy("analysis.copy.address_rva", context.image ? address_rva : std::string(),
        "The selected workspace has no image mapping for an RVA");
    copy("analysis.copy.address_file", file_offset
        ? disasm_view::address_label(context, instruction.address,
            disasm_view::addr_format_t::file_offset)
        : std::string(), "The selected address has no mapped file offset");
    std::string module_address;
    const auto image_base = context.workspace->identity().image_base();
    if (context.image && runtime >= image_base &&
        runtime - image_base < context.image->image_size()) {
        char offset[48]{};
        std::snprintf(offset, sizeof(offset), "%s+0x%llX",
            context.workspace->identity().bin_name().c_str(),
            static_cast<unsigned long long>(runtime - image_base));
        module_address = offset;
    }
    copy("analysis.copy.address_module", module_address,
        "The selected address is outside the workspace module mapping");
    copy("analysis.export.line", formatted ? address + "  " + bytes + "  " + text : std::string(),
        "The instruction is not formatted");
    menu.actions["analysis.navigate.back"].invoke = [context]() {
        disasm_view::navigate_back(context);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.forward"].invoke = [context]() {
        disasm_view::navigate_forward(context);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.disassembly"].invoke = [context, runtime, focus]() {
        disasm_view::goto_address(runtime, context);
        focus("document.disassembly");
        return action_handler_result_t::completed();
    };
    if (file_offset) {
        menu.actions["analysis.navigate.hex"].invoke = [context, address = instruction.address]() {
            std::string error;
            if (!hex_view::focus_address(context, address, &error))
                return action_handler_result_t::failed(error);
            const auto hook = analysis_bridge::view_focus_hook();
            if (hook)
                hook("document.hex");
            return action_handler_result_t::completed();
        };
    } else {
        unavailable("analysis.navigate.hex",
            "The selected address has no file or provider mapping for the Hex document");
    }
    menu.actions["analysis.navigate.functions"].invoke = [context, runtime, focus]() {
        disasm_view::select_address(runtime, context, false);
        focus("view.analysis.functions");
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.structures"].invoke = [context, runtime, focus]() {
        disasm_view::select_address(runtime, context, false);
        auto& hooks = analysis::analysis_host_hooks();
        if (hooks.activate_types_subview)
            hooks.activate_types_subview(0);
        focus("view.types.structures");
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.types"].invoke = [context, runtime, focus]() {
        disasm_view::select_address(runtime, context, false);
        focus("view.types.inferred");
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.xrefs"].invoke = [context, runtime]() {
        disasm_view::open_xrefs(runtime, context);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.xrefs_from"].invoke = [context, runtime, focus]() {
        disasm_view::select_address(runtime, context, false);
        focus("view.analysis.references");
        return action_handler_result_t::completed();
    };
    if (instruction.target_fact_count != 0 &&
        instruction.target_fact_begin < context.publication->snapshot->target_facts.size()) {
        const auto target = context.publication->snapshot->target_facts[instruction.target_fact_begin];
        if (target.direct) {
            menu.actions["analysis.navigate.follow"].invoke = [context, target]() {
                disasm_view::goto_address(disasm_view::runtime_address(context,
                    target.target).value_or(target.target.value), context);
                return action_handler_result_t::completed();
            };
            menu.actions["analysis.navigate.follow"].capability = capability_state_t::available();
        }
    }
    menu.actions["analysis.modify.rename"].invoke = [context, value = instruction.address]() {
        disasm_view::request_rename_dialog(context, value);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.modify.comment"].invoke = [context, value = instruction.address]() {
        disasm_view::request_comment_dialog(context, value);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.modify.retype"].invoke = [context, value = instruction.address, focus]() {
        const auto mapped = disasm_view::runtime_address(context, value);
        if (!mapped)
            return action_handler_result_t::failed(
                "The selected address is not mapped in the active type workspace.");
        auto& hooks = analysis::analysis_host_hooks();
        if (!hooks.stage_type_application)
            return action_handler_result_t::failed("The analysis UI is not available");
        std::string error;
        if (!hooks.stage_type_application(context.workspace, *mapped, error))
            return action_handler_result_t::failed(error);
        focus("view.types.structures");
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.modify.retype"].capability = capability_state_t::available();
    if (disasm_view::bookmarked(context, instruction.address)) {
        menu.actions["analysis.modify.remove_bookmark"].invoke = [context, value = instruction.address]() {
            return disasm_view::queue_bookmark(context, value, {})
                ? action_handler_result_t::completed()
                : action_handler_result_t::failed("The bookmark update was rejected");
        };
        menu.actions["analysis.modify.remove_bookmark"].capability = capability_state_t::available();
    } else {
        menu.actions["analysis.modify.bookmark"].invoke =
            [context, value = instruction.address, label = name.empty() ? address : name]() {
                return disasm_view::queue_bookmark(context, value, label)
                    ? action_handler_result_t::completed()
                    : action_handler_result_t::failed("The bookmark update was rejected");
            };
        menu.actions["analysis.modify.bookmark"].capability = capability_state_t::available();
    }
    const auto function = disasm_view::enclosing_function_start(runtime, context);
    if (function != 0) {
        menu.actions["analysis.navigate.graph"].invoke = [context, runtime, focus]() {
            disasm_view::goto_address(runtime, context);
            focus("document.graph");
            return action_handler_result_t::completed();
        };
        menu.actions["analysis.navigate.graph"].capability = capability_state_t::available();
        auto decompile = [context, function, focus]() {
            diag::log_tagged_fmt("decompiler", "disasm_view decompile_action typed_pipeline path=primary function=0x%llX",
                static_cast<unsigned long long>(function));
            pseudocode_view::request_decompile(context, function, false);
            focus("document.pseudocode");
            return action_handler_result_t::completed();
        };
        menu.actions["analysis.navigate.pseudocode"].invoke = decompile;
        menu.actions["analysis.navigate.pseudocode"].capability = capability_state_t::available();
        menu.actions["analysis.function.decompile"].invoke = std::move(decompile);
        menu.actions["analysis.function.decompile"].capability = capability_state_t::available();
        menu.actions["analysis.navigate.callers"].invoke = [context, function]() {
            disasm_view::open_xrefs(function, context);
            return action_handler_result_t::completed();
        };
        menu.actions["analysis.navigate.callers"].capability = capability_state_t::available();
    } else {
        unavailable("analysis.navigate.graph",
            "No recovered function contains the selected instruction");
    }
    const auto process = context.workspace->identity().process();
    if (instruction.length != 0 && file_offset) {
        menu.actions["analysis.modify.patch"].invoke =
            [context, value = instruction.address,
             extent = static_cast<std::uint64_t>(instruction.length)]() {
                std::string error;
                if (!disasm_view::open_static_patch_review(context, value, extent,
                        disasm_view::static_patch_mode_t::bytes, &error))
                    return action_handler_result_t::failed(error);
                return action_handler_result_t::completed();
            };
        menu.actions["analysis.modify.patch"].capability = capability_state_t::available();
        menu.actions["analysis.modify.nop"].invoke =
            [context, value = instruction.address,
             extent = static_cast<std::uint64_t>(instruction.length)]() {
                std::string error;
                if (!disasm_view::open_static_patch_review(context, value, extent,
                        disasm_view::static_patch_mode_t::nop_fill, &error))
                    return action_handler_result_t::failed(error);
                return action_handler_result_t::completed();
            };
        menu.actions["analysis.modify.nop"].capability = capability_state_t::available();
    } else {
        unavailable("analysis.modify.patch",
            "The selected instruction has no fully provider-backed byte range");
        unavailable("analysis.modify.nop",
            "The selected instruction has no fully provider-backed byte range");
    }
    const auto debugger_context = debugger_interaction::capture(
        debugger_interaction::kind_t::instruction, runtime, 0, -1, 0,
        static_cast<std::uint64_t>(instruction.length));
    const bool debugger_matches_workspace = process &&
        process->creation_time_100ns != 0 &&
        debugger_context.target_pid == process->pid &&
        debugger_context.process_creation_time_100ns == process->creation_time_100ns &&
        debugger_interaction::is_current(debugger_context);
    if (debugger_matches_workspace) {
        menu.actions["analysis.modify.patch"].invoke =
            [debugger_context, focus,
             extent = static_cast<std::uint64_t>(instruction.length)]() {
                std::string error;
                if (!debugger_view::stage_patch_review(debugger_context, extent,
                        "Reviewed patch from Disassembly", &error))
                    return action_handler_result_t::failed(error);
                focus("view.debug.patches");
                return action_handler_result_t::completed();
            };
        menu.actions["analysis.modify.patch"].capability = capability_state_t::available();
        if (instruction.length != 0) {
            menu.actions["analysis.modify.nop"].invoke =
                [debugger_context, focus,
                 extent = static_cast<std::uint64_t>(instruction.length)]() {
                    std::string error;
                    if (!debugger_view::stage_nop_review(debugger_context, extent, &error))
                        return action_handler_result_t::failed(error);
                    focus("view.debug.patches");
                    return action_handler_result_t::completed();
                };
            menu.actions["analysis.modify.nop"].capability = capability_state_t::available();
        }
        const auto breakpoint_capability = debugger_view::address_mutation_capability(
            debugger_context, true);
        if (breakpoint_capability.enabled) {
            menu.actions["analysis.debug.breakpoint"].invoke =
                [debugger_context]() {
                    std::string error;
                    if (!debugger_view::queue_toggle_breakpoint(debugger_context, &error))
                        return action_handler_result_t::failed(error);
                    return action_handler_result_t::completed();
                };
            menu.actions["analysis.debug.breakpoint"].capability =
                capability_state_t::available();
        } else {
            unavailable("analysis.debug.breakpoint",
                breakpoint_capability.disabled_reason
                    ? breakpoint_capability.disabled_reason
                    : "Breakpoint toggle is unavailable");
        }
    } else if (process) {
        const std::string reason = "Attach the debugger to PID " +
            std::to_string(process->pid) + " before staging a runtime breakpoint; Patch and NOP remain reversible workspace overlays";
        unavailable("analysis.debug.breakpoint", reason);
    }
    menu.actions["analysis.function.source"].invoke = [context]() {
        auto& hooks = analysis::analysis_host_hooks();
        if (hooks.open_source_reconstruction)
            hooks.open_source_reconstruction(context.workspace);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.function.aob"].invoke = [context, runtime, focus]() {
        const auto generator = aob_generator::state_for(context);
        int count = 16;
        bool wildcard = true;
        if (generator) {
            std::lock_guard<std::mutex> lock(generator->mutex);
            std::snprintf(generator->address_input, sizeof(generator->address_input), "%llX",
                static_cast<unsigned long long>(runtime));
            count = generator->instruction_count;
            wildcard = generator->auto_wildcard;
        }
        aob_generator::generate_from_address(context, runtime, count, wildcard);
        focus("view.memory.aob");
        return action_handler_result_t::completed();
    };
    if (context.view->export_pending.load(std::memory_order_acquire)) {
        unavailable("analysis.export.listing",
            "A disassembly listing export is already running");
    } else {
        menu.actions["analysis.export.listing"].invoke = [context]() {
            std::string error;
            return disasm_view::request_listing_export(context, &error)
                ? action_handler_result_t::completed()
                : action_handler_result_t::failed(error);
        };
        menu.actions["analysis.export.listing"].capability =
            capability_state_t::available();
    }
    const auto evidence_action = [context, instruction, address, bytes, text, name](bool agent) {
        std::string excerpt = address;
        if (!bytes.empty()) excerpt.append("  ").append(bytes);
        if (!text.empty()) excerpt.append("  ").append(text);
        if (!name.empty()) excerpt.append("  <").append(name).append(">");
        aida::automation_ui::evidence_envelope_t envelope;
        envelope.workspace_id = context.workspace->identity().binary_id().to_hex();
        envelope.source_view_id = "document.disassembly";
        envelope.source_kind = "instruction";
        envelope.entity_id = "instruction:" + std::to_string(instruction.id);
        envelope.display_label = name.empty() ? address : name;
        envelope.return_target = "address:" + address;
        envelope.excerpt = excerpt;
        envelope.address = disasm_view::runtime_address(context, instruction.address).value_or(
            instruction.address.value);
        envelope.revision = context.publication->analysis_revision;
        envelope.generation = context.publication->generation;
        envelope.snapshot_hash = context_generation(context);
        envelope.content_hash = analysis_bridge::disasm_evidence_hash(excerpt);
        const std::string evidence_id =
            aida::automation_ui::register_evidence(std::move(envelope));
        if (evidence_id.empty())
            return action_handler_result_t::failed(
                "The bounded evidence registry rejected this instruction snapshot");
        std::string error;
        const bool queued = agent
            ? aida::automation_ui::queue_evidence_for_agent(evidence_id, error)
            : aida::automation_ui::queue_evidence_for_chat(evidence_id, error);
        return queued ? action_handler_result_t::completed()
                      : action_handler_result_t::failed(error);
    };
    menu.actions["analysis.evidence.chat"].invoke = [evidence_action]() {
        return evidence_action(false);
    };
    menu.actions["analysis.evidence.agent"].invoke = [evidence_action]() {
        return evidence_action(true);
    };
    auto display = [&menu, context, format](const char* id, disasm_view::addr_format_t target) {
        action_slot_t slot;
        slot.check_state = format == target ? action_check_state_t::checked : action_check_state_t::unchecked;
        slot.invoke = [context, target]() {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            context.view->addr_format = target;
            context.view->ui_serial.fetch_add(1, std::memory_order_acq_rel);
            return action_handler_result_t::completed();
        };
        menu.actions.emplace(id, std::move(slot));
    };
    display("analysis.view.va", disasm_view::addr_format_t::va);
    display("analysis.view.rva", disasm_view::addr_format_t::rva);
    display("analysis.view.file_offset", disasm_view::addr_format_t::file_offset);
    menu.actions["analysis.view.bytes"] = {capability_state_t::available(), [context]() {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        context.view->show_bytes = !context.view->show_bytes;
        context.view->ui_serial.fetch_add(1, std::memory_order_acq_rel);
        return action_handler_result_t::completed();
    }, show_bytes ? action_check_state_t::checked : action_check_state_t::unchecked};
    menu.actions["analysis.view.full_line"] = {capability_state_t::available(), []() {
        editor_config::disasm_full_line_select = !editor_config::disasm_full_line_select;
        return action_handler_result_t::completed();
    }, editor_config::disasm_full_line_select ? action_check_state_t::checked : action_check_state_t::unchecked};
    return menu;
}

std::optional<aida::ui::analysis_context_menu::context_t> AidaDisasmView::makeMetadataMenu(
    std::size_t line_index)
{
    using aida::ui::action_handler_result_t;
    if (line_index >= metadata_lines_.size())
        return std::nullopt;
    std::string listing;
    for (const auto& item : metadata_lines_) {
        listing.append(item.text.toStdString());
        listing.push_back('\n');
    }
    const std::string selected_line = metadata_lines_[line_index].text.toStdString();
    std::uint64_t image_base = context_.image ? context_.image->image_base() : 0;
    {
        std::lock_guard<std::mutex> lock(context_.view->mutex);
        if (context_.view->display_image_base)
            image_base = *context_.view->display_image_base;
    }
    char identity_buffer[512]{};
    std::snprintf(identity_buffer, sizeof(identity_buffer), "%s  %s  image 0x%016llX  entry +0x%08llX",
        context_.workspace->identity().bin_name().c_str(),
        context_.image ? file_metadata_banner::machine_name(context_.image->machine()).c_str() : "unknown",
        static_cast<unsigned long long>(image_base),
        static_cast<unsigned long long>(context_.image ? context_.image->entry_rva() : 0));
    const std::string identity_line = identity_buffer;
    char address_buffer[32]{};
    std::snprintf(address_buffer, sizeof(address_buffer), "%016llX",
        static_cast<unsigned long long>(image_base));
    const std::string image_base_text = address_buffer;
    aida::ui::analysis_context_menu::context_t menu;
    menu.kind = aida::ui::analysis_context_menu::menu_kind_t::metadata;
    menu.entity_id = "metadata:" + std::to_string(image_base) + ":" +
        std::to_string(line_index);
    menu.generation = context_generation(context_);
    menu.live_generation = [context = context_]() { return context_generation(context); };
    const auto retained_line = line_index;
    menu.validate_identity = [this, retained_line]() {
        return banner_selected_all_ && banner_selected_line_ == retained_line
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The selected metadata row changed");
    };
    aida::ui::analysis_context_menu::action_slot_t copy_all;
    copy_all.invoke = [listing = std::move(listing)] {
        aida::qt::clipboard::set_text(QString::fromStdString(listing));
        return action_handler_result_t::completed();
    };
    menu.actions.emplace("analysis.copy.metadata", std::move(copy_all));
    aida::ui::analysis_context_menu::action_slot_t copy_line;
    copy_line.invoke = [identity_line] {
        aida::qt::clipboard::set_text(QString::fromStdString(identity_line));
        return action_handler_result_t::completed();
    };
    menu.actions.emplace("analysis.copy.metadata_line", std::move(copy_line));
    aida::ui::analysis_context_menu::action_slot_t copy_current_line;
    copy_current_line.invoke = [selected_line] {
        aida::qt::clipboard::set_text(QString::fromStdString(selected_line));
        return action_handler_result_t::completed();
    };
    menu.actions.emplace("analysis.copy.metadata_current_line", std::move(copy_current_line));
    aida::ui::analysis_context_menu::action_slot_t copy_address;
    copy_address.invoke = [image_base_text] {
        aida::qt::clipboard::set_text(QString::fromStdString(image_base_text));
        return action_handler_result_t::completed();
    };
    menu.actions.emplace("analysis.copy.metadata_address", std::move(copy_address));
    aida::ui::analysis_context_menu::action_slot_t select_all;
    select_all.invoke = [this] {
        banner_selected_all_ = true;
        viewport()->update();
        return action_handler_result_t::completed();
    };
    menu.actions.emplace("analysis.select.metadata_all", std::move(select_all));
    return menu;
}

}
