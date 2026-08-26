#pragma once

#include <QObject>

#include <atomic>

namespace aida::qt {

class AidaStartupOrchestrator : public QObject
{
    Q_OBJECT
public:
    explicit AidaStartupOrchestrator(QObject* parent = nullptr);
    ~AidaStartupOrchestrator() override;

    void kickoffBackgroundInit();
    void onViewsReady();

    int bgInitStep() const;
    int bgInitTotal() const;
    bool bgInitDone() const;

public Q_SLOTS:
    void onBootFinished();

Q_SIGNALS:
    void backgroundInitFinished();
    void deferredServicesTriggered();

private:
    void queueDeferredServicesTrigger(const char* source);
    void runDeferredServicesTrigger(const char* source);

    std::atomic<bool> kickoff_posted_{false};
    std::atomic<bool> views_ready_{false};
    std::atomic<bool> deferred_triggered_{false};
};

}
