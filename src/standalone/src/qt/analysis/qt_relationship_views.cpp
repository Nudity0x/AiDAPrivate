#include "qt/analysis/qt_relationship_views.hpp"

#include <QComboBox>
#include <QFontMetricsF>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <QKeyEvent>

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/analysis/workspace/paged_snapshot_view.hpp"
#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/ui/context_menu_contract.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_analysis_menu_builder.hpp"
#include "qt/analysis/qt_gui_post.hpp"
#include "qt/analysis/qt_revision_poller.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_icons.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_search_field.hpp"
#include "qt/widgets/aida_state_view.hpp"

#include <algorithm>

namespace aida::qt::analysis {

namespace {

constexpr std::size_t kSegmentScanChunkBudget = 2048;
constexpr std::size_t kProximityScanChunkBudget = 1024;
constexpr int kProximityNodeLimitDefault = 192;

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains_case_insensitive(const std::string& value, const std::string& query) {
    if (query.empty()) return true;
    if (query.size() > value.size()) return false;
    return std::search(value.begin(), value.end(), query.begin(), query.end(),
        [](unsigned char left, unsigned char right) {
            return std::tolower(left) == std::tolower(right);
        }) != value.end();
}

// Verbatim port of segment_registers::advance_projection's chunk body.
void advanceSegmentScan(QtSegmentRegistersState& state,
                        const disasm_view::workspace_context_t& context,
                        std::vector<segment_register_display_row_t>& appended,
                        QHash<quint64, QString>& text_cache) {
    if (state.complete || !context.publication || !context.publication->snapshot) return;
    const auto& snapshot = *context.publication->snapshot;
    const auto instruction_rows = aida::analysis::instructions_view(snapshot);
    const auto operand_rows = aida::analysis::operand_facts_view(snapshot);
    aida::analysis::fact_page_pin_t instruction_pin;
    aida::analysis::fact_page_pin_t operand_pin;
    const bool resident_instructions = instruction_rows.resident();
    const std::size_t begin = state.instruction_cursor;
    const std::size_t end = (std::min)(
        static_cast<std::size_t>(instruction_rows.size()),
        begin + kSegmentScanChunkBudget);
    disasm_view::request_format_range(context, begin, end);
    for (std::size_t index = begin; index < end; ++index) {
        const aida::analysis::instruction_record_t* instruction_record = nullptr;
        if (resident_instructions) {
            instruction_record = &instruction_rows.resident_span()[index];
        } else {
            auto instruction_row = instruction_rows.at(index, instruction_pin);
            if (!instruction_row)
                break;
            instruction_record = instruction_row.value();
        }
        const auto& instruction = *instruction_record;
        const std::size_t operand_begin = instruction.operand_fact_begin;
        const std::size_t operand_end = (std::min)(
            static_cast<std::size_t>(operand_rows.size()),
            operand_begin + static_cast<std::size_t>(instruction.operand_fact_count));
        for (std::size_t operand_index = operand_begin; operand_index < operand_end;
             ++operand_index) {
            auto operand_row = operand_rows.at(operand_index, operand_pin);
            if (!operand_row)
                break;
            const auto& operand = *operand_row.value();
            if (operand.kind != aida::analysis::operand_kind_t::memory ||
                operand.segment_reg == 0)
                continue;
            const auto runtime = disasm_view::runtime_address(context, instruction.address);
            if (!runtime || *runtime == 0) continue;
            const bool segment_relative = operand.address_expression ==
                aida::analysis::address_expression_kind_t::segment_relative;
            const std::uint64_t function =
                disasm_view::enclosing_function_start(*runtime, context);
            const std::uint64_t scope = function != 0 ? function : (*runtime & ~0xFFFULL);
            const segment_register_group_identity_t identity{
                scope, operand.segment_reg, segment_relative};
            const auto found = state.groups.find(identity);
            if (found == state.groups.end()) {
                const std::size_t row_index = state.rows.size();
                state.groups.emplace(identity, row_index);
                segment_register_row_t row;
                row.address = *runtime;
                row.end_address = *runtime;
                row.instruction_id = instruction.id;
                row.register_id = operand.segment_reg;
                row.operand_index = operand.operand_index;
                row.segment_relative = segment_relative;
                row.provenance = instruction.provenance;
                row.confidence = instruction.confidence;
                row.observations = 1;
                state.rows.push_back(std::move(row));
            } else {
                auto& row = state.rows[found->second];
                row.address = (std::min)(row.address, *runtime);
                row.end_address = (std::max)(row.end_address, *runtime);
                ++row.observations;
                if (instruction.confidence > row.confidence) {
                    row.instruction_id = instruction.id;
                    row.operand_index = operand.operand_index;
                    row.provenance = instruction.provenance;
                    row.confidence = instruction.confidence;
                }
            }
            ++state.observations;
        }
    }
    state.instruction_cursor = end;
    state.complete =
        static_cast<std::uint64_t>(end) >= instruction_rows.size();
    state.filter_dirty = true;
    (void)appended;
    (void)text_cache;
}

}

QtSegmentRegistersView::QtSegmentRegistersView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.analysis.segment_registers"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.segment_registers.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    const auto& tokens = theme::tokens();
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x, tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    auto* title = new QLabel(QStringLiteral("Segment Registers"), toolbar);
    title->setObjectName(QStringLiteral("aida.segment_registers.title"));
    toolbar_layout->addWidget(title);
    status_label_ = new QLabel(toolbar);
    status_label_->setObjectName(QStringLiteral("aida.segment_registers.status"));
    status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    toolbar_layout->addWidget(status_label_);
    filter_debounce_ = new QTimer(this);
    filter_debounce_->setSingleShot(true);
    filter_debounce_->setInterval(300);
    filter_ = new widgets::AidaSearchField(
        QStringLiteral("Filter register, address, instruction..."), toolbar);
    filter_->setObjectName(QStringLiteral("aida.segment_registers.filter"));
    filter_->setClearButtonEnabled(true);
    toolbar_layout->addWidget(filter_, 1);
    layout->addWidget(toolbar);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("aida.segment_registers.progress"));
    progress_->setTextVisible(false);
    progress_->setRange(0, 100);
    progress_->setVisible(false);
    layout->addWidget(progress_);

    model_ = new QtSegmentRegistersModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.segment_registers.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.compact_row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = table_->horizontalHeader();
    using Column = QtSegmentRegistersModel::Column;
    const QFontMetricsF ui_metrics(table_->font());
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const auto with_cell_pad = [&tokens](int content) {
        return content + 2 * tokens.table.cell_pad_x + tokens.spacing.xs;
    };
    horizontal->setSectionResizeMode(static_cast<int>(Column::reg), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::reg),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Register"))),
            static_cast<int>(ui_metrics.horizontalAdvance(
                QStringLiteral("Register #99"))))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::observed_span),
        QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::observed_span),
        with_cell_pad(static_cast<int>(code_metrics.horizontalAdvance(
            QStringLiteral("0xDDDDDDDDDDDDDDDD - 0xDDDDDDDDDDDDDDDD")))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::facts), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::facts),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Facts"))),
            static_cast<int>(code_metrics.horizontalAdvance(
                QStringLiteral("99999"))))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::instruction),
        QHeaderView::Stretch);
    horizontal->setSectionResizeMode(static_cast<int>(Column::evidence),
        QHeaderView::Stretch);
    horizontal->setSectionResizeMode(static_cast<int>(Column::confidence),
        QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::confidence),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Confidence"))),
            static_cast<int>(ui_metrics.horizontalAdvance(QStringLiteral("100%"))))));
    horizontal->setStretchLastSection(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.segment_registers.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);

    pump_ = new QTimer(this);
    pump_->setInterval(0);
    connect(pump_, &QTimer::timeout, this, [this] { pumpChunk(); });

    connect(filter_, &QLineEdit::textChanged, this, [this](const QString&) {
        filter_debounce_->start();
    });
    connect(filter_debounce_, &QTimer::timeout, this, [this] {
        if (!state_) return;
        state_->filter = filter_->text();
        state_->filter_dirty = true;
        publishRows();
        refreshPresentation();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!state_ || !current.isValid()) return;
        const auto* row = model_->rowAt(current.row());
        if (!row) return;
        state_->selected = model_->sourceIndexForViewRow(current.row());
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        const auto context = disasm_view::capture_workspace(workspace);
        if (context)
            disasm_view::select_address(row->row.address, context);
    });
    connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid()) return;
        const auto* row = model_->rowAt(index.row());
        if (!row) return;
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        QtAnalysisBridge::instance().navigateTo(workspace, row->row.address,
            "document.disassembly");
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        if (index.isValid())
            showRowMenu(table_->viewport()->mapToGlobal(pos), index.row());
    });
    table_->installEventFilter(this);

    connect(&QtAnalysisBridge::instance(), &QtAnalysisBridge::activeContextChanged,
            this, [this](QtWorkspaceContext* context) { rebindContext(context); });
    rebindContext(QtAnalysisBridge::instance().activeContext());
}

void QtSegmentRegistersView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (context_ && context_->poller())
        context_->poller()->arm();
    resetIfNeeded();
    if (state_ && !state_->complete)
        pump_->start();
}

void QtSegmentRegistersView::hideEvent(QHideEvent* event) {
    pump_->stop();
    QWidget::hideEvent(event);
}

bool QtSegmentRegistersView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Menu ||
            (key->key() == Qt::Key_F10 &&
                key->modifiers().testFlag(Qt::ShiftModifier))) {
            const auto current = table_->currentIndex();
            if (current.isValid())
                showRowMenu(table_->viewport()->mapToGlobal(
                    table_->visualRect(current).center()), current.row());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QtSegmentRegistersView::rebindContext(QtWorkspaceContext* context) {
    if (context_ == context) return;
    if (poller_connection_) disconnect(poller_connection_);
    context_ = context;
    state_ = context ? &context->segmentRegistersState : nullptr;
    instruction_text_cache_.clear();
    published_rows_ = 0;
    if (state_) {
        if (!state_->filter.isEmpty() && filter_->text() != state_->filter)
            filter_->setText(state_->filter);
        poller_connection_ = connect(context_->poller(),
            &QtRevisionPoller::revisionsChanged, this,
            [this](quint64, quint64, quint64, quint64) {
                resetIfNeeded();
                publishRows();
                refreshPresentation();
                if (state_ && !state_->complete && isVisible())
                    pump_->start();
            });
        publishRows();
        refreshPresentation();
        if (isVisible() && !state_->complete)
            pump_->start();
    } else {
        model_->resetAll();
        refreshPresentation();
    }
}

void QtSegmentRegistersView::resetIfNeeded() {
    if (!state_ || !context_) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context || !context.publication) return;
    auto& state = *state_;
    const auto& publication = *context.publication;
    if (state.initialized && state.generation == publication.generation &&
        state.revision == publication.analysis_revision &&
        state.overlay_revision == publication.overlay_revision)
        return;
    state.initialized = true;
    state.generation = publication.generation;
    state.revision = publication.analysis_revision;
    state.overlay_revision = publication.overlay_revision;
    state.instruction_cursor = 0;
    state.complete = false;
    state.rows.clear();
    state.groups.clear();
    state.observations = 0;
    state.filter_dirty = true;
    state.selected = static_cast<std::size_t>(-1);
    instruction_text_cache_.clear();
    published_rows_ = 0;
    model_->resetAll();
}

void QtSegmentRegistersView::pumpChunk() {
    if (!state_ || !context_) {
        pump_->stop();
        return;
    }
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context) {
        pump_->stop();
        return;
    }
    std::vector<segment_register_display_row_t> appended;
    advanceSegmentScan(*state_, context, appended, instruction_text_cache_);
    publishRows();
    refreshPresentation();
    if (state_->complete)
        pump_->stop();
}

QString QtSegmentRegistersView::instructionText(
    aida::analysis::entity_id_t instruction_id) {
    const auto cached = instruction_text_cache_.constFind(instruction_id);
    if (cached != instruction_text_cache_.constEnd()) return cached.value();
    const auto workspace = context_ ? context_->workspace().lock() : nullptr;
    const auto context = disasm_view::capture_workspace(workspace);
    QString text;
    if (context) {
        const auto formatted =
            disasm_view::formatted_instruction(context, instruction_id);
        if (formatted && !formatted->text.empty())
            text = QString::fromStdString(formatted->text);
    }
    if (text.isEmpty())
        text = QStringLiteral("Decoded memory operand");
    instruction_text_cache_.insert(instruction_id, text);
    return text;
}

void QtSegmentRegistersView::publishRows() {
    if (!state_) return;
    auto& state = *state_;
    if (published_rows_ == state.rows.size() && !state.filter_dirty) return;
    // Publish the full projection per chunk (groups merge into existing rows
    // mid-scan; snapshot identity changes), 07 S3 reset path.
    std::vector<segment_register_display_row_t> display;
    display.reserve(state.rows.size());
    for (const auto& row : state.rows) {
        segment_register_display_row_t out;
        out.row = row;
        out.instruction = instructionText(row.instruction_id);
        display.push_back(std::move(out));
    }
    model_->setRows(std::move(display));
    model_->applyFilter(state.filter.toLower());
    state.filter_dirty = false;
    published_rows_ = state.rows.size();
}

void QtSegmentRegistersView::refreshPresentation() {
    if (!state_ || !context_ || context_->workspace().expired()) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No analysis workspace"));
        state_view_->setMessage(QStringLiteral(
            "Open and analyze a binary to inspect decoded segment-register components."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        progress_->setVisible(false);
        status_label_->clear();
        return;
    }
    const auto& state = *state_;
    status_label_->setText(QStringLiteral("%1 groups | %2 facts%3")
        .arg(model_->rowCount())
        .arg(state.observations)
        .arg(state.complete ? QString() : QStringLiteral(" | scanning")));
    if (!state.complete) {
        const auto workspace = context_->workspace().lock();
        const auto context = disasm_view::capture_workspace(workspace);
        std::uint64_t total = 0;
        if (context && context.publication && context.publication->snapshot)
            total = static_cast<std::uint64_t>(aida::analysis::instructions_view(
                *context.publication->snapshot).size());
        progress_->setVisible(true);
        progress_->setRange(0, total == 0 ? 0 : 100);
        progress_->setValue(total == 0 ? 0
            : static_cast<int>((state.instruction_cursor * 100) / total));
    } else {
        progress_->setVisible(false);
    }
    if (state.rows.empty() && state.complete) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No decoded segment components"));
        state_view_->setMessage(QStringLiteral(
            "No decoded memory operand publishes a segment-register component. Live CS, DS, ES, FS, GS and SS selector values remain in Debugger CPU while a target is paused."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (model_->rowCount() == 0 && !state.rows.empty()) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No matches"));
        state_view_->setMessage(QStringLiteral(
            "No decoded segment-register component matches the current filter."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    state_view_->setVisible(false);
    table_->setVisible(true);
}

void QtSegmentRegistersView::showRowMenu(const QPoint& global_pos, int view_row) {
    if (!state_ || !context_) return;
    const auto* row = model_->rowAt(view_row);
    if (!row) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context) return;
    state_->selected = model_->sourceIndexForViewRow(view_row);
    disasm_view::select_address(row->row.address, context, false);
    const auto& source = state_->selected;
    const auto state_ptr = state_;
    auto menu = qt_analysis_menus::build_analysis_row_menu(context,
        row->row.address, true, segment_register_text(row->row.register_id),
        row->row.segment_relative
            ? std::string("Segment-relative decoded fact")
            : std::string("Decoded segment component"),
        std::to_string(row->row.observations) + " decoded observation" +
            (row->row.observations == 1 ? "" : "s") + " in " +
            segment_register_observed_span(row->row),
        [state_ptr, expected = source, identity = segment_register_observed_span(row->row)] {
            return state_ptr->selected == expected &&
                expected < state_ptr->rows.size()
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The selected segment-register entity changed");
        });
    QtAnalysisBridge::instance().showRetainedMenu(menu,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

// ---------------------------------------------------------------------------

namespace {

void beginProximityProjection(QtProximityState& state,
                              const disasm_view::workspace_context_t& context,
                              std::uint64_t root);

std::string proximity_node_name(const disasm_view::workspace_context_t& context,
                                std::uint64_t address) {
    if (const auto typed = disasm_view::typed_address(context, address)) {
        const auto name = disasm_view::resolve_name(context, *typed);
        if (!name.empty()) return name;
    }
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(address));
    return buf;
}

std::string proximity_node_kind(const disasm_view::workspace_context_t& context,
                                std::uint64_t address) {
    if (!disasm_view::typed_address(context, address)) return "External address";
    return disasm_view::enclosing_function_start(address, context) == address
        ? "Function" : "Symbol / address";
}

void beginProximityProjection(QtProximityState& state,
                              const disasm_view::workspace_context_t& context,
                              std::uint64_t root) {
    state.root = root;
    state.pass = 0;
    state.xref_cursor = 0;
    state.call_cursor = 0;
    state.edge_cursor = 0;
    state.complete = root == 0;
    state.capped = false;
    state.skipped_relations = 0;
    state.nodes.clear();
    state.relations.clear();
    state.node_by_address.clear();
    state.frontier.clear();
    state.next_frontier.clear();
    state.relation_keys.clear();
    state.selected = static_cast<std::size_t>(-1);
    state.filter_dirty = true;
    if (root != 0) {
        state.node_by_address[root] = 0;
        proximity_node_t node;
        node.address = root;
        node.name = proximity_node_name(context, root);
        node.kind = proximity_node_kind(context, root);
        node.depth = 0;
        state.nodes.push_back(std::move(node));
        state.frontier.insert(root);
        state.selected = 0;
        disasm_view::select_address(root, context, false);
    }
}

void proximityAddRelation(QtProximityState& state,
                          const disasm_view::workspace_context_t& context,
                          QtProximityState::relation_t relation) {
    if (relation.source == 0 || relation.target == 0) {
        ++state.skipped_relations;
        return;
    }
    if (relation.source == relation.target) return;
    const bool source_frontier = state.frontier.find(relation.source) != state.frontier.end();
    const bool target_frontier = state.frontier.find(relation.target) != state.frontier.end();
    if (!source_frontier && !target_frontier) return;
    const std::uint64_t neighbor = source_frontier ? relation.target : relation.source;
    auto found = state.node_by_address.find(neighbor);
    if (found == state.node_by_address.end()) {
        if (state.nodes.size() >= static_cast<std::size_t>(state.node_limit)) {
            state.capped = true;
            return;
        }
        const std::size_t index = state.nodes.size();
        state.node_by_address[neighbor] = index;
        proximity_node_t node;
        node.address = neighbor;
        node.name = proximity_node_name(context, neighbor);
        node.kind = proximity_node_kind(context, neighbor);
        node.depth = static_cast<std::uint32_t>(state.pass + 1);
        state.nodes.push_back(std::move(node));
        state.next_frontier.insert(neighbor);
    }
    const auto source_node = state.node_by_address.find(relation.source);
    const auto target_node = state.node_by_address.find(relation.target);
    if (source_node == state.node_by_address.end() ||
        target_node == state.node_by_address.end()) return;
    if (!state.relation_keys.insert(relation).second) return;
    state.relations.push_back(relation);
    auto& source = state.nodes[source_node->second];
    auto& target = state.nodes[target_node->second];
    ++source.outgoing;
    ++target.incoming;
    const auto kind = static_cast<std::size_t>(relation.kind);
    ++source.relation_counts[kind];
    ++target.relation_counts[kind];
    state.filter_dirty = true;
}

bool proximityConsumeRelation(QtProximityState& state,
                              const disasm_view::workspace_context_t& context,
                              const aida::analysis::address_t& source,
                              const aida::analysis::address_t& target,
                              proximity_relation_kind_t kind) {
    const auto source_runtime = disasm_view::runtime_address(context, source)
        .value_or(source.value);
    const auto target_runtime = disasm_view::runtime_address(context, target)
        .value_or(target.value);
    if (source_runtime == 0 || target_runtime == 0) {
        ++state.skipped_relations;
        return false;
    }
    std::uint64_t source_resolved = source_runtime;
    std::uint64_t target_resolved = target_runtime;
    const auto source_function =
        disasm_view::enclosing_function_start(source_resolved, context);
    const auto target_function =
        disasm_view::enclosing_function_start(target_resolved, context);
    if (source_function != 0) source_resolved = source_function;
    if (target_function != 0) target_resolved = target_function;
    proximityAddRelation(state, context,
        QtProximityState::relation_t{source_resolved, target_resolved, kind});
    return true;
}

void proximityFinishPass(QtProximityState& state) {
    ++state.pass;
    if (state.pass >= state.depth_limit || state.next_frontier.empty() || state.capped) {
        state.complete = true;
        return;
    }
    state.frontier = std::move(state.next_frontier);
    state.next_frontier.clear();
    state.xref_cursor = 0;
    state.call_cursor = 0;
    state.edge_cursor = 0;
}

void advanceProximityScan(QtProximityState& state,
                          const disasm_view::workspace_context_t& context) {
    if (state.complete || !context.publication || !context.publication->snapshot) return;
    const auto& snapshot = *context.publication->snapshot;
    static_assert(aida::analysis::fact_domain_count == 12);
    const auto xref_rows = aida::analysis::xrefs_view(snapshot);
    const auto edge_rows = aida::analysis::edges_view(snapshot);
    aida::analysis::fact_page_pin_t xref_pin;
    aida::analysis::fact_page_pin_t edge_pin;
    std::size_t budget = kProximityScanChunkBudget;
    while (budget != 0 && !state.capped && state.xref_cursor < xref_rows.size()) {
        auto xref_row = xref_rows.at(state.xref_cursor, xref_pin);
        if (!xref_row)
            break;
        ++state.xref_cursor;
        proximityConsumeRelation(state, context, xref_row.value()->source,
            xref_row.value()->target, proximity_relation_kind_t::xref);
        --budget;
    }
    while (budget != 0 && !state.capped && state.xref_cursor >= xref_rows.size() &&
           state.call_cursor < snapshot.call_graph.edges.size()) {
        const auto& item = snapshot.call_graph.edges[state.call_cursor++];
        proximityConsumeRelation(state, context, item.call_site, item.target,
            proximity_relation_kind_t::call);
        --budget;
    }
    while (budget != 0 && !state.capped && state.xref_cursor >= xref_rows.size() &&
           state.call_cursor >= snapshot.call_graph.edges.size() &&
           state.edge_cursor < edge_rows.size()) {
        auto edge_row = edge_rows.at(state.edge_cursor, edge_pin);
        if (!edge_row)
            break;
        ++state.edge_cursor;
        const auto& item = *edge_row.value();
        proximityConsumeRelation(state, context, item.source, item.target,
            item.kind == aida::analysis::edge_kind_t::call ||
            item.kind == aida::analysis::edge_kind_t::tail_call
                ? proximity_relation_kind_t::call : proximity_relation_kind_t::control_flow);
        --budget;
    }
    if (state.capped) {
        state.complete = true;
        return;
    }
    if (state.xref_cursor >= xref_rows.size() &&
        state.call_cursor >= snapshot.call_graph.edges.size() &&
        state.edge_cursor >= edge_rows.size())
        proximityFinishPass(state);
}

}

QtProximityBrowserView::QtProximityBrowserView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.analysis.proximity"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.proximity.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    const auto& tokens = theme::tokens();
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x, tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    back_button_ = new QToolButton(toolbar);
    back_button_->setObjectName(QStringLiteral("aida.proximity.back"));
    back_button_->setIcon(theme::icons::icon(QStringLiteral("caret-left")));
    back_button_->setAccessibleName(QStringLiteral("Back"));
    back_button_->setToolTip(QStringLiteral("Back to the previous neighborhood root"));
    forward_button_ = new QToolButton(toolbar);
    forward_button_->setObjectName(QStringLiteral("aida.proximity.forward"));
    forward_button_->setIcon(theme::icons::icon(QStringLiteral("caret-right")));
    forward_button_->setAccessibleName(QStringLiteral("Forward"));
    forward_button_->setToolTip(QStringLiteral("Forward to the next neighborhood root"));
    toolbar_layout->addWidget(back_button_);
    toolbar_layout->addWidget(forward_button_);
    use_selection_button_ = new QToolButton(toolbar);
    use_selection_button_->setObjectName(QStringLiteral("aida.proximity.use_selection"));
    use_selection_button_->setText(QStringLiteral("Use selection"));
    use_selection_button_->setToolTip(QStringLiteral(
        "Seed the neighborhood from the current disassembly selection"));
    toolbar_layout->addWidget(use_selection_button_);
    depth_combo_ = new QComboBox(toolbar);
    depth_combo_->setObjectName(QStringLiteral("aida.proximity.depth"));
    depth_combo_->setToolTip(QStringLiteral(
        "Maximum relationship hops from the neighborhood root"));
    depth_combo_->addItems({QStringLiteral("1 hop"), QStringLiteral("2 hops"),
        QStringLiteral("3 hops"), QStringLiteral("4 hops")});
    depth_combo_->setCurrentIndex(1);
    toolbar_layout->addWidget(depth_combo_);
    limit_combo_ = new QComboBox(toolbar);
    limit_combo_->setObjectName(QStringLiteral("aida.proximity.limit"));
    limit_combo_->setToolTip(QStringLiteral(
        "Maximum number of nodes retained in the neighborhood"));
    limit_combo_->addItems({QStringLiteral("96 nodes"), QStringLiteral("192 nodes"),
        QStringLiteral("384 nodes")});
    limit_combo_->setCurrentIndex(1);
    toolbar_layout->addWidget(limit_combo_);
    status_label_ = new QLabel(toolbar);
    status_label_->setObjectName(QStringLiteral("aida.proximity.status"));
    status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    toolbar_layout->addWidget(status_label_);
    toolbar_layout->addStretch(1);
    filter_debounce_ = new QTimer(this);
    filter_debounce_->setSingleShot(true);
    filter_debounce_->setInterval(300);
    filter_ = new widgets::AidaSearchField(QStringLiteral("Filter neighborhood..."),
        toolbar);
    filter_->setObjectName(QStringLiteral("aida.proximity.filter"));
    filter_->setClearButtonEnabled(true);
    toolbar_layout->addWidget(filter_, 1);
    layout->addWidget(toolbar);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("aida.proximity.progress"));
    progress_->setTextVisible(false);
    progress_->setRange(0, 100);
    progress_->setVisible(false);
    layout->addWidget(progress_);

    model_ = new QtProximityModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.proximity.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.compact_row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = table_->horizontalHeader();
    using Column = QtProximityModel::Column;
    const QFontMetricsF ui_metrics(table_->font());
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const auto with_cell_pad = [&tokens](int content) {
        return content + 2 * tokens.table.cell_pad_x + tokens.spacing.xs;
    };
    horizontal->setSectionResizeMode(static_cast<int>(Column::depth), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::depth),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Depth"))),
            static_cast<int>(ui_metrics.horizontalAdvance(QStringLiteral("16"))))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::address), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::address),
        with_cell_pad(static_cast<int>(code_metrics.horizontalAdvance(
            QStringLiteral("0xDDDDDDDDDDDDDDDD")))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::name), QHeaderView::Stretch);
    horizontal->setSectionResizeMode(static_cast<int>(Column::kind), QHeaderView::Stretch);
    horizontal->setSectionResizeMode(static_cast<int>(Column::in_out), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::in_out),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("In / Out"))),
            static_cast<int>(ui_metrics.horizontalAdvance(
                QStringLiteral("999 / 999"))))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::relationships),
        QHeaderView::Stretch);
    horizontal->setStretchLastSection(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.proximity.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);

    pump_ = new QTimer(this);
    pump_->setInterval(0);
    connect(pump_, &QTimer::timeout, this, [this] { pumpChunk(); });

    connect(back_button_, &QToolButton::clicked, this, [this] {
        if (!state_ || state_->history.empty() || state_->history_index == 0) return;
        --state_->history_index;
        navigateRoot(state_->history[state_->history_index], false);
    });
    connect(forward_button_, &QToolButton::clicked, this, [this] {
        if (!state_ || state_->history.empty() ||
            state_->history_index + 1 >= state_->history.size()) return;
        ++state_->history_index;
        navigateRoot(state_->history[state_->history_index], false);
    });
    connect(use_selection_button_, &QToolButton::clicked, this, [this] {
        useSelection();
    });
    connect(depth_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!state_ || state_->depth_limit == index + 1) return;
        state_->depth_limit = index + 1;
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        const auto context = disasm_view::capture_workspace(workspace);
        if (context) beginProximityProjection(*state_, context, state_->root);
        publishNodes();
        if (state_ && !state_->complete && isVisible()) pump_->start();
    });
    connect(limit_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!state_) return;
        const int limit = index == 0 ? 96 : (index == 2 ? 384 : 192);
        if (state_->node_limit == limit) return;
        state_->node_limit = limit;
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        const auto context = disasm_view::capture_workspace(workspace);
        if (context) beginProximityProjection(*state_, context, state_->root);
        publishNodes();
        if (state_ && !state_->complete && isVisible()) pump_->start();
    });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString&) {
        filter_debounce_->start();
    });
    connect(filter_debounce_, &QTimer::timeout, this, [this] {
        if (!state_) return;
        state_->filter = filter_->text();
        state_->filter_dirty = true;
        publishNodes();
        refreshPresentation();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!state_ || !current.isValid()) return;
        const auto* node = model_->rowAt(current.row());
        if (!node) return;
        state_->selected = model_->sourceIndexForViewRow(current.row());
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        const auto context = disasm_view::capture_workspace(workspace);
        if (context)
            disasm_view::select_address(node->address, context);
    });
    connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid()) return;
        const auto* node = model_->rowAt(index.row());
        if (!node) return;
        navigateRoot(node->address, true);
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        if (index.isValid())
            showNodeMenu(table_->viewport()->mapToGlobal(pos), index.row());
    });
    table_->installEventFilter(this);

    connect(&QtAnalysisBridge::instance(), &QtAnalysisBridge::activeContextChanged,
            this, [this](QtWorkspaceContext* context) { rebindContext(context); });
    rebindContext(QtAnalysisBridge::instance().activeContext());
}

void QtProximityBrowserView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (context_ && context_->poller())
        context_->poller()->arm();
    resetIfNeeded();
    if (state_ && !state_->complete)
        pump_->start();
}

void QtProximityBrowserView::hideEvent(QHideEvent* event) {
    pump_->stop();
    QWidget::hideEvent(event);
}

bool QtProximityBrowserView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Menu ||
            (key->key() == Qt::Key_F10 &&
                key->modifiers().testFlag(Qt::ShiftModifier))) {
            const auto current = table_->currentIndex();
            if (current.isValid())
                showNodeMenu(table_->viewport()->mapToGlobal(
                    table_->visualRect(current).center()), current.row());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QtProximityBrowserView::rebindContext(QtWorkspaceContext* context) {
    if (context_ == context) return;
    if (poller_connection_) disconnect(poller_connection_);
    context_ = context;
    state_ = context ? context->proximityState.get() : nullptr;
    if (state_) {
        if (!state_->filter.isEmpty() && filter_->text() != state_->filter)
            filter_->setText(state_->filter);
        poller_connection_ = connect(context_->poller(),
            &QtRevisionPoller::revisionsChanged, this,
            [this](quint64, quint64, quint64, quint64) {
                resetIfNeeded();
                publishNodes();
                refreshPresentation();
                if (state_ && !state_->complete && isVisible())
                    pump_->start();
            });
        publishNodes();
        refreshPresentation();
        if (isVisible()) {
            resetIfNeeded();
            if (state_ && !state_->complete) pump_->start();
        }
    } else {
        model_->clear();
        refreshPresentation();
    }
}

std::uint64_t QtProximityBrowserView::defaultRoot() const {
    const auto workspace = context_ ? context_->workspace().lock() : nullptr;
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context) return 0;
    const auto view_state = context.workspace ? context.workspace->view_state()
        : aida::analysis::workspace_view_state_t{};
    if (view_state.selection) {
        const auto runtime = disasm_view::runtime_address(context, *view_state.selection);
        if (runtime && *runtime != 0) {
            const auto function = disasm_view::enclosing_function_start(*runtime, context);
            return function != 0 ? function : *runtime;
        }
    }
    if (!context.publication || !context.publication->snapshot) return 0;
    const auto& snapshot = *context.publication->snapshot;
    if (!snapshot.functions.empty())
        return disasm_view::runtime_address(context, snapshot.functions.front().start)
            .value_or(snapshot.functions.front().start.value);
    if (!snapshot.symbols.empty())
        return disasm_view::runtime_address(context, snapshot.symbols.front().address)
            .value_or(snapshot.symbols.front().address.value);
    const auto xref_rows = aida::analysis::xrefs_view(snapshot);
    if (!xref_rows.empty()) {
        aida::analysis::fact_page_pin_t xref_pin;
        auto first = xref_rows.at(0, xref_pin);
        if (first)
            return disasm_view::runtime_address(context, first.value()->source)
                .value_or(first.value()->source.value);
    }
    return 0;
}

void QtProximityBrowserView::resetIfNeeded() {
    if (!state_ || !context_) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context || !context.publication) return;
    auto& state = *state_;
    const auto& publication = *context.publication;
    if (state.initialized && state.generation == publication.generation &&
        state.revision == publication.analysis_revision &&
        state.overlay_revision == publication.overlay_revision)
        return;
    state.initialized = true;
    state.generation = publication.generation;
    state.revision = publication.analysis_revision;
    state.overlay_revision = publication.overlay_revision;
    const std::uint64_t root = defaultRoot();
    state.history.clear();
    if (root != 0) state.history.push_back(root);
    state.history_index = 0;
    beginProximityProjection(state, context, root);
}

void QtProximityBrowserView::pumpChunk() {
    if (!state_ || !context_) {
        pump_->stop();
        return;
    }
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context) {
        pump_->stop();
        return;
    }
    advanceProximityScan(*state_, context);
    publishNodes();
    refreshPresentation();
    if (state_->complete)
        pump_->stop();
}

void QtProximityBrowserView::publishNodes() {
    if (!state_) return;
    auto& state = *state_;
    std::vector<std::size_t> visible;
    const std::string requested = state.filter.toLower().toStdString();
    visible.reserve(state.nodes.size());
    for (std::size_t index = 0; index < state.nodes.size(); ++index) {
        const auto& node = state.nodes[index];
        if (requested.empty() || contains_case_insensitive(node.name, requested) ||
            contains_case_insensitive(node.kind, requested) ||
            [&] {
                char buf[32]{};
                std::snprintf(buf, sizeof(buf), "0x%016llX",
                    static_cast<unsigned long long>(node.address));
                return contains_case_insensitive(buf, requested);
            }() ||
            contains_case_insensitive(proximity_relation_summary(node), requested))
            visible.push_back(index);
    }
    std::stable_sort(visible.begin(), visible.end(), [&](std::size_t left, std::size_t right) {
        const auto& lhs = state.nodes[left];
        const auto& rhs = state.nodes[right];
        if (lhs.depth != rhs.depth) return lhs.depth < rhs.depth;
        if (lhs.name != rhs.name) return lhs.name < rhs.name;
        return lhs.address < rhs.address;
    });
    if (state.selected >= state.nodes.size() ||
        std::find(visible.begin(), visible.end(), state.selected) == visible.end())
        state.selected = visible.empty() ? static_cast<std::size_t>(-1) : visible.front();
    state.filter_dirty = false;
    model_->setNodes(state.nodes, std::move(visible));
}

void QtProximityBrowserView::refreshPresentation() {
    if (!state_ || !context_ || context_->workspace().expired()) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No analysis workspace"));
        state_view_->setMessage(QStringLiteral(
            "Open and analyze a binary to browse related functions, symbols and references."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        progress_->setVisible(false);
        status_label_->clear();
        return;
    }
    const auto& state = *state_;
    const QString scanning = state.capped ? QStringLiteral(" | capped")
        : (state.complete ? QString() : QStringLiteral(" | scanning"));
    status_label_->setText(state.skipped_relations != 0
        ? QStringLiteral("%1 nodes | %2 edges | %3 unmapped%4")
            .arg(state.nodes.size()).arg(state.relations.size())
            .arg(state.skipped_relations).arg(scanning)
        : QStringLiteral("%1 nodes | %2 edges%3")
            .arg(state.nodes.size()).arg(state.relations.size()).arg(scanning));
    back_button_->setEnabled(!state.history.empty() && state.history_index != 0);
    forward_button_->setEnabled(!state.history.empty() &&
        state.history_index + 1 < state.history.size());
    if (!state.complete) {
        const auto workspace = context_->workspace().lock();
        const auto context = disasm_view::capture_workspace(workspace);
        std::size_t total = 0;
        std::size_t current = state.xref_cursor + state.call_cursor + state.edge_cursor;
        if (context && context.publication && context.publication->snapshot) {
            const auto& snapshot = *context.publication->snapshot;
            total = static_cast<std::size_t>(aida::analysis::xrefs_view(snapshot).size()) +
                snapshot.call_graph.edges.size() +
                static_cast<std::size_t>(aida::analysis::edges_view(snapshot).size());
        }
        const float pass_fraction = total == 0 ? 1.0f
            : static_cast<float>(current) / static_cast<float>(total);
        const float fraction = (static_cast<float>(state.pass) + pass_fraction) /
            static_cast<float>((std::max)(1, state.depth_limit));
        progress_->setVisible(true);
        progress_->setRange(0, 100);
        progress_->setValue(static_cast<int>((std::min)(1.0f, fraction) * 100.0f));
    } else {
        progress_->setVisible(false);
    }
    if (state.root == 0) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No navigable analysis entity"));
        state_view_->setMessage(QStringLiteral(
            "The current publication contains no function, symbol or reference address that can seed a proximity neighborhood."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (state.complete && state.nodes.size() == 1) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No published neighbors"));
        state_view_->setMessage(QStringLiteral(
            "No xref, call-graph or control-flow publication connects to the selected root within the requested depth."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (model_->rowCount() == 0) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No matches"));
        state_view_->setMessage(QStringLiteral(
            "No neighborhood entity matches the current filter."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    state_view_->setVisible(false);
    table_->setVisible(true);
}

void QtProximityBrowserView::navigateRoot(std::uint64_t root, bool record_history) {
    if (!state_ || root == 0) return;
    const auto workspace = context_ ? context_->workspace().lock() : nullptr;
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context) return;
    auto& state = *state_;
    if (record_history) {
        if (!state.history.empty() && state.history_index + 1 < state.history.size())
            state.history.erase(state.history.begin() +
                static_cast<std::ptrdiff_t>(state.history_index + 1),
                state.history.end());
        if (state.history.empty() || state.history.back() != root)
            state.history.push_back(root);
        state.history_index = state.history.size() - 1;
    }
    beginProximityProjection(state, context, root);
    publishNodes();
    refreshPresentation();
    if (!state.complete && isVisible()) pump_->start();
}

void QtProximityBrowserView::useSelection() {
    const auto root = defaultRoot();
    if (root != 0) navigateRoot(root, true);
}

void QtProximityBrowserView::showNodeMenu(const QPoint& global_pos, int view_row) {
    if (!state_ || !context_) return;
    const auto* node = model_->rowAt(view_row);
    if (!node) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context || !context.publication) return;
    state_->selected = model_->sourceIndexForViewRow(view_row);
    disasm_view::select_address(node->address, context, false);
    auto* state_ptr = state_;
    const std::uint64_t root = state_->root;
    const auto retained_address = node->address;
    const auto retained_name = node->name;
    const bool mapped = disasm_view::typed_address(context, node->address).has_value();
    aida::ui::application_ui::retained_entity_context_t menu;
    menu.owner_id = "analysis.proximity";
    menu.menu = aida::ui::stable_menu_id_t("menu.analysis.function");
    menu.entity_id = "proximity:" + std::to_string(node->address) + ":" + node->name +
        ":" + node->kind;
    menu.entity_generation = context.publication->generation ^
        (context.publication->analysis_revision + 0x9E3779B97F4A7C15ULL) ^ root;
    menu.validate_identity = [state_ptr, retained_address, retained_name, workspace,
                              root, generation = menu.entity_generation]() {
        const std::uint64_t live = workspace ? workspace->generation() ^
            (workspace->analysis_revision() + 0x9E3779B97F4A7C15ULL) ^ root : 0;
        if (live != generation)
            return aida::ui::capability_state_t::unavailable(
                "The analysis selection is stale; select the item again");
        return state_ptr->selected < state_ptr->nodes.size() &&
            state_ptr->nodes[state_ptr->selected].address == retained_address &&
            state_ptr->nodes[state_ptr->selected].name == retained_name
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The selected proximity entity changed");
    };
    using qt_analysis_menus::add_action;
    add_action(menu, "analysis.navigate.follow", true, "",
        [this, retained_address] {
            navigateRoot(retained_address, true);
            return aida::ui::action_handler_result_t::completed();
        });
    const auto summary = proximity_relation_summary(*node);
    qt_analysis_menus::fill_analysis_entity_actions(menu, context, node->address,
        mapped, node->name, node->kind, summary);
    if (!mapped) {
        // The plan's per-action unavailability reasons (verbatim messages).
        for (auto& action : menu.actions) {
            if (action.action_id.rfind("analysis.navigate.", 0) == 0 &&
                action.action_id != "analysis.navigate.follow")
                action.capability = aida::ui::capability_state_t::unavailable(
                    "The published entity is outside the mapped workspace address space");
            if (action.action_id == "analysis.modify.rename" ||
                action.action_id == "analysis.modify.comment")
                action.capability = aida::ui::capability_state_t::unavailable(
                    "The published entity is outside the mapped workspace address space");
        }
    }
    add_action(menu, "analysis.modify.bookmark", mapped,
        "The published entity is outside the mapped workspace address space",
        [context, address = node->address, label = node->name] {
            const auto typed = disasm_view::typed_address(context, address);
            return typed && disasm_view::queue_bookmark(context, *typed, label)
                ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(
                    "The workspace rejected the bookmark request");
        });
    QtAnalysisBridge::instance().showRetainedMenu(menu,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

bool QtProximityBrowserView::drillCapable() const {
    // Ports selected_drill_capability (07 sec. 5.2).
    if (!state_ || !context_) return false;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context || !context.publication || !context.publication->snapshot)
        return false;
    const auto& state = *state_;
    if (!state.initialized ||
        state.generation != context.publication->generation ||
        state.revision != context.publication->analysis_revision ||
        state.overlay_revision != context.publication->overlay_revision)
        return false;
    return state.selected < state.nodes.size();
}

bool QtProximityBrowserView::drillSelected() {
    if (!drillCapable()) return false;
    navigateRoot(state_->nodes[state_->selected].address, true);
    return true;
}

}
