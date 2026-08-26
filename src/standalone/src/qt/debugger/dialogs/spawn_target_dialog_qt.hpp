#pragma once

#include "qt/bridge/aida_dialog.hpp"

#include <QPointer>
#include <QString>

#include <cstdint>
#include <string>

#include "core/runtime/run_target.hpp"

class QButtonGroup;
class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;

namespace aida::qt::debugger {

// Queued-signal bridge for the custom-VM bridge operation (the
// QFutureWatcher-equivalent queued-delivery channel: the worker stays on
// aida::infra::executor; phase/completion cross via QMetaObject::invokeMethod
// with Qt::QueuedConnection; the dialog polls nothing).
class DebuggerBridgeOpBridge : public QObject {
    Q_OBJECT
public:
    explicit DebuggerBridgeOpBridge(QObject* parent = nullptr);

    bool pending() const;
    void requestCancel();

Q_SIGNALS:
    void phaseChanged(int phase, const QString& text);
    void bridgeFinished(bool activated, bool cancelled, const QString& detail,
                        const QString& guestSample);
    void dispatchFailed(const QString& detail);
};

// "Malware Lab Run" spawn target dialog (ports spawn_target_dialog.hpp's
// ImGui form). QDialog::open() (async, WindowModal); the launch consume path
// submits debugger.spawn_attach on the executor with the verbatim completion
// toasts. The Host flow keeps the stacked confirm sub-modal.
class SpawnTargetDialogQt : public bridge::AidaDialog {
    Q_OBJECT
public:
    static void requestOpen(QWidget* parent = nullptr);
    static void requestOpenWithPath(const std::string& executable_path,
                                    QWidget* parent = nullptr);
    static bool isOpen();

private:
    explicit SpawnTargetDialogQt(QWidget* parent = nullptr);

    void reject() override;
    void keyPressEvent(QKeyEvent* event) override;

    void onModeChanged();
    void browseExecutable();
    void browseWorkingDir();
    void browseBridgeDir();
    void updateLaunchEnabled();
    void updateBridgeStatus(int phase, const QString& text);
    void onBridgeFinished(bool activated, bool cancelled, const QString& detail,
                          const QString& guestSample);
    void onLaunch();
    void launchSandbox();
    void launchHostConfirmed();

    QLineEdit* exe_edit_ = nullptr;
    QLineEdit* args_edit_ = nullptr;
    QLineEdit* cwd_edit_ = nullptr;
    QButtonGroup* mode_group_ = nullptr;
    QRadioButton* sandbox_radio_ = nullptr;
    QRadioButton* custom_vm_radio_ = nullptr;
    QRadioButton* host_radio_ = nullptr;
    QGroupBox* sandbox_group_ = nullptr;
    QGroupBox* custom_vm_group_ = nullptr;
    QGroupBox* host_group_ = nullptr;
    QCheckBox* block_network_check_ = nullptr;
    QCheckBox* kill_on_host_exit_check_ = nullptr;
    QSpinBox* memory_cap_spin_ = nullptr;
    QSpinBox* auto_terminate_spin_ = nullptr;
    QLineEdit* bridge_host_edit_ = nullptr;
    QLineEdit* bridge_guest_edit_ = nullptr;
    QLineEdit* guest_sample_edit_ = nullptr;
    QLabel* bridge_status_label_ = nullptr;
    QLabel* guest_command_label_ = nullptr;
    QPushButton* copy_command_button_ = nullptr;
    QPushButton* launch_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QLabel* warning_label_ = nullptr;
    QLabel* warning_title_label_ = nullptr;
    QLabel* last_vm_label_ = nullptr;
    QLabel* exe_path_label_ = nullptr;
    QPushButton* open_vm_folder_button_ = nullptr;
    DebuggerBridgeOpBridge* bridge_op_ = nullptr;
    run_target::capability_probe_t capabilities_;

    static QPointer<SpawnTargetDialogQt> active_;
};

}
