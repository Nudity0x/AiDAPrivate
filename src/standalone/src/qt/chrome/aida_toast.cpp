#include "qt/chrome/aida_toast.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QIcon>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QResizeEvent>
#include <QShowEvent>
#include <QLabel>
#include <QPainterPath>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

#include "helpers/diag_log.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_icons.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::chrome {

namespace {

QColor toast_severity_color(AidaToastType type)
{
    const auto& t = theme::tokens();
    switch (type) {
    case AidaToastType::success: return t.success;
    case AidaToastType::warning: return t.warning;
    case AidaToastType::error:   return t.error;
    case AidaToastType::info:    return t.info;
    }
    return t.info;
}

QColor toast_severity_soft(AidaToastType type)
{
    const auto& t = theme::tokens();
    switch (type) {
    case AidaToastType::success: return t.success_soft;
    case AidaToastType::warning: return t.warning_soft;
    case AidaToastType::error:   return t.error_soft;
    case AidaToastType::info:    return t.info_soft;
    }
    return t.info_soft;
}

QString toast_severity_icon(AidaToastType type)
{
    switch (type) {
    case AidaToastType::success: return QStringLiteral("check");
    case AidaToastType::warning: return QStringLiteral("severity-warning");
    case AidaToastType::error:   return QStringLiteral("severity-error");
    case AidaToastType::info:    return QStringLiteral("severity-info");
    }
    return QStringLiteral("severity-info");
}

QString breakable_toast_text(const QString& text)
{
    constexpr int run_limit = 32;
    QString out;
    out.reserve(text.size() + 8);
    int run = 0;
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            run = 0;
            out.append(ch);
            continue;
        }
        ++run;
        out.append(ch);
        if (run >= run_limit) {
            out.append(QChar(0x200B));
            run = 0;
        }
    }
    return out;
}

double toast_wash_alpha(AidaToastType type, bool is_dark)
{
    if (type == AidaToastType::info)
        return is_dark ? 0.16 : 0.12;
    return is_dark ? 0.28 : 0.22;
}

}

AidaToastWidget::AidaToastWidget(std::uint64_t id, const QString& message,
                                 AidaToastType type, double duration_sec, QWidget* parent)
    : QFrame(parent),
      id_(id),
      message_(message),
      type_(type),
      duration_sec_(duration_sec)
{
    setObjectName(QStringLiteral("aida.toast"));
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);

    message_label_ = new QLabel(breakable_toast_text(message_), this);
    message_label_->setObjectName(QStringLiteral("aida.toast.message"));
    message_label_->setWordWrap(true);
    message_label_->setFont(theme::fonts::bodyEm());
    message_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    close_button_ = new QToolButton(this);
    close_button_->setObjectName(QStringLiteral("aida.toast.close"));
    close_button_->setIcon(theme::icons::icon(QStringLiteral("clear-x")));
    close_button_->setToolTip(QStringLiteral("Dismiss notification"));
    close_button_->setAutoRaise(true);
    close_button_->setVisible(false);
    close_button_->setCursor(Qt::PointingHandCursor);
    connect(close_button_, &QToolButton::clicked, this, [this] {
        startDismiss();
    });

    positionChildren();
}

void AidaToastWidget::setAction(const QString& label, std::function<void()> callback)
{
    if (label.isEmpty())
        return;
    action_callback_ = std::move(callback);
    action_button_ = new QToolButton(this);
    action_button_->setObjectName(QStringLiteral("aida.toast.action"));
    action_button_->setText(label);
    action_button_->setCursor(Qt::PointingHandCursor);
    connect(action_button_, &QToolButton::clicked, this, [this] {
        auto callback = std::move(action_callback_);
        action_callback_ = nullptr;
        startDismiss();
        if (callback)
            QTimer::singleShot(0, this, std::move(callback));
    });
    positionChildren();
}

void AidaToastWidget::showEvent(QShowEvent* event)
{
    QFrame::showEvent(event);
    if (!intro_done_)
        beginIntro();
}

void AidaToastWidget::beginIntro()
{
    intro_done_ = true;
    const auto& t = theme::tokens();
    if (theme::AidaMotion::reducedMotion()) {
        if (isVisible() && parentWidget()) {
            const int w = width();
            move(parentWidget()->width() - t.spacing.xxl - w, y());
        }
        return;
    }
    if (parentWidget()) {
        const int w = width();
        move(parentWidget()->width() + t.spacing.md, y());
    }
    intro_anim_ = new QVariantAnimation(this);
    intro_anim_->setDuration(t.motion.lg);
    intro_anim_->setStartValue(0.0);
    intro_anim_->setEndValue(1.0);
    intro_anim_->setEasingCurve(theme::easingFor(theme::Ease::OutCubic));
    connect(intro_anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        if (!parentWidget() || dismissing_ || swipe_dismissing_)
            return;
        const auto& t = theme::tokens();
        const double progress = v.toDouble();
        const int w = width();
        const int resting_x = parentWidget()->width() - t.spacing.xxl - w;
        const int start_x = parentWidget()->width() + t.spacing.md;
        move(start_x + static_cast<int>((resting_x - start_x) * progress), y());
    });
    intro_anim_->start(QAbstractAnimation::DeleteWhenStopped);
}

void AidaToastWidget::restackTo(int y)
{
    if (!parentWidget())
        return;
    if (!placed_) {
        placed_ = true;
        if (parentWidget()) {
            const auto& t = theme::tokens();
            const int w = width();
            move(theme::AidaMotion::reducedMotion()
                ? parentWidget()->width() - t.spacing.xxl - w
                : parentWidget()->width() + t.spacing.md, y);
        }
        return;
    }
    if (y == this->y())
        return;
    if (theme::AidaMotion::reducedMotion()) {
        move(x(), y);
        return;
    }
    if (y_anim_)
        y_anim_->stop();
    y_anim_ = new QVariantAnimation(this);
    y_anim_->setDuration(theme::tokens().motion.fast);
    y_anim_->setStartValue(static_cast<double>(this->y()));
    y_anim_->setEndValue(static_cast<double>(y));
    y_anim_->setEasingCurve(theme::easingFor(theme::Ease::OutCubic));
    connect(y_anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        move(x(), static_cast<int>(v.toDouble()));
    });
    y_anim_->start(QAbstractAnimation::DeleteWhenStopped);
}

void AidaToastWidget::tickLifetime(double dt_sec)
{
    if (dismissing_ || swipe_dismissing_ || gone_)
        return;
    const bool slowed = underMouse() || dragging_ || hasFocus()
        || (close_button_ && close_button_->hasFocus())
        || (action_button_ && action_button_->hasFocus());
    elapsed_ += dt_sec * (slowed ? toast_constants::k_hover_timer_scale : 1.0);
    updateHoverState();
    if (duration_sec_ - elapsed_ <= 0.0)
        startDismiss();
    else
        update();
}

void AidaToastWidget::updateHoverState()
{
    const bool active = (underMouse() || hasFocus()
            || (close_button_ && close_button_->hasFocus()))
        && !dismissing_ && !swipe_dismissing_;
    if (close_button_)
        close_button_->setVisible(active && !action_button_);
}

void AidaToastWidget::startDismiss()
{
    if (dismissing_ || gone_)
        return;
    dismissing_ = true;
    if (intro_anim_) {
        intro_anim_->stop();
        intro_anim_ = nullptr;
    }
    if (theme::AidaMotion::reducedMotion()) {
        opacity_ = 0.0;
        finishDismiss();
        return;
    }
    fade_anim_ = new QVariantAnimation(this);
    fade_anim_->setDuration(theme::tokens().motion.lg);
    fade_anim_->setStartValue(opacity_);
    fade_anim_->setEndValue(0.0);
    fade_anim_->setEasingCurve(theme::easingFor(theme::Ease::Linear));
    connect(fade_anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        opacity_ = v.toDouble();
        update();
    });
    connect(fade_anim_, &QVariantAnimation::finished, this, [this] {
        finishDismiss();
    });
    fade_anim_->start(QAbstractAnimation::DeleteWhenStopped);
}

void AidaToastWidget::finishDismiss()
{
    if (gone_)
        return;
    gone_ = true;
    Q_EMIT dismissFinished(id_);
    deleteLater();
}

int AidaToastWidget::contentHeightForWidth(int width)
{
    const auto& t = theme::tokens();
    const int pad = t.panel.padding;
    int reserve = pad * 2 + t.control.icon_button + t.spacing.sm;
    if (action_button_)
        reserve += action_button_->sizeHint().width() + t.spacing.sm;
    const int wrap_w = (std::max)(1, width - reserve);
    const QFontMetricsF fm(message_label_->font());
    const QRectF bounds = fm.boundingRect(QRectF(0, 0, wrap_w, 10000), Qt::TextWordWrap,
        message_label_->text());
    const int content_h = static_cast<int>(std::ceil(bounds.height())) + pad * 2;
    const int wanted = (std::max)(t.panel.view_header_h, content_h);
    const int capped = (std::min)(wanted, t.panel.view_header_h * 3);
    const bool clipped = capped < wanted;
    if (clipped != clipped_) {
        clipped_ = clipped;
        setToolTip(clipped ? message_ : QString());
    }
    return capped;
}

void AidaToastWidget::positionChildren()
{
    const auto& t = theme::tokens();
    const int pad = t.panel.padding;
    int right = width() - pad;
    if (action_button_) {
        const QSize hint = action_button_->sizeHint();
        const int bw = hint.width() + t.spacing.lg;
        const int bh = t.row.compact;
        action_button_->setGeometry(right - bw, (height() - bh) / 2, bw, bh);
        right -= bw + t.spacing.sm;
    }
    if (close_button_) {
        const int cs = toast_constants::k_close_box;
        close_button_->setGeometry(right - cs, pad - t.spacing.xs, cs, cs);
    }
    const int text_x = pad + t.control.icon_button + t.spacing.sm;
    const int text_w = (std::max)(1, right - text_x);
    if (message_label_)
        message_label_->setGeometry(text_x, 0, text_w, height());
}

void AidaToastWidget::resizeEvent(QResizeEvent* event)
{
    QFrame::resizeEvent(event);
    positionChildren();
}

void AidaToastWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    if (opacity_ <= 0.001)
        return;
    const auto& t = theme::tokens();
    const bool is_dark = widgets::relative_luminance(t.bg_base) < 0.5;
    const double a = opacity_;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF face = rect();
    const qreal rounding = t.radius.modal;

    QColor glass = widgets::with_alpha(t.bg_elevated, a * 0.96);
    p.setPen(Qt::NoPen);
    p.setBrush(glass);
    p.drawRoundedRect(face, rounding, rounding);

    const QColor overlay = widgets::with_alpha(t.bg_overlay, a * (is_dark ? 0.16 : 0.10));
    p.setBrush(overlay);
    p.drawRoundedRect(face, rounding, rounding);

    const QColor wash = widgets::with_alpha(toast_severity_soft(type_),
        a * toast_wash_alpha(type_, is_dark));
    p.setBrush(wash);
    p.drawRoundedRect(face, rounding, rounding);

    const QColor highlight_base = is_dark ? t.text_primary : t.text_dim;
    QRectF hi = face;
    hi.setBottom(face.top() + face.height() * 0.55);
    QLinearGradient hi_grad(face.topLeft(), face.bottomLeft());
    hi_grad.setColorAt(0.0, widgets::with_alpha(highlight_base, a * (is_dark ? 0.10 : 0.06)));
    hi_grad.setColorAt(1.0, widgets::with_alpha(highlight_base, 0.0));
    p.setBrush(hi_grad);
    p.drawRoundedRect(face, rounding, rounding);

    const QColor sev = toast_severity_color(type_);
    QPen border_pen(widgets::with_alpha(sev, a * 0.32), t.panel.border);
    p.setPen(border_pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(face.adjusted(0.5, 0.5, -0.5, -0.5), rounding, rounding);

    if (hasFocus()) {
        const qreal ring_inset = qreal(t.control.focus_ring) + 1.0;
        widgets::paint_focus_ring(p,
            face.adjusted(ring_inset, ring_inset, -ring_inset, -ring_inset),
            rounding, 0.85 * a);
    }

    const qreal stripe_w = t.radius.xs;
    p.setPen(Qt::NoPen);
    p.setBrush(widgets::with_alpha(sev, a * 0.85));
    p.save();
    QPainterPath face_clip;
    face_clip.addRoundedRect(face, rounding, rounding);
    p.setClipPath(face_clip);
    p.drawRect(QRectF(face.left(), face.top(), stripe_w, face.height()));
    p.restore();

    const int pad = t.panel.padding;
    const QPointF icon_center(face.left() + pad + t.control.icon_button * 0.5,
                              face.top() + face.height() * 0.5);
    const qreal ring_radius = t.control.icon_button * 0.5 - 1.0;

    const qreal ring_stroke = t.radius.xs * 0.5;
    p.setBrush(widgets::with_alpha(sev, a * 0.16));
    p.drawEllipse(icon_center, ring_radius - ring_stroke, ring_radius - ring_stroke);

    double progress = 1.0;
    if (duration_sec_ > 0.0001)
        progress = (duration_sec_ - elapsed_) / duration_sec_;
    progress = widgets::clamp01(progress);
    QPen track_pen(widgets::with_alpha(sev, a * 0.18), ring_stroke);
    p.setPen(track_pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(icon_center, ring_radius, ring_radius);
    if (progress > 0.0001) {
        QPen ring_pen(widgets::with_alpha(sev, a), ring_stroke);
        ring_pen.setCapStyle(Qt::RoundCap);
        p.setPen(ring_pen);
        const QRectF ring_rect(icon_center.x() - ring_radius, icon_center.y() - ring_radius,
                               ring_radius * 2.0, ring_radius * 2.0);
        p.drawArc(ring_rect, 90 * 16, static_cast<int>(-progress * 360 * 16));
    }

    const QIcon sev_icon = theme::icons::tinted(toast_severity_icon(type_), sev.rgba(),
        t.control.icon_glyph, devicePixelRatioF());
    const qreal icon_px = t.control.icon_glyph;
    sev_icon.paint(&p, QRectF(icon_center.x() - icon_px * 0.5, icon_center.y() - icon_px * 0.5,
                              icon_px, icon_px).toRect());
}

void AidaToastWidget::focusInEvent(QFocusEvent* event)
{
    QFrame::focusInEvent(event);
    updateHoverState();
}

void AidaToastWidget::keyPressEvent(QKeyEvent* event)
{
    const int key = event->key();
    if (!dismissing_ && !swipe_dismissing_
        && (key == Qt::Key_Escape || key == Qt::Key_Return
            || key == Qt::Key_Enter || key == Qt::Key_Space)) {
        startDismiss();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

void AidaToastWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && !dismissing_ && !swipe_dismissing_) {
        press_inside_ = true;
        dragging_ = false;
        press_pos_ = event->pos();
    }
    QFrame::mousePressEvent(event);
}

void AidaToastWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (press_inside_ && (event->buttons() & Qt::LeftButton)) {
        const int dx = event->pos().x() - press_pos_.x();
        const int dy = event->pos().y() - press_pos_.y();
        if (!dragging_ && dx > 0 && dx > std::abs(dy) && dx > 4)
            dragging_ = true;
        if (dragging_) {
            swipe_offset_ = (std::max)(0, dx);
            if (parentWidget())
                move(parentWidget()->width() - theme::tokens().spacing.xxl - width() +
                         swipe_offset_, y());
        }
    }
    QFrame::mouseMoveEvent(event);
}

void AidaToastWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && press_inside_) {
        if (dragging_) {
            const int threshold_px = toast_constants::k_swipe_dismiss_px;
            const int threshold_pct = static_cast<int>(width() * toast_constants::k_swipe_dismiss_pct);
            const int effective = (std::min)(threshold_px, threshold_pct);
            if (swipe_offset_ > effective) {
                beginSwipeDismiss();
            } else {
                dragging_ = false;
                swipe_offset_ = 0;
                if (theme::AidaMotion::reducedMotion()) {
                    if (parentWidget())
                        move(parentWidget()->width() - theme::tokens().spacing.xxl - width(), y());
                } else {
                    if (swipe_anim_)
                        swipe_anim_->stop();
                    swipe_anim_ = new QVariantAnimation(this);
                    swipe_anim_->setDuration(theme::tokens().motion.standard);
                    swipe_anim_->setStartValue(static_cast<double>(x()));
                    const double target = parentWidget()
                        ? parentWidget()->width() - theme::tokens().spacing.xxl - width()
                        : 0.0;
                    swipe_anim_->setEndValue(target);
                    swipe_anim_->setEasingCurve(theme::easingFor(theme::Ease::OutBack));
                    connect(swipe_anim_, &QVariantAnimation::valueChanged, this,
                            [this](const QVariant& v) { move(static_cast<int>(v.toDouble()), y()); });
                    swipe_anim_->start(QAbstractAnimation::DeleteWhenStopped);
                }
            }
        } else if (!action_button_ && !close_button_->underMouse()) {
            startDismiss();
        }
        press_inside_ = false;
        dragging_ = false;
    }
    QFrame::mouseReleaseEvent(event);
}

void AidaToastWidget::beginSwipeDismiss()
{
    if (swipe_dismissing_ || gone_)
        return;
    swipe_dismissing_ = true;
    dismissing_ = true;
    dragging_ = false;
    if (theme::AidaMotion::reducedMotion()) {
        finishDismiss();
        return;
    }
    double velocity = toast_constants::k_swipe_velocity_px_s;
    if (swipe_offset_ > 30)
        velocity = swipe_offset_ * 6.0 + 600.0;
    const double remaining = parentWidget()
        ? (parentWidget()->width() - x()) : 400.0;
    const int duration_ms = (std::max)(60, static_cast<int>(remaining / velocity * 1000.0));
    if (swipe_anim_)
        swipe_anim_->stop();
    swipe_anim_ = new QVariantAnimation(this);
    swipe_anim_->setDuration(duration_ms);
    swipe_anim_->setStartValue(static_cast<double>(x()));
    swipe_anim_->setEndValue(static_cast<double>(parentWidget()
        ? parentWidget()->width() + theme::tokens().spacing.xxl : x() + 400));
    swipe_anim_->setEasingCurve(theme::easingFor(theme::Ease::InQuad));
    connect(swipe_anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        move(static_cast<int>(v.toDouble()), y());
        opacity_ = widgets::clamp01(opacity_ - 0.04);
        update();
    });
    connect(swipe_anim_, &QVariantAnimation::finished, this, [this] {
        finishDismiss();
    });
    swipe_anim_->start(QAbstractAnimation::DeleteWhenStopped);
}

AidaToastHost::AidaToastHost(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.toast_host"));
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    if (parent) {
        parent->installEventFilter(this);
        setGeometry(parent->rect());
        show();
        raise();
    }
}

AidaToastHost::~AidaToastHost()
{
    if (parentWidget())
        parentWidget()->removeEventFilter(this);
}

bool AidaToastHost::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        restack();
    }
    return QWidget::eventFilter(watched, event);
}

void AidaToastHost::trackToast(AidaToastWidget* toast)
{
    toast->setParent(this);
    toast->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    toasts_.push_back(toast);
    toast->show();
    toast->raise();
    restack();
}

void AidaToastHost::untrackToast(AidaToastWidget* toast)
{
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
        [&](const QPointer<AidaToastWidget>& item) {
            return !item || item.data() == toast;
        }), toasts_.end());
    restack();
}

void AidaToastHost::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (parentWidget())
        setGeometry(parentWidget()->rect());
    restack();
}

void AidaToastHost::restack()
{
    const auto& t = theme::tokens();
    int y_cursor = height() - t.spacing.section;
    int visible = 0;
    for (int i = static_cast<int>(toasts_.size()) - 1; i >= 0; --i) {
        AidaToastWidget* toast = toasts_[static_cast<std::size_t>(i)].data();
        if (!toast)
            continue;
        const int w = (std::min)(toast_constants::k_toast_width,
            (std::max)(1, width() - t.spacing.xxl * 2));
        const int h = toast->contentHeightForWidth(w);
        if (visible >= static_cast<int>(toast_constants::k_max_visible)) {
            toast->setVisible(false);
            continue;
        }
        toast->setVisible(true);
        y_cursor -= h;
        toast->setFixedSize(w, h);
        toast->restackTo(y_cursor);
        y_cursor -= t.spacing.sm;
        ++visible;
    }
}

AidaToastManager& AidaToastManager::instance()
{
    static AidaToastManager* manager = [] {
        auto* created = new AidaToastManager();
        if (QCoreApplication* app = QCoreApplication::instance()) {
            QThread* gui_thread = app->thread();
            if (gui_thread && gui_thread != QThread::currentThread())
                created->moveToThread(gui_thread);
        }
        return created;
    }();
    return *manager;
}

AidaToastManager::AidaToastManager(QObject* parent)
    : QObject(parent)
{
    tick_timer_ = new QTimer(this);
    tick_timer_->setTimerType(Qt::PreciseTimer);
    tick_timer_->setInterval(16);
    connect(tick_timer_, &QTimer::timeout, this, [this] {
        const double dt_raw = static_cast<double>(tick_clock_.nsecsElapsed()) / 1.0e9;
        tick_clock_.restart();
        const double dt = (std::min)((std::max)(dt_raw, 0.0), 0.05);
        for (auto& toast : visible_)
            if (toast)
                toast->tickLifetime(dt);
        const bool any_visible = std::any_of(visible_.begin(), visible_.end(),
            [](const QPointer<AidaToastWidget>& t) { return t && !t->gone(); });
        if (!any_visible)
            tick_timer_->stop();
    });
}

void AidaToastManager::attachHost(AidaToastHost* host)
{
    host_ = host;
    if (host_)
        QMetaObject::invokeMethod(this, &AidaToastManager::drainPending,
                                  Qt::QueuedConnection);
}

bool AidaToastManager::dedupPendingLocked(const QString& message) const
{
    for (const auto& item : pending_)
        if (item.message == message)
            return true;
    return false;
}

bool AidaToastManager::dedupVisible(const QString& message) const
{
    for (const auto& toast : visible_)
        if (toast && !toast->dismissing() && toast->message() == message &&
            toast->elapsedSec() < toast_constants::k_dedup_window_sec)
            return true;
    return false;
}

void AidaToastManager::push(const QString& message, AidaToastType type, double duration_sec)
{
    if (message.isEmpty())
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dedupPendingLocked(message))
            return;
        pending_t item;
        item.message = message;
        item.type = type;
        item.duration_sec = duration_sec;
        pending_.push_back(std::move(item));
        while (pending_.size() > toast_constants::k_pending_cap)
            pending_.pop_front();
    }
    QMetaObject::invokeMethod(this, &AidaToastManager::drainPending, Qt::QueuedConnection);
}

void AidaToastManager::pushWithAction(const QString& message, AidaToastType type,
                                      const QString& action_label,
                                      std::function<void()> callback, double duration_sec)
{
    if (message.isEmpty())
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dedupPendingLocked(message))
            return;
        pending_t item;
        item.message = message;
        item.type = type;
        item.duration_sec = duration_sec;
        item.action_label = action_label;
        item.callback = std::move(callback);
        pending_.push_back(std::move(item));
        while (pending_.size() > toast_constants::k_pending_cap)
            pending_.pop_front();
    }
    QMetaObject::invokeMethod(this, &AidaToastManager::drainPending, Qt::QueuedConnection);
}

void AidaToastManager::drainPending()
{
    std::deque<pending_t> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.swap(pending_);
    }
    for (const auto& item : drained)
        if (!dedupVisible(item.message))
            spawn(item);
}

void AidaToastManager::spawn(const pending_t& item)
{
    if (host_.isNull()) {
        diag::log_tagged_fmt("qt_toast", "toast_dropped_no_host message_len=%lld",
            static_cast<long long>(item.message.size()));
        return;
    }
    visible_.erase(std::remove_if(visible_.begin(), visible_.end(),
        [](const QPointer<AidaToastWidget>& t) { return !t || t->gone(); }), visible_.end());
    auto* toast = new AidaToastWidget(next_id_++, item.message, item.type, item.duration_sec);
    if (!item.action_label.isEmpty())
        toast->setAction(item.action_label, std::move(item.callback));
    connect(toast, &AidaToastWidget::dismissFinished, this,
            [this](std::uint64_t id) { onToastGone(id); });
    visible_.push_back(toast);
    host_->trackToast(toast);
    if (!tick_timer_->isActive()) {
        tick_clock_.restart();
        tick_timer_->start();
    }
}

void AidaToastManager::onToastGone(std::uint64_t id)
{
    AidaToastWidget* gone_toast = nullptr;
    for (const auto& toast : visible_)
        if (toast && toast->toastId() == id) {
            gone_toast = toast.data();
            break;
        }
    visible_.erase(std::remove_if(visible_.begin(), visible_.end(),
        [&](const QPointer<AidaToastWidget>& t) { return !t || t->toastId() == id; }),
        visible_.end());
    if (host_) {
        if (gone_toast)
            host_->untrackToast(gone_toast);
        else
            host_->restack();
    }
}

void toast_info(const QString& message, double duration_sec)
{
    AidaToastManager::instance().push(message, AidaToastType::info, duration_sec);
}

void toast_success(const QString& message, double duration_sec)
{
    AidaToastManager::instance().push(message, AidaToastType::success, duration_sec);
}

void toast_warning(const QString& message, double duration_sec)
{
    AidaToastManager::instance().push(message, AidaToastType::warning, duration_sec);
}

void toast_error(const QString& message, double duration_sec)
{
    AidaToastManager::instance().push(message, AidaToastType::error, duration_sec);
}

}
