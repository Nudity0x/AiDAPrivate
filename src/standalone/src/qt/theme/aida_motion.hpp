#pragma once

#include <QEasingCurve>
#include <QVariantAnimation>

namespace aida::qt::theme {

enum class SpringPreset {
    Gentle,
    Balanced,
    Snappy,
    Playful,
    Stiff,
    Wobbly
};

enum class Ease {
    Linear,
    InQuad,
    OutQuad,
    InOutQuad,
    InCubic,
    OutCubic,
    InOutCubic,
    InQuint,
    OutQuint,
    InOutQuint,
    InBack,
    OutBack,
    InOutBack,
    OutElastic,
    OutBounce,
    InOutCirc
};

QEasingCurve easingFor(Ease ease);
QEasingCurve bezierEasing(qreal p1x, qreal p1y, qreal p2x, qreal p2y);

class AidaMotion {
public:
    static void setReducedMotion(bool reduced);
    static bool reducedMotion();
};

class AidaSpringAnimation : public QVariantAnimation {
    Q_OBJECT
public:
    explicit AidaSpringAnimation(QObject* parent = nullptr);

    void setSpring(qreal stiffness, qreal damping);
    void setSpringPreset(SpringPreset preset);
    qreal currentScalar() const;

protected:
    void updateCurrentTime(int currentTime) override;
    void updateState(QAbstractAnimation::State newState, QAbstractAnimation::State oldState) override;

private:
    qreal m_stiffness = 240.0;
    qreal m_damping = 22.0;
    qreal m_x = 0.0;
    qreal m_v = 0.0;
    int m_lastTime = 0;
};

namespace motion {

QVariantAnimation* hover(QObject* parent);
QVariantAnimation* press(QObject* parent);
QVariantAnimation* flash(QObject* parent);
QVariantAnimation* loop(int durationMs, QObject* parent);
AidaSpringAnimation* spring(QObject* parent, SpringPreset preset = SpringPreset::Balanced);

}

}
