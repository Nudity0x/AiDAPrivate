#include "qt/analysis/qt_analysis_bridge.hpp"

#include <QTimer>

#include <algorithm>

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/infra/event_bus.hpp"
#include "core/session/analysis_session.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "core/workbench/workbench_shell_integration.hpp"

#include "qt/analysis/qt_analysis_host_hooks.hpp"
#include "qt/analysis/qt_gui_post.hpp"
#include "qt/analysis/qt_revision_poller.hpp"
#include "qt/analysis/qt_session_ui_hooks.hpp"
#include "qt/analysis/qt_stealth_view.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/menu_bridge.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/docking/hub_dock.hpp"
#include "qt/overlays/aida_loading_bridge.hpp"
#include "qt/registry/qt_view_registry.hpp"

namespace aida::qt::analysis {

namespace {

// Wake-accelerator observer (07 sec. 1.2): the baseline publish observer exists on
// the workspace public API; its callback does exactly one queued pollNow.
class QtBaselinePublishObserver final
    : public aida::analysis::baseline_publish_observer_t {
public:
    explicit QtBaselinePublishObserver(QPointer<QtRevisionPoller> poller)
        : poller_(poller) {}

    void on_baseline_published(
        const std::shared_ptr<const aida::analysis::analysis_publication_t>&)
        noexcept override {
        auto* poller = poller_.data();
        if (!poller) return;
        QMetaObject::invokeMethod(poller, "pollNow", Qt::QueuedConnection);
    }

private:
    QPointer<QtRevisionPoller> poller_;
};

// Anchor object for the fabricated menu.recent.item payload; the runtime's
// recent.* actions read the staged global (open_recent_context_menu), so the
// payload only has to carry the accepted context type for compose() (S6).
struct recent_menu_payload_anchor_t {};
const recent_menu_payload_anchor_t& recentPayloadAnchor() {
    static const recent_menu_payload_anchor_t anchor{};
    return anchor;
}

}  // namespace

QtAnalysisBridge& QtAnalysisBridge::instance() {
    static QtAnalysisBridge* bridge = new QtAnalysisBridge();
    return *bridge;
}

QtAnalysisBridge::QtAnalysisBridge(QObject* parent) : QObject(parent) {}

void QtAnalysisBridge::install(docking::AidaDockHost* host,
                               bridge::MenuBridge* menus,
                               bridge::ActionBridge* actions) {
    if (!host) return;
    host_ = host;
    menus_ = menus;
    actions_ = actions;
    installSessionHooks();
    installViewFactories();
    installEngineHooks();
    wireEvents();
    diag::log_tagged("qt_analysis", "analysis_domain_installed");
}

void install_analysis_domain(docking::AidaDockHost* host, bridge::MenuBridge* menus,
                             bridge::ActionBridge* actions) {
    QtAnalysisBridge::instance().install(host, menus, actions);
}

QtWorkspaceContext* QtAnalysisBridge::contextFor(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace) {
    if (!workspace) return nullptr;
    const QString key =
        QString::fromStdString(workspace->identity().binary_id().to_hex());
    auto found = contexts_.find(key);
    if (found != contexts_.end()) {
        found.value()->last_touch = ++touch_counter_;
        return found.value();
    }
    if (contexts_.size() >= static_cast<int>(analysis_session::kMaxSessions)) {
        QtWorkspaceContext* oldest = nullptr;
        QString oldest_key;
        for (auto it = contexts_.begin(); it != contexts_.end(); ++it) {
            if (it.value() == active_context_) continue;
            if (!oldest || it.value()->last_touch < oldest->last_touch) {
                oldest = it.value();
                oldest_key = it.key();
            }
        }
        if (oldest) {
            diag::log_tagged_fmt("qt_analysis",
                "workspace_context_evict binary_id=%s reason=lru_cap",
                oldest_key.toStdString().c_str());
            contexts_.remove(oldest_key);
            oldest->deleteLater();
        }
    }
    auto* context = new QtWorkspaceContext(workspace, this);
    context->last_touch = ++touch_counter_;
    contexts_.insert(key, context);
    registerBaselineObserver(context);
    connect(context->poller(), &QtRevisionPoller::workspaceClosed, this,
            [this, key] { evictContextLocked(key); });
    context->poller()->arm();
    diag::log_tagged_fmt("qt_analysis", "workspace_context_create binary_id=%s",
        key.toStdString().c_str());
    return context;
}

void QtAnalysisBridge::pruneContexts() {
    for (auto it = contexts_.begin(); it != contexts_.end();) {
        auto* context = it.value();
        const auto workspace = context->workspace().lock();
        if (!workspace || workspace->closed()) {
            if (context == active_context_) {
                active_context_ = nullptr;
                Q_EMIT activeContextChanged(nullptr);
            }
            diag::log_tagged_fmt("qt_analysis",
                "workspace_context_prune binary_id=%s",
                it.key().toStdString().c_str());
            baseline_observers_.remove(context);
            it = contexts_.erase(it);
            context->deleteLater();
            continue;
        }
        ++it;
    }
}

void QtAnalysisBridge::evictContextLocked(const QString& binaryIdHex) {
    const auto found = contexts_.find(binaryIdHex);
    if (found == contexts_.end()) return;
    auto* context = found.value();
    if (context == active_context_) {
        active_context_ = nullptr;
        Q_EMIT activeContextChanged(nullptr);
    }
    baseline_observers_.remove(context);
    contexts_.erase(found);
    context->deleteLater();
}

void QtAnalysisBridge::registerBaselineObserver(QtWorkspaceContext* context) {
    if (!context) return;
    const auto workspace = context->workspace().lock();
    if (!workspace) return;
    auto observer = std::make_shared<QtBaselinePublishObserver>(context->poller());
    baseline_observers_.insert(context, observer);
    static_cast<void>(workspace->register_baseline_publish_observer(observer));
}

void QtAnalysisBridge::openView(const char* view_id) {
    if (!host_ || !view_id) return;
    static_cast<void>(host_->open_or_focus(registry::stable_view_id_t(view_id)));
    ensureHubTabWiring(registry::hub_kind_t::analysis);
    ensureHubTabWiring(registry::hub_kind_t::types);
}

void QtAnalysisBridge::navigateTo(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    std::uint64_t address, const char* view_id) {
    if (!workspace || address == 0 || !view_id) return;
    const auto context = disasm_view::capture_workspace(workspace);
    if (!context) return;
    openView(view_id);
    disasm_view::goto_address(address, context);
    Q_EMIT navigateRequested(
        QString::fromStdString(workspace->identity().binary_id().to_hex()),
        address, QString::fromLatin1(view_id));
}

void QtAnalysisBridge::focusView(const char* view_id) {
    if (!host_ || !view_id) return;
    const auto id = registry::stable_view_id_t(view_id);
    const auto instance = host_->registry()->instance_for(id);
    static_cast<void>(host_->registry()->focus(instance));
    ensureHubTabWiring(registry::hub_kind_t::analysis);
    ensureHubTabWiring(registry::hub_kind_t::types);
}

void QtAnalysisBridge::showRetainedMenu(
    const aida::ui::application_ui::retained_entity_context_t& context,
    aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos,
    QWidget* parent) {
    if (!menus_) return;
    menus_->show_retained_entity_menu(context, origin, global_pos, parent);
}

void QtAnalysisBridge::showRecentMenu(const std::string& path, bool open_session,
                                      aida::ui::context_menu_open_origin_t origin,
                                      const QPoint& global_pos, QWidget* parent) {
    if (!menus_ || path.empty()) return;
    aida::ui::application_ui::open_recent_context_menu(path, open_session, origin);
    aida::ui::interaction_context_t snapshot;
    snapshot.active_view = aida::ui::stable_view_id_t("view.recent");
    snapshot.payload = aida::ui::typed_context_ref_t::from(
        aida::ui::stable_context_type_id_t("context.recent.item"), recentPayloadAnchor());
    menus_->show_context_menu(aida::ui::stable_menu_id_t("menu.recent.item"),
        std::move(snapshot), origin, global_pos, parent);
}

void QtAnalysisBridge::setChatAcceptor(std::function<bool(const QString&)> acceptor) {
    chat_acceptor_ = std::move(acceptor);
}

void QtAnalysisBridge::injectToChatText(const QString& text) {
    if (text.isEmpty()) return;
    if (chat_acceptor_ && chat_acceptor_(text)) {
        Q_EMIT chatInjectRequested(text);
        toastInfo(QStringLiteral("Binary map appended to chat input"), 3.0);
        return;
    }
    clipboard::set_text(text);
    toastWarning(QStringLiteral(
        "Binary map exceeds chat buffer; copied to clipboard instead"), 4.0);
}

void QtAnalysisBridge::toastInfo(const QString& message, double seconds) {
    chrome::toast_info(message, seconds);
}

void QtAnalysisBridge::toastWarning(const QString& message, double seconds) {
    chrome::toast_warning(message, seconds);
}

void QtAnalysisBridge::toastError(const QString& message, double seconds) {
    chrome::toast_error(message, seconds);
}

void QtAnalysisBridge::installSessionHooks() {
    aida::session_ui_hooks::hooks_t hooks;
    hooks.track_loading_session = [](const std::string& session_id,
                                     const std::string& path) {
        loading_binary_overlay::track_session(session_id, path,
            loading_binary_overlay::completion_action_t::switch_to_disassembly_or_hex);
    };
    hooks.release_loading_session = [](const std::string& session_id) {
        loading_binary_overlay::release_session(session_id);
    };
    hooks.attach_workbench_workspace =
        [](const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace)
        -> aida::session_ui_hooks::workbench_hook_result_t {
        aida::workbench::workbench_shell_workspace_context_t workbench_context;
        const auto attached =
            aida::workbench::workbench_shell_runtime_t::instance()
                .attach_analysis_workspace(workspace, workbench_context);
        return {attached.ok(), static_cast<unsigned int>(attached.code),
                static_cast<unsigned long long>(attached.subject)};
    };
    hooks.close_workbench_workspace =
        [](const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace)
        -> aida::session_ui_hooks::workbench_hook_result_t {
        const auto closed =
            aida::workbench::workbench_shell_runtime_t::instance()
                .close_analysis_workspace(workspace);
        return {closed.ok(), static_cast<unsigned int>(closed.code),
                static_cast<unsigned long long>(closed.subject)};
    };
    aida::session_ui_hooks::install_hooks(std::move(hooks));
}

void QtAnalysisBridge::wireEvents() {
    if (events_wired_) return;
    events_wired_ = true;
    auto* tracker = new QTimer(this);
    tracker->setInterval(250);
    tracker->setTimerType(Qt::CoarseTimer);
    connect(tracker, &QTimer::timeout, this, [this] { trackSelectedWorkspace(); });
    tracker->start();

    aida::events::subscribe(aida::events::event_session_selected,
        [this](const aida::events::session_selected_t&) {
            gui_post(this, [this] { trackSelectedWorkspace(); });
        });
    aida::events::subscribe(aida::events::event_session_created,
        [this](const aida::events::session_created_t&) {
            gui_post(this, [this] { trackSelectedWorkspace(); });
        });
    aida::events::subscribe(aida::events::event_session_deleted,
        [this](const aida::events::session_deleted_t&) {
            gui_post(this, [this] { pruneContexts(); trackSelectedWorkspace(); });
        });
}

void QtAnalysisBridge::trackSelectedWorkspace() {
    const auto workspace = aida::analysis::workspace_registry().selected_for_ui();
    QtWorkspaceContext* next = workspace ? contextFor(workspace) : nullptr;
    if (next == active_context_.data()) return;
    active_context_ = next;
    syncTabsFromContext(next);
    Q_EMIT activeContextChanged(next);
}

void QtAnalysisBridge::registerXrefView(QObject* view) {
    xref_view_ = view;
}

void QtAnalysisBridge::registerProximityView(QObject* view) {
    proximity_view_ = view;
}

void QtAnalysisBridge::registerReconView(QObject* view) {
    recon_view_ = view;
}

void QtAnalysisBridge::registerStealthView(QObject* view) {
    stealth_view_ = view;
}

void QtAnalysisBridge::setAnalysisHubTab(int tab) {
    if (tab < 0 || tab >= kAnalysisHubTabCount) return;
    analysis_hub_default_.store(tab, std::memory_order_release);
    analysis_hub_effective_.store(tab, std::memory_order_release);
    analysis_hub_dirty_.store(true, std::memory_order_release);
    gui_post(this, [this, tab] {
        if (auto* context = active_context_.data())
            context->analysis_hub_tab.store(tab, std::memory_order_release);
        ensureHubTabWiring(registry::hub_kind_t::analysis);
        if (!host_) return;
        if (auto* hub = host_->hub_widget(registry::hub_kind_t::analysis))
            if (hub->current_subview() != tab) hub->set_subview(tab);
    });
}

void QtAnalysisBridge::setTypesHubTab(int tab) {
    if (tab < 0 || tab >= kTypesHubTabCount) return;
    types_hub_default_.store(tab, std::memory_order_release);
    types_hub_effective_.store(tab, std::memory_order_release);
    types_hub_dirty_.store(true, std::memory_order_release);
    gui_post(this, [this, tab] {
        if (auto* context = active_context_.data())
            context->types_hub_tab.store(tab, std::memory_order_release);
        ensureHubTabWiring(registry::hub_kind_t::types);
        if (!host_) return;
        if (auto* hub = host_->hub_widget(registry::hub_kind_t::types))
            if (hub->current_subview() != tab) hub->set_subview(tab);
    });
}

void QtAnalysisBridge::setStealthTab(int tab) {
    if (tab < 0 || tab >= kStealthTabCount) return;
    stealth_tab_.store(tab, std::memory_order_release);
    gui_post(this, [this, tab] {
        if (auto* view = qobject_cast<QtStealthView*>(stealth_view_.data()))
            view->setInnerTab(tab);
    });
}

void QtAnalysisBridge::noteAnalysisHubTabFromWidget(int tab) {
    if (tab < 0 || tab >= kAnalysisHubTabCount) return;
    analysis_hub_effective_.store(tab, std::memory_order_release);
    gui_post(this, [this, tab] {
        if (auto* context = active_context_.data())
            context->analysis_hub_tab.store(tab, std::memory_order_release);
    });
}

void QtAnalysisBridge::noteTypesHubTabFromWidget(int tab) {
    if (tab < 0 || tab >= kTypesHubTabCount) return;
    types_hub_effective_.store(tab, std::memory_order_release);
    gui_post(this, [this, tab] {
        if (auto* context = active_context_.data())
            context->types_hub_tab.store(tab, std::memory_order_release);
    });
}

void QtAnalysisBridge::noteStealthTabFromWidget(int tab) {
    if (tab < 0 || tab >= kStealthTabCount) return;
    stealth_tab_.store(tab, std::memory_order_release);
}

const char* QtAnalysisBridge::analysisHubTabLabel(int tab) noexcept {
    static constexpr const char* labels[] = {
        "Symbolic", "Taint", "Deobfuscation", "Fuzzer", "Protection"
    };
    return tab >= 0 && tab < kAnalysisHubTabCount ? labels[tab] : "";
}

const char* QtAnalysisBridge::typesHubTabLabel(int tab) noexcept {
    static constexpr const char* labels[] = {
        "Structures", "Unions", "Enums", "Typedefs", "Functions", "Inferred",
        "Dissector"
    };
    return tab >= 0 && tab < kTypesHubTabCount ? labels[tab] : "";
}

const char* QtAnalysisBridge::stealthTabLabel(int tab) noexcept {
    static constexpr const char* labels[] = {
        "Protection Scan", "Stealth Status"
    };
    return tab >= 0 && tab < kStealthTabCount ? labels[tab] : "";
}

void QtAnalysisBridge::ensureHubTabWiring(registry::hub_kind_t hub_kind) {
    if (!host_) return;
    const bool is_analysis = hub_kind == registry::hub_kind_t::analysis;
    if (!is_analysis && hub_kind != registry::hub_kind_t::types) return;
    bool& wired = is_analysis ? analysis_hub_wired_ : types_hub_wired_;
    if (wired) return;
    auto* hub = host_->hub_widget(hub_kind);
    if (!hub) return;
    wired = true;
    connect(hub, &docking::AidaHubWidget::subviewActivated, this,
            [this, is_analysis](int index) {
        if (is_analysis)
            noteAnalysisHubTabFromWidget(index);
        else
            noteTypesHubTabFromWidget(index);
    });
    auto& dirty = is_analysis ? analysis_hub_dirty_ : types_hub_dirty_;
    auto& effective =
        is_analysis ? analysis_hub_effective_ : types_hub_effective_;
    if (dirty.load(std::memory_order_acquire)) {
        const int wanted = effective.load(std::memory_order_acquire);
        if (hub->current_subview() != wanted) hub->set_subview(wanted);
        return;
    }
    const int current = hub->current_subview();
    if (current < 0) return;
    effective.store(current, std::memory_order_release);
    if (auto* context = active_context_.data()) {
        auto& slot = is_analysis ? context->analysis_hub_tab
                                 : context->types_hub_tab;
        slot.store(current, std::memory_order_release);
    }
}

void QtAnalysisBridge::syncTabsFromContext(QtWorkspaceContext* context) {
    if (context) {
        analysis_hub_effective_.store(
            context->analysis_hub_tab.load(std::memory_order_acquire),
            std::memory_order_release);
        types_hub_effective_.store(
            context->types_hub_tab.load(std::memory_order_acquire),
            std::memory_order_release);
    } else {
        analysis_hub_effective_.store(
            analysis_hub_default_.load(std::memory_order_acquire),
            std::memory_order_release);
        types_hub_effective_.store(
            types_hub_default_.load(std::memory_order_acquire),
            std::memory_order_release);
    }
    if (!host_) return;
    ensureHubTabWiring(registry::hub_kind_t::analysis);
    ensureHubTabWiring(registry::hub_kind_t::types);
    if (auto* hub = host_->hub_widget(registry::hub_kind_t::analysis)) {
        const int tab = analysis_hub_effective_.load(std::memory_order_acquire);
        if (hub->current_subview() != tab) hub->set_subview(tab);
    }
    if (auto* hub = host_->hub_widget(registry::hub_kind_t::types)) {
        const int tab = types_hub_effective_.load(std::memory_order_acquire);
        if (hub->current_subview() != tab) hub->set_subview(tab);
    }
}

}  // namespace aida::qt::analysis
