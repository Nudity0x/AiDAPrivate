#pragma once

#include "core/editor/code_editor.hpp"
#include "core/editor/syntax_highlight.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QTimer;

namespace aida::qt::editor {

struct mapped_text_source_t {
    void* file = nullptr;
    void* mapping = nullptr;
    const char* view = nullptr;
    std::uint64_t byte_length = 0;
    std::vector<std::uint64_t> line_offsets;

    ~mapped_text_source_t();
    mapped_text_source_t() = default;
    mapped_text_source_t(const mapped_text_source_t&) = delete;
    mapped_text_source_t& operator=(const mapped_text_source_t&) = delete;
};

struct line_cache_t {
    std::vector<std::string> lines;
    std::vector<std::vector<syntax::token_t>> tokens;
    std::vector<std::uint64_t> line_hashes;
    std::size_t content_bytes = 0;
    bool dirty = true;
};

struct pending_edit_t {
    bool active = false;
    int start_line = 0;
    int old_total_lines = 0;
    std::vector<std::string> before_lines;
    int before_caret_line = 0;
    int before_caret_col = 0;
    int coalesce_kind = 0;
    bool merge_previous = false;
};

struct review_hunk_selection_t {
    code_editor_widget::review_hunk_identity_t identity;
    bool focus_requested = false;
};

struct autocomplete_state_t {
    bool popup_visible = false;
    int selected = 0;
    int cursor_byte = 0;
    int cursor_line = 0;
    int cursor_col = 0;
    std::string partial;
    std::vector<std::string> matches;
};

struct ghost_state_t {
    std::string text;
    int trigger_line = -1;
    int trigger_col = -1;
    bool requesting = false;
    int visible_for_line = -1;
    int visible_for_col = -1;
};

class AidaCodeDocument : public QObject {
    Q_OBJECT
public:
    explicit AidaCodeDocument(quint64 document_id, QObject* parent = nullptr);
    ~AidaCodeDocument() override;

    AidaCodeDocument(const AidaCodeDocument&) = delete;
    AidaCodeDocument& operator=(const AidaCodeDocument&) = delete;

    quint64 documentId() const noexcept { return document_id_; }

    bool load(quint64 revision, std::string_view content, std::string_view filename,
              std::string_view filepath, bool dirty, int caret_line, int caret_column,
              float scroll_x, float scroll_y, bool replace_existing,
              int selection_anchor_line, int selection_anchor_column,
              bool selection_active, const std::vector<int>& folded_lines,
              std::string_view language_override);
    bool requestStreamed(quint64 revision, std::string_view filename,
                         std::string_view filepath, std::uint64_t byte_length);

    void cancelRuntimeJobs();

    const line_cache_t& cache() const noexcept { return cache_; }
    line_cache_t& cache() noexcept { return cache_; }
    bool cacheDirty() const noexcept { return cache_.dirty; }
    void rebuildLines();

    int lineCount();
    int lineCount() const;
    const std::string& lineAt(int idx);
    const std::string& lineAt(int idx) const;
    int lineLength(int idx);
    int clampCol(int line, int col);
    int clampLine(int line);
    int maxLineCells() const noexcept { return max_line_cells_; }
    void recomputeMaxLineCells();

    bool largeFileMode() const;
    bool largeReadOnlyMode() const;
    bool readOnly() const noexcept { return read_only_; }
    const std::string& readOnlyReason() const noexcept { return read_only_reason_; }

    quint64 revision() const noexcept { return revision_; }
    bool dirty() const noexcept { return dirty_; }
    bool active() const noexcept { return active_; }
    const std::string& filename() const noexcept { return filename_; }
    const std::string& filepath() const noexcept { return filepath_; }
    const std::string& languageOverride() const noexcept { return language_override_; }
    const std::string& lastError() const noexcept { return last_error_; }
    void setLastError(const std::string& value) { last_error_ = value; }

    const code_editor_widget::selection_t& selection() const noexcept { return selection_; }
    code_editor_widget::selection_t& selection() noexcept { return selection_; }

    void setCaret(int line, int col);
    void setScroll(float x, float y);
    float scrollX() const noexcept { return scroll_x_; }
    float scrollY() const noexcept { return scroll_y_; }
    float targetScrollY() const noexcept { return target_scroll_y_; }
    void applyScrollTarget(float value) { scroll_y_ = target_scroll_y_ = value; }

    void selectionOrdered(int& l0, int& c0, int& l1, int& c1) const;
    std::string selectedText() const;
    std::string selectedTextCapped(std::size_t maximum_bytes) const;

    void insertTextAtCaret(const std::string& text, int coalesce_kind = 0);
    void deleteSelection();
    void deleteForward();
    void pushUndo(int coalesce_kind = 0);
    void pushUndoRange(int first_line, int last_line, int coalesce_kind = 0);
    void breakUndoCoalescing();
    void undo();
    void redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    void rebuildBufferFromLines(bool content_bytes_are_current = false);
    void rebuildBufferFromExternal(const std::string& text);
    std::string serializedContent();
    quint64 contentFingerprint();

    bool performDocumentAction(code_editor_widget::document_action_t action);

    code_editor_widget::find_state_t& find() noexcept { return find_; }
    const code_editor_widget::find_state_t& find() const noexcept { return find_; }
    code_editor_widget::goto_state_t& goTo() noexcept { return go_to_; }

    void findAllMatches();
    void findNext();
    void findPrev();
    void replaceCurrent();
    void replaceAll();

    bool isMapped() const noexcept { return mapped_source_ != nullptr; }
    bool streamLoading() const noexcept { return stream_loading_; }
    const std::string& streamError() const noexcept { return stream_error_; }
    bool findLoading() const noexcept { return find_loading_; }
    const std::string& findError() const noexcept { return find_error_; }

    const std::vector<int>& foldedLines() const noexcept { return folded_lines_; }
    const std::vector<int>& visibleLines() const noexcept { return visible_lines_; }
    const std::vector<int>& logicalToVisual() const noexcept { return logical_to_visual_; }
    void rebuildFoldProjection();
    void revealLogicalLine(int line);
    bool toggleFold(int line);
    int cachedFoldEnd(int start_line);
    bool foldHasEnd(int line) const;
    bool findMatchingBracket(int line, int col, int& out_line, int& out_col, char& out_ch);
    int visualRowFor(int logical_line) const;
    int logicalLineFor(int visual_row) const;
    void ensureCaretVisiblePixels(qreal viewport_h, qreal line_h, qreal view_text_w,
                                  qreal char_w, qreal max_scroll_x);

    std::string caretIdentifier();

    void markSaved(quint64 revision, std::string_view filename, std::string_view filepath);

    code_editor_widget::document_metadata_snapshot_t metadataSnapshot();
    code_editor_widget::document_payload_snapshot_t payloadSnapshot(quint64 expected_revision = 0);
    code_editor_widget::document_state_t stateSnapshot(quint64 focused_document_id) const;
    code_editor_widget::document_capabilities_t capabilities();

    bool setLanguageOverride(std::string_view language);

    bool beginAgentEdit(std::string_view origin);
    bool proposeFullContent(std::string_view new_content);
    bool proposeContent(quint64 base_revision, quint64 base_content_hash,
                        std::string_view current_content, std::string_view new_content,
                        std::string_view origin);
    bool proposeReplaceRange(int start_line, int end_line, std::string_view replacement);
    bool hasPendingDiff();
    const code_editor_widget::pending_diff_t& pendingDiff() const { return diff_; }
    std::mutex& diffMutex() noexcept { return diff_mutex_; }
    code_editor_widget::review_hunk_identity_t selectedReviewHunkIdentityAssumeLocked();
    int pendingHunkCount();
    bool hasPendingReviewHunks();
    code_editor_widget::review_hunk_identity_t reviewHunkIdentity(int index);
    code_editor_widget::review_hunk_identity_t selectedReviewHunkIdentity();
    int resolveReviewHunk(const code_editor_widget::review_hunk_identity_t& identity,
                          bool require_pending);
    bool selectReviewHunk(int index, bool request_focus);
    bool selectNextPendingHunk();
    bool selectPreviousPendingHunk();
    bool acceptHunk(int index);
    bool rejectHunk(int index);
    void acceptAllHunks();
    void rejectAllHunks();
    bool commitResolvedDiff();
    void cancelAgentEdit();
    int diffHoverHunk() const noexcept { return diff_hover_hunk_; }
    void setDiffHoverHunk(int value) noexcept { diff_hover_hunk_ = value; }
    float diffScrollTarget() const noexcept { return diff_scroll_target_; }
    void setDiffScrollTarget(float value) noexcept { diff_scroll_target_ = value; }

    autocomplete_state_t& autocomplete() noexcept { return autocomplete_; }
    const autocomplete_state_t& autocomplete() const noexcept { return autocomplete_; }
    void rebuildAutocomplete(const std::string& partial, int caret_line);

    ghost_state_t& ghost() noexcept { return ghost_; }
    void ghostResetForCaretMove();
    void ghostStartDebounce();
    void ghostCancelRequest();
    void ghostTabAccept();
    void ghostDismiss();
    bool ghostTabConsumed = false;

    bool& blinkOn() noexcept { return blink_on_; }
    bool& hasFocus() noexcept { return has_focus_; }
    bool& findHasFocus() noexcept { return find_has_focus_; }
    bool& focusFindInput() noexcept { return focus_find_input_; }
    bool& mouseSelecting() noexcept { return mouse_selecting_; }
    void registerClick(qreal now_seconds);
    int clickCount() const noexcept { return click_count_; }

    void tokenizeLine(std::size_t index);
    void tokenizeRange(int first, int last);
    const std::vector<syntax::token_t>& tokensForLine(int line);
    const syntax::language_def_t& language() const noexcept { return language_; }
    void resolveLanguageIfNeeded();

    std::uint64_t nextFindGeneration() { return ++find_generation_; }
    std::uint64_t findGeneration() const { return find_generation_; }
    void observeDispatchFailures();

Q_SIGNALS:
    void contentChanged(quint64 revision);
    void metadataChanged();
    void streamStateChanged();
    void findStateChanged();
    void diffChanged();
    void foldsChanged();

private:
    friend class AidaCodeDocumentRegistry;

    struct find_result_delivery_t {
        quint64 generation = 0;
        std::string task_key;
        std::vector<code_editor_widget::find_match_t> matches;
        std::string error;
    };

    void publishStreamResult(std::shared_ptr<mapped_text_source_t> source,
                             std::string error, quint64 generation, std::string task_key);
    void publishFindResult(find_result_delivery_t delivery);
    void publishGhostResult(std::string result);

    int discoverFoldEnd(int start_line);
    void finalizeDiffIfResolved();
    bool applyResolvedDiffToBuffer();
    std::string composeResolvedText() const;
    void rebuildPendingFromProposal(const std::string& origin,
                                    const std::vector<std::string>& old_lines,
                                    const std::vector<std::string>& new_lines,
                                    quint64 document_id, quint64 base_revision,
                                    quint64 base_content_hash);
    code_editor_widget::review_hunk_identity_t reviewHunkIdentityLocked(int index) const;
    int resolveReviewHunkLocked(const code_editor_widget::review_hunk_identity_t& identity,
                                bool require_pending) const;
    bool selectReviewHunkLocked(int index, bool request_focus);
    bool selectPendingReviewHunkLocked(bool forward);

    quint64 document_id_ = 0;
    quint64 revision_ = 1;
    int max_line_cells_ = 0;
    int max_line_cells_at_max_ = 0;
    std::string serialized_content_;
    bool serialized_dirty_ = false;
    quint64 content_fingerprint_ = 0;
    quint64 fingerprint_revision_ = 0;
    std::string filename_;
    std::string filepath_;
    bool active_ = false;
    bool dirty_ = false;
    bool read_only_ = false;
    std::string read_only_reason_;
    line_cache_t cache_;
    code_editor_widget::selection_t selection_;
    code_editor_widget::find_state_t find_;
    code_editor_widget::goto_state_t go_to_;
    std::vector<code_editor_widget::undo_entry_t> undo_;
    std::vector<code_editor_widget::undo_entry_t> redo_;
    float scroll_y_ = 0.f;
    float scroll_x_ = 0.f;
    float target_scroll_y_ = 0.f;
    syntax::language_def_t language_{};
    bool language_set_ = false;
    std::string language_override_;
    std::vector<int> folded_lines_;
    std::vector<int> visible_lines_;
    std::vector<int> logical_to_visual_;
    std::unordered_map<int, int> fold_ends_;
    std::unordered_map<int, int> fold_candidates_;
    quint64 fold_candidate_revision_ = 0;
    quint64 fold_projection_revision_ = 0;
    code_editor_widget::pending_diff_t diff_;
    review_hunk_selection_t review_hunk_selection_;
    int diff_hover_hunk_ = -1;
    float diff_scroll_target_ = -1.f;
    std::string last_error_;
    QElapsedTimer edit_clock_;
    double last_edit_time_ = 0.0;
    int last_edit_line_ = -1;
    int last_edit_col_ = -1;
    int undo_kind_ = 0;
    pending_edit_t pending_edit_;
    bool blink_on_ = true;
    bool focus_find_input_ = false;
    bool find_has_focus_ = false;
    char find_last_buf_[256] = {};
    bool mouse_selecting_ = false;
    qreal last_click_time_ = 0.0;
    int click_count_ = 0;
    bool has_focus_ = false;
    autocomplete_state_t autocomplete_;
    ghost_state_t ghost_;
    std::mutex diff_mutex_;
    std::shared_ptr<mapped_text_source_t> mapped_source_;
    std::unordered_map<int, std::string> mapped_lines_;
    std::deque<int> mapped_line_lru_;
    std::size_t mapped_line_cache_bytes_ = 0;
    std::unordered_map<int, std::vector<syntax::token_t>> mapped_tokens_;
    std::unordered_map<int, std::uint64_t> mapped_hashes_;
    bool stream_loading_ = false;
    std::string stream_error_;
    quint64 stream_generation_ = 0;
    std::shared_ptr<std::atomic<bool>> stream_dispatch_failed_;
    std::shared_ptr<std::atomic<bool>> stream_cancel_;
    std::uint64_t stream_task_id_ = 0;
    std::shared_ptr<std::atomic<bool>> find_cancel_;
    quint64 find_generation_ = 0;
    std::uint64_t find_task_id_ = 0;
    bool find_loading_ = false;
    std::string find_error_;
    std::shared_ptr<std::atomic<bool>> find_dispatch_failed_;
};

class AidaCodeDocumentRegistry : public QObject {
    Q_OBJECT
public:
    explicit AidaCodeDocumentRegistry(QObject* parent = nullptr);
    ~AidaCodeDocumentRegistry() override;

    static AidaCodeDocumentRegistry& instance();

    AidaCodeDocument& ensure(quint64 document_id);
    AidaCodeDocument* find(quint64 document_id) const noexcept;
    void discard(quint64 document_id);
    void reset();

    quint64 boundDocumentId() const noexcept { return bound_document_id_; }
    quint64 focusedDocumentId() const noexcept { return focused_document_id_; }
    quint64 activeDocumentId() const noexcept;
    void bindForActions(quint64 document_id);
    bool selectForActions(quint64 document_id);
    void bindFocused();

    bool loadDocument(quint64 document_id, quint64 revision, std::string_view content,
                      std::string_view filename, std::string_view filepath, bool dirty,
                      int caret_line, int caret_column, float scroll_x, float scroll_y,
                      bool replace_existing, int selection_anchor_line,
                      int selection_anchor_column, bool selection_active,
                      const std::vector<int>& folded_lines, std::string_view language_override);
    bool requestStreamedDocument(quint64 document_id, quint64 revision,
                                 std::string_view filename, std::string_view filepath,
                                 std::uint64_t byte_length);

    code_editor_widget::document_metadata_snapshot_t metadata(quint64 document_id);
    code_editor_widget::document_payload_snapshot_t payload(quint64 document_id,
                                                            quint64 expected_revision = 0);
    code_editor_widget::document_state_t state(quint64 document_id);

    void openFind(quint64 document_id);
    void openReplace(quint64 document_id);
    void openGotoLine(quint64 document_id);

Q_SIGNALS:
    void documentAdded(quint64 document_id);
    void documentAboutToBeRemoved(quint64 document_id);
    void documentRemoved(quint64 document_id);
    void contentChanged(quint64 document_id, quint64 revision);
    void metadataChanged(quint64 document_id);
    void foldsChanged(quint64 document_id);
    void findStateChanged(quint64 document_id);
    void diffChanged(quint64 document_id);
    void streamStateChanged(quint64 document_id);
    void findOverlayRequested(quint64 document_id, bool replace_mode);
    void gotoOverlayRequested(quint64 document_id);

private:
    void wire(AidaCodeDocument* document);
    void sweepDispatchFailures();

    std::unordered_map<quint64, AidaCodeDocument*> documents_;
    quint64 bound_document_id_ = 0;
    quint64 focused_document_id_ = 0;
    QTimer* sweep_timer_ = nullptr;
};

}
