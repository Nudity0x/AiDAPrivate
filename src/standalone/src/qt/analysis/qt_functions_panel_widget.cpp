#include "qt/analysis/qt_functions_panel_widget.hpp"

#include <QFontMetricsF>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QResizeEvent>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/analysis/workspace/overlay_journal.hpp"
#include "core/analysis/workspace/publication_indexes.hpp"
#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/analysis/symbol_store.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/infra/executor.hpp"
#include "core/session/analysis_session.hpp"
#include "core/ui/context_menu_contract.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_analysis_menu_builder.hpp"
#include "qt/analysis/qt_gui_post.hpp"
#include "qt/analysis/qt_revision_poller.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_progress.hpp"
#include "qt/widgets/aida_search_field.hpp"
#include "qt/widgets/aida_state_view.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <QPainter>
#include <QStyledItemDelegate>

namespace aida::qt::analysis {

namespace {

// Compact-mode name-cell delegate (07 sec. 4.1): paints the type glyph
// (diamond = synthetic, circle = .text, rect = other) left of the name.
class QtFunctionNameDelegate : public QStyledItemDelegate {
public:
    explicit QtFunctionNameDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem adjusted(option);
        initStyleOption(&adjusted, index);
        const auto* entry = modelEntry(index);
        if (!entry) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const QRect content = option.rect;
        const auto& tokens = theme::tokens();
        const qreal glyph = static_cast<qreal>(tokens.status_bar.dot);
        const qreal glyph_cx = content.left() + glyph + tokens.spacing.xs +
            tokens.spacing.xxs;
        const QPointF center(glyph_cx, content.top() + content.height() * 0.5);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (entry->synthetic_name) {
            QPolygonF diamond;
            diamond << QPointF(center.x(), center.y() - glyph)
                    << QPointF(center.x() + glyph, center.y())
                    << QPointF(center.x(), center.y() + glyph)
                    << QPointF(center.x() - glyph, center.y());
            painter->setPen(QPen(tokens.text_dim, 1.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawPolygon(diamond);
        } else if (entry->section == ".text") {
            painter->setPen(Qt::NoPen);
            painter->setBrush(tokens.accent_dim);
            painter->drawEllipse(center, glyph, glyph);
        } else {
            painter->setPen(QPen(tokens.text_secondary, 1.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(QRectF(center.x() - glyph,
                center.y() - tokens.spacing.xs, 2.0 * glyph,
                static_cast<qreal>(tokens.spacing.sm)));
        }
        painter->restore();
        adjusted.rect.adjust(static_cast<int>(2.0 * glyph_cx), 0, 0, 0);
        QStyledItemDelegate::paint(painter, adjusted, index);
    }

private:
    static const qt_function_entry_t* modelEntry(const QModelIndex& index) {
        const auto* model = qobject_cast<const QtFunctionsModel*>(index.model());
        return model ? model->entryAt(index.row()) : nullptr;
    }
};

std::string to_lower_copy(const std::string& s) {
    std::string out;
    out.resize(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return out;
}

int compare_case_insensitive(const std::string& lhs, const std::string& rhs) {
    const std::size_t shared = (std::min)(lhs.size(), rhs.size());
    for (std::size_t index = 0; index < shared; ++index) {
        const int left = std::tolower(static_cast<unsigned char>(lhs[index]));
        const int right = std::tolower(static_cast<unsigned char>(rhs[index]));
        if (left < right) return -1;
        if (left > right) return 1;
    }
    if (lhs.size() < rhs.size()) return -1;
    if (lhs.size() > rhs.size()) return 1;
    return 0;
}

std::string make_synthetic_name(std::uint64_t addr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "sub_%llX", static_cast<unsigned long long>(addr));
    return std::string(buf);
}

std::string strip_module_prefix(const std::string& s) {
    auto pos = s.find('!');
    if (pos == std::string::npos) return s;
    return s.substr(pos + 1);
}

std::string normalize_function_name(std::string name) {
    name = strip_module_prefix(name);
    const auto first = std::find_if(name.begin(), name.end(), [](unsigned char value) {
        return !std::isspace(value);
    });
    const auto last = std::find_if(name.rbegin(), name.rend(), [](unsigned char value) {
        return !std::isspace(value);
    }).base();
    if (first >= last) return {};
    name = std::string(first, last);
    if (name.size() > 2 && name[0] == '0' &&
        (name[1] == 'x' || name[1] == 'X') &&
        std::all_of(name.begin() + 2, name.end(), [](unsigned char value) {
            return std::isxdigit(value) != 0;
        }))
        return {};
    return name;
}

}

QtFunctionsPanelWidget::QtFunctionsPanelWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.analysis.functions"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.functions.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    const auto& tokens = theme::tokens();
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x, tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    auto* title = new QLabel(QStringLiteral("Functions"), toolbar);
    title->setObjectName(QStringLiteral("aida.functions.title"));
    toolbar_layout->addWidget(title);
    filter_debounce_ = new QTimer(this);
    filter_debounce_->setSingleShot(true);
    filter_debounce_->setInterval(300);
    filter_ = new widgets::AidaSearchField(QStringLiteral("Filter functions..."), toolbar);
    filter_->setObjectName(QStringLiteral("aida.functions.filter"));
    filter_->setClearButtonEnabled(true);
    toolbar_layout->addWidget(filter_, 1);
    count_label_ = new QLabel(toolbar);
    count_label_->setObjectName(QStringLiteral("aida.functions.count"));
    count_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    toolbar_layout->addWidget(count_label_);
    layout->addWidget(toolbar);

    loading_ = new widgets::AidaProgressBar(this);
    loading_->setObjectName(QStringLiteral("aida.functions.loading"));
    loading_->setIndeterminate(true);
    loading_->setBarHeight(tokens.spacing.xxs);
    loading_->setVisible(false);
    layout->addWidget(loading_);

    model_ = new QtFunctionsModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.functions.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.compact_row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = table_->horizontalHeader();
    const QFontMetricsF ui_metrics(table_->font());
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const auto with_cell_pad = [&tokens](int content) {
        return content + 2 * tokens.table.cell_pad_x + tokens.spacing.xs;
    };
    horizontal->setSectionResizeMode(
        static_cast<int>(QtFunctionsModel::Column::name), QHeaderView::Stretch);
    horizontal->setSectionResizeMode(
        static_cast<int>(QtFunctionsModel::Column::size), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(QtFunctionsModel::Column::size),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Size"))),
            static_cast<int>(code_metrics.horizontalAdvance(
                QStringLiteral("65536 (999.9K)"))))));
    horizontal->setSectionResizeMode(
        static_cast<int>(QtFunctionsModel::Column::section), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(QtFunctionsModel::Column::section),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Section"))),
            static_cast<int>(code_metrics.horizontalAdvance(
                QStringLiteral(".rdata"))))));
    horizontal->setSectionResizeMode(
        static_cast<int>(QtFunctionsModel::Column::calls), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(QtFunctionsModel::Column::calls),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Calls"))),
            static_cast<int>(code_metrics.horizontalAdvance(
                QStringLiteral("9999/9999"))))));
    horizontal->setStretchLastSection(false);
    horizontal->setSectionsClickable(true);
    horizontal->setSortIndicatorShown(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    name_delegate_ = new QtFunctionNameDelegate(this);
    table_->setItemDelegateForColumn(
        static_cast<int>(QtFunctionsModel::Column::name), name_delegate_);
    table_->installEventFilter(this);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.functions.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);

    connect(filter_, &QLineEdit::textChanged, this, [this](const QString&) {
        filter_debounce_->start();
    });
    connect(filter_debounce_, &QTimer::timeout, this, [this] {
        if (!state_) return;
        const QString next = filter_->text();
        if (state_->filter == next) return;
        state_->filter = next;
        {
            std::lock_guard<std::mutex> lock(state_->mtx);
            state_->filter_dirty = true;
        }
        submitFilterSort();
    });
    connect(horizontal, &QHeaderView::sectionClicked, this, [this](int logical) {
        if (!state_) return;
        {
            std::lock_guard<std::mutex> lock(state_->mtx);
            const bool ascending =
                state_->sort_column == logical ? !state_->sort_ascending : true;
            state_->sort_column = logical;
            state_->sort_ascending = ascending;
            state_->sort_dirty = true;
        }
        submitFilterSort();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!state_ || !current.isValid()) return;
        const auto* entry = model_->entryAt(current.row());
        if (!entry) return;
        state_->selected_row = current.row();
        state_->selected_addr = entry->address;
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        const auto context = disasm_view::capture_workspace(workspace);
        if (context)
            disasm_view::select_address(entry->address, context);
    });
    connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid()) return;
        const auto* entry = model_->entryAt(index.row());
        if (!entry) return;
        const auto workspace = context_ ? context_->workspace().lock() : nullptr;
        QtAnalysisBridge::instance().navigateTo(workspace, entry->address,
            "document.disassembly");
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        showFunctionMenu(table_->viewport()->mapToGlobal(pos),
            index.isValid() ? index.row() : -1);
    });

    connect(&QtAnalysisBridge::instance(), &QtAnalysisBridge::activeContextChanged,
            this, [this](QtWorkspaceContext* context) { rebindContext(context); });
    rebindContext(QtAnalysisBridge::instance().activeContext());
    applyAdaptiveColumns();
}

void QtFunctionsPanelWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (context_ && context_->poller())
        context_->poller()->arm();
    refreshFromWorkspace();
}

void QtFunctionsPanelWidget::rebindContext(QtWorkspaceContext* context) {
    if (context_ == context) return;
    if (poller_connection_) disconnect(poller_connection_);
    context_ = context;
    state_ = context ? context->functionsState : nullptr;
    if (state_) {
        if (!state_->filter.isEmpty() && filter_->text() != state_->filter)
            filter_->setText(state_->filter);
        poller_connection_ = connect(context_->poller(),
            &QtRevisionPoller::revisionsChanged, this,
            [this](quint64, quint64, quint64, quint64) { refreshFromWorkspace(); });
        model_->setPresentation(state_->presentation);
        refreshPresentation();
        refreshFromWorkspace();
    } else {
        model_->setPresentation(nullptr);
        refreshPresentation();
    }
}

void QtFunctionsPanelWidget::refreshFromWorkspace() {
    if (!state_ || !context_) return;
    const auto workspace = context_->workspace().lock();
    if (!workspace) {
        model_->setPresentation(nullptr);
        refreshPresentation();
        return;
    }
    auto state_handle = state_;
    auto publication = workspace->analysis_publication();
    if (!publication || !publication->snapshot) {
        if (workspace->target_kind() == aida::analysis::target_kind_t::live_snapshot) {
            std::lock_guard<std::mutex> lock(state_handle->mtx);
            state_handle->entries =
                std::make_shared<const std::vector<qt_function_entry_t>>();
            state_handle->presentation =
                std::make_shared<const qt_functions_presentation_t>();
            state_handle->cached_module_base = workspace->identity().image_base();
            state_handle->cached_module_size = workspace->identity().module()
                ? static_cast<std::uint32_t>((std::min<std::uint64_t>)(
                    workspace->identity().module()->size, UINT32_MAX)) : 0;
            state_handle->cached_module_name = workspace->identity().bin_name();
            state_handle->ready.store(true, std::memory_order_release);
            state_handle->building.store(false, std::memory_order_release);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_handle->mtx);
        const auto current_symbols = analysis_session::symbols_for_workspace(workspace);
        const std::uint64_t symbol_revision = current_symbols
            ? current_symbols->revision() : 0;
        if (state_handle->cached_generation == publication->generation &&
            state_handle->cached_analysis_revision == publication->analysis_revision &&
            state_handle->cached_overlay_revision == publication->overlay_revision &&
            state_handle->cached_symbol_revision == symbol_revision)
            return;
    }
    bool expected = false;
    if (!state_handle->building.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return;
    state_handle->ready.store(false, std::memory_order_release);
    loading_->setVisible(true);
    const auto snapshot = publication->snapshot;
    const auto overlay = workspace->overlay();
    const auto debug_symbols = analysis_session::symbols_for_workspace(workspace);
    const std::uint64_t debug_symbol_revision = debug_symbols
        ? debug_symbols->revision() : 0;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "analysis";
    submission.label = "analysis.functions_panel.workspace_projection";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 2;
    submission.generation = publication->generation;
    submission.body = [this, workspace, state_handle, publication, snapshot, overlay,
        debug_symbols, debug_symbol_revision]() {
        std::unordered_map<aida::analysis::entity_id_t, std::string> symbols;
        symbols.reserve(snapshot->symbols.size());
        for (const auto& symbol : snapshot->symbols) {
            if (!symbol.name.empty()) symbols.emplace(symbol.id, symbol.name);
        }
        const auto image = snapshot->image;
        std::unordered_map<std::uint64_t, std::string> debug_symbol_names;
        if (debug_symbols) {
            const auto mode = image ? image->architecture_mode() :
                (workspace->identity().architecture() ==
                    aida::analysis::architecture_id_t::x86_64
                    ? aida::analysis::architecture_mode_t::x86_64
                    : (workspace->identity().architecture() ==
                        aida::analysis::architecture_id_t::x86
                        ? aida::analysis::architecture_mode_t::x86_32
                        : aida::analysis::architecture_mode_t::unknown));
            auto entries = debug_symbols->function_snapshot(
                workspace->identity().architecture(), mode);
            debug_symbol_names.reserve(entries.size());
            for (auto& entry : entries) {
                if (!entry.name.empty())
                    debug_symbol_names.emplace(entry.address.value,
                        std::move(entry.name));
            }
        }
        std::unordered_map<std::uint64_t, std::string> overlay_names;
        if (overlay) {
            const auto overlay_snapshot = overlay->snapshot();
            if (overlay_snapshot.revision == publication->overlay_revision) {
                for (const auto& item : overlay_snapshot.items) {
                    const auto& operation = item.second;
                    if (operation.kind != aida::analysis::overlay_operation_kind_t::name ||
                        operation.name.empty())
                        continue;
                    std::uint64_t operation_va = 0;
                    if (operation.address.space ==
                        aida::analysis::address_space_id_t::relative_virtual) {
                        if (!image || operation.address.value >= image->image_size() ||
                            image->image_base() > UINT64_MAX - operation.address.value)
                            continue;
                        operation_va = image->image_base() + operation.address.value;
                    } else if (operation.address.space ==
                        aida::analysis::address_space_id_t::virtual_address ||
                        operation.address.space ==
                        aida::analysis::address_space_id_t::live_virtual) {
                        operation_va = operation.address.value;
                    } else if (image) {
                        auto rva = image->file_offset_to_rva(operation.address.value);
                        if (!rva || image->image_base() > UINT64_MAX - rva.value())
                            continue;
                        operation_va = image->image_base() + rva.value();
                    }
                    if (operation_va != 0)
                        overlay_names.emplace(operation_va, operation.name);
                }
            }
        }
        const auto indexes = aida::analysis::publication_indexes::for_publication(
            publication, workspace->cancellation_token());
        const bool use_index = indexes && indexes->functions_sorted_disjoint();
        if (!use_index) {
            diag::log_tagged_fmt("functions_panel",
                "workspace_projection_index_fallback binary_id=%s reason=%s",
                workspace->identity().binary_id().to_hex().c_str(),
                indexes ? "functions_unverified" : "index_unavailable");
        }
        std::vector<const aida::analysis::function_record_t*> ordered;
        ordered.reserve(snapshot->functions.size());
        for (const auto& function : snapshot->functions)
            ordered.push_back(&function);
        if (!use_index) {
            std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
                return left->start < right->start;
            });
        }
        std::unordered_map<aida::analysis::entity_id_t, std::uint32_t> calls_in;
        std::unordered_map<aida::analysis::entity_id_t, std::uint32_t> calls_out;
        if (!use_index) {
            auto enclosing = [&](const aida::analysis::address_t& address)
                -> const aida::analysis::function_record_t* {
                auto found = std::upper_bound(ordered.begin(), ordered.end(), address,
                    [](const auto& value, const auto* function) {
                        return value < function->start;
                    });
                if (found == ordered.begin()) return nullptr;
                --found;
                const auto* function = *found;
                if (address.space != function->start.space ||
                    address.value < function->start.value ||
                    address.value >= function->end.value)
                    return nullptr;
                return function;
            };
            for (const auto& edge : snapshot->edges) {
                if (workspace->cancellation_token().stop_requested()) {
                    state_handle->building.store(false, std::memory_order_release);
                    return;
                }
                if (edge.kind != aida::analysis::edge_kind_t::call &&
                    edge.kind != aida::analysis::edge_kind_t::tail_call)
                    continue;
                const auto* caller = enclosing(edge.source);
                const auto* callee = enclosing(edge.target);
                if (caller && calls_out[caller->id] != UINT32_MAX) ++calls_out[caller->id];
                if (callee && calls_in[callee->id] != UINT32_MAX) ++calls_in[callee->id];
            }
        }
        std::vector<qt_function_entry_t> entries;
        entries.reserve(ordered.size());
        for (const auto* function : ordered) {
            if (workspace->cancellation_token().stop_requested()) {
                state_handle->building.store(false, std::memory_order_release);
                return;
            }
            qt_function_entry_t entry;
            std::uint64_t function_rva = 0;
            bool have_function_rva = false;
            if (function->start.space == aida::analysis::address_space_id_t::relative_virtual) {
                if (!image || function->start.value >= image->image_size() ||
                    image->image_base() > UINT64_MAX - function->start.value)
                    continue;
                function_rva = function->start.value;
                have_function_rva = true;
                entry.address = image->image_base() + function_rva;
            } else if (function->start.space == aida::analysis::address_space_id_t::virtual_address ||
                function->start.space == aida::analysis::address_space_id_t::live_virtual) {
                entry.address = function->start.value;
                if (image && entry.address >= image->image_base()) {
                    function_rva = entry.address - image->image_base();
                    have_function_rva = true;
                }
            } else {
                continue;
            }
            const std::uint64_t span = function->end.value > function->start.value
                ? function->end.value - function->start.value : 0;
            entry.size = static_cast<std::uint32_t>((std::min<std::uint64_t>)(span, UINT32_MAX));
            const auto overlay_name = overlay_names.find(entry.address);
            if (overlay_name != overlay_names.end())
                entry.name = overlay_name->second;
            if (entry.name.empty()) {
                const auto debug_name = debug_symbol_names.find(entry.address);
                if (debug_name != debug_symbol_names.end())
                    entry.name = debug_name->second;
            }
            if (entry.name.empty() && function->symbol_id) {
                const auto found = symbols.find(*function->symbol_id);
                if (found != symbols.end()) entry.name = found->second;
            }
            entry.name = normalize_function_name(std::move(entry.name));
            entry.synthetic_name = entry.name.empty();
            if (entry.synthetic_name) entry.name = make_synthetic_name(entry.address);
            if (image && have_function_rva) {
                const auto* section = image->section_for_rva(function_rva);
                if (section) entry.section = section->name;
            }
            if (use_index) {
                const auto ordinal = static_cast<std::size_t>(
                    function - snapshot->functions.data());
                const auto degree = indexes->function_call_degree(ordinal);
                entry.calls_in = degree.first;
                entry.calls_out = degree.second;
            } else {
                entry.calls_in = calls_in[function->id];
                entry.calls_out = calls_out[function->id];
            }
            entries.push_back(std::move(entry));
        }
        {
            std::lock_guard<std::mutex> lock(state_handle->mtx);
            if (workspace->generation() != publication->generation) {
                state_handle->building.store(false, std::memory_order_release);
                return;
            }
            state_handle->entries =
                std::make_shared<const std::vector<qt_function_entry_t>>(std::move(entries));
            state_handle->presentation =
                std::make_shared<const qt_functions_presentation_t>();
            state_handle->cached_module_base = workspace->identity().image_base();
            state_handle->cached_module_size = image ? image->image_size() : 0;
            state_handle->cached_module_name = workspace->identity().bin_name();
            state_handle->cached_generation = publication->generation;
            state_handle->cached_analysis_revision = publication->analysis_revision;
            state_handle->cached_overlay_revision = publication->overlay_revision;
            state_handle->cached_symbol_revision = debug_symbol_revision;
            state_handle->filter_dirty = true;
            state_handle->sort_dirty = true;
        }
        state_handle->ready.store(true, std::memory_order_release);
        state_handle->building.store(false, std::memory_order_release);
        gui_post(this, [this] {
            loading_->setVisible(false);
            submitFilterSort();
        });
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        state_handle->building.store(false, std::memory_order_release);
        loading_->setVisible(false);
        diag::log_tagged_fmt("functions_panel",
            "workspace_projection_submit_failed binary_id=%s reason=%s",
            workspace->identity().binary_id().to_hex().c_str(),
            submitted.reject_reason.c_str());
    }
}

void QtFunctionsPanelWidget::submitFilterSort() {
    if (!state_) return;
    auto state_handle = state_;
    auto& s = *state_handle;
    std::string current = to_lower_copy(s.filter.toStdString());
    std::shared_ptr<const std::vector<qt_function_entry_t>> entries;
    int column = 0;
    bool ascending = true;
    std::uint64_t serial = 0;
    {
        std::lock_guard<std::mutex> lock(s.mtx);
        if (!s.filter_dirty && !s.sort_dirty && current == s.last_filter_lower)
            return;
        s.last_filter_lower = current;
        s.filter_dirty = false;
        s.sort_dirty = false;
        entries = s.entries;
        column = s.sort_column;
        ascending = s.sort_ascending;
        serial = s.filter_sort_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    s.filter_sort_building.store(true, std::memory_order_release);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "analysis";
    submission.label = "analysis.functions_panel.filter_sort";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 2;
    submission.body = [this, state_handle, entries, current, column, ascending, serial]() {
        auto& state = *state_handle;
        std::vector<int> filtered;
        filtered.reserve(entries ? entries->size() : 0);
        if (current.empty()) {
            for (int index = 0; entries && index < static_cast<int>(entries->size()); ++index)
                filtered.push_back(index);
        } else {
            std::string addr_query = current;
            if (addr_query.size() > 2 && addr_query[0] == '0' && addr_query[1] == 'x')
                addr_query = addr_query.substr(2);
            char addr_buf[32];
            for (int index = 0; entries && index < static_cast<int>(entries->size()); ++index) {
                const auto& entry = (*entries)[static_cast<std::size_t>(index)];
                std::snprintf(addr_buf, sizeof(addr_buf), "%llx",
                    static_cast<unsigned long long>(entry.address));
                bool matched = std::strstr(addr_buf, addr_query.c_str()) != nullptr;
                if (!matched)
                    matched = to_lower_copy(entry.name).find(current) != std::string::npos;
                if (!matched && !entry.section.empty())
                    matched = to_lower_copy(entry.section).find(current) != std::string::npos;
                if (matched) filtered.push_back(index);
            }
        }
        auto compare = [column, ascending, &entries](int left, int right) {
            const auto& a = (*entries)[static_cast<std::size_t>(left)];
            const auto& b = (*entries)[static_cast<std::size_t>(right)];
            int c = 0;
            switch (column) {
                case 0:
                    c = compare_case_insensitive(a.name, b.name);
                    break;
                case 1:
                    if (a.size < b.size) c = -1;
                    else if (a.size > b.size) c = 1;
                    break;
                case 2:
                    c = compare_case_insensitive(a.section, b.section);
                    break;
                case 3: {
                    std::uint64_t ax = static_cast<std::uint64_t>(a.calls_in)
                        + static_cast<std::uint64_t>(a.calls_out);
                    std::uint64_t bx = static_cast<std::uint64_t>(b.calls_in)
                        + static_cast<std::uint64_t>(b.calls_out);
                    if (ax < bx) c = -1;
                    else if (ax > bx) c = 1;
                    break;
                }
                default:
                    c = compare_case_insensitive(a.name, b.name);
                    break;
            }
            if (c == 0) {
                if (a.address < b.address) c = -1;
                else if (a.address > b.address) c = 1;
            }
            return ascending ? (c < 0) : (c > 0);
        };
        if (entries)
            std::sort(filtered.begin(), filtered.end(), compare);
        auto presentation = std::make_shared<qt_functions_presentation_t>();
        presentation->entries = entries;
        presentation->sorted_indices = std::move(filtered);
        presentation->row_by_address.reserve(presentation->sorted_indices.size());
        for (std::size_t row = 0; row < presentation->sorted_indices.size(); ++row) {
            const int source = presentation->sorted_indices[row];
            if (entries && source >= 0 && source < static_cast<int>(entries->size()))
                presentation->row_by_address.emplace(
                    (*entries)[static_cast<std::size_t>(source)].address, row);
        }
        const char* drop_reason = nullptr;
        {
            std::lock_guard<std::mutex> lock(state.mtx);
            if (state.entries != entries) {
                state.filter_dirty = true;
                state.sort_dirty = true;
                drop_reason = "entries_replaced";
            } else if (state.filter_sort_serial.load(std::memory_order_acquire) != serial) {
                drop_reason = "superseded";
            } else {
                if (state.selected_addr != 0) {
                    const auto selected = presentation->row_by_address.find(state.selected_addr);
                    if (selected == presentation->row_by_address.end()) {
                        state.selected_row = -1;
                        state.selected_addr = 0;
                    } else {
                        state.selected_row = static_cast<int>(selected->second);
                    }
                }
                state.presentation = std::move(presentation);
            }
        }
        if (drop_reason != nullptr)
            diag::log_tagged_fmt("functions_panel",
                "filter_sort_stale_drop serial=%llu reason=%s",
                static_cast<unsigned long long>(serial), drop_reason);
        if (state_handle->filter_sort_serial.load(std::memory_order_acquire) == serial)
            state_handle->filter_sort_building.store(false, std::memory_order_release);
        gui_post(this, [this, state_handle, serial] {
            if (state_handle->filter_sort_serial.load(std::memory_order_acquire) != serial)
                return;
            adoptPresentation(state_handle->presentation, serial);
        });
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        s.filter_sort_building.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(s.mtx);
            s.filter_dirty = true;
            s.sort_dirty = true;
        }
        diag::log_tagged_fmt("functions_panel",
            "filter_sort_submit_failed serial=%llu reason=%s",
            static_cast<unsigned long long>(serial),
            submitted.reject_reason.c_str());
    } else {
        diag::log_tagged_fmt("functions_panel",
            "filter_sort_submit serial=%llu column=%d ascending=%d filter=%s",
            static_cast<unsigned long long>(serial), column, ascending ? 1 : 0,
            current.c_str());
    }
}

void QtFunctionsPanelWidget::adoptPresentation(
    std::shared_ptr<const qt_functions_presentation_t> presentation,
    std::uint64_t) {
    if (!state_) return;
    model_->setPresentation(std::move(presentation));
    refreshPresentation();
    if (state_->selected_row >= 0 &&
        state_->selected_row < model_->rowCount())
        table_->setCurrentIndex(model_->index(state_->selected_row, 0));
    if (state_->sort_column >= 0)
        table_->horizontalHeader()->setSortIndicator(state_->sort_column,
            state_->sort_ascending ? Qt::AscendingOrder : Qt::DescendingOrder);
}

void QtFunctionsPanelWidget::applyAdaptiveColumns() {
    const int w = table_->viewport()->width();
    compact_mode_ = w < 420;
    const bool show_size = w >= 520;
    const bool show_section = w >= 640;
    const bool show_calls = w >= 760;
    table_->setColumnHidden(static_cast<int>(QtFunctionsModel::Column::size), !show_size);
    table_->setColumnHidden(static_cast<int>(QtFunctionsModel::Column::section),
        !show_section);
    table_->setColumnHidden(static_cast<int>(QtFunctionsModel::Column::calls),
        !show_calls);
}

void QtFunctionsPanelWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    applyAdaptiveColumns();
}

void QtFunctionsPanelWidget::refreshPresentation() {
    const auto* presentation = model_->presentation();
    const std::size_t shown = presentation ? presentation->sorted_indices.size() : 0;
    const std::size_t total = state_ && state_->entries ? state_->entries->size() : 0;
    count_label_->setText(QStringLiteral("%1 functions").arg(total));
    const bool ready = state_ && state_->ready.load(std::memory_order_acquire);
    const bool building = state_ && state_->building.load(std::memory_order_acquire);
    loading_->setVisible(building);
    if (!context_ || context_->workspace().expired()) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No analyzed functions yet"));
        state_view_->setMessage(QStringLiteral(
            "Open a binary or attach to a running process to populate the symbol list."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (!ready && building) {
        state_view_->setState(widgets::AidaStateView::State::Loading);
        state_view_->setTitle(QStringLiteral("Building functions list..."));
        state_view_->setMessage(QStringLiteral(
            "Walking exception directory and resolving symbols on a worker thread."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (ready && total == 0) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No analyzed functions yet"));
        state_view_->setMessage(QStringLiteral(
            "Open a binary or attach to a running process to populate the symbol list."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    if (ready && shown == 0 && total > 0) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No matches"));
        state_view_->setMessage(QStringLiteral(
            "Nothing matches the current filter. Try a shorter query."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    state_view_->setVisible(false);
    table_->setVisible(true);
}

void QtFunctionsPanelWidget::showFunctionMenu(const QPoint& global_pos, int view_row) {
    if (!state_ || !context_) return;
    const auto* entry = view_row >= 0 ? model_->entryAt(view_row) : nullptr;
    if (!entry) return;
    const auto workspace = context_->workspace().lock();
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context) return;
    const auto target = entry->address;
    state_->ctx_row = view_row;
    state_->ctx_addr = target;
    state_->selected_row = view_row;
    state_->selected_addr = target;
    auto& s = *state_;
    auto menu = qt_analysis_menus::build_function_menu(context, target,
        entry->name, true, [&s, target]() {
            std::lock_guard<std::mutex> lock(s.mtx);
            return s.selected_addr == target
                ? aida::ui::capability_state_t::available()
                : aida::ui::capability_state_t::unavailable(
                    "The selected function changed");
        });
    QtAnalysisBridge::instance().showRetainedMenu(menu,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

void QtFunctionsPanelWidget::copySelectedName() {
    if (!state_) return;
    const auto index = table_->currentIndex();
    if (!index.isValid()) return;
    const auto* entry = model_->entryAt(index.row());
    if (!entry) return;
    clipboard::set_text(QString::fromStdString(entry->name));
}

bool QtFunctionsPanelWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers().testFlag(Qt::ControlModifier) &&
            (key->key() == Qt::Key_C || key->key() == Qt::Key_Insert)) {
            copySelectedName();
            return true;
        }
        if (key->key() == Qt::Key_Menu ||
            (key->key() == Qt::Key_F10 &&
                key->modifiers().testFlag(Qt::ShiftModifier))) {
            const auto current = table_->currentIndex();
            showFunctionMenu(table_->viewport()->mapToGlobal(current.isValid()
                ? table_->visualRect(current).center()
                : table_->viewport()->rect().center()),
                current.isValid() ? current.row() : -1);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

}
