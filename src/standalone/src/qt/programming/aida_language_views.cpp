#include "qt/programming/aida_language_views.hpp"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <utility>

#include <QAbstractItemView>
#include <QContextMenuEvent>

#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/programming/aida_output_pane.hpp"
#include "qt/programming/aida_task_center_view.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_headers.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::programming {
namespace {

namespace language = aida::editor::language_service;

QPointer<AidaLanguageServiceBridge> g_language_bridge;

bool keyboard_context_menu_event(QObject* watched, QEvent* event, QAbstractItemView* view,
        QModelIndex* index, QPoint* global_pos) {
    if (!view)
        return false;
    if (event->type() != QEvent::ContextMenu)
        return false;
    auto* context_event = static_cast<QContextMenuEvent*>(event);
    if (context_event->reason() != QContextMenuEvent::Keyboard)
        return false;
    if (watched != view && watched != view->viewport())
        return false;
    *index = view->currentIndex();
    *global_pos = index->isValid()
        ? view->viewport()->mapToGlobal(view->visualRect(*index).center())
        : view->viewport()->mapToGlobal(view->viewport()->rect().center());
    return true;
}

QColor severity_color(const std::string& severity) {
    const auto& tokens = theme::tokens();
    std::string lowered = severity;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lowered.find("error") != std::string::npos ||
        lowered.find("fatal") != std::string::npos ||
        lowered.find("security") != std::string::npos)
        return tokens.error;
    if (lowered.find("warn") != std::string::npos)
        return tokens.warning;
    if (lowered.find("info") != std::string::npos ||
        lowered.find("note") != std::string::npos)
        return tokens.info;
    if (lowered.find("hint") != std::string::npos ||
        lowered.find("debug") != std::string::npos)
        return tokens.text_dim;
    return tokens.text_primary;
}

QColor symbol_kind_color(const std::string& kind) {
    const auto& tokens = theme::tokens();
    std::string lowered = kind;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lowered.find("func") != std::string::npos || lowered.find("method") != std::string::npos ||
        lowered.find("ctor") != std::string::npos || lowered.find("constructor") != std::string::npos)
        return tokens.syn_function;
    if (lowered.find("class") != std::string::npos || lowered.find("struct") != std::string::npos ||
        lowered.find("type") != std::string::npos || lowered.find("enum") != std::string::npos ||
        lowered.find("interface") != std::string::npos || lowered.find("union") != std::string::npos)
        return tokens.syn_type;
    if (lowered.find("const") != std::string::npos || lowered.find("macro") != std::string::npos ||
        lowered.find("define") != std::string::npos)
        return tokens.syn_number;
    if (lowered.find("namespace") != std::string::npos || lowered.find("module") != std::string::npos ||
        lowered.find("package") != std::string::npos)
        return tokens.syn_preprocessor;
    if (lowered.find("var") != std::string::npos || lowered.find("field") != std::string::npos ||
        lowered.find("member") != std::string::npos || lowered.find("param") != std::string::npos)
        return tokens.syn_identifier;
    if (lowered.find("string") != std::string::npos)
        return tokens.syn_string;
    if (lowered.find("keyword") != std::string::npos || lowered.find("operator") != std::string::npos)
        return tokens.syn_keyword;
    return tokens.text_secondary;
}

bool contains_folded(const std::string& text, const std::string& query) {
    if (query.empty())
        return true;
    return std::search(text.begin(), text.end(), query.begin(), query.end(),
        [](unsigned char left, unsigned char right) {
            return std::tolower(left) == std::tolower(right);
        }) != text.end();
}

QString leaf_name(const std::string& path) {
    return QString::fromStdString(std::filesystem::path(path).filename().string());
}

void open_result_context(const language::location_t& location, std::string label,
        const language::query_result_t& result,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos,
        QWidget* parent) {
    namespace app = aida::ui::application_ui;
    app::open_programming_result_context_menu(location, std::move(label),
        result.provider_name, result.kind, result.request_id, result.request_generation,
        result.provider_generation, result.index_generation, origin);
    const bool outline = result.kind == language::capability_kind_t::document_symbols ||
        result.kind == language::capability_kind_t::workspace_symbols;
    documents::show_context_menu(
        aida::ui::stable_menu_id_t("menu.programming.language.result"),
        documents::make_menu_snapshot(
            aida::ui::stable_view_id_t(outline ? "view.programming.outline"
                                               : "view.programming.references"),
            aida::ui::stable_context_type_id_t("context.programming.language.result")),
        origin, global_pos, parent);
}

void hydrateActionButton(QPushButton* button, const char* action_id) {
    const auto presentation = aida::ui::application_ui::present_action(action_id);
    button->setVisible(presentation.visible);
    button->setEnabled(presentation.enabled);
    QString tooltip = presentation.description.empty()
        ? QString::fromStdString(presentation.label)
        : QString::fromStdString(presentation.description);
    if (!presentation.enabled && !presentation.disabled_reason.empty())
        tooltip = QString::fromStdString(presentation.disabled_reason);
    if (!presentation.shortcut.empty())
        tooltip += QStringLiteral(" (") + QString::fromStdString(presentation.shortcut) +
            QStringLiteral(")");
    button->setToolTip(tooltip);
}

} 

AidaLanguageServiceBridge& AidaLanguageServiceBridge::instance() {
    if (!g_language_bridge)
        g_language_bridge = new AidaLanguageServiceBridge();
    return *g_language_bridge;
}

bool AidaLanguageServiceBridge::exists() noexcept {
    return g_language_bridge != nullptr;
}

AidaLanguageServiceBridge::AidaLanguageServiceBridge(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(250);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &AidaLanguageServiceBridge::onTick);
    timer_->start();
}

void AidaLanguageServiceBridge::install() {
}

void AidaLanguageServiceBridge::shutdown() {
    if (timer_)
        timer_->stop();
}

language::query_snapshot_t AidaLanguageServiceBridge::latestResult(
        std::initializer_list<language::capability_kind_t> kinds) const {
    language::query_snapshot_t latest;
    for (const auto kind : kinds) {
        const auto candidate = language::result(kind);
        if (candidate && (!latest || candidate->request_id > latest->request_id))
            latest = candidate;
    }
    return latest;
}

language::query_snapshot_t AidaLanguageServiceBridge::result(
        language::capability_kind_t kind) const {
    return language::result(kind);
}

void AidaLanguageServiceBridge::onTick() {
    language::begin_frame();
    const auto publication = language::workspace_index_snapshot();
    const std::uint64_t index_generation = publication ? publication->generation : 0;
    const QString status = QString::fromStdString(language::workspace_index_status());
    if (index_generation != index_generation_ || status != index_status_) {
        index_generation_ = index_generation;
        index_snapshot_ = publication;
        index_status_ = status;
        Q_EMIT indexChanged();
    }
    const auto source = source_debug_service::snapshot();
    const std::uint64_t source_generation = source ? source->generation : 0;
    if (source_generation != source_debug_generation_) {
        source_debug_generation_ = source_generation;
        source_debug_ = source;
        Q_EMIT sourceDebugChanged();
    }
    for (int kind_index = 0; kind_index < 15; ++kind_index) {
        const auto kind = static_cast<language::capability_kind_t>(kind_index);
        const auto snapshot = language::result(kind);
        const std::uint64_t request_id = snapshot ? snapshot->request_id : 0;
        const std::uint64_t request_generation = snapshot ? snapshot->request_generation : 0;
        auto& slot = slots_[kind_index];
        if (slot.request_id != request_id || slot.request_generation != request_generation) {
            slot.request_id = request_id;
            slot.request_generation = request_generation;
            Q_EMIT resultChanged(kind_index);
        }
    }
}

AidaIndexStatusStrip::AidaIndexStatusStrip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.programming.index_status"));
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(theme::tokens().spacing.xxs);
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(QStringLiteral("Workspace Text Index"), this);
    title->setObjectName(QStringLiteral("aida.programming.index_status.title"));
    row->addWidget(title);
    status_ = new QLabel(this);
    row->addWidget(status_);
    stats_ = new QLabel(this);
    stats_->setObjectName(QStringLiteral("aida.programming.index_status.stats"));
    row->addWidget(stats_);
    row->addStretch(1);
    column->addLayout(row);
    auto* hint = new QLabel(QStringLiteral(
        "Published workspace files only; unsaved editor changes require Save and Rebuild."), this);
    hint->setObjectName(QStringLiteral("aida.programming.index_status.hint"));
    hint->setEnabled(false);
    column->addWidget(hint);
    refresh();
}

void AidaIndexStatusStrip::refresh() {
    if (!AidaLanguageServiceBridge::exists())
        return;
    const auto& bridge = AidaLanguageServiceBridge::instance();
    status_->setText(bridge.indexStatus());
    status_->setEnabled(false);
    const auto publication = bridge.indexSnapshot();
    if (publication) {
        const QString skipped = publication->skipped_files != 0
            ? QStringLiteral("  [skipped %1]").arg(publication->skipped_files) : QString();
        stats_->setText(QStringLiteral("Gen %1  %2 files  %3 MiB%4%5")
            .arg(publication->generation)
            .arg(publication->indexed_files)
            .arg(static_cast<double>(publication->indexed_bytes) / (1024.0 * 1024.0),
                0, 'f', 1)
            .arg(publication->truncated ? QStringLiteral("  [bounded]") : QString())
            .arg(skipped));
        stats_->setEnabled(false);
    } else {
        stats_->clear();
    }
}

AidaOutlineModel::AidaOutlineModel(QObject* parent) : QAbstractTableModel(parent) {
}

void AidaOutlineModel::setSnapshot(language::query_snapshot_t snapshot) {
    if (snapshot_.get() == snapshot.get())
        return;
    beginResetModel();
    snapshot_ = std::move(snapshot);
    rebuildVisible();
    endResetModel();
}

void AidaOutlineModel::setFilter(const QString& filter) {
    const std::string next = filter.toStdString();
    if (filter_ == next)
        return;
    beginResetModel();
    filter_ = next;
    rebuildVisible();
    endResetModel();
}

void AidaOutlineModel::rebuildVisible() {
    visible_.clear();
    if (!snapshot_)
        return;
    visible_.reserve(snapshot_->symbols.size());
    for (std::size_t index = 0; index < snapshot_->symbols.size(); ++index) {
        const auto& symbol = snapshot_->symbols[index];
        if (contains_folded(symbol.name, filter_) || contains_folded(symbol.kind, filter_))
            visible_.push_back(index);
    }
}

int AidaOutlineModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

int AidaOutlineModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::count);
}

const language::symbol_t* AidaOutlineModel::rowAt(int row) const noexcept {
    if (!snapshot_ || row < 0 || row >= static_cast<int>(visible_.size()))
        return nullptr;
    return &snapshot_->symbols[visible_[static_cast<std::size_t>(row)]];
}

QVariant AidaOutlineModel::data(const QModelIndex& index, int role) const {
    const auto* symbol = rowAt(index.row());
    if (!symbol)
        return {};
    if (role == Qt::DisplayRole) {
        switch (static_cast<Column>(index.column())) {
        case Column::symbol: return QString::fromStdString(symbol->name);
        case Column::kind: return QString::fromStdString(symbol->kind);
        case Column::line: return symbol->location.line;
        case Column::count: break;
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (static_cast<Column>(index.column())) {
        case Column::symbol:
            return QStringLiteral("%1\n%2  |  %3:%4")
                .arg(QString::fromStdString(symbol->name),
                    QString::fromStdString(symbol->kind))
                .arg(QString::fromStdString(symbol->location.file_path))
                .arg(symbol->location.line);
        case Column::kind: return QString::fromStdString(symbol->kind);
        case Column::line:
            return QStringLiteral("%1:%2")
                .arg(QString::fromStdString(symbol->location.file_path))
                .arg(symbol->location.line);
        case Column::count: break;
        }
        return {};
    }
    if (role == Qt::ForegroundRole &&
        static_cast<Column>(index.column()) == Column::kind)
        return symbol_kind_color(symbol->kind);
    if (role == Qt::UserRole)
        return static_cast<qulonglong>(index.row());
    return {};
}

void AidaOutlineModel::multiData(const QModelIndex& index,
                                 QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

QVariant AidaOutlineModel::headerData(int section, Qt::Orientation orientation,
                                      int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (static_cast<Column>(section)) {
    case Column::symbol: return QStringLiteral("Symbol");
    case Column::kind: return QStringLiteral("Kind");
    case Column::line: return QStringLiteral("Line");
    case Column::count: break;
    }
    return {};
}

AidaOutlineView::AidaOutlineView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.programming.outline"));
    const auto& tokens = theme::tokens();
    stack_ = new QStackedLayout(this);
    stack_->setContentsMargins(0, 0, 0, 0);
    content_ = new QWidget(this);
    auto* column = new QVBoxLayout(content_);
    column->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
        tokens.spacing.sm, tokens.spacing.xs);
    column->setSpacing(tokens.spacing.xs);

    header_ = new widgets::AidaViewHeader(QStringLiteral("Programming Outline"),
        QStringLiteral("Programming"), content_);
    column->addWidget(header_);
    strip_ = new AidaIndexStatusStrip(content_);
    column->addWidget(strip_);
    auto* controls = new QHBoxLayout();
    rebuild_button_ = new QPushButton(QStringLiteral("Rebuild"), content_);
    rebuild_button_->setObjectName(QStringLiteral("aida.view.programming.outline.rebuild"));
    connect(rebuild_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(aida::ui::application_ui::execute_action(
            "programming.index.rebuild", aida::ui::action_invocation_source_t::toolbar));
    });
    controls->addWidget(rebuild_button_);
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), content_);
    cancel_button_->setObjectName(QStringLiteral("aida.view.programming.outline.cancel"));
    connect(cancel_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(aida::ui::application_ui::execute_action(
            "programming.index.cancel", aida::ui::action_invocation_source_t::toolbar));
    });
    controls->addWidget(cancel_button_);
    filter_edit_ = new QLineEdit(content_);
    filter_edit_->setObjectName(QStringLiteral("aida.view.programming.outline.filter"));
    filter_edit_->setPlaceholderText(QStringLiteral("Filter symbols..."));
    filter_edit_->setClearButtonEnabled(true);
    controls->addWidget(filter_edit_, 1);
    column->addLayout(controls);
    truncated_label_ = new QLabel(QStringLiteral(
        "Bounded result: additional symbols may exist outside this publication."), content_);
    truncated_label_->setObjectName(QStringLiteral("aida.view.programming.outline.truncated"));
    truncated_label_->setVisible(false);
    column->addWidget(truncated_label_);

    model_ = new AidaOutlineModel(this);
    table_ = new QTableView(content_);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.view.programming.outline.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(theme::tokens().table.row_h);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->installEventFilter(this);
    table_->viewport()->installEventFilter(this);
    auto* header = table_->horizontalHeader();
    const int cell_pad = theme::tokens().table.cell_pad_x;
    const int char_w = header->fontMetrics().averageCharWidth();
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::Fixed);
    header->resizeSection(1, char_w * 12 + cell_pad * 2);
    header->setSectionResizeMode(2, QHeaderView::Fixed);
    header->resizeSection(2, char_w * 8 + cell_pad * 2);
    column->addWidget(table_, 1);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.view.programming.outline.state"));
    stack_->addWidget(content_);
    stack_->addWidget(state_view_);

    auto& bridge = AidaLanguageServiceBridge::instance();
    connect(&bridge, &AidaLanguageServiceBridge::resultChanged,
            this, &AidaOutlineView::onResultChanged);
    connect(&bridge, &AidaLanguageServiceBridge::indexChanged, this, [this] {
        strip_->refresh();
        refreshPresentation();
    });
    connect(filter_edit_, &QLineEdit::textChanged, model_, &AidaOutlineModel::setFilter);
    connect(table_, &QTableView::activated, this, &AidaOutlineView::activateRow);
    connect(table_, &QTableView::customContextMenuRequested, this, [this](const QPoint& pos) {
        openRowContext(table_->indexAt(pos), aida::ui::context_menu_open_origin_t::pointer,
            table_->viewport()->mapToGlobal(pos));
    });
    refreshPresentation();
}

bool AidaOutlineView::eventFilter(QObject* watched, QEvent* event) {
    QModelIndex index;
    QPoint global_pos;
    if (keyboard_context_menu_event(watched, event, table_, &index, &global_pos)) {
        openRowContext(index, aida::ui::context_menu_open_origin_t::menu_key, global_pos);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void AidaOutlineView::onResultChanged(int kind) {
    if (kind != static_cast<int>(language::capability_kind_t::document_symbols) &&
        kind != static_cast<int>(language::capability_kind_t::workspace_symbols))
        return;
    refreshPresentation();
}

void AidaOutlineView::refreshPresentation() {
    auto& bridge = AidaLanguageServiceBridge::instance();
    hydrateActionButton(rebuild_button_, "programming.index.rebuild");
    hydrateActionButton(cancel_button_, "programming.index.cancel");
    const auto document = language::active_document_context();
    auto snapshot = bridge.latestResult({language::capability_kind_t::document_symbols,
        language::capability_kind_t::workspace_symbols});
    if (snapshot && snapshot->kind == language::capability_kind_t::document_symbols &&
        (snapshot->document_id != document.document_id ||
         snapshot->document_path != document.file_path)) {
        auto unavailable = std::make_shared<language::query_result_t>();
        unavailable->state = language::result_state_t::unavailable;
        unavailable->kind = language::capability_kind_t::document_symbols;
        unavailable->document_id = document.document_id;
        unavailable->document_revision = document.revision;
        unavailable->document_path = document.file_path;
        const auto capability = language::capability(
            language::capability_kind_t::document_symbols, document);
        unavailable->status = capability.available
            ? "The Programming Outline is waiting for the active document query"
            : capability.reason;
        snapshot = std::move(unavailable);
    }
    const QString breadcrumb = document.file_path.empty()
        ? QStringLiteral("Programming / Plain Text")
        : QStringLiteral("Programming / ") + leaf_name(document.file_path);
    header_->setSubtitle(breadcrumb);
    header_->setStatus(snapshot && snapshot->state == language::result_state_t::ready
        ? widgets::AidaSemantic::Success : widgets::AidaSemantic::Neutral);
    shown_ = snapshot;
    if (!snapshot || snapshot->state != language::result_state_t::ready) {
        if (!snapshot) {
            state_view_->setState(widgets::AidaStateView::State::Empty);
            state_view_->setTitle(QStringLiteral("No document symbols"));
            state_view_->setMessage(QStringLiteral(
                "No provider query has been published for this view."));
        } else {
            switch (snapshot->state) {
            case language::result_state_t::loading:
                state_view_->setState(widgets::AidaStateView::State::Loading);
                state_view_->setTitle(QStringLiteral("Querying language provider"));
                break;
            case language::result_state_t::cancelled:
                state_view_->setState(widgets::AidaStateView::State::Empty);
                state_view_->setTitle(QStringLiteral("Query cancelled"));
                break;
            case language::result_state_t::error:
                state_view_->setState(widgets::AidaStateView::State::Error);
                state_view_->setTitle(QStringLiteral("Language provider failed"));
                break;
            case language::result_state_t::unavailable:
                state_view_->setState(widgets::AidaStateView::State::Empty);
                state_view_->setTitle(QStringLiteral("Capability unavailable"));
                break;
            default:
                state_view_->setState(widgets::AidaStateView::State::Empty);
                state_view_->setTitle(QStringLiteral("No document symbols"));
                break;
            }
            state_view_->setMessage(QString::fromStdString(snapshot->status));
        }
        stack_->setCurrentWidget(state_view_);
        return;
    }
    stack_->setCurrentWidget(content_);
    truncated_label_->setVisible(snapshot->truncated);
    model_->setSnapshot(snapshot);
}

void AidaOutlineView::activateRow(const QModelIndex& index) {
    const auto* symbol = model_->rowAt(index.row());
    if (symbol)
        static_cast<void>(language::open_location(symbol->location));
}

void AidaOutlineView::openRowContext(const QModelIndex& index,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos) {
    const auto* symbol = model_->rowAt(index.row());
    if (!symbol || !shown_)
        return;
    open_result_context(symbol->location, symbol->name, *shown_, origin, global_pos, this);
}

AidaReferencesModel::AidaReferencesModel(QObject* parent) : QAbstractTableModel(parent) {
}

void AidaReferencesModel::setSnapshot(language::query_snapshot_t snapshot) {
    if (snapshot_.get() == snapshot.get())
        return;
    beginResetModel();
    snapshot_ = std::move(snapshot);
    if (!snapshot_) {
        mode_ = Mode::none;
    } else if (!snapshot_->locations.empty()) mode_ = Mode::locations;
    else if (!snapshot_->symbols.empty()) mode_ = Mode::symbols;
    else if (!snapshot_->completions.empty()) mode_ = Mode::completions;
    else if (!snapshot_->information.empty()) mode_ = Mode::information;
    else if (!snapshot_->diagnostics.empty()) mode_ = Mode::diagnostics;
    else if (!snapshot_->proposed_edits.empty()) mode_ = Mode::proposed_edits;
    else if (!snapshot_->code_actions.empty()) mode_ = Mode::code_actions;
    else mode_ = Mode::none;
    rebuildVisible();
    endResetModel();
}

void AidaReferencesModel::setFilter(const QString& filter) {
    const std::string next = filter.toStdString();
    if (filter_ == next)
        return;
    beginResetModel();
    filter_ = next;
    rebuildVisible();
    endResetModel();
}

void AidaReferencesModel::rebuildVisible() {
    visible_.clear();
    if (!snapshot_)
        return;
    const auto include = [&](std::size_t index) {
        switch (mode_) {
        case Mode::locations: {
            const auto& location = snapshot_->locations[index];
            return contains_folded(location.file_path, filter_) ||
                contains_folded(location.preview, filter_);
        }
        case Mode::symbols: {
            const auto& symbol = snapshot_->symbols[index];
            return contains_folded(symbol.name, filter_) ||
                contains_folded(symbol.kind, filter_);
        }
        case Mode::completions: {
            const auto& item = snapshot_->completions[index];
            return contains_folded(item.label, filter_) ||
                contains_folded(item.detail, filter_) || contains_folded(item.kind, filter_);
        }
        case Mode::information: {
            const auto& item = snapshot_->information[index];
            return contains_folded(item.label, filter_) || contains_folded(item.content, filter_);
        }
        case Mode::diagnostics: {
            const auto& diagnostic = snapshot_->diagnostics[index];
            return contains_folded(diagnostic.message, filter_) ||
                contains_folded(diagnostic.location.file_path, filter_) ||
                contains_folded(diagnostic.severity, filter_) ||
                contains_folded(diagnostic.source, filter_);
        }
        case Mode::proposed_edits: {
            const auto& edit = snapshot_->proposed_edits[index];
            return contains_folded(edit.file_path, filter_) ||
                contains_folded(edit.expected_text, filter_) ||
                contains_folded(edit.replacement_text, filter_);
        }
        case Mode::code_actions: {
            const auto& action = snapshot_->code_actions[index];
            return contains_folded(action.title, filter_) ||
                contains_folded(action.kind, filter_) || contains_folded(action.detail, filter_);
        }
        case Mode::none: return false;
        }
        return false;
    };
    std::size_t count = 0;
    switch (mode_) {
    case Mode::locations: count = snapshot_->locations.size(); break;
    case Mode::symbols: count = snapshot_->symbols.size(); break;
    case Mode::completions: count = snapshot_->completions.size(); break;
    case Mode::information: count = snapshot_->information.size(); break;
    case Mode::diagnostics: count = snapshot_->diagnostics.size(); break;
    case Mode::proposed_edits: count = snapshot_->proposed_edits.size(); break;
    case Mode::code_actions: count = snapshot_->code_actions.size(); break;
    case Mode::none: count = 0; break;
    }
    visible_.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        if (include(index))
            visible_.push_back(index);
}

int AidaReferencesModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

int AidaReferencesModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    switch (mode_) {
    case Mode::locations: return 3;
    case Mode::symbols: return 3;
    case Mode::completions: return 3;
    case Mode::information: return 2;
    case Mode::diagnostics: return 4;
    case Mode::proposed_edits: return 4;
    case Mode::code_actions: return 3;
    case Mode::none: return 0;
    }
    return 0;
}

language::location_t AidaReferencesModel::locationAt(int row) const {
    language::location_t location;
    if (!snapshot_ || row < 0 || row >= static_cast<int>(visible_.size()))
        return location;
    const std::size_t source = visible_[static_cast<std::size_t>(row)];
    switch (mode_) {
    case Mode::locations: return snapshot_->locations[source];
    case Mode::symbols: return snapshot_->symbols[source].location;
    case Mode::diagnostics: return snapshot_->diagnostics[source].location;
    case Mode::proposed_edits: {
        const auto& edit = snapshot_->proposed_edits[source];
        location.root_path = snapshot_->root_path;
        location.file_path = edit.file_path;
        location.line = edit.range.start.line;
        location.column = edit.range.start.column;
        location.match_length = (std::max)(0, edit.range.end.column - edit.range.start.column);
        location.preview = edit.expected_text;
        return location;
    }
    default:
        return location;
    }
}

QVariant AidaReferencesModel::data(const QModelIndex& index, int role) const {
    if (!snapshot_ || index.row() < 0 || index.row() >= static_cast<int>(visible_.size()))
        return {};
    const std::size_t source = visible_[static_cast<std::size_t>(index.row())];
    const int column = index.column();
    if (role == Qt::ToolTipRole) {
        switch (mode_) {
        case Mode::locations: {
            const auto& location = snapshot_->locations[source];
            if (column == 0)
                return QString::fromStdString(location.file_path);
            if (column == 2 && !location.preview.empty())
                return QString::fromStdString(location.preview);
            return {};
        }
        case Mode::symbols: {
            const auto& symbol = snapshot_->symbols[source];
            if (column == 0)
                return QStringLiteral("%1\n%2  |  %3:%4")
                    .arg(QString::fromStdString(symbol.name),
                        QString::fromStdString(symbol.kind))
                    .arg(QString::fromStdString(symbol.location.file_path))
                    .arg(symbol.location.line);
            return {};
        }
        case Mode::completions: {
            const auto& item = snapshot_->completions[source];
            if (column == 0 && !item.detail.empty())
                return QStringLiteral("%1\n%2").arg(QString::fromStdString(item.label),
                    QString::fromStdString(item.detail));
            return {};
        }
        case Mode::information: {
            const auto& item = snapshot_->information[source];
            if (column == 1 && !item.content.empty())
                return QString::fromStdString(item.content);
            return {};
        }
        case Mode::diagnostics: {
            const auto& diagnostic = snapshot_->diagnostics[source];
            if (column == 1)
                return QString::fromStdString(diagnostic.location.file_path);
            if (column == 3)
                return QStringLiteral("[%1] %2")
                    .arg(QString::fromStdString(diagnostic.severity),
                        QString::fromStdString(diagnostic.message));
            return {};
        }
        case Mode::proposed_edits: {
            const auto& edit = snapshot_->proposed_edits[source];
            if (column == 0)
                return QString::fromStdString(edit.file_path);
            if (column == 2)
                return QString::fromStdString(edit.expected_text);
            if (column == 3)
                return QString::fromStdString(edit.replacement_text);
            return {};
        }
        case Mode::code_actions: {
            const auto& action = snapshot_->code_actions[source];
            if (column == 0 && !action.detail.empty())
                return QStringLiteral("%1\n%2").arg(QString::fromStdString(action.title),
                    QString::fromStdString(action.detail));
            return {};
        }
        case Mode::none: return {};
        }
        return {};
    }
    if (role == Qt::ForegroundRole && mode_ == Mode::diagnostics && column == 0)
        return severity_color(snapshot_->diagnostics[source].severity);
    if (role != Qt::DisplayRole)
        return {};
    switch (mode_) {
    case Mode::locations: {
        const auto& location = snapshot_->locations[source];
        if (column == 0) return leaf_name(location.file_path);
        if (column == 1) return QStringLiteral("%1:%2").arg(location.line).arg(location.column);
        if (column == 2) return QString::fromStdString(location.preview);
        break;
    }
    case Mode::symbols: {
        const auto& symbol = snapshot_->symbols[source];
        if (column == 0) return QString::fromStdString(symbol.name);
        if (column == 1) return QString::fromStdString(symbol.kind);
        if (column == 2) return symbol.location.line;
        break;
    }
    case Mode::completions: {
        const auto& item = snapshot_->completions[source];
        if (column == 0) return QString::fromStdString(item.label);
        if (column == 1)
            return QString::fromStdString(item.kind) +
                (item.snippet ? QStringLiteral(" snippet") : QString());
        if (column == 2) return QString::fromStdString(item.detail);
        break;
    }
    case Mode::information: {
        const auto& item = snapshot_->information[source];
        if (column == 0) return QString::fromStdString(item.label);
        if (column == 1) return QString::fromStdString(item.content);
        break;
    }
    case Mode::diagnostics: {
        const auto& diagnostic = snapshot_->diagnostics[source];
        if (column == 0) return QString::fromStdString(diagnostic.severity);
        if (column == 1) return leaf_name(diagnostic.location.file_path);
        if (column == 2)
            return QStringLiteral("%1:%2").arg(diagnostic.location.line)
                .arg(diagnostic.location.column);
        if (column == 3) return QString::fromStdString(diagnostic.message);
        break;
    }
    case Mode::proposed_edits: {
        const auto& edit = snapshot_->proposed_edits[source];
        if (column == 0) return leaf_name(edit.file_path);
        if (column == 1)
            return QStringLiteral("%1:%2-%3:%4").arg(edit.range.start.line)
                .arg(edit.range.start.column).arg(edit.range.end.line).arg(edit.range.end.column);
        if (column == 2) return QString::fromStdString(edit.expected_text);
        if (column == 3) return QString::fromStdString(edit.replacement_text);
        break;
    }
    case Mode::code_actions: {
        const auto& action = snapshot_->code_actions[source];
        if (column == 0)
            return QString::fromStdString(action.title) +
                (action.preferred ? QStringLiteral("  preferred") : QString());
        if (column == 1) return QString::fromStdString(action.kind);
        if (column == 2)
            return QString::fromStdString(action.disabled_reason.empty()
                ? action.detail : action.disabled_reason);
        break;
    }
    case Mode::none: break;
    }
    return {};
}

void AidaReferencesModel::multiData(const QModelIndex& index,
                                    QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

QVariant AidaReferencesModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (mode_) {
    case Mode::locations:
        switch (section) {
        case 0: return QStringLiteral("File");
        case 1: return QStringLiteral("Location");
        case 2: return QStringLiteral("Preview");
        }
        break;
    case Mode::symbols:
        switch (section) {
        case 0: return QStringLiteral("Symbol");
        case 1: return QStringLiteral("Kind");
        case 2: return QStringLiteral("Line");
        }
        break;
    case Mode::completions:
        switch (section) {
        case 0: return QStringLiteral("Completion");
        case 1: return QStringLiteral("Kind");
        case 2: return QStringLiteral("Detail");
        }
        break;
    case Mode::information:
        switch (section) {
        case 0: return QStringLiteral("Item");
        case 1: return QStringLiteral("Information");
        }
        break;
    case Mode::diagnostics:
        switch (section) {
        case 0: return QStringLiteral("Severity");
        case 1: return QStringLiteral("File");
        case 2: return QStringLiteral("Location");
        case 3: return QStringLiteral("Message");
        }
        break;
    case Mode::proposed_edits:
        switch (section) {
        case 0: return QStringLiteral("File");
        case 1: return QStringLiteral("Range");
        case 2: return QStringLiteral("Expected");
        case 3: return QStringLiteral("Replacement");
        }
        break;
    case Mode::code_actions:
        switch (section) {
        case 0: return QStringLiteral("Action");
        case 1: return QStringLiteral("Kind");
        case 2: return QStringLiteral("Detail");
        }
        break;
    case Mode::none: break;
    }
    return {};
}

AidaReferencesView::AidaReferencesView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.programming.references"));
    const auto& tokens = theme::tokens();
    stack_ = new QStackedLayout(this);
    stack_->setContentsMargins(0, 0, 0, 0);
    content_ = new QWidget(this);
    auto* column = new QVBoxLayout(content_);
    column->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
        tokens.spacing.sm, tokens.spacing.xs);
    column->setSpacing(tokens.spacing.xs);

    header_ = new widgets::AidaViewHeader(QStringLiteral("Programming References"),
        QStringLiteral("Programming / Provider Results"), content_);
    column->addWidget(header_);
    strip_ = new AidaIndexStatusStrip(content_);
    column->addWidget(strip_);
    auto* query_row = new QHBoxLayout();
    query_edit_ = new QLineEdit(content_);
    query_edit_->setObjectName(QStringLiteral("aida.view.programming.references.query"));
    query_edit_->setPlaceholderText(QStringLiteral("Identifier or text..."));
    query_edit_->setClearButtonEnabled(true);
    query_row->addWidget(query_edit_, 1);
    find_button_ = new QPushButton(QStringLiteral("Find"), content_);
    find_button_->setObjectName(QStringLiteral("aida.view.programming.references.find"));
    query_row->addWidget(find_button_);
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), content_);
    cancel_button_->setObjectName(QStringLiteral("aida.view.programming.references.cancel"));
    query_row->addWidget(cancel_button_);
    column->addLayout(query_row);
    summary_ = new QLabel(content_);
    summary_->setObjectName(QStringLiteral("aida.view.programming.references.summary"));
    summary_->setEnabled(false);
    column->addWidget(summary_);

    model_ = new AidaReferencesModel(this);
    table_ = new QTableView(content_);
    table_->setObjectName(QStringLiteral("aida.view.programming.references.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(theme::tokens().table.row_h);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->installEventFilter(this);
    table_->viewport()->installEventFilter(this);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setModel(model_);
    column->addWidget(table_, 1);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.view.programming.references.state"));
    stack_->addWidget(content_);
    stack_->addWidget(state_view_);

    auto& bridge = AidaLanguageServiceBridge::instance();
    connect(&bridge, &AidaLanguageServiceBridge::resultChanged,
            this, &AidaReferencesView::onResultChanged);
    connect(&bridge, &AidaLanguageServiceBridge::indexChanged, this, [this] {
        strip_->refresh();
    });
    connect(query_edit_, &QLineEdit::returnPressed, this, &AidaReferencesView::submitQuery);
    connect(find_button_, &QPushButton::clicked, this, &AidaReferencesView::submitQuery);
    connect(cancel_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(aida::ui::application_ui::execute_action(
            "programming.language.cancel_query", aida::ui::action_invocation_source_t::toolbar));
    });
    connect(table_, &QTableView::activated, this, &AidaReferencesView::activateRow);
    connect(table_, &QTableView::customContextMenuRequested, this, [this](const QPoint& pos) {
        openRowContext(table_->indexAt(pos), aida::ui::context_menu_open_origin_t::pointer,
            table_->viewport()->mapToGlobal(pos));
    });
    refreshPresentation();
}

bool AidaReferencesView::eventFilter(QObject* watched, QEvent* event) {
    QModelIndex index;
    QPoint global_pos;
    if (keyboard_context_menu_event(watched, event, table_, &index, &global_pos)) {
        openRowContext(index, aida::ui::context_menu_open_origin_t::menu_key, global_pos);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void AidaReferencesView::onResultChanged(int kind) {
    static_cast<void>(kind);
    refreshPresentation();
}

void AidaReferencesView::submitQuery() {
    language::query_t query;
    query.kind = language::capability_kind_t::references;
    query.document = language::active_document_context();
    query.text = query_edit_->text().toStdString();
    query.maximum_results = 4096;
    static_cast<void>(language::request(std::move(query)));
    refreshPresentation();
}

void AidaReferencesView::refreshPresentation() {
    auto& bridge = AidaLanguageServiceBridge::instance();
    hydrateActionButton(cancel_button_, "programming.language.cancel_query");
    auto snapshot = bridge.latestResult({language::capability_kind_t::references,
        language::capability_kind_t::definition,
        language::capability_kind_t::completion,
        language::capability_kind_t::hover,
        language::capability_kind_t::signature_help,
        language::capability_kind_t::diagnostics,
        language::capability_kind_t::declaration,
        language::capability_kind_t::implementation,
        language::capability_kind_t::type_definition,
        language::capability_kind_t::semantic_rename,
        language::capability_kind_t::formatting,
        language::capability_kind_t::range_formatting,
        language::capability_kind_t::code_actions});
    header_->setStatus(snapshot && snapshot->state == language::result_state_t::ready
        ? widgets::AidaSemantic::Success : widgets::AidaSemantic::Neutral);
    if (snapshot && snapshot->request_id != adopted_request_id_ &&
        !snapshot->query_text.empty()) {
        adopted_request_id_ = snapshot->request_id;
        query_edit_->setText(QString::fromStdString(snapshot->query_text));
    }
    shown_ = snapshot;
    if (!snapshot || snapshot->state != language::result_state_t::ready) {
        if (!snapshot) {
            state_view_->setState(widgets::AidaStateView::State::Empty);
            state_view_->setTitle(QStringLiteral("No programming references"));
            state_view_->setMessage(QStringLiteral(
                "No provider query has been published for this view."));
        } else {
            switch (snapshot->state) {
            case language::result_state_t::loading:
                state_view_->setState(widgets::AidaStateView::State::Loading);
                state_view_->setTitle(QStringLiteral("Querying language provider"));
                break;
            case language::result_state_t::cancelled:
                state_view_->setState(widgets::AidaStateView::State::Empty);
                state_view_->setTitle(QStringLiteral("Query cancelled"));
                break;
            case language::result_state_t::error:
                state_view_->setState(widgets::AidaStateView::State::Error);
                state_view_->setTitle(QStringLiteral("Language provider failed"));
                break;
            case language::result_state_t::unavailable:
                state_view_->setState(widgets::AidaStateView::State::Empty);
                state_view_->setTitle(QStringLiteral("Capability unavailable"));
                break;
            default:
                state_view_->setState(widgets::AidaStateView::State::Empty);
                state_view_->setTitle(QStringLiteral("No programming references"));
                break;
            }
            state_view_->setMessage(QString::fromStdString(snapshot->status));
        }
        stack_->setCurrentWidget(state_view_);
        return;
    }
    stack_->setCurrentWidget(content_);
    const std::size_t result_count = snapshot->locations.size() +
        snapshot->symbols.size() + snapshot->completions.size() +
        snapshot->information.size() + snapshot->diagnostics.size() +
        snapshot->proposed_edits.size() + snapshot->code_actions.size();
    summary_->setText(QStringLiteral("%1 result%2 for '%3'  |  %4  |  generation %5%6")
        .arg(result_count)
        .arg(result_count == 1 ? QString() : QStringLiteral("s"))
        .arg(QString::fromStdString(snapshot->query_text),
            QString::fromStdString(snapshot->provider_name))
        .arg(snapshot->index_generation)
        .arg(snapshot->truncated ? QStringLiteral("  |  truncated") : QString()));
    model_->setSnapshot(snapshot);
}

void AidaReferencesView::activateRow(const QModelIndex& index) {
    const auto location = model_->locationAt(index.row());
    if (!location.file_path.empty())
        static_cast<void>(language::open_location(location));
}

void AidaReferencesView::openRowContext(const QModelIndex& index,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos) {
    if (!shown_)
        return;
    const auto location = model_->locationAt(index.row());
    if (location.file_path.empty())
        return;
    open_result_context(location, shown_->query_text, *shown_, origin, global_pos, this);
}

AidaRenameDialog::AidaRenameDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("aida.dialog.programming.rename"));
    setWindowTitle(QStringLiteral("Semantic Rename"));
    setModal(true);
    auto* column = new QVBoxLayout(this);
    auto* title = new QLabel(QStringLiteral("Provider-reviewed semantic rename"), this);
    column->addWidget(title);
    auto* explainer = new QLabel(QStringLiteral(
        "The provider must return revision-bound proposed edits. AiDA does not apply them automatically."), this);
    explainer->setEnabled(false);
    explainer->setWordWrap(true);
    column->addWidget(explainer);
    column->addWidget(new QLabel(QStringLiteral("Identifier"), this));
    identifier_ = new QLineEdit(this);
    identifier_->setObjectName(QStringLiteral("aida.dialog.programming.rename.identifier"));
    identifier_->setReadOnly(true);
    column->addWidget(identifier_);
    column->addWidget(new QLabel(QStringLiteral("New name"), this));
    replacement_ = new QLineEdit(this);
    replacement_->setObjectName(QStringLiteral("aida.dialog.programming.rename.replacement"));
    replacement_->setPlaceholderText(QStringLiteral("New identifier (no whitespace)"));
    column->addWidget(replacement_);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("aida.dialog.programming.rename.error"));
    error_->setWordWrap(true);
    error_->setProperty("aidaVariant", "error");
    error_->setVisible(false);
    column->addWidget(error_);
    auto* buttons = new QHBoxLayout();
    request_button_ = new QPushButton(QStringLiteral("Request Reviewed Edits"), this);
    request_button_->setDefault(true);
    buttons->addStretch(1);
    buttons->addWidget(request_button_);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
    buttons->addWidget(cancel);
    column->addLayout(buttons);
    connect(request_button_, &QPushButton::clicked, this, &AidaRenameDialog::requestEdits);
    connect(replacement_, &QLineEdit::returnPressed, this, [this] {
        if (request_button_->isEnabled())
            requestEdits();
    });
    connect(replacement_, &QLineEdit::textChanged, this, [this] {
        submit_error_active_ = false;
        validateInput();
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    validateInput();
}

void AidaRenameDialog::openFor(const std::string& identifier) {
    identifier_->setText(QString::fromStdString(identifier));
    replacement_->clear();
    submit_error_active_ = false;
    error_->clear();
    error_->setVisible(false);
    validateInput();
    open();
    replacement_->setFocus();
}

void AidaRenameDialog::validateInput() {
    if (submit_error_active_)
        return;
    const QString replacement = replacement_->text();
    const QString identifier = identifier_->text();
    QString problem;
    if (replacement.isEmpty())
        problem = QStringLiteral("Enter a non-empty replacement name.");
    else if (replacement == identifier)
        problem = QStringLiteral("The replacement must differ from the current identifier.");
    else if (std::any_of(replacement.begin(), replacement.end(),
            [](QChar ch) { return ch.isSpace(); }))
        problem = QStringLiteral("An identifier cannot contain whitespace.");
    error_->setText(problem);
    error_->setVisible(!problem.isEmpty());
    request_button_->setEnabled(problem.isEmpty());
    request_button_->setToolTip(problem.isEmpty()
        ? QStringLiteral("Request provider-reviewed rename edits")
        : problem);
}

void AidaRenameDialog::requestEdits() {
    const std::string identifier = identifier_->text().toStdString();
    const std::string replacement = replacement_->text().toStdString();
    const auto fail = [this](const QString& message) {
        submit_error_active_ = true;
        error_->setText(message);
        error_->setVisible(true);
        request_button_->setEnabled(false);
    };
    if (identifier.empty()) {
        fail(QStringLiteral("The caret identifier is no longer available; close and retry from the editor."));
        return;
    }
    if (replacement.empty()) {
        fail(QStringLiteral("Enter a non-empty replacement name."));
        return;
    }
    if (replacement == identifier) {
        fail(QStringLiteral("The replacement must differ from the current identifier."));
        return;
    }
    if (std::any_of(replacement.begin(), replacement.end(),
            [](const QString::value_type ch) { return ch.isSpace(); })) {
        fail(QStringLiteral("An identifier cannot contain whitespace."));
        return;
    }
    language::query_t query;
    query.kind = language::capability_kind_t::semantic_rename;
    query.document = language::active_document_context();
    query.text = identifier;
    query.replacement_text = replacement;
    query.maximum_results = 4096;
    const auto requested = language::request(std::move(query));
    if (!requested.accepted) {
        fail(QString::fromStdString(requested.reason));
        return;
    }
    accept();
}

void open_rename_dialog(QWidget* parent) {
    auto* dialog = new AidaRenameDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->openFor(language::active_query_text());
}

AidaSourceDebugTaskModel::AidaSourceDebugTaskModel(QObject* parent)
    : QAbstractTableModel(parent) {
}

void AidaSourceDebugTaskModel::setSnapshot(
        aida::ui::task_center::immutable_snapshot_ptr snapshot) {
    const std::uint64_t next_generation = snapshot ? snapshot->generation : 0;
    if (next_generation == generation_)
        return;
    beginResetModel();
    snapshot_ = std::move(snapshot);
    generation_ = next_generation;
    rows_.clear();
    if (snapshot_) {
        for (const auto& task : snapshot_->tasks) {
            if (task.owner == "source_debug" || task.source == "Source Debugger" ||
                task.id.compare(0, 13, "source.debug.") == 0)
                rows_.push_back(&task);
        }
    }
    endResetModel();
}

int AidaSourceDebugTaskModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int AidaSourceDebugTaskModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::count);
}

QVariant AidaSourceDebugTaskModel::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= static_cast<int>(rows_.size()))
        return {};
    const auto& task = *rows_[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole) {
        switch (static_cast<Column>(index.column())) {
        case Column::operation: return QString::fromStdString(task.label);
        case Column::stage: return QString::fromStdString(task.stage);
        case Column::target: return QString::fromStdString(task.target);
        case Column::progress:
            return task.progress >= 0.0f
                ? QStringLiteral("%1%").arg(static_cast<int>(task.progress * 100.0f))
                : QStringLiteral("active");
        case Column::count: break;
        }
    }
    if (role == Qt::UserRole)
        return QString::fromStdString(task.id);
    return {};
}

void AidaSourceDebugTaskModel::multiData(const QModelIndex& index,
                                         QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

QVariant AidaSourceDebugTaskModel::headerData(int section, Qt::Orientation orientation,
                                              int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (static_cast<Column>(section)) {
    case Column::operation: return QStringLiteral("Operation");
    case Column::stage: return QStringLiteral("Stage");
    case Column::target: return QStringLiteral("Target");
    case Column::progress: return QStringLiteral("Progress");
    case Column::count: break;
    }
    return {};
}

AidaSourceDebugConsole::AidaSourceDebugConsole(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.programming.source_debug_console"));
    const auto& tokens = theme::tokens();
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(tokens.spacing.sm, tokens.spacing.xs,
        tokens.spacing.sm, tokens.spacing.xs);
    column->setSpacing(tokens.spacing.xs);

    header_ = new widgets::AidaViewHeader(QStringLiteral("Source Debug Console"),
        QStringLiteral("Programming / Debug"), this);
    column->addWidget(header_);

    auto* toolbar = new QHBoxLayout();
    mixed_button_ = new QPushButton(QStringLiteral("Mixed Source / Assembly"), this);
    mixed_button_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.mixed"));
    rebind_button_ = new QPushButton(QStringLiteral("Rebind"), this);
    rebind_button_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.rebind"));
    problems_button_ = new QPushButton(QStringLiteral("Problems"), this);
    problems_button_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.problems"));
    output_button_ = new QPushButton(QStringLiteral("Output"), this);
    output_button_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.output"));
    toolbar->addWidget(mixed_button_);
    toolbar->addWidget(rebind_button_);
    toolbar->addWidget(problems_button_);
    toolbar->addWidget(output_button_);
    toolbar->addStretch(1);
    column->addLayout(toolbar);
    connect(mixed_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(aida::ui::application_ui::execute_action(
            "debug.source.open_mixed", aida::ui::action_invocation_source_t::toolbar));
    });
    connect(rebind_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(aida::ui::application_ui::execute_action(
            "debug.source.rebind", aida::ui::action_invocation_source_t::toolbar));
    });
    connect(problems_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(aida::ui::application_ui::execute_action(
            "programming.show_problems", aida::ui::action_invocation_source_t::toolbar));
    });
    connect(output_button_, &QPushButton::clicked, this, [] {
        static_cast<void>(aida::ui::application_ui::execute_action(
            "view.focus.view.output", aida::ui::action_invocation_source_t::toolbar));
    });

    status_line_ = new QLabel(this);
    status_line_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.status"));
    status_line_->setEnabled(false);
    column->addWidget(status_line_);
    pending_label_ = new QLabel(this);
    pending_label_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.pending"));
    pending_label_->setProperty("aidaVariant", "warning");
    pending_label_->setVisible(false);
    column->addWidget(pending_label_);
    error_label_ = new QLabel(this);
    error_label_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.error"));
    error_label_->setWordWrap(true);
    error_label_->setProperty("aidaVariant", "error");
    error_label_->setVisible(false);
    column->addWidget(error_label_);
    location_label_ = new QLabel(this);
    location_label_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.location"));
    location_label_->setVisible(false);
    column->addWidget(location_label_);
    location_detail_ = new QLabel(this);
    location_detail_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.location_detail"));
    location_detail_->setEnabled(false);
    location_detail_->setVisible(false);
    column->addWidget(location_detail_);
    bind_hint_ = new QLabel(QStringLiteral(
        "Open an analysis workspace or attach a process to bind source breakpoints."), this);
    bind_hint_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.bind_hint"));
    bind_hint_->setEnabled(false);
    bind_hint_->setVisible(false);
    column->addWidget(bind_hint_);

    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.splitter"));
    splitter_->setOpaqueResize(true);
    splitter_->setChildrenCollapsible(true);
    model_ = new AidaSourceDebugTaskModel(this);
    table_ = new QTableView(splitter_);
    table_->setObjectName(QStringLiteral("aida.view.programming.source_debug_console.tasks"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(theme::tokens().table.row_h);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->setModel(model_);
    table_->setMinimumHeight(theme::tokens().table.row_h * 3);
    table_->setMaximumHeight(theme::tokens().table.row_h * 6 +
        theme::tokens().table.header_h);
    splitter_->addWidget(table_);
    output_pane_ = new AidaOutputPane(bottom_tab_t::output,
        QStringLiteral("embedded.output"), splitter_);
    output_pane_->setMinimumHeight(theme::tokens().table.row_h * 3);
    splitter_->addWidget(output_pane_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    column->addWidget(splitter_, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(250);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &AidaSourceDebugConsole::onTick);
    timer_->start();
    onTick();
}

void AidaSourceDebugConsole::onTick() {
    auto& bridge = AidaLanguageServiceBridge::instance();
    model_->setSnapshot(AidaTaskCenterController::instance().current());
    const auto source = bridge.sourceDebugSnapshot();
    const auto focus_action = aida::ui::application_ui::present_action(
        "view.focus.view.programming.source_debug_console");
    const bool target_available = source && !source->target_key.empty();
    const bool current_location = source && source->current.valid;
    header_->setSubtitle(target_available ? QString::fromStdString(source->target_key)
                                          : QStringLiteral("No debug target"));
    header_->setStatus(source && !source->error.empty() ? widgets::AidaSemantic::Error
        : source && source->operation_pending ? widgets::AidaSemantic::Warning
        : target_available ? widgets::AidaSemantic::Success
        : widgets::AidaSemantic::Neutral);
    static_cast<void>(focus_action);
    refreshActions();
    if (!source) {
        status_line_->setText(QStringLiteral(
            "The source-debug service has not published a usable snapshot."));
        pending_label_->setVisible(false);
        error_label_->setVisible(false);
        location_label_->setVisible(false);
        location_detail_->setVisible(false);
        bind_hint_->setVisible(false);
        return;
    }
    status_line_->setText(QStringLiteral("PID %1  |  %2 breakpoint%3  |  generation %4")
        .arg(source->target_pid)
        .arg(source->definitions.size())
        .arg(source->definitions.size() == 1 ? QString() : QStringLiteral("s"))
        .arg(source->generation));
    pending_label_->setVisible(source->operation_pending);
    if (source->operation_pending)
        pending_label_->setText(source->operation_label.empty()
            ? QStringLiteral("Source-debug operation running")
            : QString::fromStdString(source->operation_label));
    error_label_->setVisible(!source->error.empty());
    if (!source->error.empty())
        error_label_->setText(QStringLiteral("Source debugger: %1")
            .arg(QString::fromStdString(source->error)));
    if (current_location) {
        location_label_->setVisible(true);
        location_label_->setText(QStringLiteral("Stopped at %1:%2  |  %3+0x%4  |  0x%5")
            .arg(QString::fromStdString(source->current.file_path))
            .arg(source->current.line)
            .arg(QString::fromStdString(source->current.module_name))
            .arg(source->current.module_rva, 0, 16)
            .arg(source->current.address, 0, 16));
        location_detail_->setVisible(!source->current.detail.empty());
        location_detail_->setText(QString::fromStdString(source->current.detail));
        bind_hint_->setVisible(false);
    } else {
        location_label_->setVisible(false);
        location_detail_->setVisible(false);
        bind_hint_->setVisible(!target_available);
    }
}

void AidaSourceDebugConsole::refreshActions() {
    hydrateActionButton(mixed_button_, "debug.source.open_mixed");
    hydrateActionButton(rebind_button_, "debug.source.rebind");
    hydrateActionButton(problems_button_, "programming.show_problems");
    hydrateActionButton(output_button_, "view.focus.view.output");
}

}
