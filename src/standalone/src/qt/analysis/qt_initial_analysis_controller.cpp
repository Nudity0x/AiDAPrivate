#include "qt/analysis/qt_initial_analysis_controller.hpp"

#include <QCheckBox>
#include <QTimer>

#include <chrono>

#include "helpers/diag_log.hpp"

#include "core/analysis/initial_analysis.hpp"
#include "core/analysis/symbol_store.hpp"
#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/session/analysis_session.hpp"
#include "core/ui/task_center.hpp"

#include "qt/analysis/qt_initial_analysis_dialogs.hpp"
#include "qt/analysis/qt_workspace_context.hpp"

namespace aida::qt::analysis {

QtInitialAnalysisController::QtInitialAnalysisController(
    QtWorkspaceContext* context, QWidget* dialog_parent, QObject* parent)
    : QObject(parent), context_(context), dialog_parent_(dialog_parent) {}

QtInitialAnalysisController::~QtInitialAnalysisController() {
    shutdownDialogs();
}

void QtInitialAnalysisController::shutdownDialogs() {
    if (progress_dialog_) progress_dialog_->deleteLater();
    if (remote_pdb_dialog_) remote_pdb_dialog_->deleteLater();
    if (local_pdb_dialog_) local_pdb_dialog_->deleteLater();
    if (pdb_status_dialog_) pdb_status_dialog_->deleteLater();
    if (batch_dialog_) batch_dialog_->deleteLater();
    progress_dialog_ = nullptr;
    remote_pdb_dialog_ = nullptr;
    local_pdb_dialog_ = nullptr;
    pdb_status_dialog_ = nullptr;
    batch_dialog_ = nullptr;
}

void QtInitialAnalysisController::poll() {
    if (!context_) return;
    const auto workspace = context_->workspace().lock();
    if (!workspace) {
        shutdownDialogs();
        return;
    }
    auto& state = *context_->initialAnalysisState;
    if (state.generation != workspace->generation()) {
        state.generation = workspace->generation();
        state.dismissed = false;
        state.load_types = true;
        state.load_names = true;
        state.local_pdb_path.clear();
        state.analysis_error.clear();
        state.pdb_error.clear();
    }
    syncProgressDialog();
    syncPdbDialogs();
    syncBatchTask();
    syncBatchDialog();
}

void QtInitialAnalysisController::syncProgressDialog() {
    const auto workspace = context_->workspace().lock();
    auto& state = *context_->initialAnalysisState;
    const auto progress = workspace->progress();
    using aida::analysis::workspace_readiness_t;
    const bool completed_without_error = progress.total_units != 0 &&
        progress.completed_units >= progress.total_units && !progress.error;
    const bool visible =
        progress.readiness == workspace_readiness_t::analyzing ||
        progress.readiness == workspace_readiness_t::cancelling ||
        progress.readiness == workspace_readiness_t::failed ||
        (progress.readiness == workspace_readiness_t::partial &&
            !completed_without_error);
    if (!visible || state.dismissed) {
        if (progress_dialog_) progress_dialog_->hide();
        return;
    }
    if (!progress_dialog_) {
        progress_dialog_ = new QtAnalysisProgressDialog(dialog_parent_);
        progress_dialog_->adoptWorkspace(workspace);
        connect(progress_dialog_, &QtAnalysisProgressDialog::retryRequested, this,
                [this] {
            auto& s = *context_->initialAnalysisState;
            const auto ws = context_->workspace().lock();
            if (!ws) return;
            s.dismissed = false;
            const auto started = initial_analysis::run_initial_analysis(ws);
            if (started) {
                s.analysis_error.clear();
            } else {
                s.analysis_error = started.error().stable_code() + ": " +
                    started.error().message;
            }
        });
        connect(progress_dialog_, &QtAnalysisProgressDialog::dismissed, this,
                [this] {
            context_->initialAnalysisState->dismissed = true;
        });
    }
    progress_dialog_->poll();
    if (!state.analysis_error.empty()) {
        // Surface the retained analysis error under the engine error slot.
    }
    progress_dialog_->show();
}

void QtInitialAnalysisController::syncPdbDialogs() {
    const auto workspace = context_->workspace().lock();
    auto& state = *context_->initialAnalysisState;
    if (workspace->target_kind() != aida::analysis::target_kind_t::static_file ||
        workspace->closing() || workspace->closed()) {
        if (remote_pdb_dialog_) remote_pdb_dialog_->hide();
        if (local_pdb_dialog_) local_pdb_dialog_->hide();
        if (pdb_status_dialog_) pdb_status_dialog_->hide();
        return;
    }
    // suppress_automated_prompts (verbatim): auto-decline under pdb_skip_active.
    const auto automation = symbol_store::pdb_automation_context();
    if (automation.pdb_skip_active) {
        const auto snapshot = analysis_session::pdb_prompt_snapshot(workspace);
        if (snapshot) {
            if (snapshot.value().remote_pending)
                static_cast<void>(analysis_session::decline_remote_pdb(workspace));
            if (snapshot.value().local_pending)
                static_cast<void>(analysis_session::decline_local_pdb(workspace));
        }
        if (remote_pdb_dialog_) remote_pdb_dialog_->hide();
        if (local_pdb_dialog_) local_pdb_dialog_->hide();
    }
    const auto prompt = analysis_session::pdb_prompt_snapshot(workspace);
    if (!prompt) {
        if (remote_pdb_dialog_) remote_pdb_dialog_->hide();
        if (local_pdb_dialog_) local_pdb_dialog_->hide();
        if (pdb_status_dialog_) pdb_status_dialog_->hide();
        return;
    }
    const auto& value = prompt.value();
    if (value.remote_pending && !automation.pdb_skip_active) {
        if (!remote_pdb_dialog_) {
            remote_pdb_dialog_ = new QtPdbPromptDialog(
                QtPdbPromptDialog::Mode::remote, dialog_parent_);
            remote_pdb_dialog_->setAttribute(Qt::WA_DeleteOnClose, false);
            connect(remote_pdb_dialog_, &QtPdbPromptDialog::decided, this,
                    [this](bool approved, bool load_types, bool load_names,
                           const QString&) {
                auto& s = *context_->initialAnalysisState;
                const auto ws = context_->workspace().lock();
                if (!ws) return;
                const auto result = approved
                    ? analysis_session::approve_remote_pdb(ws, load_types, load_names)
                    : analysis_session::decline_remote_pdb(ws);
                if (result) {
                    s.pdb_error.clear();
                    remote_pdb_dialog_->hide();
                } else {
                    s.pdb_error = result.error().stable_code() + ": " +
                        result.error().message;
                }
            });
        }
        remote_pdb_dialog_->adoptWorkspace(workspace, value, state.load_types,
            state.load_names);
        state.load_types = remote_pdb_dialog_->findChild<QCheckBox*>() != nullptr
            ? state.load_types : state.load_types;
        remote_pdb_dialog_->open();
    } else if (remote_pdb_dialog_) {
        remote_pdb_dialog_->hide();
    }
    if (value.local_pending && !automation.pdb_skip_active) {
        if (!local_pdb_dialog_) {
            local_pdb_dialog_ = new QtPdbPromptDialog(
                QtPdbPromptDialog::Mode::local, dialog_parent_);
            local_pdb_dialog_->setAttribute(Qt::WA_DeleteOnClose, false);
            connect(local_pdb_dialog_, &QtPdbPromptDialog::decided, this,
                    [this](bool approved, bool load_types, bool load_names,
                           const QString& local_path) {
                auto& s = *context_->initialAnalysisState;
                const auto ws = context_->workspace().lock();
                if (!ws) return;
                if (!approved) {
                    const auto declined = analysis_session::decline_local_pdb(ws);
                    if (declined) {
                        s.pdb_error.clear();
                        local_pdb_dialog_->hide();
                    } else {
                        s.pdb_error = declined.error().stable_code() + ": " +
                            declined.error().message;
                    }
                    return;
                }
                const auto accepted = analysis_session::approve_local_pdb(ws,
                    local_path.toStdString(), load_types, load_names);
                if (accepted) {
                    s.pdb_error.clear();
                    local_pdb_dialog_->hide();
                } else {
                    s.pdb_error = accepted.error().stable_code() + ": " +
                        accepted.error().message;
                }
            });
        }
        local_pdb_dialog_->adoptWorkspace(workspace, value, state.load_types,
            state.load_names);
        local_pdb_dialog_->open();
    } else if (local_pdb_dialog_) {
        local_pdb_dialog_->hide();
    }
    if (value.loading) {
        if (!pdb_status_dialog_) {
            pdb_status_dialog_ = new QtPdbStatusDialog(dialog_parent_);
            pdb_status_dialog_->setAttribute(Qt::WA_DeleteOnClose, false);
            pdb_status_dialog_->adoptWorkspace(workspace);
        }
        pdb_status_dialog_->poll();
        pdb_status_dialog_->show();
    } else if (pdb_status_dialog_) {
        pdb_status_dialog_->hide();
    }
}

void QtInitialAnalysisController::syncBatchTask() {
    const auto workspace = context_->workspace().lock();
    auto& state = *context_->initialAnalysisState;
    const auto orchestrator = workspace->background_decompile();
    if (!orchestrator) {
        if (state.batch_task_registered) {
            // terminal cancelled (Workspace closed).
            const auto processed = state.batch_snapshot.completed +
                state.batch_snapshot.failed + state.batch_snapshot.cancelled;
            const float fraction = state.batch_snapshot.total != 0
                ? static_cast<float>((std::min)(1.0,
                    static_cast<double>(processed) /
                    static_cast<double>(state.batch_snapshot.total)))
                : 1.0f;
            char summary[192]{};
            std::snprintf(summary, sizeof(summary),
                "%llu/%llu decompiled | %llu failed | %llu cancelled",
                static_cast<unsigned long long>(state.batch_snapshot.completed),
                static_cast<unsigned long long>(state.batch_snapshot.total),
                static_cast<unsigned long long>(state.batch_snapshot.failed),
                static_cast<unsigned long long>(state.batch_snapshot.cancelled));
            static_cast<void>(aida::ui::task_center::update_task(state.batch_task_id,
                aida::ui::task_center::task_state_t::cancelled, fraction,
                "Workspace closed", summary));
            state.batch_task_registered = false;
            state.batch_task_id.clear();
            state.batch_generation = 0;
        }
        state.batch_cancel_requested = false;
        return;
    }
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (state.batch_task_registered &&
        static_cast<std::uint64_t>(now_ms) - state.batch_last_update_ms < 500)
        return;
    const auto snap = orchestrator->run_snapshot();
    if (snap.active) {
        if (state.batch_task_registered && snap.generation != state.batch_generation) {
            // terminal partial (Superseded by a new run).
            const auto processed = state.batch_snapshot.completed +
                state.batch_snapshot.failed + state.batch_snapshot.cancelled;
            const float fraction = state.batch_snapshot.total != 0
                ? static_cast<float>((std::min)(1.0,
                    static_cast<double>(processed) /
                    static_cast<double>(state.batch_snapshot.total)))
                : 1.0f;
            static_cast<void>(aida::ui::task_center::update_task(state.batch_task_id,
                aida::ui::task_center::task_state_t::partial, fraction,
                "Superseded by a new run"));
            state.batch_task_registered = false;
            state.batch_task_id.clear();
        }
        state.batch_snapshot = snap;
        if (!state.batch_task_registered && state.batch_generation == snap.generation) {
            state.batch_last_update_ms = static_cast<std::uint64_t>(now_ms);
            return;
        }
        if (!state.batch_task_registered) {
            const std::string binary_hex =
                workspace->identity().binary_id().to_hex();
            const std::string task_id = "decompile.batch." + binary_hex + "." +
                std::to_string(snap.generation);
            aida::ui::task_center::task_registration_t registration;
            registration.id = task_id;
            registration.source = "decompiler";
            registration.owner = "Decompiler";
            registration.owner_view = "document.pseudocode";
            registration.target = workspace->identity().bin_name();
            registration.label = "Background decompilation";
            registration.stage = "Starting background decompilation";
            registration.cancellation_is_safe = true;
            std::weak_ptr<aida::analysis::decompile_batch_orchestrator_t> weak =
                orchestrator;
            registration.callbacks.cancel = [weak]() {
                const auto strong = weak.lock();
                if (!strong) return false;
                strong->request_cancel();
                return true;
            };
            bool registered =
                aida::ui::task_center::register_task(std::move(registration));
            if (!registered)
                registered = aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::running, 0.0f,
                    "Restarting background decompilation");
            diag::log_tagged_fmt("dec_batch",
                "progress_task_register id=%s ok=%d total=%llu",
                task_id.c_str(), registered ? 1 : 0,
                static_cast<unsigned long long>(snap.total));
            if (registered) {
                state.batch_task_registered = true;
                state.batch_task_id = task_id;
                state.batch_cancel_requested = false;
            }
            state.batch_generation = snap.generation;
        } else {
            const auto processed = snap.completed + snap.failed + snap.cancelled;
            const float fraction = snap.total != 0
                ? static_cast<float>((std::min)(1.0,
                    static_cast<double>(processed) / static_cast<double>(snap.total)))
                : -1.0f;
            const auto eta_total_s = static_cast<unsigned>(
                snap.eta_s > 0.0 ? snap.eta_s + 0.5 : 0.0);
            char stage[192]{};
            std::snprintf(stage, sizeof(stage),
                "Decompiling functions %llu/%llu | %.1f funcs/s | ETA %02u:%02u",
                static_cast<unsigned long long>(processed),
                static_cast<unsigned long long>(snap.total),
                snap.rate_funcs_s, eta_total_s / 60, eta_total_s % 60);
            static_cast<void>(aida::ui::task_center::update_task(state.batch_task_id,
                aida::ui::task_center::task_state_t::running, fraction, stage));
        }
        state.batch_last_update_ms = static_cast<std::uint64_t>(now_ms);
        return;
    }
    if (state.batch_task_registered) {
        const auto processed = snap.completed + snap.failed + snap.cancelled;
        auto terminal = aida::ui::task_center::task_state_t::completed;
        if (snap.cancelled != 0)
            terminal = aida::ui::task_center::task_state_t::cancelled;
        else if (snap.failed != 0 || processed < snap.total)
            terminal = aida::ui::task_center::task_state_t::partial;
        const float fraction = snap.total != 0
            ? static_cast<float>((std::min)(1.0,
                static_cast<double>(processed) / static_cast<double>(snap.total)))
            : 1.0f;
        char summary[192]{};
        std::snprintf(summary, sizeof(summary),
            "%llu/%llu decompiled | %llu failed | %llu cancelled",
            static_cast<unsigned long long>(snap.completed),
            static_cast<unsigned long long>(snap.total),
            static_cast<unsigned long long>(snap.failed),
            static_cast<unsigned long long>(snap.cancelled));
        static_cast<void>(aida::ui::task_center::update_task(state.batch_task_id,
            terminal, fraction, "Background decompilation finished", summary));
        state.batch_task_registered = false;
        state.batch_task_id.clear();
        state.batch_generation = 0;
        state.batch_cancel_requested = false;
        state.batch_last_update_ms = static_cast<std::uint64_t>(now_ms);
    }
    state.batch_snapshot = snap;
}

void QtInitialAnalysisController::syncBatchDialog() {
    const auto workspace = context_->workspace().lock();
    const auto orchestrator = workspace->background_decompile();
    if (!orchestrator) {
        if (batch_dialog_) batch_dialog_->hide();
        return;
    }
    const auto snap = orchestrator->run_snapshot();
    if (!snap.active) {
        if (batch_dialog_) batch_dialog_->hide();
        return;
    }
    if (!batch_dialog_) {
        batch_dialog_ = new QtBatchDecompileDialog(dialog_parent_);
        batch_dialog_->setAttribute(Qt::WA_DeleteOnClose, false);
        batch_dialog_->adoptWorkspace(workspace);
    }
    batch_dialog_->poll();
    batch_dialog_->show();
}

}
