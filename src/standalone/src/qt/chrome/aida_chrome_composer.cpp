#include "qt/chrome/aida_chrome_composer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QHBoxLayout>
#include <QMenuBar>
#include <QTimer>
#include <QVBoxLayout>

#include <optional>
#include <string>
#include <functional>
#include <exception>
#include <utility>

#include "core/session/analysis_session.hpp"
#include "core/settings/settings_persistence_service.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/shell_host_contract.hpp"
#include "core/ui/toast_notification.hpp"
#include "core/editor/code_editor.hpp"
#include "helpers/diag_log.hpp"
#include "qt/ai/qt_ai_chat_view.hpp"
#include "qt/analysis/analysis_context_menu_qt.hpp"
#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/auth/qt_auth_view.hpp"
#include "qt/boot/aida_boot_screen.hpp"
#include "qt/bridge/action_bridge.hpp"
#include "qt/bridge/aida_dialog.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/bridge/menu_bridge.hpp"
#include "qt/bridge/settings_bridge.hpp"
#include "qt/bridge/shortcut_bridge.hpp"
#include "qt/chrome/aida_activity_rail.hpp"
#include "qt/chrome/aida_exit_review.hpp"
#include "qt/chrome/aida_legacy_chrome_bridge.hpp"
#include "qt/chrome/aida_status_bar.hpp"
#include "qt/chrome/aida_theme_catalog.hpp"
#include "qt/chrome/aida_theme_picker.hpp"
#include "qt/chrome/aida_title_row.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/chrome/chrome_visibility_tracer.hpp"
#include "qt/dialogs/aida_chat_select_dialog.hpp"
#include "qt/dialogs/aida_driver_status_dialog.hpp"
#include "qt/dialogs/aida_process_attach_dialog.hpp"
#include "qt/dialogs/aida_shortcuts_dialog.hpp"
#include "qt/dialogs/aida_theme_editor.hpp"
#include "qt/dialogs/aida_workspace_dialogs.hpp"
#include "qt/debugger/debugger_domain.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/documents/aida_document_controller.hpp"
#include "qt/documents/aida_document_domain.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/layout/workspace_persistence.hpp"
#include "qt/mcp/qt_mcp_marketplace_view.hpp"
#include "qt/network/network_domain_install.hpp"
#include "qt/overlays/aida_loading_bridge.hpp"
#include "qt/overlays/aida_loading_overlay.hpp"
#include "qt/overlays/aida_overlay_host.hpp"
#include "qt/overlays/aida_quick_open.hpp"
#include "qt/programming/aida_programming_domain.hpp"
#include "qt/qt_main_window.hpp"
#include "qt/qt_startup_orchestrator.hpp"
#include "qt/registry/qt_view_registry.hpp"
#include "qt/scanner/qt_scanner_domain.hpp"
#include "qt/settings/qt_settings_view.hpp"
#include "qt/theme/aida_theme_controller.hpp"

namespace aida::qt::chrome {

namespace {

AidaChromeComposer* g_composer = nullptr;

docking::workspace_preset_t preset_from_ui(aida::ui::workspace_preset_t preset) noexcept
{
    return static_cast<docking::workspace_preset_t>(static_cast<int>(preset));
}

aida::ui::workspace_preset_t preset_to_ui(docking::workspace_preset_t preset) noexcept
{
    return static_cast<aida::ui::workspace_preset_t>(static_cast<int>(preset));
}

aida::ui::workspace_request_result_t workspace_result_to_ui(
    docking::workspace_request_result_t result) noexcept
{
    return static_cast<aida::ui::workspace_request_result_t>(static_cast<int>(result));
}

aida::ui::view_operation_result_t view_result_to_ui(
    const registry::view_operation_result_t& result)
{
    aida::ui::view_operation_result_t mapped;
    mapped.status = static_cast<aida::ui::view_operation_status_t>(
        static_cast<int>(result.status));
    mapped.detail = result.detail;
    return mapped;
}

aida::ui::view_instance_id_t instance_to_ui(
    const registry::view_instance_id_t& instance)
{
    aida::ui::view_instance_id_t mapped;
    mapped.view = instance.view;
    mapped.instance = instance.instance;
    return mapped;
}

registry::view_instance_id_t instance_from_ui(
    const aida::ui::view_instance_id_t& instance)
{
    registry::view_instance_id_t mapped;
    mapped.view = instance.view;
    mapped.instance = instance.instance;
    return mapped;
}

aida::ui::view_category_t category_to_ui(registry::view_category_t category) noexcept
{
    return static_cast<aida::ui::view_category_t>(static_cast<int>(category));
}

docking::dock_region_t region_from_ui(aida::ui::dock_region_t region) noexcept
{
    switch (region) {
    case aida::ui::dock_region_t::navigator: return docking::dock_region_t::left;
    case aida::ui::dock_region_t::documents: return docking::dock_region_t::center;
    case aida::ui::dock_region_t::inspector: return docking::dock_region_t::right;
    case aida::ui::dock_region_t::bottom: return docking::dock_region_t::bottom;
    }
    return docking::dock_region_t::center;
}

}

AidaChromeComposer::AidaChromeComposer(AidaMainWindow* window, QObject* parent)
    : QObject(parent), window_(window)
{
}

AidaChromeComposer::~AidaChromeComposer() = default;

void AidaChromeComposer::compose()
{
    if (!window_)
        return;
    if (g_composer) {
        diag::log_tagged_critical("qt_chrome", "compose_called_twice ignored=1");
        return;
    }
    g_composer = this;

    const auto run_step = [this](const char* name, std::function<void()> fn) {
        diag::log_tagged_critical_fmt("qt_chrome", "compose_step_enter name=%s tid=%lu", name,
            static_cast<unsigned long>(::GetCurrentThreadId()));
        try {
            fn();
        } catch (const std::exception& ex) {
            diag::log_tagged_critical_fmt("qt_chrome",
                "compose_step_exception name=%s what=%s tid=%lu", name, ex.what(),
                static_cast<unsigned long>(::GetCurrentThreadId()));
            throw;
        } catch (...) {
            diag::log_tagged_critical_fmt("qt_chrome",
                "compose_step_exception name=%s what=<non-std> tid=%lu", name,
                static_cast<unsigned long>(::GetCurrentThreadId()));
            throw;
        }
        diag::log_tagged_critical_fmt("qt_chrome", "compose_step_exit name=%s", name);
    };

    run_step("theme_catalog_init", [this]() { AidaThemeCatalogController::instance().initializeFromSettings(); });
    run_step("buildBridges", [this]() { buildBridges(); });
    run_step("wireShellHostServices", [this]() { wireShellHostServices(); });
    run_step("buildCentralArea", [this]() { buildCentralArea(); });
    run_step("buildChromeStrip", [this]() { buildChromeStrip(); });
    run_step("buildOverlays", [this]() { buildOverlays(); });
    run_step("buildControllers", [this]() { buildControllers(); });
    run_step("installDomainRegistrations", [this]() { installDomainRegistrations(); });
    run_step("wireShellCallbacks", [this]() { wireShellCallbacks(); });
    run_step("wireTheme", [this]() { wireTheme(); });
    run_step("wireStatusSegments", [this]() { wireStatusSegments(); });

    diag::log_tagged_critical_fmt("qt_chrome", "chrome_composed tid=%lu",
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

void AidaChromeComposer::buildBridges()
{
    context_ = new bridge::InteractionContextProvider(this);
    actions_ = new bridge::ActionBridge(context_, this);
    shortcuts_ = new bridge::ShortcutBridge(context_, actions_, this);
    menus_ = new bridge::MenuBridge(actions_, context_, this);
    settings_ = new bridge::QtSettingsBridge(this);

    if (window_ && window_->dockHost() && window_->dockHost()->registry()) {
        auto* registry = window_->dockHost()->registry();
        context_->set_active_view_hook([registry]() -> std::pair<std::string, std::string> {
            const auto focused = registry->focused_instance();
            if (!focused)
                return {};
            return { focused->view.value(), focused->instance.value() };
        });
    }
    shortcuts_->install();
}

void AidaChromeComposer::buildCentralArea()
{
    auto* host = window_ ? window_->dockHost() : nullptr;
    QWidget* dock_widget = host ? host->manager() : nullptr;
    if (!dock_widget) {
        diag::log_tagged_critical("qt_chrome", "central_area_missing_dock_manager");
        return;
    }

    QWidget* taken = window_->takeCentralWidget();
    if (taken != dock_widget)
        diag::log_tagged_critical_fmt("qt_chrome",
            "central_take_unexpected taken=0x%llX manager=0x%llX",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(taken)),
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(dock_widget)));

    diag::log_tagged_critical("qt_chrome", "central_ide_container_pre");
    ide_container_ = new QWidget(window_);
    ide_container_->setObjectName(QStringLiteral("aida.ide_container"));
    auto* layout = new QHBoxLayout(ide_container_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    diag::log_tagged_critical("qt_chrome", "central_activity_rail_pre");
    rail_ = new AidaActivityRail(host, actions_, ide_container_);
    diag::log_tagged_critical("qt_chrome", "central_activity_rail_post");
    layout->addWidget(rail_);
    layout->addWidget(dock_widget, 1);
    diag::log_tagged_critical("qt_chrome", "central_set_central_widget_pre");
    window_->setCentralWidget(ide_container_);
    diag::log_tagged_critical("qt_chrome", "central_set_central_widget_post");

    diag::log_tagged_critical("qt_chrome", "central_toast_host_pre");
    toast_host_ = new AidaToastHost(ide_container_);
    AidaToastManager::instance().attachHost(toast_host_);
    diag::log_tagged_critical("qt_chrome", "central_toast_host_post");

    diag::log_tagged_critical("qt_chrome", "central_overlay_host_pre");
    overlay_host_ = new overlays::AidaOverlayHost(ide_container_);
    diag::log_tagged_critical("qt_chrome", "central_overlay_host_post");
}

void AidaChromeComposer::buildChromeStrip()
{
    auto* host = window_ ? window_->dockHost() : nullptr;
    chrome_strip_ = new QWidget(window_);
    chrome_strip_->setObjectName(QStringLiteral("aida.chrome_strip"));
    auto* layout = new QVBoxLayout(chrome_strip_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    title_row_ = new AidaTitleRow(chrome_strip_);
    layout->addWidget(title_row_);

    if (menus_ && host) {
        QMenuBar* bar = menus_->installBar(host);
        layout->addWidget(bar);
        chrome::ChromeVisibilityTracer::install(bar, QStringLiteral("aida.menu_bar"), true);
    }
    window_->setMenuWidget(chrome_strip_);
    chrome::ChromeVisibilityTracer::install(chrome_strip_, QStringLiteral("aida.chrome_strip"), true);

    status_bar_ = new AidaStatusBar(host, window_);
    window_->setStatusBar(status_bar_);

    connect(title_row_, &AidaTitleRow::themeToggleRequested, this, [] {
        AidaThemeCatalogController::instance().toggleDayNight();
    });
    connect(title_row_, &AidaTitleRow::themeMenuRequested, this, [this](const QPoint& pos) {
        if (theme_picker_)
            theme_picker_->openAt(pos);
    });
}

void AidaChromeComposer::buildOverlays()
{
    bridge::AidaDialog::set_notification_hook([](const QString& message) {
        AidaToastManager::instance().push(message, AidaToastType::error, 6.0);
    });

    loading_ = new overlays::AidaLoadingOverlayController(this);
    loading_->bind(overlay_host_);
    auto* host = window_ ? window_->dockHost() : nullptr;
    auto focus_hook = [host](const char* view_id) {
        if (!host || !view_id)
            return;
        static_cast<void>(host->open_or_focus(aida::ui::stable_view_id_t(view_id)));
    };
    loading_->setViewFocusHook(focus_hook);
    loading_binary_overlay::set_view_focus_hook(focus_hook);
    legacy_chrome_hooks().focus_view = [focus_hook](const std::string& view_id) {
        focus_hook(view_id.c_str());
    };

    dialogs::set_process_attach_hooks({
        focus_hook,
        [](const std::string& text) {
            auto& hooks = legacy_chrome_hooks();
            if (hooks.push_output_line)
                hooks.push_output_line(text);
        }
    });
}

void AidaChromeComposer::buildControllers()
{
    auto* host = window_ ? window_->dockHost() : nullptr;

    quick_open_ = new overlays::AidaQuickOpenController(host, window_, this);
    workspace_ = new dialogs::AidaWorkspaceController(host, window_, this);
    attach_dialog_ = new dialogs::AidaProcessAttachDialog(window_);
    driver_dialog_ = new dialogs::AidaDriverStatusDialog(window_);
    shortcuts_dialog_ = new dialogs::AidaShortcutsDialog(shortcuts_, window_);
    theme_editor_ = new dialogs::AidaThemeEditorDialog(window_);
    theme_picker_ = new AidaThemePickerPopup(title_row_);
    connect(theme_picker_, &AidaThemePickerPopup::editThemeRequested, this,
            [this](int index) { theme_editor_->editTheme(index); });
    connect(theme_picker_, &AidaThemePickerPopup::createThemeRequested, this,
            [this] { theme_editor_->createTheme(); });

    exit_review_ = new AidaExitReviewController(window_, this);
    window_->setExitReviewGateHook([this] { return exit_review_->gateHook(); });

    dialogs::chatSelectController().install(window_);

    boot_screen_ = new boot::AidaBootScreen();
    welcome_screen_ = new boot::AidaWelcomeScreen();
    boot_ = new boot::AidaBootController(this);
    boot_->attach(ide_container_, boot_screen_, welcome_screen_);
    boot_->begin();
    connect(boot_, &boot::AidaBootController::bootFinished, this, [this] {
        if (toast_host_)
            toast_host_->raise();
        if (overlay_host_)
            overlay_host_->raise();
    });

    if (host && host->registry()) {
        connect(host->registry(), &registry::qt_view_registry_t::focusedInstanceChanged,
                this, [this] { refreshBreadcrumb(); });
    }
    if (host && host->persistence()) {
        connect(host->persistence(),
                &layout::WorkspacePersistenceController::activeWorkspaceChanged,
                this, [this] { refreshBreadcrumb(); });
    }
    auto* breadcrumb_timer = new QTimer(this);
    breadcrumb_timer->setInterval(750);
    breadcrumb_timer->setTimerType(Qt::CoarseTimer);
    connect(breadcrumb_timer, &QTimer::timeout, this, [this] { refreshBreadcrumb(); });
    breadcrumb_timer->start();
}

void AidaChromeComposer::installDomainRegistrations()
{
    auto* host = window_ ? window_->dockHost() : nullptr;
    if (!host) {
        diag::log_tagged_critical("qt_chrome", "domain_install_skipped reason=no_dock_host");
        return;
    }

    diag::log_tagged_critical("qt_chrome", "domain_install enter=documents");
    documents::install_document_domain(host, menus_);
    diag::log_tagged_critical("qt_chrome", "domain_install exit=documents");
    diag::log_tagged_critical("qt_chrome", "domain_install enter=analysis");
    analysis::install_analysis_domain(host, menus_, actions_);
    diag::log_tagged_critical("qt_chrome", "domain_install exit=analysis");
    diag::log_tagged_critical("qt_chrome", "domain_install enter=debugger");
    debugger::install_debugger_domain(host, menus_, actions_);
    diag::log_tagged_critical("qt_chrome", "domain_install exit=debugger");
    diag::log_tagged_critical("qt_chrome", "domain_install enter=scanner");
    scanner::install_scanner_domain(host, menus_, actions_);
    diag::log_tagged_critical("qt_chrome", "domain_install exit=scanner");
    diag::log_tagged_critical("qt_chrome", "domain_install enter=network");
    net::install_network_domain(host);
    diag::log_tagged_critical("qt_chrome", "domain_install exit=network");
    diag::log_tagged_critical("qt_chrome", "domain_install enter=ai");
    ai::install_ai_domain(host, menus_, actions_);
    diag::log_tagged_critical("qt_chrome", "domain_install exit=ai");
    diag::log_tagged_critical("qt_chrome", "domain_install enter=settings");
    settings::install_settings_domain(host);
    diag::log_tagged_critical("qt_chrome", "domain_install exit=settings");
    diag::log_tagged_critical("qt_chrome", "domain_install enter=auth");
    auth::install_auth_domain(host);
    diag::log_tagged_critical("qt_chrome", "domain_install exit=auth");
    diag::log_tagged_critical("qt_chrome", "domain_install enter=mcp");
    mcp::install_mcp_marketplace_domain(host);
    diag::log_tagged_critical("qt_chrome", "domain_install exit=mcp");
    diag::log_tagged_critical("qt_chrome", "domain_install enter=programming");
    programming::install_programming_domain(host, menus_, actions_);
    diag::log_tagged_critical("qt_chrome", "domain_install exit=programming");
    diag::log_tagged_critical("qt_chrome", "domain_install enter=analysis_ctx_menu");
    analysis::install_analysis_context_menu_display();
    diag::log_tagged_critical("qt_chrome", "domain_install exit=analysis_ctx_menu");

    documents::set_retained_entity_menu_display(
        [menus = menus_](const aida::ui::application_ui::retained_entity_context_t& context,
                         aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos,
                         QWidget* parent) {
            if (menus)
                menus->show_retained_entity_menu(context, origin, global_pos, parent);
        });

    toast_notification::install_qt_forward(
        [](const std::string& message, toast_notification::toast_type_t type, float duration,
           std::string action_label, std::function<void()> on_click) {
            const auto qt_type = static_cast<AidaToastType>(static_cast<int>(type));
            const QString text = QString::fromStdString(message);
            if (action_label.empty()) {
                AidaToastManager::instance().push(text, qt_type, static_cast<double>(duration));
            } else {
                AidaToastManager::instance().pushWithAction(text, qt_type,
                    QString::fromStdString(action_label), std::move(on_click),
                    static_cast<double>(duration));
            }
        });

    auto& hooks = legacy_chrome_hooks();
    hooks.exit_gate.committed = [] {
        auto* controller = documents::document_domain().controller;
        return controller ? controller->exitReviewCommitted() : true;
    };
    hooks.exit_gate.request = []() -> std::pair<bool, std::string> {
        auto* controller = documents::document_domain().controller;
        if (!controller)
            return { false, "The document controller is unavailable" };
        const auto result = controller->requestExitReview();
        return { result.succeeded, result.detail };
    };
    hooks.exit_gate.consume_ready = [] {
        auto* controller = documents::document_domain().controller;
        return controller && controller->consumeExitReviewReady();
    };
    hooks.exit_gate.cancel = [] {
        if (auto* controller = documents::document_domain().controller)
            controller->cancelCloseAll();
    };

    diag::log_tagged("qt_chrome", "domain_registrations_installed");
}

void AidaChromeComposer::wireShellCallbacks()
{
    aida::ui::application_ui::shell_callbacks_t callbacks;
    callbacks.open_file = [this] {
        static const char k_open_file_filter[] =
            "All files (*.*)\0*.*\0"
            "C/C++ (*.c;*.cpp;*.h;*.hpp)\0*.c;*.cpp;*.h;*.hpp\0\0";
        const auto picked = dialogs::open_file(window_, QStringLiteral("Open File"),
            k_open_file_filter);
        if (!picked)
            return;
        auto& hooks = legacy_chrome_hooks();
        if (hooks.open_file_path)
            hooks.open_file_path(*picked);
    };
    callbacks.open_folder = [this] {
        const auto picked = dialogs::choose_directory(window_,
            QStringLiteral("Open Workspace Folder"));
        if (!picked)
            return;
        auto& hooks = legacy_chrome_hooks();
        if (hooks.open_folder_path)
            hooks.open_folder_path(*picked);
    };
    callbacks.save_as = [] {
        auto& hooks = legacy_chrome_hooks();
        if (hooks.save_active_document_as)
            hooks.save_active_document_as();
    };
    callbacks.exit_application = [this] {
        if (window_)
            window_->close();
    };
    callbacks.load_binary = [this] {
        static const char k_load_binary_filter[] = "All files (*.*)\0*.*\0\0";
        const auto picked = dialogs::open_file(window_, QStringLiteral("Open"),
            k_load_binary_filter);
        if (!picked || picked->empty()) {
            diag::log_tagged("chrome", "load_pe cancelled");
            return;
        }
        const std::string path = *picked;
        const bool ok = analysis_session::open_session(path);
        if (ok) {
            diag::log_tagged_fmt("chrome", "load_pe ok path=%s", path.c_str());
        } else {
            const char* err = analysis_session::last_error();
            diag::log_tagged_fmt("chrome", "load_pe failed path=%s err=%s",
                path.c_str(), err ? err : "(none)");
        }
    };
    callbacks.attach_process = [this] {
        if (attach_dialog_)
            attach_dialog_->openFresh();
    };
    callbacks.open_settings = [this] {
        auto* host = window_ ? window_->dockHost() : nullptr;
        if (host)
            static_cast<void>(host->open_or_focus(aida::ui::stable_view_id_t("view.settings")));
    };
    callbacks.open_driver_status = [this] {
        if (driver_dialog_)
            driver_dialog_->openFresh();
    };
    callbacks.new_chat = [] {
        auto& hooks = legacy_chrome_hooks();
        if (hooks.new_chat)
            hooks.new_chat();
    };
    callbacks.open_shortcuts = [this] {
        if (shortcuts_dialog_)
            shortcuts_dialog_->openFresh();
        diag::log_tagged("chrome", "shortcuts_popup open=true source=action");
    };
    callbacks.open_workspace_save_as = [this] {
        if (workspace_)
            workspace_->openSaveAs();
    };
    callbacks.open_workspace_manager = [this] {
        if (workspace_)
            workspace_->openManager();
    };
    callbacks.open_workspace_reset_all = [this] {
        if (workspace_)
            workspace_->openResetAllReview();
    };
    callbacks.persist_workspace = [] {
        static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
    };
    callbacks.toggle_maximize = [this] {
        if (!window_)
            return;
        if (window_->isMaximized())
            window_->showNormal();
        else
            window_->showMaximized();
    };
    callbacks.decompile_or_focus_pseudocode = [] {
        auto& hooks = legacy_chrome_hooks();
        if (hooks.decompile_or_focus_pseudocode)
            return hooks.decompile_or_focus_pseudocode();
        return aida::ui::action_handler_result_t::failed(
            "The decompile action bridge is not available");
    };
    callbacks.decompile_or_focus_pseudocode_capability = [] {
        auto& hooks = legacy_chrome_hooks();
        if (hooks.decompile_or_focus_pseudocode_capability)
            return hooks.decompile_or_focus_pseudocode_capability();
        return aida::ui::capability_state_t::unavailable(
            "The decompile action bridge is not available");
    };
    callbacks.action_executed = [](const char* action_id) {
        diag::log_tagged_fmt("ui", "action_executed id=%s", action_id ? action_id : "<null>");
    };
    aida::ui::application_ui::configure_shell_callbacks(std::move(callbacks));
}

void AidaChromeComposer::wireShellHostServices()
{
    auto* host = window_ ? window_->dockHost() : nullptr;
    auto* registry = host ? host->registry() : nullptr;
    auto* persistence = host ? host->persistence() : nullptr;

    aida::ui::shell_host_services_t services;
    services.set_clipboard_text = [](const char* text) {
        clipboard::set_text(QString::fromUtf8(text ? text : ""));
    };
    services.show_error_toast = [](const std::string& message, double duration) {
        AidaToastManager::instance().push(QString::fromStdString(message),
            AidaToastType::error, duration);
    };
    services.for_each_view_descriptor = [registry](const std::function<void(
            const aida::ui::view_host_descriptor_t&)>& visitor) {
        if (!registry)
            return;
        registry->for_each_descriptor([&](const registry::qt_view_descriptor_t& descriptor) {
            aida::ui::view_host_descriptor_t mapped;
            mapped.id = descriptor.id;
            mapped.display_name = descriptor.display_name;
            mapped.category = category_to_ui(descriptor.category);
            mapped.closeable = descriptor.closeable;
            mapped.registry_surface = true;
            visitor(mapped);
        });
    };
    services.for_each_open_view_instance = [registry, host](const std::function<void(
            const aida::ui::view_host_instance_t&)>& visitor) {
        if (!registry)
            return;
        registry->for_each_instance([&](const registry::qt_view_descriptor_t& descriptor,
                                        const registry::view_instance_state_t& instance) {
            aida::ui::view_host_instance_t mapped;
            mapped.id = instance_to_ui(instance.id);
            mapped.display_name = instance.display_name;
            mapped.window_name = instance.display_name;
            mapped.open = instance.open;
            mapped.focused = instance.focused;
            mapped.closeable = descriptor.closeable;
            mapped.pinned = host ? host->is_pinned(instance.id) : false;
            visitor(mapped);
        }, true);
    };
    services.find_view_descriptor = [registry](const aida::ui::stable_view_id_t& id)
        -> std::optional<aida::ui::view_host_descriptor_t> {
        if (!registry)
            return std::nullopt;
        const auto* descriptor = registry->find_descriptor(id);
        if (!descriptor)
            return std::nullopt;
        aida::ui::view_host_descriptor_t mapped;
        mapped.id = descriptor->id;
        mapped.display_name = descriptor->display_name;
        mapped.category = category_to_ui(descriptor->category);
        mapped.closeable = descriptor->closeable;
        mapped.registry_surface = true;
        return mapped;
    };
    services.evaluate_view = [registry](const aida::ui::stable_view_id_t& id,
        const aida::ui::interaction_context_t& context) {
        return registry
            ? registry->evaluate(id, context)
            : aida::ui::capability_state_t::unavailable("The view host is unavailable");
    };
    services.is_view_open = [host](const aida::ui::stable_view_id_t& id) {
        return host && host->is_open(id);
    };
    services.is_view_instance_open = [registry](const aida::ui::view_instance_id_t& id) {
        return registry && registry->is_open(instance_from_ui(id));
    };
    services.focused_view_instance = [registry]()
        -> std::optional<aida::ui::view_instance_id_t> {
        if (!registry)
            return std::nullopt;
        const auto focused = registry->focused_instance();
        if (!focused)
            return std::nullopt;
        return instance_to_ui(*focused);
    };
    services.view_window_name = [registry](const aida::ui::view_instance_id_t& id) {
        if (!registry)
            return std::string();
        const auto* instance = static_cast<const registry::qt_view_registry_t*>(registry)
            ->find_instance(instance_from_ui(id));
        return instance ? instance->display_name : std::string();
    };
    services.is_view_pinned = [host](const aida::ui::view_instance_id_t& id) {
        return host && host->is_pinned(instance_from_ui(id));
    };
    services.can_duplicate_view = [host](const aida::ui::view_instance_id_t& id) {
        return host && host->can_duplicate(instance_from_ui(id));
    };
    services.can_reset_view_state = [host](const aida::ui::view_instance_id_t& id) {
        return host && host->can_reset_state(instance_from_ui(id));
    };
    services.can_reopen_last_closed_view = [host] {
        return host && host->can_reopen_last_closed();
    };
    services.open_or_focus_view = [host](const aida::ui::stable_view_id_t& id) {
        return view_result_to_ui(host ? host->open_or_focus(id)
            : registry::view_operation_result_t{
                registry::view_operation_status_t::unavailable, "The view host is unavailable"});
    };
    services.close_view = [host](const aida::ui::stable_view_id_t& id) {
        return view_result_to_ui(host ? host->close(id)
            : registry::view_operation_result_t{
                registry::view_operation_status_t::unavailable, "The view host is unavailable"});
    };
    services.close_view_instance = [host](const aida::ui::view_instance_id_t& id) {
        return view_result_to_ui(host ? host->close_instance(instance_from_ui(id))
            : registry::view_operation_result_t{
                registry::view_operation_status_t::unavailable, "The view host is unavailable"});
    };
    services.close_other_view_instances = [host](const aida::ui::view_instance_id_t& id) {
        return view_result_to_ui(host ? host->close_other_instances(instance_from_ui(id))
            : registry::view_operation_result_t{
                registry::view_operation_status_t::unavailable, "The view host is unavailable"});
    };
    services.toggle_view_pin = [host](const aida::ui::view_instance_id_t& id) {
        return view_result_to_ui(host ? host->toggle_pin(instance_from_ui(id))
            : registry::view_operation_result_t{
                registry::view_operation_status_t::unavailable, "The view host is unavailable"});
    };
    services.duplicate_view_instance = [host](const aida::ui::view_instance_id_t& id) {
        return view_result_to_ui(host ? host->duplicate_instance(instance_from_ui(id))
            : registry::view_operation_result_t{
                registry::view_operation_status_t::unavailable, "The view host is unavailable"});
    };
    services.request_view_reset_state = [host](const aida::ui::view_instance_id_t& id) {
        return view_result_to_ui(host ? host->request_reset_state(instance_from_ui(id))
            : registry::view_operation_result_t{
                registry::view_operation_status_t::unavailable, "The view host is unavailable"});
    };
    services.reopen_last_closed_view = [host] {
        return view_result_to_ui(host ? host->reopen_last_closed()
            : registry::view_operation_result_t{
                registry::view_operation_status_t::unavailable, "The view host is unavailable"});
    };
    services.open_default_missing_views = [host] {
        return view_result_to_ui(host ? host->open_default_missing()
            : registry::view_operation_result_t{
                registry::view_operation_status_t::unavailable, "The view host is unavailable"});
    };
    services.active_workspace_preset = [persistence] {
        return persistence ? preset_to_ui(persistence->active_preset())
            : aida::ui::workspace_preset_t::analysis;
    };
    services.active_workspace_identity = [persistence] {
        aida::ui::workspace_identity_t identity;
        if (!persistence)
            return identity;
        const auto active = persistence->active_identity();
        identity.kind = active.kind == docking::workspace_identity_kind_t::user
            ? aida::ui::workspace_identity_kind_t::user
            : aida::ui::workspace_identity_kind_t::built_in;
        identity.preset = preset_to_ui(active.preset);
        identity.user_name = active.user_name;
        return identity;
    };
    services.user_layout_catalog_ready = [persistence] {
        return persistence && persistence->user_layout_catalog_ready();
    };
    services.layout_locked = [persistence] {
        return persistence && persistence->layout_locked();
    };
    services.set_layout_locked = [persistence](bool locked) {
        return workspace_result_to_ui(persistence
            ? persistence->set_layout_locked(locked)
            : docking::workspace_request_result_t::unavailable);
    };
    services.workspace_operation_pending = [persistence] {
        return persistence && persistence->operation_pending();
    };
    services.workspace_operation_status = [persistence] {
        return persistence ? persistence->operation_status() : std::string();
    };
    services.dock_space_ready = [host] {
        return host && host->manager() != nullptr;
    };
    services.dock_region_available = [](aida::ui::dock_region_t) {
        return true;
    };
    services.inspect_surface_placement = [host, registry](const std::string& surface_id) {
        aida::ui::surface_placement_t placement;
        const aida::ui::stable_view_id_t id(surface_id);
        placement.realized = registry && registry->find_descriptor(id) != nullptr;
        placement.docked = host && host->is_open(id);
        placement.region = std::nullopt;
        return placement;
    };
    services.float_window = [host, registry](const std::string& surface_id) {
        if (!host || !registry)
            return workspace_result_to_ui(docking::workspace_request_result_t::unavailable);
        const auto instance = registry->instance_for(aida::ui::stable_view_id_t(surface_id));
        const auto result = host->float_instance(instance);
        return workspace_result_to_ui(result.ok() ? docking::workspace_request_result_t::completed
            : docking::workspace_request_result_t::failed);
    };
    services.dock_window = [host, registry](const std::string& surface_id,
        aida::ui::dock_region_t region) {
        if (!host || !registry)
            return workspace_result_to_ui(docking::workspace_request_result_t::unavailable);
        const auto instance = registry->instance_for(aida::ui::stable_view_id_t(surface_id));
        const auto result = host->move_instance(instance, region_from_ui(region));
        return workspace_result_to_ui(result.ok() ? docking::workspace_request_result_t::completed
            : docking::workspace_request_result_t::failed);
    };
    services.split_window = [host, registry](const std::string& surface_id,
        const std::string&, aida::ui::dock_split_direction_t direction) {
        if (!host || !registry)
            return workspace_result_to_ui(docking::workspace_request_result_t::unavailable);
        const auto instance = registry->instance_for(aida::ui::stable_view_id_t(surface_id));
        const auto region = direction == aida::ui::dock_split_direction_t::left
            ? docking::dock_region_t::left
            : direction == aida::ui::dock_split_direction_t::right
                ? docking::dock_region_t::right
                : direction == aida::ui::dock_split_direction_t::down
                    ? docking::dock_region_t::bottom
                    : docking::dock_region_t::center;
        const auto result = host->move_instance(instance, region);
        return workspace_result_to_ui(result.ok() ? docking::workspace_request_result_t::completed
            : docking::workspace_request_result_t::failed);
    };
    services.switch_workspace = [persistence](aida::ui::workspace_preset_t preset) {
        return workspace_result_to_ui(persistence
            ? persistence->switch_to(preset_from_ui(preset))
            : docking::workspace_request_result_t::unavailable);
    };
    services.save_active_user_layout = [persistence] {
        return workspace_result_to_ui(persistence
            ? persistence->save_active_user_layout()
            : docking::workspace_request_result_t::unavailable);
    };
    services.restore_builtin_workspace = [persistence](aida::ui::workspace_preset_t preset) {
        return workspace_result_to_ui(persistence
            ? persistence->restore_builtin(preset_from_ui(preset))
            : docking::workspace_request_result_t::unavailable);
    };
    services.reset_current_layout = [persistence] {
        return workspace_result_to_ui(persistence
            ? persistence->reset_current()
            : docking::workspace_request_result_t::unavailable);
    };
    services.activate_safe_layout = [persistence] {
        return workspace_result_to_ui(persistence
            ? persistence->activate_safe_layout()
            : docking::workspace_request_result_t::unavailable);
    };
    services.open_missing_views = [host] {
        return host ? workspace_result_to_ui(host->open_missing_views())
            : workspace_result_to_ui(docking::workspace_request_result_t::unavailable);
    };
    aida::ui::application_ui::configure_shell_host_services(std::move(services));
}

void AidaChromeComposer::wireTheme()
{
    connect(&theme::AidaThemeController::instance(),
            &theme::AidaThemeController::themeChanged, window_, [this] {
        if (window_)
            window_->applyDwmBackdrop();
    });
    if (workspace_) {
        connect(workspace_, &dialogs::AidaWorkspaceController::statusMessage, this,
                [this](const QString& message, int timeout_ms) {
            if (status_bar_)
                status_bar_->showMessage(message, timeout_ms);
        });
    }
}

void AidaChromeComposer::wireStatusSegments()
{
    if (!status_bar_)
        return;
    connect(status_bar_, &AidaStatusBar::segmentActivated, this,
            [this](AidaStatusSegmentId id) {
        auto* host = window_ ? window_->dockHost() : nullptr;
        if (!host)
            return;
        const char* view = nullptr;
        switch (id) {
        case AidaStatusSegmentId::Target: view = "view.sessions"; break;
        case AidaStatusSegmentId::Debugger: view = "view.debug.cpu"; break;
        case AidaStatusSegmentId::Network: view = "view.network.capture"; break;
        case AidaStatusSegmentId::Mcp: view = "view.mcp_log"; break;
        case AidaStatusSegmentId::Driver: view = "view.driver_log"; break;
        case AidaStatusSegmentId::Tasks: view = "view.background_tasks"; break;
        case AidaStatusSegmentId::Diagnostics: view = "view.diagnostics"; break;
        case AidaStatusSegmentId::Location:
        case AidaStatusSegmentId::Workspace:
        case AidaStatusSegmentId::Frame: break;
        }
        if (view)
            static_cast<void>(host->open_or_focus(aida::ui::stable_view_id_t(view)));
    });
}

void AidaChromeComposer::refreshBreadcrumb()
{
    if (!title_row_)
        return;
    QStringList segments;
    const auto workspace = analysis_session::active_workspace();
    bool live_target = false;
    if (workspace) {
        live_target = true;
        const auto& bin = workspace->identity().bin_name();
        if (!bin.empty())
            segments.push_back(QString::fromStdString(bin));
    }
    const auto editor_document = code_editor_widget::document_state();
    if (editor_document.active && !editor_document.filename.empty())
        segments.push_back(QString::fromStdString(editor_document.filename));
    auto* host = window_ ? window_->dockHost() : nullptr;
    auto* registry = host ? host->registry() : nullptr;
    if (registry) {
        const auto focused = registry->focused_instance();
        if (focused) {
            const auto* descriptor = registry->find_descriptor(focused->view);
            if (descriptor &&
                (focused->view.value() != "document.code" ||
                 editor_document.filename.empty()))
                segments.push_back(QString::fromStdString(descriptor->display_name));
        }
    }
    title_row_->setBreadcrumbSegments(segments);

    auto* persistence = host ? host->persistence() : nullptr;
    if (persistence) {
        const auto identity = persistence->active_identity();
        title_row_->setWorkspacePillText(
            identity.kind == docking::workspace_identity_kind_t::user
                ? QString::fromStdString(identity.user_name)
                : QString::fromLatin1(docking::preset_descriptor(identity.preset)
                    .display_name.data(),
                    static_cast<qsizetype>(docking::preset_descriptor(identity.preset)
                        .display_name.size())),
            live_target);
    }
}

void AidaChromeComposer::bindOrchestrator(AidaStartupOrchestrator* orchestrator)
{
    if (boot_)
        boot_->setOrchestrator(orchestrator);
    if (boot_ && orchestrator)
        connect(boot_, &boot::AidaBootController::bootFinished, orchestrator,
                &AidaStartupOrchestrator::onBootFinished);
}

AidaChromeComposer* composeChrome(AidaMainWindow* window)
{
    auto* composer = new AidaChromeComposer(window, window);
    composer->compose();
    return composer;
}

AidaChromeComposer* chromeComposer()
{
    return g_composer;
}

}
