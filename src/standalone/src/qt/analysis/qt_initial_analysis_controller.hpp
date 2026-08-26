#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <string>

#include "core/analysis/decompiler/decompile_batch_orchestrator.hpp"
#include "core/analysis/workspace/analysis_workspace.hpp"

namespace aida::qt::analysis {

class QtWorkspaceContext;
class QtAnalysisProgressDialog;
class QtPdbPromptDialog;
class QtPdbStatusDialog;
class QtBatchDecompileDialog;

// Per-binary initial-analysis state (07 sec. 7.7); replaces
// initial_analysis_view::states().
struct QtInitialAnalysisState {
    bool dismissed = false;
    bool load_types = true;
    bool load_names = true;
    std::uint64_t generation = 0;
    QString local_pdb_path;
    std::string analysis_error;
    std::string pdb_error;
    std::uint64_t batch_last_update_ms = 0;
    bool batch_task_registered = false;
    std::string batch_task_id;
    std::uint64_t batch_generation = 0;
    bool batch_cancel_requested = false;
    aida::analysis::decompile_batch_orchestrator_t::run_snapshot_t batch_snapshot{};
};

// Drives the four transient initial-analysis surfaces for one workspace
// (07 sec. 7.7): progress dialog, remote/local PDB prompts, PDB status, and the
// background-decompile dialog. All are non-modal QDialogs owned here except
// the modal PDB prompts.
class QtInitialAnalysisController : public QObject {
    Q_OBJECT
public:
    QtInitialAnalysisController(QtWorkspaceContext* context, QWidget* dialog_parent,
                                QObject* parent = nullptr);
    ~QtInitialAnalysisController() override;

    void poll();
    void shutdownDialogs();

private:
    void syncProgressDialog();
    void syncPdbDialogs();
    void syncBatchDialog();
    void syncBatchTask();

    QtWorkspaceContext* context_ = nullptr;
    QWidget* dialog_parent_ = nullptr;
    QtAnalysisProgressDialog* progress_dialog_ = nullptr;
    QtPdbPromptDialog* remote_pdb_dialog_ = nullptr;
    QtPdbPromptDialog* local_pdb_dialog_ = nullptr;
    QtPdbStatusDialog* pdb_status_dialog_ = nullptr;
    QtBatchDecompileDialog* batch_dialog_ = nullptr;
    std::uint64_t observed_generation_ = 0;
};

}
