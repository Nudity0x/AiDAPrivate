#pragma once

#include <QObject>
#include <QString>

class QTimer;

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "qt/analysis/qt_analysis_list_model.hpp"
#include "qt/analysis/qt_binary_map_types.hpp"
#include "qt/analysis/qt_functions_model.hpp"
#include "qt/analysis/qt_proximity_model.hpp"
#include "qt/analysis/qt_segment_registers_model.hpp"
#include "qt/analysis/qt_xref_model.hpp"

namespace aida::analysis {
class analysis_workspace_t;
}

namespace aida::qt::analysis {

class QtInitialAnalysisController;
class QtRevisionPoller;

// Forward-declared view state for the types catalog (defined in
// qt_types_catalog_model.hpp) and initial-analysis (qt_initial_analysis_controller.hpp).
struct QtTypesHubState;
struct QtInitialAnalysisState;

// One context per binary_id (07 sec. 1.3). Replaces every function-static
// unordered_map<binary_id, state> registry in the ImGui analysis views.
// Owned by QtAnalysisBridge; destroyed on workspace close (16-cap LRU).
class QtWorkspaceContext : public QObject {
    Q_OBJECT
public:
    QtWorkspaceContext(std::shared_ptr<aida::analysis::analysis_workspace_t> workspace,
                       QObject* parent = nullptr);
    ~QtWorkspaceContext() override;

    const std::weak_ptr<aida::analysis::analysis_workspace_t>& workspace() const noexcept {
        return workspace_;
    }
    QString binaryIdHex() const noexcept { return binary_id_hex_; }
    QtRevisionPoller* poller() const noexcept { return poller_; }

    // Per-binary view states. Worker-captured states are shared_ptr (in-flight
    // workers keep them alive after eviction); GUI-only states are direct.
    QtAnalysisListState listState[6];
    std::shared_ptr<QtFunctionsPanelState> functionsState =
        std::make_shared<QtFunctionsPanelState>();
    QtSegmentRegistersState segmentRegistersState;
    std::shared_ptr<struct QtProximityState> proximityState;
    std::shared_ptr<QtXrefViewState> xrefState =
        std::make_shared<QtXrefViewState>();
    std::shared_ptr<QtBinaryMapViewState> binaryMapState;
    std::shared_ptr<QtTypesHubState> typesHubState;
    std::shared_ptr<QtInitialAnalysisState> initialAnalysisState;

    // The per-binary initial-analysis dialog driver (07 sec. 7.7), driven by a
    // 250 ms timer owned by the context.
    QtInitialAnalysisController* initialAnalysis() const noexcept {
        return initial_analysis_;
    }

    // Per-binary hub tab state (replaces the per-binary sub-tab state that
    // analysis_hub_view / types_hub_view kept in function-static registries).
    // QtAnalysisBridge mirrors these into its effective atomics for Qt-free
    // callers; stealth stays bridge-global (old stealth_view state was a
    // function-static, not per-binary).
    std::atomic<int> analysis_hub_tab{0};
    std::atomic<int> types_hub_tab{0};

    // LRU touch (07 sec. 1.3, precedent workbench_registry_views.hpp:637-656).
    std::uint64_t last_touch = 0;

private:
    std::weak_ptr<aida::analysis::analysis_workspace_t> workspace_;
    QString binary_id_hex_;
    QtRevisionPoller* poller_ = nullptr;
    QtInitialAnalysisController* initial_analysis_ = nullptr;
    QTimer* initial_analysis_timer_ = nullptr;
};

}
