#include "qt/analysis/qt_workspace_context.hpp"

#include <QTimer>

#include "qt/analysis/qt_binary_map_shared.hpp"
#include "qt/analysis/qt_initial_analysis_controller.hpp"
#include "qt/analysis/qt_revision_poller.hpp"
#include "qt/analysis/qt_types_catalog_model.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"

namespace aida::qt::analysis {

QtWorkspaceContext::QtWorkspaceContext(
    std::shared_ptr<aida::analysis::analysis_workspace_t> workspace, QObject* parent)
    : QObject(parent), workspace_(workspace) {
    binary_id_hex_ = workspace
        ? QString::fromStdString(workspace->identity().binary_id().to_hex())
        : QStringLiteral("none");
    poller_ = new QtRevisionPoller(this);
    proximityState = std::make_shared<QtProximityState>();
    binaryMapState = std::make_shared<QtBinaryMapViewState>();
    binaryMapState->workspace = workspace;
    binaryMapState->binary_id = binary_id_hex_.toStdString();
    bm_install_event_subscriptions(binaryMapState);
    typesHubState = std::make_shared<QtTypesHubState>();
    initialAnalysisState = std::make_shared<QtInitialAnalysisState>();
    initial_analysis_ = new QtInitialAnalysisController(this, nullptr, this);
    initial_analysis_timer_ = new QTimer(this);
    initial_analysis_timer_->setInterval(250);
    initial_analysis_timer_->setTimerType(Qt::CoarseTimer);
    connect(initial_analysis_timer_, &QTimer::timeout, this,
            [this] { initial_analysis_->poll(); });
    initial_analysis_timer_->start();
}

QtWorkspaceContext::~QtWorkspaceContext() = default;

}
