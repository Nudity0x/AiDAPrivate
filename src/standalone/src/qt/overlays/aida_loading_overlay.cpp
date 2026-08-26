#include "qt/overlays/aida_loading_overlay.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>

#include "core/session/analysis_session.hpp"
#include "helpers/diag_log.hpp"
#include "qt/overlays/aida_overlay_host.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_progress.hpp"

namespace aida::qt::overlays {

namespace {

QString title_for(loading_binary_overlay::phase_t phase, bool failed, bool cancelled,
                  bool pdb_loading)
{
    if (cancelled) return QStringLiteral("Analysis cancelled");
    if (failed) return QStringLiteral("Analysis failed");
    if (pdb_loading) return QStringLiteral("Loading debug symbols");
    switch (phase) {
    case loading_binary_overlay::phase_t::awaiting_pdb_decision:
        return QStringLiteral("Debug symbols require a decision");
    case loading_binary_overlay::phase_t::loading:
        return QStringLiteral("Loading binary");
    case loading_binary_overlay::phase_t::finalizing:
        return QStringLiteral("Finalizing analysis");
    default:
        return QStringLiteral("Analyzing binary");
    }
}

}

AidaLoadingOverlay::AidaLoadingOverlay(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.loading_overlay"));
    setAutoFillBackground(false);

    card_ = new QWidget(this);
    card_->setObjectName(QStringLiteral("aida.loading_overlay.card"));
    card_->setProperty("aidaRole", QStringLiteral("loading_card"));
    card_->setAttribute(Qt::WA_StyledBackground, true);

    auto* card_layout = new QVBoxLayout(card_);
    const auto& t = theme::tokens();
    card_layout->setContentsMargins(t.panel.padding * 2, t.panel.padding * 2,
                                    t.panel.padding * 2, t.panel.padding * 2);
    card_layout->setSpacing(t.spacing.sm);

    title_label_ = new QLabel(card_);
    title_label_->setObjectName(QStringLiteral("aida.loading_overlay.title"));
    title_label_->setFont(theme::fonts::h2());
    card_layout->addWidget(title_label_);

    target_label_ = new QLabel(card_);
    target_label_->setObjectName(QStringLiteral("aida.loading_overlay.target"));
    target_label_->setFont(theme::fonts::caption());
    card_layout->addWidget(target_label_);

    status_label_ = new QLabel(card_);
    status_label_->setObjectName(QStringLiteral("aida.loading_overlay.status"));
    status_label_->setFont(theme::fonts::body());
    status_label_->setWordWrap(true);
    card_layout->addWidget(status_label_);

    progress_ = new widgets::AidaProgressBar(card_);
    progress_->setObjectName(QStringLiteral("aida.loading_overlay.progress"));
    card_layout->addWidget(progress_);

    elapsed_label_ = new QLabel(card_);
    elapsed_label_->setObjectName(QStringLiteral("aida.loading_overlay.elapsed"));
    elapsed_label_->setFont(theme::fonts::caption());
    card_layout->addWidget(elapsed_label_);

    auto* button_row = new QHBoxLayout();
    button_row->addStretch(1);
    cancel_button_ = new widgets::AidaButton(QStringLiteral("Cancel Analysis"), card_);
    cancel_button_->setObjectName(QStringLiteral("aida.loading_overlay.cancel"));
    cancel_button_->setKind(widgets::AidaButton::Kind::Destructive);
    cancel_button_->setToolTip(QStringLiteral("Request cancellation from the active analysis owner"));
    connect(cancel_button_, &widgets::AidaButton::clicked, this, [this] {
        Q_EMIT cancelRequested();
    });
    button_row->addWidget(cancel_button_);
    details_button_ = new widgets::AidaButton(QStringLiteral("View Details"), card_);
    details_button_->setObjectName(QStringLiteral("aida.loading_overlay.details"));
    details_button_->setKind(widgets::AidaButton::Kind::Secondary);
    details_button_->setToolTip(QStringLiteral("Open persistent diagnostics for this failure"));
    connect(details_button_, &widgets::AidaButton::clicked, this, [this] {
        Q_EMIT detailsRequested();
    });
    button_row->addWidget(details_button_);
    card_layout->addLayout(button_row);

    layoutCard();
}

void AidaLoadingOverlay::layoutCard()
{
    if (!card_)
        return;
    const auto& t = theme::tokens();
    const int margin = t.panel.overlay_margin;
    const int avail_w = (std::max)(1, width() - 2 * margin);
    const int avail_h = (std::max)(1, height() - 2 * margin);
    const int card_w = (std::max)(1, (std::min)(t.grid * 155, avail_w));
    int wanted_h = 0;
    if (QLayout* card_layout = card_->layout()) {
        wanted_h = card_layout->hasHeightForWidth()
            ? card_layout->totalHeightForWidth(card_w)
            : card_layout->sizeHint().height();
    }
    if (wanted_h <= 0)
        wanted_h = t.grid * 75;
    const int card_h = (std::max)(1, (std::min)(wanted_h, avail_h));
    card_->setGeometry((width() - card_w) / 2, (height() - card_h) / 2, card_w, card_h);
}

void AidaLoadingOverlay::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutCard();
}

void AidaLoadingOverlay::showProgress(const QString& title, const QString& label,
                                      const QString& target, const QString& stage,
                                      qreal progress, double elapsed_seconds,
                                      bool cancellable, bool failed, qreal alpha)
{
    alpha_ = alpha;
    title_label_->setText(title);
    target_label_->setText(target);
    status_label_->setText(label);
    progress_->setIndeterminate(progress < 0.0);
    if (progress >= 0.0)
        progress_->setProgress(progress);
    const qint64 total_seconds = static_cast<qint64>(elapsed_seconds);
    elapsed_label_->setText(QStringLiteral("%1 · %2:%3")
        .arg(stage,
             QString::number(total_seconds / 60),
             QString::number(total_seconds % 60).rightJustified(2, QLatin1Char('0'))));
    cancel_button_->setVisible(cancellable && !failed);
    details_button_->setVisible(failed);
    layoutCard();
    update();
}

void AidaLoadingOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter p(this);
    const auto& t = theme::tokens();
    p.setPen(Qt::NoPen);
    p.setBrush(widgets::with_alpha(t.bg_base, alpha_ * 0.59));
    p.drawRect(rect());
}

AidaLoadingOverlayController::AidaLoadingOverlayController(QObject* parent)
    : QObject(parent)
{
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(100);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, [this] { tick(); });
    poll_timer_->start();
    tick_clock_.start();
}

AidaLoadingOverlayController::~AidaLoadingOverlayController() = default;

void AidaLoadingOverlayController::bind(AidaOverlayHost* host)
{
    host_ = host;
    if (host_)
        connect(host_, &AidaOverlayHost::cancelRequested, this,
                [this] { onCancel(); });
}

void AidaLoadingOverlayController::setViewFocusHook(
    std::function<void(const char* view_id)> hook)
{
    view_focus_hook_ = std::move(hook);
}

void AidaLoadingOverlayController::tick()
{
    const double dt_raw = static_cast<double>(tick_clock_.nsecsElapsed()) / 1.0e9;
    tick_clock_.restart();
    const float dt = static_cast<float>((std::min)((std::max)(dt_raw, 0.0), 0.05));

    loading_binary_overlay::poll_completion();

    const size_t active = analysis_session::active_session_idx();
    const auto summary = active == static_cast<size_t>(-1)
        ? analysis_session::session_summary_t{}
        : analysis_session::summarize_session_at(active);
    auto state = loading_binary_overlay::detail::selected_state();
    if (!state) {
        if (overlay_visible_)
            dismiss();
        return;
    }

    const bool visible = !summary.id.empty() &&
        ((summary.load_state != analysis_session::session_load_state_t::ready &&
          summary.load_state != analysis_session::session_load_state_t::closed) ||
         summary.pdb_loading);

    float alpha = state->visual_alpha.load(std::memory_order_acquire);
    if (theme::AidaMotion::reducedMotion()) {
        alpha = visible ? 1.f : 0.f;
    } else {
        alpha += ((visible ? 1.f : 0.f) - alpha) * (std::min)(dt * 14.f, 1.f);
        if (std::fabs(alpha - (visible ? 1.f : 0.f)) < 0.003f)
            alpha = visible ? 1.f : 0.f;
    }
    state->visual_alpha.store(alpha, std::memory_order_release);

    if (alpha < 0.005f) {
        if (overlay_visible_)
            dismiss();
        return;
    }

    const float target_progress = loading_binary_overlay::detail::progress_for(summary);
    float progress = state->visual_progress.load(std::memory_order_acquire);
    if (target_progress >= 0.f) {
        if (progress < 0.f)
            progress = target_progress;
        else
            progress += (target_progress - progress) * (std::min)(dt * 9.f, 1.f);
    } else {
        progress = -1.f;
    }
    state->visual_progress.store(progress, std::memory_order_release);

    const bool failed = summary.load_state == analysis_session::session_load_state_t::failed;
    const bool cancelled = summary.error && summary.error->cancellation;
    const loading_binary_overlay::phase_t phase = loading_binary_overlay::current_phase();
    const bool cancellation_latched =
        state->cancellation_requested.load(std::memory_order_acquire);
    const bool owner_cancellable = !failed && !cancellation_latched &&
        (phase == loading_binary_overlay::phase_t::loading ||
         phase == loading_binary_overlay::phase_t::awaiting_analysis ||
         phase == loading_binary_overlay::phase_t::loading_pdb);

    QString label = cancellation_latched
        ? QStringLiteral("Cancellation requested; waiting for the analysis owner to confirm a terminal state.")
        : QString::fromStdString(loading_binary_overlay::detail::label_for(summary));

    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - state->tracked_at).count();

    if (!overlay_visible_)
        present();
    if (overlay_) {
        if (host_)
            host_->setCancelable(owner_cancellable);
        overlay_->showProgress(
            title_for(phase, failed, cancelled, summary.pdb_loading),
            label,
            QString::fromStdString(state->filename),
            QString::fromLatin1(loading_binary_overlay::detail::phase_name(phase)),
            static_cast<qreal>(progress), elapsed, owner_cancellable, failed,
            static_cast<qreal>(alpha));
    }
}

void AidaLoadingOverlayController::present()
{
    if (!host_ || overlay_visible_)
        return;
    overlay_ = new AidaLoadingOverlay();
    connect(overlay_, &AidaLoadingOverlay::cancelRequested, this,
            [this] { onCancel(); });
    connect(overlay_, &AidaLoadingOverlay::detailsRequested, this,
            [this] { onDetails(); });
    overlay_visible_ = true;
    host_->present(overlay_);
    Q_EMIT overlayShown();
    loading_binary_overlay::log_state("qt_overlay_present");
}

void AidaLoadingOverlayController::dismiss()
{
    if (!overlay_visible_)
        return;
    overlay_visible_ = false;
    if (host_)
        host_->dismiss();
    overlay_ = nullptr;
    Q_EMIT overlayHidden();
    loading_binary_overlay::log_state("qt_overlay_dismissed");
}

void AidaLoadingOverlayController::onCancel()
{
    const size_t active = analysis_session::active_session_idx();
    if (active == static_cast<size_t>(-1))
        return;
    const auto summary = analysis_session::summarize_session_at(active);
    const bool failed = summary.load_state == analysis_session::session_load_state_t::failed;
    const auto phase = loading_binary_overlay::current_phase();
    auto state = loading_binary_overlay::detail::selected_state();
    const bool owner_cancellable = !failed && state &&
        !state->cancellation_requested.load(std::memory_order_acquire) &&
        (phase == loading_binary_overlay::phase_t::loading ||
         phase == loading_binary_overlay::phase_t::awaiting_analysis ||
         phase == loading_binary_overlay::phase_t::loading_pdb);
    if (!owner_cancellable || !state)
        return;
    if (loading_binary_overlay::cancel_queued_load("human_overlay"))
        state->cancellation_requested.store(true, std::memory_order_release);
}

void AidaLoadingOverlayController::onDetails()
{
    diag::log_tagged_fmt("loading_binary_overlay", "qt_overlay_view_details view=view.diagnostics");
    if (view_focus_hook_)
        view_focus_hook_("view.diagnostics");
}

}
