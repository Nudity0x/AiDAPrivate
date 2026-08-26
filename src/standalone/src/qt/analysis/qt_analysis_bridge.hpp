#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/registry/qt_view_descriptor.hpp"

class QWidget;

namespace aida::ui {
enum class context_menu_open_origin_t : std::uint8_t;
namespace application_ui {
struct retained_entity_context_t;
}
}

namespace aida::qt {
class AidaMainWindow;
namespace bridge {
class ActionBridge;
class MenuBridge;
}
namespace docking {
class AidaDockHost;
}
}

namespace aida::qt::analysis {

class QtWorkspaceContext;

// QtAnalysisBridge: the analysis-domain composition root. Owns the per-binary
// QtWorkspaceContext registry, drives the engine-seam session hooks (07 sec. 1.4),
// emits chatInjectRequested (S10) and navigateRequested, and is the single
// GUI-thread entry point for view navigation / menu presentation.
class QtAnalysisBridge : public QObject {
    Q_OBJECT
public:
    static QtAnalysisBridge& instance();

    // Called once by the composition root (shell) after the dock host and the
    // W2.2/W2.3 bridges exist. Installs view factories + engine hooks.
    void install(docking::AidaDockHost* host, bridge::MenuBridge* menus,
                 bridge::ActionBridge* actions);
    bool installed() const noexcept { return host_ != nullptr; }

    docking::AidaDockHost* host() const noexcept { return host_; }

    // Per-binary context registry (07 sec. 1.3). GUI thread only.
    QtWorkspaceContext* contextFor(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace);
    QtWorkspaceContext* activeContext() const noexcept { return active_context_; }
    void pruneContexts();

    // Live view-instance registrations (engine hooks route through these).
    void registerXrefView(QObject* view);
    void registerProximityView(QObject* view);
    void registerReconView(QObject* view);
    void registerStealthView(QObject* view);
    QObject* xrefView() const noexcept { return xref_view_.data(); }
    QObject* proximityView() const noexcept { return proximity_view_.data(); }
    QObject* reconView() const noexcept { return recon_view_.data(); }
    QObject* stealthView() const noexcept { return stealth_view_.data(); }

    // Hub tab-state mirror. Replaces the ImGui-era
    // analysis_hub_view::{active,set}_sub_tab / sub_tab_label,
    // types_hub_view::{active,set}_sub_tab / sub_tab_label, and
    // stealth_view::{active,set}_sub_tab / sub_tab_label / sub_tab_count APIs
    // for every Qt-free caller (testlab, engine-side navigation requests).
    // Getters are pure atomic reads (any thread). Setters store synchronously,
    // then queue the widget routing + per-binary persistence onto the GUI
    // thread. note*FromWidget is the widget->mirror direction (no re-route).
    // Indices match the descriptor hub subview order in view_catalog.hpp,
    // which is the old enum order (analysis: symbolic..protection = 0..4;
    // types: structures..dissector = 0..6; stealth: scan..status = 0..1).
    static constexpr int kAnalysisHubTabCount = 5;
    static constexpr int kTypesHubTabCount = 7;
    static constexpr int kStealthTabCount = 2;
    int analysisHubTab() const noexcept {
        return analysis_hub_effective_.load(std::memory_order_acquire);
    }
    int typesHubTab() const noexcept {
        return types_hub_effective_.load(std::memory_order_acquire);
    }
    int stealthTab() const noexcept {
        return stealth_tab_.load(std::memory_order_acquire);
    }
    void setAnalysisHubTab(int tab);
    void setTypesHubTab(int tab);
    void setStealthTab(int tab);
    void noteAnalysisHubTabFromWidget(int tab);
    void noteTypesHubTabFromWidget(int tab);
    void noteStealthTabFromWidget(int tab);
    static const char* analysisHubTabLabel(int tab) noexcept;
    static const char* typesHubTabLabel(int tab) noexcept;
    static const char* stealthTabLabel(int tab) noexcept;

    // Navigation / view presentation used by all analysis views.
    void openView(const char* view_id);
    void navigateTo(const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
                    std::uint64_t address, const char* view_id);
    void focusView(const char* view_id);

    // Context-menu presentation (S6): the view supplies the retained entity
    // contract; the bridge forwards to the W2.2 menu bridge.
    void showRetainedMenu(
        const aida::ui::application_ui::retained_entity_context_t& context,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos,
        QWidget* parent);
    void showRecentMenu(const std::string& path, bool open_session,
                        aida::ui::context_menu_open_origin_t origin,
                        const QPoint& global_pos, QWidget* parent);

    // Chat injection (S10). P-chat installs the acceptor; when no acceptor is
    // installed or it rejects, the text falls back to the clipboard + toast.
    void setChatAcceptor(std::function<bool(const QString&)> acceptor);
    void injectToChatText(const QString& text);

    // Toast passthrough (views use this instead of the removed
    // toast_notification::push).
    void toastInfo(const QString& message, double seconds = 3.0);
    void toastWarning(const QString& message, double seconds = 4.0);
    void toastError(const QString& message, double seconds = 4.0);

    Q_SIGNAL void chatInjectRequested(const QString& text);
    Q_SIGNAL void navigateRequested(const QString& binaryIdHex, quint64 address,
                                    const QString& viewId);
    Q_SIGNAL void activeContextChanged(QtWorkspaceContext* context);

private:
    explicit QtAnalysisBridge(QObject* parent = nullptr);

    void installSessionHooks();
    void installViewFactories();
    void installEngineHooks();
    void wireEvents();
    void trackSelectedWorkspace();
    void registerBaselineObserver(QtWorkspaceContext* context);
    void evictContextLocked(const QString& binaryIdHex);
    void ensureHubTabWiring(registry::hub_kind_t hub);
    void syncTabsFromContext(QtWorkspaceContext* context);

    docking::AidaDockHost* host_ = nullptr;
    bridge::MenuBridge* menus_ = nullptr;
    bridge::ActionBridge* actions_ = nullptr;
    QPointer<QtWorkspaceContext> active_context_;
    std::function<bool(const QString&)> chat_acceptor_;
    bool events_wired_ = false;
    QHash<QString, QtWorkspaceContext*> contexts_;
    QHash<QtWorkspaceContext*,
          std::shared_ptr<aida::analysis::baseline_publish_observer_t>>
        baseline_observers_;
    std::uint64_t touch_counter_ = 0;
    QPointer<QObject> xref_view_;
    QPointer<QObject> proximity_view_;
    QPointer<QObject> recon_view_;
    QPointer<QObject> stealth_view_;
    std::atomic<int> analysis_hub_default_{0};
    std::atomic<int> analysis_hub_effective_{0};
    std::atomic<int> types_hub_default_{0};
    std::atomic<int> types_hub_effective_{0};
    std::atomic<int> stealth_tab_{0};
    std::atomic<bool> analysis_hub_dirty_{false};
    std::atomic<bool> types_hub_dirty_{false};
    bool analysis_hub_wired_ = false;
    bool types_hub_wired_ = false;
};

// Composition entry point (mirrors documents::install_document_domain). Called
// by the shell composition root; see the wave report wiring spec.
void install_analysis_domain(docking::AidaDockHost* host, bridge::MenuBridge* menus,
                             bridge::ActionBridge* actions);

}
