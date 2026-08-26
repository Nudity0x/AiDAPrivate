#pragma once

#include <QObject>
#include <QModelIndex>
#include <QString>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/network/burp/browser_launch.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaPill;
class AidaStateView;
}

namespace aida::qt::net {

class QtBrowserProcessModel : public SnapshotTableModel<aida::burp::browser::browser_status_t> {
    Q_OBJECT
public:
    enum Column { Pid = 0, Proxy, Uptime, Strategy, Spki, Browser, Profile, Actions,
                  ColumnCount };

    explicit QtBrowserProcessModel(QObject* parent = nullptr);

    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const aida::burp::browser::browser_status_t& row, int column,
                      int role) const override;
};

class QtBrowserLauncherController : public QObject {
    Q_OBJECT
public:
    explicit QtBrowserLauncherController(QObject* parent = nullptr);

    void launch(const aida::burp::browser::browser_launch_config_t& config);
    void killAll();
    void killProcess(std::uint32_t pid);
    void refreshStatus();
    void pollCertStatus();

    bool launching() const noexcept { return launching_.load(); }
    QString lastLaunchStatus() const { return last_launch_status_; }

    // Byte-verbatim port of browser::stage_camoufox_url (http/https allowlist,
    // identical rejection string). The staged URL is consumed by the next
    // launcher view creation or applied live to the registered instance.
    static bool stageCamoufoxUrl(const std::string& url, std::string& reason);
    static QString stagedUrl();
    static void clearStagedUrl();

Q_SIGNALS:
    void launchStateChanged();
    void statusChanged();
    void certStatusChanged(bool caReady, bool caInstalled, const QString& spkiPrefix);

private:
    void setLastLaunchStatus(const QString& status);

    std::atomic<bool> launching_{false};
    QString last_launch_status_;
    std::uint64_t launch_generation_ = 0;
    std::uint64_t cert_poll_serial_ = 0;
};

// QtBrowserLauncherView ports burp/browser_view.cpp. The launch/stop calls
// block for seconds inside camoufox_bridge (process spawn + readiness poll),
// so they run on executor workers with generation-fenced queued completion —
// the GUI thread never waits. QProcess is deliberately NOT introduced: the
// Camoufox child process tree is spawned/owned/supervised inside
// camoufox_bridge.cpp/browser_launch.cpp.
class QtBrowserLauncherView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit QtBrowserLauncherView(QWidget* parent = nullptr);
    ~QtBrowserLauncherView() override;

    void applyStagedUrl(const QString& url);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void refreshStatusWidgets();
    void refreshProcessTable();
    void refreshCertPill(bool caReady, bool caInstalled, const QString& spkiPrefix);
    void launchNow();
    void stopNow();

    QtBrowserLauncherController* controller_ = nullptr;
    QLabel* statusLine_ = nullptr;
    widgets::AidaPill* camoufoxPill_ = nullptr;
    widgets::AidaPill* webrtcPill_ = nullptr;
    widgets::AidaPill* nativeUaPill_ = nullptr;
    widgets::AidaPill* pageVerifiedPill_ = nullptr;
    widgets::AidaPill* privacyPill_ = nullptr;
    widgets::AidaPill* proxyPill_ = nullptr;
    widgets::AidaPill* caPill_ = nullptr;
    QLineEdit* urlEdit_ = nullptr;
    QLineEdit* profileEdit_ = nullptr;
    QComboBox* strategyCombo_ = nullptr;
    QLabel* debugWarning_ = nullptr;
    QCheckBox* clearProfileCheck_ = nullptr;
    QCheckBox* proxyOverrideCheck_ = nullptr;
    QSpinBox* proxyPortSpin_ = nullptr;
    widgets::AidaButton* openButton_ = nullptr;
    widgets::AidaButton* stopButton_ = nullptr;
    QLabel* launchStatusLabel_ = nullptr;
    QTableView* processView_ = nullptr;
    QtBrowserProcessModel* processModel_ = nullptr;
    widgets::AidaStateView* processesEmpty_ = nullptr;
    QTimer* statusTimer_ = nullptr;
    QTimer* certTimer_ = nullptr;
    bool ca_ready_ = false;
    bool ca_installed_ = false;
    QString spki_prefix_;
};

}
