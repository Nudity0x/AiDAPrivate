#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class QWidget;

namespace aida::qt {
class AidaMainWindow;
class AidaStartupOrchestrator;
}

namespace aida::qt::bridge {
class ActionBridge;
class InteractionContextProvider;
class MenuBridge;
class QtSettingsBridge;
class ShortcutBridge;
}

namespace aida::qt::boot {
class AidaBootController;
class AidaBootScreen;
class AidaWelcomeScreen;
}

namespace aida::qt::overlays {
class AidaLoadingOverlayController;
class AidaOverlayHost;
class AidaQuickOpenController;
}

namespace aida::qt::dialogs {
class AidaDriverStatusDialog;
class AidaProcessAttachDialog;
class AidaShortcutsDialog;
class AidaThemeEditorDialog;
class AidaWorkspaceController;
}

namespace aida::qt::chrome {

class AidaActivityRail;
class AidaExitReviewController;
class AidaStatusBar;
class AidaThemePickerPopup;
class AidaTitleRow;
class AidaToastHost;

class AidaChromeComposer : public QObject {
    Q_OBJECT
public:
    AidaChromeComposer(AidaMainWindow* window, QObject* parent = nullptr);
    ~AidaChromeComposer() override;

    void compose();
    void bindOrchestrator(AidaStartupOrchestrator* orchestrator);

    AidaMainWindow* window() const noexcept { return window_; }
    bridge::ActionBridge* actions() const noexcept { return actions_; }
    bridge::MenuBridge* menus() const noexcept { return menus_; }
    bridge::ShortcutBridge* shortcuts() const noexcept { return shortcuts_; }
    bridge::InteractionContextProvider* context() const noexcept { return context_; }
    bridge::QtSettingsBridge* settings() const noexcept { return settings_; }
    AidaTitleRow* titleRow() const noexcept { return title_row_; }
    AidaActivityRail* activityRail() const noexcept { return rail_; }
    AidaStatusBar* statusBar() const noexcept { return status_bar_; }
    AidaToastHost* toastHost() const noexcept { return toast_host_; }
    overlays::AidaOverlayHost* overlayHost() const noexcept { return overlay_host_; }
    overlays::AidaQuickOpenController* quickOpen() const noexcept { return quick_open_; }
    dialogs::AidaWorkspaceController* workspace() const noexcept { return workspace_; }
    AidaExitReviewController* exitReview() const noexcept { return exit_review_; }
    boot::AidaBootController* boot() const noexcept { return boot_; }

private:
    void buildBridges();
    void buildChromeStrip();
    void buildCentralArea();
    void buildOverlays();
    void buildControllers();
    void installDomainRegistrations();
    void wireShellCallbacks();
    void wireShellHostServices();
    void wireTheme();
    void wireStatusSegments();
    void refreshBreadcrumb();

    AidaMainWindow* window_ = nullptr;

    bridge::InteractionContextProvider* context_ = nullptr;
    bridge::ActionBridge* actions_ = nullptr;
    bridge::MenuBridge* menus_ = nullptr;
    bridge::ShortcutBridge* shortcuts_ = nullptr;
    bridge::QtSettingsBridge* settings_ = nullptr;

    QWidget* chrome_strip_ = nullptr;
    QWidget* ide_container_ = nullptr;
    AidaTitleRow* title_row_ = nullptr;
    AidaActivityRail* rail_ = nullptr;
    AidaStatusBar* status_bar_ = nullptr;
    AidaToastHost* toast_host_ = nullptr;
    overlays::AidaOverlayHost* overlay_host_ = nullptr;
    overlays::AidaLoadingOverlayController* loading_ = nullptr;
    overlays::AidaQuickOpenController* quick_open_ = nullptr;
    dialogs::AidaWorkspaceController* workspace_ = nullptr;
    dialogs::AidaProcessAttachDialog* attach_dialog_ = nullptr;
    dialogs::AidaDriverStatusDialog* driver_dialog_ = nullptr;
    dialogs::AidaShortcutsDialog* shortcuts_dialog_ = nullptr;
    dialogs::AidaThemeEditorDialog* theme_editor_ = nullptr;
    AidaThemePickerPopup* theme_picker_ = nullptr;
    AidaExitReviewController* exit_review_ = nullptr;
    boot::AidaBootScreen* boot_screen_ = nullptr;
    boot::AidaWelcomeScreen* welcome_screen_ = nullptr;
    boot::AidaBootController* boot_ = nullptr;
};

AidaChromeComposer* composeChrome(AidaMainWindow* window);
AidaChromeComposer* chromeComposer();

}
