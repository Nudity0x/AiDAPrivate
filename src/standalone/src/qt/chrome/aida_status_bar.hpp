#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QStatusBar>
#include <QString>

#include <cstdint>

class QKeyEvent;
class QTimer;

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::chrome {

enum class AidaStatusSegmentId : int {
    Target = 0,
    Location,
    Workspace,
    Debugger,
    Network,
    Mcp,
    Driver,
    Tasks,
    Diagnostics,
    Frame
};

class AidaStatusSegment : public QFrame {
    Q_OBJECT
public:
    AidaStatusSegment(AidaStatusSegmentId id, QWidget* parent = nullptr);

    void setSegmentText(const QString& text, const QString& tooltip, int semantic);
    void setSemantic(int semantic);
    int semantic() const noexcept { return semantic_; }
    void setShowsSeparator(bool shows);

    QSize sizeHint() const override;

Q_SIGNALS:
    void clicked(AidaStatusSegmentId id);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

private:
    AidaStatusSegmentId id_;
    QString text_;
    int semantic_ = 0;
    bool hovered_ = false;
    bool shows_separator_ = true;
    Qt::FocusReason focus_reason_ = Qt::OtherFocusReason;
};

class AidaStatusBar : public QStatusBar {
    Q_OBJECT
public:
    AidaStatusBar(docking::AidaDockHost* host, QWidget* parent = nullptr);
    ~AidaStatusBar() override;

    void showMessage(const QString& message, int timeout_ms = 0);
    void clearMessage();

Q_SIGNALS:
    void segmentActivated(AidaStatusSegmentId id);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void tick();
    void tickDriverWatchdog();
    void buildSegments();
    void applySegmentMetrics();
    void applyNarrowing();
    QString elidedMessage(const QString& message) const;
    int messageBudgetWidth() const;

    docking::AidaDockHost* host_ = nullptr;
    AidaStatusSegment* segments_[10] = {};
    bool wanted_visible_[10] = {};
    bool permanent_[10] = {};
    QTimer* poll_timer_ = nullptr;
    QTimer* watchdog_timer_ = nullptr;
    QTimer* frame_probe_ = nullptr;
    QElapsedTimer* frame_clock_ = nullptr;
    QElapsedTimer message_clock_;
    QString raw_message_;
    int message_timeout_ms_ = 0;
    double frame_ms_ = 0.0;
    bool driver_degraded_ = false;
};

}
