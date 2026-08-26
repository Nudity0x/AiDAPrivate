#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

class QTimer;
class QVariantAnimation;

namespace aida::qt {
class AidaStartupOrchestrator;
}

namespace aida::qt::boot {

class AidaBootScreen : public QWidget {
    Q_OBJECT
public:
    explicit AidaBootScreen(QWidget* parent = nullptr);
    ~AidaBootScreen() override;

    void setProgress(int step, int total);
    void setFade(qreal fade);
    qreal fade() const noexcept { return fade_; }
    void coverParent();

    static const char* phaseLabel(int step);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void startClock();
    void stopClock();

    int step_ = 0;
    int total_ = 6;
    qreal fade_ = 1.0;
    qreal anim_progress_ = 0.0;
    qreal swap_ = 1.0;
    qreal last_tick_sec_ = 0.0;
    QString prev_phase_;
    QString cur_phase_;
    QElapsedTimer clock_;
    QVariantAnimation* ticker_ = nullptr;
    QPointer<QVariantAnimation> swap_anim_;
    QImage background_;
    QImage logo_;
    bool background_ok_ = false;
    bool logo_ok_ = false;
};

class AidaWelcomeScreen : public QWidget {
    Q_OBJECT
public:
    explicit AidaWelcomeScreen(QWidget* parent = nullptr);
    ~AidaWelcomeScreen() override;

    void restart();
    void coverParent();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void startClock();
    void stopClock();

    QElapsedTimer clock_;
    QVariantAnimation* ticker_ = nullptr;
    QImage logo_;
    bool logo_ok_ = false;
};

class AidaBootController : public QObject {
    Q_OBJECT
public:
    enum class State {
        Loading,
        LoadingFade,
        Welcome,
        IdeFade,
        Ready
    };

    explicit AidaBootController(QObject* parent = nullptr);
    ~AidaBootController() override;

    void attach(QWidget* container, AidaBootScreen* boot_screen,
                AidaWelcomeScreen* welcome_screen);
    void setOrchestrator(AidaStartupOrchestrator* orchestrator);

    void begin();
    State state() const noexcept { return state_; }
    bool finished() const noexcept { return finished_; }

Q_SIGNALS:
    void bootFinished();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
    void poll();

private:
    void enterLoadingFade();
    void enterWelcome();
    void enterIdeFade();
    void enterReady();
    void logLoadingWaitIfDue();
    void syncCovers();

    QWidget* container_ = nullptr;
    AidaBootScreen* boot_screen_ = nullptr;
    AidaWelcomeScreen* welcome_screen_ = nullptr;
    QWidget* fade_cover_ = nullptr;
    AidaStartupOrchestrator* orchestrator_ = nullptr;

    State state_ = State::Loading;
    QTimer* poll_timer_ = nullptr;
    QElapsedTimer boot_clock_;
    QElapsedTimer phase_clock_;
    QPointer<QVariantAnimation> fade_anim_;
    bool bg_completed_ = false;
    bool finished_ = false;
    bool wait_logged_once_ = false;
    double last_wait_log_sec_ = 0.0;
};

}
