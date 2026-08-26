#include "qt/analysis_bridge/revision_poller.hpp"

#include "qt/analysis_bridge/revision_combine.hpp"

#include <QTimer>

namespace aida::qt::analysis_bridge {

AidaRevisionPoller::AidaRevisionPoller(QObject* parent) : QObject(parent)
{
    timer_ = new QTimer(this);
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, &AidaRevisionPoller::tick);
}

AidaRevisionPoller::~AidaRevisionPoller() = default;

void AidaRevisionPoller::set_source(std::function<revision_sample_t()> source)
{
    source_ = std::move(source);
    has_last_ = false;
}

void AidaRevisionPoller::set_polling(bool polling)
{
    if (polling_ == polling)
        return;
    polling_ = polling;
    if (polling_) {
        poll_now();
        timer_->start();
    } else {
        timer_->stop();
    }
}

void AidaRevisionPoller::poll_now()
{
    if (source_)
        tick();
}

void AidaRevisionPoller::tick()
{
    if (!source_)
        return;
    const revision_sample_t sample = source_();
    if (!sample.valid) {
        if (has_last_) {
            has_last_ = false;
            Q_EMIT sourceInvalidated();
        }
        return;
    }
    if (!has_last_) {
        last_ = sample;
        has_last_ = true;
        Q_EMIT revisionChanged(
            aida::analysis_bridge::combine_generation_revision(sample.generation,
                sample.analysis_revision),
            sample.overlay_revision);
        if (sample.ui_serial != 0)
            Q_EMIT uiSerialChanged(sample.ui_serial);
        return;
    }
    if (sample.workspace != last_.workspace ||
        sample.generation != last_.generation ||
        sample.analysis_revision != last_.analysis_revision ||
        sample.overlay_revision != last_.overlay_revision ||
        sample.view_revision != last_.view_revision) {
        last_ = sample;
        Q_EMIT revisionChanged(
            aida::analysis_bridge::combine_generation_revision(sample.generation,
                sample.analysis_revision),
            sample.overlay_revision);
        return;
    }
    if (sample.ui_serial != last_.ui_serial) {
        last_ = sample;
        Q_EMIT uiSerialChanged(sample.ui_serial);
    }
}

}
