#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QApplication>
#include <QEvent>
#include <QObject>
#include <QPaintEvent>
#include <QRect>
#include <QString>
#include <QWidget>

#include "helpers/diag_log.hpp"

namespace aida::qt::chrome {

inline const char* chrome_trace_event_name(QEvent::Type type)
{
    switch (type) {
    case QEvent::Hide: return "Hide";
    case QEvent::Show: return "Show";
    case QEvent::HideToParent: return "HideToParent";
    case QEvent::ShowToParent: return "ShowToParent";
    case QEvent::Resize: return "Resize";
    case QEvent::Move: return "Move";
    case QEvent::ParentChange: return "ParentChange";
    case QEvent::WinIdChange: return "WinIdChange";
    case QEvent::StyleChange: return "StyleChange";
    case QEvent::Polish: return "Polish";
    case QEvent::EnabledChange: return "EnabledChange";
    case QEvent::ZOrderChange: return "ZOrderChange";
    case QEvent::WindowActivate: return "WindowActivate";
    case QEvent::WindowDeactivate: return "WindowDeactivate";
    case QEvent::Close: return "Close";
    case QEvent::LayoutRequest: return "LayoutRequest";
    case QEvent::Paint: return "Paint";
    default: return nullptr;
    }
}

class ChromeVisibilityTracer : public QObject {
public:
    explicit ChromeVisibilityTracer(const QString& tag, bool log_paint, QObject* parent = nullptr)
        : QObject(parent), tag_(tag), log_paint_(log_paint) {}

    static void install(QWidget* widget, const QString& tag, bool log_paint = false)
    {
        if (!widget)
            return;
        widget->installEventFilter(new ChromeVisibilityTracer(tag, log_paint, widget));
        QWidget* window = widget->window();
        diag::log_tagged_fmt("qt_chrome_trace",
            "trace_installed target=%s vis=%d geo=%d,%d %dx%d win=0x%llX paint=%d tid=%lu",
            tag.toUtf8().constData(),
            widget->isVisible() ? 1 : 0,
            widget->geometry().x(), widget->geometry().y(),
            widget->geometry().width(), widget->geometry().height(),
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(window)),
            log_paint ? 1 : 0,
            static_cast<unsigned long>(::GetCurrentThreadId()));
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!watched || !event)
            return false;
        const QEvent::Type type = event->type();
        const char* name = chrome_trace_event_name(type);
        if (!name)
            return false;
        if (type == QEvent::Paint && !log_paint_)
            return false;
        QWidget* w = qobject_cast<QWidget*>(watched);
        const QRect g = w ? w->geometry() : QRect();
        QWidget* parent = w ? w->parentWidget() : nullptr;
        QWidget* window = w ? w->window() : nullptr;
        if (type == QEvent::Paint) {
            auto* pe = static_cast<QPaintEvent*>(event);
            const QRect pr = pe->rect();
            diag::log_tagged_fmt("qt_chrome_trace",
                "trace target=%s ev=Paint vis=%d geo=%d,%d %dx%d cliprect=%d,%d %dx%d updates=%d tid=%lu",
                tag_.toUtf8().constData(),
                (w && w->isVisible()) ? 1 : 0,
                g.x(), g.y(), g.width(), g.height(),
                pr.x(), pr.y(), pr.width(), pr.height(),
                (w && w->updatesEnabled()) ? 1 : 0,
                static_cast<unsigned long>(::GetCurrentThreadId()));
            return false;
        }
        diag::log_tagged_fmt("qt_chrome_trace",
            "trace target=%s ev=%s vis=%d hid=%d geo=%d,%d %dx%d minH=%d maxH=%d parentvis=%d winvis=%d popup=%d tid=%lu",
            tag_.toUtf8().constData(), name,
            (w && w->isVisible()) ? 1 : 0,
            (w && w->isHidden()) ? 1 : 0,
            g.x(), g.y(), g.width(), g.height(),
            w ? w->minimumHeight() : -1,
            w ? w->maximumHeight() : -1,
            (parent && parent->isVisible()) ? 1 : 0,
            (window && window->isVisible()) ? 1 : 0,
            QApplication::activePopupWidget() ? 1 : 0,
            static_cast<unsigned long>(::GetCurrentThreadId()));
        return false;
    }

private:
    QString tag_;
    bool log_paint_ = false;
};

}
