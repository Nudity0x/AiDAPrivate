#include "qt/debugger/modules_pane.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_view.hpp"
#include "core/debugger/module_view.hpp"
#include "core/infra/executor.hpp"
#include "core/ui/toast_notification.hpp"

#include "qt/debugger/debugger_models.hpp"
#include "qt/debugger/debugger_session_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_pill.hpp"

namespace aida::qt::debugger {

ModulesPane::ModulesPane(QWidget* parent)
    : DebuggerPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.debug.modules"));
    setOwnerViewId("view.debug.modules");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral(
            "Attach to a process to enumerate its loaded modules."));
    setEmptyContentText(QStringLiteral("No modules enumerated"),
        QStringLiteral(
            "Module enumeration runs automatically while attached; press "
            "Refresh to re-enumerate now."));
    setLoadingText(QStringLiteral("Enumerating modules"),
        QStringLiteral(
            "The engine is enumerating the attached target's loaded "
            "modules."));
    setErrorText(QStringLiteral("Module refresh failed"),
        QStringLiteral(
            "The module enumeration worker failed; press Refresh to retry."));

    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    const auto& tokens = theme::tokens();
    bar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    bar_layout->setSpacing(tokens.toolbar.group_gap);

    filter_edit_ = new widgets::AidaLineEdit(QStringLiteral("Filter modules..."),
        bar);
    filter_edit_->setObjectName(QStringLiteral("aida.view.debug.modules.filter"));
    filter_edit_->setMaxLength(127);
    bar_layout->addWidget(filter_edit_, 1);

    refresh_button_ = new widgets::AidaButton(QStringLiteral("Refresh"), bar);
    refresh_button_->setObjectName(
        QStringLiteral("aida.view.debug.modules.refresh"));
    refresh_button_->setKind(widgets::AidaButton::Kind::Secondary);
    refresh_button_->setToolTip(QStringLiteral(
        "Re-enumerate the attached target's loaded modules"));
    connect(refresh_button_, &widgets::AidaButton::clicked, this, [] {
        module_view::refresh();
    });
    bar_layout->addWidget(refresh_button_);

    dump_button_ = new widgets::AidaButton(QStringLiteral("Dump Selected"), bar);
    dump_button_->setObjectName(QStringLiteral("aida.view.debug.modules.dump"));
    dump_button_->setKind(widgets::AidaButton::Kind::Primary);
    dump_button_->setToolTip(QStringLiteral(
        "Dump the selected module's bytes to disk"));
    connect(dump_button_, &widgets::AidaButton::clicked, this,
        &ModulesPane::dumpSelected);
    bar_layout->addWidget(dump_button_);

    selection_pill_ = new widgets::AidaPill(QStringLiteral("No module selected"),
        widgets::AidaSemantic::Neutral, bar);
    selection_pill_->setObjectName(
        QStringLiteral("aida.view.debug.modules.selection"));
    bar_layout->addWidget(selection_pill_);
    auto* inject_pill = new widgets::AidaPill(QStringLiteral("Inject unavailable"),
        widgets::AidaSemantic::Warning, bar);
    inject_pill->setObjectName(QStringLiteral("aida.view.debug.modules.inject"));
    inject_pill->setToolTip(QStringLiteral(
        "Remote LoadLibrary is not exposed by driver_bridge in this build."));
    bar_layout->addWidget(inject_pill);
    auto* unload_pill = new widgets::AidaPill(QStringLiteral("Unload unavailable"),
        widgets::AidaSemantic::Warning, bar);
    unload_pill->setObjectName(QStringLiteral("aida.view.debug.modules.unload"));
    unload_pill->setToolTip(QStringLiteral(
        "Remote FreeLibrary is not exposed by driver_bridge in this build."));
    bar_layout->addWidget(unload_pill);
    setToolBar(bar);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);

    modules_model_ = new ModulesModel(this);
    modules_view_ = new QTableView(splitter);
    modules_view_->setObjectName(QStringLiteral("aida.view.debug.modules.table"));
    wireTable(modules_view_, modules_model_);
    splitter->addWidget(modules_view_);

    detail_tabs_ = new QTabWidget(splitter);
    detail_tabs_->setObjectName(QStringLiteral("aida.view.debug.modules.detail"));
    exports_model_ = new ModuleExportsModel(this);
    exports_view_ = new QTableView(detail_tabs_);
    exports_view_->setObjectName(
        QStringLiteral("aida.view.debug.modules.exports"));
    wireTable(exports_view_, exports_model_);
    detail_tabs_->addTab(exports_view_, QStringLiteral("Exports"));
    imports_model_ = new ModuleImportsModel(this);
    imports_view_ = new QTableView(detail_tabs_);
    imports_view_->setObjectName(
        QStringLiteral("aida.view.debug.modules.imports"));
    wireTable(imports_view_, imports_model_);
    detail_tabs_->addTab(imports_view_, QStringLiteral("Imports"));
    splitter->addWidget(detail_tabs_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    setContent(splitter);

    connect(filter_edit_, &QLineEdit::textChanged, this, [this] {
        modules_model_->setFilter(filter_edit_->text());
    });
    connect(modules_view_->selectionModel(),
        &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            if (current.isValid())
                onModuleSelected(current.row());
        });

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(250);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, &ModulesPane::pollModel);

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(5000);
    refresh_timer_->setTimerType(Qt::CoarseTimer);
    connect(refresh_timer_, &QTimer::timeout, this, [] {
        module_view::refresh();
    });

    connect(&DebuggerSessionController::instance(),
        &DebuggerSessionController::modulesRefreshRequested, this, [] {
            module_view::refresh();
        });
}

void ModulesPane::onShown() {
    poll_timer_->start();
    refresh_timer_->start();
    module_view::ensure_subscriptions();
    if (driver_bridge::attached_pid() != 0)
        module_view::refresh();
    pollModel();
}

void ModulesPane::onHidden() {
    poll_timer_->stop();
    refresh_timer_->stop();
}

void ModulesPane::onSessionTick() {
    updateOverlayState();
}

bool ModulesPane::hasContentRows() const {
    return modules_model_ && (modules_model_->rowCount() > 0 ||
        (filter_edit_ && !filter_edit_->text().isEmpty()));
}

bool ModulesPane::isContentLoading() const {
    return module_view::g_ui.loading.load(std::memory_order_acquire);
}

bool ModulesPane::contentError(QString* detail) const {
    if (last_error_.isEmpty())
        return false;
    if (detail)
        *detail = last_error_;
    return true;
}

void ModulesPane::pollModel() {
    const auto generation = module_view::modules_generation();
    const bool loading = module_view::g_ui.loading.load(
        std::memory_order_acquire);
    refresh_button_->setEnabled(!loading);
    refresh_button_->setLoading(loading);
    {
        std::unique_lock<std::mutex> lock(module_view::g_ui.modules_mutex,
            std::try_to_lock);
        if (lock.owns_lock()) {
            const QString error_text =
                QString::fromStdString(module_view::g_ui.last_error);
            if (error_text != last_error_) {
                last_error_ = error_text;
                updateOverlayState();
            }
        }
    }
    if (generation != last_data_generation_) {
        last_data_generation_ = generation;
        const auto modules = module_view::modules_snapshot();
        const auto selected = capture_selected_row_ids(*modules_model_,
            modules_view_->selectionModel());
        const quint64 focus = modules_view_->currentIndex().isValid()
            ? modules_model_->rowId(modules_view_->currentIndex().row()) : 0;
        modules_model_->applyModules(modules, generation);
        restore_selected_row_ids(*modules_model_, modules_view_, selected,
            focus);
        std::vector<pe_parser::export_entry_t> exports;
        std::vector<pe_parser::import_entry_t> imports;
        module_view::details_snapshot(exports, imports);
        exports_model_->applySnapshot(
            std::make_shared<const std::vector<pe_parser::export_entry_t>>(
                std::move(exports)),
            generation);
        imports_model_->applySnapshot(
            std::make_shared<const std::vector<pe_parser::import_entry_t>>(
                std::move(imports)),
            generation);
    }
    updateSelectionPill();
}

void ModulesPane::updateSelectionPill() {
    const auto selected = module_view::selected_module_snapshot();
    const bool selection_stale = selected.base != 0 && !selected.present;
    selection_pill_->setText(selected.present
        ? QStringLiteral("Module selected")
        : (selection_stale ? QStringLiteral("Selection unloaded")
                           : QStringLiteral("No module selected")));
    selection_pill_->setKind(selected.present
        ? widgets::AidaSemantic::Success
        : (selection_stale ? widgets::AidaSemantic::Warning
                           : widgets::AidaSemantic::Neutral));
    selection_pill_->setToolTip(selected.base != 0
        ? QString::asprintf("%s base=0x%016llX",
            selected.name.empty() ? "Selected module" : selected.name.c_str(),
            static_cast<unsigned long long>(selected.base))
        : QString());
    const bool can_dump = driver_bridge::attached_pid() != 0 &&
        selected.present && selected.size != 0;
    dump_button_->setEnabled(can_dump);
}

void ModulesPane::onModuleSelected(int row) {
    const auto* module = modules_model_->rowAt(row);
    if (!module)
        return;
    module_view::select_module_by_base(module->base, module->name);
    module_view::load_module_details_by_base(module->base);
}

void ModulesPane::dumpSelected() {
    const auto dump_target = module_view::selected_module_snapshot();
    if (driver_bridge::attached_pid() == 0) {
        toast_notification::push("Attach to a target first.",
            toast_notification::toast_type_t::warning);
        diag::log_tagged("dbg_audit",
            "[dbg_audit] modules dump fail reason=no_attached_target");
        return;
    }
    if (!dump_target.present || dump_target.base == 0 || dump_target.size == 0) {
        toast_notification::push(dump_target.base != 0
            ? "Selected module is no longer loaded."
            : "Select a module first.",
            toast_notification::toast_type_t::warning);
        diag::log_tagged("dbg_audit", dump_target.base != 0
            ? "[dbg_audit] modules dump fail reason=selected_module_unloaded"
            : "[dbg_audit] modules dump fail reason=no_selection");
        return;
    }
    if (dump_target.size > 256ULL * 1024ULL * 1024ULL) {
        toast_notification::push("Module exceeds 256 MiB dump cap.",
            toast_notification::toast_type_t::warning);
        diag::log_tagged("dbg_audit",
            "[dbg_audit] modules dump fail reason=cap_exceeded");
        return;
    }
    debugger_view::dump_module_bytes(dump_target.base, dump_target.size,
        dump_target.name);
}

}
