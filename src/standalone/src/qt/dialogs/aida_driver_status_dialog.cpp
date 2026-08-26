#include "qt/dialogs/aida_driver_status_dialog.hpp"

#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>
#include <mutex>

#include "core/infra/executor.hpp"
#include "helpers/diag_log.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::dialogs {

AidaDriverModulesModel::AidaDriverModulesModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int AidaDriverModulesModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(modules_.size());
}

QVariant AidaDriverModulesModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(modules_.size()))
        return {};
    if (role != Qt::DisplayRole)
        return {};
    const auto& module = modules_[static_cast<std::size_t>(index.row())];
    return QStringLiteral("0x%1  %2")
        .arg(module.base, 0, 16)
        .toUpper()
        .arg(QString::fromStdString(module.name));
}

void AidaDriverModulesModel::applySnapshot(
    std::vector<driver_bridge::module_info_t> modules, std::uint32_t pid)
{
    beginResetModel();
    modules_ = std::move(modules);
    pid_ = pid;
    endResetModel();
}

AidaDriverThreadsModel::AidaDriverThreadsModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int AidaDriverThreadsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(threads_.size());
}

QVariant AidaDriverThreadsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(threads_.size()))
        return {};
    if (role != Qt::DisplayRole)
        return {};
    const auto& thread = threads_[static_cast<std::size_t>(index.row())];
    return QStringLiteral("TID %1  Priority %2")
        .arg(thread.tid)
        .arg(thread.priority);
}

void AidaDriverThreadsModel::applySnapshot(
    std::vector<driver_bridge::thread_info_t> threads, std::uint32_t pid)
{
    beginResetModel();
    threads_ = std::move(threads);
    pid_ = pid;
    endResetModel();
}

namespace {

struct modules_cache_t {
    std::mutex mutex;
    std::vector<driver_bridge::module_info_t> modules;
    std::uint32_t pid = 0;
    std::uint64_t applied_ms = 0;
};

struct threads_cache_t {
    std::mutex mutex;
    std::vector<driver_bridge::thread_info_t> threads;
    std::uint32_t pid = 0;
    std::uint64_t applied_ms = 0;
};

modules_cache_t& modules_cache()
{
    static modules_cache_t value;
    return value;
}

threads_cache_t& threads_cache()
{
    static threads_cache_t value;
    return value;
}

std::atomic<bool> g_modules_inflight{false};
std::atomic<bool> g_threads_inflight{false};
std::atomic<bool> g_modules_ready{false};
std::atomic<bool> g_threads_ready{false};
std::atomic<std::uint64_t> g_modules_epoch{0};
std::atomic<std::uint64_t> g_threads_epoch{0};

}

AidaDriverStatusDialog::AidaDriverStatusDialog(QWidget* parent)
    : bridge::AidaDialog(parent)
{
    setObjectName(QStringLiteral("aida.driver_status"));
    setWindowTitle(QStringLiteral("Driver Status"));
    setModal(false);

    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
                             t.panel.padding);
    root->setSpacing(t.spacing.sm);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.driver_status.summary"));
    status_label_->setFont(theme::fonts::strong());
    root->addWidget(status_label_);
    process_label_ = new QLabel(this);
    process_label_->setObjectName(QStringLiteral("aida.driver_status.process"));
    process_label_->setFont(theme::fonts::body());
    root->addWidget(process_label_);
    pid_label_ = new QLabel(this);
    pid_label_->setObjectName(QStringLiteral("aida.driver_status.pid"));
    pid_label_->setFont(theme::fonts::body());
    root->addWidget(pid_label_);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("aida.driver_status.tabs"));
    modules_model_ = new AidaDriverModulesModel(this);
    threads_model_ = new AidaDriverThreadsModel(this);
    modules_view_ = new QListView(tabs_);
    modules_view_->setObjectName(QStringLiteral("aida.driver_status.modules"));
    modules_view_->setModel(modules_model_);
    modules_view_->setUniformItemSizes(true);
    modules_view_->setAlternatingRowColors(true);
    modules_view_->setFont(theme::fonts::codeRegular());
    tabs_->addTab(modules_view_, QStringLiteral("Modules"));
    threads_view_ = new QListView(tabs_);
    threads_view_->setObjectName(QStringLiteral("aida.driver_status.threads"));
    threads_view_->setModel(threads_model_);
    threads_view_->setUniformItemSizes(true);
    threads_view_->setAlternatingRowColors(true);
    threads_view_->setFont(theme::fonts::codeRegular());
    tabs_->addTab(threads_view_, QStringLiteral("Threads"));
    root->addWidget(tabs_, 1);

    auto* buttons = new QHBoxLayout();
    detach_button_ = new QPushButton(QStringLiteral("Detach"), this);
    detach_button_->setObjectName(QStringLiteral("aida.driver_status.detach"));
    connect(detach_button_, &QPushButton::clicked, this, [this] { onDetachReview(); });
    buttons->addWidget(detach_button_);
    buttons->addStretch(1);
    close_button_ = new QPushButton(QStringLiteral("Close"), this);
    close_button_->setObjectName(QStringLiteral("aida.driver_status.close"));
    close_button_->setDefault(true);
    connect(close_button_, &QPushButton::clicked, this, [this] { reject(); });
    buttons->addWidget(close_button_);
    root->addLayout(buttons);

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(2000);
    refresh_timer_->setTimerType(Qt::CoarseTimer);
    connect(refresh_timer_, &QTimer::timeout, this, [this] { tick(); });

    setMinimumSize(360, 280);
    resize(500, 380);
}

AidaDriverStatusDialog::~AidaDriverStatusDialog()
{
    refresh_timer_->stop();
}

void AidaDriverStatusDialog::openFresh()
{
    refreshHeader();
    tick();
    refresh_timer_->start();
    open();
    connect(this, &QDialog::finished, this, [this](int) {
        refresh_timer_->stop();
    }, Qt::SingleShotConnection);
}

void AidaDriverStatusDialog::refreshHeader()
{
    const std::uint32_t pid = driver_bridge::attached_pid();
    const bool attached = pid != 0;
    status_label_->setText(attached ? QStringLiteral("Status: Attached")
                                    : QStringLiteral("Status: Detached"));
    if (!header_state_set_ || attached != header_attached_) {
        header_attached_ = attached;
        header_state_set_ = true;
        status_label_->setProperty("aidaState", attached
            ? QVariant(QStringLiteral("live")) : QVariant());
        status_label_->setProperty("aidaVariant", attached
            ? QVariant() : QVariant(QStringLiteral("secondary")));
        theme::stylesheet::repolish(status_label_);
    }
    process_label_->setText(attached
        ? QStringLiteral("Process: %1").arg(QString::fromStdString(
            driver_bridge::attached_process_name()))
        : QString());
    pid_label_->setText(attached
        ? QStringLiteral("PID: %1").arg(pid)
        : QString());
    tabs_->setVisible(attached);
    detach_button_->setVisible(attached);
}

void AidaDriverStatusDialog::tick()
{
    refreshHeader();
    const std::uint32_t current_pid = driver_bridge::attached_pid();
    if (current_pid == 0)
        return;
    const std::uint64_t now_ms = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() / 1000000);

    bool modules_needed = false;
    {
        std::lock_guard<std::mutex> lock(modules_cache().mutex);
        modules_needed = modules_cache().pid != current_pid ||
            now_ms >= modules_cache().applied_ms + 2000;
    }
    if (modules_needed && !g_modules_inflight.load(std::memory_order_acquire)) {
        if (!g_modules_inflight.exchange(true, std::memory_order_acq_rel)) {
            const std::uint64_t epoch = g_modules_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
            aida::infra::executor::submission_t submission;
            submission.owner_subsystem = "driver_state";
            submission.label = "driver_state.enumerate_modules";
            submission.thread_class = "bounded_task";
            submission.domain = aida::infra::executor::domain_t::feature_worker;
            submission.body = [current_pid, epoch]() {
                std::vector<driver_bridge::module_info_t> fresh;
                try {
                    fresh = driver_bridge::enumerate_modules();
                } catch (...) {
                    fresh.clear();
                }
                {
                    std::lock_guard<std::mutex> lock(modules_cache().mutex);
                    modules_cache().modules = std::move(fresh);
                    modules_cache().pid = current_pid;
                }
                g_modules_ready.store(true, std::memory_order_release);
                g_modules_inflight.store(false, std::memory_order_release);
            };
            const auto submitted = aida::infra::executor::submit(std::move(submission));
            if (!submitted.submitted)
                g_modules_inflight.store(false, std::memory_order_release);
        }
    }
    if (g_modules_ready.exchange(false, std::memory_order_acq_rel)) {
        std::lock_guard<std::mutex> lock(modules_cache().mutex);
        if (modules_cache().pid == current_pid) {
            modules_cache().applied_ms = now_ms;
            modules_model_->applySnapshot(modules_cache().modules, current_pid);
        }
    }

    bool threads_needed = false;
    {
        std::lock_guard<std::mutex> lock(threads_cache().mutex);
        threads_needed = threads_cache().pid != current_pid ||
            now_ms >= threads_cache().applied_ms + 2000;
    }
    if (threads_needed && !g_threads_inflight.load(std::memory_order_acquire)) {
        if (!g_threads_inflight.exchange(true, std::memory_order_acq_rel)) {
            const std::uint64_t epoch = g_threads_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
            aida::infra::executor::submission_t submission;
            submission.owner_subsystem = "driver_state";
            submission.label = "driver_state.enumerate_threads";
            submission.thread_class = "bounded_task";
            submission.domain = aida::infra::executor::domain_t::feature_worker;
            submission.body = [current_pid, epoch]() {
                std::vector<driver_bridge::thread_info_t> fresh;
                try {
                    fresh = driver_bridge::enumerate_threads();
                } catch (...) {
                    fresh.clear();
                }
                {
                    std::lock_guard<std::mutex> lock(threads_cache().mutex);
                    threads_cache().threads = std::move(fresh);
                    threads_cache().pid = current_pid;
                }
                g_threads_ready.store(true, std::memory_order_release);
                g_threads_inflight.store(false, std::memory_order_release);
            };
            const auto submitted = aida::infra::executor::submit(std::move(submission));
            if (!submitted.submitted)
                g_threads_inflight.store(false, std::memory_order_release);
        }
    }
    if (g_threads_ready.exchange(false, std::memory_order_acq_rel)) {
        std::lock_guard<std::mutex> lock(threads_cache().mutex);
        if (threads_cache().pid == current_pid) {
            threads_cache().applied_ms = now_ms;
            threads_model_->applySnapshot(threads_cache().threads, current_pid);
        }
    }
}

void AidaDriverStatusDialog::onDetachReview()
{
    const std::uint32_t reviewed_pid = driver_bridge::attached_pid();
    if (reviewed_pid == 0)
        return;
    const QString reviewed_name = QString::fromStdString(
        driver_bridge::attached_process_name());

    auto* confirm = new bridge::AidaDialog(this);
    confirm->setObjectName(QStringLiteral("aida.driver_status.detach_confirm"));
    confirm->setWindowTitle(QStringLiteral("Detach Process"));
    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(confirm);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
                               t.panel.padding);
    layout->setSpacing(t.spacing.sm);
    auto* verb = new QLabel(QStringLiteral("Detach the active driver-backed session"), confirm);
    verb->setFont(theme::fonts::strong());
    layout->addWidget(verb);
    auto* target = new QLabel(QStringLiteral("Target: %1 (PID %2)")
        .arg(reviewed_name.isEmpty() ? QStringLiteral("Process") : reviewed_name)
        .arg(reviewed_pid), confirm);
    layout->addWidget(target);
    auto* scope_label = new QLabel(QStringLiteral(
        "Scope: Debugger, live-memory, and target-specific inspection state"), confirm);
    scope_label->setFont(theme::fonts::caption());
    scope_label->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(scope_label);
    auto* effect = new QLabel(QStringLiteral(
        "The target process remains running, but live target state becomes unavailable."), confirm);
    effect->setWordWrap(true);
    layout->addWidget(effect);
    auto* reversible = new QLabel(QStringLiteral(
        "Attach again to create a new live session."), confirm);
    reversible->setFont(theme::fonts::caption());
    reversible->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(reversible);
    auto* prerequisite = new QLabel(confirm);
    prerequisite->setWordWrap(true);
    prerequisite->setFont(theme::fonts::caption());
    layout->addWidget(prerequisite);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Cancel, confirm);
    auto* confirm_button = box->addButton(QStringLiteral("Detach"),
        QDialogButtonBox::DestructiveRole);
    confirm_button->setObjectName(QStringLiteral("aida.driver_status.detach_confirm.button"));
    confirm_button->setProperty("aidaVariant", QStringLiteral("destructive"));
    if (auto* cancel = box->button(QDialogButtonBox::Cancel)) {
        cancel->setObjectName(QStringLiteral("aida.driver_status.detach_confirm.cancel"));
        cancel->setDefault(true);
    }
    connect(box, &QDialogButtonBox::rejected, confirm, &QDialog::reject);
    layout->addWidget(box);
    confirm->setMinimumSize(340, 220);

    const bool target_current = reviewed_pid != 0 &&
        driver_bridge::attached_pid() == reviewed_pid;
    prerequisite->setText(target_current
        ? QStringLiteral("The reviewed PID is still the active target.")
        : QStringLiteral("The reviewed target is no longer active. Cancel and review the current target."));
    confirm_button->setEnabled(target_current);
    auto& scope = confirm->add_revalidate_scope({
        [] {
            return QString::number(driver_bridge::attached_pid());
        },
        {}}, QStringLiteral("The reviewed target is no longer active. Cancel and review the current target."));
    Q_UNUSED(scope);
    connect(confirm_button, &QPushButton::clicked, confirm, [reviewed_pid, confirm] {
        if (reviewed_pid == 0 || driver_bridge::attached_pid() != reviewed_pid)
            return;
        driver_bridge::detach();
        confirm->accept();
    });
    connect(confirm, &QDialog::finished, this, [this](int) { refreshHeader(); });
    confirm->setAttribute(Qt::WA_DeleteOnClose, true);
    confirm->open();
}

}
