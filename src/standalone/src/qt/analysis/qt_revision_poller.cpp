#include "qt/analysis/qt_revision_poller.hpp"

#include "qt/analysis/qt_workspace_context.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/session/analysis_session.hpp"
#include "core/analysis/symbol_store.hpp"

namespace aida::qt::analysis {

QtRevisionPoller::QtRevisionPoller(QtWorkspaceContext* parent)
    : QObject(parent) {
    timer_.setInterval(33);
    timer_.setTimerType(Qt::CoarseTimer);
    connect(&timer_, &QTimer::timeout, this, &QtRevisionPoller::pollNow);
}

void QtRevisionPoller::arm() {
    if (!timer_.isActive() && !close_emitted_)
        timer_.start();
}

void QtRevisionPoller::disarm() {
    timer_.stop();
}

void QtRevisionPoller::pollNow() {
    auto* context = qobject_cast<QtWorkspaceContext*>(parent());
    if (!context) {
        disarm();
        return;
    }
    const auto workspace = context->workspace().lock();
    if (!workspace || workspace->closed()) {
        disarm();
        if (!close_emitted_) {
            close_emitted_ = true;
            Q_EMIT workspaceClosed();
        }
        return;
    }
    revision_tuple_t current;
    current.generation = workspace->generation();
    current.analysis = workspace->analysis_revision();
    current.overlay = workspace->overlay_revision();
    const auto symbols = analysis_session::symbols_for_workspace(workspace);
    current.symbol = symbols ? symbols->revision() : 0;
    if (current != last_) {
        last_ = current;
        Q_EMIT revisionsChanged(current.generation, current.analysis,
                                current.overlay, current.symbol);
    }
}

}
