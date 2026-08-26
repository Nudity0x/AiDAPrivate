#include "qt/debugger/source_pane.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QSplitter>
#include <QStringList>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "core/debugger/debugger_view.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/toast_notification.hpp"

#include "qt/bridge/menu_bridge.hpp"
#include "qt/debugger/debugger_action_bridge.hpp"
#include "qt/debugger/debugger_models.hpp"
#include "qt/debugger/disasm_slice_widget.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::debugger {

namespace {

// Excerpt rows for the SOURCE CONTEXT strip (line number, text, current flag).
class SourceExcerptModel : public DebuggerTableModelBase {
public:
    explicit SourceExcerptModel(QObject* parent = nullptr)
        : DebuggerTableModelBase({{QStringLiteral("Line"), 70, false},
                                  {QStringLiteral("Source"), 0, true}},
            parent) {}

    void applyExcerpt(
        const std::vector<source_debug_service::source_excerpt_line_t>& lines) {
        beginResetModel();
        lines_ = lines;
        endResetModel();
    }

    quint64 rowId(int row) const override {
        return (row >= 0 && row < static_cast<int>(lines_.size()))
            ? lines_[static_cast<std::size_t>(row)].line : 0;
    }

protected:
    int implRowCount() const override {
        return static_cast<int>(lines_.size());
    }

    QVariant cellData(int row, int column, int role) const override {
        if (row < 0 || row >= static_cast<int>(lines_.size()))
            return QVariant();
        const auto& t = theme::tokens();
        const auto& line = lines_[static_cast<std::size_t>(row)];
        if (role == Qt::DisplayRole) {
            if (column == 0)
                return QString::asprintf("%5u", line.line);
            return QString::fromStdString(line.text);
        }
        if (role == Qt::ForegroundRole) {
            if (column == 0)
                return line.current ? t.accent_hover : t.text_dim;
            return t.text_primary;
        }
        if (role == Qt::FontRole)
            return theme::fonts::codeRegular();
        if (role == Qt::BackgroundRole && line.current)
            return widgets::with_alpha(t.accent_glow, 0.36);
        if (role == TooltipTextRole && column == 1)
            return QString::fromStdString(line.text);
        return QVariant();
    }

private:
    std::vector<source_debug_service::source_excerpt_line_t> lines_;
};

// Persistent source-breakpoint definitions (State/File:line/Locations/
// Address/Detail).
class SourceDefinitionsModel : public DebuggerTableModelBase {
public:
    explicit SourceDefinitionsModel(QObject* parent = nullptr)
        : DebuggerTableModelBase({{QStringLiteral("State"), 76, false},
                                  {QStringLiteral("File : line"), 0, true},
                                  {QStringLiteral("Locations"), 74, false},
                                  {QStringLiteral("Address"), 150, false},
                                  {QStringLiteral("Detail"), 260, false}},
            parent) {}

    void applyDefinitions(
        std::vector<source_debug_service::definition_t> definitions,
        std::uint64_t generation) {
        beginResetModel();
        definitions_ = std::move(definitions);
        generation_ = generation;
        endResetModel();
    }

    const source_debug_service::definition_t* definitionAt(int row) const {
        return (row >= 0 && row < static_cast<int>(definitions_.size()))
            ? &definitions_[static_cast<std::size_t>(row)] : nullptr;
    }

    quint64 rowId(int row) const override {
        quint64 hash = 1469598103934665603ULL;
        const auto* definition = definitionAt(row);
        if (!definition)
            return 0;
        for (const char c : definition->id) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

protected:
    int implRowCount() const override {
        return static_cast<int>(definitions_.size());
    }

    QVariant cellData(int row, int column, int role) const override {
        const auto* definition = definitionAt(row);
        if (!definition)
            return QVariant();
        const auto& t = theme::tokens();
        if (role == Qt::DisplayRole) {
            switch (column) {
                case 0:
                    return QString::fromLatin1(
                        source_debug_service::binding_state_label(
                            definition->state));
                case 1:
                    return QString::fromStdString(definition->file_path + ":" +
                        std::to_string(definition->line));
                case 2:
                    return QString::number(static_cast<qulonglong>(
                        definition->locations.size()));
                case 3:
                    return definition->locations.empty()
                        ? QVariant(QStringLiteral("unresolved"))
                        : QVariant(QString::asprintf("0x%016llX",
                            static_cast<unsigned long long>(
                                definition->locations.front().address)));
                case 4:
                    return QString::fromStdString(definition->detail);
                default:
                    return QVariant();
            }
        }
        if (role == Qt::ForegroundRole) {
            if (column == 0) {
                if (definition->state ==
                    source_debug_service::binding_state_t::bound)
                    return t.success;
                if (definition->state ==
                        source_debug_service::binding_state_t::error ||
                    definition->state ==
                        source_debug_service::binding_state_t::stale)
                    return t.error;
                return t.warning;
            }
            if (column == 1) return t.text_primary;
            if (column == 3) return t.text_address;
            return t.text_secondary;
        }
        if (role == Qt::FontRole && column == 3)
            return theme::fonts::codeRegular();
        if (role == TooltipTextRole) {
            QStringList lines;
            lines << QString::fromStdString(definition->file_path) +
                QStringLiteral(":") + QString::number(definition->line);
            if (!definition->detail.empty())
                lines << QString::fromStdString(definition->detail);
            return lines.join(QStringLiteral("\n"));
        }
        return QVariant();
    }

private:
    std::vector<source_debug_service::definition_t> definitions_;
    std::uint64_t generation_ = 0;
};

void applyColumnSpec(QTableView* view, const DebuggerTableModelBase* model) {
    for (int section = 0;
         section < static_cast<int>(model->columns().size()); ++section) {
        const auto& column =
            model->columns()[static_cast<std::size_t>(section)];
        if (column.stretch)
            view->horizontalHeader()->setSectionResizeMode(section,
                QHeaderView::Stretch);
        else if (column.width > 0) {
            view->horizontalHeader()->setSectionResizeMode(section,
                QHeaderView::Interactive);
            view->setColumnWidth(section, column.width);
        }
    }
}

}

SourcePane::SourcePane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.source"));
    setOwnerViewId("view.debug.source");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral(
            "Attach to a process to inspect source context at the stopped "
            "location."));
    setEmptyContentText(QStringLiteral("No stopped source location"),
        QStringLiteral(
            "Pause the target on a symbol-resolved address to inspect the "
            "source context."));
    setLoadingText(QStringLiteral("Source operation running"),
        QStringLiteral(
            "A source-debug operation is running; the excerpt and definitions "
            "update when it completes."));
    setErrorText(QStringLiteral("Source snapshot failed"),
        QStringLiteral(
            "The source-debug service reported a failure; the pane retries "
            "automatically."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);
    open_source_button_ = new widgets::AidaButton(QStringLiteral("Open Source"),
        bar);
    open_source_button_->setObjectName(
        QStringLiteral("aida.view.debug.source.open_source"));
    open_source_button_->setKind(widgets::AidaButton::Kind::Primary);
    open_source_button_->setToolTip(QStringLiteral(
        "Open the stopped source location in the editor"));
    connect(open_source_button_, &widgets::AidaButton::clicked, this,
        &SourcePane::openCurrentSource);
    bar_layout->addWidget(open_source_button_);
    open_asm_button_ = new widgets::AidaButton(QStringLiteral("Open Assembly"),
        bar);
    open_asm_button_->setObjectName(
        QStringLiteral("aida.view.debug.source.open_asm"));
    open_asm_button_->setKind(widgets::AidaButton::Kind::Secondary);
    open_asm_button_->setToolTip(QStringLiteral(
        "Synchronize the disassembly view to the stopped address"));
    connect(open_asm_button_, &widgets::AidaButton::clicked, this,
        &SourcePane::openCurrentDisassembly);
    bar_layout->addWidget(open_asm_button_);
    toggle_bp_button_ = new widgets::AidaButton(QStringLiteral("Toggle BP"),
        bar);
    toggle_bp_button_->setObjectName(
        QStringLiteral("aida.view.debug.source.toggle_bp"));
    toggle_bp_button_->setKind(widgets::AidaButton::Kind::Secondary);
    toggle_bp_button_->setToolTip(QStringLiteral(
        "Toggle a source breakpoint at the current line (F9)"));
    connect(toggle_bp_button_, &widgets::AidaButton::clicked, this,
        &SourcePane::toggleCurrentBreakpoint);
    bar_layout->addWidget(toggle_bp_button_);
    rebind_button_ = new widgets::AidaButton(QStringLiteral("Rebind"), bar);
    rebind_button_->setObjectName(QStringLiteral("aida.view.debug.source.rebind"));
    rebind_button_->setKind(widgets::AidaButton::Kind::Ghost);
    rebind_button_->setToolTip(QStringLiteral(
        "Re-resolve every persistent source breakpoint against the PDBs"));
    connect(rebind_button_, &widgets::AidaButton::clicked, this,
        &SourcePane::rebindAll);
    bar_layout->addWidget(rebind_button_);
    bar_layout->addStretch(1);
    setToolBar(bar);

    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(tokens.panel.padding_compact,
        tokens.spacing.xs, tokens.panel.padding_compact, tokens.spacing.xs);
    body_layout->setSpacing(tokens.spacing.sm);

    auto* status = new QWidget(body);
    auto* status_layout = new QVBoxLayout(status);
    status_layout->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
        tokens.spacing.sm, tokens.spacing.xs);
    status_layout->setSpacing(tokens.spacing.xxs);
    status->setObjectName(QStringLiteral("aida.view.debug.source.status"));
    location_label_ = new QLabel(status);
    location_label_->setFont(theme::fonts::codeRegular());
    location_detail_ = new QLabel(status);
    location_detail_->setFont(theme::fonts::caption());
    status_layout->addWidget(location_label_);
    status_layout->addWidget(location_detail_);
    body_layout->addWidget(status, 0);

    auto* splitter = new QSplitter(Qt::Horizontal, body);
    splitter->setChildrenCollapsible(false);
    excerpt_model_ = new SourceExcerptModel(this);
    excerpt_view_ = new QTableView(splitter);
    excerpt_view_->setObjectName(QStringLiteral("aida.view.debug.source.excerpt"));
    excerpt_view_->verticalHeader()->setVisible(false);
    excerpt_view_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    const auto& mono_grid = theme::fonts::monoGrid();
    excerpt_view_->verticalHeader()->setDefaultSectionSize(
        mono_grid.valid && mono_grid.line_h > 0.0
            ? qRound(mono_grid.line_h)
            : tokens.table.compact_row_h);
    excerpt_view_->setShowGrid(false);
    excerpt_view_->setSelectionMode(QAbstractItemView::NoSelection);
    excerpt_view_->setModel(excerpt_model_);
    applyColumnSpec(excerpt_view_, excerpt_model_);
    splitter->addWidget(excerpt_view_);
    disasm_widget_ = new DisasmSliceWidget(splitter);
    disasm_widget_->setObjectName(QStringLiteral("aida.view.debug.source.disasm"));
    splitter->addWidget(disasm_widget_);
    body_layout->addWidget(splitter, 1);

    auto* definitions_header = new QLabel(QStringLiteral(
        "PERSISTENT SOURCE BREAKPOINTS"), body);
    definitions_header->setObjectName(
        QStringLiteral("aida.view.debug.source.definitions_header"));
    definitions_header->setFont(theme::fonts::caption());
    definitions_header->setProperty("aidaVariant", QStringLiteral("secondary"));
    body_layout->addWidget(definitions_header, 0);
    definitions_model_ = new SourceDefinitionsModel(this);
    definitions_view_ = new QTableView(body);
    definitions_view_->setObjectName(
        QStringLiteral("aida.view.debug.source.definitions"));
    definitions_view_->verticalHeader()->setVisible(false);
    definitions_view_->verticalHeader()->setSectionResizeMode(
        QHeaderView::Fixed);
    definitions_view_->verticalHeader()->setDefaultSectionSize(
        tokens.table.compact_row_h);
    definitions_view_->setShowGrid(false);
    definitions_view_->setAlternatingRowColors(true);
    definitions_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    definitions_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    definitions_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    definitions_view_->setModel(definitions_model_);
    applyColumnSpec(definitions_view_, definitions_model_);
    definitions_view_->installEventFilter(this);
    body_layout->addWidget(definitions_view_, 1);
    setContent(body);

    connect(definitions_view_, &QAbstractItemView::activated, this,
        [this](const QModelIndex& index) {
            const auto* definition =
                static_cast<SourceDefinitionsModel*>(definitions_model_)
                    ->definitionAt(index.row());
            if (!definition)
                return;
            std::string error;
            report(source_debug_service::request_open_source(
                definition->file_path, definition->line, &error), error,
                "Opened the source breakpoint location");
        });
    connect(definitions_view_, &QTableView::customContextMenuRequested, this,
        [this](const QPoint& pos) { openDefinitionMenu(pos); });

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(250);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, &SourcePane::poll);
}

void SourcePane::onShown() {
    poll_timer_->start();
    poll();
}

void SourcePane::onHidden() {
    poll_timer_->stop();
}

void SourcePane::onSessionTick() {
    updateOverlayState();
}

bool SourcePane::hasContentRows() const {
    return snapshot_had_current_ ||
        (definitions_model_ && definitions_model_->rowCount() > 0);
}

bool SourcePane::isContentLoading() const {
    return snapshot_operation_pending_;
}

bool SourcePane::contentError(QString* detail) const {
    if (snapshot_error_.isEmpty())
        return false;
    if (detail)
        *detail = snapshot_error_;
    return true;
}

void SourcePane::report(bool accepted, const std::string& error,
                        const char* accepted_text) {
    if (accepted) {
        toast_notification::push(accepted_text,
            toast_notification::toast_type_t::info);
    } else {
        toast_notification::push(error.empty()
                ? "The source-debug action was rejected" : error,
            toast_notification::toast_type_t::error);
    }
}

void SourcePane::poll() {
    const auto published = source_debug_service::snapshot();
    if (!published)
        return;
    snapshot_had_current_ = published->current.valid;
    snapshot_operation_pending_ = published->operation_pending;
    snapshot_error_ = QString::fromStdString(published->error);
    const bool has_current = published->current.valid;
    const bool has_source = has_current &&
        (published->current.source_state ==
                source_debug_service::source_state_t::available ||
            (published->current.source_state ==
                    source_debug_service::source_state_t::truncated &&
                !published->current.excerpt.empty()));
    const bool operation_pending = published->operation_pending;

    open_source_button_->setEnabled(has_source);
    open_asm_button_->setEnabled(has_current);
    toggle_bp_button_->setEnabled(has_current && !operation_pending);
    rebind_button_->setEnabled(!operation_pending);

    QString location_text = QStringLiteral("No stopped source location");
    QString detail = published->error.empty()
        ? QString::fromStdString(published->current.detail)
        : QString::fromStdString(published->error);
    if (has_current) {
        location_text = QString::fromStdString(published->current.file_path +
            ":" + std::to_string(published->current.line) + "  " +
            published->current.module_name);
        detail = QString::fromStdString(published->current.detail);
    }
    if (operation_pending)
        detail = published->operation_label.empty()
            ? QStringLiteral("Source-debug operation is running")
            : QString::fromStdString(published->operation_label);
    const QFontMetricsF location_fm(location_label_->font());
    const int location_w = location_label_->width();
    location_label_->setText(location_w > 0
        ? location_fm.elidedText(location_text, Qt::ElideMiddle, location_w)
        : location_text);
    location_label_->setToolTip(location_text);
    const QFontMetricsF detail_fm(location_detail_->font());
    const int detail_w = location_detail_->width();
    location_detail_->setText(detail_w > 0
        ? detail_fm.elidedText(detail, Qt::ElideRight, detail_w)
        : detail);
    location_detail_->setToolTip(detail);

    if (published->generation != last_generation_) {
        last_generation_ = published->generation;
        static_cast<SourceExcerptModel*>(excerpt_model_)->applyExcerpt(
            published->current.excerpt);
        static_cast<SourceDefinitionsModel*>(definitions_model_)
            ->applyDefinitions(published->definitions, published->generation);
        disasm_widget_->setRip(has_current ? published->current.address : 0);
    }
    disasm_widget_->tick();
}

void SourcePane::openCurrentSource() {
    std::string error;
    report(source_debug_service::request_open_current_source(&error), error,
        "Opened the exact stopped source location");
}

void SourcePane::openCurrentDisassembly() {
    std::string error;
    report(source_debug_service::request_open_current_disassembly(&error),
        error, "Synchronized the disassembly view");
}

void SourcePane::toggleCurrentBreakpoint() {
    const auto published = source_debug_service::snapshot();
    if (!published || !published->current.valid)
        return;
    std::string error;
    report(source_debug_service::request_toggle(published->current.file_path,
        published->current.line, &error), error,
        "Queued the source-breakpoint update");
}

void SourcePane::rebindAll() {
    std::string error;
    report(source_debug_service::request_rebind(&error), error,
        "Queued exact PDB source rebinding");
}

bool SourcePane::eventFilter(QObject* watched, QEvent* event) {
    if (watched == definitions_view_ &&
        event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Menu ||
            (key->key() == Qt::Key_F10 &&
                key->modifiers().testFlag(Qt::ShiftModifier))) {
            const auto current = definitions_view_->currentIndex();
            if (current.isValid())
                openDefinitionMenu(
                    definitions_view_->visualRect(current).center(),
                    key->key() == Qt::Key_Menu
                        ? aida::ui::context_menu_open_origin_t::menu_key
                        : aida::ui::context_menu_open_origin_t::shift_f10);
            return true;
        }
    }
    return DebuggerPaneBase::eventFilter(watched, event);
}

void SourcePane::openDefinitionMenu(const QPoint& pos,
                                    aida::ui::context_menu_open_origin_t origin) {
    const QModelIndex index = definitions_view_->indexAt(pos);
    if (!index.isValid())
        return;
    auto* definitions =
        static_cast<SourceDefinitionsModel*>(definitions_model_);
    const auto* definition = definitions->definitionAt(index.row());
    const auto published = source_debug_service::snapshot();
    if (!definition || !published)
        return;
    auto retained = debugger_view::build_source_breakpoint_actions(*definition,
        published->generation);
    if (retained.actions.empty())
        return;
    auto* menus = DebuggerActionBridge::instance().menus();
    if (menus)
        menus->show_retained_entity_menu(retained, origin,
            definitions_view_->viewport()->mapToGlobal(pos),
            definitions_view_);
}

}
