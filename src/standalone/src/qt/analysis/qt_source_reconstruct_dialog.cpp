#include "qt/analysis/qt_source_reconstruct_dialog.hpp"

#include <QAbstractTableModel>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"

#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::analysis {

namespace {

class QtStringRowsModel : public QAbstractTableModel {
public:
    explicit QtStringRowsModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}
    void setRows(QStringList rows) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }
    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : 1;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || role != Qt::DisplayRole || index.row() < 0 ||
            index.row() >= rows_.size())
            return {};
        return rows_.at(index.row());
    }
    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override {
        for (QModelRoleData& roleData : span) {
            if (roleData.role() == Qt::DisplayRole)
                roleData.setData(data(index, Qt::DisplayRole));
            else
                roleData.clearData();
        }
    }
private:
    QStringList rows_;
};

const char* diagnostic_severity_label(
    aida::analysis::decompiler_diagnostic_severity_t severity) {
    switch (severity) {
    case aida::analysis::decompiler_diagnostic_severity_t::note: return "Note";
    case aida::analysis::decompiler_diagnostic_severity_t::warning: return "Warning";
    case aida::analysis::decompiler_diagnostic_severity_t::error: return "Error";
    }
    return "Diagnostic";
}

std::string diagnostic_text(
    const aida::analysis::decompiler_diagnostic_t& diagnostic) {
    std::string text = diagnostic_severity_label(diagnostic.severity);
    if (diagnostic.ordinal != 0) {
        text += " #";
        text += std::to_string(diagnostic.ordinal);
    }
    text += " [code ";
    text += std::to_string(static_cast<unsigned int>(diagnostic.code));
    text += "]: ";
    text += diagnostic.localization_key.empty()
        ? "decompiler_diagnostic" : diagnostic.localization_key;
    for (const auto& argument : diagnostic.localization_arguments) {
        text.push_back(' ');
        text += argument;
    }
    if (diagnostic.retryable)
        text += " [retryable]";
    if (diagnostic.confidence != 0) {
        text += " [confidence ";
        text += std::to_string(diagnostic.confidence);
        text += "%]";
    }
    return text;
}

}

QtSourceReconstructDialog::QtSourceReconstructDialog(QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("aida.analysis.source-reconstruction"));
    setWindowTitle(QStringLiteral("Reconstruct Source"));
    setModal(true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(680, 520);
    setMinimumSize(420, 320);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    binary_value_ = new QLabel(this);
    form->addRow(QStringLiteral("Binary"), binary_value_);
    auto* scope = new QLabel(QStringLiteral(
        "Functions, imports, exports, headers, modules, metadata"), this);
    form->addRow(QStringLiteral("Scope"), scope);
    auto* output_row = new QHBoxLayout();
    output_dir_ = new QLineEdit(this);
    output_dir_->setMaxLength(512);
    output_row->addWidget(output_dir_, 1);
    auto* browse = new QPushButton(QStringLiteral("Browse..."), this);
    output_row->addWidget(browse);
    form->addRow(QStringLiteral("Output directory"), output_row);
    layout->addLayout(form);
    stage_value_ = new QLabel(this);
    layout->addWidget(stage_value_);
    status_label_ = new QLabel(this);
    status_label_->setWordWrap(true);
    layout->addWidget(status_label_);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 100);
    layout->addWidget(progress_);
    progress_text_ = new QLabel(this);
    layout->addWidget(progress_text_);
    result_summary_ = new QLabel(this);
    result_summary_->setWordWrap(true);
    layout->addWidget(result_summary_);
    files_model_ = new QtStringRowsModel(this);
    diagnostics_model_ = new QtStringRowsModel(this);
    files_table_ = new QTableView(this);
    files_table_->verticalHeader()->setVisible(false);
    files_table_->verticalHeader()->setDefaultSectionSize(
        theme::tokens().table.compact_row_h);
    files_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    files_table_->horizontalHeader()->setVisible(false);
    files_table_->horizontalHeader()->setStretchLastSection(true);
    files_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    files_table_->setModel(files_model_);
    diagnostics_table_ = new QTableView(this);
    diagnostics_table_->verticalHeader()->setVisible(false);
    diagnostics_table_->verticalHeader()->setDefaultSectionSize(
        theme::tokens().table.compact_row_h);
    diagnostics_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    diagnostics_table_->horizontalHeader()->setVisible(false);
    diagnostics_table_->horizontalHeader()->setStretchLastSection(true);
    diagnostics_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    diagnostics_table_->setModel(diagnostics_model_);
    auto* lists = new QHBoxLayout();
    auto* files_panel = new QVBoxLayout();
    files_panel->addWidget(new QLabel(QStringLiteral("Output files"), this));
    files_panel->addWidget(files_table_, 1);
    auto* diagnostics_panel = new QVBoxLayout();
    diagnostics_panel->addWidget(new QLabel(QStringLiteral("Diagnostics"), this));
    diagnostics_panel->addWidget(diagnostics_table_, 1);
    lists->addLayout(files_panel, 1);
    lists->addLayout(diagnostics_panel, 1);
    layout->addLayout(lists, 1);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this);
    layout->addWidget(buttons_);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString picked = QFileDialog::getExistingDirectory(this,
            QStringLiteral("Select Output Directory"), output_dir_->text());
        if (!picked.isEmpty()) output_dir_->setText(picked);
    });
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        if (!started_) {
            if (output_dir_->text().isEmpty()) {
                status_label_->setText(QStringLiteral("Choose an output directory."));
                return;
            }
            startReconstruction();
            return;
        }
        if (source_reconstructor::is_running_workspace(recon_state_)) {
            if (!cancellation_requested_) {
                cancellation_requested_ = true;
                source_reconstructor::cancel_workspace(recon_state_);
                buttons_->button(QDialogButtonBox::Ok)->setText(
                    QStringLiteral("Cancellation Requested"));
                buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
            }
            return;
        }
        accept();
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, [this] { reject(); });

    timer_ = new QTimer(this);
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, [this] { pollWorker(); });
}

void QtSourceReconstructDialog::openFor(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    QWidget* parent) {
    if (!workspace) return;
    const QString key = QString::fromStdString(
        workspace->identity().binary_id().to_hex());
    auto* dialog = new QtSourceReconstructDialog(parent);
    dialog->adoptWorkspace(workspace);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setProperty("aida.binary", key);
    dialog->open();
}

void QtSourceReconstructDialog::adoptWorkspace(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace) {
    workspace_ = workspace;
    binary_id_hex_ = QString::fromStdString(
        workspace->identity().binary_id().to_hex());
    binary_value_->setText(QString::fromStdString(workspace->identity().bin_name()));
    const bool running =
        source_reconstructor::is_running_workspace(recon_state_);
    started_ = running;
    cancellation_requested_ = false;
    if (running) {
        cancellation_requested_ =
            recon_state_.cancel_requested.load(std::memory_order_acquire);
        std::string retained_output;
        {
            std::lock_guard<std::mutex> lock(recon_state_.mutex);
            retained_output = recon_state_.last_result.output_dir;
        }
        if (!retained_output.empty())
            output_dir_->setText(QString::fromStdString(retained_output));
    } else if (output_dir_->text().isEmpty()) {
        const QString home = QString::fromLocal8Bit(qgetenv("USERPROFILE"));
        output_dir_->setText(home.isEmpty()
            ? QStringLiteral("C:\\AiDA_Reconstruction")
            : home + QStringLiteral("\\Documents\\AiDA_Reconstruction"));
    }
    rebuildPhase();
}

void QtSourceReconstructDialog::open() {
    timer_->start();
    QDialog::open();
}

void QtSourceReconstructDialog::pollWorker() {
    rebuildPhase();
}

void QtSourceReconstructDialog::rebuildPhase() {
    const bool running = source_reconstructor::is_running_workspace(recon_state_);
    auto* ok = buttons_->button(QDialogButtonBox::Ok);
    auto* cancel = buttons_->button(QDialogButtonBox::Cancel);
    if (!started_) {
        ok->setText(QStringLiteral("Start Reconstruction"));
        ok->setEnabled(!output_dir_->text().isEmpty());
        cancel->setText(QStringLiteral("Cancel"));
        progress_->setVisible(false);
        progress_text_->setVisible(false);
        result_summary_->setVisible(false);
        stage_value_->setVisible(false);
        return;
    }
    if (running) {
        const auto stage = static_cast<source_reconstructor::stage_t>(
            recon_state_.stage.load(std::memory_order_relaxed));
        stage_value_->setVisible(true);
        stage_value_->setText(QString::fromLatin1(stage_label(stage)));
        const int total = source_reconstructor::get_total_functions_workspace(
            recon_state_);
        const int done = source_reconstructor::get_decompiled_count_workspace(
            recon_state_);
        const float progress = (std::clamp)(
            source_reconstructor::get_progress_workspace(recon_state_), 0.0f, 1.0f);
        progress_->setVisible(true);
        progress_->setValue(static_cast<int>(progress * 100.f));
        progress_text_->setVisible(true);
        progress_text_->setText(QStringLiteral("%1 of %2 functions processed")
            .arg(done).arg(total));
        const std::string status =
            source_reconstructor::get_status_workspace(recon_state_);
        status_label_->setText(cancellation_requested_
            ? QStringLiteral(
                "Cancellation was requested. Waiting for the worker to publish its terminal result.")
            : QString::fromStdString(status));
        result_summary_->setVisible(false);
        ok->setText(cancellation_requested_ ? QStringLiteral("Cancellation Requested")
            : QStringLiteral("Request Cancellation"));
        ok->setEnabled(!cancellation_requested_);
        cancel->setVisible(false);
        return;
    }
    // Result phase.
    const auto& result =
        source_reconstructor::get_last_result_workspace(recon_state_);
    const bool cancelled = !result.success && result.error == "Cancelled.";
    const std::string summary = std::to_string(result.decompiled_functions) +
        " of " + std::to_string(result.total_functions) + " functions; " +
        std::to_string(result.files_created.size()) + " files; " +
        std::to_string(result.diagnostics.size()) + " diagnostics.";
    stage_value_->setVisible(true);
    stage_value_->setText(result.success ? QStringLiteral("Reconstruction complete")
        : cancelled ? QStringLiteral("Reconstruction cancelled")
        : QStringLiteral("Reconstruction incomplete"));
    status_label_->setText(QString::fromStdString(result.error));
    result_summary_->setVisible(true);
    result_summary_->setText(QString::fromStdString(summary));
    QStringList files;
    files.reserve(static_cast<int>(result.files_created.size()));
    for (const auto& file : result.files_created)
        files.push_back(QString::fromStdString(file));
    static_cast<QtStringRowsModel*>(files_model_)->setRows(std::move(files));
    QStringList diagnostics;
    diagnostics.reserve(static_cast<int>(result.diagnostics.size()));
    for (const auto& diagnostic : result.diagnostics)
        diagnostics.push_back(QString::fromStdString(diagnostic_text(diagnostic)));
    static_cast<QtStringRowsModel*>(diagnostics_model_)->setRows(
        std::move(diagnostics));
    progress_->setVisible(false);
    progress_text_->setVisible(false);
    ok->setText(QStringLiteral("Close"));
    ok->setEnabled(true);
    cancel->setVisible(false);
    timer_->stop();
}

void QtSourceReconstructDialog::startReconstruction() {
    const auto workspace = workspace_.lock();
    if (!workspace) {
        status_label_->setText(QStringLiteral(
            "The retained binary workspace is no longer available."));
        return;
    }
    source_reconstructor::workspace_reconstruction_config_t config;
    config.workspace = workspace;
    config.output_dir = output_dir_->text().toStdString();
    config.project_name = "reconstructed";
    config.include_imports = true;
    config.include_exports = true;
    config.generate_cmake = true;
    config.max_functions = 0;
    source_reconstructor::reconstruct_workspace(config, recon_state_);
    started_ = true;
    timer_->start();
    rebuildPhase();
}

const char* QtSourceReconstructDialog::stage_label(
    source_reconstructor::stage_t stage) noexcept {
    switch (stage) {
    case source_reconstructor::stage_t::idle: return "Ready";
    case source_reconstructor::stage_t::collect: return "Collecting functions";
    case source_reconstructor::stage_t::decompile: return "Decompiling";
    case source_reconstructor::stage_t::cluster: return "Clustering modules";
    case source_reconstructor::stage_t::headers: return "Generating headers";
    case source_reconstructor::stage_t::modules: return "Writing modules";
    case source_reconstructor::stage_t::metadata: return "Writing metadata";
    case source_reconstructor::stage_t::done: return "Complete";
    case source_reconstructor::stage_t::failed: return "Failed";
    }
    return "Unknown";
}

}
