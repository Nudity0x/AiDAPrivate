#pragma once

#include <QObject>

#include <utility>

namespace aida::qt::analysis {

// Worker -> GUI delivery primitive for the analysis domain. Queued delivery packs
// a QMetaCallEvent and deep-copies captured arguments (qmetaobject.cpp:1642-1657);
// if the receiver is destroyed first the event is dropped (qobject.cpp:201-202).
template <typename Fn>
bool gui_post(QObject* context, Fn&& fn)
{
    if (!context)
        return false;
    return QMetaObject::invokeMethod(context, std::forward<Fn>(fn),
        Qt::QueuedConnection);
}

}
