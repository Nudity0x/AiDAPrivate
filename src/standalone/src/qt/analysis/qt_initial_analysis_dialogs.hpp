#pragma once

#include <QDialog>

#include <cstdint>
#include <memory>
#include <string>

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/session/analysis_session.hpp"

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTimer;

namespace aida::qt::analysis {

// "Workspace Analysis" progress dialog (07 sec. 7.7): shown while readiness is in
// {analyzing, cancelling, failed, partial-incomplete}. Non-modal, Qt::Tool.
class QtAnalysisProgressDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtAnalysisProgressDialog(QWidget* parent = nullptr);
    void adoptWorkspace(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace);
    void poll();

    Q_SIGNAL void dismissed();
    Q_SIGNAL void retryRequested();

private:
    std::weak_ptr<aida::analysis::analysis_workspace_t> workspace_;
    QLabel* name_label_ = nullptr;
    QLabel* readiness_label_ = nullptr;
    QLabel* phase_label_ = nullptr;
    QLabel* units_label_ = nullptr;
    QLabel* error_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* retry_button_ = nullptr;
    QPushButton* dismiss_button_ = nullptr;
};

// PDB prompt (two modes share one class, 07 sec. 7.7): remote ("Debug information
// available") and local ("Locate local PDB"). Modal (open() + ApplicationModal).
class QtPdbPromptDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode : std::uint8_t { remote, local };
    QtPdbPromptDialog(Mode mode, QWidget* parent = nullptr);

    void adoptWorkspace(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
        const analysis_session::pdb_prompt_snapshot_t& snapshot,
        bool load_types, bool load_names);

    Q_SIGNAL void decided(bool approved, bool load_types, bool load_names,
                          const QString& local_path);

private:
    void rebuild();

    Mode mode_;
    std::weak_ptr<aida::analysis::analysis_workspace_t> workspace_;
    analysis_session::pdb_prompt_snapshot_t snapshot_{};
    QLabel* title_label_ = nullptr;
    QLabel* body_label_ = nullptr;
    QLabel* guid_label_ = nullptr;
    QLabel* server_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QCheckBox* load_types_ = nullptr;
    QCheckBox* load_names_ = nullptr;
    QLineEdit* path_edit_ = nullptr;
    QPushButton* browse_button_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

// Floating PDB status dialog ("PDB operation"). Non-modal, top-right-ish.
class QtPdbStatusDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtPdbStatusDialog(QWidget* parent = nullptr);
    void adoptWorkspace(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace);
    void poll();

private:
    std::weak_ptr<aida::analysis::analysis_workspace_t> workspace_;
    QLabel* module_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
};

// "Background Decompilation" dialog (07 sec. 7.7): observes the workspace's
// decompile_batch_orchestrator without starting it.
class QtBatchDecompileDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtBatchDecompileDialog(QWidget* parent = nullptr);
    void adoptWorkspace(
        const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace);
    void poll();

private:
    std::weak_ptr<aida::analysis::analysis_workspace_t> workspace_;
    QLabel* title_label_ = nullptr;
    QLabel* rate_label_ = nullptr;
    QLabel* failed_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
};

}
