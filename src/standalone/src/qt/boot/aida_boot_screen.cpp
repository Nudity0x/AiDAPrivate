#include "qt/boot/aida_boot_screen.hpp"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QLinearGradient>
#include <QHideEvent>
#include <QShowEvent>
#include <QPainterPath>
#include <QTimer>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

#include "helpers/diag_log.hpp"
#include "qt/qt_startup_orchestrator.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::boot {

namespace {

using widgets::with_alpha;

class BootFadeOverlay : public QWidget {
public:
    explicit BootFadeOverlay(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    }
    void setAlpha(qreal alpha)
    {
        alpha_ = widgets::clamp01(alpha);
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(theme::tokens().bg_base, alpha_));
        p.drawRect(rect());
    }
private:
    qreal alpha_ = 1.0;
};


constexpr double k_loading_min_sec = 3.0;
constexpr double k_loading_fade_rate = 1.5;
constexpr double k_welcome_total_sec = 3.5;
constexpr double k_welcome_fade_in_sec = 0.6;
constexpr double k_welcome_fade_out_from_sec = 2.6;
constexpr double k_welcome_fade_out_span_sec = 0.9;
constexpr int k_ide_fade_ms = 1000;

QRectF cover_fit(const QSizeF& image, const QRectF& target)
{
    if (image.width() <= 0.0 || image.height() <= 0.0 || target.isEmpty())
        return target;
    const qreal aspect_img = image.width() / image.height();
    const qreal aspect_win = target.width() / target.height();
    qreal w, h;
    if (aspect_img > aspect_win) {
        h = target.height();
        w = h * aspect_img;
    } else {
        w = target.width();
        h = w / aspect_img;
    }
    return QRectF(target.left() + (target.width() - w) * 0.5,
                  target.top() + (target.height() - h) * 0.5, w, h);
}

void paint_constellation(QPainter& p, const QPointF& center, qreal radius, int dot_count,
                         qreal t_seconds, const QColor& col)
{
    for (int i = 0; i < dot_count; ++i) {
        const qreal ang = (static_cast<qreal>(i) / dot_count) * 6.2831853 + t_seconds * 0.45;
        const QPointF pt(center.x() + std::cos(ang) * radius, center.y() + std::sin(ang) * radius);
        const qreal wave = std::sin(t_seconds * 1.5 + i * 0.7) * 0.5 + 0.5;
        const qreal br = 0.3 + wave * 0.5;
        const qreal halo_a = br * 0.5;
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(col, halo_a * 0.4));
        p.drawEllipse(pt, 4.0, 4.0);
        p.setBrush(with_alpha(col, halo_a));
        p.drawEllipse(pt, 2.0, 2.0);
    }
}

void paint_logomark(QPainter& p, const QPointF& center, qreal size, qreal reveal,
                    qreal pulse, qreal alpha)
{
    const auto& t = theme::tokens();
    const qreal r = size * 0.5;
    const qreal reveal_c = widgets::clamp01(reveal);

    const qreal halo_r = r * (1.4 + pulse * 0.15);
    const QColor halo = with_alpha(t.accent_glow, alpha * (0.45 + pulse * 0.35) * reveal_c);
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 5; ++i) {
        const qreal rr = halo_r + i * size * 0.08;
        const qreal ha = (1.0 - i / 5.0) * 0.25;
        p.setBrush(with_alpha(halo, ha));
        p.drawEllipse(center, rr, rr);
    }

    const QColor stroke = with_alpha(t.accent_grad_top, alpha * reveal_c);
    const qreal th = size * 0.10;
    const QPointF left_top(center.x() - r * 0.55, center.y() + r * 0.65);
    const QPointF left_apex(center.x() - r * 0.05, center.y() - r * 0.65);
    const QPointF right_apex(center.x() + r * 0.05, center.y() - r * 0.65);
    const QPointF right_bot(center.x() + r * 0.55, center.y() + r * 0.65);
    const QPointF cross_left(center.x() - r * 0.30, center.y() + r * 0.05);
    const QPointF cross_right(center.x() + r * 0.30, center.y() + r * 0.05);

    auto draw_segment = [&](const QPointF& a, const QPointF& b, qreal t0, qreal t1) {
        if (reveal_c <= t0)
            return;
        qreal local = (reveal_c - t0) / (t1 - t0);
        local = widgets::clamp01(local);
        if (local <= 0.0)
            return;
        const qreal eased = theme::easingFor(theme::Ease::OutCubic).valueForProgress(local);
        p.setPen(QPen(stroke, th, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, QPointF(a.x() + (b.x() - a.x()) * eased,
                              a.y() + (b.y() - a.y()) * eased));
    };

    draw_segment(left_top, left_apex, 0.00, 0.45);
    draw_segment(left_apex, right_apex, 0.30, 0.50);
    draw_segment(right_apex, right_bot, 0.40, 0.85);
    draw_segment(cross_left, cross_right, 0.65, 0.95);
}

void paint_wordmark(QPainter& p, const QPointF& origin, qreal scale, const QFont& font,
                    qreal reveal, qreal alpha)
{
    const auto& t = theme::tokens();
    const QString letters = QStringLiteral("AiDA");
    const qreal tracking = 4.0 * scale;
    const QColor base = with_alpha(t.text_primary, alpha);
    qreal x = origin.x();
    const QFontMetricsF fm(font);
    for (int i = 0; i < 4; ++i) {
        const qreal t0 = i * 0.12;
        const qreal t1 = t0 + 0.35;
        const qreal local = widgets::clamp01((reveal - t0) / (t1 - t0));
        const qreal eased = theme::easingFor(theme::Ease::OutQuint).valueForProgress(local);
        const QString glyph = letters.mid(i, 1);
        const qreal glyph_w = fm.horizontalAdvance(glyph);
        const qreal yoff = (1.0 - eased) * 8.0 * scale;
        p.setPen(with_alpha(base, eased * alpha));
        p.setFont(font);
        p.drawText(QPointF(x, origin.y() + yoff + fm.ascent()), glyph);
        x += glyph_w;
        if (i < 3)
            x += tracking;
    }
}

qreal wordmark_width(const QFont& font, qreal scale)
{
    const QString letters = QStringLiteral("AiDA");
    const qreal tracking = 4.0 * scale;
    const QFontMetricsF fm(font);
    qreal total = 0.0;
    for (int i = 0; i < 4; ++i) {
        total += fm.horizontalAdvance(letters.mid(i, 1));
        if (i < 3)
            total += tracking;
    }
    return total;
}

qreal pulse_06(qreal seconds)
{
    return std::sin(seconds * 6.2831853 / 1.6666) * 0.5 + 0.5;
}

}

AidaBootScreen::AidaBootScreen(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.boot_screen"));
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    background_ = QImage(QStringLiteral(":/img/background.webp"));
    background_ok_ = !background_.isNull();
    logo_ = QImage(QStringLiteral(":/img/aidalogo.png"));
    logo_ok_ = !logo_.isNull();
    if (!background_ok_)
        diag::log_tagged_critical("qt_boot", "boot_background_missing resource=:/img/background.webp");
    if (!logo_ok_)
        diag::log_tagged_critical("qt_boot", "boot_logo_missing resource=:/img/aidalogo.png");
    clock_.start();
}

AidaBootScreen::~AidaBootScreen()
{
    stopClock();
}

const char* AidaBootScreen::phaseLabel(int step)
{
    switch (step) {
    case 0: return "Bootstrapping";
    case 1: return "Initializing AiDA runtime core";
    case 2: return "Probing network surface";
    case 3: return "Arming memory scanner";
    case 4: return "Spinning up MITM proxy";
    case 5: return "Loading script engine";
    case 6: return "Ready";
    default: return "Ready";
    }
}

void AidaBootScreen::setProgress(int step, int total)
{
    if (total < 1)
        total = 1;
    const int clamped = (std::min)(step, total);
    if (clamped != step_) {
        prev_phase_ = cur_phase_.isEmpty()
            ? QString::fromLatin1(phaseLabel(step_))
            : cur_phase_;
        cur_phase_ = QString::fromLatin1(phaseLabel(clamped));
        if (swap_anim_)
            swap_anim_->stop();
        if (theme::AidaMotion::reducedMotion()) {
            swap_ = 1.0;
        } else {
            auto* anim = new QVariantAnimation(this);
            anim->setStartValue(0.0);
            anim->setEndValue(1.0);
            anim->setDuration(theme::tokens().motion.fast);
            anim->setEasingCurve(theme::easingFor(theme::Ease::OutCubic));
            connect(anim, &QVariantAnimation::valueChanged, this,
                    [this](const QVariant& v) { swap_ = v.toDouble(); });
            anim->start(QAbstractAnimation::DeleteWhenStopped);
            swap_anim_ = anim;
        }
    }
    step_ = clamped;
    total_ = total;
    update();
}

void AidaBootScreen::setFade(qreal fade)
{
    fade_ = widgets::clamp01(fade);
    update();
}

void AidaBootScreen::coverParent()
{
    if (parentWidget())
        setGeometry(parentWidget()->rect());
}

void AidaBootScreen::startClock()
{
    if (ticker_ || theme::AidaMotion::reducedMotion())
        return;
    ticker_ = new QVariantAnimation(this);
    ticker_->setStartValue(0.0);
    ticker_->setEndValue(1000000.0);
    ticker_->setDuration(1000000 * 1000);
    ticker_->setLoopCount(-1);
    ticker_->setEasingCurve(theme::easingFor(theme::Ease::Linear));
    connect(ticker_, &QVariantAnimation::valueChanged, this, [this](const QVariant&) {
        update();
    });
    ticker_->start();
}

void AidaBootScreen::stopClock()
{
    if (ticker_) {
        ticker_->stop();
        ticker_->deleteLater();
        ticker_ = nullptr;
    }
}

void AidaBootScreen::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    startClock();
}

void AidaBootScreen::hideEvent(QHideEvent* event)
{
    stopClock();
    QWidget::hideEvent(event);
}

void AidaBootScreen::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto& t = theme::tokens();
    const bool reduced = theme::AidaMotion::reducedMotion();
    const qreal seconds = clock_.nsecsElapsed() / 1.0e9;
    const qreal vis = fade_;
    const QRectF r = rect();
    const qreal ww = r.width();
    const qreal wh = r.height();
    const qreal cx = r.left() + ww * 0.5;
    const qreal cy = r.top() + wh * 0.5 - 8.0;

    p.setPen(Qt::NoPen);
    p.setBrush(t.bg_base);
    p.drawRect(r);

    if (background_ok_) {
        const QRectF bg = cover_fit(background_.size(), r);
        p.setOpacity(0.42 * vis);
        p.drawImage(bg, background_);
        p.setOpacity(1.0);
        p.setBrush(with_alpha(t.bg_base, 0.55 * vis));
        p.drawRect(r);
    }

    const qreal aura_r = ww * 0.55;
    const QColor aura = with_alpha(t.accent_glow, 0.45 * vis);
    for (int i = 0; i < 5; ++i) {
        const qreal rr = aura_r + i * 14.0;
        const qreal fa = (1.0 - i / 5.0) * 0.55;
        p.setBrush(with_alpha(aura, fa));
        p.drawEllipse(QPointF(cx, cy), rr, rr);
    }

    const qreal reveal_t = reduced ? 1.0 : (std::min)(seconds / 0.480, 1.0);
    const qreal reveal_eased = theme::easingFor(theme::Ease::OutBack).valueForProgress(reveal_t);
    const qreal pulse = reduced ? 0.5 : pulse_06(seconds);

    paint_constellation(p, QPointF(cx, cy), 80.0, 12, seconds * 0.4,
                        with_alpha(t.accent, vis));

    const qreal logo_size = 96.0;
    if (logo_ok_) {
        const qreal ls = logo_size * (0.6 + 0.4 * reveal_eased);
        const qreal aspect = logo_.height() > 0
            ? static_cast<qreal>(logo_.width()) / logo_.height() : 1.0;
        const qreal lw = ls * aspect;
        p.setOpacity(vis * (0.85 + 0.15 * pulse));
        p.drawImage(QRectF(cx - lw * 0.5, cy - 18.0 - ls * 0.5, lw, ls), logo_);
        p.setOpacity(1.0);
    } else {
        paint_logomark(p, QPointF(cx, cy - 18.0), logo_size, reveal_eased, pulse, vis);
    }

    const QFont display_font = theme::fonts::display();
    const qreal wm_total_w = wordmark_width(display_font, 1.0);
    const qreal wm_x = cx - wm_total_w * 0.5;
    const qreal wm_y = cy + logo_size * 0.5 + 12.0;
    const qreal wm_reveal = reduced ? 1.0
        : (std::max)(0.0, (std::min)((seconds - 0.18) / 0.62, 1.0));
    paint_wordmark(p, QPointF(wm_x, wm_y), 1.0, display_font, wm_reveal, vis);

    const QFontMetricsF display_fm(display_font);
    const qreal tag_a = reduced ? vis
        : (std::min)((std::max)(seconds - 1.6, 0.0) / 0.5, 1.0) * vis;
    if (tag_a > 0.01) {
        const QString tag = QStringLiteral("Reverse engineering, reimagined.");
        const QFont body = theme::fonts::body();
        const QFontMetricsF body_fm(body);
        p.setFont(body);
        p.setPen(with_alpha(t.text_secondary, tag_a));
        p.drawText(QPointF(cx - body_fm.horizontalAdvance(tag) * 0.5,
                           wm_y + display_fm.height() + 18.0 + body_fm.ascent()), tag);
    }

    const qreal bar_w = (std::min)(ww * 0.55, 280.0);
    const qreal bar_h = t.spacing.xxs + t.panel.border;
    const qreal bar_x = cx - bar_w * 0.5;
    const qreal bar_y = r.top() + wh - 60.0;

    int total_steps = total_;
    int cur_step = step_;
    if (total_steps < 1)
        total_steps = 1;
    if (cur_step > total_steps)
        cur_step = total_steps;
    const qreal prog = static_cast<qreal>(cur_step) / total_steps;
    const qreal dt_paint = (std::min)((std::max)(seconds - last_tick_sec_, 0.0), 0.05);
    last_tick_sec_ = seconds;
    if (reduced)
        anim_progress_ = prog;
    else
        anim_progress_ += (prog - anim_progress_) * (std::min)(8.0 * dt_paint, 1.0);

    p.setBrush(with_alpha(t.panel_header, 0.85 * vis));
    p.drawRoundedRect(QRectF(bar_x, bar_y, bar_w, bar_h), bar_h * 0.5, bar_h * 0.5);

    const qreal fw = bar_w * anim_progress_;
    if (fw > 1.0) {
        QLinearGradient fill_grad(QPointF(bar_x, bar_y), QPointF(bar_x, bar_y + bar_h));
        fill_grad.setColorAt(0.0, with_alpha(t.accent_grad_top, vis));
        fill_grad.setColorAt(1.0, with_alpha(t.accent_grad_bot, vis));
        p.setBrush(fill_grad);
        p.drawRect(QRectF(bar_x, bar_y, fw, bar_h));

        const qreal sweep_period = 1.4;
        const qreal ph = std::fmod(seconds / sweep_period, 1.0);
        const qreal sx = bar_x + fw * ph - fw * 0.18;
        const qreal sw = fw * 0.36;
        if (!reduced && sw > 4.0) {
            p.save();
            p.setClipRect(QRectF(bar_x, bar_y, fw, bar_h));
            QLinearGradient sweep(QPointF(sx, bar_y), QPointF(sx + sw, bar_y));
            sweep.setColorAt(0.0, Qt::transparent);
            sweep.setColorAt(0.5, with_alpha(t.text_primary, 0.35 * vis));
            sweep.setColorAt(1.0, Qt::transparent);
            p.setBrush(sweep);
            p.drawRect(QRectF(sx, bar_y, sw, bar_h));
            p.restore();
        }
    }

    const QFont cap = theme::fonts::body();
    const QFontMetricsF cap_fm(cap);
    const qreal ph_y = bar_y - cap_fm.height() - 8.0;
    p.setFont(cap);
    if (swap_ < 1.0 && !prev_phase_.isEmpty()) {
        const qreal prev_a = (1.0 - swap_) * vis;
        const qreal prev_y = ph_y - swap_ * 6.0;
        p.setPen(with_alpha(t.text_secondary, prev_a));
        p.drawText(QPointF(cx - cap_fm.horizontalAdvance(prev_phase_) * 0.5, prev_y),
                   prev_phase_);
        const qreal cur_y = ph_y + (1.0 - swap_) * 6.0;
        p.setPen(with_alpha(t.text_secondary, swap_ * vis));
        p.drawText(QPointF(cx - cap_fm.horizontalAdvance(cur_phase_) * 0.5, cur_y),
                   cur_phase_);
    } else {
        p.setPen(with_alpha(t.text_secondary, vis));
        const QString label = cur_phase_.isEmpty()
            ? QString::fromLatin1(phaseLabel(cur_step)) : cur_phase_;
        p.drawText(QPointF(cx - cap_fm.horizontalAdvance(label) * 0.5, ph_y), label);
    }

    const QString step_text = QStringLiteral("%1 / %2").arg(cur_step).arg(total_steps);
    p.setPen(with_alpha(t.text_dim, vis));
    p.drawText(QPointF(cx + bar_w * 0.5 - cap_fm.horizontalAdvance(step_text),
                       bar_y + bar_h + 12.0 + cap_fm.ascent()),
               step_text);
}

AidaWelcomeScreen::AidaWelcomeScreen(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.welcome_screen"));
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    logo_ = QImage(QStringLiteral(":/img/aidalogo.png"));
    logo_ok_ = !logo_.isNull();
}

AidaWelcomeScreen::~AidaWelcomeScreen()
{
    stopClock();
}

void AidaWelcomeScreen::restart()
{
    clock_.start();
    update();
}

void AidaWelcomeScreen::coverParent()
{
    if (parentWidget())
        setGeometry(parentWidget()->rect());
}

void AidaWelcomeScreen::startClock()
{
    if (ticker_ || theme::AidaMotion::reducedMotion())
        return;
    ticker_ = new QVariantAnimation(this);
    ticker_->setStartValue(0.0);
    ticker_->setEndValue(1000000.0);
    ticker_->setDuration(1000000 * 1000);
    ticker_->setLoopCount(-1);
    ticker_->setEasingCurve(theme::easingFor(theme::Ease::Linear));
    connect(ticker_, &QVariantAnimation::valueChanged, this, [this](const QVariant&) {
        update();
    });
    ticker_->start();
}

void AidaWelcomeScreen::stopClock()
{
    if (ticker_) {
        ticker_->stop();
        ticker_->deleteLater();
        ticker_ = nullptr;
    }
}

void AidaWelcomeScreen::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    clock_.start();
    startClock();
}

void AidaWelcomeScreen::hideEvent(QHideEvent* event)
{
    stopClock();
    QWidget::hideEvent(event);
}

void AidaWelcomeScreen::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto& t = theme::tokens();
    const bool reduced = theme::AidaMotion::reducedMotion();
    const qreal sec = clock_.nsecsElapsed() / 1.0e9;
    const qreal fade_in = reduced ? 1.0 : (std::min)(sec / k_welcome_fade_in_sec, 1.0);
    const qreal fade_out = reduced ? 1.0 : (sec > k_welcome_fade_out_from_sec
        ? (std::max)(0.0, 1.0 - (sec - k_welcome_fade_out_from_sec) / k_welcome_fade_out_span_sec)
        : 1.0);
    const qreal base_a = fade_in * fade_out;

    const QRectF r = rect();
    const qreal ww = r.width();
    const qreal cx = r.left() + ww * 0.5;
    const qreal cy = r.top() + r.height() * 0.5 - 4.0;

    p.setPen(Qt::NoPen);
    p.setBrush(t.bg_base);
    p.drawRect(r);

    const qreal aura_r = ww * 0.45;
    for (int i = 0; i < 5; ++i) {
        const qreal rr = aura_r + i * 18.0;
        const qreal fa = (1.0 - i / 5.0) * 0.40 * base_a;
        p.setBrush(with_alpha(t.accent_glow, fa));
        p.drawEllipse(QPointF(cx, cy), rr, rr);
    }

    paint_constellation(p, QPointF(cx, cy), 92.0, 12, sec * 0.4,
                        with_alpha(t.accent, base_a));

    const qreal reveal = reduced ? 1.0 : theme::easingFor(theme::Ease::OutBack)
        .valueForProgress((std::min)(sec / 0.480, 1.0));
    const qreal pulse = reduced ? 0.5 : pulse_06(sec);
    if (logo_ok_) {
        const qreal ls = 84.0;
        const qreal aspect = logo_.height() > 0
            ? static_cast<qreal>(logo_.width()) / logo_.height() : 1.0;
        const qreal lw = ls * aspect;
        p.setOpacity(base_a * (0.85 + 0.15 * pulse) * reveal);
        p.drawImage(QRectF(cx - lw * 0.5, cy - 26.0 - ls * 0.5, lw, ls), logo_);
        p.setOpacity(1.0);
    } else {
        paint_logomark(p, QPointF(cx, cy - 26.0), 84.0, reveal, pulse, base_a);
    }

    const QFont display_font = theme::fonts::display();
    const qreal wm_total_w = wordmark_width(display_font, 1.0);
    const qreal wm_x = cx - wm_total_w * 0.5;
    const qreal wm_y = cy + 38.0;
    const qreal wm_reveal = reduced ? 1.0
        : (std::min)((std::max)(sec - 0.18, 0.0) / 0.62, 1.0);
    paint_wordmark(p, QPointF(wm_x, wm_y), 1.0, display_font, wm_reveal, base_a);

    const QFont body = theme::fonts::body();
    const QFontMetricsF body_fm(body);
    const qreal sub_a = reduced ? base_a
        : (std::min)((std::max)(sec - 0.7, 0.0) / 0.5, 1.0) * fade_out;
    if (sub_a > 0.01) {
        const QString tagline = QStringLiteral("Reverse engineering, reimagined.");
        p.setFont(body);
        p.setPen(with_alpha(t.text_secondary, sub_a));
        p.drawText(QPointF(cx - body_fm.horizontalAdvance(tagline) * 0.5,
                           wm_y + 32.0 + body_fm.ascent()), tagline);
        const qreal msg_a = reduced ? base_a
            : (std::min)((std::max)(sec - 1.4, 0.0) / 0.5, 1.0) * fade_out;
        if (msg_a > 0.01) {
            const QString msg = QStringLiteral("Your session is ready.");
            p.setPen(with_alpha(t.text_dim, msg_a));
            p.drawText(QPointF(cx - body_fm.horizontalAdvance(msg) * 0.5,
                               wm_y + 32.0 + body_fm.height() + 28.0 + body_fm.ascent()), msg);
        }
    }
}

AidaBootController::AidaBootController(QObject* parent)
    : QObject(parent)
{
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(33);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, [this] { poll(); });
}

AidaBootController::~AidaBootController() = default;

void AidaBootController::attach(QWidget* container, AidaBootScreen* boot_screen,
                                AidaWelcomeScreen* welcome_screen)
{
    container_ = container;
    boot_screen_ = boot_screen;
    welcome_screen_ = welcome_screen;
    if (container_) {
        container_->installEventFilter(this);
        if (boot_screen_)
            boot_screen_->setParent(container_);
        if (welcome_screen_)
            welcome_screen_->setParent(container_);
    }
    syncCovers();
    if (welcome_screen_)
        welcome_screen_->hide();
    if (boot_screen_)
        boot_screen_->hide();
}

void AidaBootController::setOrchestrator(AidaStartupOrchestrator* orchestrator)
{
    orchestrator_ = orchestrator;
}

void AidaBootController::begin()
{
    if (poll_timer_->isActive())
        return;
    boot_clock_.start();
    phase_clock_.start();
    poll_timer_->start();
    diag::log_tagged_critical_fmt("qt_boot", "boot_controller_begin tid=%lu",
        static_cast<unsigned long>(::GetCurrentThreadId()));
}

void AidaBootController::poll()
{
    if (finished_)
        return;

    if (!bg_completed_ && orchestrator_ && orchestrator_->bgInitDone())
        bg_completed_ = true;

    logLoadingWaitIfDue();

    switch (state_) {
    case State::Loading:
        if (bg_completed_)
            enterIdeFade();
        break;
    case State::LoadingFade:
        if (phase_clock_.nsecsElapsed() / 1.0e9 > 2.0)
            enterWelcome();
        break;
    case State::Welcome:
        if (phase_clock_.nsecsElapsed() / 1.0e9 >= k_welcome_total_sec)
            enterIdeFade();
        break;
    case State::IdeFade:
        break;
    case State::Ready:
        break;
    }
}

void AidaBootController::logLoadingWaitIfDue()
{
    if (state_ != State::Loading)
        return;
    const double elapsed = boot_clock_.nsecsElapsed() / 1.0e9;
    if (elapsed < 5.0)
        return;
    if (wait_logged_once_ && (elapsed - last_wait_log_sec_) < 5.0)
        return;
    wait_logged_once_ = true;
    last_wait_log_sec_ = elapsed;
    const bool bg_done = orchestrator_ && orchestrator_->bgInitDone();
    diag::log_tagged_critical_fmt("render",
        "loading_screen_wait timer=%.2f bg_completed=%d bg_done_ptr=1 bg_done=%d bg_step=%d bg_total=%d",
        elapsed,
        bg_completed_ ? 1 : 0,
        bg_done ? 1 : 0,
        orchestrator_ ? orchestrator_->bgInitStep() : 0,
        orchestrator_ ? orchestrator_->bgInitTotal() : 0);
}

void AidaBootController::enterLoadingFade()
{
    state_ = State::LoadingFade;
    phase_clock_.start();
    if (theme::AidaMotion::reducedMotion()) {
        enterWelcome();
        return;
    }
    if (fade_anim_)
        fade_anim_->stop();
    auto* anim = new QVariantAnimation(this);
    anim->setDuration(static_cast<int>(1000.0 / k_loading_fade_rate));
    anim->setStartValue(1.0);
    anim->setEndValue(0.0);
    anim->setEasingCurve(theme::easingFor(theme::Ease::Linear));
    connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        if (boot_screen_)
            boot_screen_->setFade(v.toDouble());
    });
    connect(anim, &QVariantAnimation::finished, this, [this] {
        enterWelcome();
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
    fade_anim_ = anim;
}

void AidaBootController::enterWelcome()
{
    if (state_ == State::Welcome)
        return;
    state_ = State::Welcome;
    phase_clock_.start();
    if (boot_screen_)
        boot_screen_->hide();
    if (welcome_screen_) {
        welcome_screen_->coverParent();
        welcome_screen_->show();
        welcome_screen_->raise();
        welcome_screen_->restart();
    }
    diag::log_tagged_critical_fmt("qt_boot", "boot_welcome_enter elapsed_ms=%llu",
        static_cast<unsigned long long>(boot_clock_.nsecsElapsed() / 1000000));
}

void AidaBootController::enterIdeFade()
{
    if (state_ == State::IdeFade || state_ == State::Ready)
        return;
    state_ = State::IdeFade;
    phase_clock_.start();
    if (welcome_screen_)
        welcome_screen_->hide();
    if (boot_screen_)
        boot_screen_->hide();

    if (!theme::AidaMotion::reducedMotion() && container_) {
        auto* overlay = new BootFadeOverlay(container_);
        overlay->setObjectName(QStringLiteral("aida.boot_fade_overlay"));
        overlay->setGeometry(container_->rect());
        overlay->show();
        overlay->raise();
        fade_cover_ = overlay;
        fade_anim_ = new QVariantAnimation(this);
        fade_anim_->setDuration(k_ide_fade_ms);
        fade_anim_->setStartValue(0.0);
        fade_anim_->setEndValue(1.0);
        fade_anim_->setEasingCurve(theme::easingFor(theme::Ease::Linear));
        connect(fade_anim_, &QVariantAnimation::valueChanged, this,
                [overlay](const QVariant& v) {
            const qreal t = v.toDouble();
            overlay->setAlpha((1.0 - t) * (1.0 - t));
        });
        connect(fade_anim_, &QVariantAnimation::finished, this, [this] {
            if (fade_cover_) {
                fade_cover_->hide();
                fade_cover_->deleteLater();
                fade_cover_ = nullptr;
            }
            enterReady();
        });
        fade_anim_->start(QAbstractAnimation::DeleteWhenStopped);
    } else {
        enterReady();
    }
}

bool AidaBootController::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == container_ && event && event->type() == QEvent::Resize)
        syncCovers();
    return QObject::eventFilter(watched, event);
}

void AidaBootController::syncCovers()
{
    if (!container_)
        return;
    if (boot_screen_)
        boot_screen_->coverParent();
    if (welcome_screen_)
        welcome_screen_->coverParent();
    if (fade_cover_)
        fade_cover_->setGeometry(container_->rect());
}

void AidaBootController::enterReady()
{
    if (finished_)
        return;
    state_ = State::Ready;
    finished_ = true;
    poll_timer_->stop();
    diag::log_tagged_critical_fmt("qt_boot", "boot_finished elapsed_ms=%llu tid=%lu",
        static_cast<unsigned long long>(boot_clock_.nsecsElapsed() / 1000000),
        static_cast<unsigned long>(::GetCurrentThreadId()));
    Q_EMIT bootFinished();
}

}
