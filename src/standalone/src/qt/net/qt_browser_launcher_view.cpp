#include "qt/net/qt_browser_launcher_view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "core/infra/executor.hpp"
#include "core/network/burp/camoufox_bridge.hpp"
#include "core/network/cert_generator.hpp"
#include "core/network/mitm_proxy.hpp"
#include "helpers/diag_log.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_pill.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

std::uint64_t nowMs()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

QString formatElapsed(std::uint64_t launchedMs)
{
    if (launchedMs == 0)
        return QStringLiteral("-");
    const std::uint64_t now = nowMs();
    if (now < launchedMs)
        return QStringLiteral("0s");
    const std::uint64_t diff = (now - launchedMs) / 1000ULL;
    if (diff < 60)
        return QStringLiteral("%1s").arg(static_cast<quint64>(diff));
    if (diff < 3600)
        return QStringLiteral("%1m %2s").arg(diff / 60).arg(diff % 60);
    return QStringLiteral("%1h %2m").arg(diff / 3600).arg((diff % 3600) / 60);
}

const char* bridgeStateLabel(aida::burp::camoufox::bridge_state_t state)
{
    switch (state) {
    case aida::burp::camoufox::bridge_state_t::stopped:  return "stopped";
    case aida::burp::camoufox::bridge_state_t::starting: return "starting";
    case aida::burp::camoufox::bridge_state_t::ready:    return "ready";
    case aida::burp::camoufox::bridge_state_t::error:    return "error";
    default: return "unknown";
    }
}

struct browser_stage_store_t {
    QString staged_url;
};

browser_stage_store_t& browserStageStore()
{
    static browser_stage_store_t store;
    return store;
}

QPointer<QtBrowserLauncherView> g_browser_view;

}

QtBrowserProcessModel::QtBrowserProcessModel(QObject* parent)
    : SnapshotTableModel(parent) {}

int QtBrowserProcessModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant QtBrowserProcessModel::cellData(
    const aida::burp::browser::browser_status_t& row, int column, int role) const
{
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Pid:    return QString::number(row.pid);
        case Proxy:  return QStringLiteral(":%1").arg(row.proxy_port);
        case Uptime: return formatElapsed(row.launched_ms);
        case Strategy: return QString::fromLatin1(
            aida::burp::browser::certificate_strategy_name(row.certificate_strategy));
        case Spki:   return row.spki_hash_prefix.empty() ? QStringLiteral("-")
            : QString::fromStdString(row.spki_hash_prefix);
        case Browser: return QString::fromStdString(row.browser_path);
        case Profile: return QString::fromStdString(row.profile_path);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Uptime)
            return row.running ? t.success : t.error;
        return row.running ? t.text_primary : t.text_dim;
    }
    if (role == Qt::ToolTipRole && (column == Browser || column == Profile))
        return QString::fromStdString(column == Browser ? row.browser_path : row.profile_path);
    return {};
}

QVariant QtBrowserProcessModel::headerData(int section, Qt::Orientation orientation,
                                           int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Pid:     return QStringLiteral("PID");
    case Proxy:   return QStringLiteral("Proxy");
    case Uptime:  return QStringLiteral("Uptime");
    case Strategy: return QStringLiteral("Strategy");
    case Spki:    return QStringLiteral("SPKI");
    case Browser: return QStringLiteral("Browser");
    case Profile: return QStringLiteral("Profile");
    case Actions: return QStringLiteral("Actions");
    default: return {};
    }
}

// Per-row Kill affordance painted as a button cell in the Actions column. The
// hit is the view's own clicked(index) signal — with NoEditTriggers the
// delegate editorEvent path is gated off upstream
// (shouldEdit()->(trigger & editTriggers) == 0,
// qabstractitemview.cpp:4380-4398), so the click is handled at the view level.
// setIndexWidget is not used: the table adopts a fresh snapshot every second,
// which would churn child widgets mid-click.
class QtBrowserKillDelegate : public QStyledItemDelegate {
public:
    explicit QtBrowserKillDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const auto* row = static_cast<const QtBrowserProcessModel*>(index.model())->rowAt(
            index.row());
        if (!row || !row->running)
            return;
        const auto& t = theme::tokens();
        painter->save();
        const QRect cell = option.rect.adjusted(4, 3, -4, -3);
        painter->setPen(QPen(t.border_focus, 1));
        painter->setBrush(t.error_soft);
        painter->drawRoundedRect(cell, t.radius.sm, t.radius.sm);
        painter->setPen(t.error);
        painter->drawText(cell, Qt::AlignCenter, QStringLiteral("Kill"));
        painter->restore();
    }
};

QtBrowserLauncherController::QtBrowserLauncherController(QObject* parent)
    : QObject(parent) {}

void QtBrowserLauncherController::setLastLaunchStatus(const QString& status)
{
    last_launch_status_ = status;
    Q_EMIT statusChanged();
}

void QtBrowserLauncherController::launch(
    const aida::burp::browser::browser_launch_config_t& config)
{
    if (launching_.exchange(true, std::memory_order_acq_rel))
        return;
    Q_EMIT launchStateChanged();
    const std::uint64_t generation = ++launch_generation_;
    QPointer<QtBrowserLauncherController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.browser_view";
    submission.label = "browser.launch";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, config, generation]() {
        std::uint32_t pid = 0;
        const bool ok = aida::burp::browser::launch(config, pid);
        QString status;
        if (ok) {
            status = QStringLiteral(
                "Launched Camoufox bridge pid=%1 proxy=127.0.0.1:%2 strategy=%3")
                .arg(pid)
                .arg(config.proxy_port)
                .arg(QLatin1String(aida::burp::browser::certificate_strategy_name(
                    config.certificate_strategy)));
        } else {
            status = QStringLiteral("Launch failed: %1")
                .arg(QString::fromStdString(aida::burp::browser::last_error()));
        }
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(), [guard, generation, status]() {
            auto* self = guard.data();
            if (!self || generation != self->launch_generation_)
                return;
            self->launching_.store(false, std::memory_order_release);
            self->setLastLaunchStatus(status);
            Q_EMIT self->launchStateChanged();
            Q_EMIT self->statusChanged();
        }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        launching_.store(false, std::memory_order_release);
        setLastLaunchStatus(QStringLiteral("Launch failed: the bounded operation queue rejected the request."));
        Q_EMIT launchStateChanged();
    }
}

void QtBrowserLauncherController::killAll()
{
    QPointer<QtBrowserLauncherController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.browser_view";
    submission.label = "browser.kill_all";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard]() {
        aida::burp::browser::kill_all();
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(), [guard]() {
            auto* self = guard.data();
            if (!self)
                return;
            self->setLastLaunchStatus(QStringLiteral("camoufox_stopped"));
            Q_EMIT self->statusChanged();
        }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtBrowserLauncherController::killProcess(std::uint32_t pid)
{
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.browser_view";
    submission.label = "browser.kill";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pid]() {
        aida::burp::browser::kill(pid);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void QtBrowserLauncherController::refreshStatus()
{
    Q_EMIT statusChanged();
}

void QtBrowserLauncherController::pollCertStatus()
{
    const std::uint64_t serial = ++cert_poll_serial_;
    QPointer<QtBrowserLauncherController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.browser_view";
    submission.label = "browser.cert_status";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 4;
    submission.body = [guard, serial]() {
        const bool caReady = cert_generator::is_ready();
        bool caInstalled = false;
        std::string spkiPrefix;
        if (caReady) {
            const auto& ca = cert_generator::get_root_ca();
            caInstalled = cert_generator::is_root_ca_installed(ca);
            spkiPrefix = aida::burp::browser::spki_hash_prefix(
                cert_generator::spki_sha256_base64(ca));
        }
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(),
            [guard, serial, caReady, caInstalled, spkiPrefix]() {
                auto* self = guard.data();
                if (!self || serial != self->cert_poll_serial_)
                    return;
                Q_EMIT self->certStatusChanged(caReady, caInstalled,
                    QString::fromStdString(spkiPrefix));
                Q_EMIT self->statusChanged();
            }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

bool QtBrowserLauncherController::stageCamoufoxUrl(const std::string& url,
                                                   std::string& reason)
{
    if (url.empty() || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) {
        reason = "Camoufox requires an absolute HTTP or HTTPS URL.";
        return false;
    }
    browserStageStore().staged_url = QString::fromStdString(url);
    if (g_browser_view)
        g_browser_view->applyStagedUrl(browserStageStore().staged_url);
    reason.clear();
    return true;
}

QString QtBrowserLauncherController::stagedUrl()
{
    return browserStageStore().staged_url;
}

void QtBrowserLauncherController::clearStagedUrl()
{
    browserStageStore().staged_url.clear();
}

QtBrowserLauncherView::QtBrowserLauncherView(QWidget* parent)
    : NetworkPaneBase(parent)
{
    setObjectName(QStringLiteral("view.network.browser"));
    setRequiresTarget(false);

    const auto& t = theme::tokens();
    controller_ = new QtBrowserLauncherController(this);
    g_browser_view = this;

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QFrame(content);
    header->setObjectName(QStringLiteral("view.network.browser.header"));
    header->setProperty("aidaRole", QStringLiteral("header"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(t.spacing.sm, t.spacing.xs, t.spacing.sm,
        t.spacing.xs);
    auto* headerTitle = new QLabel(QStringLiteral("Camoufox browser launcher"), header);
    headerTitle->setProperty("aidaTone", QStringLiteral("primary"));
    headerLayout->addWidget(headerTitle);
    headerLayout->addStretch(1);
    layout->addWidget(header);

    auto* body = new QWidget(content);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    bodyLayout->setSpacing(t.spacing.sm);

    statusLine_ = new QLabel(body);
    statusLine_->setProperty("aidaTone", QStringLiteral("secondary"));
    bodyLayout->addWidget(statusLine_);

    auto* pillRow = new QHBoxLayout();
    pillRow->setSpacing(t.spacing.xs);
    camoufoxPill_ = new widgets::AidaPill(QStringLiteral("Camoufox only"),
        widgets::AidaSemantic::Success, body);
    pillRow->addWidget(camoufoxPill_);
    webrtcPill_ = new widgets::AidaPill(QStringLiteral("WebRTC blocked"),
        widgets::AidaSemantic::Neutral, body);
    pillRow->addWidget(webrtcPill_);
    nativeUaPill_ = new widgets::AidaPill(QStringLiteral("Native UA"),
        widgets::AidaSemantic::Neutral, body);
    pillRow->addWidget(nativeUaPill_);
    pageVerifiedPill_ = new widgets::AidaPill(QStringLiteral("Page verified"),
        widgets::AidaSemantic::Neutral, body);
    pillRow->addWidget(pageVerifiedPill_);
    privacyPill_ = new widgets::AidaPill(QStringLiteral("Privacy verified"),
        widgets::AidaSemantic::Neutral, body);
    pillRow->addWidget(privacyPill_);
    proxyPill_ = new widgets::AidaPill(QStringLiteral("Proxy active"),
        widgets::AidaSemantic::Neutral, body);
    pillRow->addWidget(proxyPill_);
    caPill_ = new widgets::AidaPill(QStringLiteral("AiDA CA not trusted"),
        widgets::AidaSemantic::Neutral, body);
    pillRow->addWidget(caPill_);
    pillRow->addStretch(1);
    bodyLayout->addLayout(pillRow);

    auto* formGrid = new QVBoxLayout();
    formGrid->setSpacing(t.spacing.xs);
    auto* urlRow = new QHBoxLayout();
    urlRow->setSpacing(t.spacing.xs);
    auto* urlLabel = new QLabel(QStringLiteral("URL:"), body);
    urlLabel->setProperty("aidaTone", QStringLiteral("secondary"));
    urlRow->addWidget(urlLabel);
    urlEdit_ = new QLineEdit(body);
    urlEdit_->setObjectName(QStringLiteral("view.network.browser.url"));
    urlEdit_->setMaxLength(511);
    urlEdit_->setPlaceholderText(QStringLiteral("about:blank"));
    urlEdit_->setText(QStringLiteral("about:blank"));
    urlRow->addWidget(urlEdit_, 1);
    formGrid->addLayout(urlRow);

    auto* profileRow = new QHBoxLayout();
    profileRow->setSpacing(t.spacing.xs);
    auto* profileLabel = new QLabel(QStringLiteral("Profile subdir:"), body);
    profileLabel->setProperty("aidaTone", QStringLiteral("secondary"));
    profileRow->addWidget(profileLabel);
    profileEdit_ = new QLineEdit(body);
    profileEdit_->setObjectName(QStringLiteral("view.network.browser.profile"));
    profileEdit_->setMaxLength(127);
    profileEdit_->setPlaceholderText(QStringLiteral("BurpBrowser"));
    profileEdit_->setText(QStringLiteral("BurpBrowser"));
    profileEdit_->setMaximumWidth(field_width_chars(profileEdit_, 32));
    profileRow->addWidget(profileEdit_);
    profileRow->addStretch(1);
    formGrid->addLayout(profileRow);

    auto* strategyRow = new QHBoxLayout();
    strategyRow->setSpacing(t.spacing.xs);
    strategyCombo_ = new QComboBox(body);
    strategyCombo_->setObjectName(QStringLiteral("view.network.browser.strategy"));
    strategyCombo_->addItem(QStringLiteral("trust_store_only"),
        static_cast<int>(aida::burp::browser::certificate_strategy_t::trust_store_only));
    strategyCombo_->addItem(QStringLiteral("camoufox_spki_allowlist"),
        static_cast<int>(aida::burp::browser::certificate_strategy_t::camoufox_spki_allowlist));
    if (aida::burp::browser::certificate_strategy_debug_only_available()) {
        strategyCombo_->addItem(QStringLiteral("debug unsafe ignore"),
            static_cast<int>(
                aida::burp::browser::certificate_strategy_t::unsafe_ignore_all_for_debug_builds_only));
    }
    strategyCombo_->setCurrentIndex(1);
    strategyCombo_->setMinimumWidth(field_width_chars(strategyCombo_, 26));
    strategyRow->addWidget(strategyCombo_);
    clearProfileCheck_ = new QCheckBox(QStringLiteral("Clear profile first"), body);
    strategyRow->addWidget(clearProfileCheck_);
    strategyRow->addStretch(1);
    formGrid->addLayout(strategyRow);

    debugWarning_ = new QLabel(QStringLiteral("Debug-only certificate bypass is selected."),
        body);
    debugWarning_->setProperty("aidaTone", QStringLiteral("warning"));
    debugWarning_->setVisible(false);
    formGrid->addWidget(debugWarning_);

    auto* proxyRow = new QHBoxLayout();
    proxyRow->setSpacing(t.spacing.xs);
    proxyOverrideCheck_ = new QCheckBox(QStringLiteral("Override proxy port"), body);
    proxyRow->addWidget(proxyOverrideCheck_);
    proxyPortSpin_ = new QSpinBox(body);
    proxyPortSpin_->setObjectName(QStringLiteral("view.network.browser.proxy_port"));
    proxyPortSpin_->setRange(1, 65535);
    proxyPortSpin_->setValue(8443);
    proxyPortSpin_->setEnabled(false);
    proxyRow->addWidget(proxyPortSpin_);
    proxyRow->addStretch(1);
    formGrid->addLayout(proxyRow);
    bodyLayout->addLayout(formGrid);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(t.spacing.sm);
    openButton_ = new widgets::AidaButton(QStringLiteral("Open Camoufox"), body);
    openButton_->setObjectName(QStringLiteral("view.network.browser.open"));
    openButton_->setKind(widgets::AidaButton::Kind::Primary);
    openButton_->setControlSize(widgets::AidaButton::ControlSize::Medium);
    openButton_->setMinimumWidth(field_width_chars(openButton_, 20));
    buttonRow->addWidget(openButton_);
    stopButton_ = new widgets::AidaButton(QStringLiteral("Stop Camoufox"), body);
    stopButton_->setObjectName(QStringLiteral("view.network.browser.stop"));
    stopButton_->setKind(widgets::AidaButton::Kind::Secondary);
    stopButton_->setControlSize(widgets::AidaButton::ControlSize::Medium);
    stopButton_->setMinimumWidth(field_width_chars(stopButton_, 17));
    buttonRow->addWidget(stopButton_);
    buttonRow->addStretch(1);
    bodyLayout->addLayout(buttonRow);

    launchStatusLabel_ = new QLabel(body);
    launchStatusLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    launchStatusLabel_->setWordWrap(true);
    launchStatusLabel_->setVisible(false);
    bodyLayout->addWidget(launchStatusLabel_);

    processModel_ = new QtBrowserProcessModel(body);
    processView_ = new QTableView(body);
    processView_->setObjectName(QStringLiteral("view.network.browser.processes"));
    processView_->setModel(processModel_);
    processView_->verticalHeader()->hide();
    processView_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    processView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    processView_->horizontalHeader()->setStretchLastSection(true);
    processView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    processView_->setSelectionMode(QAbstractItemView::SingleSelection);
    processView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    processView_->setAlternatingRowColors(true);
    processView_->setItemDelegateForColumn(QtBrowserProcessModel::Actions,
        new QtBrowserKillDelegate(processView_));
    connect(processView_, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) {
        if (index.column() != QtBrowserProcessModel::Actions)
            return;
        const auto* status = processModel_->rowAt(index.row());
        if (status && status->running)
            controller_->killProcess(status->pid);
    });
    bodyLayout->addWidget(processView_, 1);
    processesEmpty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No tracked browser processes"),
        QStringLiteral("Launch a Camoufox browser to track it here."), body);
    bodyLayout->addWidget(processesEmpty_);

    layout->addWidget(body, 1);

    statusTimer_ = new QTimer(this);
    statusTimer_->setInterval(1000);
    connect(statusTimer_, &QTimer::timeout, this, [this] {
        refreshStatusWidgets();
        refreshProcessTable();
    });
    certTimer_ = new QTimer(this);
    certTimer_->setInterval(1500);
    connect(certTimer_, &QTimer::timeout, this,
        [this] { controller_->pollCertStatus(); });

    connect(openButton_, &QAbstractButton::clicked, this, &QtBrowserLauncherView::launchNow);
    connect(stopButton_, &QAbstractButton::clicked, this, &QtBrowserLauncherView::stopNow);
    connect(proxyOverrideCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        proxyPortSpin_->setEnabled(checked);
    });
    connect(strategyCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        const auto strategy = static_cast<aida::burp::browser::certificate_strategy_t>(
            strategyCombo_->currentData().toInt());
        debugWarning_->setVisible(
            strategy == aida::burp::browser::certificate_strategy_t::unsafe_ignore_all_for_debug_builds_only);
    });
    connect(controller_, &QtBrowserLauncherController::launchStateChanged, this, [this] {
        openButton_->setEnabled(!controller_->launching());
        openButton_->setText(controller_->launching() ? QStringLiteral("Launching...")
                                                      : QStringLiteral("Open Camoufox"));
    });
    connect(controller_, &QtBrowserLauncherController::statusChanged, this, [this] {
        const QString status = controller_->lastLaunchStatus();
        launchStatusLabel_->setText(status);
        launchStatusLabel_->setVisible(!status.isEmpty());
        refreshStatusWidgets();
    });
    connect(controller_, &QtBrowserLauncherController::certStatusChanged, this,
        &QtBrowserLauncherView::refreshCertPill);

    setContent(content);
    if (!QtBrowserLauncherController::stagedUrl().isEmpty())
        applyStagedUrl(QtBrowserLauncherController::stagedUrl());
    refreshStatusWidgets();
    refreshProcessTable();
}

QtBrowserLauncherView::~QtBrowserLauncherView()
{
    if (g_browser_view == this)
        g_browser_view = nullptr;
}

void QtBrowserLauncherView::applyStagedUrl(const QString& url)
{
    urlEdit_->setText(url);
}

void QtBrowserLauncherView::onPaneShown()
{
    refreshStatusWidgets();
    refreshProcessTable();
    controller_->pollCertStatus();
    statusTimer_->start();
    certTimer_->start();
}

void QtBrowserLauncherView::onPaneHidden()
{
    statusTimer_->stop();
    certTimer_->stop();
}

void QtBrowserLauncherView::refreshCertPill(bool caReady, bool caInstalled,
                                            const QString& spkiPrefix)
{
    ca_ready_ = caReady;
    ca_installed_ = caInstalled;
    spki_prefix_ = spkiPrefix;
    caPill_->setText(caInstalled ? QStringLiteral("AiDA CA trusted")
                                 : QStringLiteral("AiDA CA not trusted"));
    caPill_->setKind(caInstalled ? widgets::AidaSemantic::Success
                                 : widgets::AidaSemantic::Neutral);
    refreshStatusWidgets();
}

void QtBrowserLauncherView::refreshStatusWidgets()
{
    const auto status = aida::burp::camoufox::get_status();
    const bool bridgeReady =
        status.state == aida::burp::camoufox::bridge_state_t::ready &&
        status.child_alive && status.browser_open && status.page_verified &&
        !status.cleanup_pending;
    statusLine_->setText(QStringLiteral("state=%1  pid=%2  open=%3  cleanup=%4  spki=%5")
        .arg(QString::fromLatin1(bridgeReady ? "ready" : bridgeStateLabel(status.state)))
        .arg(status.child_pid)
        .arg(status.browser_open ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(status.cleanup_pending ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(spki_prefix_.isEmpty() ? QStringLiteral("-") : spki_prefix_));

    const bool nativeUa = !status.ua_override &&
        (status.effective_ua_policy.empty() || status.effective_ua_policy == "camoufox_native");
    const auto pillKind = [](bool ok, bool neutralWhenFalse) {
        return ok ? widgets::AidaSemantic::Success
                  : (neutralWhenFalse ? widgets::AidaSemantic::Neutral
                                      : widgets::AidaSemantic::Warning);
    };
    webrtcPill_->setKind(pillKind(
        status.webrtc_blocked ||
            status.state == aida::burp::camoufox::bridge_state_t::stopped, true));
    nativeUaPill_->setKind(pillKind(nativeUa, true));
    pageVerifiedPill_->setKind(pillKind(status.page_verified, true));
    privacyPill_->setKind(pillKind(status.privacy_verified, true));
    proxyPill_->setKind(pillKind(mitm_proxy::is_running(), true));
}

void QtBrowserLauncherView::refreshProcessTable()
{
    auto rows = aida::burp::browser::list_running();
    processModel_->adopt(
        std::make_shared<const QVector<aida::burp::browser::browser_status_t>>(
            rows.begin(), rows.end()),
        processModel_->generation() + 1);
    const bool empty = rows.empty();
    processesEmpty_->setVisible(empty);
    processView_->setVisible(!empty);
}

void QtBrowserLauncherView::launchNow()
{
    aida::burp::browser::browser_launch_config_t config;
    config.initial_url = urlEdit_->text().toStdString();
    config.profile_subdir = profileEdit_->text().toStdString();
    auto strategy = static_cast<aida::burp::browser::certificate_strategy_t>(
        strategyCombo_->currentData().toInt());
    if (strategy ==
            aida::burp::browser::certificate_strategy_t::unsafe_ignore_all_for_debug_builds_only &&
        !aida::burp::browser::certificate_strategy_debug_only_available()) {
        strategy = aida::burp::browser::certificate_strategy_t::camoufox_spki_allowlist;
        const int index = strategyCombo_->findData(static_cast<int>(strategy));
        if (index >= 0)
            strategyCombo_->setCurrentIndex(index);
    }
    config.certificate_strategy = strategy;
    config.clear_profile_first = clearProfileCheck_->isChecked();
    config.proxy_host = "127.0.0.1";
    if (proxyOverrideCheck_->isChecked() && proxyPortSpin_->value() > 0) {
        config.proxy_port = static_cast<std::uint16_t>(proxyPortSpin_->value());
    } else {
        config.proxy_port = mitm_proxy::g_state.config.bind_port;
        if (config.proxy_port == 0)
            config.proxy_port = 8443;
    }
    controller_->launch(config);
}

void QtBrowserLauncherView::stopNow()
{
    controller_->killAll();
}

}
