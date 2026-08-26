#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>

#include <string>

class QLocalServer;
class QLocalSocket;

namespace aida::qt {

class AidaMainWindow;

class AidaSingleInstance : public QObject
{
    Q_OBJECT
public:
    enum class AcquireResult {
        primary,
        duplicate_notified,
        duplicate_notify_failed,
        mutex_failed,
    };

    static AcquireResult acquire_process_gate();
    static void release_process_gate();

    explicit AidaSingleInstance(QObject* parent = nullptr);

    bool startServer(AidaMainWindow* window);

private Q_SLOTS:
    void onNewConnection();

private:
    void onReadyRead(QLocalSocket* connection);

    QLocalServer* server_ = nullptr;
    AidaMainWindow* window_ = nullptr;
    QHash<QLocalSocket*, QByteArray> rx_buffers_;
};

std::string single_instance_server_name();

}
