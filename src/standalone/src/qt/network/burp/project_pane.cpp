#include "qt/network/burp/project_pane.hpp"

#include <QAbstractButton>
#include <QCheckBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <atomic>
#include <exception>
#include <string>
#include <utility>

#include "core/infra/executor.hpp"
#include "core/network/burp/project.hpp"
#include "core/ui/task_center.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"

namespace aida::qt::net {

ProjectPane::ProjectPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.project"));
    setRequiresTarget(false);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* titleLabel = new QLabel("Burp Project", content);
    titleLabel->setProperty("aidaTone", QStringLiteral("title"));
    layout->addWidget(titleLabel);

    auto* separator = new QFrame(content);
    separator->setFrameShape(QFrame::HLine);
    separator->setProperty("aidaRole", QStringLiteral("separator"));
    layout->addWidget(separator);

    auto* pathRow = new QHBoxLayout();
    pathRow->setSpacing(t.spacing.sm);
    pathEdit_ = new QLineEdit("aida-burp-project.json", content);
    pathEdit_->setPlaceholderText("Project file path");
    pathEdit_->setMaxLength(1023);
    pathRow->addWidget(pathEdit_, 1);
    layout->addLayout(pathRow);

    replaceCheck_ = new QCheckBox("Replace existing state when loading", content);
    replaceCheck_->setChecked(true);
    layout->addWidget(replaceCheck_);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(t.spacing.sm);
    saveButton_ = new widgets::AidaButton("Save Project", content);
    saveButton_->setKind(widgets::AidaButton::Kind::Primary);
    saveButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(saveButton_);
    loadButton_ = new widgets::AidaButton("Load Project", content);
    loadButton_->setKind(widgets::AidaButton::Kind::Secondary);
    loadButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(loadButton_);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    statusLabel_ = new QLabel(content);
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);
    layout->addStretch(1);

    connect(saveButton_, &QAbstractButton::clicked, this, [this] { startOperation(true); });
    connect(loadButton_, &QAbstractButton::clicked, this, [this] { startOperation(false); });
    connect(pathEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        const bool enabled = !running_.load(std::memory_order_acquire) && !text.isEmpty();
        saveButton_->setEnabled(enabled);
        loadButton_->setEnabled(enabled);
    });

    setContent(content);
}

ProjectPane::~ProjectPane() = default;

void ProjectPane::startOperation(bool save) {
    if (running_.exchange(true, std::memory_order_acq_rel))
        return;
    const std::string path = pathEdit_->text().toStdString();
    const bool replaceExisting = replaceCheck_->isChecked();
    statusLabel_->setText(save ? "Saving project..." : "Loading project...");
    set_label_tone(statusLabel_, "secondary");
    succeeded_ = false;
    saveButton_->setEnabled(false);
    loadButton_->setEnabled(false);
    const std::uint64_t serial = operationSerial_.fetch_add(1, std::memory_order_acq_rel) + 1;

    static std::atomic<std::uint64_t> operationSequence{1};
    const std::string taskId = "network.project." + std::to_string(
        operationSequence.fetch_add(1, std::memory_order_acq_rel));
    aida::ui::task_center::task_registration_t registration;
    registration.id = taskId;
    registration.source = "network.project";
    registration.owner = "network.project";
    registration.owner_view = "view.network.project";
    registration.owner_action = save ? "save" : "load";
    registration.target = path;
    registration.label = save ? "Save Burp project" : "Load Burp project";
    registration.stage = save ? "Serializing project" : "Restoring project";
    if (!aida::ui::task_center::register_task(std::move(registration))) {
        statusLabel_->setText("Task Center rejected the project operation");
        running_.store(false, std::memory_order_release);
        saveButton_->setEnabled(true);
        loadButton_->setEnabled(true);
        return;
    }
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.project";
    submission.label = save ? "network.project.save" : "network.project.load";
    submission.thread_class = "blocking_file_io";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 4;
    submission.ui_access_policy = "immutable_snapshots_only";
    submission.failure_policy = "typed_diagnostic";
    submission.body = [this, save, path, replaceExisting, taskId, serial] {
        bool ok = false;
        std::string detail;
        try {
            ok = save ? aida::burp::project::save_to_file(path)
                      : aida::burp::project::load_from_file(path, replaceExisting);
            detail = ok ? (save ? "Project saved" : "Project loaded") : aida::burp::project::last_error();
        } catch (const std::exception& exception) {
            detail = exception.what();
        } catch (...) {
            detail = "Project operation failed with an unknown exception";
        }
        QMetaObject::invokeMethod(this,
            [this, save, ok, detail = std::move(detail), taskId, serial]() mutable {
                applyResult(save, ok, QString::fromStdString(detail), serial);
                static_cast<void>(aida::ui::task_center::update_task(taskId,
                    ok ? aida::ui::task_center::task_state_t::completed
                       : aida::ui::task_center::task_state_t::failed,
                    1.0f, ok ? "Completed" : "Failed", detail,
                    ok ? std::string{} : "network.project.operation.failure." + taskId));
            }, Qt::QueuedConnection);
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        statusLabel_->setText(QString::fromStdString(
            "Project operation could not be scheduled: " + submitted.reject_reason));
        running_.store(false, std::memory_order_release);
        saveButton_->setEnabled(true);
        loadButton_->setEnabled(true);
        static_cast<void>(aida::ui::task_center::update_task(taskId,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Scheduling failed", "Project operation could not be scheduled",
            "network.project.schedule.failure." + taskId));
        return;
    }
    static_cast<void>(aida::ui::task_center::update_task(taskId,
        aida::ui::task_center::task_state_t::running, -1.0f,
        save ? "Serializing project" : "Restoring project"));
}

void ProjectPane::applyResult(bool save, bool ok, QString detail, std::uint64_t serial) {
    static_cast<void>(save);
    if (serial != operationSerial_.load(std::memory_order_acquire))
        return;
    succeeded_ = ok;
    statusLabel_->setText(detail.isEmpty() ? "Project operation failed" : detail);
    set_label_tone(statusLabel_, ok ? "success" : "error");
    running_.store(false, std::memory_order_release);
    const bool enabled = !pathEdit_->text().isEmpty();
    saveButton_->setEnabled(enabled);
    loadButton_->setEnabled(enabled);
}

}
