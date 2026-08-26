#include "qt/network/reviewed_context_banner.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"

namespace aida::qt::net {

namespace {
constexpr qint64 k_revalidate_gate_ms = 2000;
}

ReviewedContextBanner::ReviewedContextBanner(QWidget* parent)
    : QFrame(parent) {
    setFrameShape(QFrame::StyledPanel);
    setProperty("aidaRole", QStringLiteral("notice"));
    setProperty("aidaVariant", QStringLiteral("success"));

    auto* layout = new QVBoxLayout(this);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding_compact, t.panel.padding_compact,
        t.panel.padding_compact, t.panel.padding_compact);
    layout->setSpacing(t.spacing.xs);

    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(t.spacing.sm);
    status_label_ = new QLabel(this);
    topRow->addWidget(status_label_);
    recheck_button_ = new widgets::AidaButton(QStringLiteral("Recheck"), this);
    recheck_button_->setKind(widgets::AidaButton::Kind::Ghost);
    recheck_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    recheck_button_->setToolTip(QStringLiteral("Revalidate the reviewed source now"));
    topRow->addWidget(recheck_button_);
    clear_button_ = new widgets::AidaButton(QStringLiteral("Clear"), this);
    clear_button_->setKind(widgets::AidaButton::Kind::Ghost);
    clear_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    clear_button_->setToolTip(QStringLiteral("Dismiss the reviewed source banner"));
    topRow->addWidget(clear_button_);
    label_text_ = new QLabel(this);
    topRow->addWidget(label_text_, 1);
    layout->addLayout(topRow);

    identity_label_ = new QLabel(this);
    identity_label_->setProperty("aidaTone", QStringLiteral("dim"));
    layout->addWidget(identity_label_);

    reason_label_ = new QLabel(this);
    reason_label_->setProperty("aidaTone", QStringLiteral("dim"));
    reason_label_->setWordWrap(true);
    reason_label_->setVisible(false);
    layout->addWidget(reason_label_);

    connect(recheck_button_, &QAbstractButton::clicked, this, [this] { revalidate(true); });
    connect(clear_button_, &QAbstractButton::clicked, this, [this] { clearContext(); });

    timer_ = new QTimer(this);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, [this] { revalidate(false); });

    setVisible(false);
}

void ReviewedContextBanner::setContext(const network_view::artifact_identity_t& identity) {
    context_ = identity;
    current_ = true;
    reason_.clear();
    validation_clock_.restart();
    setVisible(true);
    refreshLabels();
}

void ReviewedContextBanner::clearContext() {
    context_ = network_view::artifact_identity_t{};
    current_ = false;
    reason_.clear();
    setVisible(false);
    Q_EMIT cleared();
}

void ReviewedContextBanner::revalidateNow() {
    revalidate(true);
}

void ReviewedContextBanner::showEvent(QShowEvent* event) {
    QFrame::showEvent(event);
    if (hasContext())
        timer_->start();
}

void ReviewedContextBanner::hideEvent(QHideEvent* event) {
    timer_->stop();
    QFrame::hideEvent(event);
}

void ReviewedContextBanner::revalidate(bool force) {
    if (!hasContext())
        return;
    if (!force) {
        if (!validation_clock_.isValid() || validation_clock_.elapsed() < k_revalidate_gate_ms)
            return;
    }
    network_view::artifact_snapshot_t snapshot;
    std::string reason;
    const bool ok = network_view::resolve_artifact(context_, snapshot, reason);
    current_ = ok;
    reason_ = ok ? QString()
        : reason.empty() ? QStringLiteral("The retained source is stale.")
                         : QString::fromStdString(reason);
    validation_clock_.restart();
    refreshLabels();
    Q_EMIT revalidated(current_);
}

void ReviewedContextBanner::refreshLabels() {
    status_label_->setText(current_ ? current_text_ : stale_text_);
    set_label_tone(status_label_, current_ ? "titleSuccess" : "titleError");
    if (property("aidaVariant") != (current_ ? QStringLiteral("success") : QStringLiteral("error"))) {
        setProperty("aidaVariant", current_ ? QStringLiteral("success") : QStringLiteral("error"));
        theme::stylesheet::repolish(this);
    }
    label_text_->setText(context_.label.empty()
        ? QString::fromStdString(context_.id)
        : QString::fromStdString(context_.label));
    identity_label_->setText(QStringLiteral("%1://%2:%3 | rev %4 | hash %5 | %6 bytes%7")
        .arg(context_.use_tls ? QStringLiteral("https") : QStringLiteral("http"))
        .arg(QString::fromStdString(context_.target_host))
        .arg(static_cast<unsigned>(context_.target_port))
        .arg(static_cast<unsigned long long>(context_.revision))
        .arg(QString::number(static_cast<unsigned long long>(context_.content_hash), 16)
            .toUpper().rightJustified(16, QLatin1Char('0')))
        .arg(context_.content_size)
        .arg(identity_suffix_));
    reason_label_->setText(reason_);
    reason_label_->setVisible(!current_ && !reason_.isEmpty());
}

}
