#include "qt/documents/aida_document_model.hpp"

#include <QTimer>

#include <algorithm>
#include <filesystem>

#include "helpers/globals.h"

namespace aida::qt::documents {

namespace {

bool flags_any(document_change_flags_t flags, document_change_flags_t mask)
{
    return (static_cast<quint32>(flags) & static_cast<quint32>(mask)) != 0u;
}

bool flags_only(document_change_flags_t flags, document_change_flags_t mask)
{
    return (static_cast<quint32>(flags) & ~static_cast<quint32>(mask)) == 0u;
}

}

AidaDocumentModel::AidaDocumentModel(QObject* parent) : QObject(parent)
{
    caret_timer_ = new QTimer(this);
    caret_timer_->setSingleShot(true);
    caret_timer_->setInterval(150);
    connect(caret_timer_, &QTimer::timeout, this, [this] {
        const auto pending = pending_caret_emit_;
        pending_caret_emit_.clear();
        for (const quint64 id : pending) {
            const OpenTab* tab = recordAt(findDocument(id));
            if (!tab)
                continue;
            snapshots_.insert(id, snapshotOf(*tab));
            Q_EMIT documentChanged(id, document_change_t::caret | document_change_t::scroll |
                document_change_t::selection);
        }
    });
}

AidaDocumentModel::~AidaDocumentModel() = default;

int AidaDocumentModel::recordCount() const
{
    return static_cast<int>(file_tabs::tabs.size());
}

const OpenTab* AidaDocumentModel::recordAt(int index) const
{
    if (!isValidIndex(index))
        return nullptr;
    return &file_tabs::tabs[static_cast<std::size_t>(index)];
}

OpenTab* AidaDocumentModel::recordAt(int index)
{
    if (!isValidIndex(index))
        return nullptr;
    return &file_tabs::tabs[static_cast<std::size_t>(index)];
}

int AidaDocumentModel::findDocument(quint64 document_id) const
{
    return file_tabs::find_document(document_id);
}

int AidaDocumentModel::findPathDocument(const std::string& path) const
{
    return file_tabs::find_path_document(path);
}

bool AidaDocumentModel::isValidIndex(int index) const
{
    return file_tabs::is_valid_tab_index(index);
}

int AidaDocumentModel::activeDocument() const
{
    return file_tabs::active_tab;
}

quint64 AidaDocumentModel::activeDocumentId() const
{
    return file_tabs::is_valid_tab_index(file_tabs::active_tab)
        ? file_tabs::tabs[static_cast<std::size_t>(file_tabs::active_tab)].document_id : 0;
}

std::vector<std::uint32_t> AidaDocumentModel::groups() const
{
    std::vector<std::uint32_t> result;
    for (const auto& tab : file_tabs::tabs) {
        if (std::find(result.begin(), result.end(), tab.group_id) == result.end())
            result.push_back(tab.group_id);
    }
    return result;
}

int AidaDocumentModel::activeInGroup(std::uint32_t group_id)
{
    return file_tabs::active_in_group(group_id);
}

quint64 AidaDocumentModel::activeDocumentInGroup(std::uint32_t group_id)
{
    const auto found = file_tabs::active_document_by_group.find(group_id);
    return found == file_tabs::active_document_by_group.end() ? 0 : found->second;
}

std::size_t AidaDocumentModel::groupDocumentCount(std::uint32_t group_id) const
{
    std::size_t count = 0;
    for (const auto& tab : file_tabs::tabs)
        if (tab.group_id == group_id)
            ++count;
    return count;
}

bool AidaDocumentModel::closeReviewInProgress() const
{
    return file_tabs::close_review_in_progress();
}

AidaDocumentModel::record_snapshot_t AidaDocumentModel::snapshotOf(const OpenTab& tab)
{
    record_snapshot_t snapshot;
    snapshot.revision = tab.revision;
    snapshot.content_hash = tab.content_hash;
    snapshot.dirty = tab.dirty;
    snapshot.caret_line = tab.caret_line;
    snapshot.caret_column = tab.caret_column;
    snapshot.anchor_line = tab.selection_anchor_line;
    snapshot.anchor_column = tab.selection_anchor_column;
    snapshot.selection_active = tab.selection_active;
    snapshot.scroll_x = tab.scroll_x;
    snapshot.scroll_y = tab.scroll_y;
    std::size_t folds_signature = tab.folded_lines.size();
    for (const int line : tab.folded_lines)
        folds_signature = folds_signature * 1315423911u + static_cast<std::size_t>(line);
    snapshot.folds_signature = folds_signature;
    snapshot.proposal_pending = tab.proposal_pending;
    snapshot.load_in_progress = tab.load_in_progress;
    snapshot.load_failed = tab.load_failed;
    snapshot.save_in_progress = tab.save_in_progress;
    snapshot.buffer_loaded = tab.buffer_loaded;
    snapshot.external_conflict = tab.external_conflict;
    snapshot.recovery_available = tab.recovery.available;
    snapshot.recovery_operation_pending = tab.recovery_operation_pending;
    snapshot.recovery_checkpoint_pending = tab.recovery_checkpoint_pending;
    snapshot.pinned = tab.pinned;
    snapshot.language_override = tab.language_override;
    snapshot.save_error = tab.save_error;
    snapshot.load_error = tab.load_error;
    snapshot.recovery_error = tab.recovery_error;
    return snapshot;
}

document_change_flags_t AidaDocumentModel::diffOf(quint64 document_id,
    const record_snapshot_t& before, const record_snapshot_t& after) const
{
    static_cast<void>(document_id);
    document_change_flags_t flags;
    if (before.dirty != after.dirty || before.content_hash != after.content_hash)
        flags |= document_change_t::dirty;
    if (before.revision != after.revision)
        flags |= document_change_t::content;
    if (before.caret_line != after.caret_line || before.caret_column != after.caret_column)
        flags |= document_change_t::caret;
    if (before.scroll_x != after.scroll_x || before.scroll_y != after.scroll_y)
        flags |= document_change_t::scroll;
    if (before.anchor_line != after.anchor_line || before.anchor_column != after.anchor_column ||
        before.selection_active != after.selection_active)
        flags |= document_change_t::selection;
    if (before.folds_signature != after.folds_signature)
        flags |= document_change_t::folds;
    if (before.recovery_available != after.recovery_available ||
        before.recovery_operation_pending != after.recovery_operation_pending ||
        before.recovery_checkpoint_pending != after.recovery_checkpoint_pending ||
        before.recovery_error != after.recovery_error)
        flags |= document_change_t::recovery;
    if (before.load_in_progress != after.load_in_progress ||
        before.load_failed != after.load_failed ||
        before.buffer_loaded != after.buffer_loaded ||
        before.load_error != after.load_error)
        flags |= document_change_t::load;
    if (before.save_in_progress != after.save_in_progress ||
        before.save_error != after.save_error)
        flags |= document_change_t::save;
    if (before.external_conflict != after.external_conflict)
        flags |= document_change_t::external_conflict;
    if (before.proposal_pending != after.proposal_pending)
        flags |= document_change_t::proposal;
    if (before.language_override != after.language_override)
        flags |= document_change_t::language;
    if (before.pinned != after.pinned)
        flags |= document_change_t::pinned;
    return flags;
}

void AidaDocumentModel::syncFromBackend()
{
    QSet<quint64> present;
    for (const auto& tab : file_tabs::tabs)
        present.insert(tab.document_id);
    for (const quint64 id : known_ids_) {
        if (!present.contains(id)) {
            Q_EMIT documentAboutToBeRemoved(id);
            pending_caret_emit_.remove(id);
            snapshots_.remove(id);
            group_of_.remove(id);
            Q_EMIT documentRemoved(id);
        }
    }
    for (auto& tab : file_tabs::tabs) {
        const quint64 id = tab.document_id;
        if (!known_ids_.contains(id)) {
            known_ids_.insert(id);
            group_of_.insert(id, tab.group_id);
            snapshots_.insert(id, snapshotOf(tab));
            Q_EMIT documentAdded(id);
            continue;
        }
        const auto before = snapshots_.value(id);
        const auto after = snapshotOf(tab);
        const auto flags = diffOf(id, before, after);
        snapshots_.insert(id, after);
        const auto move_mask = document_change_t::caret | document_change_t::scroll |
            document_change_t::selection;
        if (flags_any(flags, move_mask) && flags_only(flags, move_mask)) {
            pending_caret_emit_.insert(id);
            if (!caret_timer_->isActive())
                caret_timer_->start();
            continue;
        }
        if (flags != document_change_t::none)
            Q_EMIT documentChanged(id, flags);
        const auto group_before = group_of_.value(id, 0);
        if (group_before != tab.group_id) {
            group_of_.insert(id, tab.group_id);
            Q_EMIT groupMembershipChanged(id, group_before, tab.group_id);
        }
    }
    known_ids_ = present;
    const quint64 active_id = activeDocumentId();
    const int active_idx = activeDocument();
    const std::uint32_t active_group = isValidIndex(active_idx)
        ? file_tabs::tabs[static_cast<std::size_t>(active_idx)].group_id : 0;
    if (active_id != last_active_document_ || active_group != last_active_group_) {
        last_active_document_ = active_id;
        last_active_group_ = active_group;
        Q_EMIT activeDocumentChanged(active_id, active_group);
    }
}

void AidaDocumentModel::updateFromEditor(quint64 document_id)
{
    const int index = findDocument(document_id);
    if (!isValidIndex(index))
        return;
    OpenTab* tab = recordAt(index);
    if (!tab)
        return;
    const auto metadata = code_editor_widget::document_metadata(document_id);
    if (!metadata.found)
        return;
    tab->dirty = metadata.dirty;
    tab->caret_line = metadata.caret_line;
    tab->caret_column = metadata.caret_column;
    tab->selection_anchor_line = metadata.selection_anchor_line;
    tab->selection_anchor_column = metadata.selection_anchor_column;
    tab->selection_active = metadata.selection_active;
    tab->scroll_x = metadata.scroll_x;
    tab->scroll_y = metadata.scroll_y;
    tab->folded_lines = metadata.folded_lines;
    tab->language_override = metadata.language_override;
    tab->revision = metadata.revision;
    tab->proposal_pending = metadata.proposal_pending;
    if (metadata.dirty)
        tab->content_hash = 0;
    const auto before = snapshots_.value(document_id);
    const auto after = snapshotOf(*tab);
    const auto flags = diffOf(document_id, before, after);
    snapshots_.insert(document_id, after);
    if (flags == document_change_t::none)
        return;
    const auto move_mask = document_change_t::caret | document_change_t::scroll |
        document_change_t::selection;
    if (flags_any(flags, move_mask) && flags_only(flags, move_mask)) {
        pending_caret_emit_.insert(document_id);
        if (!caret_timer_->isActive())
            caret_timer_->start();
        return;
    }
    Q_EMIT documentChanged(document_id, flags);
}

void AidaDocumentModel::notifyChanged(quint64 document_id, document_change_flags_t flags)
{
    const int index = findDocument(document_id);
    if (!isValidIndex(index))
        return;
    snapshots_.insert(document_id, snapshotOf(*recordAt(index)));
    Q_EMIT documentChanged(document_id, flags);
}

void AidaDocumentModel::notifyStructureChanged()
{
    Q_EMIT structureChanged();
}

void AidaDocumentModel::notifyGroupMembershipChanged(quint64 document_id,
    std::uint32_t old_group, std::uint32_t new_group)
{
    Q_EMIT groupMembershipChanged(document_id, old_group, new_group);
}

}
