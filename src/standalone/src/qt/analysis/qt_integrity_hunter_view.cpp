#include "qt/analysis/qt_integrity_hunter_view.hpp"

#include <QAbstractTableModel>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/disasm/disasm_view.hpp"
#include "core/disasm/pseudocode_view.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/shell_host_contract.hpp"
#include "helpers/diag_log.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::analysis {

namespace {

class QtIntegrityHunterModel : public QAbstractTableModel {
public:
    enum class Column : int {
        reader_rip = 0,
        module = 1,
        compare_addr = 2,
        reads = 3,
        rate = 4,
        status = 5,
        column_count = 6
    };

    explicit QtIntegrityHunterModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    // Full reset only when the hunt generation changed; tight dataChanged on
    // Reads/R/s columns otherwise (07 sec. 7.3).
    void syncNodes(std::vector<integrity_hunter::integrity_node_t> nodes,
                   bool generation_changed) {
        if (generation_changed || nodes.size() != nodes_.size()) {
            beginResetModel();
            nodes_ = std::move(nodes);
            endResetModel();
            return;
        }
        std::vector<int> changed;
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            if (nodes[index].read_count != nodes_[index].read_count ||
                nodes[index].reads_per_second != nodes_[index].reads_per_second ||
                nodes[index].neutralized != nodes_[index].neutralized)
                changed.push_back(static_cast<int>(index));
        }
        nodes_ = std::move(nodes);
        for (const int row : changed) {
            Q_EMIT dataChanged(indexAt(row, static_cast<int>(Column::reads)),
                indexAt(row, static_cast<int>(Column::status)));
        }
    }

    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(nodes_.size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
    }

    QModelIndex indexAt(int row, int column) const {
        return createIndex(row, column);
    }

    const integrity_hunter::integrity_node_t* rowAt(int row) const noexcept {
        if (row < 0 || static_cast<std::size_t>(row) >= nodes_.size()) return nullptr;
        return &nodes_[static_cast<std::size_t>(row)];
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.parent().isValid()) return {};
        const auto* node = rowAt(index.row());
        if (!node) return {};
        const auto& tokens = theme::tokens();
        if (role == Qt::DisplayRole) {
            switch (static_cast<Column>(index.column())) {
            case Column::reader_rip: {
                char buf[24]{};
                std::snprintf(buf, sizeof(buf), "0x%llX",
                    static_cast<unsigned long long>(node->reader_rip));
                return QString::fromLatin1(buf);
            }
            case Column::module: {
                std::string display = node->module_name;
                if (display.size() > 30) display = display.substr(0, 27) + "...";
                return QString::fromStdString(display);
            }
            case Column::compare_addr: {
                if (node->hash_compare_addr == 0) return QStringLiteral("N/A");
                char buf[24]{};
                std::snprintf(buf, sizeof(buf), "0x%llX",
                    static_cast<unsigned long long>(node->hash_compare_addr));
                return QString::fromLatin1(buf);
            }
            case Column::reads: return QString::number(node->read_count);
            case Column::rate:
                return QString::number(static_cast<double>(node->reads_per_second), 'f', 1);
            case Column::status:
                return node->neutralized ? QStringLiteral("Patched")
                    : QStringLiteral("Active");
            default: return {};
            }
        }
        if (role == Qt::ForegroundRole) {
            switch (static_cast<Column>(index.column())) {
            case Column::reader_rip: return tokens.syn_address;
            case Column::module:
            case Column::reads:
            case Column::rate: return tokens.text_dim;
            case Column::compare_addr:
                return node->hash_compare_addr != 0 ? tokens.warning : tokens.text_dim;
            case Column::status:
                return node->neutralized ? tokens.success : tokens.error;
            default: return {};
            }
        }
        if (role == Qt::FontRole) {
            switch (static_cast<Column>(index.column())) {
            case Column::reader_rip:
            case Column::compare_addr:
            case Column::reads:
            case Column::rate:
                return aida::qt::theme::fonts::codeRegular();
            default: return {};
            }
        }
        if (role == Qt::ToolTipRole) {
            if (static_cast<Column>(index.column()) == Column::module)
                return QString::fromStdString(node->module_name);
            return data(index, Qt::DisplayRole);
        }
        return {};
    }

    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override {
        for (QModelRoleData& roleData : span) {
            switch (roleData.role()) {
            case Qt::DisplayRole:
            case Qt::ForegroundRole:
            case Qt::FontRole:
            case Qt::ToolTipRole:
                roleData.setData(data(index, roleData.role()));
                break;
            default:
                roleData.clearData();
                break;
            }
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (static_cast<Column>(section)) {
        case Column::reader_rip: return QStringLiteral("Reader RIP");
        case Column::module: return QStringLiteral("Module");
        case Column::compare_addr: return QStringLiteral("Compare Addr");
        case Column::reads: return QStringLiteral("Reads");
        case Column::rate: return QStringLiteral("R/s");
        case Column::status: return QStringLiteral("Status");
        default: return {};
        }
    }

private:
    std::vector<integrity_hunter::integrity_node_t> nodes_;
};

}

QtIntegrityHunterView::QtIntegrityHunterView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.memory.integrity"));
    const auto& tokens = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.integrity.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    const QFontMetricsF ui_metrics(font());
    const int address_width = static_cast<int>(ui_metrics.horizontalAdvance(
        QStringLiteral("0xDDDDDDDDDDDDDDDD"))) + 2 * tokens.table.cell_pad_x +
        tokens.spacing.lg;
    const int size_width = static_cast<int>(ui_metrics.horizontalAdvance(
        QStringLiteral("4294967295"))) + 2 * tokens.table.cell_pad_x +
        tokens.spacing.lg;
    address_edit_ = new widgets::AidaLineEdit(QStringLiteral("Target Address (hex)"),
        toolbar);
    address_edit_->setObjectName(QStringLiteral("aida.integrity.address"));
    address_edit_->setMinimumWidth(address_width);
    address_edit_->setToolTip(QStringLiteral(
        "Code address to watch for integrity-checker reads, in hexadecimal"));
    const auto& engine = integrity_hunter::g_state;
    address_edit_->setText(QString::fromLatin1(engine.address_input));
    toolbar_layout->addWidget(address_edit_);
    size_edit_ = new widgets::AidaLineEdit(QStringLiteral("Size"), toolbar);
    size_edit_->setObjectName(QStringLiteral("aida.integrity.size"));
    size_edit_->setMinimumWidth(size_width);
    size_edit_->setToolTip(QStringLiteral(
        "Number of bytes to watch, decimal or 0x-prefixed hexadecimal"));
    size_edit_->setText(QString::fromLatin1(engine.size_input));
    toolbar_layout->addWidget(size_edit_);
    auto* start = new QPushButton(QStringLiteral("Start Hunt"), toolbar);
    start->setObjectName(QStringLiteral("aida.integrity.start"));
    start->setToolTip(QStringLiteral(
        "Start monitoring the region for integrity-checker reads"));
    auto* stop = new QPushButton(QStringLiteral("Stop Hunt"), toolbar);
    stop->setObjectName(QStringLiteral("aida.integrity.stop"));
    stop->setToolTip(QStringLiteral("Stop the active hunt"));
    auto* toggle_log = new QPushButton(QStringLiteral("Show Log"), toolbar);
    toggle_log->setObjectName(QStringLiteral("aida.integrity.toggle_log"));
    toggle_log->setCheckable(true);
    toggle_log->setToolTip(QStringLiteral(
        "Show or hide the raw integrity-event log"));
    toolbar_layout->addWidget(start);
    toolbar_layout->addWidget(stop);
    toolbar_layout->addWidget(toggle_log);
    toolbar_layout->addStretch(1);
    layout->addWidget(toolbar);

    auto* stats = new QWidget(this);
    stats->setObjectName(QStringLiteral("aida.integrity.stats"));
    auto* stats_layout = new QHBoxLayout(stats);
    stats_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    stats_layout->setSpacing(tokens.spacing.lg);
    const auto make_stat = [stats, stats_layout](const QString& label,
                                                     const char* object_name) {
        auto* name = new QLabel(label, stats);
        name->setObjectName(QStringLiteral("aida.integrity.stat.name.") +
            QString::fromLatin1(object_name));
        name->setProperty("aidaVariant", QStringLiteral("secondary"));
        auto* value = new QLabel(QStringLiteral("0"), stats);
        value->setObjectName(QStringLiteral("aida.integrity.stat.value.") +
            QString::fromLatin1(object_name));
        stats_layout->addWidget(name);
        stats_layout->addWidget(value);
        return value;
    };
    stat_checkers_ = make_stat(QStringLiteral("Checkers"), "checkers");
    stat_active_ = make_stat(QStringLiteral("Active"), "active");
    stat_neutralized_ = make_stat(QStringLiteral("Neutralized"), "neutralized");
    stat_rate_ = make_stat(QStringLiteral("Avg Rate"), "rate");
    stats_layout->addStretch(1);
    layout->addWidget(stats);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.integrity.status"));
    status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(status_label_);

    auto* model = new QtIntegrityHunterModel(this);
    model_ = model;
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.integrity.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.compact_row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = table_->horizontalHeader();
    using Column = QtIntegrityHunterModel::Column;
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const auto with_cell_pad = [&tokens](int content) {
        return content + 2 * tokens.table.cell_pad_x + tokens.spacing.xs;
    };
    const int hex_w = static_cast<int>(code_metrics.horizontalAdvance(
        QStringLiteral("0xDDDDDDDDDDDDDDDD")));
    horizontal->setSectionResizeMode(static_cast<int>(Column::reader_rip),
        QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::reader_rip),
        with_cell_pad(hex_w));
    horizontal->setSectionResizeMode(static_cast<int>(Column::module),
        QHeaderView::Stretch);
    horizontal->setSectionResizeMode(static_cast<int>(Column::compare_addr),
        QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::compare_addr),
        with_cell_pad(hex_w));
    horizontal->setSectionResizeMode(static_cast<int>(Column::reads), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::reads),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Reads"))),
            static_cast<int>(code_metrics.horizontalAdvance(
                QStringLiteral("9999999"))))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::rate), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::rate),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("R/s"))),
            static_cast<int>(code_metrics.horizontalAdvance(
                QStringLiteral("9999.9"))))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::status), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::status),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Status"))),
            static_cast<int>(ui_metrics.horizontalAdvance(
                QStringLiteral("Patched"))))));
    horizontal->setStretchLastSection(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);

    event_log_ = new QPlainTextEdit(this);
    event_log_->setObjectName(QStringLiteral("aida.integrity.event_log"));
    event_log_->setReadOnly(true);
    event_log_->setMaximumBlockCount(100);
    event_log_->setVisible(false);
    event_log_->setFixedHeight(5 * tokens.row.inspector);
    layout->addWidget(event_log_);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.integrity.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(66);
    connect(timer_, &QTimer::timeout, this, [this] { pollEngine(); });

    connect(start, &QPushButton::clicked, this, [this] {
        auto& ih = integrity_hunter::g_state;
        const auto address_text = address_edit_->text().toStdString();
        const auto size_text = size_edit_->text().toStdString();
        std::strncpy(ih.address_input, address_text.c_str(),
            sizeof(ih.address_input) - 1);
        std::strncpy(ih.size_input, size_text.c_str(), sizeof(ih.size_input) - 1);
        std::uint64_t addr = 0;
        std::uint64_t size = 4096;
        if (ih.address_input[0])
            addr = std::strtoull(ih.address_input, nullptr, 16);
        if (ih.size_input[0])
            size = std::strtoull(ih.size_input, nullptr, 0);
        if (addr != 0)
            integrity_hunter::start_hunt(addr, size);
    });
    connect(stop, &QPushButton::clicked, this, [] {
        integrity_hunter::stop_hunt();
    });
    connect(toggle_log, &QPushButton::toggled, this, [this](bool checked) {
        show_event_log_ = checked;
        event_log_->setVisible(checked);
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        if (index.isValid())
            showRowMenu(table_->viewport()->mapToGlobal(pos), index.row());
    });
    connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid()) return;
        auto* model = static_cast<QtIntegrityHunterModel*>(model_);
        const auto* node = model ? model->rowAt(index.row()) : nullptr;
        if (!node) return;
        const auto workspace_context = disasm_view::capture_selected_workspace();
        if (workspace_context)
            QtAnalysisBridge::instance().navigateTo(workspace_context.workspace,
                node->reader_rip, "document.disassembly");
    });
    table_->installEventFilter(this);
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        auto* model = static_cast<QtIntegrityHunterModel*>(model_);
        const auto* node = model && current.isValid()
            ? model->rowAt(current.row()) : nullptr;
        selected_rip_ = node ? node->reader_rip : 0;
    });

    refreshPresentation();
}

void QtIntegrityHunterView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    timer_->start();
}

void QtIntegrityHunterView::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

void QtIntegrityHunterView::pollEngine() {
    auto& ih = integrity_hunter::g_state;
    const std::uint64_t generation = ih.generation.load(std::memory_order_acquire);
    std::vector<integrity_hunter::integrity_node_t> nodes;
    {
        std::lock_guard<std::mutex> lock(ih.mutex);
        nodes = ih.nodes;
    }
    auto* model = static_cast<QtIntegrityHunterModel*>(model_);
    if (model)
        model->syncNodes(std::move(nodes), generation != generation_);
    generation_ = generation;

    int found = 0;
    int neutralized = 0;
    int active = 0;
    double avg_rps = 0.0;
    {
        std::lock_guard<std::mutex> lock(ih.mutex);
        found = static_cast<int>(ih.nodes.size());
        for (auto& node : ih.nodes) {
            if (node.neutralized) ++neutralized;
            else ++active;
            avg_rps += node.reads_per_second;
        }
        if (found > 0) avg_rps /= static_cast<double>(found);
    }
    stat_checkers_->setText(QString::number(found));
    stat_active_->setText(QString::number(active));
    stat_neutralized_->setText(QString::number(neutralized));
    stat_rate_->setText(QStringLiteral("%1/s").arg(avg_rps, 0, 'f', 1));
    const auto apply_variant = [](QLabel* label, const char* variant) {
        if (label->property("aidaVariant").toString() == QLatin1String(variant))
            return;
        label->setProperty("aidaVariant", QString::fromLatin1(variant));
        theme::stylesheet::repolish(label);
    };
    apply_variant(stat_active_, active > 0 ? "error" : "success");
    apply_variant(stat_neutralized_, neutralized > 0 ? "success" : "secondary");

    std::string status;
    {
        std::lock_guard<std::mutex> lock(ih.mutex);
        status = ih.status_text;
    }
    const bool hunting = ih.hunting.load();
    if (hunting) {
        const auto reads = ih.total_reads.load();
        status += (status.empty() ? "" : "  |  ");
        status += "Total reads: " + std::to_string(reads);
    }
    status_label_->setText(QString::fromStdString(status));

    if (show_event_log_) {
        std::vector<integrity_hunter::capture_event_t> events;
        {
            std::lock_guard<std::mutex> lock(ih.mutex);
            if (ih.event_log.size() < event_log_appended_) {
                event_log_appended_ = 0;
                event_log_->clear();
            }
            const std::size_t retained_start = ih.event_log.size() > 100
                ? ih.event_log.size() - 100 : 0;
            const std::size_t start_index = (std::max)(event_log_appended_,
                retained_start);
            events.assign(ih.event_log.begin() +
                static_cast<std::ptrdiff_t>(start_index), ih.event_log.end());
            event_log_appended_ = ih.event_log.size();
        }
        for (const auto& event : events) {
            char line[128];
            std::snprintf(line, sizeof(line), "[%s] RIP=0x%llX  Addr=0x%llX",
                event.access_type == 0 ? "R" : "W",
                static_cast<unsigned long long>(event.rip),
                static_cast<unsigned long long>(event.fault_addr));
            event_log_->appendPlainText(QString::fromLatin1(line));
        }
    }
    refreshPresentation();
}

bool QtIntegrityHunterView::eventFilter(QObject* watched, QEvent* event) {
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

void QtIntegrityHunterView::refreshPresentation() {
    const bool hunting = integrity_hunter::g_state.hunting.load();
    const bool empty = model_ && model_->rowCount() == 0;
    if (empty) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(hunting
            ? QStringLiteral("Monitoring for integrity checkers")
            : QStringLiteral("No integrity checkers"));
        state_view_->setMessage(hunting
            ? QStringLiteral("Monitoring for integrity checker reads - trigger the target's verification path.")
            : QStringLiteral("Enter a code address and click Start Hunt to locate integrity checkers."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    state_view_->setVisible(false);
    table_->setVisible(true);
}

void QtIntegrityHunterView::showRowMenu(const QPoint& global_pos, int view_row) {
    auto* model = static_cast<QtIntegrityHunterModel*>(model_);
    const auto* node = model ? model->rowAt(view_row) : nullptr;
    if (!node) return;
    selected_rip_ = node->reader_rip;
    const auto workspace_context = disasm_view::capture_selected_workspace();
    const std::uint64_t reader_rip = node->reader_rip;
    const std::uint64_t generation =
        integrity_hunter::g_state.generation.load(std::memory_order_acquire);
    aida::ui::application_ui::retained_entity_context_t retained;
    retained.owner_id = "memory.integrity.reader";
    retained.entity_id = std::to_string(reader_rip);
    retained.entity_generation = generation;
    retained.active_view = aida::ui::stable_view_id_t("view.memory.integrity");
    retained.validate_identity = [reader_rip, generation] {
        auto& state = integrity_hunter::g_state;
        if (state.generation.load(std::memory_order_acquire) != generation)
            return aida::ui::capability_state_t::unavailable(
                "The hunt generation changed; select the reader again");
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto found = std::find_if(state.nodes.begin(), state.nodes.end(),
            [reader_rip](const auto& candidate) {
                return candidate.reader_rip == reader_rip;
            });
        return found != state.nodes.end()
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The retained reader is no longer present in the active hunt");
    };
    const bool neutralized = node->neutralized;
    const auto add_action = [&retained](const char* id, bool enabled,
        const char* reason, auto invoke) {
        aida::ui::application_ui::retained_entity_action_t action;
        action.action_id = id;
        action.capability = enabled
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(reason);
        action.invoke = std::move(invoke);
        retained.actions.push_back(std::move(action));
    };
    add_action("memory.integrity.reader.neutralize", !neutralized,
        "This reader is already neutralized", [reader_rip] {
            auto& state = integrity_hunter::g_state;
            int index = -1;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                for (std::size_t candidate = 0; candidate < state.nodes.size(); ++candidate)
                    if (state.nodes[candidate].reader_rip == reader_rip) {
                        index = static_cast<int>(candidate);
                        break;
                    }
            }
            if (index < 0)
                return aida::ui::action_handler_result_t::failed(
                    "The integrity reader disappeared before neutralization");
            integrity_hunter::neutralize(index);
            return aida::ui::action_handler_result_t::completed();
        });
    add_action("memory.integrity.reader.restore", neutralized,
        "This reader has not been neutralized", [reader_rip] {
            auto& state = integrity_hunter::g_state;
            int index = -1;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                for (std::size_t candidate = 0; candidate < state.nodes.size(); ++candidate)
                    if (state.nodes[candidate].reader_rip == reader_rip) {
                        index = static_cast<int>(candidate);
                        break;
                    }
            }
            if (index < 0)
                return aida::ui::action_handler_result_t::failed(
                    "The integrity reader disappeared before restoration");
            integrity_hunter::restore(index);
            return aida::ui::action_handler_result_t::completed();
        });
    const bool workspace_available = static_cast<bool>(workspace_context);
    add_action("memory.integrity.reader.follow_disassembly", workspace_available,
        "No analysis workspace is selected", [reader_rip, workspace_context] {
            QtAnalysisBridge::instance().navigateTo(workspace_context.workspace,
                reader_rip, "document.disassembly");
            return aida::ui::action_handler_result_t::completed();
        });
    add_action("memory.integrity.reader.decompile", workspace_available,
        "No analysis workspace is selected", [reader_rip, workspace_context] {
            std::uint64_t entry =
                disasm_view::enclosing_function_start(reader_rip, workspace_context);
            if (entry == 0) entry = reader_rip;
            diag::log_tagged_critical_fmt("dec_ui",
                "integrity_reader_dispatched addr=0x%llX entry=0x%llX",
                static_cast<unsigned long long>(reader_rip),
                static_cast<unsigned long long>(entry));
            pseudocode_view::request_decompile(workspace_context, entry, false);
            return aida::ui::action_handler_result_t::completed();
        });
    QtAnalysisBridge::instance().showRetainedMenu(retained,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
