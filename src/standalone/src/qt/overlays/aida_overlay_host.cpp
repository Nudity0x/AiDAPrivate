#include "qt/overlays/aida_overlay_host.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QResizeEvent>

#include <cstdint>

#include "helpers/diag_log.hpp"

namespace aida::qt::overlays {

AidaOverlayHost::AidaOverlayHost(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.overlay_host"));
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    hide();
    if (parent) {
        parent->installEventFilter(this);
        setGeometry(parent->rect());
        if (parent->window())
            filter_target_ = parent->window();
    }
}

AidaOverlayHost::~AidaOverlayHost()
{
    removeFilter();
    if (parentWidget())
        parentWidget()->removeEventFilter(this);
}

void AidaOverlayHost::present(QWidget* content)
{
    if (!content)
        return;
    if (content_) {
        content_->hide();
        content_->deleteLater();
    }
    focus_before_ = QApplication::focusWidget();
    if (focus_before_ == this)
        focus_before_ = nullptr;
    content_ = content;
    content_->setParent(this);
    content_->setGeometry(rect());
    content_->show();
    active_ = true;
    syncGeometry();
    show();
    raise();
    setFocus(Qt::OtherFocusReason);
    installFilter();
    diag::log_tagged_fmt("qt_overlay", "overlay_present content=0x%llX tid=%lu",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(content)),
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

void AidaOverlayHost::dismiss()
{
    if (!active_)
        return;
    active_ = false;
    removeFilter();
    if (content_) {
        content_->hide();
        content_->deleteLater();
        content_ = nullptr;
    }
    hide();
    if (focus_before_ && focus_before_->isVisible() && focus_before_->isEnabled()) {
        focus_before_->setFocus(Qt::OtherFocusReason);
        focus_before_ = nullptr;
    }
    Q_EMIT dismissed();
}

void AidaOverlayHost::setCancelable(bool cancelable)
{
    cancelable_ = cancelable;
}

void AidaOverlayHost::syncGeometry()
{
    if (parentWidget())
        setGeometry(parentWidget()->rect());
    if (content_)
        content_->setGeometry(rect());
}

void AidaOverlayHost::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    syncGeometry();
}

void AidaOverlayHost::installFilter()
{
    if (!filter_target_ && parentWidget())
        filter_target_ = parentWidget()->window();
    if (filter_target_)
        filter_target_->installEventFilter(this);
}

void AidaOverlayHost::removeFilter()
{
    if (filter_target_)
        filter_target_->removeEventFilter(this);
}

bool AidaOverlayHost::eventFilter(QObject* watched, QEvent* event)
{
    if (!event)
        return QWidget::eventFilter(watched, event);
    const QEvent::Type type = event->type();
    if (watched == parentWidget() && type == QEvent::Resize) {
        syncGeometry();
        return QWidget::eventFilter(watched, event);
    }
    if (active_ && watched == filter_target_ &&
        (type == QEvent::ShortcutOverride || type == QEvent::Shortcut))
        return true;
    return QWidget::eventFilter(watched, event);
}

void AidaOverlayHost::keyPressEvent(QKeyEvent* event)
{
    if (cancelable_ && event->key() == Qt::Key_Escape) {
        event->accept();
        Q_EMIT cancelRequested();
        return;
    }
    event->accept();
}

void AidaOverlayHost::keyReleaseEvent(QKeyEvent* event)
{
    event->accept();
}

}
