#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QWidget>

#include <functional>

#include "qt/overlays/aida_loading_bridge.hpp"

class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaProgressBar;
}

class QLabel;

namespace aida::qt::overlays {

class AidaLoadingOverlay : public QWidget {
    Q_OBJECT
public:
    explicit AidaLoadingOverlay(QWidget* parent = nullptr);

    void showProgress(const QString& title, const QString& label, const QString& target,
                      const QString& stage, qreal progress, double elapsed_seconds,
                      bool cancellable, bool failed, qreal alpha);

Q_SIGNALS:
    void cancelRequested();
    void detailsRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void layoutCard();

    QWidget* card_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* target_label_ = nullptr;
    QLabel* elapsed_label_ = nullptr;
    widgets::AidaProgressBar* progress_ = nullptr;
    widgets::AidaButton* cancel_button_ = nullptr;
    widgets::AidaButton* details_button_ = nullptr;
    qreal alpha_ = 0.0;
};

class AidaLoadingOverlayController : public QObject {
    Q_OBJECT
public:
    explicit AidaLoadingOverlayController(QObject* parent = nullptr);
    ~AidaLoadingOverlayController() override;

    void bind(class AidaOverlayHost* host);
    void setViewFocusHook(std::function<void(const char* view_id)> hook);

    bool overlayVisible() const noexcept { return overlay_visible_; }

Q_SIGNALS:
    void overlayShown();
    void overlayHidden();

private:
    void tick();
    void present();
    void dismiss();
    void onCancel();
    void onDetails();

    AidaOverlayHost* host_ = nullptr;
    AidaLoadingOverlay* overlay_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    QElapsedTimer tick_clock_;
    std::function<void(const char* view_id)> view_focus_hook_;
    bool overlay_visible_ = false;
};

}
