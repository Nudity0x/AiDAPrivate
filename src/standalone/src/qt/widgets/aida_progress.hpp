#pragma once

#include <QWidget>

class QVariantAnimation;

namespace aida::qt::widgets {

class AidaProgressBar : public QWidget
{
    Q_OBJECT
public:
    explicit AidaProgressBar(QWidget* parent = nullptr);

    void setProgress(qreal progress);
    qreal progress() const { return progress_; }

    void setIndeterminate(bool indeterminate);
    bool isIndeterminate() const { return indeterminate_; }

    void setShimmerEnabled(bool enabled);
    bool isShimmerEnabled() const { return shimmer_enabled_; }

    void setBarHeight(int height);
    int barHeight() const { return bar_height_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updateAnimations();
    void paintDeterminate(QPainter& p, const QRectF& track);
    void paintIndeterminate(QPainter& p, const QRectF& track);

    qreal progress_ = 0.0;
    bool indeterminate_ = false;
    bool shimmer_enabled_ = true;
    int bar_height_;
    QVariantAnimation* shimmer_anim_ = nullptr;
    QVariantAnimation* slide_anim_ = nullptr;
    qreal shimmer_phase_ = 0.0;
    qreal slide_phase_ = 0.5;
};

class AidaProgressRing : public QWidget
{
    Q_OBJECT
public:
    explicit AidaProgressRing(QWidget* parent = nullptr);

    void setProgress(qreal progress);
    qreal progress() const { return progress_; }

    void setIndeterminate(bool indeterminate);
    bool isIndeterminate() const { return indeterminate_; }

    void setThickness(qreal thickness);
    qreal thickness() const { return thickness_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updateAnimations();

    qreal progress_ = 0.0;
    bool indeterminate_ = false;
    qreal thickness_;
    QVariantAnimation* spin_anim_ = nullptr;
    qreal spin_degrees_ = 0.0;
};

}
