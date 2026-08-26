#include "core/editor/code_editor.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/editor/aida_code_document.hpp"

namespace {

using aida::qt::editor::AidaCodeDocument;
using aida::qt::editor::AidaCodeDocumentRegistry;

AidaCodeDocumentRegistry& registry()
{
    return AidaCodeDocumentRegistry::instance();
}

AidaCodeDocument* bound_document()
{
    registry().bindFocused();
    return registry().find(registry().activeDocumentId());
}

std::string clipboard_text_normalized()
{
    const QString text = aida::qt::clipboard::text();
    if (text.isEmpty())
        return {};
    std::string result = text.toUtf8().toStdString();
    std::string normalized;
    normalized.reserve(result.size());
    for (std::size_t i = 0; i < result.size(); i++) {
        if (result[i] == '\r') continue;
        normalized += result[i];
    }
    return normalized;
}

}

void code_editor_widget::init() {
    (void)registry();
    diag::log_tagged("editor", "init registry-backed");
}

bool code_editor_widget::load_document(std::uint64_t document_id, std::uint64_t revision,
        std::string_view content, std::string_view filename, std::string_view filepath,
        bool dirty, int caret_line, int caret_column, float scroll_x, float scroll_y,
        bool replace_existing, int selection_anchor_line, int selection_anchor_column,
        bool selection_active, const std::vector<int>& folded_lines,
        std::string_view language_override) {
    return registry().loadDocument(document_id, revision, content, filename, filepath, dirty,
        caret_line, caret_column, scroll_x, scroll_y, replace_existing, selection_anchor_line,
        selection_anchor_column, selection_active, folded_lines, language_override);
}

code_editor_widget::document_state_t code_editor_widget::document_state() {
    return document_state(active_document_id());
}

code_editor_widget::document_state_t code_editor_widget::document_state(
        std::uint64_t document_id) {
    return registry().state(document_id);
}

code_editor_widget::document_capabilities_t code_editor_widget::document_capabilities() {
    AidaCodeDocument* document = bound_document();
    return document ? document->capabilities() : document_capabilities_t{};
}

bool code_editor_widget::request_document_action(document_action_t action) {
    AidaCodeDocument* document = bound_document();
    if (!document || !document->active()) {
        return false;
    }
    const auto capabilities = document->capabilities();
    if (action == document_action_t::copy_path && document->filepath().empty()) {
        return false;
    }
    if (action == document_action_t::toggle_line_comment && !capabilities.line_comment) {
        return false;
    }
    const bool mutates = action == document_action_t::duplicate_line ||
        action == document_action_t::delete_line ||
        action == document_action_t::move_line_up ||
        action == document_action_t::move_line_down ||
        action == document_action_t::toggle_line_comment ||
        action == document_action_t::trim_trailing_whitespace;
    if (mutates && document->readOnly())
        return false;
    const std::uint32_t index = static_cast<std::uint32_t>(action);
    if (index > static_cast<std::uint32_t>(document_action_t::trim_trailing_whitespace))
        return false;
    return document->performDocumentAction(action);
}

void code_editor_widget::get_caret(int& line, int& col) {
    AidaCodeDocument* document = bound_document();
    if (!document) { line = 0; col = 0; return; }
    line = document->selection().caret_line;
    col = document->selection().caret_col;
}

void code_editor_widget::set_caret(int line, int col) {
    AidaCodeDocument* document = bound_document();
    if (!document) return;
    document->setCaret(line, col);
}

void code_editor_widget::get_scroll(float& x, float& y) {
    AidaCodeDocument* document = bound_document();
    if (!document) { x = 0.f; y = 0.f; return; }
    x = document->scrollX();
    y = document->scrollY();
}

void code_editor_widget::set_scroll(float x, float y) {
    AidaCodeDocument* document = bound_document();
    if (!document) return;
    document->setScroll(x, y);
}

void code_editor_widget::discard_document_state(std::uint64_t document_id) {
    registry().discard(document_id);
}

bool code_editor_widget::select_document_for_actions(std::uint64_t document_id) {
    return registry().selectForActions(document_id);
}

std::uint64_t code_editor_widget::active_document_id() {
    return registry().activeDocumentId();
}

std::uint64_t code_editor_widget::document_revision() {
    AidaCodeDocument* document = bound_document();
    return document ? document->revision() : 0;
}

std::uint64_t code_editor_widget::document_revision(std::uint64_t document_id) {
    AidaCodeDocument* document = registry().find(document_id);
    return document ? document->revision() : 0;
}

std::string code_editor_widget::document_content(std::uint64_t document_id) {
    AidaCodeDocument* document = registry().find(document_id);
    return document ? document->serializedContent() : std::string{};
}

std::uint64_t code_editor_widget::document_content_fingerprint(std::uint64_t document_id) {
    AidaCodeDocument* document = registry().find(document_id);
    return document ? document->contentFingerprint() : 0;
}

bool code_editor_widget::get_document_caret(std::uint64_t document_id, int& line, int& col) {
    AidaCodeDocument* document = registry().find(document_id);
    if (!document) return false;
    line = document->selection().caret_line;
    col = document->selection().caret_col;
    return true;
}

bool code_editor_widget::set_document_caret(std::uint64_t document_id, int line, int col) {
    AidaCodeDocument* document = registry().find(document_id);
    if (!document) return false;
    registry().bindForActions(document_id);
    document->setCaret(line, col);
    return true;
}

bool code_editor_widget::get_document_scroll(std::uint64_t document_id, float& x, float& y) {
    AidaCodeDocument* document = registry().find(document_id);
    if (!document) return false;
    x = document->scrollX();
    y = document->scrollY();
    return true;
}

bool code_editor_widget::set_document_scroll(std::uint64_t document_id, float x, float y) {
    AidaCodeDocument* document = registry().find(document_id);
    if (!document) return false;
    document->setScroll(x, y);
    return true;
}

code_editor_widget::document_metadata_snapshot_t code_editor_widget::document_metadata(
        std::uint64_t document_id) {
    return registry().metadata(document_id);
}

bool code_editor_widget::set_document_language_override(std::uint64_t document_id,
        std::string_view language) {
    AidaCodeDocument* document = registry().find(document_id);
    return document ? document->setLanguageOverride(language) : false;
}

bool code_editor_widget::toggle_document_fold(std::uint64_t document_id, int line) {
    AidaCodeDocument* document = registry().find(document_id);
    return document ? document->toggleFold(line) : false;
}

code_editor_widget::document_payload_snapshot_t code_editor_widget::document_payload(
        std::uint64_t document_id, std::uint64_t expected_revision) {
    return registry().payload(document_id, expected_revision);
}

std::string code_editor_widget::caret_identifier() {
    AidaCodeDocument* document = bound_document();
    return document ? document->caretIdentifier() : std::string{};
}

bool code_editor_widget::document_dirty(std::uint64_t document_id) {
    AidaCodeDocument* document = registry().find(document_id);
    return document && document->dirty();
}

void code_editor_widget::mark_document_saved(std::uint64_t document_id, std::uint64_t revision,
        std::string_view filename, std::string_view filepath) {
    AidaCodeDocument* document = registry().find(document_id);
    if (document)
        document->markSaved(revision, filename, filepath);
}

bool code_editor_widget::request_streamed_document(std::uint64_t document_id,
        std::uint64_t revision, std::string_view filename, std::string_view filepath,
        std::uint64_t byte_length) {
    return registry().requestStreamedDocument(document_id, revision, filename, filepath,
        byte_length);
}

void code_editor_widget::trigger_undo() {
    AidaCodeDocument* document = bound_document();
    if (document && !document->readOnly()) document->undo();
}

void code_editor_widget::trigger_redo() {
    AidaCodeDocument* document = bound_document();
    if (document && !document->readOnly()) document->redo();
}

void code_editor_widget::trigger_cut() {
    AidaCodeDocument* document = bound_document();
    if (!document || document->readOnly()) return;
    const std::string selected = document->selectedText();
    if (!selected.empty()) {
        aida::qt::clipboard::set_text(QString::fromStdString(selected));
        document->deleteSelection();
    }
}

void code_editor_widget::trigger_copy() {
    AidaCodeDocument* document = bound_document();
    if (!document) return;
    const std::string selected = document->selectedText();
    if (!selected.empty())
        aida::qt::clipboard::set_text(QString::fromStdString(selected));
}

void code_editor_widget::trigger_paste() {
    AidaCodeDocument* document = bound_document();
    if (!document || document->readOnly()) return;
    const std::string pasted = clipboard_text_normalized();
    if (!pasted.empty())
        document->insertTextAtCaret(pasted);
}

void code_editor_widget::trigger_delete() {
    AidaCodeDocument* document = bound_document();
    if (document && !document->readOnly()) document->deleteForward();
}

void code_editor_widget::trigger_select_all() {
    AidaCodeDocument* document = bound_document();
    if (!document) return;
    document->selection().anchor_line = 0;
    document->selection().anchor_col = 0;
    document->selection().caret_line = document->lineCount() - 1;
    document->selection().caret_col = document->lineLength(document->selection().caret_line);
    document->selection().active = true;
}

void code_editor_widget::open_find() {
    const std::uint64_t id = active_document_id();
    if (id != 0) registry().openFind(id);
}

void code_editor_widget::open_replace() {
    const std::uint64_t id = active_document_id();
    if (id != 0) registry().openReplace(id);
}

void code_editor_widget::open_goto_line() {
    const std::uint64_t id = active_document_id();
    if (id != 0) registry().openGotoLine(id);
}

bool code_editor_widget::can_undo() {
    AidaCodeDocument* document = bound_document();
    return document && document->canUndo();
}

bool code_editor_widget::can_redo() {
    AidaCodeDocument* document = bound_document();
    return document && document->canRedo();
}

bool code_editor_widget::can_paste() {
    return !clipboard_text_normalized().empty();
}

bool code_editor_widget::has_selection() {
    AidaCodeDocument* document = bound_document();
    return document && document->selection().has_selection();
}

std::string code_editor_widget::selected_text(std::size_t maximum_bytes) {
    AidaCodeDocument* document = bound_document();
    return document ? document->selectedTextCapped(maximum_bytes) : std::string{};
}

bool code_editor_widget::selected_range(int& start_line, int& start_column,
        int& end_line, int& end_column) {
    AidaCodeDocument* document = bound_document();
    if (!document || !document->selection().has_selection())
        return false;
    document->selectionOrdered(start_line, start_column, end_line, end_column);
    start_line = document->clampLine(start_line);
    end_line = document->clampLine(end_line);
    start_column = document->clampCol(start_line, start_column);
    end_column = document->clampCol(end_line, end_column);
    return true;
}

std::string code_editor_widget::last_error() {
    AidaCodeDocument* document = bound_document();
    return document ? document->lastError() : std::string{};
}

std::uint64_t code_editor_widget::document_content_fingerprint() {
    AidaCodeDocument* document = bound_document();
    return document ? document->contentFingerprint() : 0;
}

bool code_editor_widget::begin_agent_edit(std::string_view origin) {
    AidaCodeDocument* document = bound_document();
    return document && document->beginAgentEdit(origin);
}

bool code_editor_widget::propose_full_content(std::string_view new_content) {
    AidaCodeDocument* document = bound_document();
    return document && document->proposeFullContent(new_content);
}

bool code_editor_widget::propose_document_content(std::uint64_t document_id,
        std::uint64_t base_revision, std::uint64_t base_content_hash,
        std::string_view current_content, std::string_view new_content,
        std::string_view origin) {
    AidaCodeDocument* document = registry().find(document_id);
    return document && document->proposeContent(base_revision, base_content_hash,
        current_content, new_content, origin);
}

bool code_editor_widget::propose_replace_range(int start_line, int end_line,
        std::string_view replacement) {
    AidaCodeDocument* document = bound_document();
    return document && document->proposeReplaceRange(start_line, end_line, replacement);
}

bool code_editor_widget::has_pending_diff() {
    AidaCodeDocument* document = bound_document();
    return document && document->hasPendingDiff();
}

const code_editor_widget::pending_diff_t& code_editor_widget::pending_diff() {
    AidaCodeDocument* document = bound_document();
    if (document) return document->pendingDiff();
    static const pending_diff_t empty{};
    return empty;
}

int code_editor_widget::pending_hunk_count() {
    AidaCodeDocument* document = bound_document();
    return document ? document->pendingHunkCount() : 0;
}

bool code_editor_widget::has_pending_review_hunks() {
    AidaCodeDocument* document = bound_document();
    return document && document->hasPendingReviewHunks();
}

code_editor_widget::review_hunk_identity_t code_editor_widget::review_hunk_identity(int index) {
    AidaCodeDocument* document = bound_document();
    return document ? document->reviewHunkIdentity(index) : review_hunk_identity_t{};
}

code_editor_widget::review_hunk_identity_t code_editor_widget::selected_review_hunk_identity() {
    AidaCodeDocument* document = bound_document();
    return document ? document->selectedReviewHunkIdentity() : review_hunk_identity_t{};
}

int code_editor_widget::resolve_review_hunk(const review_hunk_identity_t& identity,
        bool require_pending) {
    AidaCodeDocument* document = registry().find(identity.document_id);
    return document ? document->resolveReviewHunk(identity, require_pending) : -1;
}

bool code_editor_widget::select_review_hunk(int index, bool request_focus) {
    AidaCodeDocument* document = bound_document();
    return document && document->selectReviewHunk(index, request_focus);
}

bool code_editor_widget::select_next_pending_hunk() {
    AidaCodeDocument* document = bound_document();
    return document && document->selectNextPendingHunk();
}

bool code_editor_widget::select_previous_pending_hunk() {
    AidaCodeDocument* document = bound_document();
    return document && document->selectPreviousPendingHunk();
}

bool code_editor_widget::accept_hunk(int index) {
    AidaCodeDocument* document = bound_document();
    return document && document->acceptHunk(index);
}

bool code_editor_widget::reject_hunk(int index) {
    AidaCodeDocument* document = bound_document();
    return document && document->rejectHunk(index);
}

void code_editor_widget::accept_all() {
    AidaCodeDocument* document = bound_document();
    if (document) document->acceptAllHunks();
}

void code_editor_widget::reject_all() {
    AidaCodeDocument* document = bound_document();
    if (document) document->rejectAllHunks();
}

bool code_editor_widget::commit_resolved_diff() {
    AidaCodeDocument* document = bound_document();
    return document && document->commitResolvedDiff();
}

void code_editor_widget::cancel_agent_edit() {
    AidaCodeDocument* document = bound_document();
    if (document) document->cancelAgentEdit();
}
