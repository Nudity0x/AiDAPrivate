#include "qt/layout/monitor_rehome.hpp"

#include "helpers/diag_log.hpp"

#include <DockManager.h>
#include <FloatingDockContainer.h>

#include <QGuiApplication>
#include <QMainWindow>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <algorithm>

namespace aida::qt::layout {

namespace {

constexpr int k_debounce_ms = 250;
constexpr int k_min_visible_width = 96;
constexpr int k_min_visible_height = 28;

}

MonitorRehomeController::MonitorRehomeController(QMainWindow* window,
                                                 ads::CDockManager* manager, QObject* parent)
    : QObject(parent), window_(window), manager_(manager) {
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(k_debounce_ms);
    connect(debounce_, &QTimer::timeout, this, &MonitorRehomeController::sweep);
    connect(qGuiApp, &QGuiApplication::screenAdded,
            this, [this](QScreen*) { schedule(); });
    connect(qGuiApp, &QGuiApplication::screenRemoved,
            this, [this](QScreen*) { schedule(); });
    if (manager_) {
        connect(manager_, &ads::CDockManager::floatingWidgetCreated,
                this, [this](ads::CFloatingDockContainer* floating) {
                    attach_floating_signals(floating);
                    schedule();
                });
    }
    QTimer::singleShot(0, this, [this] { attach_screen_signals(); attach_window_signals(); });
    for (ads::CFloatingDockContainer* floating : manager_ ? manager_->floatingWidgets()
            : QList<ads::CFloatingDockContainer*>{})
        attach_floating_signals(floating);
}

MonitorRehomeController::~MonitorRehomeController() = default;

void MonitorRehomeController::attach_screen_signals() {
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (QScreen* screen : screens) {
        if (!screen)
            continue;
        const auto existing = std::find_if(screens_.begin(), screens_.end(),
            [screen](const QPointer<QScreen>& known) { return known == screen; });
        if (existing != screens_.end())
            continue;
        screens_.push_back(screen);
        connect(screen, &QScreen::geometryChanged, this, [this](const QRect&) { schedule(); });
        connect(screen, &QScreen::availableGeometryChanged, this, [this](const QRect&) { schedule(); });
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this](qreal) { schedule(); });
        connect(screen, &QObject::destroyed, this, [this, screen] {
            screens_.erase(std::remove_if(screens_.begin(), screens_.end(),
                [screen](const QPointer<QScreen>& known) { return known == screen; }),
                screens_.end());
        });
    }
}

void MonitorRehomeController::attach_window_signals() {
    if (!window_ || window_handle_)
        return;
    QWindow* handle = window_->windowHandle();
    if (!handle)
        return;
    window_handle_ = handle;
    connect(handle, &QWindow::screenChanged, this, [this](QScreen*) { schedule(); });
}

void MonitorRehomeController::attach_floating_signals(ads::CFloatingDockContainer* floating) {
    if (!floating)
        return;
    const auto existing = std::find_if(floating_attached_.begin(), floating_attached_.end(),
        [floating](const QPointer<ads::CFloatingDockContainer>& known) { return known == floating; });
    if (existing != floating_attached_.end())
        return;
    floating_attached_.push_back(floating);
    QPointer<ads::CFloatingDockContainer> guard(floating);
    QTimer::singleShot(0, this, [this, guard] {
        if (!guard)
            return;
        QWindow* handle = guard->windowHandle();
        if (!handle)
            return;
        connect(handle, &QWindow::screenChanged, this, [this](QScreen*) { schedule(); });
    });
    connect(floating, &QObject::destroyed, this, [this, floating] {
        floating_attached_.erase(std::remove_if(floating_attached_.begin(),
            floating_attached_.end(),
            [floating](const QPointer<ads::CFloatingDockContainer>& known) {
                return known == floating; }),
            floating_attached_.end());
    });
}

void MonitorRehomeController::schedule() {
    if (debounce_)
        debounce_->start();
}

void MonitorRehomeController::rehome_now() {
    sweep();
}

void MonitorRehomeController::sweep() {
    attach_screen_signals();
    attach_window_signals();
    if (!manager_)
        return;
    const QList<ads::CFloatingDockContainer*> floating = manager_->floatingWidgets();
    if (floating.isEmpty())
        return;
    std::size_t moved = 0;
    for (ads::CFloatingDockContainer* container : floating) {
        if (!container)
            continue;
        const QRect current = container->frameGeometry();
        if (current.width() <= 0 || current.height() <= 0)
            continue;
        QScreen* screen = QGuiApplication::screenAt(current.center());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (!screen)
            continue;
        const QRect work = screen->availableGeometry();
        if (work.width() <= 0 || work.height() <= 0)
            continue;
        const int visible_width = (std::min)(k_min_visible_width,
            (std::max)(1, work.width()));
        const int visible_height = (std::min)(k_min_visible_height,
            (std::max)(1, work.height()));
        const int clamped_width = (std::min)(current.width(), (std::max)(1, work.width()));
        const int clamped_height = (std::min)(current.height(), (std::max)(1, work.height()));
        const int minimum_x = work.left() - (std::max)(0, clamped_width - visible_width);
        const int maximum_x = work.right() + 1 - (std::min)(visible_width, clamped_width);
        const int minimum_y = work.top();
        const int maximum_y = work.bottom() + 1 - (std::min)(visible_height, clamped_height);
        const int clamped_x = std::clamp(current.x(), minimum_x, (std::max)(minimum_x, maximum_x));
        const int clamped_y = std::clamp(current.y(), minimum_y, (std::max)(minimum_y, maximum_y));
        if (clamped_x == current.x() && clamped_y == current.y() &&
            clamped_width == current.width() && clamped_height == current.height())
            continue;
        container->setGeometry(clamped_x, clamped_y, clamped_width, clamped_height);
        ++moved;
        diag::log_tagged_fmt("workspace_layout",
            "floating_container_rehomed from=%d,%d %dx%d to=%d,%d %dx%d screen=%s",
            current.x(), current.y(), current.width(), current.height(),
            clamped_x, clamped_y, clamped_width, clamped_height,
            screen->name().toUtf8().constData());
    }
    if (moved != 0)
        Q_EMIT rehomed();
}

}
