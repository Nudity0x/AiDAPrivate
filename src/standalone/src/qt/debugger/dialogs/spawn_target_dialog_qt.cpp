#include "qt/debugger/dialogs/spawn_target_dialog_qt.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include "helpers/diag_log.hpp"

#include "core/debugger/spawn_target_dialog.hpp"

#include "qt/bridge/clipboard.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::debugger {

QPointer<SpawnTargetDialogQt> SpawnTargetDialogQt::active_;

bool SpawnTargetDialogQt::isOpen() {
    return active_ != nullptr;
}

void SpawnTargetDialogQt::requestOpen(QWidget* parent) {
    if (active_) {
        active_->raise();
        active_->activateWindow();
        return;
    }
    auto* dialog = new SpawnTargetDialogQt(parent);
    active_ = dialog;
    diag::log_tagged_critical("spawn", "spawn_dialog_open_requested");
    dialog->open();
}

void SpawnTargetDialogQt::requestOpenWithPath(
    const std::string& executable_path, QWidget* parent) {
    requestOpen(parent);
    if (active_ && !executable_path.empty())
        active_->exe_edit_->setText(
            QString::fromStdString(executable_path));
}

DebuggerBridgeOpBridge::DebuggerBridgeOpBridge(QObject* parent)
    : QObject(parent) {
}

bool DebuggerBridgeOpBridge::pending() const {
    return spawn_target_dialog::custom_bridge_pending();
}

void DebuggerBridgeOpBridge::requestCancel() {
    static_cast<void>(spawn_target_dialog::request_custom_bridge_cancel());
    if (pending())
        Q_EMIT phaseChanged(-1, QStringLiteral(
            "Cancellation requested; waiting for the current reversible step."));
}

SpawnTargetDialogQt::SpawnTargetDialogQt(QWidget* parent)
    : AidaDialog(parent) {
    setObjectName(QStringLiteral("aida.spawn_target_dialog"));
    setWindowTitle(QStringLiteral("Malware Lab Run"));
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(520, 360);
    resize(760, 760);

    capabilities_ = run_target::probe_capabilities();

    auto* root = new QVBoxLayout(this);
    auto* title = new QLabel(QStringLiteral("Malware Lab Run"), this);
    title->setObjectName(QStringLiteral("aida.spawn_target.title"));
    title->setFont(theme::fonts::strong());
    root->addWidget(title);
    auto* subtitle = new QLabel(QStringLiteral(
        "Choose Windows Sandbox, Custom VM, or Host every time you launch a "
        "sample."), this);
    subtitle->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(subtitle);

    auto* form = new QFormLayout();
    exe_edit_ = new QLineEdit(this);
    exe_edit_->setObjectName(QStringLiteral("aida.spawn_target.exe"));
    exe_edit_->setPlaceholderText(QStringLiteral("C:\\path\\to\\target.exe"));
    exe_edit_->setMaxLength(1023);
    auto* exe_row = new QWidget(this);
    auto* exe_row_layout = new QHBoxLayout(exe_row);
    exe_row_layout->setContentsMargins(0, 0, 0, 0);
    exe_row_layout->addWidget(exe_edit_, 1);
    auto* exe_browse = new QPushButton(QStringLiteral("Browse..."), exe_row);
    exe_browse->setObjectName(QStringLiteral("aida.spawn_target.exe.browse"));
    connect(exe_browse, &QPushButton::clicked, this,
        &SpawnTargetDialogQt::browseExecutable);
    exe_row_layout->addWidget(exe_browse);
    exe_path_label_ = new QLabel(QStringLiteral("Executable path"), this);
    form->addRow(exe_path_label_, exe_row);

    args_edit_ = new QLineEdit(this);
    args_edit_->setObjectName(QStringLiteral("aida.spawn_target.args"));
    args_edit_->setPlaceholderText(QStringLiteral("--example-arg value"));
    args_edit_->setMaxLength(2047);
    form->addRow(QStringLiteral("Arguments (optional)"), args_edit_);
    root->addLayout(form);

    auto* mode_row = new QWidget(this);
    auto* mode_layout = new QHBoxLayout(mode_row);
    mode_layout->setContentsMargins(0, 0, 0, 0);
    mode_group_ = new QButtonGroup(this);
    sandbox_radio_ = new QRadioButton(QStringLiteral("Windows Sandbox"),
        mode_row);
    custom_vm_radio_ = new QRadioButton(QStringLiteral("Custom VM"), mode_row);
    host_radio_ = new QRadioButton(QStringLiteral("Host"), mode_row);
    sandbox_radio_->setObjectName(QStringLiteral("aida.spawn_target.mode.sandbox"));
    custom_vm_radio_->setObjectName(QStringLiteral("aida.spawn_target.mode.custom_vm"));
    host_radio_->setObjectName(QStringLiteral("aida.spawn_target.mode.host"));
    mode_group_->addButton(sandbox_radio_, 0);
    mode_group_->addButton(custom_vm_radio_, 1);
    mode_group_->addButton(host_radio_, 2);
    sandbox_radio_->setChecked(true);
    mode_layout->addWidget(sandbox_radio_);
    mode_layout->addWidget(custom_vm_radio_);
    mode_layout->addWidget(host_radio_);
    mode_layout->addStretch(1);
    root->addWidget(mode_row);
    connect(mode_group_, &QButtonGroup::idToggled, this,
        [this](int, bool) { onModeChanged(); });

    sandbox_group_ = new QGroupBox(QStringLiteral(
        "First-time setup for Run in VM"), this);
    auto* sandbox_layout = new QVBoxLayout(sandbox_group_);
    auto* sandbox_notes = new QLabel(QStringLiteral(
        "Windows edition: Pro, Enterprise, or Education. Windows Home users "
        "need a full VM such as VMware Workstation Pro or VirtualBox.\n"
        "For QEMU, VirtualBox, or VMware, keep AiDAStandalone.exe on the host. "
        "Do not copy AiDAStandalone.exe into the guest VM.\n"
        "Run only the target sample and your MCP client or guest agent in the "
        "guest VM. Route that client through an authenticated host bridge or "
        "tunnel that terminates at AiDA's localhost MCP endpoint.\n"
        "Do not bind AiDA's MCP endpoint directly to a guest, LAN, or "
        "untrusted adapter. MCP tools can mutate host files, sessions, "
        "debugger state, and process memory.\n"
        "For VMware, VirtualBox, QEMU, Hyper-V, or a manual Windows VM, use "
        "Custom VM mode to activate a shared-folder bridge through "
        "AiDAGuestAgent.\n"
        "BIOS/UEFI: enable Intel VT-x or AMD-V/SVM, then boot back into "
        "Windows.\n"
        "Admin PowerShell: Enable-WindowsOptionalFeature -Online -FeatureName "
        "Containers-DisposableClientVM -All\n"
        "Reboot, reopen AiDA, press Run, select Run in VM, then Open VM."),
        sandbox_group_);
    sandbox_notes->setWordWrap(true);
    sandbox_layout->addWidget(sandbox_notes);
    if (!capabilities_.has_windows_sandbox) {
        auto* unavailable = new QLabel(QStringLiteral(
            "Windows Sandbox is not available on this PC right now."),
            sandbox_group_);
        unavailable->setObjectName(
            QStringLiteral("aida.spawn_target.sandbox_unavailable"));
        unavailable->setProperty("aidaVariant", QStringLiteral("warning"));
        sandbox_layout->addWidget(unavailable);
    }
    auto* docs_row = new QWidget(sandbox_group_);
    auto* docs_layout = new QHBoxLayout(docs_row);
    docs_layout->setContentsMargins(0, 0, 0, 0);
    const auto add_doc = [this, docs_layout](const QString& label,
                                             const wchar_t* url) {
        auto* button = new QPushButton(label, docs_layout->parentWidget());
        button->setObjectName(QStringLiteral("aida.spawn_target.docs.") +
            label);
        connect(button, &QPushButton::clicked, this, [url] {
            spawn_target_dialog::open_url_external(url);
        });
        docs_layout->addWidget(button);
    };
    add_doc(QStringLiteral("Sandbox Docs"),
        L"https://learn.microsoft.com/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-install");
    add_doc(QStringLiteral("WSB Config"),
        L"https://learn.microsoft.com/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-configure-using-wsb-file");
    add_doc(QStringLiteral("Eval ISO"),
        L"https://www.microsoft.com/en-us/evalcenter/download-windows-11-enterprise");
    add_doc(QStringLiteral("VMware"),
        L"https://knowledge.broadcom.com/external/article/344595/downloading-vmware-workstation-pro.html");
    add_doc(QStringLiteral("VirtualBox"),
        L"https://www.virtualbox.org/wiki/Downloads");
    docs_layout->addStretch(1);
    sandbox_layout->addWidget(docs_row);
    root->addWidget(sandbox_group_);

    custom_vm_group_ = new QGroupBox(QStringLiteral(
        "Custom VM bridge for VMware, VirtualBox, QEMU, Hyper-V, or a manually "
        "built Windows VM"), this);
    auto* custom_layout = new QVBoxLayout(custom_vm_group_);
    auto* custom_notes = new QLabel(QStringLiteral(
        "Share one folder between host and guest. AiDA writes requests on the "
        "host; AiDAGuestAgent reads them inside the guest.\n"
        "AiDAStandalone.exe remains on the host. The guest receives only the "
        "selected sample, launch_config.json, and AiDAGuestAgent.exe.\n"
        "Keep the shared folder private to this VM. Do not expose AiDA's "
        "localhost MCP server to the VM network.\n"
        "After activation, copy the guest command below and run it inside the "
        "VM."), custom_vm_group_);
    custom_notes->setWordWrap(true);
    custom_layout->addWidget(custom_notes);
    auto* bridge_form = new QFormLayout();
    bridge_host_edit_ = new QLineEdit(custom_vm_group_);
    bridge_host_edit_->setObjectName(
        QStringLiteral("aida.spawn_target.bridge_host"));
    bridge_host_edit_->setPlaceholderText(
        QStringLiteral("C:\\AiDA-VM-Bridge\\case-001"));
    bridge_host_edit_->setMaxLength(1023);
    auto* bridge_host_row = new QWidget(custom_vm_group_);
    auto* bridge_host_layout = new QHBoxLayout(bridge_host_row);
    bridge_host_layout->setContentsMargins(0, 0, 0, 0);
    bridge_host_layout->addWidget(bridge_host_edit_, 1);
    auto* bridge_browse = new QPushButton(QStringLiteral("Browse"),
        bridge_host_row);
    bridge_browse->setObjectName(
        QStringLiteral("aida.spawn_target.bridge_host.browse"));
    connect(bridge_browse, &QPushButton::clicked, this,
        &SpawnTargetDialogQt::browseBridgeDir);
    bridge_host_layout->addWidget(bridge_browse);
    bridge_form->addRow(QStringLiteral("Host bridge folder"),
        bridge_host_row);
    bridge_guest_edit_ = new QLineEdit(custom_vm_group_);
    bridge_guest_edit_->setObjectName(
        QStringLiteral("aida.spawn_target.bridge_guest"));
    bridge_guest_edit_->setPlaceholderText(
        QStringLiteral("Z:\\AiDA-VM-Bridge\\case-001"));
    bridge_guest_edit_->setMaxLength(1023);
    bridge_form->addRow(QStringLiteral("Guest path to the same shared folder"),
        bridge_guest_edit_);
    guest_sample_edit_ = new QLineEdit(custom_vm_group_);
    guest_sample_edit_->setObjectName(
        QStringLiteral("aida.spawn_target.guest_sample"));
    guest_sample_edit_->setPlaceholderText(
        QStringLiteral("Z:\\AiDA-VM-Bridge\\case-001\\samples\\sample.exe"));
    guest_sample_edit_->setMaxLength(1023);
    bridge_form->addRow(QStringLiteral(
        "Guest sample path (optional; auto-filled when host sample is staged)"),
        guest_sample_edit_);
    custom_layout->addLayout(bridge_form);
    guest_command_label_ = new QLabel(custom_vm_group_);
    guest_command_label_->setWordWrap(true);
    guest_command_label_->setFont(theme::fonts::codeRegular());
    guest_command_label_->setVisible(false);
    custom_layout->addWidget(guest_command_label_);
    auto* command_row = new QWidget(custom_vm_group_);
    auto* command_layout = new QHBoxLayout(command_row);
    command_layout->setContentsMargins(0, 0, 0, 0);
    copy_command_button_ = new QPushButton(QStringLiteral("Copy command"),
        command_row);
    copy_command_button_->setObjectName(
        QStringLiteral("aida.spawn_target.copy_command"));
    copy_command_button_->setVisible(false);
    connect(copy_command_button_, &QPushButton::clicked, this, [this] {
        clipboard::set_text(guest_command_label_->property(
            "aidaGuestCommand").toString());
    });
    command_layout->addWidget(copy_command_button_);
    auto* guide_button = new QPushButton(QStringLiteral("Guide"), command_row);
    guide_button->setObjectName(QStringLiteral("aida.spawn_target.guide"));
    connect(guide_button, &QPushButton::clicked, this, [] {
        spawn_target_dialog::open_custom_vm_guide();
    });
    command_layout->addWidget(guide_button);
    command_layout->addStretch(1);
    custom_layout->addWidget(command_row);
    bridge_status_label_ = new QLabel(custom_vm_group_);
    bridge_status_label_->setWordWrap(true);
    bridge_status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    bridge_status_label_->setVisible(false);
    custom_layout->addWidget(bridge_status_label_);
    root->addWidget(custom_vm_group_);

    host_group_ = new QGroupBox(QStringLiteral("Host mode"), this);
    auto* host_layout = new QVBoxLayout(host_group_);
    auto* host_notes = new QLabel(QStringLiteral(
        "Host mode runs the selected binary on this Windows installation. It "
        "is not a malware or BYOVD containment boundary and can compromise "
        "the host."), host_group_);
    host_notes->setWordWrap(true);
    host_layout->addWidget(host_notes);
    auto* cwd_row = new QWidget(host_group_);
    auto* cwd_layout = new QHBoxLayout(cwd_row);
    cwd_layout->setContentsMargins(0, 0, 0, 0);
    cwd_edit_ = new QLineEdit(cwd_row);
    cwd_edit_->setObjectName(QStringLiteral("aida.spawn_target.cwd"));
    cwd_edit_->setPlaceholderText(
        QStringLiteral("C:\\path\\to\\working-directory"));
    cwd_edit_->setMaxLength(1023);
    cwd_layout->addWidget(cwd_edit_, 1);
    auto* cwd_browse = new QPushButton(QStringLiteral("Browse"), cwd_row);
    cwd_browse->setObjectName(QStringLiteral("aida.spawn_target.cwd.browse"));
    connect(cwd_browse, &QPushButton::clicked, this,
        &SpawnTargetDialogQt::browseWorkingDir);
    cwd_layout->addWidget(cwd_browse);
    auto* cwd_form = new QFormLayout();
    cwd_form->addRow(QStringLiteral("Working directory"), cwd_row);
    host_layout->addLayout(cwd_form);
    root->addWidget(host_group_);

    auto* options_row = new QWidget(this);
    auto* options_layout = new QHBoxLayout(options_row);
    options_layout->setContentsMargins(0, 0, 0, 0);
    block_network_check_ = new QCheckBox(QStringLiteral("Block network access"),
        options_row);
    block_network_check_->setChecked(true);
    kill_on_host_exit_check_ = new QCheckBox(
        QStringLiteral("Kill target on AiDA exit"), options_row);
    kill_on_host_exit_check_->setChecked(true);
    options_layout->addWidget(block_network_check_);
    options_layout->addWidget(kill_on_host_exit_check_);
    options_layout->addStretch(1);
    root->addWidget(options_row);

    auto* limits_form = new QFormLayout();
    memory_cap_spin_ = new QSpinBox(this);
    memory_cap_spin_->setObjectName(QStringLiteral("aida.spawn_target.memcap"));
    memory_cap_spin_->setRange(0, 1024 * 1024);
    memory_cap_spin_->setValue(0);
    limits_form->addRow(QStringLiteral("Memory limit (MB, 0 = no limit)"),
        memory_cap_spin_);
    auto_terminate_spin_ = new QSpinBox(this);
    auto_terminate_spin_->setObjectName(
        QStringLiteral("aida.spawn_target.autoterm"));
    auto_terminate_spin_->setRange(0, 7 * 24 * 3600);
    auto_terminate_spin_->setValue(0);
    limits_form->addRow(QStringLiteral(
        "Auto-terminate after (seconds, 0 = no limit)"), auto_terminate_spin_);
    root->addLayout(limits_form);

    last_vm_label_ = new QLabel(this);
    last_vm_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    last_vm_label_->setVisible(false);
    root->addWidget(last_vm_label_);
    open_vm_folder_button_ = new QPushButton(QStringLiteral("Open folder"),
        this);
    open_vm_folder_button_->setObjectName(
        QStringLiteral("aida.spawn_target.open_vm_folder"));
    open_vm_folder_button_->setVisible(false);
    connect(open_vm_folder_button_, &QPushButton::clicked, this, [] {
        const auto dir = spawn_target_dialog::last_sandbox_dir();
        if (!dir.empty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                QString::fromStdWString(dir)));
    });
    root->addWidget(open_vm_folder_button_);

    warning_title_label_ = new QLabel(this);
    warning_title_label_->setObjectName(
        QStringLiteral("aida.spawn_target.warning_title"));
    root->addWidget(warning_title_label_);
    warning_label_ = new QLabel(this);
    warning_label_->setObjectName(QStringLiteral("aida.spawn_target.warning"));
    warning_label_->setWordWrap(true);
    root->addWidget(warning_label_);

    root->addStretch(1);

    auto* button_row = new QHBoxLayout();
    button_row->addStretch(1);
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_button_->setObjectName(QStringLiteral("aida.spawn_target.cancel"));
    connect(cancel_button_, &QPushButton::clicked, this, [this] {
        if (bridge_op_ && bridge_op_->pending())
            bridge_op_->requestCancel();
        else
            reject();
    });
    button_row->addWidget(cancel_button_);
    launch_button_ = new QPushButton(QStringLiteral("Open VM"), this);
    launch_button_->setObjectName(QStringLiteral("aida.spawn_target.launch"));
    launch_button_->setDefault(true);
    connect(launch_button_, &QPushButton::clicked, this,
        &SpawnTargetDialogQt::onLaunch);
    button_row->addWidget(launch_button_);
    root->addLayout(button_row);

    bridge_op_ = new DebuggerBridgeOpBridge(this);
    connect(bridge_op_, &DebuggerBridgeOpBridge::phaseChanged, this,
        &SpawnTargetDialogQt::updateBridgeStatus);
    connect(bridge_op_, &DebuggerBridgeOpBridge::bridgeFinished, this,
        &SpawnTargetDialogQt::onBridgeFinished);
    connect(bridge_op_, &DebuggerBridgeOpBridge::dispatchFailed, this,
        [this](const QString& detail) {
            bridge_status_label_->setText(detail);
            bridge_status_label_->setVisible(true);
            updateLaunchEnabled();
        });

    const auto sandbox_dir = spawn_target_dialog::last_sandbox_dir();
    if (!sandbox_dir.empty()) {
        last_vm_label_->setText(QStringLiteral("Last VM workspace: %1")
            .arg(QString::fromStdWString(sandbox_dir)));
        last_vm_label_->setVisible(true);
        open_vm_folder_button_->setVisible(true);
    }

    connect(exe_edit_, &QLineEdit::textChanged, this,
        [this] { updateLaunchEnabled(); });
    connect(bridge_host_edit_, &QLineEdit::textChanged, this,
        [this] { updateLaunchEnabled(); });
    connect(bridge_guest_edit_, &QLineEdit::textChanged, this,
        [this] { updateLaunchEnabled(); });

    onModeChanged();
}

void SpawnTargetDialogQt::onModeChanged() {
    const int mode = mode_group_->checkedId();
    sandbox_group_->setVisible(mode == 0);
    custom_vm_group_->setVisible(mode == 1);
    host_group_->setVisible(mode == 2);
    exe_path_label_->setText(mode == 1
        ? QStringLiteral("Host sample path (optional, staged into the bridge)")
        : QStringLiteral("Executable path"));

    QString title;
    QString body;
    const char* warning_variant = mode == 2 ? "error" : "info";
    if (mode == 2) {
        title = QStringLiteral("Host execution warning");
        body = QStringLiteral(
            "Run in Host starts the selected binary on this Windows "
            "installation and may attach AiDA's host driver. Do not use this "
            "for malware, cheat loaders, BYOVD samples, unknown drivers, or "
            "anything you do not fully trust.");
    } else if (mode == 1) {
        title = QStringLiteral("Custom VM bridge");
        body = QStringLiteral(
            "Custom VM activates a shared-folder bridge for a guest-side "
            "AiDAGuestAgent. AiDA never exposes the host MCP listener to the "
            "VM; the guest only sees files in the bridge folder you choose.");
    } else {
        title = QStringLiteral("Interactive VM sandbox");
        body = QStringLiteral(
            "Run in VM copies the sample into a disposable Windows Sandbox "
            "workspace, disables clipboard and device redirection, optionally "
            "disables networking, and starts the sample in the sandbox "
            "window. AiDAStandalone.exe remains on the host; the sandbox "
            "receives only the staged sample and guest bridge.");
    }
    warning_title_label_->setText(title);
    warning_label_->setText(body);
    if (warning_title_label_->property("aidaVariant") != warning_variant) {
        warning_title_label_->setProperty("aidaVariant",
            QString::fromLatin1(warning_variant));
        warning_title_label_->style()->unpolish(warning_title_label_);
        warning_title_label_->style()->polish(warning_title_label_);
    }
    if (warning_label_->property("aidaVariant") != warning_variant) {
        warning_label_->setProperty("aidaVariant",
            QString::fromLatin1(warning_variant));
        warning_label_->style()->unpolish(warning_label_);
        warning_label_->style()->polish(warning_label_);
    }

    launch_button_->setText(mode == 0 ? QStringLiteral("Open VM")
        : mode == 1 ? QStringLiteral("Activate")
            : QStringLiteral("Run Host"));
    updateLaunchEnabled();
}

void SpawnTargetDialogQt::updateLaunchEnabled() {
    const int mode = mode_group_->checkedId();
    const bool exe_empty = exe_edit_->text().trimmed().isEmpty();
    const bool bridge_pending = bridge_op_ && bridge_op_->pending();
    bool disabled = false;
    QString reason;
    if (mode == 0) {
        disabled = exe_empty || !capabilities_.has_windows_sandbox;
        reason = QStringLiteral(
            "Choose an executable and ensure Windows Sandbox is available.");
    } else if (mode == 1) {
        disabled = bridge_host_edit_->text().trimmed().isEmpty() ||
            bridge_guest_edit_->text().trimmed().isEmpty() || bridge_pending;
        reason = bridge_pending ? QStringLiteral(
            "A custom VM bridge operation is already active; cancel it or wait "
            "for its terminal state.") : QStringLiteral(
            "Choose both the host bridge folder and guest shared-folder path.");
    } else {
        disabled = exe_empty;
        reason = QStringLiteral(
            "Choose an executable before reviewing a Host run.");
    }
    launch_button_->setEnabled(!disabled);
    launch_button_->setToolTip(disabled ? reason : QString());
    cancel_button_->setText(bridge_pending
        ? QStringLiteral("Cancel activation") : QStringLiteral("Cancel"));
}

void SpawnTargetDialogQt::browseExecutable() {
    diag::log_tagged_critical("file_dialog",
        "spawn_target.browse_executable invoking getOpenFileName");
    const QString picked = QFileDialog::getOpenFileName(this,
        QStringLiteral("Select target binary (.exe / .dll / .sys)"),
        exe_edit_->text(),
        QStringLiteral(
            "Binary files (*.exe *.dll *.sys *.com *.scr *.efi *.cpl);;"
            "Executable files (*.exe *.com *.scr);;Libraries (*.dll *.cpl);;"
            "Drivers (*.sys *.efi);;All files (*.*)"));
    if (picked.isEmpty())
        return;
    exe_edit_->setText(QDir::toNativeSeparators(picked));
    if (cwd_edit_->text().isEmpty())
        cwd_edit_->setText(QDir::toNativeSeparators(
            QFileInfo(picked).absolutePath()));
    diag::log_tagged_critical_fmt("dialog", "spawn_browse_exe_selected path='%s'",
        picked.toUtf8().constData());
    updateLaunchEnabled();
}

void SpawnTargetDialogQt::browseWorkingDir() {
    const QString picked = QFileDialog::getExistingDirectory(this,
        QStringLiteral("Select working directory"), cwd_edit_->text());
    if (picked.isEmpty())
        return;
    cwd_edit_->setText(QDir::toNativeSeparators(picked));
}

void SpawnTargetDialogQt::browseBridgeDir() {
    const QString picked = QFileDialog::getExistingDirectory(this,
        QStringLiteral("Select host-side custom VM bridge folder"),
        bridge_host_edit_->text());
    if (picked.isEmpty())
        return;
    bridge_host_edit_->setText(QDir::toNativeSeparators(picked));
    updateLaunchEnabled();
}

void SpawnTargetDialogQt::keyPressEvent(QKeyEvent* event) {
    if (event->modifiers().testFlag(Qt::ControlModifier) &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        launch_button_->isEnabled()) {
        onLaunch();
        event->accept();
        return;
    }
    AidaDialog::keyPressEvent(event);
}

void SpawnTargetDialogQt::reject() {
    if (bridge_op_ && bridge_op_->pending())
        return;
    diag::log_tagged_critical("spawn", "spawn_dialog_cancelled");
    AidaDialog::reject();
}

void SpawnTargetDialogQt::updateBridgeStatus(int phase, const QString& text) {
    (void)phase;
    bridge_status_label_->setText(text);
    bridge_status_label_->setVisible(true);
    updateLaunchEnabled();
}

void SpawnTargetDialogQt::onBridgeFinished(bool activated, bool cancelled,
                                           const QString& detail,
                                           const QString& guestSample) {
    (void)cancelled;
    bridge_status_label_->setText(detail);
    bridge_status_label_->setVisible(true);
    if (activated && !guestSample.isEmpty())
        guest_sample_edit_->setText(guestSample);
    if (activated) {
        const auto dir = spawn_target_dialog::last_custom_bridge_dir();
        if (!dir.empty()) {
            last_vm_label_->setText(QStringLiteral("Last VM workspace: %1")
                .arg(QString::fromStdWString(dir)));
            last_vm_label_->setVisible(true);
        }
    }
    updateLaunchEnabled();
}

void SpawnTargetDialogQt::onLaunch() {
    if (!launch_button_->isEnabled())
        return;
    const int mode = mode_group_->checkedId();
    if (mode == 2) {
        auto* confirm = new bridge::AidaDialog(this);
        confirm->setObjectName(
            QStringLiteral("aida.spawn_target.host_confirm"));
        confirm->setWindowTitle(QStringLiteral("Confirm Host Run"));
        confirm->setModal(true);
        confirm->setAttribute(Qt::WA_DeleteOnClose);
        auto* layout = new QVBoxLayout(confirm);
        auto* title = new QLabel(QStringLiteral("Run in Host?"), confirm);
        layout->addWidget(title);
        auto* body = new QLabel(QStringLiteral(
            "This will execute the selected binary on your real desktop, not "
            "inside the VM.\n\nCancel unless the file is trusted. Host mode "
            "can expose your PC, credentials, kernel, files, and drivers to "
            "the sample."), confirm);
        body->setWordWrap(true);
        layout->addWidget(body);
        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, confirm);
        buttons->button(QDialogButtonBox::Ok)->setText(
            QStringLiteral("Run in Host"));
        connect(buttons, &QDialogButtonBox::accepted, confirm, [this, confirm] {
            confirm->accept();
            launchHostConfirmed();
        });
        connect(buttons, &QDialogButtonBox::rejected, confirm,
            &QDialog::reject);
        layout->addWidget(buttons);
        confirm->open();
        return;
    }
    if (mode == 1) {
        spawn_target_dialog::custom_bridge_fields_t fields;
        fields.host_bridge = bridge_host_edit_->text().toStdString();
        fields.guest_bridge = bridge_guest_edit_->text().toStdString();
        fields.executable = exe_edit_->text().toStdString();
        fields.arguments = args_edit_->text().toStdString();
        fields.guest_sample = guest_sample_edit_->text().toStdString();
        spawn_target_dialog::activate_custom_bridge(std::move(fields),
            [bridge = bridge_op_](std::function<void()> fn) {
                return QMetaObject::invokeMethod(bridge, std::move(fn),
                    Qt::QueuedConnection);
            },
            [bridge = bridge_op_](
                const spawn_target_dialog::custom_bridge_result_t& result) {
                Q_EMIT bridge->bridgeFinished(result.activated, result.cancelled,
                    QString::fromStdString(result.status_text),
                    QString::fromStdString(result.guest_sample_result));
            },
            [bridge = bridge_op_](int phase) {
                Q_EMIT bridge->phaseChanged(phase,
                    QString::fromLatin1(
                        spawn_target_dialog::custom_bridge_phase_text(phase)));
            });
        updateLaunchEnabled();
        return;
    }
    launchSandbox();
}

void SpawnTargetDialogQt::launchSandbox() {
    run_target::launch_options_t options;
    options.exe_path = exe_edit_->text().toStdWString();
    options.args = args_edit_->text().toStdWString();
    options.working_dir.clear();
    options.isolation = run_target::isolation_t::windows_sandbox;
    options.block_network = block_network_check_->isChecked();
    options.kill_on_host_exit = kill_on_host_exit_check_->isChecked();
    options.attach_after_resume = false;
    options.memory_cap_mb = memory_cap_spin_->value() > 0
        ? static_cast<std::uint32_t>(memory_cap_spin_->value()) : 0u;
    options.auto_terminate_sec = auto_terminate_spin_->value() > 0
        ? static_cast<std::uint32_t>(auto_terminate_spin_->value()) : 0u;
    options.malware_safe_mode = false;
    options.log_network_traffic = false;
    options.lower_integrity_untrusted = false;
    options.allow_child_processes = true;
    options.force_mitigations_strict = false;
    options.redirect_user_paths_to_sandbox = false;
    options.register_kernel_sandbox_guard = false;
    if (spawn_target_dialog::submit_spawn_launch(std::move(options)))
        accept();
}

void SpawnTargetDialogQt::launchHostConfirmed() {
    run_target::launch_options_t options;
    options.exe_path = exe_edit_->text().toStdWString();
    options.args = args_edit_->text().toStdWString();
    options.working_dir = cwd_edit_->text().toStdWString();
    options.isolation = run_target::isolation_t::same_desktop_jobbed;
    options.block_network = block_network_check_->isChecked();
    options.kill_on_host_exit = kill_on_host_exit_check_->isChecked();
    options.attach_after_resume = true;
    options.memory_cap_mb = memory_cap_spin_->value() > 0
        ? static_cast<std::uint32_t>(memory_cap_spin_->value()) : 0u;
    options.auto_terminate_sec = auto_terminate_spin_->value() > 0
        ? static_cast<std::uint32_t>(auto_terminate_spin_->value()) : 0u;
    options.malware_safe_mode = false;
    options.log_network_traffic = false;
    options.lower_integrity_untrusted = false;
    options.allow_child_processes = true;
    options.force_mitigations_strict = false;
    options.redirect_user_paths_to_sandbox = false;
    options.register_kernel_sandbox_guard = false;
    if (spawn_target_dialog::submit_spawn_launch(std::move(options)))
        accept();
}

}
