#pragma once

#include <QFrame>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

class QHBoxLayout;
class QLabel;
class QToolButton;

namespace aida::qt::chrome {

class AidaBreadcrumbWidget : public QWidget {
    Q_OBJECT
public:
    explicit AidaBreadcrumbWidget(QWidget* parent = nullptr);

    void setSegments(const QStringList& segments);
    QStringList segments() const noexcept { return segments_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void rebuildLayout();

    QStringList segments_;
    std::vector<QRectF> segment_rects_;
    std::vector<QString> elided_;
    int hovered_ = -1;
};

class AidaTitleRow : public QWidget {
    Q_OBJECT
public:
    explicit AidaTitleRow(QWidget* parent = nullptr);

    void setBreadcrumbSegments(const QStringList& segments);
    void setWorkspacePillText(const QString& name, bool liveTarget);
    void refreshThemeIcon();

Q_SIGNALS:
    void themeToggleRequested();
    void themeMenuRequested(const QPoint& globalPos);

private:
    void refreshLogo();
    void applyPill();
    void reapplyMetrics();

    AidaBreadcrumbWidget* breadcrumb_ = nullptr;
    QLabel* logo_ = nullptr;
    QLabel* wordmark_ = nullptr;
    QFrame* pill_ = nullptr;
    QHBoxLayout* pill_layout_ = nullptr;
    QLabel* pill_dot_ = nullptr;
    QLabel* pill_label_ = nullptr;
    QToolButton* theme_toggle_ = nullptr;
    QString pill_text_;
    int pill_text_max_w_ = 0;
    bool pill_live_target_ = false;
};

QPixmap sunPixmap(qreal size, qreal dpr, const QColor& color);
QPixmap moonPixmap(qreal size, qreal dpr, const QColor& color);

}
