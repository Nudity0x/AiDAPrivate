#include "qt/editor/aida_code_document.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QTimer>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <new>
#include <regex>

#include "core/ai/standalone_ai_client.hpp"
#include "core/editor/programming_document_service.hpp"
#include "core/infra/executor.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/task_center.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"

namespace aida::qt::editor {

namespace {

constexpr int UNDO_MAX = 100;
constexpr std::size_t LARGE_FILE_BYTES =
    aida::editor::programming_documents::normal_editable_document_bytes;
constexpr std::size_t LARGE_READ_ONLY_BYTES =
    aida::editor::programming_documents::maximum_editable_document_bytes + 1U;
constexpr std::size_t MAXIMUM_VIEWABLE_BYTES =
    aida::editor::programming_documents::maximum_viewable_document_bytes;
constexpr std::size_t HISTORY_BUDGET_BYTES = 32ULL * 1024ULL * 1024ULL;
constexpr std::size_t LARGE_HISTORY_BUDGET_BYTES = 12ULL * 1024ULL * 1024ULL;
constexpr std::size_t k_mapped_line_cache_budget = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t k_max_rendered_line = 64ULL * 1024ULL;
constexpr std::uint64_t k_max_mapped_lines = 16ULL * 1024ULL * 1024ULL;

std::uint64_t line_hash(std::string_view text)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

int line_cell_width(std::string_view text, int tab_size)
{
    const int tab = (std::max)(1, tab_size);
    int cells = 0;
    for (const char character : text) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if ((byte & 0xC0) == 0x80) continue;
        if (character == '\t') cells += tab - (cells % tab);
        else ++cells;
    }
    return cells;
}

int mapped_max_line_cells(const mapped_text_source_t& source)
{
    constexpr std::uint64_t truncation_suffix_allowance = 64;
    std::uint64_t widest = 0;
    bool truncated = false;
    const std::size_t count = source.line_offsets.size();
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t start = source.line_offsets[index];
        const std::uint64_t end = index + 1 < count
            ? source.line_offsets[index + 1] : source.byte_length;
        if (end <= start + 1) continue;
        std::uint64_t length = end - start - 1;
        if (length > k_max_rendered_line) {
            length = k_max_rendered_line;
            truncated = true;
        }
        widest = (std::max)(widest, length);
    }
    if (truncated) widest += truncation_suffix_allowance;
    return static_cast<int>((std::min)(widest,
        static_cast<std::uint64_t>((std::numeric_limits<int>::max)())));
}

std::vector<std::string> split_to_lines(std::string_view text)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            std::size_t end = i;
            if (end > start && text[end - 1] == '\r') end--;
            out.emplace_back(text.substr(start, end - start));
            start = i + 1;
        }
    }
    std::size_t end = text.size();
    if (end > start && text[end - 1] == '\r') end--;
    out.emplace_back(text.substr(start, end - start));
    return out;
}

std::string serialize_lines(const line_cache_t& cache)
{
    std::string result;
    result.reserve(cache.content_bytes);
    for (std::size_t index = 0; index < cache.lines.size(); ++index) {
        if (index != 0) result.push_back('\n');
        result.append(cache.lines[index]);
    }
    return result;
}

bool is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

const syntax::language_def_t& resolved_language(const std::string& filename,
                                                std::string_view override_name)
{
    if (override_name == "C/C++") return syntax::detect_language("override.cpp");
    if (override_name == "x86 Assembly") return syntax::detect_language("override.asm");
    if (override_name == "Python") return syntax::detect_language("override.py");
    if (override_name == "JSON") return syntax::detect_language("override.json");
    if (override_name == "Lua") return syntax::detect_language("override.lua");
    return syntax::detect_language(filename);
}

const std::vector<std::string>& autocomplete_keywords()
{
    static const std::vector<std::string> kw = {
        "alignas","alignof","auto","bool","break","case","catch","char",
        "char16_t","char32_t","class","const","constexpr","continue",
        "decltype","default","delete","do","double","dynamic_cast","else",
        "enum","explicit","extern","false","float","for","friend","goto",
        "if","inline","int","long","mutable","namespace","new","noexcept",
        "nullptr","operator","override","private","protected","public",
        "register","reinterpret_cast","requires","return","short","signed",
        "sizeof","static","static_assert","static_cast","struct","switch",
        "template","this","thread_local","throw","true","try","typedef",
        "typeid","typename","union","unsigned","using","virtual","void",
        "volatile","wchar_t","while",
        "int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t",
        "uint32_t","uint64_t","size_t","uintptr_t","intptr_t","ptrdiff_t",
        "string","vector","map","unordered_map","set","unordered_set",
        "array","deque","list","pair","tuple","shared_ptr","unique_ptr",
        "optional","variant","any","function","thread","mutex","atomic",
        "printf","sprintf","snprintf","fprintf","memcpy","memset","memmove",
        "strlen","strcmp","strncmp","strcpy","strncpy","malloc","calloc",
        "realloc","free",
        "DWORD","HANDLE","HMODULE","LPVOID","LPCSTR","LPCWSTR","BOOL",
        "INVALID_HANDLE_VALUE","CreateFile","ReadFile","WriteFile",
        "CloseHandle","GetLastError","VirtualAlloc","VirtualFree",
        "VirtualProtect","LoadLibrary","GetProcAddress","FreeLibrary",
        "CreateThread","WaitForSingleObject","TerminateProcess",
        "CreateProcess","OpenProcess","ReadProcessMemory","WriteProcessMemory",
        "IMAGE_DOS_HEADER","IMAGE_NT_HEADERS","IMAGE_SECTION_HEADER",
        "IMAGE_IMPORT_DESCRIPTOR","IMAGE_EXPORT_DIRECTORY",
        "PIMAGE_DOS_HEADER","PIMAGE_NT_HEADERS",
        "RtlInitUnicodeString","ZwQuerySystemInformation",
        "NtQueryInformationProcess","PsLookupProcessByProcessId",
    };
    return kw;
}

bool fuzzy_subsequence_score(const std::string& lower_pat, const std::string& lower_cand,
                             int& score)
{
    if (lower_pat.empty()) { score = 0; return true; }
    std::size_t pi = 0;
    int s = 0;
    int prev_match = -2;
    int consecutive = 0;
    for (std::size_t ci = 0; ci < lower_cand.size() && pi < lower_pat.size(); ++ci) {
        if (lower_cand[ci] == lower_pat[pi]) {
            if (static_cast<int>(ci) == prev_match + 1) {
                consecutive++;
                s += 6 + consecutive * 2;
            } else {
                consecutive = 0;
                s += 2;
            }
            if (ci == 0) s += 12;
            else {
                char p = lower_cand[ci - 1];
                if (p == '_' || p == ':' || p == '.') s += 8;
            }
            prev_match = static_cast<int>(ci);
            pi++;
        }
    }
    if (pi != lower_pat.size()) return false;
    if (lower_cand.size() == lower_pat.size()) s += 4;
    s -= static_cast<int>(lower_cand.size()) / 4;
    score = s;
    return true;
}

char matching_close_bracket(char open)
{
    switch (open) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '"': return '"';
    case '\'': return '\'';
    default:  return 0;
    }
}

bool is_open_bracket(char c) { return c == '(' || c == '[' || c == '{'; }
bool is_close_bracket(char c) { return c == ')' || c == ']' || c == '}'; }

void identity_hash_value(std::uint64_t& hash, std::uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<unsigned char>((value >> shift) & 0xFFU);
        hash *= 1099511628211ULL;
    }
}

void identity_hash_text(std::uint64_t& hash, std::string_view value)
{
    identity_hash_value(hash, static_cast<std::uint64_t>(value.size()));
    for (const char character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
}

void assign_diff_identities(code_editor_widget::pending_diff_t& diff)
{
    std::uint64_t proposal = 14695981039346656037ULL;
    identity_hash_value(proposal, diff.document_id);
    identity_hash_value(proposal, diff.base_revision);
    identity_hash_value(proposal, diff.base_content_hash);
    identity_hash_text(proposal, diff.origin);
    identity_hash_value(proposal, static_cast<std::uint64_t>(diff.new_lines.size()));
    for (const auto& line : diff.new_lines)
        identity_hash_text(proposal, line);
    diff.proposal_id = proposal == 0 ? 1 : proposal;

    for (auto& hunk : diff.hunks) {
        std::uint64_t identity = 14695981039346656037ULL;
        identity_hash_value(identity, static_cast<std::uint64_t>(hunk.old_start));
        identity_hash_value(identity, static_cast<std::uint64_t>(hunk.old_count));
        identity_hash_value(identity, static_cast<std::uint64_t>(hunk.new_start));
        identity_hash_value(identity, static_cast<std::uint64_t>(hunk.new_count));
        identity_hash_value(identity, static_cast<std::uint64_t>(hunk.lines.size()));
        for (const auto& line : hunk.lines) {
            identity_hash_value(identity, static_cast<std::uint64_t>(line.kind));
            identity_hash_value(identity, static_cast<std::uint64_t>(line.old_line));
            identity_hash_value(identity, static_cast<std::uint64_t>(line.new_line));
            identity_hash_text(identity, line.text);
        }
        hunk.stable_id = identity == 0 ? 1 : identity;
    }
}

void compute_lcs_diff(const std::vector<std::string>& a, const std::vector<std::string>& b,
                      code_editor_widget::pending_diff_t& diff)
{
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    const auto& a_at = [&a](int index) -> const std::string& {
        return a[static_cast<std::size_t>(index)];
    };
    const auto& b_at = [&b](int index) -> const std::string& {
        return b[static_cast<std::size_t>(index)];
    };

    int pre = 0;
    while (pre < n && pre < m && a_at(pre) == b_at(pre)) pre++;
    int suf = 0;
    while (suf < (n - pre) && suf < (m - pre) &&
           a_at(n - 1 - suf) == b_at(m - 1 - suf)) suf++;

    const int an = n - pre - suf;
    const int bm = m - pre - suf;

    struct op_t { int kind; std::string text; int old_line; int new_line; };
    std::vector<op_t> ops;

    for (int i = 0; i < pre; ++i)
        ops.push_back({ 0, a_at(i), i, i });

    const long long dp_cells =
        static_cast<long long>(an + 1) * static_cast<long long>(bm + 1);
    const long long dp_cap = 6000000;

    if (dp_cells > dp_cap) {
        for (int i = 0; i < an; ++i)
            ops.push_back({ 2, a_at(pre + i), pre + i, -1 });
        for (int j = 0; j < bm; ++j)
            ops.push_back({ 1, b_at(pre + j), -1, pre + j });
    } else {
        std::vector<std::vector<int>> dp(static_cast<std::size_t>(an + 1),
            std::vector<int>(static_cast<std::size_t>(bm + 1), 0));
        for (int i = an - 1; i >= 0; --i) {
            for (int j = bm - 1; j >= 0; --j) {
                const std::size_t i_idx = static_cast<std::size_t>(i);
                const std::size_t j_idx = static_cast<std::size_t>(j);
                if (a_at(pre + i) == b_at(pre + j))
                    dp[i_idx][j_idx] = dp[static_cast<std::size_t>(i + 1)][static_cast<std::size_t>(j + 1)] + 1;
                else
                    dp[i_idx][j_idx] = (std::max)(dp[static_cast<std::size_t>(i + 1)][j_idx],
                        dp[i_idx][static_cast<std::size_t>(j + 1)]);
            }
        }

        int i = 0, j = 0;
        while (i < an && j < bm) {
            if (a_at(pre + i) == b_at(pre + j)) {
                ops.push_back({ 0, a_at(pre + i), pre + i, pre + j });
                i++; j++;
            } else if (dp[static_cast<std::size_t>(i + 1)][static_cast<std::size_t>(j)] >=
                       dp[static_cast<std::size_t>(i)][static_cast<std::size_t>(j + 1)]) {
                ops.push_back({ 2, a_at(pre + i), pre + i, -1 });
                i++;
            } else {
                ops.push_back({ 1, b_at(pre + j), -1, pre + j });
                j++;
            }
        }
        while (i < an) { ops.push_back({ 2, a_at(pre + i), pre + i, -1 }); i++; }
        while (j < bm) { ops.push_back({ 1, b_at(pre + j), -1, pre + j }); j++; }
    }

    for (int i = 0; i < suf; ++i)
        ops.push_back({ 0, a_at(n - suf + i), n - suf + i, m - suf + i });

    diff.hunks.clear();
    diff.total_added   = 0;
    diff.total_removed = 0;

    std::size_t idx = 0;
    while (idx < ops.size()) {
        if (ops[idx].kind == 0) { idx++; continue; }

        code_editor_widget::diff_hunk_t hunk;
        hunk.state = code_editor_widget::diff_hunk_state_t::pending;
        std::size_t hs = idx;
        int ctx_run = 0;
        std::size_t he = idx;
        while (he < ops.size()) {
            if (ops[he].kind == 0) {
                ctx_run++;
                if (ctx_run > 2) break;
            } else {
                ctx_run = 0;
            }
            he++;
        }
        while (he > hs && ops[he - 1].kind == 0) he--;

        int first_old = -1, first_new = -1, last_old = -1, last_new = -1;
        for (std::size_t k = hs; k < he; ++k) {
            const op_t& o = ops[k];
            code_editor_widget::diff_line_t dl;
            dl.text     = o.text;
            dl.old_line = o.old_line;
            dl.new_line = o.new_line;
            if (o.kind == 0)      dl.kind = code_editor_widget::diff_line_kind_t::context;
            else if (o.kind == 1) { dl.kind = code_editor_widget::diff_line_kind_t::added;   hunk.added++; }
            else                  { dl.kind = code_editor_widget::diff_line_kind_t::removed; hunk.removed++; }

            if (o.old_line >= 0) {
                if (first_old < 0) first_old = o.old_line;
                last_old = o.old_line;
            }
            if (o.new_line >= 0) {
                if (first_new < 0) first_new = o.new_line;
                last_new = o.new_line;
            }
            hunk.lines.push_back(std::move(dl));
        }

        hunk.old_start = first_old < 0 ? 0 : first_old;
        hunk.new_start = first_new < 0 ? 0 : first_new;
        hunk.old_count = (first_old < 0) ? 0 : (last_old - first_old + 1);
        hunk.new_count = (first_new < 0) ? 0 : (last_new - first_new + 1);

        diff.total_added   += hunk.added;
        diff.total_removed += hunk.removed;
        diff.hunks.push_back(std::move(hunk));
        idx = he;
    }
    assign_diff_identities(diff);
}

}

mapped_text_source_t::~mapped_text_source_t()
{
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(static_cast<HANDLE>(mapping));
    if (file && file != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(file));
}

AidaCodeDocument::AidaCodeDocument(quint64 document_id, QObject* parent)
    : QObject(parent), document_id_(document_id)
{
    edit_clock_.start();
}

AidaCodeDocument::~AidaCodeDocument()
{
    cancelRuntimeJobs();
}

void AidaCodeDocument::cancelRuntimeJobs()
{
    if (stream_cancel_)
        stream_cancel_->store(true, std::memory_order_release);
    if (find_cancel_)
        find_cancel_->store(true, std::memory_order_release);
    if (stream_task_id_ != 0)
        aida::infra::executor::cancel(stream_task_id_);
    if (find_task_id_ != 0)
        aida::infra::executor::cancel(find_task_id_);
    stream_task_id_ = 0;
    find_task_id_ = 0;
    stream_cancel_.reset();
    find_cancel_.reset();
}

bool AidaCodeDocument::load(quint64 revision, std::string_view content,
    std::string_view filename, std::string_view filepath, bool dirty, int caret_line,
    int caret_column, float scroll_x, float scroll_y, bool replace_existing,
    int selection_anchor_line, int selection_anchor_column, bool selection_active,
    const std::vector<int>& folded_lines, std::string_view language_override)
{
    if (document_id_ == 0 || revision == 0 ||
        content.size() > aida::editor::programming_documents::maximum_editable_document_bytes)
        return false;
    if (replace_existing) cancelRuntimeJobs();
    revision_ = revision;
    serialized_content_.assign(content);
    serialized_dirty_ = false;
    content_fingerprint_ = 0;
    fingerprint_revision_ = 0;
    filename_.assign(filename);
    filepath_.assign(filepath);
    active_ = true;
    dirty_ = dirty;
    read_only_ = false;
    read_only_reason_.clear();
    cache_ = {};
    cache_.dirty = true;
    selection_ = {};
    selection_.caret_line = (std::max)(0, caret_line);
    selection_.caret_col = (std::max)(0, caret_column);
    selection_.anchor_line = (std::max)(0, selection_anchor_line);
    selection_.anchor_col = (std::max)(0, selection_anchor_column);
    selection_.active = selection_active &&
        (selection_.anchor_line != selection_.caret_line ||
         selection_.anchor_col != selection_.caret_col);
    scroll_x_ = (std::max)(0.f, scroll_x);
    scroll_y_ = target_scroll_y_ = (std::max)(0.f, scroll_y);
    find_ = {};
    go_to_ = {};
    undo_.clear();
    redo_.clear();
    diff_ = {};
    review_hunk_selection_ = {};
    mapped_source_.reset();
    mapped_lines_.clear();
    mapped_line_lru_.clear();
    mapped_tokens_.clear();
    mapped_hashes_.clear();
    mapped_line_cache_bytes_ = 0;
    stream_loading_ = false;
    stream_error_.clear();
    language_override_.assign(language_override.substr(0, 64));
    language_ = resolved_language(filename_, language_override_);
    language_set_ = !filename_.empty() || !language_override_.empty();
    folded_lines_.clear();
    folded_lines_.reserve((std::min)(folded_lines.size(), std::size_t{4096}));
    for (const int line : folded_lines)
        if (line >= 0)
            folded_lines_.push_back(line);
    std::sort(folded_lines_.begin(), folded_lines_.end());
    folded_lines_.erase(std::unique(folded_lines_.begin(), folded_lines_.end()),
        folded_lines_.end());
    fold_projection_revision_ = 0;
    Q_EMIT metadataChanged();
    return true;
}

bool AidaCodeDocument::requestStreamed(quint64 revision, std::string_view filename,
    std::string_view filepath, std::uint64_t byte_length)
{
    if (document_id_ == 0 || filepath.empty() || byte_length < LARGE_READ_ONLY_BYTES ||
        byte_length > MAXIMUM_VIEWABLE_BYTES)
        return false;
    cancelRuntimeJobs();
    revision_ = (std::max)(revision, std::uint64_t{1});
    filename_.assign(filename);
    filepath_.assign(filepath);
    active_ = true;
    dirty_ = false;
    read_only_ = true;
    diff_ = {};
    review_hunk_selection_ = {};
    const bool very_large_file = byte_length >=
        aida::editor::programming_documents::large_document_milestone_bytes;
    read_only_reason_ = very_large_file
        ? "Files from 50 MiB through 500 MiB use cancellable, Task-Center-owned memory-mapped indexing with bounded line/token caches; editing and full-document copies are disabled."
        : "Files above 1 MiB through 50 MiB use a memory-mapped, searchable read-only view to preserve frame pacing and exact crash recovery.";
    stream_error_.clear();
    stream_loading_ = true;
    const std::uint64_t generation = ++stream_generation_;
    const std::string path(filepath);
    const std::string task_key = "editor.stream." + std::to_string(document_id_) + "." +
        std::to_string(generation);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto dispatch_failed = std::make_shared<std::atomic<bool>>(false);
    stream_cancel_ = cancelled;
    stream_dispatch_failed_ = dispatch_failed;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "code_editor";
    submission.label = "code_editor.streamed_document";
    submission.thread_class = "memory_mapped_file_index";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 2;
    submission.generation = generation;
    submission.ui_access_policy = "immutable_snapshots_only";
    submission.failure_policy = "typed_diagnostic";
    submission.shutdown_policy = "cancel";
    submission.cancel_hook = [cancelled] { cancelled->store(true, std::memory_order_release); };
    submission.body = [cancelled, generation, byte_length, path, task_key, dispatch_failed,
                       guard = QPointer<AidaCodeDocument>(this)]() {
        auto source = std::make_shared<mapped_text_source_t>();
        std::string error;
        const std::wstring wide_path = std::filesystem::path(path).wstring();
        source->file = CreateFileW(wide_path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (source->file == INVALID_HANDLE_VALUE)
            error = "The large file could not be opened (Win32 " + std::to_string(GetLastError()) + ").";
        LARGE_INTEGER exact_size{};
        if (error.empty() && (!GetFileSizeEx(static_cast<HANDLE>(source->file), &exact_size) ||
                exact_size.QuadPart < 0 ||
                static_cast<std::uint64_t>(exact_size.QuadPart) != byte_length))
            error = "The large file changed size before its mapped view could be created.";
        if (error.empty()) {
            source->mapping = CreateFileMappingW(static_cast<HANDLE>(source->file), nullptr,
                PAGE_READONLY, 0, 0, nullptr);
            if (!source->mapping)
                error = "The read-only file mapping could not be created (Win32 " +
                    std::to_string(GetLastError()) + ").";
        }
        if (error.empty()) {
            source->view = static_cast<const char*>(MapViewOfFile(
                static_cast<HANDLE>(source->mapping), FILE_MAP_READ, 0, 0, 0));
            if (!source->view)
                error = "The read-only file mapping could not be viewed (Win32 " +
                    std::to_string(GetLastError()) + ").";
        }
        if (error.empty()) {
            try {
                source->byte_length = byte_length;
                source->line_offsets.reserve(static_cast<std::size_t>((std::min)(
                    byte_length / 48U + 1U, k_max_mapped_lines)));
                source->line_offsets.push_back(0);
                for (std::uint64_t offset = 0; offset < byte_length; ++offset) {
                    if ((offset & 0x3FFFFFU) == 0U && cancelled->load(std::memory_order_acquire))
                        break;
                    if (source->view[offset] == '\0') {
                        error = "The artifact contains binary NUL bytes; open it in Hex View or Binary Map.";
                        break;
                    }
                    if (source->view[offset] == '\n') {
                        if (source->line_offsets.size() >= k_max_mapped_lines) {
                            error = "The mapped text exceeds the 16,777,216-line index budget; open it in Hex View or Binary Map.";
                            break;
                        }
                        source->line_offsets.push_back(offset + 1U);
                    }
                }
            } catch (const std::bad_alloc&) {
                error = "The bounded memory-mapped line index could not be allocated; close other large views or use Hex View.";
                source->line_offsets.clear();
            }
        }
        if (cancelled->load(std::memory_order_acquire) && error.empty())
            error = "Large-file indexing was cancelled.";
        const bool posted = QMetaObject::invokeMethod(guard,
            [guard, generation, task_key, source = std::move(source),
             error = std::move(error)]() mutable {
                if (AidaCodeDocument* document = guard.data())
                    document->publishStreamResult(std::move(source), std::move(error), generation,
                                    std::move(task_key));
            }, Qt::QueuedConnection);
        if (!posted)
            dispatch_failed->store(true, std::memory_order_release);
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        stream_loading_ = false;
        stream_dispatch_failed_.reset();
        stream_cancel_.reset();
        stream_error_ = "The large-file worker could not be scheduled: " + submitted.reject_reason;
        Q_EMIT streamStateChanged();
        return false;
    }
    stream_task_id_ = submitted.task_id;
    aida::ui::task_center::task_registration_t registration;
    registration.id = task_key;
    registration.source = "code_editor";
    registration.owner = "Code Editor";
    registration.owner_view = "document.code";
    registration.owner_action = "file.open";
    registration.target = path;
    registration.label = "Index large text file";
    registration.stage = very_large_file
        ? "Building bounded index for 50-500 MiB text"
        : "Building memory-mapped line index";
    registration.affected_entity = std::to_string(document_id_);
    registration.cancellation_is_safe = true;
    if (!aida::ui::task_center::register_executor_job(submitted.task_id, std::move(registration))) {
        aida::infra::executor::cancel(submitted.task_id);
        cancelled->store(true, std::memory_order_release);
        ++stream_generation_;
        stream_loading_ = false;
        stream_dispatch_failed_.reset();
        stream_task_id_ = 0;
        stream_cancel_.reset();
        stream_error_ = "Task Center could not own large-file indexing; the operation was cancelled.";
        Q_EMIT streamStateChanged();
        return false;
    }
    Q_EMIT streamStateChanged();
    return true;
}

void AidaCodeDocument::publishStreamResult(std::shared_ptr<mapped_text_source_t> source,
    std::string error, quint64 generation, std::string task_key)
{
    if (stream_generation_ != generation) return;
    stream_loading_ = false;
    stream_dispatch_failed_.reset();
    stream_task_id_ = 0;
    stream_cancel_.reset();
    stream_error_ = std::move(error);
    const auto task_state = stream_error_.empty()
        ? aida::ui::task_center::task_state_t::completed
        : stream_error_.find("cancelled") != std::string::npos
            ? aida::ui::task_center::task_state_t::cancelled
            : aida::ui::task_center::task_state_t::failed;
    static_cast<void>(aida::ui::task_center::update_task(task_key, task_state, 1.f,
        stream_error_.empty() ? "Index complete" : "Index failed",
        stream_error_.empty() ? "Memory-mapped line index is ready." : stream_error_));
    if (!stream_error_.empty()) {
        Q_EMIT streamStateChanged();
        return;
    }
    mapped_source_ = std::move(source);
    mapped_lines_.clear();
    mapped_line_lru_.clear();
    mapped_line_cache_bytes_ = 0;
    mapped_tokens_.clear();
    mapped_hashes_.clear();
    cache_.content_bytes = static_cast<std::size_t>(mapped_source_->byte_length);
    cache_.dirty = false;
    recomputeMaxLineCells();
    Q_EMIT streamStateChanged();
    Q_EMIT contentChanged(revision_);
    Q_EMIT metadataChanged();
}

void AidaCodeDocument::observeDispatchFailures()
{
    if (stream_dispatch_failed_ &&
        stream_dispatch_failed_->exchange(false, std::memory_order_acq_rel)) {
        const std::string task_id = "editor.stream." + std::to_string(document_id_) + "." +
            std::to_string(stream_generation_);
        ++stream_generation_;
        stream_loading_ = false;
        stream_error_ = "Large-file indexing completed, but its result could not return to the UI owner. Retry opening the document.";
        stream_dispatch_failed_.reset();
        stream_task_id_ = 0;
        stream_cancel_.reset();
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::failed, 1.f,
            "Index result dispatch failed", stream_error_));
        Q_EMIT streamStateChanged();
    }
    if (find_dispatch_failed_ &&
        find_dispatch_failed_->exchange(false, std::memory_order_acq_rel)) {
        const std::string task_id = "editor.search." + std::to_string(document_id_) + "." +
            std::to_string(find_generation_);
        ++find_generation_;
        find_loading_ = false;
        find_error_ = "Mapped search completed, but its result could not return to the UI owner. Retry the search.";
        find_dispatch_failed_.reset();
        find_task_id_ = 0;
        find_cancel_.reset();
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::failed, 1.f,
            "Search result dispatch failed", find_error_));
        Q_EMIT findStateChanged();
    }
}

bool AidaCodeDocument::largeFileMode() const
{
    return cache_.content_bytes > LARGE_FILE_BYTES;
}

bool AidaCodeDocument::largeReadOnlyMode() const
{
    return read_only_ || cache_.content_bytes >= LARGE_READ_ONLY_BYTES;
}

void AidaCodeDocument::tokenizeLine(std::size_t index)
{
    if (mapped_source_) {
        const int line_index = static_cast<int>(index);
        const std::string& text = lineAt(line_index);
        const std::uint64_t hash = line_hash(text);
        if (mapped_hashes_[line_index] == hash) return;
        syntax::tokenize(text, language_, mapped_tokens_[line_index]);
        mapped_hashes_[line_index] = hash;
        return;
    }
    if (index >= cache_.lines.size()) return;
    if (cache_.tokens.size() < cache_.lines.size())
        cache_.tokens.resize(cache_.lines.size());
    if (cache_.line_hashes.size() < cache_.lines.size())
        cache_.line_hashes.resize(cache_.lines.size());
    const std::uint64_t hash = line_hash(cache_.lines[index]);
    if (cache_.line_hashes[index] == hash) return;
    syntax::tokenize(cache_.lines[index], language_, cache_.tokens[index]);
    cache_.line_hashes[index] = hash;
}

void AidaCodeDocument::tokenizeRange(int first, int last)
{
    if (cache_.lines.empty()) return;
    first = (std::max)(0, first);
    last = (std::min)(last, static_cast<int>(cache_.lines.size()) - 1);
    for (int line = first; line <= last; ++line)
        tokenizeLine(static_cast<std::size_t>(line));
}

const std::vector<syntax::token_t>& AidaCodeDocument::tokensForLine(int line)
{
    static const std::vector<syntax::token_t> empty;
    if (mapped_source_) {
        tokenizeLine(static_cast<std::size_t>(line));
        const auto found = mapped_tokens_.find(line);
        return found == mapped_tokens_.end() ? empty : found->second;
    }
    if (line < 0) return empty;
    tokenizeLine(static_cast<std::size_t>(line));
    if (static_cast<std::size_t>(line) >= cache_.tokens.size()) return empty;
    return cache_.tokens[static_cast<std::size_t>(line)];
}

void AidaCodeDocument::recomputeMaxLineCells()
{
    const int tab = (std::max)(1, g_sa_settings.editor_tab_size);
    max_line_cells_ = 0;
    max_line_cells_at_max_ = 0;
    if (mapped_source_) {
        max_line_cells_ = mapped_max_line_cells(*mapped_source_);
        max_line_cells_at_max_ = 1;
        return;
    }
    for (const auto& line : cache_.lines) {
        const int cells = line_cell_width(line, tab);
        if (cells > max_line_cells_) {
            max_line_cells_ = cells;
            max_line_cells_at_max_ = 1;
        } else if (cells == max_line_cells_) {
            ++max_line_cells_at_max_;
        }
    }
}

void AidaCodeDocument::rebuildLines()
{
    if (mapped_source_) {
        cache_.lines.clear();
        cache_.tokens.clear();
        cache_.line_hashes.clear();
        cache_.content_bytes = static_cast<std::size_t>(
            (std::min)(mapped_source_->byte_length,
                static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())));
        cache_.dirty = false;
        recomputeMaxLineCells();
        return;
    }
    cache_.lines.clear();
    cache_.content_bytes = 0;
    if (serialized_content_.empty()) {
        cache_.lines.push_back("");
        cache_.tokens.clear();
        cache_.tokens.push_back({});
        cache_.line_hashes.assign(1, line_hash(""));
        cache_.dirty = false;
        return;
    }

    const char* txt = serialized_content_.c_str();
    const char* p = txt;
    const char* line_start = txt;
    while (*p) {
        if (*p == '\n') {
            const char* line_end = (p > line_start && *(p - 1) == '\r') ? p - 1 : p;
            cache_.lines.emplace_back(line_start, line_end);
            line_start = p + 1;
        }
        p++;
    }
    const char* line_end = (p > line_start && *(p - 1) == '\r') ? p - 1 : p;
    cache_.lines.emplace_back(line_start, line_end);
    cache_.content_bytes = static_cast<std::size_t>(p - txt);

    cache_.tokens.assign(cache_.lines.size(), {});
    cache_.line_hashes.assign(cache_.lines.size(), 0);
    if (!largeFileMode())
        tokenizeRange(0, static_cast<int>(cache_.lines.size()) - 1);
    cache_.dirty = false;
    recomputeMaxLineCells();
}

void AidaCodeDocument::rebuildBufferFromLines(bool content_bytes_are_current)
{
    auto& pending = pending_edit_;
    const bool had_pending = pending.active;
    const int new_total_lines = static_cast<int>(cache_.lines.size());
    bool content_bytes_updated = content_bytes_are_current;
    if (pending.active) {
        const int before_count = static_cast<int>(pending.before_lines.size());
        const int after_count = (std::max)(0,
            before_count + new_total_lines - pending.old_total_lines);
        const int after_end = (std::min)(new_total_lines,
            pending.start_line + after_count);
        std::vector<std::string> after_lines;
        if (pending.start_line >= 0 && pending.start_line < after_end)
            after_lines.assign(cache_.lines.begin() + pending.start_line,
                cache_.lines.begin() + after_end);
        std::size_t before_bytes = 0;
        std::size_t after_bytes = 0;
        for (const auto& line : pending.before_lines) before_bytes += line.size();
        for (const auto& line : after_lines) after_bytes += line.size();
        const std::int64_t line_delta = static_cast<std::int64_t>(new_total_lines) -
            static_cast<std::int64_t>(pending.old_total_lines);
        const std::int64_t byte_delta = static_cast<std::int64_t>(after_bytes) -
            static_cast<std::int64_t>(before_bytes) + line_delta;
        const std::int64_t updated_bytes = static_cast<std::int64_t>(cache_.content_bytes) + byte_delta;
        cache_.content_bytes = static_cast<std::size_t>((std::max)(std::int64_t{0}, updated_bytes));
        content_bytes_updated = true;
        if (pending.before_lines != after_lines) {
            const int tab = (std::max)(1, g_sa_settings.editor_tab_size);
            for (const auto& line : pending.before_lines) {
                if (max_line_cells_at_max_ > 0 &&
                    line_cell_width(line, tab) == max_line_cells_)
                    --max_line_cells_at_max_;
            }
            for (const auto& line : after_lines) {
                const int cells = line_cell_width(line, tab);
                if (cells > max_line_cells_) {
                    max_line_cells_ = cells;
                    max_line_cells_at_max_ = 1;
                } else if (cells == max_line_cells_) {
                    ++max_line_cells_at_max_;
                }
            }
            if (max_line_cells_at_max_ <= 0)
                recomputeMaxLineCells();
            code_editor_widget::undo_entry_t entry;
            entry.start_line = pending.start_line;
            entry.before_lines = std::move(pending.before_lines);
            entry.after_lines = std::move(after_lines);
            entry.before_caret_line = pending.before_caret_line;
            entry.before_caret_col = pending.before_caret_col;
            entry.after_caret_line = selection_.caret_line;
            entry.after_caret_col = selection_.caret_col;
            entry.coalesce_kind = pending.coalesce_kind;
            for (const auto& line : entry.before_lines) entry.memory_bytes += line.size();
            for (const auto& line : entry.after_lines) entry.memory_bytes += line.size();
            if (pending.merge_previous && !undo_.empty()) {
                auto& previous = undo_.back();
                previous.after_lines = std::move(entry.after_lines);
                previous.after_caret_line = entry.after_caret_line;
                previous.after_caret_col = entry.after_caret_col;
                previous.memory_bytes = 0;
                for (const auto& line : previous.before_lines) previous.memory_bytes += line.size();
                for (const auto& line : previous.after_lines) previous.memory_bytes += line.size();
            } else {
                undo_.push_back(std::move(entry));
            }
            const std::size_t budget = largeFileMode() ? LARGE_HISTORY_BUDGET_BYTES : HISTORY_BUDGET_BYTES;
            const std::size_t entry_limit = largeFileMode() ? 8U : static_cast<std::size_t>(UNDO_MAX);
            std::size_t bytes = 0;
            for (const auto& item : undo_) bytes += item.memory_bytes;
            while (!undo_.empty() && (undo_.size() > entry_limit || bytes > budget)) {
                bytes -= undo_.front().memory_bytes;
                undo_.erase(undo_.begin());
            }
            redo_.clear();
        }
        pending = {};
    }
    if (!content_bytes_updated) {
        std::size_t bytes = cache_.lines.empty() ? 0U : cache_.lines.size() - 1U;
        for (const auto& line : cache_.lines) bytes += line.size();
        cache_.content_bytes = bytes;
    }
    if (!had_pending)
        recomputeMaxLineCells();
    serialized_dirty_ = true;
    dirty_ = true;
    if (document_id_ != 0) {
        ++revision_;
        review_hunk_selection_ = {};
    }
    cache_.tokens.resize(cache_.lines.size());
    cache_.line_hashes.resize(cache_.lines.size());
    Q_EMIT contentChanged(revision_);
    Q_EMIT metadataChanged();
}

int AidaCodeDocument::lineCount()
{
    if (mapped_source_)
        return static_cast<int>((std::min)(mapped_source_->line_offsets.size(),
            static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    return static_cast<int>(cache_.lines.size());
}

int AidaCodeDocument::lineCount() const
{
    return const_cast<AidaCodeDocument*>(this)->lineCount();
}

const std::string& AidaCodeDocument::lineAt(int idx)
{
    static const std::string empty;
    if (mapped_source_) {
        const auto& source = *mapped_source_;
        if (idx < 0 || static_cast<std::size_t>(idx) >= source.line_offsets.size())
            return empty;
        const auto found = mapped_lines_.find(idx);
        if (found != mapped_lines_.end()) return found->second;
        const std::uint64_t start = source.line_offsets[static_cast<std::size_t>(idx)];
        std::uint64_t end = static_cast<std::size_t>(idx + 1) < source.line_offsets.size()
            ? source.line_offsets[static_cast<std::size_t>(idx + 1)] - 1U
            : source.byte_length;
        if (end > start && source.view[end - 1U] == '\r') --end;
        const std::uint64_t bounded_end = (std::min)(end, start + k_max_rendered_line);
        std::string value(source.view + start, source.view + bounded_end);
        if (bounded_end != end)
            value.append(" [line truncated at 64 KiB; use Hex View for the complete record]");
        auto [inserted, inserted_ok] = mapped_lines_.emplace(idx, std::move(value));
        static_cast<void>(inserted_ok);
        mapped_line_cache_bytes_ += inserted->second.size();
        mapped_line_lru_.push_back(idx);
        while (mapped_line_lru_.size() > 4096U ||
            mapped_line_cache_bytes_ > k_mapped_line_cache_budget) {
            const int expired = mapped_line_lru_.front();
            mapped_line_lru_.pop_front();
            if (expired != idx) {
                const auto expired_line = mapped_lines_.find(expired);
                if (expired_line != mapped_lines_.end())
                    mapped_line_cache_bytes_ -= expired_line->second.size();
                mapped_lines_.erase(expired);
                mapped_tokens_.erase(expired);
                mapped_hashes_.erase(expired);
            }
        }
        return inserted->second;
    }
    if (idx < 0 || idx >= static_cast<int>(cache_.lines.size())) return empty;
    return cache_.lines[static_cast<std::size_t>(idx)];
}

const std::string& AidaCodeDocument::lineAt(int idx) const
{
    return const_cast<AidaCodeDocument*>(this)->lineAt(idx);
}

int AidaCodeDocument::lineLength(int idx) { return static_cast<int>(lineAt(idx).size()); }

int AidaCodeDocument::clampCol(int line, int col)
{
    int clamped = (std::max)(0, (std::min)(col, lineLength(line)));
    const std::string& ln = lineAt(line);
    while (clamped > 0 && clamped < static_cast<int>(ln.size()) &&
           (static_cast<unsigned char>(ln[static_cast<std::size_t>(clamped)]) & 0xC0) == 0x80)
        clamped--;
    return clamped;
}

int AidaCodeDocument::clampLine(int line)
{
    return (std::max)(0, (std::min)(line, lineCount() - 1));
}

void AidaCodeDocument::selectionOrdered(int& l0, int& c0, int& l1, int& c1) const
{
    if (selection_.anchor_line < selection_.caret_line ||
        (selection_.anchor_line == selection_.caret_line && selection_.anchor_col <= selection_.caret_col)) {
        l0 = selection_.anchor_line; c0 = selection_.anchor_col;
        l1 = selection_.caret_line;  c1 = selection_.caret_col;
    } else {
        l0 = selection_.caret_line;  c0 = selection_.caret_col;
        l1 = selection_.anchor_line; c1 = selection_.anchor_col;
    }
}

std::string AidaCodeDocument::selectedText() const
{
    if (!selection_.has_selection()) return {};
    int l0, c0, l1, c1;
    selectionOrdered(l0, c0, l1, c1);
    if (l0 == l1) {
        auto& ln = const_cast<AidaCodeDocument*>(this)->lineAt(l0);
        c0 = std::clamp(c0, 0, static_cast<int>(ln.size()));
        c1 = std::clamp(c1, 0, static_cast<int>(ln.size()));
        return ln.substr(static_cast<std::size_t>(c0), static_cast<std::size_t>(c1 - c0));
    }
    std::string result;
    auto* self = const_cast<AidaCodeDocument*>(this);
    c0 = std::clamp(c0, 0, self->lineLength(l0));
    c1 = std::clamp(c1, 0, self->lineLength(l1));
    result += self->lineAt(l0).substr(static_cast<std::size_t>(c0));
    result += '\n';
    for (int i = l0 + 1; i < l1; i++) {
        result += self->lineAt(i);
        result += '\n';
    }
    result += self->lineAt(l1).substr(0, static_cast<std::size_t>(c1));
    return result;
}

std::string AidaCodeDocument::selectedTextCapped(std::size_t maximum_bytes) const
{
    if (maximum_bytes == 0 || !selection_.has_selection())
        return {};
    std::string result = selectedText();
    if (result.size() > maximum_bytes)
        result.resize(maximum_bytes);
    return result;
}

void AidaCodeDocument::pushUndoRange(int first_line, int last_line, int coalesce_kind)
{
    if (read_only_ || pending_edit_.active) return;
    first_line = clampLine(first_line);
    last_line = clampLine(last_line);
    if (last_line < first_line) std::swap(first_line, last_line);
    bool merge_previous = false;
    if (coalesce_kind != 0 && coalesce_kind == undo_kind_ && !undo_.empty()) {
        const double now = edit_clock_.elapsed() / 1000.0;
        const bool adjacent = (last_edit_line_ == selection_.caret_line) &&
                        (std::abs(selection_.caret_col - last_edit_col_) <= 1);
        const auto& previous = undo_.back();
        merge_previous = adjacent && (now - last_edit_time_) < 1.2 &&
            previous.start_line == first_line && previous.before_lines.size() == 1 &&
            previous.after_lines.size() == 1;
    }
    auto& pending = pending_edit_;
    pending.active = true;
    pending.start_line = first_line;
    pending.old_total_lines = lineCount();
    pending.before_lines.assign(cache_.lines.begin() + first_line,
        cache_.lines.begin() + last_line + 1);
    pending.before_caret_line = selection_.caret_line;
    pending.before_caret_col = selection_.caret_col;
    pending.coalesce_kind = coalesce_kind;
    pending.merge_previous = merge_previous;
    undo_kind_ = coalesce_kind;
    last_edit_time_ = edit_clock_.elapsed() / 1000.0;
    last_edit_line_ = selection_.caret_line;
    last_edit_col_  = selection_.caret_col;
}

void AidaCodeDocument::breakUndoCoalescing()
{
    undo_kind_ = 0;
    last_edit_line_ = -1;
    last_edit_col_  = -1;
}

void AidaCodeDocument::pushUndo(int coalesce_kind)
{
    int first = selection_.caret_line;
    int last = selection_.caret_line;
    if (selection_.has_selection()) {
        int c0 = 0;
        int c1 = 0;
        selectionOrdered(first, c0, last, c1);
    }
    pushUndoRange(first, last, coalesce_kind);
}

void AidaCodeDocument::deleteSelection()
{
    if (!selection_.has_selection()) return;
    int l0, c0, l1, c1;
    selectionOrdered(l0, c0, l1, c1);
    l0 = clampLine(l0);
    l1 = clampLine(l1);
    c0 = clampCol(l0, c0);
    c1 = clampCol(l1, c1);
    pushUndoRange(l0, l1);
    const std::size_t l0_idx = static_cast<std::size_t>(l0);
    const std::size_t l1_idx = static_cast<std::size_t>(l1);
    const std::size_t c0_idx = static_cast<std::size_t>(c0);
    const std::size_t c1_idx = static_cast<std::size_t>(c1);

    if (l0 == l1) {
        cache_.lines[l0_idx].erase(c0_idx, static_cast<std::size_t>(c1 - c0));
    } else {
        std::string merged = cache_.lines[l0_idx].substr(0, c0_idx) +
                             cache_.lines[l1_idx].substr(c1_idx);
        cache_.lines[l0_idx] = merged;
        cache_.lines.erase(cache_.lines.begin() + l0 + 1,
                            cache_.lines.begin() + l1 + 1);
        cache_.tokens.erase(cache_.tokens.begin() + l0 + 1,
                             cache_.tokens.begin() + l1 + 1);
        cache_.line_hashes.erase(cache_.line_hashes.begin() + l0 + 1,
            cache_.line_hashes.begin() + l1 + 1);
    }
    selection_.caret_line = selection_.anchor_line = l0;
    selection_.caret_col  = selection_.anchor_col  = c0;
    selection_.active = false;
    rebuildBufferFromLines();
}

void AidaCodeDocument::deleteForward()
{
    if (selection_.has_selection()) {
        deleteSelection();
        return;
    }
    if (selection_.caret_col < lineLength(selection_.caret_line)) {
        pushUndo();
        const int caret_line = clampLine(selection_.caret_line);
        auto& line = cache_.lines[static_cast<std::size_t>(caret_line)];
        const int caret_col = clampCol(caret_line, selection_.caret_col);
        int delete_end = caret_col + 1;
        const int line_size = static_cast<int>(line.size());
        while (delete_end < line_size &&
               (static_cast<unsigned char>(line[static_cast<std::size_t>(delete_end)]) & 0xC0) == 0x80)
            ++delete_end;
        line.erase(static_cast<std::size_t>(caret_col),
            static_cast<std::size_t>(delete_end - caret_col));
        rebuildBufferFromLines();
        return;
    }
    if (selection_.caret_line < lineCount() - 1) {
        pushUndoRange(selection_.caret_line, selection_.caret_line + 1);
        const auto caret = static_cast<std::size_t>(selection_.caret_line);
        cache_.lines[caret] += cache_.lines[caret + 1];
        cache_.lines.erase(cache_.lines.begin() + static_cast<std::ptrdiff_t>(caret + 1));
        cache_.tokens.erase(cache_.tokens.begin() + static_cast<std::ptrdiff_t>(caret + 1));
        cache_.line_hashes.erase(cache_.line_hashes.begin() +
            static_cast<std::ptrdiff_t>(caret + 1));
        rebuildBufferFromLines();
    }
}

void AidaCodeDocument::insertTextAtCaret(const std::string& text, int coalesce_kind)
{
    if (selection_.has_selection()) { deleteSelection(); breakUndoCoalescing(); }
    else pushUndo(coalesce_kind);

    const int line = clampLine(selection_.caret_line);
    const int col  = clampCol(line, selection_.caret_col);
    const std::size_t line_idx = static_cast<std::size_t>(line);

    std::vector<std::string> ins_lines;
    {
        const char* p = text.c_str();
        const char* s = p;
        while (*p) {
            if (*p == '\n') {
                ins_lines.emplace_back(s, p);
                s = p + 1;
            }
            p++;
        }
        ins_lines.emplace_back(s, p);
    }

    if (ins_lines.size() == 1) {
        cache_.lines[line_idx].insert(static_cast<std::size_t>(col), ins_lines.front());
        selection_.caret_col = selection_.anchor_col = col + static_cast<int>(ins_lines.front().size());
    } else {
        std::string tail = cache_.lines[line_idx].substr(static_cast<std::size_t>(col));
        cache_.lines[line_idx] = cache_.lines[line_idx].substr(0, static_cast<std::size_t>(col)) + ins_lines.front();

        for (std::size_t i = 1; i < ins_lines.size() - 1; i++) {
            const std::size_t insertion_idx = line_idx + i;
            cache_.lines.insert(cache_.lines.begin() + static_cast<std::ptrdiff_t>(insertion_idx), ins_lines[i]);
            cache_.tokens.insert(cache_.tokens.begin() + static_cast<std::ptrdiff_t>(insertion_idx), {});
            cache_.line_hashes.insert(cache_.line_hashes.begin() +
                static_cast<std::ptrdiff_t>(insertion_idx), 0);
        }

        const int last_idx = line + static_cast<int>(ins_lines.size()) - 1;
        std::string last_line = ins_lines.back() + tail;
        cache_.lines.insert(cache_.lines.begin() + static_cast<std::ptrdiff_t>(last_idx), last_line);
        cache_.tokens.insert(cache_.tokens.begin() + static_cast<std::ptrdiff_t>(last_idx), {});
        cache_.line_hashes.insert(cache_.line_hashes.begin() +
            static_cast<std::ptrdiff_t>(last_idx), 0);

        selection_.caret_line = selection_.anchor_line = last_idx;
        selection_.caret_col  = selection_.anchor_col  = static_cast<int>(ins_lines.back().size());
    }
    selection_.active = false;
    rebuildBufferFromLines();
}

void AidaCodeDocument::undo()
{
    if (undo_.empty()) return;
    breakUndoCoalescing();
    code_editor_widget::undo_entry_t entry = std::move(undo_.back());
    undo_.pop_back();
    std::size_t removed_bytes = 0;
    std::size_t inserted_bytes = 0;
    for (const auto& line : entry.after_lines) removed_bytes += line.size();
    for (const auto& line : entry.before_lines) inserted_bytes += line.size();
    const std::int64_t byte_delta = static_cast<std::int64_t>(inserted_bytes) -
        static_cast<std::int64_t>(removed_bytes) +
        static_cast<std::int64_t>(entry.before_lines.size()) -
        static_cast<std::int64_t>(entry.after_lines.size());
    cache_.content_bytes = static_cast<std::size_t>((std::max)(std::int64_t{0},
        static_cast<std::int64_t>(cache_.content_bytes) + byte_delta));
    const auto begin = cache_.lines.begin() + entry.start_line;
    cache_.lines.erase(begin, begin + static_cast<std::ptrdiff_t>(entry.after_lines.size()));
    cache_.lines.insert(cache_.lines.begin() + entry.start_line,
        entry.before_lines.begin(), entry.before_lines.end());
    const auto token_begin = cache_.tokens.begin() + entry.start_line;
    cache_.tokens.erase(token_begin,
        token_begin + static_cast<std::ptrdiff_t>(entry.after_lines.size()));
    cache_.tokens.insert(cache_.tokens.begin() + entry.start_line,
        entry.before_lines.size(), std::vector<syntax::token_t>{});
    const auto hash_begin = cache_.line_hashes.begin() + entry.start_line;
    cache_.line_hashes.erase(hash_begin,
        hash_begin + static_cast<std::ptrdiff_t>(entry.after_lines.size()));
    cache_.line_hashes.insert(cache_.line_hashes.begin() + entry.start_line,
        entry.before_lines.size(), 0);
    selection_.caret_line = selection_.anchor_line = entry.before_caret_line;
    selection_.caret_col  = selection_.anchor_col  = entry.before_caret_col;
    selection_.active = false;
    redo_.push_back(std::move(entry));
    const std::size_t redo_budget = largeFileMode() ? LARGE_HISTORY_BUDGET_BYTES : HISTORY_BUDGET_BYTES;
    const std::size_t redo_limit = largeFileMode() ? 8U : static_cast<std::size_t>(UNDO_MAX);
    std::size_t redo_bytes = 0;
    for (const auto& item : redo_) redo_bytes += item.memory_bytes;
    while (!redo_.empty() && (redo_.size() > redo_limit || redo_bytes > redo_budget)) {
        redo_bytes -= redo_.front().memory_bytes;
        redo_.erase(redo_.begin());
    }
    pending_edit_ = {};
    rebuildBufferFromLines(true);
}

void AidaCodeDocument::redo()
{
    if (redo_.empty()) return;
    breakUndoCoalescing();
    code_editor_widget::undo_entry_t entry = std::move(redo_.back());
    redo_.pop_back();
    std::size_t removed_bytes = 0;
    std::size_t inserted_bytes = 0;
    for (const auto& line : entry.before_lines) removed_bytes += line.size();
    for (const auto& line : entry.after_lines) inserted_bytes += line.size();
    const std::int64_t byte_delta = static_cast<std::int64_t>(inserted_bytes) -
        static_cast<std::int64_t>(removed_bytes) +
        static_cast<std::int64_t>(entry.after_lines.size()) -
        static_cast<std::int64_t>(entry.before_lines.size());
    cache_.content_bytes = static_cast<std::size_t>((std::max)(std::int64_t{0},
        static_cast<std::int64_t>(cache_.content_bytes) + byte_delta));
    const auto begin = cache_.lines.begin() + entry.start_line;
    cache_.lines.erase(begin, begin + static_cast<std::ptrdiff_t>(entry.before_lines.size()));
    cache_.lines.insert(cache_.lines.begin() + entry.start_line,
        entry.after_lines.begin(), entry.after_lines.end());
    const auto token_begin = cache_.tokens.begin() + entry.start_line;
    cache_.tokens.erase(token_begin,
        token_begin + static_cast<std::ptrdiff_t>(entry.before_lines.size()));
    cache_.tokens.insert(cache_.tokens.begin() + entry.start_line,
        entry.after_lines.size(), std::vector<syntax::token_t>{});
    const auto hash_begin = cache_.line_hashes.begin() + entry.start_line;
    cache_.line_hashes.erase(hash_begin,
        hash_begin + static_cast<std::ptrdiff_t>(entry.before_lines.size()));
    cache_.line_hashes.insert(cache_.line_hashes.begin() + entry.start_line,
        entry.after_lines.size(), 0);
    selection_.caret_line = selection_.anchor_line = entry.after_caret_line;
    selection_.caret_col  = selection_.anchor_col  = entry.after_caret_col;
    selection_.active = false;
    undo_.push_back(std::move(entry));
    const std::size_t undo_budget = largeFileMode() ? LARGE_HISTORY_BUDGET_BYTES : HISTORY_BUDGET_BYTES;
    const std::size_t undo_limit = largeFileMode() ? 8U : static_cast<std::size_t>(UNDO_MAX);
    std::size_t undo_bytes = 0;
    for (const auto& item : undo_) undo_bytes += item.memory_bytes;
    while (!undo_.empty() && (undo_.size() > undo_limit || undo_bytes > undo_budget)) {
        undo_bytes -= undo_.front().memory_bytes;
        undo_.erase(undo_.begin());
    }
    pending_edit_ = {};
    rebuildBufferFromLines(true);
}

std::string AidaCodeDocument::serializedContent()
{
    if (serialized_dirty_ && !cache_.dirty) {
        serialized_content_ = serialize_lines(cache_);
        serialized_dirty_ = false;
    }
    return serialized_content_;
}

quint64 AidaCodeDocument::contentFingerprint()
{
    if (mapped_source_) return 0;
    if (fingerprint_revision_ == revision_ && content_fingerprint_ != 0)
        return content_fingerprint_;
    std::uint64_t hash = 14695981039346656037ULL;
    std::size_t byte_count = 0;
    if (!cache_.dirty) {
        for (std::size_t line = 0; line < cache_.lines.size(); ++line) {
            if (line != 0) {
                hash ^= static_cast<unsigned char>('\n');
                hash *= 1099511628211ULL;
                ++byte_count;
            }
            for (const char character : cache_.lines[line]) {
                hash ^= static_cast<unsigned char>(character);
                hash *= 1099511628211ULL;
                ++byte_count;
            }
        }
    } else {
        for (const char character : serialized_content_) {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1099511628211ULL;
            ++byte_count;
        }
    }
    hash ^= static_cast<std::uint64_t>(byte_count);
    hash *= 1099511628211ULL;
    if (hash == 0) hash = 1;
    content_fingerprint_ = hash;
    fingerprint_revision_ = revision_;
    return hash;
}

void AidaCodeDocument::rebuildBufferFromExternal(const std::string& text)
{
    cache_.lines = split_to_lines(text);
    if (cache_.lines.empty()) cache_.lines.push_back("");
    cache_.tokens.assign(cache_.lines.size(), {});
    cache_.line_hashes.assign(cache_.lines.size(), 0);
    cache_.dirty = false;
    rebuildBufferFromLines();
}

void AidaCodeDocument::rebuildFoldProjection()
{
    const std::uint64_t revision = revision_ ^
        (static_cast<std::uint64_t>(folded_lines_.size()) << 48U);
    if (folded_lines_.empty()) {
        fold_ends_.clear();
        visible_lines_.clear();
        logical_to_visual_.clear();
        fold_projection_revision_ = revision;
        return;
    }
    if (fold_projection_revision_ == revision &&
        logical_to_visual_.size() == static_cast<std::size_t>(lineCount())) return;
    fold_ends_.clear();
    for (const int start : folded_lines_) {
        const int end = cachedFoldEnd(start);
        if (end > start) fold_ends_.emplace(start, end);
    }
    visible_lines_.clear();
    logical_to_visual_.assign(static_cast<std::size_t>(lineCount()), -1);
    for (int line = 0; line < lineCount();) {
        const int visual = static_cast<int>(visible_lines_.size());
        visible_lines_.push_back(line);
        logical_to_visual_[static_cast<std::size_t>(line)] = visual;
        const auto folded = fold_ends_.find(line);
        if (folded == fold_ends_.end()) { ++line; continue; }
        for (int hidden = line + 1; hidden <= folded->second; ++hidden)
            logical_to_visual_[static_cast<std::size_t>(hidden)] = visual;
        line = folded->second + 1;
    }
    fold_projection_revision_ = revision;
}

void AidaCodeDocument::revealLogicalLine(int line)
{
    for (auto it = fold_ends_.begin(); it != fold_ends_.end(); ++it) {
        if (line > it->first && line <= it->second) {
            folded_lines_.erase(std::remove(folded_lines_.begin(),
                folded_lines_.end(), it->first), folded_lines_.end());
            fold_projection_revision_ = 0;
            rebuildFoldProjection();
            return;
        }
    }
}

int AidaCodeDocument::visualRowFor(int logical_line) const
{
    return logical_line >= 0 && logical_line < static_cast<int>(logical_to_visual_.size())
        ? logical_to_visual_[static_cast<std::size_t>(logical_line)] : logical_line;
}

int AidaCodeDocument::logicalLineFor(int visual_row) const
{
    return visible_lines_.empty() ? const_cast<AidaCodeDocument*>(this)->clampLine(visual_row)
        : visible_lines_[static_cast<std::size_t>(std::clamp(
            visual_row, 0, static_cast<int>(visible_lines_.size()) - 1))];
}

void AidaCodeDocument::ensureCaretVisiblePixels(qreal viewport_h, qreal line_h,
    qreal view_text_w, qreal char_w, qreal max_scroll_x)
{
    rebuildFoldProjection();
    revealLogicalLine(selection_.caret_line);
    const int visual_line = visualRowFor(selection_.caret_line);
    const qreal caret_y = static_cast<qreal>((std::max)(visual_line, 0)) * line_h;
    if (caret_y < scroll_y_)
        target_scroll_y_ = static_cast<float>(caret_y);
    else if (caret_y + line_h > scroll_y_ + viewport_h)
        target_scroll_y_ = static_cast<float>(caret_y - viewport_h + line_h * 2.0);

    if (view_text_w <= 0.0) return;
    const int caret_line = clampLine(selection_.caret_line);
    const int caret_col = clampCol(caret_line, selection_.caret_col);
    const std::string& caret_text = lineAt(caret_line);
    const int caret_cells = line_cell_width(std::string_view(caret_text).substr(0,
        static_cast<std::size_t>(caret_col)), (std::max)(1, g_sa_settings.editor_tab_size));
    const qreal caret_x = static_cast<qreal>(caret_cells) * char_w;
    const qreal pad = char_w * 4.0;
    if (caret_x - pad < scroll_x_)
        scroll_x_ = static_cast<float>((std::max)(0.0, caret_x - pad));
    else if (caret_x + pad > scroll_x_ + view_text_w)
        scroll_x_ = static_cast<float>(caret_x + pad - view_text_w);
    if (scroll_x_ > max_scroll_x) scroll_x_ = static_cast<float>(max_scroll_x);
    if (scroll_x_ < 0.f) scroll_x_ = 0.f;
}

int AidaCodeDocument::discoverFoldEnd(int start_line)
{
    if (start_line < 0 || start_line >= lineCount() - 1) return start_line;
    int brace_depth = 0;
    bool saw_open = false;
    bool quoted = false;
    char quote = 0;
    bool escaped = false;
    for (int line = start_line; line < lineCount(); ++line) {
        if (line > start_line && !saw_open) break;
        const std::string& text = lineAt(line);
        for (std::size_t column = 0; column < text.size(); ++column) {
            const char ch = text[column];
            if (quoted) {
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == quote) quoted = false;
                continue;
            }
            if (ch == '\'' || ch == '"') { quoted = true; quote = ch; continue; }
            if (ch == '/' && column + 1U < text.size() && text[column + 1U] == '/') break;
            if (ch == '{' || ch == '[' || ch == '(') { ++brace_depth; saw_open = true; }
            else if (ch == '}' || ch == ']' || ch == ')') {
                if (saw_open && --brace_depth == 0) return line;
            }
        }
    }
    const auto indentation_width = [](std::string_view line) {
        int width = 0;
        for (const char ch : line) {
            if (ch == ' ') ++width;
            else if (ch == '\t') width += (std::max)(1, g_sa_settings.editor_tab_size);
            else break;
        }
        return width;
    };
    const int base_indent = indentation_width(lineAt(start_line));
    int last_content = start_line;
    for (int line = start_line + 1; line < lineCount(); ++line) {
        const std::string& text = lineAt(line);
        const auto first = text.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        if (indentation_width(text) <= base_indent) break;
        last_content = line;
    }
    return last_content;
}

int AidaCodeDocument::cachedFoldEnd(int start_line)
{
    if (fold_candidate_revision_ != revision_) {
        fold_candidates_.clear();
        fold_candidate_revision_ = revision_;
    }
    const auto found = fold_candidates_.find(start_line);
    if (found != fold_candidates_.end()) return found->second;
    const int end = discoverFoldEnd(start_line);
    if (fold_candidates_.size() >= 4096U) fold_candidates_.clear();
    fold_candidates_.emplace(start_line, end);
    return end;
}

bool AidaCodeDocument::foldHasEnd(int line) const
{
    return fold_ends_.find(line) != fold_ends_.end();
}

bool AidaCodeDocument::findMatchingBracket(int line, int col, int& out_line, int& out_col,
    char& out_ch)
{
    const std::string& cur = lineAt(line);
    const char here = (col >= 0 && col < static_cast<int>(cur.size()))
        ? cur[static_cast<std::size_t>(col)] : 0;
    const char before = (col > 0 && col - 1 < static_cast<int>(cur.size()))
        ? cur[static_cast<std::size_t>(col - 1)] : 0;

    int probe_line = line;
    int probe_col  = col;
    char open_ch   = 0;
    bool forward   = true;

    if (is_open_bracket(here) || is_close_bracket(here)) {
        open_ch  = here;
        forward  = is_open_bracket(here);
    } else if (is_open_bracket(before) || is_close_bracket(before)) {
        open_ch  = before;
        probe_col = col - 1;
        forward  = is_open_bracket(before);
    } else {
        return false;
    }

    char want_open  = forward ? open_ch : 0;
    char want_close = 0;
    if (forward) {
        want_close = matching_close_bracket(open_ch);
    } else {
        if (open_ch == ')') { want_open = '('; want_close = ')'; }
        else if (open_ch == ']') { want_open = '['; want_close = ']'; }
        else if (open_ch == '}') { want_open = '{'; want_close = '}'; }
    }
    if (want_open == 0 || want_close == 0) return false;

    int depth = 0;
    if (forward) {
        for (int li = probe_line; li < lineCount(); ++li) {
            const std::string& ln = lineAt(li);
            const int start = (li == probe_line) ? probe_col : 0;
            for (int ci = start; ci < static_cast<int>(ln.size()); ++ci) {
                const char c = ln[static_cast<std::size_t>(ci)];
                if (c == want_open) depth++;
                else if (c == want_close) {
                    depth--;
                    if (depth == 0) {
                        out_line = li; out_col = ci; out_ch = c;
                        return true;
                    }
                }
            }
        }
    } else {
        for (int li = probe_line; li >= 0; --li) {
            const std::string& ln = lineAt(li);
            const int start = (li == probe_line) ? probe_col : static_cast<int>(ln.size()) - 1;
            for (int ci = start; ci >= 0; --ci) {
                const char c = ln[static_cast<std::size_t>(ci)];
                if (c == want_close) depth++;
                else if (c == want_open) {
                    depth--;
                    if (depth == 0) {
                        out_line = li; out_col = ci; out_ch = c;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool AidaCodeDocument::toggleFold(int line)
{
    if (line < 0) return false;
    auto& folds = folded_lines_;
    const auto position = std::lower_bound(folds.begin(), folds.end(), line);
    if (position != folds.end() && *position == line) {
        folds.erase(position);
    } else if (folds.size() < 4096U) {
        const int end = cachedFoldEnd(line);
        folds.insert(position, line);
        if (end > line && selection_.caret_line > line &&
            selection_.caret_line <= end) {
            selection_.caret_line = line;
            selection_.caret_col = (std::min)(selection_.caret_col, lineLength(line));
            selection_.anchor_line = selection_.caret_line;
            selection_.anchor_col = selection_.caret_col;
            selection_.active = false;
        }
    } else {
        return false;
    }
    fold_projection_revision_ = 0;
    Q_EMIT foldsChanged();
    Q_EMIT metadataChanged();
    return true;
}

void AidaCodeDocument::setCaret(int line, int col)
{
    if (cache_.dirty)
        rebuildLines();
    const int target_line = clampLine(line);
    rebuildFoldProjection();
    revealLogicalLine(target_line);
    const int target_column = clampCol(target_line, col);
    selection_.caret_line = selection_.anchor_line = target_line;
    selection_.caret_col = selection_.anchor_col = target_column;
    selection_.active = false;
    blink_on_ = true;
    const int visual_line = visualRowFor(target_line);
    target_scroll_y_ = static_cast<float>(visual_line) *
        (std::max)(static_cast<float>(g_sa_settings.editor_font_size) * 1.55f, 1.f);
    breakUndoCoalescing();
    Q_EMIT metadataChanged();
}

void AidaCodeDocument::setScroll(float x, float y)
{
    scroll_x_ = (std::max)(x, 0.f);
    scroll_y_ = (std::max)(y, 0.f);
    target_scroll_y_ = scroll_y_;
}

std::string AidaCodeDocument::caretIdentifier()
{
    if (!active_) return {};
    const std::string* current_line = nullptr;
    if (mapped_source_) {
        const auto found = mapped_lines_.find(selection_.caret_line);
        if (found == mapped_lines_.end()) return {};
        current_line = &found->second;
    } else {
        if (cache_.dirty || selection_.caret_line < 0 ||
            static_cast<std::size_t>(selection_.caret_line) >= cache_.lines.size())
            return {};
        current_line = &cache_.lines[static_cast<std::size_t>(selection_.caret_line)];
    }
    const std::string& line = *current_line;
    int start = std::clamp(selection_.caret_col, 0, static_cast<int>(line.size()));
    int end = start;
    while (start > 0 && is_word_char(line[static_cast<std::size_t>(start - 1)]) &&
            end - start < 256) --start;
    while (end < static_cast<int>(line.size()) && is_word_char(line[static_cast<std::size_t>(end)]) &&
            end - start < 256) ++end;
    return end > start ? line.substr(static_cast<std::size_t>(start),
        static_cast<std::size_t>(end - start)) : std::string{};
}

void AidaCodeDocument::markSaved(quint64 revision, std::string_view filename,
    std::string_view filepath)
{
    filename_.assign(filename);
    filepath_.assign(filepath);
    if (revision_ == revision)
        dirty_ = false;
    Q_EMIT metadataChanged();
}

code_editor_widget::document_metadata_snapshot_t AidaCodeDocument::metadataSnapshot()
{
    code_editor_widget::document_metadata_snapshot_t result;
    result.found = true;
    result.revision = revision_;
    result.dirty = dirty_;
    result.caret_line = selection_.caret_line;
    result.caret_column = selection_.caret_col;
    result.selection_anchor_line = selection_.anchor_line;
    result.selection_anchor_column = selection_.anchor_col;
    result.selection_active = selection_.has_selection();
    result.scroll_x = scroll_x_;
    result.scroll_y = scroll_y_;
    result.folded_lines = folded_lines_;
    result.language_override = language_override_;
    result.proposal_pending = diff_.active;
    result.read_only = read_only_;
    return result;
}

code_editor_widget::document_payload_snapshot_t AidaCodeDocument::payloadSnapshot(
    quint64 expected_revision)
{
    code_editor_widget::document_payload_snapshot_t result;
    if (expected_revision != 0 && revision_ != expected_revision) return result;
    const auto metadata = metadataSnapshot();
    static_cast<code_editor_widget::document_metadata_snapshot_t&>(result) = metadata;
    if (mapped_source_) return result;
    result.content = serializedContent();
    result.content_hash = 14695981039346656037ULL;
    for (const char character : result.content) {
        result.content_hash ^= static_cast<unsigned char>(character);
        result.content_hash *= 1099511628211ULL;
    }
    result.content_hash ^= static_cast<std::uint64_t>(result.content.size());
    result.content_hash *= 1099511628211ULL;
    if (result.content_hash == 0) result.content_hash = 1;
    content_fingerprint_ = result.content_hash;
    fingerprint_revision_ = revision_;
    return result;
}

code_editor_widget::document_state_t AidaCodeDocument::stateSnapshot(
    quint64 focused_document_id) const
{
    code_editor_widget::document_state_t result;
    result.filename = filename_;
    result.filepath = filepath_;
    result.active = active_;
    result.dirty = dirty_;
    result.focused = focused_document_id == document_id_;
    result.content_bytes = mapped_source_
        ? static_cast<std::size_t>(mapped_source_->byte_length)
        : cache_.dirty ? serialized_content_.size() : cache_.content_bytes;
    result.line_count = mapped_source_ ? mapped_source_->line_offsets.size()
        : cache_.dirty ? 0U : cache_.lines.size();
    result.caret_line = selection_.caret_line;
    result.caret_column = selection_.caret_col;
    result.large_file_mode = result.content_bytes >= LARGE_FILE_BYTES;
    result.has_selection = selection_.has_selection();
    result.streamed = mapped_source_ != nullptr;
    result.stream_loading = stream_loading_;
    result.stream_error = stream_error_;
    result.capabilities.text_editing = active_ && !read_only_;
    result.capabilities.save = result.capabilities.text_editing && !filepath_.empty();
    result.capabilities.syntax_highlighting = active_;
    result.capabilities.find = active_;
    result.capabilities.replace = result.capabilities.text_editing;
    result.capabilities.goto_line = active_;
    result.capabilities.ai_diff_review = result.capabilities.text_editing &&
        result.content_bytes <= LARGE_FILE_BYTES;
    if (result.active) {
        const auto& language = resolved_language(filename_, language_override_);
        result.language = language.name ? language.name : "Text";
        result.capabilities.line_comment = language.line_comment && language.line_comment[0] != '\0';
    }
    return result;
}

code_editor_widget::document_capabilities_t AidaCodeDocument::capabilities()
{
    code_editor_widget::document_capabilities_t result;
    if (!active_) return result;
    if (cache_.dirty) rebuildLines();
    const auto& language = resolved_language(filename_, language_override_);
    result.text_editing = !largeReadOnlyMode();
    result.save = !largeReadOnlyMode() && !filepath_.empty();
    result.syntax_highlighting = true;
    result.line_comment = language.line_comment && language.line_comment[0] != '\0';
    result.find = true;
    result.replace = !largeReadOnlyMode();
    result.goto_line = true;
    result.ai_diff_review = !largeFileMode();
    return result;
}

bool AidaCodeDocument::setLanguageOverride(std::string_view language)
{
    if (language.size() > 64) return false;
    static constexpr std::array<std::string_view, 6> supported = {
        "", "C/C++", "x86 Assembly", "Python", "JSON", "Lua"
    };
    if (std::find(supported.begin(), supported.end(), language) == supported.end())
        return false;
    language_override_.assign(language);
    language_ = resolved_language(filename_, language_override_);
    language_set_ = true;
    cache_.tokens.assign(cache_.lines.size(), {});
    mapped_tokens_.clear();
    mapped_hashes_.clear();
    Q_EMIT metadataChanged();
    return true;
}

void AidaCodeDocument::resolveLanguageIfNeeded()
{
    if (!language_set_ && !filename_.empty()) {
        language_ = resolved_language(filename_, language_override_);
        language_set_ = true;
    }
}

bool AidaCodeDocument::performDocumentAction(code_editor_widget::document_action_t action)
{
    using action_t = code_editor_widget::document_action_t;
    if (!active_ || lineCount() == 0) return false;
    int first = 0;
    int last = 0;
    if (!selection_.has_selection()) {
        first = last = clampLine(selection_.caret_line);
    } else {
        int c0 = 0;
        int c1 = 0;
        selectionOrdered(first, c0, last, c1);
        first = clampLine(first);
        last = clampLine(last);
        if (c1 == 0 && last > first) --last;
    }
    switch (action) {
    case action_t::select_word: {
        int start = 0;
        int end = 0;
        {
            auto& ln = lineAt(selection_.caret_line);
            start = std::clamp(selection_.caret_col, 0, static_cast<int>(ln.size()));
            end = start;
            while (start > 0 && is_word_char(ln[static_cast<std::size_t>(start - 1)])) start--;
            while (end < static_cast<int>(ln.size()) && is_word_char(ln[static_cast<std::size_t>(end)])) end++;
        }
        selection_.anchor_line = selection_.caret_line;
        selection_.anchor_col = start;
        selection_.caret_col = end;
        selection_.active = end > start;
        Q_EMIT metadataChanged();
        return true;
    }
    case action_t::select_line:
        selection_.anchor_line = first;
        selection_.anchor_col = 0;
        selection_.caret_line = last;
        selection_.caret_col = lineLength(last);
        selection_.active = true;
        Q_EMIT metadataChanged();
        return true;
    case action_t::copy_line: {
        std::string text;
        for (int line = first; line <= last; ++line) {
            if (!text.empty()) text.push_back('\n');
            text += lineAt(line);
        }
        if (!text.empty())
            aida::qt::clipboard::set_text(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
        return true;
    }
    case action_t::copy_path:
        if (filepath_.empty()) return false;
        aida::qt::clipboard::set_text(QString::fromUtf8(filepath_.data(), static_cast<qsizetype>(filepath_.size())));
        return true;
    case action_t::duplicate_line: {
        pushUndo();
        const auto begin = cache_.lines.begin() + first;
        const auto end = cache_.lines.begin() + last + 1;
        std::vector<std::string> duplicate(begin, end);
        cache_.lines.insert(cache_.lines.begin() + last + 1, duplicate.begin(), duplicate.end());
        const int count = last - first + 1;
        cache_.tokens.insert(cache_.tokens.begin() + last + 1, static_cast<std::size_t>(count), std::vector<syntax::token_t>{});
        cache_.line_hashes.insert(cache_.line_hashes.begin() + last + 1, static_cast<std::size_t>(count), std::uint64_t{0});
        selection_.anchor_line = selection_.caret_line = last + count;
        selection_.anchor_col = selection_.caret_col = clampCol(selection_.caret_line, selection_.caret_col);
        selection_.active = false;
        rebuildBufferFromLines();
        return true;
    }
    case action_t::delete_line: {
        pushUndo();
        if (first == 0 && last == lineCount() - 1) {
            cache_.lines.assign(1, "");
            cache_.tokens.assign(1, {});
            cache_.line_hashes.assign(1, 0);
        } else {
            cache_.lines.erase(cache_.lines.begin() + first, cache_.lines.begin() + last + 1);
            cache_.tokens.erase(cache_.tokens.begin() + first, cache_.tokens.begin() + last + 1);
            cache_.line_hashes.erase(cache_.line_hashes.begin() + first, cache_.line_hashes.begin() + last + 1);
        }
        selection_.anchor_line = selection_.caret_line = (std::min)(first, lineCount() - 1);
        selection_.anchor_col = selection_.caret_col = 0;
        selection_.active = false;
        rebuildBufferFromLines();
        return true;
    }
    case action_t::move_line_up:
        if (first <= 0) return false;
        pushUndoRange(first - 1, last);
        std::rotate(cache_.lines.begin() + first - 1, cache_.lines.begin() + first, cache_.lines.begin() + last + 1);
        std::rotate(cache_.tokens.begin() + first - 1, cache_.tokens.begin() + first, cache_.tokens.begin() + last + 1);
        std::rotate(cache_.line_hashes.begin() + first - 1, cache_.line_hashes.begin() + first, cache_.line_hashes.begin() + last + 1);
        --selection_.anchor_line;
        --selection_.caret_line;
        rebuildBufferFromLines();
        return true;
    case action_t::move_line_down:
        if (last >= lineCount() - 1) return false;
        pushUndoRange(first, last + 1);
        std::rotate(cache_.lines.begin() + first, cache_.lines.begin() + last + 1, cache_.lines.begin() + last + 2);
        std::rotate(cache_.tokens.begin() + first, cache_.tokens.begin() + last + 1, cache_.tokens.begin() + last + 2);
        std::rotate(cache_.line_hashes.begin() + first, cache_.line_hashes.begin() + last + 1, cache_.line_hashes.begin() + last + 2);
        ++selection_.anchor_line;
        ++selection_.caret_line;
        rebuildBufferFromLines();
        return true;
    case action_t::toggle_line_comment: {
        if (!language_set_ || !language_.line_comment || language_.line_comment[0] == '\0') return false;
        const std::string marker = language_.line_comment;
        bool remove = true;
        for (int line = first; line <= last; ++line) {
            const auto& text = cache_.lines[static_cast<std::size_t>(line)];
            const std::size_t nonspace = text.find_first_not_of(" \t");
            if (nonspace == std::string::npos || text.compare(nonspace, marker.size(), marker) != 0) {
                remove = false;
                break;
            }
        }
        pushUndo();
        for (int line = first; line <= last; ++line) {
            auto& text = cache_.lines[static_cast<std::size_t>(line)];
            const std::size_t nonspace = text.find_first_not_of(" \t");
            if (nonspace == std::string::npos) continue;
            if (remove) {
                text.erase(nonspace, marker.size());
                if (nonspace < text.size() && text[nonspace] == ' ') text.erase(nonspace, 1);
            } else {
                text.insert(nonspace, marker + " ");
            }
        }
        rebuildBufferFromLines();
        return true;
    }
    case action_t::trim_trailing_whitespace: {
        bool changed = false;
        for (const auto& text : cache_.lines)
            if (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
                changed = true;
                break;
            }
        if (!changed) return false;
        pushUndoRange(0, lineCount() - 1);
        for (auto& text : cache_.lines) {
            const std::size_t end = text.find_last_not_of(" \t");
            if (end == std::string::npos) text.clear();
            else text.erase(end + 1);
        }
        selection_.caret_col = selection_.anchor_col = clampCol(selection_.caret_line, selection_.caret_col);
        rebuildBufferFromLines();
        return true;
    }
    }
    return false;
}

void AidaCodeDocument::findAllMatches()
{
    find_.match_positions.clear();
    find_.total_matches = 0;
    find_.current_match = -1;
    if (find_.find_buf[0] == '\0') return;

    std::string needle = find_.find_buf;
    if (needle.empty()) return;

    if (mapped_source_) {
        if (find_cancel_)
            find_cancel_->store(true, std::memory_order_release);
        if (find_task_id_ != 0)
            aida::infra::executor::cancel(find_task_id_);
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        auto dispatch_failed = std::make_shared<std::atomic<bool>>(false);
        find_cancel_ = cancelled;
        find_dispatch_failed_ = dispatch_failed;
        find_loading_ = true;
        find_error_.clear();
        const std::uint64_t generation = ++find_generation_;
        const std::string task_key = "editor.search." + std::to_string(document_id_) + "." +
            std::to_string(generation);
        const auto source = mapped_source_;
        const bool case_sensitive = find_.case_sensitive;
        const bool whole_word = find_.whole_word;
        const bool use_regex = find_.use_regex;
        aida::infra::executor::submission_t submission;
        submission.owner_subsystem = "code_editor";
        submission.label = "code_editor.streamed_search";
        submission.thread_class = "memory_mapped_text_search";
        submission.domain = aida::infra::executor::domain_t::feature_worker;
        submission.priority = 3;
        submission.generation = generation;
        submission.ui_access_policy = "immutable_snapshots_only";
        submission.shutdown_policy = "cancel";
        submission.cancel_hook = [cancelled] { cancelled->store(true, std::memory_order_release); };
        submission.body = [source, cancelled, dispatch_failed, generation, task_key,
                needle = std::move(needle), case_sensitive, whole_word, use_regex,
                guard = QPointer<AidaCodeDocument>(this)]() mutable {
            std::vector<code_editor_widget::find_match_t> matches;
            matches.reserve(4096);
            std::string error;
            constexpr std::size_t k_match_limit = 250000;
            try {
                std::optional<std::regex> expression;
                if (use_regex) {
                    try {
                        auto flags = std::regex_constants::ECMAScript;
                        if (!case_sensitive) flags |= std::regex_constants::icase;
                        expression.emplace(needle, flags);
                    } catch (const std::regex_error& failure) {
                        error = "Invalid regular expression: " + std::string(failure.what());
                    }
                }
                auto is_word = [](char character) {
                    const unsigned char value = static_cast<unsigned char>(character);
                    return std::isalnum(value) != 0 || character == '_';
                };
                for (std::size_t line = 0; error.empty() && line < source->line_offsets.size(); ++line) {
                    if ((line & 0x3FFU) == 0U && cancelled->load(std::memory_order_acquire)) break;
                    const std::uint64_t start = source->line_offsets[line];
                    std::uint64_t end = line + 1U < source->line_offsets.size()
                        ? source->line_offsets[line + 1U] - 1U : source->byte_length;
                    if (end > start && source->view[end - 1U] == '\r') --end;
                    const std::size_t length = static_cast<std::size_t>(end - start);
                    if (use_regex) {
                        if (length > 4U * 1024U * 1024U) {
                            error = "Regex search cannot process a single mapped line larger than 4 MiB; use literal search or Hex View.";
                            break;
                        }
                        const std::string text(source->view + start, source->view + end);
                        for (std::sregex_iterator found(text.begin(), text.end(), *expression), finish;
                                found != finish; ++found) {
                            const int column = static_cast<int>(found->position());
                            const int count = static_cast<int>(found->length());
                            const bool left_ok = !whole_word || column == 0 || !is_word(text[static_cast<std::size_t>(column - 1)]);
                            const bool right_ok = !whole_word || column + count >= static_cast<int>(text.size()) ||
                                !is_word(text[static_cast<std::size_t>(column + count)]);
                            if (left_ok && right_ok)
                                matches.push_back({static_cast<int>(line), column, count});
                            if (matches.size() >= k_match_limit) break;
                        }
                    } else if (needle.size() <= length) {
                        for (std::size_t column = 0; column + needle.size() <= length; ++column) {
                            bool equal = true;
                            for (std::size_t offset = 0; offset < needle.size(); ++offset) {
                                char left = source->view[start + column + offset];
                                char right = needle[offset];
                                if (!case_sensitive) {
                                    left = static_cast<char>(std::tolower(static_cast<unsigned char>(left)));
                                    right = static_cast<char>(std::tolower(static_cast<unsigned char>(right)));
                                }
                                if (left != right) { equal = false; break; }
                            }
                            if (!equal) continue;
                            const bool left_ok = !whole_word || column == 0 ||
                                !is_word(source->view[start + column - 1U]);
                            const bool right_ok = !whole_word || column + needle.size() == length ||
                                !is_word(source->view[start + column + needle.size()]);
                            if (left_ok && right_ok)
                                matches.push_back({static_cast<int>(line), static_cast<int>(column),
                                    static_cast<int>(needle.size())});
                            if (matches.size() >= k_match_limit) break;
                        }
                    }
                    if (matches.size() >= k_match_limit) {
                        error = "Search was truncated at 250,000 matches; refine the query to navigate deterministically.";
                        break;
                    }
                }
            } catch (const std::bad_alloc&) {
                matches.clear();
                error = "Mapped search exhausted its bounded allocation budget; refine the query or use Hex View.";
            }
            if (cancelled->load(std::memory_order_acquire) && error.empty())
                error = "Search was cancelled.";
            const bool posted = QMetaObject::invokeMethod(guard,
                [guard, generation, task_key, matches = std::move(matches),
                 error = std::move(error)]() mutable {
                    AidaCodeDocument* document = guard.data();
                    if (!document) return;
                    find_result_delivery_t delivery;
                    delivery.generation = generation;
                    delivery.task_key = std::move(task_key);
                    delivery.matches = std::move(matches);
                    delivery.error = std::move(error);
                    document->publishFindResult(std::move(delivery));
                }, Qt::QueuedConnection);
            if (!posted)
                dispatch_failed->store(true, std::memory_order_release);
        };
        const auto submitted = aida::infra::executor::submit(std::move(submission));
        if (!submitted.submitted) {
            find_loading_ = false;
            find_dispatch_failed_.reset();
            find_cancel_.reset();
            find_error_ = "The mapped-search worker could not be scheduled: " + submitted.reject_reason;
            Q_EMIT findStateChanged();
            return;
        }
        find_task_id_ = submitted.task_id;
        aida::ui::task_center::task_registration_t registration;
        registration.id = task_key;
        registration.source = "code_editor";
        registration.owner = "Code Editor";
        registration.owner_view = "document.code";
        registration.owner_action = "edit.find";
        registration.target = filepath_;
        registration.label = "Search mapped text";
        registration.stage = "Scanning memory-mapped content";
        registration.affected_entity = std::to_string(document_id_);
        registration.cancellation_is_safe = true;
        if (!aida::ui::task_center::register_executor_job(submitted.task_id, std::move(registration))) {
            aida::infra::executor::cancel(submitted.task_id);
            cancelled->store(true, std::memory_order_release);
            ++find_generation_;
            find_loading_ = false;
            find_dispatch_failed_.reset();
            find_task_id_ = 0;
            find_cancel_.reset();
            find_error_ = "Task Center could not own mapped search; the operation was cancelled.";
        }
        Q_EMIT findStateChanged();
        return;
    }

    if (find_.use_regex) {
        try {
            auto flags = std::regex_constants::ECMAScript;
            if (!find_.case_sensitive)
                flags |= std::regex_constants::icase;
            std::regex re(needle, flags);
            for (std::size_t i = 0; i < cache_.lines.size(); i++) {
                const std::string& line = cache_.lines[i];
                auto it  = std::sregex_iterator(line.begin(), line.end(), re);
                auto end = std::sregex_iterator();
                for (; it != end; ++it) {
                    const int match_len = static_cast<int>(it->length());
                    if (match_len <= 0) continue;
                    const int match_pos = static_cast<int>(it->position());
                    if (find_.whole_word) {
                        const std::size_t match_pos_idx = static_cast<std::size_t>(match_pos);
                        const std::size_t match_end_idx = static_cast<std::size_t>(match_pos + match_len);
                        const bool left_ok  = (match_pos == 0) || (!isalnum(static_cast<unsigned char>(line[match_pos_idx - 1])) && line[match_pos_idx - 1] != '_');
                        const bool right_ok = (match_end_idx >= line.size()) || (!isalnum(static_cast<unsigned char>(line[match_end_idx])) && line[match_end_idx] != '_');
                        if (!left_ok || !right_ok) continue;
                    }
                    code_editor_widget::find_match_t m;
                    m.line = static_cast<int>(i);
                    m.col = match_pos;
                    m.length = match_len;
                    find_.match_positions.push_back(m);
                }
            }
        } catch (const std::regex_error& e) {
            last_error_ = std::string("code_editor: invalid regex in live highlight: ") + e.what();
            find_.match_positions.clear();
        } catch (...) {
            last_error_ = "code_editor: invalid regex in live highlight";
            find_.match_positions.clear();
        }
    } else {
        if (!find_.case_sensitive) {
            for (auto& c : needle) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        const int needle_len = static_cast<int>(needle.size());

        for (std::size_t i = 0; i < cache_.lines.size(); i++) {
            std::string haystack = cache_.lines[i];
            if (!find_.case_sensitive) {
                for (auto& c : haystack) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            }
            std::size_t pos = 0;
            while ((pos = haystack.find(needle, pos)) != std::string::npos) {
                if (find_.whole_word) {
                    const bool left_ok  = (pos == 0) || (!isalnum(static_cast<unsigned char>(haystack[pos - 1])) && haystack[pos - 1] != '_');
                    const std::size_t match_end = pos + static_cast<std::size_t>(needle_len);
                    const bool right_ok = (match_end >= haystack.size()) || (!isalnum(static_cast<unsigned char>(haystack[match_end])) && haystack[match_end] != '_');
                    if (!left_ok || !right_ok) { pos += 1; continue; }
                }
                code_editor_widget::find_match_t m;
                m.line = static_cast<int>(i);
                m.col = static_cast<int>(pos);
                m.length = needle_len;
                find_.match_positions.push_back(m);
                pos += static_cast<std::size_t>(needle_len);
            }
        }
    }
    find_.total_matches = static_cast<int>(find_.match_positions.size());
    Q_EMIT findStateChanged();
}

void AidaCodeDocument::publishFindResult(find_result_delivery_t delivery)
{
    if (find_generation_ != delivery.generation) return;
    find_loading_ = false;
    find_dispatch_failed_.reset();
    find_task_id_ = 0;
    find_cancel_.reset();
    find_error_ = std::move(delivery.error);
    const auto task_state = find_error_.empty()
        ? aida::ui::task_center::task_state_t::completed
        : find_error_.find("cancelled") != std::string::npos
            ? aida::ui::task_center::task_state_t::cancelled
            : aida::ui::task_center::task_state_t::failed;
    static_cast<void>(aida::ui::task_center::update_task(delivery.task_key, task_state, 1.f,
        find_error_.empty() ? "Search complete" : "Search failed",
        find_error_.empty() ? "Mapped text search completed." : find_error_));
    find_.match_positions = std::move(delivery.matches);
    find_.total_matches = static_cast<int>(find_.match_positions.size());
    find_.current_match = find_.total_matches == 0 ? -1 : 0;
    Q_EMIT findStateChanged();
}

void AidaCodeDocument::findNext()
{
    if (find_.match_positions.empty()) return;
    find_.current_match = (find_.current_match + 1) % static_cast<int>(find_.match_positions.size());
    const auto& m = find_.match_positions[static_cast<std::size_t>(find_.current_match)];
    selection_.caret_line = selection_.anchor_line = m.line;
    selection_.anchor_col = m.col;
    selection_.caret_col  = m.col + m.length;
    selection_.active = true;
    Q_EMIT metadataChanged();
}

void AidaCodeDocument::findPrev()
{
    if (find_.match_positions.empty()) return;
    find_.current_match = (find_.current_match - 1 + static_cast<int>(find_.match_positions.size()))
                            % static_cast<int>(find_.match_positions.size());
    const auto& m = find_.match_positions[static_cast<std::size_t>(find_.current_match)];
    selection_.caret_line = selection_.anchor_line = m.line;
    selection_.anchor_col = m.col;
    selection_.caret_col  = m.col + m.length;
    selection_.active = true;
    Q_EMIT metadataChanged();
}

namespace {

std::string compute_replacement(const code_editor_widget::find_state_t& find,
    const std::string& line_text, const code_editor_widget::find_match_t& m)
{
    std::string replacement = find.replace_buf;
    if (!find.use_regex) return replacement;
    try {
        auto flags = std::regex_constants::ECMAScript;
        if (!find.case_sensitive)
            flags |= std::regex_constants::icase;
        std::regex re(find.find_buf, flags);
        const int col = std::clamp(m.col, 0, static_cast<int>(line_text.size()));
        const int length = std::clamp(m.length, 0, static_cast<int>(line_text.size()) - col);
        std::string slice = line_text.substr(static_cast<std::size_t>(col), static_cast<std::size_t>(length));
        return std::regex_replace(slice, re, replacement,
            std::regex_constants::format_first_only);
    } catch (...) {
        return replacement;
    }
}

}

void AidaCodeDocument::replaceCurrent()
{
    if (find_.current_match < 0 || find_.current_match >= static_cast<int>(find_.match_positions.size()))
        return;
    const auto& m = find_.match_positions[static_cast<std::size_t>(find_.current_match)];
    if (m.line < 0 || m.line >= static_cast<int>(cache_.lines.size())) return;
    pushUndoRange(m.line, m.line);
    std::string& ln = cache_.lines[static_cast<std::size_t>(m.line)];
    const int col_clamped = std::clamp(m.col, 0, static_cast<int>(ln.size()));
    const int len_clamped = std::clamp(m.length, 0, static_cast<int>(ln.size()) - col_clamped);
    const std::string replacement = compute_replacement(find_, ln, m);
    ln.erase(static_cast<std::size_t>(col_clamped), static_cast<std::size_t>(len_clamped));
    ln.insert(static_cast<std::size_t>(col_clamped), replacement);
    selection_.caret_line = selection_.anchor_line = m.line;
    selection_.anchor_col = col_clamped;
    selection_.caret_col  = col_clamped + static_cast<int>(replacement.size());
    selection_.active = true;
    rebuildBufferFromLines();
    findAllMatches();
}

void AidaCodeDocument::replaceAll()
{
    if (find_.match_positions.empty()) return;
    int first_line = find_.match_positions.front().line;
    int last_line = first_line;
    for (const auto& match : find_.match_positions) {
        first_line = (std::min)(first_line, match.line);
        last_line = (std::max)(last_line, match.line);
    }
    pushUndoRange(first_line, last_line);
    for (int i = static_cast<int>(find_.match_positions.size()) - 1; i >= 0; i--) {
        const auto& m = find_.match_positions[static_cast<std::size_t>(i)];
        if (m.line < 0 || m.line >= static_cast<int>(cache_.lines.size())) continue;
        std::string& ln = cache_.lines[static_cast<std::size_t>(m.line)];
        const int col_clamped = std::clamp(m.col, 0, static_cast<int>(ln.size()));
        const int len_clamped = std::clamp(m.length, 0, static_cast<int>(ln.size()) - col_clamped);
        const std::string replacement = compute_replacement(find_, ln, m);
        ln.erase(static_cast<std::size_t>(col_clamped), static_cast<std::size_t>(len_clamped));
        ln.insert(static_cast<std::size_t>(col_clamped), replacement);
    }
    rebuildBufferFromLines();
    findAllMatches();
}

void AidaCodeDocument::rebuildAutocomplete(const std::string& partial, int caret_line)
{
    autocomplete_.matches.clear();
    autocomplete_.selected = 0;
    if (partial.size() < 2) {
        autocomplete_.popup_visible = false;
        return;
    }

    std::string lower_pat = partial;
    for (auto& c : lower_pat) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    struct cand_t { std::string text; int score; };
    std::vector<cand_t> cands;
    std::unordered_set<std::string> seen;
    seen.insert(partial);

    for (const auto& kw : autocomplete_keywords()) {
        if (kw == partial) continue;
        std::string lk = kw;
        for (auto& c : lk) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        int sc = 0;
        if (fuzzy_subsequence_score(lower_pat, lk, sc)) {
            if (seen.insert(kw).second)
                cands.push_back({ kw, sc + 6 });
        }
    }

    std::vector<std::string> idents;
    {
        std::unordered_set<std::string> seen_ids;
        const int total = lineCount();
        const int lo = (std::max)(0, caret_line - 1500);
        const int hi = (std::min)(total, caret_line + 1500);
        for (int i = lo; i < hi; ++i) {
            const std::string& ln = cache_.lines[static_cast<std::size_t>(i)];
            std::size_t j = 0;
            while (j < ln.size()) {
                const unsigned char c = static_cast<unsigned char>(ln[j]);
                const bool starts = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
                if (!starts) { j++; continue; }
                const std::size_t s = j;
                while (j < ln.size()) {
                    const unsigned char d = static_cast<unsigned char>(ln[j]);
                    const bool cont = (d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                                (d >= '0' && d <= '9') || d == '_';
                    if (!cont) break;
                    j++;
                }
                if (j - s >= 3 && j - s <= 80) {
                    std::string word = ln.substr(s, j - s);
                    if (seen_ids.insert(word).second)
                        idents.push_back(std::move(word));
                }
            }
        }
    }
    for (const auto& id : idents) {
        if (id == partial) continue;
        std::string li = id;
        for (auto& c : li) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        int sc = 0;
        if (fuzzy_subsequence_score(lower_pat, li, sc)) {
            if (seen.insert(id).second)
                cands.push_back({ id, sc });
        }
    }

    std::stable_sort(cands.begin(), cands.end(),
        [](const cand_t& a, const cand_t& b) { return a.score > b.score; });

    int cap = static_cast<int>(cands.size());
    if (cap > 12) cap = 12;
    for (int i = 0; i < cap; ++i)
        autocomplete_.matches.push_back(cands[static_cast<std::size_t>(i)].text);

    autocomplete_.partial      = partial;
    autocomplete_.popup_visible = !autocomplete_.matches.empty();
    autocomplete_.cursor_line  = caret_line;
}

void AidaCodeDocument::ghostResetForCaretMove()
{
    ghost_.trigger_line = selection_.caret_line;
    ghost_.trigger_col = selection_.caret_col;
    ghost_.text.clear();
    ghost_.visible_for_line = -1;
    ghost_.visible_for_col = -1;
}

void AidaCodeDocument::ghostCancelRequest()
{
    ghost_.requesting = false;
}

void AidaCodeDocument::ghostTabAccept()
{
    if (ghost_.text.empty()) return;
    insertTextAtCaret(ghost_.text);
    ghost_.text.clear();
    ghost_.visible_for_line = -1;
    ghost_.visible_for_col = -1;
}

void AidaCodeDocument::ghostDismiss()
{
    ghost_.text.clear();
    ghost_.visible_for_line = -1;
    ghost_.visible_for_col = -1;
}

void AidaCodeDocument::publishGhostResult(std::string result)
{
    ghost_.text = std::move(result);
    ghost_.requesting = false;
    ghost_.visible_for_line = selection_.caret_line;
    ghost_.visible_for_col = selection_.caret_col;
    Q_EMIT metadataChanged();
}

void AidaCodeDocument::ghostStartDebounce()
{
    if (!g_sa_settings.ghost_text_enabled || !has_focus_ || read_only_ || mapped_source_)
        return;
    if (cache_.dirty)
        rebuildLines();
    if (!ghost_.text.empty() || ghost_.requesting)
        return;
    const int n_lines = lineCount();
    if (n_lines <= 0)
        return;

    const int ctx_start = (std::max)(0, selection_.caret_line - 20);
    std::string context;
    context.reserve(2048);
    for (int i = ctx_start; i < n_lines && i <= selection_.caret_line; i++) {
        const std::string& ln = lineAt(i);
        if (i == selection_.caret_line) {
            const int col = std::clamp(selection_.caret_col, 0, static_cast<int>(ln.size()));
            context.append(ln, 0, static_cast<std::size_t>(col));
        } else {
            context += ln;
            context += '\n';
        }
    }
    if (context.empty())
        return;
    if (!g_sa_ai_client)
        return;
    ghost_.requesting = true;
    const int trigger_line = ghost_.trigger_line;
    const int trigger_col = ghost_.trigger_col;
    QPointer<AidaCodeDocument> guard(this);
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "code_editor";
    sub.label = "code_editor.ghost_completion";
    sub.thread_class = "bounded_task";
    sub.domain = aida::infra::executor::domain_t::feature_worker;
    sub.priority = 3;
    sub.body = [context, trigger_line, trigger_col, guard]() {
        std::string prompt = "Complete the following code. Output ONLY the completion text (the part that comes after the cursor), nothing else. No explanation, no markdown. If there's nothing meaningful to suggest, output nothing.\n\n```\n" + context + "```";
        std::vector<std::pair<std::string, std::string>> empty_history;
        std::string result = g_sa_ai_client->chat_blocking(prompt, empty_history);

        if (result.size() > 6 && result.substr(0, 3) == "```") {
            auto nl = result.find('\n');
            if (nl != std::string::npos) result = result.substr(nl + 1);
            if (result.size() >= 3 && result.substr(result.size()-3) == "```")
                result.resize(result.size()-3);
        }

        auto nl = result.find('\n');
        if (nl != std::string::npos) result.resize(nl);

        while (!result.empty() && (result.back() == ' ' || result.back() == '\t' || result.back() == '\r'))
            result.pop_back();

        if (result.find("Error:") == 0 || result.find("error") == 0 ||
            result.find("{\"error\"") != std::string::npos ||
            result.find("API returned status") != std::string::npos) {
            result.clear();
        }

        const bool posted = QMetaObject::invokeMethod(guard,
            [guard, result = std::move(result), trigger_line, trigger_col]() mutable {
                AidaCodeDocument* document = guard.data();
                if (!document) return;
                if (document->selection().caret_line != trigger_line ||
                    document->selection().caret_col != trigger_col) {
                    document->ghost().requesting = false;
                    return;
                }
                document->publishGhostResult(std::move(result));
            }, Qt::QueuedConnection);
        if (!posted && guard)
            guard->ghost().requesting = false;
    };
    if (!aida::infra::executor::submit(std::move(sub)).submitted)
        ghost_.requesting = false;
}

bool AidaCodeDocument::beginAgentEdit(std::string_view origin)
{
    if (!active_) {
        last_error_ = "code_editor: begin_agent_edit called with no active document";
        return false;
    }
    if (!capabilities().ai_diff_review) {
        last_error_ = "code_editor: AI diff review is unavailable in large-file mode";
        return false;
    }
    std::vector<std::string> base = split_to_lines(serializedContent());
    std::lock_guard<std::mutex> lk(diff_mutex_);
    diff_ = code_editor_widget::pending_diff_t{};
    diff_.active = true;
    diff_.document_id = document_id_;
    diff_.base_revision = revision_;
    diff_.base_content_hash = contentFingerprint();
    diff_.origin = std::string(origin);
    diff_.old_lines = base;
    diff_.new_lines = base;
    assign_diff_identities(diff_);
    review_hunk_selection_ = {};
    diff_hover_hunk_ = -1;
    Q_EMIT diffChanged();
    return true;
}

bool AidaCodeDocument::proposeFullContent(std::string_view new_content)
{
    if (!active_) {
        last_error_ = "code_editor: propose_full_content called with no active document";
        return false;
    }
    if (!capabilities().ai_diff_review) {
        last_error_ = "code_editor: AI diff review is unavailable in large-file mode";
        return false;
    }
    std::vector<std::string> old_lines = split_to_lines(serializedContent());
    std::vector<std::string> new_lines = split_to_lines(new_content);
    std::string origin;
    {
        std::lock_guard<std::mutex> lk(diff_mutex_);
        origin = diff_.active ? diff_.origin : std::string("agent");
    }
    rebuildPendingFromProposal(origin, old_lines, new_lines,
        document_id_, revision_, contentFingerprint());
    return true;
}

bool AidaCodeDocument::proposeContent(quint64 base_revision, quint64 base_content_hash,
    std::string_view current_content, std::string_view new_content, std::string_view origin)
{
    if (document_id_ == 0 || base_revision == 0 || base_content_hash == 0) {
        last_error_ = "code_editor: proposal binding is incomplete";
        return false;
    }
    std::uint64_t observed_hash = 14695981039346656037ULL;
    for (const char character : current_content) {
        observed_hash ^= static_cast<unsigned char>(character);
        observed_hash *= 1099511628211ULL;
    }
    observed_hash ^= static_cast<std::uint64_t>(current_content.size());
    observed_hash *= 1099511628211ULL;
    if (observed_hash == 0)
        observed_hash = 1;
    if (observed_hash != base_content_hash) {
        last_error_ = "code_editor: proposal base content changed before review creation";
        return false;
    }
    if (revision_ != base_revision) {
        last_error_ = "code_editor: document changed before review creation";
        return false;
    }
    code_editor_widget::pending_diff_t proposal;
    proposal.active = true;
    proposal.document_id = document_id_;
    proposal.base_revision = base_revision;
    proposal.base_content_hash = base_content_hash;
    proposal.origin = std::string(origin);
    proposal.old_lines = split_to_lines(current_content);
    proposal.new_lines = split_to_lines(new_content);
    compute_lcs_diff(proposal.old_lines, proposal.new_lines, proposal);
    std::lock_guard<std::mutex> lock(diff_mutex_);
    diff_ = std::move(proposal);
    review_hunk_selection_ = {};
    if (!diff_.hunks.empty())
        static_cast<void>(selectReviewHunkLocked(0, false));
    diff_hover_hunk_ = -1;
    diff_scroll_target_ = -1.f;
    last_error_.clear();
    Q_EMIT diffChanged();
    return true;
}

bool AidaCodeDocument::proposeReplaceRange(int start_line, int end_line,
    std::string_view replacement)
{
    if (!active_) {
        last_error_ = "code_editor: propose_replace_range called with no active document";
        return false;
    }
    if (!capabilities().ai_diff_review) {
        last_error_ = "code_editor: AI diff review is unavailable in large-file mode";
        return false;
    }

    std::vector<std::string> old_lines = split_to_lines(serializedContent());
    int n = static_cast<int>(old_lines.size());
    if (start_line < 0) start_line = 0;
    if (end_line < start_line) end_line = start_line;
    if (start_line > n) start_line = n;
    if (end_line > n) end_line = n;

    std::vector<std::string> repl = split_to_lines(replacement);
    if (replacement.empty()) repl.clear();

    std::vector<std::string> new_lines;
    new_lines.reserve(old_lines.size());
    for (int i = 0; i < start_line && i < n; ++i)
        new_lines.push_back(old_lines[static_cast<std::size_t>(i)]);
    for (auto& r : repl)
        new_lines.push_back(std::move(r));
    for (int i = end_line; i < n; ++i)
        new_lines.push_back(old_lines[static_cast<std::size_t>(i)]);
    if (new_lines.empty()) new_lines.push_back("");

    std::string origin;
    {
        std::lock_guard<std::mutex> lk(diff_mutex_);
        origin = diff_.active ? diff_.origin : std::string("agent");
    }
    rebuildPendingFromProposal(origin, old_lines, new_lines,
        document_id_, revision_, contentFingerprint());
    return true;
}

void AidaCodeDocument::rebuildPendingFromProposal(const std::string& origin,
    const std::vector<std::string>& old_lines, const std::vector<std::string>& new_lines,
    quint64 document_id, quint64 base_revision, quint64 base_content_hash)
{
    std::lock_guard<std::mutex> lk(diff_mutex_);
    diff_.active        = true;
    diff_.document_id = document_id;
    diff_.base_revision = base_revision;
    diff_.base_content_hash = base_content_hash;
    diff_.origin        = origin;
    diff_.old_lines     = old_lines;
    diff_.new_lines     = new_lines;
    compute_lcs_diff(old_lines, new_lines, diff_);
    diff_hover_hunk_    = -1;
    review_hunk_selection_ = {};
    if (!diff_.hunks.empty())
        static_cast<void>(selectReviewHunkLocked(0, true));
    Q_EMIT diffChanged();
}

code_editor_widget::review_hunk_identity_t AidaCodeDocument::reviewHunkIdentityLocked(
    int index) const
{
    code_editor_widget::review_hunk_identity_t identity;
    const auto& diff = diff_;
    if (!diff.active || index < 0 || index >= static_cast<int>(diff.hunks.size()))
        return identity;
    const auto& hunk = diff.hunks[static_cast<std::size_t>(index)];
    auto* self = const_cast<AidaCodeDocument*>(this);
    if (diff.document_id != document_id_ || diff.base_revision != revision_ ||
        diff.proposal_id == 0 ||
        diff.base_content_hash != self->contentFingerprint() || hunk.stable_id == 0)
        return identity;
    identity.document_id = diff.document_id;
    identity.proposal_id = diff.proposal_id;
    identity.base_revision = diff.base_revision;
    identity.stable_hunk_id = hunk.stable_id;
    return identity;
}

int AidaCodeDocument::resolveReviewHunkLocked(
    const code_editor_widget::review_hunk_identity_t& identity, bool require_pending) const
{
    const auto& diff = diff_;
    auto* self = const_cast<AidaCodeDocument*>(this);
    if (!identity.valid() || !diff.active ||
        identity.document_id != document_id_ ||
        identity.document_id != diff.document_id ||
        identity.proposal_id != diff.proposal_id ||
        identity.base_revision != revision_ ||
        identity.base_revision != diff.base_revision ||
        diff.base_content_hash != self->contentFingerprint())
        return -1;
    int match = -1;
    for (std::size_t index = 0; index < diff.hunks.size(); ++index) {
        if (diff.hunks[index].stable_id != identity.stable_hunk_id)
            continue;
        if (match >= 0)
            return -1;
        match = static_cast<int>(index);
    }
    if (match < 0 || (require_pending &&
        diff.hunks[static_cast<std::size_t>(match)].state !=
            code_editor_widget::diff_hunk_state_t::pending))
        return -1;
    return match;
}

bool AidaCodeDocument::selectReviewHunkLocked(int index, bool request_focus)
{
    const auto identity = reviewHunkIdentityLocked(index);
    if (!identity.valid() || resolveReviewHunkLocked(identity, true) != index)
        return false;
    review_hunk_selection_.identity = identity;
    review_hunk_selection_.focus_requested = request_focus;
    if (request_focus)
        has_focus_ = true;
    return true;
}

bool AidaCodeDocument::selectPendingReviewHunkLocked(bool forward)
{
    const auto& hunks = diff_.hunks;
    if (!diff_.active || hunks.empty() ||
        diff_.document_id != document_id_ ||
        diff_.base_revision != revision_ ||
        diff_.base_content_hash != contentFingerprint())
        return false;
    const int current = resolveReviewHunkLocked(review_hunk_selection_.identity, false);
    const int count = static_cast<int>(hunks.size());
    const int start = current >= 0 ? current : (forward ? -1 : 0);
    for (int offset = 1; offset <= count; ++offset) {
        const int candidate = forward
            ? (start + offset) % count
            : (start - offset % count + count) % count;
        if (hunks[static_cast<std::size_t>(candidate)].state ==
            code_editor_widget::diff_hunk_state_t::pending)
            return selectReviewHunkLocked(candidate, true);
    }
    review_hunk_selection_ = {};
    return false;
}

bool AidaCodeDocument::hasPendingDiff()
{
    std::lock_guard<std::mutex> lk(diff_mutex_);
    return diff_.active && !diff_.hunks.empty();
}

int AidaCodeDocument::pendingHunkCount()
{
    std::lock_guard<std::mutex> lk(diff_mutex_);
    return static_cast<int>(diff_.hunks.size());
}

bool AidaCodeDocument::hasPendingReviewHunks()
{
    std::lock_guard<std::mutex> lock(diff_mutex_);
    if (!diff_.active || diff_.document_id != document_id_ ||
        diff_.base_revision != revision_ ||
        diff_.base_content_hash != contentFingerprint())
        return false;
    return std::any_of(diff_.hunks.begin(), diff_.hunks.end(),
        [](const code_editor_widget::diff_hunk_t& hunk) {
            return hunk.state == code_editor_widget::diff_hunk_state_t::pending;
        });
}

code_editor_widget::review_hunk_identity_t AidaCodeDocument::reviewHunkIdentity(int index)
{
    std::lock_guard<std::mutex> lock(diff_mutex_);
    return reviewHunkIdentityLocked(index);
}

code_editor_widget::review_hunk_identity_t AidaCodeDocument::selectedReviewHunkIdentityAssumeLocked()
{
    if (resolveReviewHunkLocked(review_hunk_selection_.identity, true) < 0) {
        review_hunk_selection_ = {};
        return {};
    }
    return review_hunk_selection_.identity;
}

code_editor_widget::review_hunk_identity_t AidaCodeDocument::selectedReviewHunkIdentity()
{
    std::lock_guard<std::mutex> lock(diff_mutex_);
    return selectedReviewHunkIdentityAssumeLocked();
}

int AidaCodeDocument::resolveReviewHunk(
    const code_editor_widget::review_hunk_identity_t& identity, bool require_pending)
{
    std::lock_guard<std::mutex> lock(diff_mutex_);
    return resolveReviewHunkLocked(identity, require_pending);
}

bool AidaCodeDocument::selectReviewHunk(int index, bool request_focus)
{
    std::lock_guard<std::mutex> lock(diff_mutex_);
    return selectReviewHunkLocked(index, request_focus);
}

bool AidaCodeDocument::selectNextPendingHunk()
{
    std::lock_guard<std::mutex> lock(diff_mutex_);
    return selectPendingReviewHunkLocked(true);
}

bool AidaCodeDocument::selectPreviousPendingHunk()
{
    std::lock_guard<std::mutex> lock(diff_mutex_);
    return selectPendingReviewHunkLocked(false);
}

bool AidaCodeDocument::acceptHunk(int index)
{
    std::lock_guard<std::mutex> lk(diff_mutex_);
    if (!diff_.active || diff_.document_id != document_id_ ||
        diff_.base_revision != revision_ ||
        diff_.base_content_hash != contentFingerprint() ||
        index < 0 || index >= static_cast<int>(diff_.hunks.size()))
        return false;
    const auto identity = reviewHunkIdentityLocked(index);
    if (resolveReviewHunkLocked(identity, true) != index)
        return false;
    diff_.hunks[static_cast<std::size_t>(index)].state = code_editor_widget::diff_hunk_state_t::accepted;
    static_cast<void>(selectPendingReviewHunkLocked(true));
    Q_EMIT diffChanged();
    return true;
}

bool AidaCodeDocument::rejectHunk(int index)
{
    std::lock_guard<std::mutex> lk(diff_mutex_);
    if (!diff_.active || diff_.document_id != document_id_ ||
        diff_.base_revision != revision_ ||
        diff_.base_content_hash != contentFingerprint() ||
        index < 0 || index >= static_cast<int>(diff_.hunks.size()))
        return false;
    const auto identity = reviewHunkIdentityLocked(index);
    if (resolveReviewHunkLocked(identity, true) != index)
        return false;
    diff_.hunks[static_cast<std::size_t>(index)].state = code_editor_widget::diff_hunk_state_t::rejected;
    static_cast<void>(selectPendingReviewHunkLocked(true));
    Q_EMIT diffChanged();
    return true;
}

void AidaCodeDocument::acceptAllHunks()
{
    std::lock_guard<std::mutex> lk(diff_mutex_);
    if (!diff_.active || diff_.document_id != document_id_ ||
        diff_.base_revision != revision_ ||
        diff_.base_content_hash != contentFingerprint()) return;
    for (auto& h : diff_.hunks)
        h.state = code_editor_widget::diff_hunk_state_t::accepted;
    review_hunk_selection_ = {};
    Q_EMIT diffChanged();
}

void AidaCodeDocument::rejectAllHunks()
{
    std::lock_guard<std::mutex> lk(diff_mutex_);
    if (!diff_.active || diff_.document_id != document_id_ ||
        diff_.base_revision != revision_ ||
        diff_.base_content_hash != contentFingerprint()) return;
    for (auto& h : diff_.hunks)
        h.state = code_editor_widget::diff_hunk_state_t::rejected;
    review_hunk_selection_ = {};
    Q_EMIT diffChanged();
}

std::string AidaCodeDocument::composeResolvedText() const
{
    std::vector<std::string> result;
    result.reserve(diff_.new_lines.size() + diff_.old_lines.size());

    std::size_t old_idx = 0;
    std::size_t hi = 0;

    auto emit_context_until = [&](int old_target) {
        const std::size_t target = static_cast<std::size_t>((std::max)(0, old_target));
        while (old_idx < target && old_idx < diff_.old_lines.size()) {
            result.push_back(diff_.old_lines[old_idx]);
            old_idx++;
        }
    };

    while (hi < diff_.hunks.size()) {
        const code_editor_widget::diff_hunk_t& h = diff_.hunks[hi];

        int hunk_old_begin = h.old_count > 0 ? h.old_start : static_cast<int>(old_idx);
        if (h.old_count == 0) {
            for (const auto& dl : h.lines) {
                if (dl.kind == code_editor_widget::diff_line_kind_t::context &&
                    dl.old_line >= 0) {
                    hunk_old_begin = dl.old_line;
                    break;
                }
            }
        }
        emit_context_until(hunk_old_begin);

        if (h.state == code_editor_widget::diff_hunk_state_t::accepted) {
            for (const auto& dl : h.lines) {
                if (dl.kind == code_editor_widget::diff_line_kind_t::added ||
                    dl.kind == code_editor_widget::diff_line_kind_t::context)
                    result.push_back(dl.text);
            }
        } else {
            for (const auto& dl : h.lines) {
                if (dl.kind == code_editor_widget::diff_line_kind_t::removed ||
                    dl.kind == code_editor_widget::diff_line_kind_t::context)
                    result.push_back(dl.text);
            }
        }

        int consumed_old = 0;
        for (const auto& dl : h.lines)
            if (dl.old_line >= 0) consumed_old++;
        old_idx = static_cast<std::size_t>((std::max)(0, hunk_old_begin)) +
            static_cast<std::size_t>((std::max)(0, consumed_old));
        hi++;
    }

    emit_context_until(static_cast<int>(diff_.old_lines.size()));

    std::string joined;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (i > 0) joined += '\n';
        joined += result[i];
    }
    return joined;
}

bool AidaCodeDocument::applyResolvedDiffToBuffer()
{
    const std::string text = composeResolvedText();
    if (text == serializedContent())
        return false;
    const int caret_l = (std::min)(selection_.caret_line,
        (std::max)(0, static_cast<int>(split_to_lines(text).size()) - 1));
    pushUndoRange(0, lineCount() - 1);
    rebuildBufferFromExternal(text);
    selection_.caret_line = selection_.anchor_line = clampLine(caret_l);
    selection_.caret_col  = selection_.anchor_col  = clampCol(selection_.caret_line, selection_.caret_col);
    selection_.active = false;
    return true;
}

void AidaCodeDocument::finalizeDiffIfResolved()
{
    if (!diff_.active) return;
    if (!diff_.fully_resolved()) return;
    if (diff_.document_id == 0 || diff_.document_id != document_id_ ||
        diff_.base_revision != revision_ ||
        diff_.base_content_hash != contentFingerprint()) {
        last_error_ = "code_editor: document changed after review was created";
        return;
    }
    const bool applied = applyResolvedDiffToBuffer();
    diff_ = code_editor_widget::pending_diff_t{};
    diff_.active = false;
    review_hunk_selection_ = {};
    diff_hover_hunk_ = -1;
    diff_scroll_target_ = -1.f;
    if (applied) {
        scroll_y_ = scroll_x_ = target_scroll_y_ = 0.f;
        breakUndoCoalescing();
    }
    Q_EMIT diffChanged();
}

bool AidaCodeDocument::commitResolvedDiff()
{
    std::lock_guard<std::mutex> lk(diff_mutex_);
    if (!diff_.active || !diff_.fully_resolved())
        return false;
    if (diff_.document_id == 0 || diff_.document_id != document_id_ ||
        diff_.base_revision != revision_ ||
        diff_.base_content_hash != contentFingerprint()) {
        last_error_ = "code_editor: document changed after review was created";
        return false;
    }
    finalizeDiffIfResolved();
    return !diff_.active;
}

void AidaCodeDocument::cancelAgentEdit()
{
    std::lock_guard<std::mutex> lk(diff_mutex_);
    diff_ = code_editor_widget::pending_diff_t{};
    diff_.active = false;
    review_hunk_selection_ = {};
    diff_hover_hunk_ = -1;
    Q_EMIT diffChanged();
}

void AidaCodeDocument::registerClick(qreal now_seconds)
{
    if (now_seconds - last_click_time_ < 0.3) click_count_++;
    else click_count_ = 1;
    last_click_time_ = now_seconds;
}

AidaCodeDocumentRegistry::AidaCodeDocumentRegistry(QObject* parent) : QObject(parent)
{
    sweep_timer_ = new QTimer(this);
    sweep_timer_->setInterval(250);
    connect(sweep_timer_, &QTimer::timeout, this,
            &AidaCodeDocumentRegistry::sweepDispatchFailures);
    sweep_timer_->start();
}

AidaCodeDocumentRegistry::~AidaCodeDocumentRegistry()
{
    reset();
}

AidaCodeDocumentRegistry& AidaCodeDocumentRegistry::instance()
{
    static AidaCodeDocumentRegistry* registry = new AidaCodeDocumentRegistry();
    return *registry;
}

void AidaCodeDocumentRegistry::wire(AidaCodeDocument* document)
{
    const quint64 id = document->documentId();
    connect(document, &AidaCodeDocument::contentChanged, this,
        [this, id](quint64 revision) { Q_EMIT contentChanged(id, revision); });
    connect(document, &AidaCodeDocument::metadataChanged, this,
        [this, id] { Q_EMIT metadataChanged(id); });
    connect(document, &AidaCodeDocument::foldsChanged, this,
        [this, id] { Q_EMIT foldsChanged(id); });
    connect(document, &AidaCodeDocument::findStateChanged, this,
        [this, id] { Q_EMIT findStateChanged(id); });
    connect(document, &AidaCodeDocument::diffChanged, this,
        [this, id] { Q_EMIT diffChanged(id); });
    connect(document, &AidaCodeDocument::streamStateChanged, this,
        [this, id] { Q_EMIT streamStateChanged(id); });
}

AidaCodeDocument& AidaCodeDocumentRegistry::ensure(quint64 document_id)
{
    auto found = documents_.find(document_id);
    if (found != documents_.end())
        return *found->second;
    auto* document = new AidaCodeDocument(document_id, this);
    wire(document);
    documents_.emplace(document_id, document);
    Q_EMIT documentAdded(document_id);
    return *document;
}

AidaCodeDocument* AidaCodeDocumentRegistry::find(quint64 document_id) const noexcept
{
    const auto found = documents_.find(document_id);
    return found == documents_.end() ? nullptr : found->second;
}

void AidaCodeDocumentRegistry::discard(quint64 document_id)
{
    const auto found = documents_.find(document_id);
    if (found == documents_.end()) return;
    Q_EMIT documentAboutToBeRemoved(document_id);
    AidaCodeDocument* document = found->second;
    document->cancelRuntimeJobs();
    documents_.erase(found);
    if (bound_document_id_ == document_id)
        bound_document_id_ = 0;
    if (focused_document_id_ == document_id)
        focused_document_id_ = 0;
    document->deleteLater();
    Q_EMIT documentRemoved(document_id);
}

void AidaCodeDocumentRegistry::reset()
{
    const auto ids = documents_;
    for (const auto& entry : ids) {
        Q_EMIT documentAboutToBeRemoved(entry.first);
        entry.second->cancelRuntimeJobs();
        entry.second->deleteLater();
    }
    documents_.clear();
    bound_document_id_ = 0;
    focused_document_id_ = 0;
}

quint64 AidaCodeDocumentRegistry::activeDocumentId() const noexcept
{
    return focused_document_id_ != 0 ? focused_document_id_ : bound_document_id_;
}

void AidaCodeDocumentRegistry::bindForActions(quint64 document_id)
{
    if (document_id == 0 || documents_.find(document_id) == documents_.end())
        return;
    bound_document_id_ = document_id;
}

bool AidaCodeDocumentRegistry::selectForActions(quint64 document_id)
{
    if (document_id == 0 || documents_.find(document_id) == documents_.end())
        return false;
    bound_document_id_ = document_id;
    focused_document_id_ = document_id;
    return true;
}

void AidaCodeDocumentRegistry::bindFocused()
{
    if (focused_document_id_ != 0 && documents_.find(focused_document_id_) != documents_.end())
        bound_document_id_ = focused_document_id_;
}

bool AidaCodeDocumentRegistry::loadDocument(quint64 document_id, quint64 revision,
    std::string_view content, std::string_view filename, std::string_view filepath, bool dirty,
    int caret_line, int caret_column, float scroll_x, float scroll_y, bool replace_existing,
    int selection_anchor_line, int selection_anchor_column, bool selection_active,
    const std::vector<int>& folded_lines, std::string_view language_override)
{
    if (document_id == 0 || revision == 0 ||
        content.size() > aida::editor::programming_documents::maximum_editable_document_bytes)
        return false;
    auto* found = find(document_id);
    if (found && !replace_existing) {
        bound_document_id_ = document_id;
        return true;
    }
    AidaCodeDocument& target = ensure(document_id);
    const bool loaded = target.load(revision, content, filename, filepath, dirty, caret_line,
        caret_column, scroll_x, scroll_y, replace_existing, selection_anchor_line,
        selection_anchor_column, selection_active, folded_lines, language_override);
    if (loaded)
        bound_document_id_ = document_id;
    return loaded;
}

bool AidaCodeDocumentRegistry::requestStreamedDocument(quint64 document_id, quint64 revision,
    std::string_view filename, std::string_view filepath, std::uint64_t byte_length)
{
    if (document_id == 0 || filepath.empty() || byte_length == 0)
        return false;
    AidaCodeDocument& target = ensure(document_id);
    return target.requestStreamed(revision, filename, filepath, byte_length);
}

code_editor_widget::document_metadata_snapshot_t AidaCodeDocumentRegistry::metadata(
    quint64 document_id)
{
    code_editor_widget::document_metadata_snapshot_t result;
    AidaCodeDocument* document = find(document_id);
    if (!document) return result;
    return document->metadataSnapshot();
}

code_editor_widget::document_payload_snapshot_t AidaCodeDocumentRegistry::payload(
    quint64 document_id, quint64 expected_revision)
{
    code_editor_widget::document_payload_snapshot_t result;
    AidaCodeDocument* document = find(document_id);
    if (!document) return result;
    return document->payloadSnapshot(expected_revision);
}

code_editor_widget::document_state_t AidaCodeDocumentRegistry::state(quint64 document_id)
{
    AidaCodeDocument* document = find(document_id);
    if (!document) return {};
    return document->stateSnapshot(focused_document_id_);
}

void AidaCodeDocumentRegistry::sweepDispatchFailures()
{
    for (const auto& entry : documents_)
        entry.second->observeDispatchFailures();
}

void AidaCodeDocumentRegistry::openFind(quint64 document_id)
{
    AidaCodeDocument* document = find(document_id);
    if (!document)
        return;
    document->find().visible = true;
    document->find().replace_mode = false;
    document->focusFindInput() = true;
    Q_EMIT findOverlayRequested(document_id, false);
}

void AidaCodeDocumentRegistry::openReplace(quint64 document_id)
{
    AidaCodeDocument* document = find(document_id);
    if (!document)
        return;
    document->find().visible = true;
    document->find().replace_mode = true;
    document->focusFindInput() = true;
    Q_EMIT findOverlayRequested(document_id, true);
}

void AidaCodeDocumentRegistry::openGotoLine(quint64 document_id)
{
    AidaCodeDocument* document = find(document_id);
    if (!document)
        return;
    document->goTo().visible = true;
    document->goTo().line_buf[0] = '\0';
    Q_EMIT gotoOverlayRequested(document_id);
}

}
