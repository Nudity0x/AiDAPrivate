#include "aida_progress.hpp"
#include <algorithm>

#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

#include "../theme/aida_motion.hpp"
#include "aida_paint_utils.hpp"

#include <cmath>

namespace aida::qt::widgets {

namespace {
    constexpr qreal kPi = 3.14159265358979323846;
    constexpr int kShimmerPeriodMs = 400;
    constexpr int kSlidePeriodMs = 1200;
    constexpr int kSpinPeriodMs = 1571;
    constexpr qreal kIndeterminateBandFraction = 0.30;
}

AidaProgressBar::AidaProgressBar(QWidget* parent)
    : QWidget(parent)
    , bar_height_(aida::qt::theme::tokens().spacing.md / 2)
{
    setObjectName(QStringLiteral("aida.progress_bar"));
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(QStringLiteral("Progress"));
}

void AidaProgressBar::setProgress(qreal progress)
{
    const qreal clamped = clamp01(progress);
    if (progress_ == clamped)
        return;
    progress_ = clamped;
    setAccessibleDescription(QStringLiteral("%1%").arg(qRound(progress_ * 100.0)));
    updateAnimations();
    update();
}

void AidaProgressBar::setIndeterminate(bool indeterminate)
{
    if (indeterminate_ == indeterminate)
        return;
    indeterminate_ = indeterminate;
    setAccessibleDescription(indeterminate_
        ? QStringLiteral("Busy") : QStringLiteral("%1%").arg(qRound(progress_ * 100.0)));
    updateAnimations();
    update();
}

void AidaProgressBar::setShimmerEnabled(bool enabled)
{
    if (shimmer_enabled_ == enabled)
        return;
    shimmer_enabled_ = enabled;
    updateAnimations();
    update();
}

void AidaProgressBar::setBarHeight(int height)
{
    if (bar_height_ == height || height < 2)
        return;
    bar_height_ = height;
    updateGeometry();
    update();
}

QSize AidaProgressBar::sizeHint() const
{
    return QSize(aida::qt::theme::tokens().panel.overlay_margin * 5, bar_height_);
}

QSize AidaProgressBar::minimumSizeHint() const
{
    return QSize(aida::qt::theme::tokens().row.compact, bar_height_);
}

void AidaProgressBar::updateAnimations()
{
    const bool reduced = aida::qt::theme::AidaMotion::reducedMotion();
    const bool visible_ok = isVisible();

    const bool want_shimmer = visible_ok && !reduced && !indeterminate_
        && shimmer_enabled_ && progress_ > 0.0;
    if (want_shimmer) {
        if (!shimmer_anim_) {
            shimmer_anim_ = aida::qt::theme::motion::loop(kShimmerPeriodMs, this);
            if (shimmer_anim_) {
                connect(shimmer_anim_, &QVariantAnimation::valueChanged, this,
                    [this](const QVariant& v) {
                        if (aida::qt::theme::AidaMotion::reducedMotion()) {
                            shimmer_anim_->stop();
                            update();
                            return;
                        }
                        shimmer_phase_ = v.toReal();
                        update();
                    });
            }
        }
        if (shimmer_anim_)
            shimmer_anim_->start();
    } else if (shimmer_anim_) {
        shimmer_anim_->stop();
    }

    const bool want_slide = visible_ok && !reduced && indeterminate_;
    if (want_slide) {
        if (!slide_anim_) {
            slide_anim_ = aida::qt::theme::motion::loop(kSlidePeriodMs, this);
            if (slide_anim_) {
                connect(slide_anim_, &QVariantAnimation::valueChanged, this,
                    [this](const QVariant& v) {
                        if (aida::qt::theme::AidaMotion::reducedMotion()) {
                            slide_anim_->stop();
                            update();
                            return;
                        }
                        slide_phase_ = v.toReal();
                        update();
                    });
            }
        }
        if (slide_anim_)
            slide_anim_->start();
    } else if (slide_anim_) {
        slide_anim_->stop();
    }
}

void AidaProgressBar::paintDeterminate(QPainter& p, const QRectF& track)
{
    const auto& t = aida::qt::theme::tokens();
    const qreal fw = track.width() * progress_;
    if (fw <= 0.5)
        return;

    const QRectF fill(track.left(), track.top(), fw, track.height());
    QLinearGradient grad(fill.topLeft(), fill.topRight());
    grad.setColorAt(0.0, t.accent_grad_top);
    grad.setColorAt(1.0, t.accent_grad_bot);
    p.setPen(Qt::NoPen);
    p.setBrush(grad);
    const qreal fill_r = (std::min)(track.height() * 0.5, fw * 0.5);
    p.drawRoundedRect(fill, fill_r, fill_r);

    const bool shimmer_active = !aida::qt::theme::AidaMotion::reducedMotion()
        && shimmer_anim_ && shimmer_anim_->state() == QAbstractAnimation::Running;
    if (shimmer_active) {
        const qreal sw = fw * 0.30;
        if (sw > qreal(t.spacing.xs)) {
            QPainterPath clip_path;
            clip_path.addRoundedRect(track, track.height() * 0.5, track.height() * 0.5);
            p.setClipPath(clip_path);
            const qreal sx = fill.left() + fw * shimmer_phase_ - fw * 0.15;
            QLinearGradient band(QPointF(sx, 0.0), QPointF(sx + sw, 0.0));
            band.setColorAt(0.0, with_alpha(t.sheen, 0.0));
            band.setColorAt(0.5, with_alpha(t.sheen, 0.16));
            band.setColorAt(1.0, with_alpha(t.sheen, 0.0));
            p.fillRect(QRectF(sx, fill.top(), sw, fill.height()), band);
            p.setClipping(false);
        }
    }
}

void AidaProgressBar::paintIndeterminate(QPainter& p, const QRectF& track)
{
    const auto& t = aida::qt::theme::tokens();
    const qreal w = track.width();
    const qreal bw = w * kIndeterminateBandFraction;
    const qreal bx = track.left() + (w + bw) * slide_phase_ - bw;

    const QRectF band_rect((std::max)(bx, track.left()), track.top(),
        (std::min)(bx + bw, track.right()) - (std::max)(bx, track.left()), track.height());
    if (band_rect.width() <= 0.0)
        return;

    QPainterPath clip_path;
    clip_path.addRoundedRect(track, track.height() * 0.5, track.height() * 0.5);
    p.setClipPath(clip_path);

    const QColor band_col = mix_colors(t.accent_grad_top, t.accent_grad_bot, 0.5);
    QLinearGradient band(QPointF(bx, 0.0), QPointF(bx + bw, 0.0));
    band.setColorAt(0.0, with_alpha(band_col, 0.0));
    band.setColorAt(0.5, with_alpha(band_col, 1.0));
    band.setColorAt(1.0, with_alpha(band_col, 0.0));
    p.fillRect(QRectF(bx, track.top(), bw, track.height()), band);
    p.setClipping(false);
}

void AidaProgressBar::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const auto& t = aida::qt::theme::tokens();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF track = QRectF(rect());
    p.setPen(Qt::NoPen);
    p.setBrush(t.panel_header);
    p.drawRoundedRect(track, track.height() * 0.5, track.height() * 0.5);

    if (indeterminate_)
        paintIndeterminate(p, track);
    else
        paintDeterminate(p, track);
}

void AidaProgressBar::showEvent(QShowEvent* event)
{
    updateAnimations();
    QWidget::showEvent(event);
}

void AidaProgressBar::hideEvent(QHideEvent* event)
{
    if (shimmer_anim_)
        shimmer_anim_->stop();
    if (slide_anim_)
        slide_anim_->stop();
    QWidget::hideEvent(event);
}

AidaProgressRing::AidaProgressRing(QWidget* parent)
    : QWidget(parent)
    , thickness_(qreal(aida::qt::theme::tokens().control.focus_ring))
{
    setObjectName(QStringLiteral("aida.progress_ring"));
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAccessibleName(QStringLiteral("Progress indicator"));
}

void AidaProgressRing::setProgress(qreal progress)
{
    const qreal clamped = clamp01(progress);
    if (progress_ == clamped)
        return;
    progress_ = clamped;
    setAccessibleDescription(QStringLiteral("%1%").arg(qRound(progress_ * 100.0)));
    update();
}

void AidaProgressRing::setIndeterminate(bool indeterminate)
{
    if (indeterminate_ == indeterminate)
        return;
    indeterminate_ = indeterminate;
    setAccessibleDescription(indeterminate_
        ? QStringLiteral("Busy") : QStringLiteral("%1%").arg(qRound(progress_ * 100.0)));
    updateAnimations();
    update();
}

void AidaProgressRing::setThickness(qreal thickness)
{
    if (thickness_ == thickness || thickness <= 0.0)
        return;
    thickness_ = thickness;
    update();
}

QSize AidaProgressRing::sizeHint() const
{
    const int side = aida::qt::theme::tokens().row.compact;
    return QSize(side, side);
}

QSize AidaProgressRing::minimumSizeHint() const
{
    const int side = aida::qt::theme::tokens().spacing.lg;
    return QSize(side, side);
}

void AidaProgressRing::updateAnimations()
{
    const bool want_spin = isVisible() && indeterminate_
        && !aida::qt::theme::AidaMotion::reducedMotion();
    if (want_spin) {
        if (!spin_anim_) {
            spin_anim_ = aida::qt::theme::motion::loop(kSpinPeriodMs, this);
            if (spin_anim_) {
                connect(spin_anim_, &QVariantAnimation::valueChanged, this,
                    [this](const QVariant& v) {
                        if (aida::qt::theme::AidaMotion::reducedMotion()) {
                            spin_anim_->stop();
                            update();
                            return;
                        }
                        spin_degrees_ = v.toReal() * 360.0;
                        update();
                    });
            }
        }
        if (spin_anim_)
            spin_anim_->start();
    } else if (spin_anim_) {
        spin_anim_->stop();
    }
}

void AidaProgressRing::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const auto& t = aida::qt::theme::tokens();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal side = (std::min)(qreal(rect().width()), qreal(rect().height()));
    const QPointF center(qreal(rect().left()) + qreal(rect().width()) * 0.5,
        qreal(rect().top()) + qreal(rect().height()) * 0.5);
    const qreal radius = side * 0.5 - thickness_ * 0.5 - 1.0;
    if (radius <= 0.5)
        return;
    const QRectF arc_rect(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0);

    QPen track_pen(with_alpha(t.panel_header, 0.85), thickness_);
    track_pen.setCapStyle(Qt::RoundCap);
    p.setPen(track_pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(arc_rect);

    if (indeterminate_) {
        const qreal span_deg = 1.5 * 180.0 / kPi;
        const bool spinning = !aida::qt::theme::AidaMotion::reducedMotion()
            && spin_anim_ && spin_anim_->state() == QAbstractAnimation::Running;
        if (spinning) {
            for (int i = 0; i < 32; ++i) {
                const qreal fade = 1.0 - qreal(i) / 32.0;
                const qreal a0 = -(spin_degrees_ + qreal(i) / 32.0 * span_deg);
                const qreal seg_len = span_deg / 32.0;
                QPen seg_pen(with_alpha(t.accent, fade), thickness_);
                seg_pen.setCapStyle(Qt::RoundCap);
                p.setPen(seg_pen);
                p.drawArc(arc_rect, qRound(a0 * 16.0), -qRound(seg_len * 16.0));
            }
        } else {
            QPen seg_pen(with_alpha(t.accent, 0.8), thickness_);
            seg_pen.setCapStyle(Qt::RoundCap);
            p.setPen(seg_pen);
            p.drawArc(arc_rect, 90 * 16, -qRound(span_deg * 16.0));
        }
    } else {
        QPen arc_pen(t.accent, thickness_);
        arc_pen.setCapStyle(Qt::RoundCap);
        p.setPen(arc_pen);
        p.drawArc(arc_rect, 90 * 16, -qRound(progress_ * 360.0 * 16.0));
    }
}

void AidaProgressRing::showEvent(QShowEvent* event)
{
    updateAnimations();
    QWidget::showEvent(event);
}

void AidaProgressRing::hideEvent(QHideEvent* event)
{
    if (spin_anim_)
        spin_anim_->stop();
    QWidget::hideEvent(event);
}

}
