#include "qt/qt_shell_application.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QByteArray>
#include <QEvent>

#include <exception>

#include "helpers/diag_log.hpp"
#include "core/diagnostics/crash_snapshot.hpp"

namespace aida::qt {

void qt_message_handler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    static thread_local bool in_handler = false;
    if (in_handler)
        return;
    in_handler = true;
    const QByteArray text = msg.toUtf8();
    const QByteArray file = context.file ? QByteArray(context.file) : QByteArray();
    const QByteArray function = context.function ? QByteArray(context.function) : QByteArray();
    const char* level = "debug";
    switch (type) {
    case QtDebugMsg: level = "debug"; break;
    case QtInfoMsg: level = "info"; break;
    case QtWarningMsg: level = "warning"; break;
    case QtCriticalMsg: level = "critical"; break;
    case QtFatalMsg: level = "fatal"; break;
    }
    if (type == QtCriticalMsg || type == QtFatalMsg) {
        diag::log_tagged_critical_fmt("qt",
            "qt_message level=%s file=%s line=%d function=%s tid=%lu msg=%.900s",
            level,
            file.isEmpty() ? "<none>" : file.constData(),
            context.line,
            function.isEmpty() ? "<none>" : function.constData(),
            static_cast<unsigned long>(::GetCurrentThreadId()),
            text.constData());
    } else {
        diag::log_tagged_fmt("qt",
            "qt_message level=%s file=%s line=%d function=%s tid=%lu msg=%.900s",
            level,
            file.isEmpty() ? "<none>" : file.constData(),
            context.line,
            function.isEmpty() ? "<none>" : function.constData(),
            static_cast<unsigned long>(::GetCurrentThreadId()),
            text.constData());
    }
    in_handler = false;
}

void install_qt_message_handler()
{
    qInstallMessageHandler(qt_message_handler);
    diag::log_tagged_critical_fmt("qt",
        "qt_message_handler_installed tid=%lu",
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

AidaApplication::AidaApplication(int& argc, char** argv)
    : QApplication(argc, argv)
{
}

bool AidaApplication::notify(QObject* receiver, QEvent* event)
{
    try {
        return QApplication::notify(receiver, event);
    } catch (const std::exception& ex) {
        aida::diagnostics::crash::emit_crash_breadcrumb(0xE06D7363u, nullptr, "aida_application_notify");
        diag::log_tagged_critical_fmt("qt_app",
            "notify_exception what=%.180s tid=%lu",
            ex.what(),
            static_cast<unsigned long>(::GetCurrentThreadId()));
        throw;
    } catch (...) {
        aida::diagnostics::crash::emit_crash_breadcrumb(0xE06D7363u, nullptr, "aida_application_notify");
        diag::log_tagged_critical_fmt("qt_app",
            "notify_unknown_exception tid=%lu",
            static_cast<unsigned long>(::GetCurrentThreadId()));
        throw;
    }
}

}
