#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>

namespace aida::qt::ai {

class AidaChatInjectBridge : public QObject {
    Q_OBJECT
public:
    static AidaChatInjectBridge& instance();

    void post(const std::string& text);
    void post(const QString& text);
    void requestClearComposer();

    void installBackendHooks();

Q_SIGNALS:
    void appendToComposer(const QString& text);
    void clearComposerRequested();

private:
    explicit AidaChatInjectBridge(QObject* parent = nullptr);

    void scheduleDrain();
    void drainNow();

    std::atomic<bool> drain_pending_{false};
    std::uint64_t observed_clear_seq_ = 0;
};

}
