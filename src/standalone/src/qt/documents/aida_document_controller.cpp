#include "qt/documents/aida_document_controller.hpp"

#include <QCoreApplication>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <filesystem>

#include "core/ui/task_center.hpp"
#include "helpers/diag_log.hpp"
#include "helpers/globals.h"
#include "qt/documents/aida_document_model.hpp"
#include "qt/editor/aida_code_document.hpp"

namespace aida::qt::documents {

namespace {

constexpr char kExitReviewPendingNotifyProperty[] = "aida.exit_review.pending_notify";

}

AidaDocumentController::AidaDocumentController(AidaDocumentModel* model, QObject* parent)
    : QObject(parent), model_(model)
{
    external_poll_timer_ = new QTimer(this);
    external_poll_timer_->setInterval(1000);
    connect(external_poll_timer_, &QTimer::timeout, this,
            &AidaDocumentController::onExternalPollTimer);
    external_poll_timer_->start();

    dispatch_sweep_timer_ = new QTimer(this);
    dispatch_sweep_timer_->setInterval(250);
    connect(dispatch_sweep_timer_, &QTimer::timeout, this,
            &AidaDocumentController::onDispatchSweepTimer);
    dispatch_sweep_timer_->start();

    recovery_checkpoint_timer_ = new QTimer(this);
    recovery_checkpoint_timer_->setInterval(1000);
    connect(recovery_checkpoint_timer_, &QTimer::timeout, this,
            &AidaDocumentController::onRecoveryCheckpointTimer);
    recovery_checkpoint_timer_->start();

    exit_review_timer_ = new QTimer(this);
    exit_review_timer_->setInterval(250);
    connect(exit_review_timer_, &QTimer::timeout, this,
            &AidaDocumentController::onExitReviewTimer);

    hookHotExit();
    auto* code_registry = &editor::AidaCodeDocumentRegistry::instance();
    connect(code_registry, &editor::AidaCodeDocumentRegistry::metadataChanged, model_,
        &AidaDocumentModel::updateFromEditor);
    connect(code_registry, &editor::AidaCodeDocumentRegistry::contentChanged, model_,
        [this](quint64 document_id, quint64) { model_->updateFromEditor(document_id); });
    connect(code_registry, &editor::AidaCodeDocumentRegistry::documentRemoved, model_,
        [this](quint64) { model_->syncFromBackend(); });
    model_->syncFromBackend();
}

AidaDocumentController::~AidaDocumentController() = default;

void AidaDocumentController::hookHotExit()
{
    if (hot_exit_hooked_)
        return;
    QCoreApplication* app = QCoreApplication::instance();
    if (!app)
        return;
    connect(app, &QCoreApplication::aboutToQuit, this, [] {
        file_tabs::write_hot_exit_snapshot_all();
    });
    hot_exit_hooked_ = true;
}

void AidaDocumentController::onExternalPollTimer()
{
    file_tabs::poll_external_changes();
    model_->syncFromBackend();
}

void AidaDocumentController::onDispatchSweepTimer()
{
    for (std::size_t index = 0; index < file_tabs::tabs.size(); ++index) {
        file_tabs::observe_document_load_dispatch_failure(static_cast<int>(index));
        file_tabs::observe_recovery_dispatch_failure(static_cast<int>(index));
    }
    file_tabs::observe_document_save_dispatch_failure(-1);
    if (file_tabs::pending_close_after_save_document_id != 0)
        file_tabs::resolve_pending_close_after_save();
    model_->syncFromBackend();
}

void AidaDocumentController::onRecoveryCheckpointTimer()
{
    if (file_tabs::is_valid_tab_index(file_tabs::active_tab))
        file_tabs::checkpoint_recovery(file_tabs::active_tab);
}

void AidaDocumentController::onExitReviewTimer()
{
    if (!file_tabs::exit_review_requested)
        return;
    pollExitReview();
}

bool AidaDocumentController::openDocument(const std::string& path, const std::string& name,
    int caret_line, int caret_column)
{
    const bool result = file_tabs::request_document_open(path, name, caret_line, caret_column);
    model_->syncFromBackend();
    return result;
}

void AidaDocumentController::openOrFocusContent(const std::string& path,
    const std::string& name, const std::string& content)
{
    file_tabs::open_or_focus(path, name, content);
    model_->syncFromBackend();
}

bool AidaDocumentController::switchTo(quint64 document_id, bool record_history)
{
    const int index = file_tabs::find_document(document_id);
    if (!file_tabs::is_valid_tab_index(index))
        return false;
    const std::uint32_t old_group = file_tabs::is_valid_tab_index(file_tabs::active_tab)
        ? file_tabs::tabs[static_cast<std::size_t>(file_tabs::active_tab)].group_id : 0;
    file_tabs::switch_to(index, record_history);
    model_->syncFromBackend();
    const std::uint32_t new_group = file_tabs::tabs[static_cast<std::size_t>(index)].group_id;
    if (old_group != new_group)
        model_->notifyStructureChanged();
    return true;
}

void AidaDocumentController::closeDocument(quint64 document_id, bool confirmed_discard)
{
    const int index = file_tabs::find_document(document_id);
    if (!file_tabs::is_valid_tab_index(index))
        return;
    file_tabs::close_tab(index, confirmed_discard);
    model_->syncFromBackend();
}

void AidaDocumentController::requestTabClose(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    if (!file_tabs::is_valid_tab_index(index))
        return;
    auto& tab = file_tabs::tabs[static_cast<std::size_t>(index)];
    if (tab.dirty) {
        file_tabs::pending_close_idx = index;
        file_tabs::show_close_confirm = true;
        Q_EMIT closeConfirmationRequested(document_id);
        Q_EMIT closeConfirmStateChanged();
        return;
    }
    file_tabs::close_tab(index);
    model_->syncFromBackend();
}

std::uint32_t AidaDocumentController::createGroupForTab(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    if (!file_tabs::is_valid_tab_index(index))
        return 0;
    const std::uint32_t old_group = file_tabs::tabs[static_cast<std::size_t>(index)].group_id;
    const std::uint32_t group = file_tabs::create_group_for_tab(index);
    model_->syncFromBackend();
    if (group != old_group)
        model_->notifyGroupMembershipChanged(document_id, old_group, group);
    model_->notifyStructureChanged();
    return group;
}

bool AidaDocumentController::moveToGroup(quint64 document_id, std::uint32_t group_id)
{
    const int index = file_tabs::find_document(document_id);
    if (!file_tabs::is_valid_tab_index(index))
        return false;
    const std::uint32_t old_group = file_tabs::tabs[static_cast<std::size_t>(index)].group_id;
    const bool moved = file_tabs::move_to_group(index, group_id);
    model_->syncFromBackend();
    if (moved && old_group != group_id)
        model_->notifyGroupMembershipChanged(document_id, old_group, group_id);
    return moved;
}

bool AidaDocumentController::navigateGroupHistory(std::uint32_t group_id, bool forward)
{
    const bool navigated = file_tabs::navigate_group_history(group_id, forward);
    model_->syncFromBackend();
    return navigated;
}

bool AidaDocumentController::reopenClosedDocument()
{
    const bool reopened = file_tabs::reopen_closed_document();
    model_->syncFromBackend();
    return reopened;
}

bool AidaDocumentController::canReopenClosedDocument() const
{
    return file_tabs::can_reopen_closed_document();
}

void AidaDocumentController::reorderDocument(quint64 source_document,
    quint64 target_document, bool insert_after)
{
    if (source_document == 0 || target_document == 0 || source_document == target_document)
        return;
    file_tabs::normalize_document_identities();
    const int source = file_tabs::find_document(source_document);
    const int target = file_tabs::find_document(target_document);
    if (!file_tabs::is_valid_tab_index(source) || !file_tabs::is_valid_tab_index(target) ||
        file_tabs::tabs[static_cast<std::size_t>(source)].group_id !=
            file_tabs::tabs[static_cast<std::size_t>(target)].group_id)
        return;
    const std::uint64_t active_document = file_tabs::is_valid_tab_index(file_tabs::active_tab)
        ? file_tabs::tabs[static_cast<std::size_t>(file_tabs::active_tab)].document_id : 0;
    std::size_t insertion = static_cast<std::size_t>(target) + (insert_after ? 1U : 0U);
    if (static_cast<std::size_t>(source) < insertion)
        --insertion;
    OpenTab moved = std::move(file_tabs::tabs[static_cast<std::size_t>(source)]);
    file_tabs::tabs.erase(file_tabs::tabs.begin() + source);
    file_tabs::tabs.insert(file_tabs::tabs.begin() +
        static_cast<std::vector<OpenTab>::difference_type>(insertion), std::move(moved));
    file_tabs::active_tab = file_tabs::find_document(active_document);
    model_->syncFromBackend();
    model_->notifyStructureChanged();
}

document_op_result_t AidaDocumentController::verifySaveGate(quint64 document_id,
    bool require_destination)
{
    const int index = file_tabs::find_document(document_id);
    const auto gate = file_tabs::verify_tab_save_gate(index, require_destination);
    return {gate.succeeded, gate.detail};
}

document_op_result_t AidaDocumentController::saveDocument(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    if (!file_tabs::is_valid_tab_index(index))
        return {false, "The document is no longer open."};
    const auto result = file_tabs::save_tab_to_disk_result(index);
    model_->syncFromBackend();
    return {result.succeeded, result.detail};
}

document_op_result_t AidaDocumentController::saveDocumentAs(quint64 document_id,
    const std::string& destination)
{
    const int index = file_tabs::find_document(document_id);
    if (!file_tabs::is_valid_tab_index(index))
        return {false, "The document is no longer open."};
    const auto result = file_tabs::save_tab_as(index, destination);
    model_->syncFromBackend();
    return {result.succeeded, result.detail};
}

document_op_result_t AidaDocumentController::saveAndClose(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    if (!file_tabs::is_valid_tab_index(index))
        return {false, "The document is no longer open."};
    file_tabs::pending_close_idx = index;
    file_tabs::pending_close_after_save_document_id = document_id;
    const auto result = file_tabs::save_tab_to_disk_result(index);
    model_->syncFromBackend();
    if (!result.succeeded) {
        file_tabs::pending_close_after_save_document_id = 0;
        if (file_tabs::is_valid_tab_index(file_tabs::pending_close_idx) &&
            file_tabs::tabs[static_cast<std::size_t>(file_tabs::pending_close_idx)].document_id == document_id)
            file_tabs::pending_close_idx = -1;
    }
    return {result.succeeded, result.detail};
}

document_op_result_t AidaDocumentController::saveAll()
{
    const auto result = file_tabs::save_all_tabs_result();
    model_->syncFromBackend();
    return {result.succeeded, result.detail};
}

bool AidaDocumentController::reloadExternal(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    const bool result = file_tabs::reload_external(index);
    model_->syncFromBackend();
    return result;
}

bool AidaDocumentController::keepEditorVersion(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    const bool result = file_tabs::keep_editor_version(index);
    model_->syncFromBackend();
    return result;
}

bool AidaDocumentController::stageExternalProposal(const std::string& path,
    const std::string& proposed_content, const std::string& origin, std::string& detail)
{
    const bool result = file_tabs::stage_external_proposal(path, proposed_content, origin, detail);
    model_->syncFromBackend();
    return result;
}

void AidaDocumentController::acceptExternalWrite(const std::string& path,
    const std::string& content)
{
    file_tabs::accept_external_write(path, content);
    model_->syncFromBackend();
}

void AidaDocumentController::requestRecoveryProbe(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    if (!file_tabs::is_valid_tab_index(index))
        return;
    file_tabs::request_recovery_probe(index);
    model_->syncFromBackend();
}

document_op_result_t AidaDocumentController::recoverFromJournal(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    const auto result = file_tabs::recover_from_journal(index);
    model_->syncFromBackend();
    return {result.succeeded, result.detail};
}

document_op_result_t AidaDocumentController::compareWithJournal(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    const auto result = file_tabs::compare_with_journal(index);
    model_->syncFromBackend();
    return {result.succeeded, result.detail};
}

document_op_result_t AidaDocumentController::compareWithDisk(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    const auto result = file_tabs::compare_with_disk(index);
    model_->syncFromBackend();
    return {result.succeeded, result.detail};
}

document_op_result_t AidaDocumentController::discardRecovery(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    const auto result = file_tabs::discard_recovery(index);
    model_->syncFromBackend();
    return {result.succeeded, result.detail};
}

document_op_result_t AidaDocumentController::requestRecoveryDiscard(quint64 document_id)
{
    const int index = file_tabs::find_document(document_id);
    const auto result = file_tabs::request_recovery_discard(index);
    model_->syncFromBackend();
    return {result.succeeded, result.detail};
}

quint64 AidaDocumentController::pendingRecoveryDiscardDocument() const
{
    return file_tabs::pending_recovery_discard_document;
}

void AidaDocumentController::clearPendingRecoveryDiscard()
{
    file_tabs::pending_recovery_discard_document = 0;
}

bool AidaDocumentController::cancelDocumentLoad(quint64 document_id)
{
    const bool cancelled = file_tabs::cancel_document_load(document_id);
    model_->syncFromBackend();
    return cancelled;
}

std::size_t AidaDocumentController::requestCloseAll()
{
    const std::size_t requested = file_tabs::request_close_all();
    model_->syncFromBackend();
    if (!file_tabs::pending_close_all_document_ids.empty())
        advanceCloseAll();
    return requested;
}

void AidaDocumentController::advanceCloseAll()
{
    file_tabs::advance_close_all();
    if (file_tabs::is_valid_tab_index(file_tabs::pending_close_idx)) {
        const auto& tab = file_tabs::tabs[static_cast<std::size_t>(file_tabs::pending_close_idx)];
        Q_EMIT closeConfirmationRequested(tab.document_id);
        Q_EMIT closeConfirmStateChanged();
    }
    model_->syncFromBackend();
}

void AidaDocumentController::resolvePendingCloseAfterSave()
{
    file_tabs::resolve_pending_close_after_save();
    model_->syncFromBackend();
}

void AidaDocumentController::cancelCloseAll()
{
    file_tabs::cancel_close_all();
    Q_EMIT closeConfirmStateChanged();
    model_->syncFromBackend();
}

document_op_result_t AidaDocumentController::requestExitReview()
{
    const auto result = file_tabs::request_exit_review();
    if (result.succeeded && !exit_review_timer_->isActive())
        exit_review_timer_->start();
    Q_EMIT exitReviewStateChanged();
    return {result.succeeded, result.detail};
}

bool AidaDocumentController::exitReviewRequested() const
{
    return file_tabs::exit_review_requested;
}

bool AidaDocumentController::exitReviewReady() const
{
    return file_tabs::exit_review_ready;
}

bool AidaDocumentController::exitReviewCommitted() const
{
    return file_tabs::exit_review_committed;
}

void AidaDocumentController::pollExitReview()
{
    const bool was_ready = file_tabs::exit_review_ready;
    file_tabs::poll_exit_review();
    const qint64 pending_id = pendingCloseDocumentId();
    const qint64 notified_id = property(kExitReviewPendingNotifyProperty).toLongLong();
    if (pending_id != notified_id) {
        setProperty(kExitReviewPendingNotifyProperty, pending_id);
        if (pending_id >= 0)
            Q_EMIT closeConfirmationRequested(static_cast<quint64>(pending_id));
    }
    if (file_tabs::exit_review_ready != was_ready || file_tabs::exit_review_committed)
        Q_EMIT exitReviewStateChanged();
    model_->syncFromBackend();
}

bool AidaDocumentController::consumeExitReviewReady()
{
    const bool ready = file_tabs::consume_exit_review_ready();
    if (ready) {
        exit_review_timer_->stop();
        Q_EMIT exitReviewStateChanged();
    }
    return ready;
}

void AidaDocumentController::resolveExitReviewDocument(quint64 document_id, quint64 revision,
    bool confirmed_discard)
{
    file_tabs::resolve_exit_review_document(document_id, revision, confirmed_discard);
    model_->syncFromBackend();
    Q_EMIT exitReviewStateChanged();
}

document_op_result_t AidaDocumentController::beginExitDiscardCleanup(quint64 document_id,
    quint64 revision)
{
    const auto result = file_tabs::begin_exit_discard_cleanup(document_id, revision);
    return {result.succeeded, result.detail};
}

void AidaDocumentController::failExitDiscardCleanup(quint64 document_id, std::string detail)
{
    file_tabs::fail_exit_discard_cleanup(document_id, std::move(detail));
    model_->syncFromBackend();
    Q_EMIT exitReviewStateChanged();
}

document_op_result_t AidaDocumentController::restoreSession(const std::string& serialized,
    int legacy_active_index)
{
    const auto result = file_tabs::restore_programming_session(serialized, legacy_active_index);
    model_->syncFromBackend();
    model_->notifyStructureChanged();
    return {result.succeeded, result.detail};
}

std::string AidaDocumentController::serializeSession()
{
    return file_tabs::serialize_programming_session();
}

qint64 AidaDocumentController::pendingCloseDocumentId() const
{
    return file_tabs::is_valid_tab_index(file_tabs::pending_close_idx)
        ? static_cast<qint64>(file_tabs::tabs[static_cast<std::size_t>(file_tabs::pending_close_idx)].document_id)
        : -1;
}

bool AidaDocumentController::showCloseConfirm() const
{
    return file_tabs::show_close_confirm;
}

void AidaDocumentController::clearPendingClose()
{
    file_tabs::pending_close_idx = -1;
    file_tabs::show_close_confirm = false;
    file_tabs::close_confirm_error.clear();
    setProperty(kExitReviewPendingNotifyProperty, static_cast<qint64>(-1));
    Q_EMIT closeConfirmStateChanged();
}

std::string AidaDocumentController::closeConfirmError() const
{
    return file_tabs::close_confirm_error;
}

void AidaDocumentController::finishCloseAllDocument(quint64 document_id)
{
    file_tabs::finish_close_all_document(document_id);
    model_->syncFromBackend();
}

std::string AidaDocumentController::labelForGroup(std::uint32_t group_id) const
{
    const OpenTab* single = nullptr;
    std::size_t count = 0;
    for (const auto& tab : file_tabs::tabs) {
        if (tab.group_id != group_id)
            continue;
        single = &tab;
        ++count;
    }
    if (count == 1 && single) {
        std::string label = single->filename.empty() ? "Untitled" : single->filename;
        if (single->dirty)
            label.append(" *");
        if (single->pinned)
            label.append(" [Pinned]");
        if (single->external_conflict)
            label.append(" [Disk conflict]");
        if (single->proposal_pending)
            label.append(" [Review]");
        return label;
    }
    std::string label = "Editor";
    if (group_id != 0)
        label.append(" - Group ").append(std::to_string(group_id + 1));
    return label;
}

std::vector<std::uint32_t> AidaDocumentController::currentGroups() const
{
    std::vector<std::uint32_t> result;
    for (const auto& tab : file_tabs::tabs) {
        if (std::find(result.begin(), result.end(), tab.group_id) == result.end())
            result.push_back(tab.group_id);
    }
    return result;
}

std::uint32_t AidaDocumentController::activeGroup() const
{
    return file_tabs::is_valid_tab_index(file_tabs::active_tab)
        ? file_tabs::tabs[static_cast<std::size_t>(file_tabs::active_tab)].group_id : 0;
}

bool AidaDocumentController::prepareGroupClose(std::uint32_t group_id, std::string& reason)
{
    for (const auto& tab : file_tabs::tabs) {
        if (tab.group_id != group_id)
            continue;
        if (tab.pinned) {
            reason = "Unpin every document in this editor group before closing it";
            return false;
        }
        if (tab.dirty) {
            const int index = file_tabs::find_document(tab.document_id);
            if (index >= 0) {
                file_tabs::pending_close_idx = index;
                file_tabs::show_close_confirm = true;
                Q_EMIT closeConfirmationRequested(tab.document_id);
            }
            reason = "Review the unsaved document before closing this editor group";
            return false;
        }
    }
    return true;
}

void AidaDocumentController::closeGroup(std::uint32_t group_id)
{
    for (int index = static_cast<int>(file_tabs::tabs.size()) - 1; index >= 0; --index)
        if (file_tabs::tabs[static_cast<std::size_t>(index)].group_id == group_id)
            file_tabs::close_tab(index);
    model_->syncFromBackend();
}

void AidaDocumentController::writeHotExitSnapshots()
{
    file_tabs::write_hot_exit_snapshot_all();
}

}
