#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QObject>
#include <QString>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/network/burp/camoufox_bridge.hpp"
#include "core/network/burp/camoufox_install.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaPill;
class AidaStateView;
class AidaStatusDot;
}

namespace aida::qt::net {

class QtByteCappedPlainTextEdit;

class QtJsonLogModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Kind { Console, Network };

    explicit QtJsonLogModel(Kind kind, QObject* parent = nullptr);

    void adopt(std::shared_ptr<const std::vector<nlohmann::json>> rows,
               std::uint64_t signature);
    std::uint64_t signature() const noexcept { return signature_; }
    std::size_t rowCountRaw() const noexcept { return rows_ ? rows_->size() : 0; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    Kind kind_;
    std::shared_ptr<const std::vector<nlohmann::json>> rows_ =
        std::make_shared<const std::vector<nlohmann::json>>();
    std::uint64_t signature_ = 0;
};

// QtHeadlessBrowserController owns the Camoufox headless-bridge state and the
// executor workers. GUI-thread affinity for all view-facing members; every
// bridge mutator runs off-GUI and delivers through queued invokeMethod (dropped
// silently if the controller is destroyed, qobject.cpp:201-202). The event-bus
// callback runs on the publisher thread, so it only re-posts a copied payload
// (never touches members directly).
class QtHeadlessBrowserController : public QObject {
    Q_OBJECT
public:
    explicit QtHeadlessBrowserController(QObject* parent = nullptr);
    ~QtHeadlessBrowserController() override;

    bool initialize();
    void shutdown();
    std::string lastError();

    static QtHeadlessBrowserController* instance() noexcept { return instance_; }
    static void registerInstance(QtHeadlessBrowserController* controller) noexcept;
    static void clearInstance(QtHeadlessBrowserController* controller) noexcept;

    const aida::burp::camoufox::bridge_status_t& bridgeStatus() const { return bridge_status_; }
    const aida::burp::camoufox::install::status_t& installStatus() const { return install_status_; }
    QString lastScreenshotPath() const { return last_screenshot_path_; }
    QString evalOutput() const { return eval_output_; }
    std::shared_ptr<const std::vector<nlohmann::json>> consoleCache() const;
    std::shared_ptr<const std::vector<nlohmann::json>> networkCache() const;
    std::uint64_t consoleSignature() const noexcept { return console_signature_; }
    std::uint64_t networkSignature() const noexcept { return network_signature_; }
    bool installPanelAutoShow() const noexcept { return show_install_panel_; }
    bool installPanelUserToggled() const noexcept { return install_panel_user_toggled_; }

    void scheduleStatusPoll();
    void scheduleInstallProbe(bool alsoLog);
    void installModule();
    void fetchBrowser();
    void startBridge(bool headless, bool humanize, bool blockImages,
                     const QString& osPreset, const QString& localePreset);
    void stopBridge();
    void resetState();
    void navigate(const QString& url);
    void reload();
    void takeScreenshot();
    void injectHookPreset(const QString& preset);
    void removeAllHooks();
    void evaluateJs(const QString& expression);
    void addInitScript(const QString& script);
    void clearConsoleCache();
    void clearNetworkCache();
    void clearEvalOutput();
    void setInstallPanelUserToggled(bool visible);
    void applyBridgeState(int state, const QString& lastError, std::uint32_t childPid);
    void appendInstallLog(const QString& line);

Q_SIGNALS:
    void bridgeStateChanged();
    void installStatusChanged();
    void logsChanged();
    void evalOutputChanged();
    void screenshotChanged();
    void installPanelAutoChanged(bool visible);
    void installLogAppended(const QString& line);

private:
    friend class QtHeadlessBrowserView;

    aida::burp::camoufox::bridge_status_t bridge_status_;
    aida::burp::camoufox::install::status_t install_status_;
    std::shared_ptr<const std::vector<nlohmann::json>> console_cache_ =
        std::make_shared<const std::vector<nlohmann::json>>();
    std::shared_ptr<const std::vector<nlohmann::json>> network_cache_ =
        std::make_shared<const std::vector<nlohmann::json>>();
    std::uint64_t console_signature_ = 0;
    std::uint64_t network_signature_ = 0;
    QString last_screenshot_path_;
    QString eval_output_;
    bool show_install_panel_ = false;
    bool install_panel_user_toggled_ = false;
    std::atomic<bool> poll_in_flight_{false};
    std::atomic<bool> install_poll_in_flight_{false};

    static QtHeadlessBrowserController* instance_;
};

// QtHeadlessBrowserView ports burp/headless_view.cpp. The ImGui 750 ms
// render-driven poll becomes a GUI-thread QTimer started in showEvent and
// stopped in hideEvent (qwidget.h:697-698; timers start/stop on the owning
// thread only, qtimer.cpp:54-60). The 256-line install log uses the documented
// Qt log pattern (setMaximumBlockCount + appendPlainText with native
// at-bottom auto-scroll, qplaintextedit.cpp:1071-1077,2983-3047).
class QtHeadlessBrowserView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit QtHeadlessBrowserView(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void refreshStatusHeader();
    void refreshInstallPanel();
    void refreshBridgeControls();
    void refreshPageInfo();
    void applyConsoleSnapshot();
    void applyNetworkSnapshot();
    void refreshEvalOutput();
    void refreshScreenshot();
    void autoScrollIfPinned(QTableView* view, QAbstractTableModel* model,
                            bool enabled, std::uint64_t signature, std::uint64_t& lastSeen);
    QWidget* buildInstallPanel(QWidget* parent);
    QWidget* buildLeftPane(QWidget* parent);
    QWidget* buildRightPane(QWidget* parent);

    QtHeadlessBrowserController* controller_ = nullptr;
    QTimer* pollTimer_ = nullptr;

    widgets::AidaStatusDot* statusDot_ = nullptr;
    QLabel* statusTitle_ = nullptr;
    QLabel* statusSub_ = nullptr;
    QLabel* statusRight_ = nullptr;
    QLabel* statusRightSub_ = nullptr;
    widgets::AidaPill* camoufoxPill_ = nullptr;
    widgets::AidaPill* webrtcPill_ = nullptr;
    widgets::AidaPill* nativeUaPill_ = nullptr;
    widgets::AidaPill* pageVerifiedPill_ = nullptr;
    widgets::AidaPill* privacyPill_ = nullptr;

    QPushButton* installToggle_ = nullptr;
    QLabel* installStateLabel_ = nullptr;
    QWidget* installPanel_ = nullptr;
    widgets::AidaStatusDot* pythonDot_ = nullptr;
    QLabel* pythonLabel_ = nullptr;
    QLabel* pythonDetail_ = nullptr;
    widgets::AidaStatusDot* moduleDot_ = nullptr;
    QLabel* moduleLabel_ = nullptr;
    QLabel* moduleDetail_ = nullptr;
    widgets::AidaStatusDot* browserDot_ = nullptr;
    QLabel* browserLabel_ = nullptr;
    QLabel* browserDetail_ = nullptr;
    QPlainTextEdit* installLog_ = nullptr;

    widgets::AidaButton* startButton_ = nullptr;
    widgets::AidaButton* stopButton_ = nullptr;
    widgets::AidaButton* resetButton_ = nullptr;
    QCheckBox* headlessCheck_ = nullptr;
    QCheckBox* humanizeCheck_ = nullptr;
    QCheckBox* blockImagesCheck_ = nullptr;

    QLineEdit* urlEdit_ = nullptr;
    widgets::AidaButton* navigateButton_ = nullptr;
    widgets::AidaButton* reloadButton_ = nullptr;
    widgets::AidaButton* screenshotButton_ = nullptr;

    QLabel* pageUrlLabel_ = nullptr;
    QLabel* pageBrowserLabel_ = nullptr;
    QLabel* pageCallsLabel_ = nullptr;
    QLabel* pageErrorLabel_ = nullptr;

    QCheckBox* consoleAutoscroll_ = nullptr;
    QTableView* consoleView_ = nullptr;
    QtJsonLogModel* consoleModel_ = nullptr;
    widgets::AidaStateView* consoleEmpty_ = nullptr;
    QCheckBox* networkAutoscroll_ = nullptr;
    QTableView* networkView_ = nullptr;
    QtJsonLogModel* networkModel_ = nullptr;
    widgets::AidaStateView* networkEmpty_ = nullptr;

    QtByteCappedPlainTextEdit* evalInput_ = nullptr;
    QPlainTextEdit* evalOutput_ = nullptr;
    QComboBox* hookPresetCombo_ = nullptr;
    widgets::AidaButton* injectHookButton_ = nullptr;
    widgets::AidaButton* removeHooksButton_ = nullptr;
    QLabel* screenshotPath_ = nullptr;
    widgets::AidaButton* openExplorerButton_ = nullptr;
    widgets::AidaButton* copyPathButton_ = nullptr;
    QComboBox* osCombo_ = nullptr;
    QComboBox* localeCombo_ = nullptr;

    std::uint64_t last_console_signature_ = 0;
    std::uint64_t last_network_signature_ = 0;
};

}
