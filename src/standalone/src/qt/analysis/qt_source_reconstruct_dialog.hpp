#pragma once

#include <QDialog>

#include <cstdint>
#include <memory>
#include <string>

#include "core/analysis/source_reconstructor.hpp"

class QAbstractTableModel;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableView;
class QTimer;

namespace aida::analysis {
class analysis_workspace_t;
}

namespace aida::qt::analysis {

class QtWorkspaceContext;

// "Reconstruct Source" modal (07 sec. 7.6): one instance per binary; hides on
// reject while the engine runs and re-adopts in-flight state on reopen.
// open(), never exec() (S7).
class QtSourceReconstructDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtSourceReconstructDialog(QWidget* parent = nullptr);

    // Engine-hook entry point (analysis_host_hooks.open_source_reconstruction).
    static void openFor(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
        QWidget* parent = nullptr);

    void adoptWorkspace(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace);
    void open();

private Q_SLOTS:
    void pollWorker();

private:
    void rebuildPhase();
    void startReconstruction();
    static const char* stage_label(source_reconstructor::stage_t stage) noexcept;

    std::weak_ptr<aida::analysis::analysis_workspace_t> workspace_;
    QString binary_id_hex_;
    source_reconstructor::workspace_reconstruction_state_t recon_state_;
    bool started_ = false;
    bool cancellation_requested_ = false;
    QLineEdit* output_dir_ = nullptr;
    QLabel* binary_value_ = nullptr;
    QLabel* stage_value_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* progress_text_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QAbstractTableModel* files_model_ = nullptr;
    QAbstractTableModel* diagnostics_model_ = nullptr;
    QTableView* files_table_ = nullptr;
    QTableView* diagnostics_table_ = nullptr;
    QLabel* result_summary_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    QTimer* timer_ = nullptr;
};

}
