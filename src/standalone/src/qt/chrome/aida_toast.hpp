#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

class QLabel;
class QTimer;
class QToolButton;
class QVariantAnimation;

namespace aida::qt::chrome {

enum class AidaToastType : int {
    info = 0,
    success,
    warning,
    error
};

namespace toast_constants {
inline constexpr std::size_t k_max_visible = 5;
inline constexpr double k_dedup_window_sec = 3.0;
inline constexpr int k_toast_width = 380;
inline constexpr int k_close_box = 20;
inline constexpr int k_swipe_dismiss_px = 100;
inline constexpr double k_swipe_dismiss_pct = 0.30;
inline constexpr double k_hover_timer_scale = 0.18;
inline constexpr int k_swipe_velocity_px_s = 1400;
inline constexpr std::size_t k_pending_cap = k_max_visible * 2;
}

class AidaToastWidget : public QFrame {
    Q_OBJECT
public:
    AidaToastWidget(std::uint64_t id, const QString& message, AidaToastType type,
                    double duration_sec, QWidget* parent = nullptr);

    void setAction(const QString& label, std::function<void()> callback);

    std::uint64_t toastId() const noexcept { return id_; }
    QString message() const noexcept { return message_; }
    double elapsedSec() const noexcept { return elapsed_; }
    bool dismissing() const noexcept { return dismissing_; }
    bool gone() const noexcept { return gone_; }

    void restackTo(int y);
    void tickLifetime(double dt_sec);
    void startDismiss();
    int contentHeightForWidth(int width);

Q_SIGNALS:
    void dismissFinished(std::uint64_t id);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void beginIntro();
    void beginSwipeDismiss();
    void finishDismiss();
    void positionChildren();
    void updateHoverState();

    std::uint64_t id_ = 0;
    QString message_;
    AidaToastType type_ = AidaToastType::info;
    double duration_sec_ = 4.0;
    double elapsed_ = 0.0;
    bool dismissing_ = false;
    bool gone_ = false;
    bool swipe_dismissing_ = false;
    bool press_inside_ = false;
    bool dragging_ = false;
    QPoint press_pos_;
    int swipe_offset_ = 0;
    double opacity_ = 1.0;
    QLabel* message_label_ = nullptr;
    QToolButton* action_button_ = nullptr;
    QToolButton* close_button_ = nullptr;
    QPointer<QVariantAnimation> intro_anim_;
    QPointer<QVariantAnimation> fade_anim_;
    QPointer<QVariantAnimation> y_anim_;
    QPointer<QVariantAnimation> swipe_anim_;
    std::function<void()> action_callback_;
    bool intro_done_ = false;
    bool placed_ = false;
    bool clipped_ = false;
};

class AidaToastHost : public QWidget {
    Q_OBJECT
public:
    explicit AidaToastHost(QWidget* parent = nullptr);
    ~AidaToastHost() override;

    void trackToast(AidaToastWidget* toast);
    void untrackToast(AidaToastWidget* toast);
    void restack();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    std::vector<QPointer<AidaToastWidget>> toasts_;
};

class AidaToastManager : public QObject {
    Q_OBJECT
public:
    static AidaToastManager& instance();

    void attachHost(AidaToastHost* host);
    AidaToastHost* host() const noexcept { return host_; }

    void push(const QString& message, AidaToastType type, double duration_sec = 4.0);
    void pushWithAction(const QString& message, AidaToastType type,
                        const QString& action_label, std::function<void()> callback,
                        double duration_sec = 6.0);

private Q_SLOTS:
    void drainPending();

private:
    struct pending_t {
        QString message;
        AidaToastType type = AidaToastType::info;
        double duration_sec = 4.0;
        QString action_label;
        std::function<void()> callback;
    };

    explicit AidaToastManager(QObject* parent = nullptr);

    bool dedupPendingLocked(const QString& message) const;
    bool dedupVisible(const QString& message) const;
    void spawn(const pending_t& item);
    void onToastGone(std::uint64_t id);

    mutable std::mutex mutex_;
    std::deque<pending_t> pending_;
    std::vector<QPointer<AidaToastWidget>> visible_;
    QPointer<AidaToastHost> host_;
    QTimer* tick_timer_ = nullptr;
    QElapsedTimer tick_clock_;
    std::uint64_t next_id_ = 1;
};

void toast_info(const QString& message, double duration_sec = 4.0);
void toast_success(const QString& message, double duration_sec = 4.0);
void toast_warning(const QString& message, double duration_sec = 6.0);
void toast_error(const QString& message, double duration_sec = 6.0);

}
