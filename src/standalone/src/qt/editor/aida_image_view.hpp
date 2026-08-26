#pragma once

#include <QImage>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <string>

class QLabel;
class QTimer;

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::editor {

class AidaImageCanvas : public QWidget {
    Q_OBJECT
public:
    explicit AidaImageCanvas(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void clearImage();
    bool hasImage() const noexcept { return !image_.isNull(); }
    QSize imageSize() const { return image_.size(); }
    qreal zoom() const noexcept { return zoom_; }

    void setFitToView();
    void resetView();

Q_SIGNALS:
    void zoomChanged(qreal zoom);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void easeTick();
    void stepZoom(qreal factor);

    QImage image_;
    qreal zoom_ = 1.0;
    qreal target_zoom_ = 1.0;
    qreal pan_x_ = 0.0;
    qreal pan_y_ = 0.0;
    bool fit_to_view_ = true;
    bool panning_ = false;
    QPoint pan_start_;
    QTimer* ease_timer_ = nullptr;
};

class AidaImageView : public QWidget {
    Q_OBJECT
public:
    explicit AidaImageView(QWidget* parent = nullptr);
    ~AidaImageView() override;

    bool load(const QString& path);
    void clear();
    bool active() const noexcept { return active_.load(std::memory_order_acquire); }
    QString lastError() const;

    static bool isImageAdmission(const QString& extension_lower);

Q_SIGNALS:
    void loadFinished(const QString& path, bool ok);

private:
    void onDecodeResult(quint64 serial, QString path, QImage image, QString error);
    void refreshInfoLabel();
    void syncCanvasState();

    QLabel* name_label_ = nullptr;
    QLabel* info_label_ = nullptr;
    AidaImageCanvas* canvas_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    std::atomic<bool> loading_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> active_{false};
    std::atomic<quint64> load_serial_{0};
    QString path_;
    QString filename_;
    QString error_;
};

}
