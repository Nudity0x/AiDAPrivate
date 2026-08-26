#include "qt/analysis/qt_initial_analysis_dialogs.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

#include "core/analysis/decompiler/decompile_batch_orchestrator.hpp"
#include "core/analysis/initial_analysis.hpp"
#include "core/analysis/workspace/analysis_workspace.hpp"

#include "qt/bridge/dialogs.hpp"
#include "qt/theme/aida_stylesheet.hpp"

namespace aida::qt::analysis {

namespace {

const char* readiness_name(aida::analysis::workspace_readiness_t readiness) {
    using aida::analysis::workspace_readiness_t;
    switch (readiness) {
    case workspace_readiness_t::created: return "Created";
    case workspace_readiness_t::provider_ready: return "Provider ready";
    case workspace_readiness_t::parsed: return "Parsed";
    case workspace_readiness_t::analyzing: return "Analyzing";
    case workspace_readiness_t::baseline_ready: return "Baseline ready";
    case workspace_readiness_t::partial: return "Partial";
    case workspace_readiness_t::failed: return "Failed";
    case workspace_readiness_t::cancelling: return "Cancelling";
    case workspace_readiness_t::closing: return "Closing";
    case workspace_readiness_t::closed: return "Closed";
    default: return "Unknown";
    }
}

float progress_fraction(const aida::analysis::workspace_progress_t& progress) {
    if (progress.total_units != 0)
        return static_cast<float>((std::min)(1.0,
            static_cast<double>(progress.completed_units) /
            static_cast<double>(progress.total_units)));
    if (progress.total_bytes != 0)
        return static_cast<float>((std::min)(1.0,
            static_cast<double>(progress.completed_bytes) /
            static_cast<double>(progress.total_bytes)));
    return progress.readiness ==
        aida::analysis::workspace_readiness_t::baseline_ready ? 1.0f : 0.0f;
}

}

// ------------------------------------------------------------ progress dialog

QtAnalysisProgressDialog::QtAnalysisProgressDialog(QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("aida.workspace.analysis.progress"));
    setWindowTitle(QStringLiteral("Workspace Analysis"));
    setModal(false);
    setWindowFlag(Qt::Tool);
    resize(430, 196);
    auto* layout = new QVBoxLayout(this);
    name_label_ = new QLabel(this);
    name_label_->setObjectName(QStringLiteral("aida.analysis.progress.name"));
    layout->addWidget(name_label_);
    readiness_label_ = new QLabel(this);
    readiness_label_->setObjectName(QStringLiteral("aida.analysis.progress.readiness"));
    layout->addWidget(readiness_label_);
    phase_label_ = new QLabel(this);
    phase_label_->setObjectName(QStringLiteral("aida.analysis.progress.phase"));
    phase_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(phase_label_);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 100);
    layout->addWidget(progress_);
    units_label_ = new QLabel(this);
    units_label_->setObjectName(QStringLiteral("aida.analysis.progress.units"));
    units_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(units_label_);
    error_label_ = new QLabel(this);
    error_label_->setObjectName(QStringLiteral("aida.analysis.progress.error"));
    error_label_->setProperty("aidaVariant", QStringLiteral("error"));
    error_label_->setWordWrap(true);
    error_label_->setVisible(false);
    layout->addWidget(error_label_);
    auto* buttons = new QHBoxLayout();
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    retry_button_ = new QPushButton(QStringLiteral("Retry"), this);
    dismiss_button_ = new QPushButton(QStringLiteral("Dismiss"), this);
    buttons->addWidget(cancel_button_);
    buttons->addWidget(retry_button_);
    buttons->addWidget(dismiss_button_);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    connect(cancel_button_, &QPushButton::clicked, this, [this] {
        if (const auto workspace = workspace_.lock()) workspace->request_cancel();
    });
    connect(retry_button_, &QPushButton::clicked, this, [this] {
        Q_EMIT retryRequested();
    });
    connect(dismiss_button_, &QPushButton::clicked, this, [this] {
        Q_EMIT dismissed();
        hide();
    });
}

void QtAnalysisProgressDialog::adoptWorkspace(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace) {
    workspace_ = workspace;
}

void QtAnalysisProgressDialog::poll() {
    const auto workspace = workspace_.lock();
    if (!workspace) return;
    const auto progress = workspace->progress();
    name_label_->setText(QString::fromStdString(workspace->identity().bin_name()));
    readiness_label_->setText(QString::fromLatin1(
        readiness_name(progress.readiness)));
    const char* readiness_variant = "neutral";
    switch (progress.readiness) {
    case aida::analysis::workspace_readiness_t::analyzing:
        readiness_variant = "info";
        break;
    case aida::analysis::workspace_readiness_t::baseline_ready:
        readiness_variant = "success";
        break;
    case aida::analysis::workspace_readiness_t::partial:
    case aida::analysis::workspace_readiness_t::cancelling:
        readiness_variant = "warning";
        break;
    case aida::analysis::workspace_readiness_t::failed:
        readiness_variant = "error";
        break;
    default:
        break;
    }
    if (readiness_label_->property("aidaVariant").toString() !=
        QLatin1String(readiness_variant)) {
        readiness_label_->setProperty("aidaVariant",
            QString::fromLatin1(readiness_variant));
        theme::stylesheet::repolish(readiness_label_);
    }
    phase_label_->setText(progress.phase.empty()
        ? QStringLiteral("Preparing analysis")
        : QString::fromStdString(progress.phase));
    progress_->setValue(static_cast<int>(progress_fraction(progress) * 100.f));
    if (progress.total_units != 0) {
        units_label_->setVisible(true);
        units_label_->setText(QStringLiteral("%1 / %2 units")
            .arg(progress.completed_units).arg(progress.total_units));
    } else {
        units_label_->setVisible(false);
    }
    error_label_->setVisible(progress.error.has_value());
    if (progress.error) {
        error_label_->setText(QStringLiteral("%1: %2")
            .arg(QString::fromStdString(progress.error->stable_code()))
            .arg(QString::fromStdString(progress.error->message)));
    }
    const bool running =
        progress.readiness == aida::analysis::workspace_readiness_t::analyzing ||
        progress.readiness == aida::analysis::workspace_readiness_t::cancelling;
    cancel_button_->setVisible(running);
    cancel_button_->setEnabled(!progress.cancellation_requested);
    retry_button_->setVisible(!running &&
        progress.readiness != aida::analysis::workspace_readiness_t::baseline_ready);
    dismiss_button_->setVisible(!running);
}

// ------------------------------------------------------------------ pdb prompt

QtPdbPromptDialog::QtPdbPromptDialog(Mode mode, QWidget* parent)
    : QDialog(parent), mode_(mode) {
    setObjectName(mode == Mode::remote
        ? QStringLiteral("aida.dialog.pdb.remote")
        : QStringLiteral("aida.dialog.pdb.local"));
    setWindowTitle(mode == Mode::remote
        ? QStringLiteral("Debug information available")
        : QStringLiteral("Locate local PDB"));
    setModal(true);
    resize(620, mode == Mode::remote ? 420 : 460);
    setMinimumSize(400, 300);
    auto* layout = new QVBoxLayout(this);
    title_label_ = new QLabel(this);
    title_label_->setObjectName(QStringLiteral("aida.dialog.pdb.title"));
    layout->addWidget(title_label_);
    body_label_ = new QLabel(this);
    body_label_->setWordWrap(true);
    layout->addWidget(body_label_);
    guid_label_ = new QLabel(this);
    guid_label_->setObjectName(QStringLiteral("aida.dialog.pdb.guid"));
    guid_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(guid_label_);
    server_label_ = new QLabel(this);
    server_label_->setObjectName(QStringLiteral("aida.dialog.pdb.server"));
    server_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    server_label_->setWordWrap(true);
    layout->addWidget(server_label_);
    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.dialog.pdb.status"));
    status_label_->setWordWrap(true);
    layout->addWidget(status_label_);
    load_types_ = new QCheckBox(QStringLiteral("Load types"), this);
    load_types_->setChecked(true);
    layout->addWidget(load_types_);
    load_names_ = new QCheckBox(QStringLiteral("Load names"), this);
    load_names_->setChecked(true);
    layout->addWidget(load_names_);
    auto* path_row = new QHBoxLayout();
    path_edit_ = new QLineEdit(this);
    path_edit_->setMaxLength(32767);
    path_row->addWidget(path_edit_, 1);
    browse_button_ = new QPushButton(QStringLiteral("Browse..."), this);
    path_row->addWidget(browse_button_);
    layout->addLayout(path_row);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No,
        this);
    layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        Q_EMIT decided(true, load_types_->isChecked(), load_names_->isChecked(),
            mode_ == Mode::local ? path_edit_->text() : QString());
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, [this] {
        Q_EMIT decided(false, load_types_->isChecked(), load_names_->isChecked(),
            QString());
    });
    connect(browse_button_, &QPushButton::clicked, this, [this] {
        const auto picked = qt::dialogs::open_file(this,
            QStringLiteral("Select PDB file"),
            "PDB files (*.pdb)\0*.pdb\0All files (*.*)\0*.*\0\0", path_edit_->text());
        if (picked && !picked->empty())
            path_edit_->setText(QString::fromStdString(*picked));
    });
}

void QtPdbPromptDialog::adoptWorkspace(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    const analysis_session::pdb_prompt_snapshot_t& snapshot, bool load_types,
    bool load_names) {
    workspace_ = workspace;
    snapshot_ = snapshot;
    load_types_->setChecked(load_types);
    load_names_->setChecked(load_names);
    rebuild();
}

void QtPdbPromptDialog::rebuild() {
    if (mode_ == Mode::remote) {
        title_label_->setText(QStringLiteral("Debug information available"));
        body_label_->setText(QStringLiteral("The selected workspace references %1.")
            .arg(snapshot_.pdb_name.empty()
                ? QStringLiteral("an external PDB")
                : QString::fromStdString(snapshot_.pdb_name)));
        guid_label_->setText(QStringLiteral("GUID: %1   Age: %2")
            .arg(QString::fromStdString(snapshot_.pdb_guid))
            .arg(snapshot_.pdb_age));
        server_label_->setText(QString::fromStdString(snapshot_.symbol_server));
        path_edit_->setVisible(false);
        browse_button_->setVisible(false);
        buttons_->button(QDialogButtonBox::Yes)->setText(
            QStringLiteral("Yes, download"));
        buttons_->button(QDialogButtonBox::No)->setText(
            QStringLiteral("No, skip"));
    } else {
        title_label_->setText(QStringLiteral("Locate local debug symbols"));
        body_label_->setText(QString::fromStdString(snapshot_.module_name));
        guid_label_->setText(QString::fromStdString(snapshot_.reason));
        server_label_->clear();
        path_edit_->setVisible(true);
        browse_button_->setVisible(true);
        if (path_edit_->text().isEmpty())
            path_edit_->setText(QString::fromStdString(snapshot_.local_candidate));
        buttons_->button(QDialogButtonBox::Yes)->setText(
            QStringLiteral("Load this PDB"));
        buttons_->button(QDialogButtonBox::No)->setText(
            QStringLiteral("No, skip"));
    }
    status_label_->setText(QString::fromStdString(snapshot_.status));
    const bool can_accept = mode_ == Mode::remote
        ? (load_types_->isChecked() || load_names_->isChecked())
        : !path_edit_->text().isEmpty() &&
            (load_types_->isChecked() || load_names_->isChecked());
    buttons_->button(QDialogButtonBox::Yes)->setEnabled(can_accept);
}

// ------------------------------------------------------------------ pdb status

QtPdbStatusDialog::QtPdbStatusDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("aida.dialog.pdb.status"));
    setWindowTitle(QStringLiteral("PDB operation"));
    setModal(false);
    setWindowFlag(Qt::Tool);
    resize(430, 132);
    auto* layout = new QVBoxLayout(this);
    module_label_ = new QLabel(this);
    module_label_->setObjectName(QStringLiteral("aida.dialog.pdb.status.module"));
    layout->addWidget(module_label_);
    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.dialog.pdb.status.detail"));
    status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    status_label_->setWordWrap(true);
    layout->addWidget(status_label_);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 100);
    layout->addWidget(progress_);
    cancel_button_ = new QPushButton(QStringLiteral("Cancel PDB"), this);
    layout->addWidget(cancel_button_);
    connect(cancel_button_, &QPushButton::clicked, this, [this] {
        if (const auto workspace = workspace_.lock())
            static_cast<void>(analysis_session::cancel_pdb(workspace));
    });
}

void QtPdbStatusDialog::adoptWorkspace(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace) {
    workspace_ = workspace;
}

void QtPdbStatusDialog::poll() {
    const auto workspace = workspace_.lock();
    if (!workspace) return;
    const auto prompt = analysis_session::pdb_prompt_snapshot(workspace);
    if (!prompt) return;
    const auto& value = prompt.value();
    module_label_->setText(QString::fromStdString(value.module_name));
    status_label_->setText(QString::fromStdString(value.status));
    const float fraction = value.bytes_total != 0
        ? static_cast<float>((std::min)(1.0,
            static_cast<double>(value.bytes_received) /
            static_cast<double>(value.bytes_total)))
        : static_cast<float>((std::clamp)(value.progress_percent, 0, 100)) / 100.0f;
    progress_->setValue(static_cast<int>(fraction * 100.f));
}

// --------------------------------------------------------------- batch decompile

QtBatchDecompileDialog::QtBatchDecompileDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("aida.decompile.batch.progress"));
    setWindowTitle(QStringLiteral("Background Decompilation"));
    setModal(false);
    setWindowFlag(Qt::Tool);
    resize(430, 150);
    auto* layout = new QVBoxLayout(this);
    title_label_ = new QLabel(this);
    title_label_->setObjectName(QStringLiteral("aida.decompile.batch.title"));
    layout->addWidget(title_label_);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 100);
    layout->addWidget(progress_);
    rate_label_ = new QLabel(this);
    rate_label_->setObjectName(QStringLiteral("aida.decompile.batch.rate"));
    rate_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(rate_label_);
    failed_label_ = new QLabel(this);
    failed_label_->setObjectName(QStringLiteral("aida.decompile.batch.failed"));
    failed_label_->setProperty("aidaVariant", QStringLiteral("error"));
    failed_label_->setVisible(false);
    layout->addWidget(failed_label_);
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    layout->addWidget(cancel_button_);
    connect(cancel_button_, &QPushButton::clicked, this, [this] {
        const auto workspace = workspace_.lock();
        if (!workspace) return;
        const auto orchestrator = workspace->background_decompile();
        if (orchestrator) orchestrator->request_cancel();
    });
}

void QtBatchDecompileDialog::adoptWorkspace(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace) {
    workspace_ = workspace;
}

void QtBatchDecompileDialog::poll() {
    const auto workspace = workspace_.lock();
    if (!workspace) return;
    const auto orchestrator = workspace->background_decompile();
    if (!orchestrator) return;
    const auto snap = orchestrator->run_snapshot();
    if (!snap.active) return;
    title_label_->setText(QStringLiteral("Background decompilation  |  %1")
        .arg(QString::fromStdString(workspace->identity().bin_name())));
    const auto processed = snap.completed + snap.failed + snap.cancelled;
    const float fraction = snap.total != 0
        ? static_cast<float>((std::min)(1.0,
            static_cast<double>(processed) / static_cast<double>(snap.total)))
        : 0.0f;
    progress_->setValue(static_cast<int>(fraction * 100.f));
    const auto eta_total_s = static_cast<unsigned>(
        snap.eta_s > 0.0 ? snap.eta_s + 0.5 : 0.0);
    rate_label_->setText(QStringLiteral(
        "%1 / %2 functions | %3 funcs/s | ETA %4:%5")
        .arg(processed).arg(snap.total)
        .arg(snap.rate_funcs_s, 0, 'f', 1)
        .arg(eta_total_s / 60, 2, 10, QLatin1Char('0'))
        .arg(eta_total_s % 60, 2, 10, QLatin1Char('0')));
    failed_label_->setVisible(snap.failed != 0);
    if (snap.failed != 0)
        failed_label_->setText(QStringLiteral("%1 failed").arg(snap.failed));
}

}
