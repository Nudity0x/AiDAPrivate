#include "qt/qt_single_instance.hpp"

#include "qt/qt_main_window.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QLocalServer>
#include <QLocalSocket>

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "helpers/diag_log.hpp"

namespace aida::qt {

namespace {

constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\AiDAStandalone_8E9F73D8_SingleInstance";
constexpr char kAumidUtf8[] = "AiDA.Standalone.IDE";
constexpr char kRaisePayload[] = "raise";
constexpr int kRaiseConnectTimeoutMs = 500;
constexpr int kRaiseWriteTimeoutMs = 500;

HANDLE& single_instance_mutex_handle()
{
    static HANDLE h = nullptr;
    return h;
}

uint64_t fnv1a64(const char* data, std::size_t len)
{
    uint64_t hash = 14695981039346656037ULL;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(data[i]));
        hash *= 1099511628211ULL;
    }
    return hash;
}

void log_startup(const char* fmt, ...)
{
    char buf[2048] = {};
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    diag::log_tagged_critical("startup", buf);
}

bool raise_duplicate_instance()
{
    const std::string server_name = single_instance_server_name();
    const uint64_t started = static_cast<uint64_t>(::GetTickCount64());
    QLocalSocket socket;
    socket.connectToServer(QString::fromStdString(server_name));
    if (!socket.waitForConnected(kRaiseConnectTimeoutMs)) {
        log_startup("single_instance_raise_connect_failed pid=%lu tid=%lu error=%d elapsed_ms=%llu",
            GetCurrentProcessId(),
            GetCurrentThreadId(),
            static_cast<int>(socket.error()),
            static_cast<unsigned long long>(static_cast<uint64_t>(::GetTickCount64()) - started));
        return false;
    }
    QByteArray frame;
    const quint32 payload_len = static_cast<quint32>(sizeof(kRaisePayload) - 1);
    frame.append(static_cast<char>((payload_len >> 24) & 0xFF));
    frame.append(static_cast<char>((payload_len >> 16) & 0xFF));
    frame.append(static_cast<char>((payload_len >> 8) & 0xFF));
    frame.append(static_cast<char>(payload_len & 0xFF));
    frame.append(kRaisePayload, static_cast<int>(payload_len));
    const qint64 written = socket.write(frame);
    const bool flushed = written == frame.size() && socket.waitForBytesWritten(kRaiseWriteTimeoutMs);
    socket.disconnectFromServer();
    log_startup("single_instance_raise_sent pid=%lu tid=%lu written=%lld flushed=%d elapsed_ms=%llu",
        GetCurrentProcessId(),
        GetCurrentThreadId(),
        static_cast<long long>(written),
        flushed ? 1 : 0,
        static_cast<unsigned long long>(static_cast<uint64_t>(::GetTickCount64()) - started));
    return flushed;
}

}

std::string single_instance_server_name()
{
    const uint64_t hash = fnv1a64(kAumidUtf8, sizeof(kAumidUtf8) - 1);
    char suffix[32] = {};
    _snprintf_s(suffix, sizeof(suffix), _TRUNCATE, "%016llX", static_cast<unsigned long long>(hash));
    std::string name = "AiDAStandalone.raise.";
    name += suffix;
    return name;
}

AidaSingleInstance::AidaSingleInstance(QObject* parent)
    : QObject(parent)
{
}

AidaSingleInstance::AcquireResult AidaSingleInstance::acquire_process_gate()
{
    HANDLE h = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    DWORD gle = GetLastError();
    if (!h) {
        log_startup("single_instance_mutex_create_failed gle=%lu pid=%lu tid=%lu",
            gle, GetCurrentProcessId(), GetCurrentThreadId());
        diag::log_tagged_fmt("main", "single_instance_mutex_create_failed gle=%lu", gle);
        return AcquireResult::mutex_failed;
    }
    if (gle == ERROR_ALREADY_EXISTS) {
        log_startup("single_instance_duplicate_exit pid=%lu tid=%lu mutex=0x%llX",
            GetCurrentProcessId(),
            GetCurrentThreadId(),
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(h)));
        const bool notified = raise_duplicate_instance();
        CloseHandle(h);
        diag::log_tagged("main", "single_instance_duplicate_exit");
        return notified ? AcquireResult::duplicate_notified : AcquireResult::duplicate_notify_failed;
    }
    single_instance_mutex_handle() = h;
    log_startup("single_instance_acquired pid=%lu tid=%lu mutex=0x%llX",
        GetCurrentProcessId(),
        GetCurrentThreadId(),
        static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(h)));
    return AcquireResult::primary;
}

void AidaSingleInstance::release_process_gate()
{
    HANDLE& h = single_instance_mutex_handle();
    if (!h) return;
    ReleaseMutex(h);
    CloseHandle(h);
    diag::log_tagged_critical_fmt("main", "single_instance_released pid=%lu tid=%lu",
        GetCurrentProcessId(), GetCurrentThreadId());
    h = nullptr;
}

bool AidaSingleInstance::startServer(AidaMainWindow* window)
{
    if (server_) {
        diag::log_tagged_critical("qt_shell", "single_instance_server_already_started");
        return true;
    }
    window_ = window;
    const std::string name = single_instance_server_name();
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    const bool listening = server_->listen(QString::fromStdString(name));
    if (!listening) {
        diag::log_tagged_critical_fmt("qt_shell",
            "single_instance_server_listen_failed name=%s error=%d tid=%lu",
            name.c_str(),
            static_cast<int>(server_->serverError()),
            static_cast<unsigned long>(::GetCurrentThreadId()));
        delete server_;
        server_ = nullptr;
        return false;
    }
    connect(server_, &QLocalServer::newConnection, this, &AidaSingleInstance::onNewConnection);
    diag::log_tagged_critical_fmt("qt_shell",
        "single_instance_server_listening name=%s user_access=1 tid=%lu",
        name.c_str(),
        static_cast<unsigned long>(::GetCurrentThreadId()));
    return true;
}

void AidaSingleInstance::onNewConnection()
{
    if (!server_)
        return;
    while (QLocalSocket* connection = server_->nextPendingConnection()) {
        rx_buffers_.insert(connection, QByteArray());
        connect(connection, &QLocalSocket::readyRead, this, [this, connection]() {
            onReadyRead(connection);
        });
        connect(connection, &QLocalSocket::disconnected, this, [this, connection]() {
            rx_buffers_.remove(connection);
            connection->deleteLater();
        });
        if (connection->bytesAvailable() > 0)
            onReadyRead(connection);
    }
}

void AidaSingleInstance::onReadyRead(QLocalSocket* connection)
{
    QByteArray& buffer = rx_buffers_[connection];
    buffer.append(connection->readAll());
    if (buffer.size() < 4)
        return;
    const auto* raw = reinterpret_cast<const unsigned char*>(buffer.constData());
    const quint32 payload_len = (static_cast<quint32>(raw[0]) << 24) |
        (static_cast<quint32>(raw[1]) << 16) |
        (static_cast<quint32>(raw[2]) << 8) |
        static_cast<quint32>(raw[3]);
    if (payload_len > 4096 || buffer.size() < static_cast<qint64>(4 + payload_len))
        return;
    const QByteArray payload = buffer.mid(4, static_cast<int>(payload_len));
    if (payload == QByteArray(kRaisePayload, static_cast<int>(sizeof(kRaisePayload) - 1))) {
        diag::log_tagged_critical_fmt("qt_shell",
            "single_instance_raise_received pid=%lu tid=%lu",
            GetCurrentProcessId(),
            static_cast<unsigned long>(::GetCurrentThreadId()));
        if (window_) {
            window_->showNormal();
            window_->raise();
            window_->activateWindow();
        }
    } else {
        diag::log_tagged_fmt("qt_shell",
            "single_instance_unknown_payload len=%u tid=%lu",
            static_cast<unsigned>(payload_len),
            static_cast<unsigned long>(::GetCurrentThreadId()));
    }
    connection->disconnectFromServer();
}

}
