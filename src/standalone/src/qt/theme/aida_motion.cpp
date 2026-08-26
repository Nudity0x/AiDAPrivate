#include "aida_motion.hpp"

#include <QPointF>
#include <QVariant>

#include <atomic>
#include <cmath>

#include "aida_tokens.hpp"

namespace aida::qt::theme {

namespace {

std::atomic<bool> s_reduced_motion{ false };

struct spring_constants_t {
    qreal stiffness;
    qreal damping;
};

spring_constants_t springConstants(SpringPreset preset)
{
    switch (preset) {
    case SpringPreset::Gentle:  return { 170.0, 22.0 };
    case SpringPreset::Balanced: return { 180.0, 14.0 };
    case SpringPreset::Snappy:  return { 240.0, 22.0 };
    case SpringPreset::Playful: return { 220.0, 11.0 };
    case SpringPreset::Stiff:   return { 320.0, 30.0 };
    case SpringPreset::Wobbly:  return { 180.0, 8.0 };
    }
    return { 180.0, 14.0 };
}

}

QEasingCurve easingFor(Ease ease)
{
    switch (ease) {
    case Ease::Linear:    return QEasingCurve(QEasingCurve::Linear);
    case Ease::InQuad:    return QEasingCurve(QEasingCurve::InQuad);
    case Ease::OutQuad:   return QEasingCurve(QEasingCurve::OutQuad);
    case Ease::InOutQuad: return QEasingCurve(QEasingCurve::InOutQuad);
    case Ease::InCubic:   return QEasingCurve(QEasingCurve::InCubic);
    case Ease::OutCubic:  return QEasingCurve(QEasingCurve::OutCubic);
    case Ease::InOutCubic: return QEasingCurve(QEasingCurve::InOutCubic);
    case Ease::InQuint:   return QEasingCurve(QEasingCurve::InQuint);
    case Ease::OutQuint:  return QEasingCurve(QEasingCurve::OutQuint);
    case Ease::InOutQuint: return QEasingCurve(QEasingCurve::InOutQuint);
    case Ease::InBack: {
        QEasingCurve curve(QEasingCurve::InBack);
        curve.setOvershoot(1.70158);
        return curve;
    }
    case Ease::OutBack: {
        QEasingCurve curve(QEasingCurve::OutBack);
        curve.setOvershoot(1.70158);
        return curve;
    }
    case Ease::InOutBack: {
        QEasingCurve curve(QEasingCurve::InOutBack);
        curve.setOvershoot(1.70158);
        return curve;
    }
    case Ease::OutElastic: return QEasingCurve(QEasingCurve::OutElastic);
    case Ease::OutBounce: return QEasingCurve(QEasingCurve::OutBounce);
    case Ease::InOutCirc: return QEasingCurve(QEasingCurve::InOutCirc);
    }
    return QEasingCurve(QEasingCurve::Linear);
}

QEasingCurve bezierEasing(qreal p1x, qreal p1y, qreal p2x, qreal p2y)
{
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment(QPointF(p1x, p1y), QPointF(p2x, p2y), QPointF(1.0, 1.0));
    return curve;
}

void AidaMotion::setReducedMotion(bool reduced)
{
    s_reduced_motion.store(reduced, std::memory_order_release);
}

bool AidaMotion::reducedMotion()
{
    return s_reduced_motion.load(std::memory_order_acquire);
}

AidaSpringAnimation::AidaSpringAnimation(QObject* parent)
    : QVariantAnimation(parent)
{
    setStartValue(0.0);
    setEndValue(1.0);
    setDuration(tokens().motion.spring_max);
}

void AidaSpringAnimation::setSpring(qreal stiffness, qreal damping)
{
    m_stiffness = stiffness;
    m_damping = damping;
}

void AidaSpringAnimation::setSpringPreset(SpringPreset preset)
{
    const spring_constants_t constants = springConstants(preset);
    setSpring(constants.stiffness, constants.damping);
}

qreal AidaSpringAnimation::currentScalar() const
{
    return m_x;
}

void AidaSpringAnimation::updateState(QAbstractAnimation::State newState, QAbstractAnimation::State oldState)
{
    QVariantAnimation::updateState(newState, oldState);
    if (newState == QAbstractAnimation::Running && oldState == QAbstractAnimation::Stopped) {
        m_x = startValue().toReal();
        m_v = 0.0;
        m_lastTime = 0;
    }
}

void AidaSpringAnimation::updateCurrentTime(int currentTime)
{
    const qreal target = endValue().toReal();

    if (AidaMotion::reducedMotion()) {
        m_x = target;
        m_v = 0.0;
        const QVariant endValueVariant = QVariant::fromValue(target);
        updateCurrentValue(endValueVariant);
        Q_EMIT valueChanged(endValueVariant);
        stop();
        return;
    }

    qreal dt = static_cast<qreal>(currentTime - m_lastTime) / 1000.0;
    m_lastTime = currentTime;
    if (dt < 0.0)
        dt = 0.0;
    if (dt > 0.04)
        dt = 0.04;

    const qreal force = -m_stiffness * (m_x - target) - m_damping * m_v;
    m_v += force * dt;
    m_x += m_v * dt;

    const QVariant value = QVariant::fromValue(m_x);
    updateCurrentValue(value);
    Q_EMIT valueChanged(value);

    if (std::fabs(m_x - target) < 0.001 && std::fabs(m_v) < 0.001) {
        m_x = target;
        m_v = 0.0;
        const QVariant endValueVariant = QVariant::fromValue(target);
        updateCurrentValue(endValueVariant);
        Q_EMIT valueChanged(endValueVariant);
        stop();
        return;
    }
}

namespace motion {

QVariantAnimation* hover(QObject* parent)
{
    QVariantAnimation* animation = new QVariantAnimation(parent);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    animation->setEasingCurve(easingFor(Ease::OutCubic));
    animation->setDuration(AidaMotion::reducedMotion() ? 0 : tokens().motion.fast);
    return animation;
}

QVariantAnimation* press(QObject* parent)
{
    QVariantAnimation* animation = new QVariantAnimation(parent);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    animation->setEasingCurve(easingFor(Ease::OutCubic));
    animation->setDuration(AidaMotion::reducedMotion() ? 0 : tokens().motion.instant);
    return animation;
}

QVariantAnimation* flash(QObject* parent)
{
    QVariantAnimation* animation = new QVariantAnimation(parent);
    animation->setStartValue(1.0);
    animation->setEndValue(0.0);
    animation->setEasingCurve(easingFor(Ease::Linear));
    animation->setDuration(AidaMotion::reducedMotion() ? 0 : tokens().motion.lg);
    return animation;
}

QVariantAnimation* loop(int durationMs, QObject* parent)
{
    QVariantAnimation* animation = new QVariantAnimation(parent);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    animation->setEasingCurve(easingFor(Ease::Linear));
    if (AidaMotion::reducedMotion()) {
        animation->setDuration(0);
        animation->setLoopCount(1);
    } else {
        animation->setDuration(durationMs);
        animation->setLoopCount(-1);
    }
    return animation;
}

AidaSpringAnimation* spring(QObject* parent, SpringPreset preset)
{
    AidaSpringAnimation* animation = new AidaSpringAnimation(parent);
    animation->setSpringPreset(preset);
    return animation;
}

}

}
