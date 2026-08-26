#include "test_all_ui.h"
#include "test_all_features.hpp"

#include "../../helpers/diag_log.hpp"
#include "../../helpers/globals.h"
#include "../editor/code_editor.hpp"
#include "../mcp/mcp_marketplace.hpp"
#include "../runtime/diagnostic_exception_scope.hpp"
#include "../settings/standalone_settings.hpp"
#include "../ui/ui_thread_dispatcher.hpp"

#include "test_lab.hpp"
#include "qt/test_lab_controller.hpp"
#include "qt/test_lab_widget.hpp"

#include "core/session/analysis_session.hpp"
#include "core/terminal/terminal_session.hpp"

#include "qt/ai/qt_command_palette.hpp"
#include "qt/documents/aida_document_controller.hpp"
#include "qt/documents/aida_document_model.hpp"
#include "qt/editor/aida_code_document.hpp"
#include "qt/editor/aida_code_editor.hpp"
#include "qt/editor/aida_image_view.hpp"
#include "qt/explorer/aida_explorer_model.hpp"
#include "qt/explorer/aida_explorer_view.hpp"
#include "qt/explorer/exchange/open_dispatch.hpp"
#include "qt/programming/aida_output_pane.hpp"
#include "qt/programming/aida_terminal_view.hpp"
#include "qt/qt_center_pages.hpp"
#include "qt/registry/qt_view_registry.hpp"
#include "qt/registry/view_catalog.hpp"
#include "qt/settings/qt_settings_mcp_page.hpp"
#include "qt/workbench/qt_recent_view.hpp"
#include "qt/workbench/qt_sessions_model.hpp"

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTabBar>
#include <QTableView>

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace test_all_features {

namespace {

static void format_timestamp(char* out, std::size_t cap) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
        static_cast<unsigned>(st.wYear),
        static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay),
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond),
        static_cast<unsigned>(st.wMilliseconds));
}

static void write_log_file(HANDLE hf, const std::string& line) {
    test_all_features::write_full_test_log_line(hf, line.data(), line.size());
}

static void log_msg(HANDLE hf, const char* tag, const char* fmt, ...) {
    char ts[40];
    format_timestamp(ts, sizeof(ts));

    char detail[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(detail, sizeof(detail), _TRUNCATE, fmt, ap);
    va_end(ap);

    char line[1200];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[%s] [%s] %s\n", ts, tag, detail);
    std::string s(line);
    write_log_file(hf, s);
    test_all_features::mirror_full_test_log_line(tag, detail, s.c_str());
}

struct check_accum_t {
    bool ok = true;
    int checked = 0;
    std::string failures;

    void require(bool cond, const char* label) {
        ++checked;
        if (cond) return;
        ok = false;
        if (failures.size() < 640) {
            if (!failures.empty()) failures += "; ";
            failures += label;
        }
    }
};

struct ui_phase_job_t {
    std::uint64_t id = 0;
    HANDLE hf = INVALID_HANDLE_VALUE;
    std::atomic<int>* passed = nullptr;
    std::atomic<int>* failed = nullptr;
    std::atomic<int>* skipped = nullptr;
    bool(*cancelled)() = nullptr;
    DWORD worker_tid = 0;
    DWORD ui_tid = 0;
    std::uint64_t queued_ms = 0;
    std::uint64_t started_ms = 0;
    std::uint64_t finished_ms = 0;
    std::size_t next_step = 0;
    std::uint64_t processed_steps = 0;
    bool started = false;
    bool dispatch_cancelled = false;
    bool done = false;
};

std::mutex g_ui_phase_mtx;
std::condition_variable g_ui_phase_cv;
std::atomic<std::size_t> g_ui_phase_pending_jobs{0};
std::atomic<DWORD> g_ui_phase_thread_id{0};
std::atomic<std::uint64_t> g_ui_phase_next_job_id{0};
std::atomic<std::uint64_t> g_ui_phase_active_job_id{0};
std::atomic<DWORD> g_ui_phase_active_worker_tid{0};
std::atomic<int> g_ui_phase_active_step_index{-1};
std::atomic<const char*> g_ui_phase_active_step_name{"<idle>"};
std::atomic<std::uint64_t> g_ui_phase_last_lock_wait_ms{0};
std::atomic<std::uint64_t> g_ui_phase_last_job_run_ms{0};
std::atomic<std::uint64_t> g_ui_phase_last_job_wait_ms{0};
std::atomic<std::uint64_t> g_ui_phase_last_pump_wall_ms{0};
std::atomic<std::uint64_t> g_ui_phase_last_pump_seq{0};
std::atomic<std::uint64_t> g_ui_phase_skipped_by_budget_count{0};
std::atomic<std::uint64_t> g_ui_phase_skipped_no_job_count{0};
std::atomic<std::uint64_t> g_ui_phase_lock_busy_count{0};
std::atomic<std::uint64_t> g_ui_phase_steps_processed_total{0};
std::atomic<std::size_t> g_ui_phase_last_pending_count{0};

static std::uint64_t ui_now_ms() {
    return static_cast<std::uint64_t>(GetTickCount64());
}

namespace qt_reg = aida::qt::registry;
namespace qt_docs = aida::qt::documents;
namespace qt_editor = aida::qt::editor;
namespace qt_explorer = aida::qt::explorer;
namespace qt_programming = aida::qt::programming;
namespace qt_settings = aida::qt::settings;
namespace qt_workbench = aida::qt::workbench;
namespace qt_ai = aida::qt::ai;
namespace qt_testlab = aida::qt::testlab;

template <typename SpyT, typename PredT>
static bool spy_pump_until(SpyT& spy, PredT&& pred, std::uint64_t deadline_ms) {
    while (!pred()) {
        if (ui_now_ms() >= deadline_ms)
            return false;
        (void)spy.wait(250);
    }
    return true;
}

struct temp_workspace_t {
    std::filesystem::path root;

    temp_workspace_t() {
        wchar_t tmp[MAX_PATH] = {};
        constexpr DWORD tmp_cap = static_cast<DWORD>(sizeof(tmp) / sizeof(tmp[0]));
        DWORD len = GetTempPathW(tmp_cap, tmp);
        std::filesystem::path base = (len > 0 && len < tmp_cap) ? std::filesystem::path(tmp) : std::filesystem::temp_directory_path();
        root = base / ("aida_ui_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
    }

    ~temp_workspace_t() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path write_file(const char* name, const char* content) const {
        std::filesystem::path p = root / name;
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        ofs.write(content, static_cast<std::streamsize>(std::strlen(content)));
        ofs.close();
        return p;
    }
};

static void pass(HANDLE hf, std::atomic<int>& passed, const char* tag, const char* fmt, ...) {
    char detail[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(detail, sizeof(detail), _TRUNCATE, fmt, ap);
    va_end(ap);
    log_msg(hf, tag, "PASS -- %s", detail);
    passed.fetch_add(1);
}

static void fail(HANDLE hf, std::atomic<int>& failed, const char* tag, const char* fmt, ...) {
    char detail[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(detail, sizeof(detail), _TRUNCATE, fmt, ap);
    va_end(ap);
    log_msg(hf, tag, "FAIL -- %s", detail);
    failed.fetch_add(1);
}

static void skip(HANDLE hf, std::atomic<int>& failed, const char* tag, const char* fmt, ...) {
    char detail[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(detail, sizeof(detail), _TRUNCATE, fmt, ap);
    va_end(ap);
    log_msg(hf, tag, "SKIP -- %s", detail);
    failed.fetch_add(1);
}

static std::string row_text(const aida::terminal::TerminalSession& session, std::size_t row_idx) {
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(session.buffer_mtx));
    if (row_idx >= session.lines.size()) return {};
    std::string out;
    for (const auto& c : session.lines[row_idx]) {
        if (c.ch == '\0') break;
        out.push_back(c.ch);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

static bool wait_document_idle(qt_docs::AidaDocumentModel& model, quint64 document_id, std::uint64_t timeout_ms) {
    QSignalSpy changed_spy(&model, &qt_docs::AidaDocumentModel::documentChanged);
    const std::uint64_t deadline = ui_now_ms() + timeout_ms;
    for (;;) {
        const int idx = model.findDocument(document_id);
        if (idx < 0)
            return true;
        const OpenTab* tab = model.recordAt(idx);
        if (!tab)
            return false;
        if (!tab->save_in_progress && !tab->recovery_operation_pending && !tab->recovery_checkpoint_pending)
            return true;
        if (ui_now_ms() >= deadline)
            return false;
        (void)changed_spy.wait(250);
    }
}

static bool close_document_when_idle(qt_docs::AidaDocumentModel& model, qt_docs::AidaDocumentController& controller, quint64 document_id) {
    if (document_id == 0)
        return false;
    if (model.findDocument(document_id) < 0)
        return true;
    if (!wait_document_idle(model, document_id, 10000))
        return false;
    controller.closeDocument(document_id, true);
    return model.findDocument(document_id) < 0;
}

static void test_center_view_file_open(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed) {
    auto t0 = std::chrono::steady_clock::now();
    temp_workspace_t tmp;
    auto p = tmp.write_file("ui_center.cpp", "int ui_center = 7;\r\n");

    check_accum_t ck;
    static const char* const k_expected_center_ids[] = {
        "document.code", "document.disassembly", "document.hex", "document.pseudocode",
        "document.graph", "document.image", "document.diff", "view.start_center",
        "view.analysis.binary_map", "view.types.struct_recon", "view.test_lab"
    };
    for (const char* id : k_expected_center_ids)
        ck.require(qt_reg::find_catalog_entry(id) != nullptr, "catalog center page present");

    bool test_lab_center_listed = false;
    for (const char* page : aida::qt::k_center_pages)
        if (std::string_view(page) == "test_lab") test_lab_center_listed = true;
    ck.require(test_lab_center_listed, "k_center_pages lists test_lab");

    std::size_t superset_checked = 0;
    bool superset_ok = true;
    std::string superset_missing;
    for (const auto& entry : qt_reg::k_catalog) {
        if (entry.role != qt_reg::view_presentation_role_t::document)
            continue;
        ++superset_checked;
        bool listed = false;
        for (const char* page : aida::qt::k_center_pages) {
            if (std::string_view(page) == entry.id) {
                listed = true;
                break;
            }
        }
        if (!listed) {
            superset_ok = false;
            superset_missing = entry.id;
        }
    }
    ck.require(superset_checked > 0, "catalog document views enumerated");
    ck.require(superset_ok, "k_center_pages covers every catalog document view");

    qt_docs::AidaDocumentModel model;
    qt_docs::AidaDocumentController controller(&model);
    const int baseline_count = model.recordCount();

    auto& dispatch = qt_explorer::AidaOpenDispatch::instance();
    qt_docs::AidaDocumentController* previous_controller = dispatch.documentController();
    const std::string recent_before = g_sa_settings.recent_workspaces_json;

    QSignalSpy added_spy(&model, &qt_docs::AidaDocumentModel::documentAdded);
    ck.require(added_spy.isValid(), "documentAdded spy bound");

    dispatch.setDocumentController(&controller);
    dispatch.openPath(p.string());
    dispatch.setDocumentController(previous_controller);

    const int after_count = model.recordCount();
    const quint64 new_document = model.activeDocumentId();
    const int found_index = model.findPathDocument(p.string());
    ck.require(after_count == baseline_count + 1, "open dispatch appended a document tab");
    ck.require(new_document != 0, "opened document became active");
    ck.require(found_index >= 0, "opened document path registered in the model");
    ck.require(added_spy.count() >= 1, "documentAdded signal observed");

    const OpenTab* record = found_index >= 0 ? model.recordAt(found_index) : nullptr;
    ck.require(record != nullptr && record->filepath == p.string(), "document record path matches");

    const bool closed_ok = close_document_when_idle(model, controller, new_document);
    ck.require(closed_ok && model.recordCount() == baseline_count, "document tab closed back to baseline");
    g_sa_settings.recent_workspaces_json = recent_before;

    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (ck.ok) {
        pass(hf, passed, "ui_center", "Qt view catalog/center-page table coherent and open dispatch routed a temp text file into the Qt document controller (%d checks, %zu superset views, elapsed_us=%lld)",
            ck.checked,
            superset_checked,
            us);
    } else {
        fail(hf, failed, "ui_center", "checked=%d failures=%s superset_missing=%s baseline=%d after=%d new_doc=%llu found_index=%d elapsed_us=%lld",
            ck.checked,
            ck.failures.c_str(),
            superset_missing.c_str(),
            baseline_count,
            after_count,
            static_cast<unsigned long long>(new_document),
            found_index,
            us);
    }
}

static void test_file_browser_directory_and_routes(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed) {
    auto t0 = std::chrono::steady_clock::now();
    temp_workspace_t tmp;
    std::error_code ec;
    std::filesystem::create_directories(tmp.root / "subdir", ec);
    auto text = tmp.write_file("subdir/route.cpp", "int route_value = 11;\r\n");
    tmp.write_file("payload.bin", "not-a-pe-binary");

    qt_explorer::AidaExplorerView view;
    qt_explorer::AidaExplorerModel* model = view.model();
    if (!model) {
        fail(hf, failed, "ui_file_browser", "explorer view exposes no model");
        return;
    }
    const QString previous_root = model->currentRoot();
    QSignalSpy indexing_spy(model, &qt_explorer::AidaExplorerModel::indexingStateChanged);
    QSignalSpy rows_spy(model, &qt_explorer::AidaExplorerModel::rowsInserted);

    model->refresh(tmp.root.string());
    const bool settled = spy_pump_until(indexing_spy, [&] { return !model->indexing(); }, ui_now_ms() + 10000);

    const bool root_ok = qt_explorer::explorer_path_key(model->currentRoot().toStdString())
        == qt_explorer::explorer_path_key(tmp.root.string());

    int dir_row = -1;
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex idx = model->index(row);
        if (model->data(idx, qt_explorer::AidaExplorerModel::IsDirRole).toBool() &&
            model->data(idx, qt_explorer::AidaExplorerModel::NameRole).toString() == QStringLiteral("subdir"))
            dir_row = row;
    }
    if (dir_row >= 0)
        model->toggleDir(dir_row);

    const QString child_name = QString::fromStdString(text.filename().string());
    bool child_seen = false;
    if (dir_row >= 0) {
        child_seen = spy_pump_until(indexing_spy, [&] {
            for (int row = 0; row < model->rowCount(); ++row) {
                const QModelIndex idx = model->index(row);
                if (!model->data(idx, qt_explorer::AidaExplorerModel::IsDirRole).toBool() &&
                    model->data(idx, qt_explorer::AidaExplorerModel::NameRole).toString() == child_name)
                    return true;
            }
            return false;
        }, ui_now_ms() + 10000);
    }

    const int full_rows = model->rowCount();
    model->setFilter(QStringLiteral("route"));
    const int filtered_rows = model->rowCount();
    const bool filter_ok = filtered_rows >= 1 && filtered_rows < full_rows;
    model->setFilter(QString());
    const bool filter_restored = model->rowCount() == full_rows;

    const bool classifier_ok =
        qt_editor::AidaImageView::isImageAdmission(QStringLiteral("png")) &&
        qt_editor::AidaImageView::isImageAdmission(QStringLiteral("jpg")) &&
        qt_editor::AidaImageView::isImageAdmission(QStringLiteral("ppm")) &&
        !qt_editor::AidaImageView::isImageAdmission(QStringLiteral("exe")) &&
        !qt_editor::AidaImageView::isImageAdmission(QStringLiteral("bin"));

    const bool restore_requested = !previous_root.isEmpty();
    if (restore_requested)
        model->refresh(previous_root.toStdString());
    else if (model->indexing())
        model->cancelRefresh();

    const bool ok = settled && root_ok && dir_row >= 0 && child_seen && filter_ok && filter_restored && classifier_ok;
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    log_msg(hf, "ui_file_browser", "STATE -- settled=%d root_ok=%d dir_row=%d child_seen=%d filter_ok=%d filter_restored=%d classifier_ok=%d full_rows=%d filtered_rows=%d indexing_signals=%d row_insert_signals=%d restore_requested=%d elapsed_us=%lld",
        settled ? 1 : 0,
        root_ok ? 1 : 0,
        dir_row,
        child_seen ? 1 : 0,
        filter_ok ? 1 : 0,
        filter_restored ? 1 : 0,
        classifier_ok ? 1 : 0,
        full_rows,
        filtered_rows,
        static_cast<int>(indexing_spy.count()),
        static_cast<int>(rows_spy.count()),
        restore_requested ? 1 : 0,
        us);
    if (ok) {
        pass(hf, passed, "ui_file_browser", "Qt explorer model opened a temp workspace root, expanded a directory, surfaced the child file, filtered rows, and the image admission classifier held (elapsed_us=%lld)", us);
    } else {
        fail(hf, failed, "ui_file_browser", "settled=%d root_ok=%d dir_row=%d child_seen=%d filter_ok=%d filter_restored=%d classifier_ok=%d full_rows=%d filtered_rows=%d index_error=%s elapsed_us=%lld",
            settled ? 1 : 0,
            root_ok ? 1 : 0,
            dir_row,
            child_seen ? 1 : 0,
            filter_ok ? 1 : 0,
            filter_restored ? 1 : 0,
            classifier_ok ? 1 : 0,
            full_rows,
            filtered_rows,
            model->indexError().toStdString().c_str(),
            us);
    }
}

static void test_file_tab_lifecycle(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed) {
    auto t0 = std::chrono::steady_clock::now();
    temp_workspace_t tmp;
    auto a = tmp.write_file("ui_tab_a.txt", "alpha\r\n");
    auto b = tmp.write_file("ui_tab_b.txt", "beta\r\n");

    qt_docs::AidaDocumentModel model;
    qt_docs::AidaDocumentController controller(&model);
    const int baseline = model.recordCount();
    const quint64 baseline_active = model.activeDocumentId();

    QSignalSpy added_spy(&model, &qt_docs::AidaDocumentModel::documentAdded);
    QSignalSpy removed_spy(&model, &qt_docs::AidaDocumentModel::documentRemoved);
    QSignalSpy about_spy(&model, &qt_docs::AidaDocumentModel::documentAboutToBeRemoved);
    QSignalSpy active_spy(&model, &qt_docs::AidaDocumentModel::activeDocumentChanged);
    const bool spies_valid = added_spy.isValid() && removed_spy.isValid() && about_spy.isValid() && active_spy.isValid();

    controller.openOrFocusContent(a.string(), "ui_tab_a.txt", "alpha\r\n");
    const quint64 id_a = model.activeDocumentId();
    const bool opened_a = id_a != 0 && model.recordCount() == baseline + 1 && model.findPathDocument(a.string()) >= 0;

    controller.openOrFocusContent(b.string(), "ui_tab_b.txt", "beta\r\n");
    const quint64 id_b = model.activeDocumentId();
    const bool opened_b = id_b != 0 && id_b != id_a && model.recordCount() == baseline + 2
        && model.findPathDocument(b.string()) >= 0;

    const bool switched = id_a != 0 && controller.switchTo(id_a) && model.activeDocumentId() == id_a;

    const bool closed_a = close_document_when_idle(model, controller, id_a)
        && model.recordCount() == baseline + 1
        && model.findPathDocument(a.string()) < 0
        && model.activeDocumentId() != id_a;
    const bool cleared = close_document_when_idle(model, controller, id_b)
        && model.recordCount() == baseline
        && model.findPathDocument(b.string()) < 0;

    if (baseline_active != 0 && model.findDocument(baseline_active) >= 0)
        (void)controller.switchTo(baseline_active);

    const bool signals_ok = spies_valid && added_spy.count() >= 2 && removed_spy.count() >= 2
        && about_spy.count() >= 2 && active_spy.count() >= 3;

    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (opened_a && opened_b && switched && closed_a && cleared && signals_ok) {
        pass(hf, passed, "ui_tabs", "Qt document controller open/focus/switch/close lifecycle kept the model coherent with Qt signals (added=%d removed=%d active=%d, elapsed_us=%lld)",
            static_cast<int>(added_spy.count()),
            static_cast<int>(removed_spy.count()),
            static_cast<int>(active_spy.count()),
            us);
    } else {
        fail(hf, failed, "ui_tabs", "opened_a=%d opened_b=%d switched=%d closed_a=%d cleared=%d signals_ok=%d added=%d removed=%d about=%d active=%d rows=%d baseline=%d elapsed_us=%lld",
            opened_a ? 1 : 0,
            opened_b ? 1 : 0,
            switched ? 1 : 0,
            closed_a ? 1 : 0,
            cleared ? 1 : 0,
            signals_ok ? 1 : 0,
            static_cast<int>(added_spy.count()),
            static_cast<int>(removed_spy.count()),
            static_cast<int>(about_spy.count()),
            static_cast<int>(active_spy.count()),
            model.recordCount(),
            baseline,
            us);
    }
}

static void test_code_editor_save_find_and_diff(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed) {
    auto t0 = std::chrono::steady_clock::now();
    temp_workspace_t tmp;
    auto p = tmp.write_file("editor_workflow.cpp", "alpha\r\nbeta\r\ngamma\r\n");

    qt_docs::AidaDocumentModel model;
    qt_docs::AidaDocumentController controller(&model);
    const int baseline_count = model.recordCount();
    const quint64 previous_actions_document = code_editor_widget::active_document_id();

    check_accum_t ck;
    controller.openOrFocusContent(p.string(), "editor_workflow.cpp", "alpha\r\nbeta\r\ngamma\r\n");
    const quint64 doc_id = model.activeDocumentId();
    ck.require(doc_id != 0 && model.recordCount() == baseline_count + 1, "document opened through the Qt controller");

    auto& registry = qt_editor::AidaCodeDocumentRegistry::instance();
    qt_editor::AidaCodeDocument* document = registry.find(doc_id);
    ck.require(document != nullptr, "document present in the code document registry");
    ck.require(code_editor_widget::select_document_for_actions(doc_id), "document selected for actions");
    ck.require(code_editor_widget::active_document_id() == doc_id, "active document routed through code_editor_widget");

    QPointer<qt_editor::AidaCodeDocument> document_guard(document);

    std::unique_ptr<qt_editor::AidaCodeEditor> editor;
    if (document)
        editor = std::make_unique<qt_editor::AidaCodeEditor>(&registry, doc_id, nullptr);
    ck.require(editor != nullptr, "off-screen code editor constructed");

    int content_signals = 0;
    bool typing_ok = false;
    if (editor && document) {
        QSignalSpy content_spy(&registry, &qt_editor::AidaCodeDocumentRegistry::contentChanged);
        document->setCaret(1, 4);
        QTest::keyClicks(editor.get(), QStringLiteral(" edited"));
        content_signals = content_spy.count();
        const std::string content = code_editor_widget::document_content(doc_id);
        typing_ok = content_spy.isValid() && content_signals >= 1
            && content.find("beta edited") != std::string::npos
            && code_editor_widget::document_dirty(doc_id);
    }
    ck.require(typing_ok, "QTest keystrokes reached the document buffer");

    int find_matches = -1;
    bool find_signal_ok = false;
    if (document) {
        auto& find_state = document->find();
        find_state.visible = true;
        find_state.replace_mode = false;
        find_state.case_sensitive = true;
        find_state.whole_word = false;
        find_state.use_regex = false;
        _snprintf_s(find_state.find_buf, sizeof(find_state.find_buf), _TRUNCATE, "%s", "beta");
        QSignalSpy find_spy(document, &qt_editor::AidaCodeDocument::findStateChanged);
        document->findAllMatches();
        find_matches = document->find().total_matches;
        find_signal_ok = find_spy.isValid() && find_spy.count() >= 1;
    }
    ck.require(find_matches == 1 && find_signal_ok, "document find computed the single beta match");

    bool save_gate_ok = false;
    bool saved_clean = false;
    bool saved_content_ok = false;
    std::string save_detail;
    if (doc_id != 0 && typing_ok) {
        const bool idle_before_save = wait_document_idle(model, doc_id, 10000);
        QSignalSpy saved_spy(&registry, &qt_editor::AidaCodeDocumentRegistry::metadataChanged);
        const auto save_result = controller.saveDocument(doc_id);
        save_gate_ok = idle_before_save && save_result.succeeded;
        save_detail = save_result.detail;
        if (save_gate_ok) {
            saved_clean = spy_pump_until(saved_spy,
                [&] { return !code_editor_widget::document_dirty(doc_id); }, ui_now_ms() + 10000);
            std::ifstream saved_in(p, std::ios::binary);
            const std::string saved((std::istreambuf_iterator<char>(saved_in)), std::istreambuf_iterator<char>());
            saved_content_ok = saved.find("beta edited") != std::string::npos;
        } else if (!idle_before_save) {
            save_detail = "document stayed busy before save";
        }
    }
    ck.require(save_gate_ok, "save gate accepted the document");
    ck.require(saved_clean, "save completion cleared the dirty state");
    ck.require(saved_content_ok, "saved file contains the typed edit");

    bool diff_begin = false, diff_propose = false, diff_pending = false;
    bool diff_accept = false, diff_resolved = false, diff_cancelled = false;
    int hunk_count = 0;
    if (doc_id != 0) {
        code_editor_widget::cancel_agent_edit();
        diff_begin = code_editor_widget::begin_agent_edit("ui-test");
        diff_propose = diff_begin && code_editor_widget::propose_replace_range(1, 2, "beta accepted");
        hunk_count = code_editor_widget::pending_hunk_count();
        diff_pending = code_editor_widget::has_pending_diff() && hunk_count > 0;
        diff_accept = diff_pending && code_editor_widget::accept_hunk(0);
        if (diff_accept) {
            const auto& diff = code_editor_widget::pending_diff();
            diff_resolved = diff.active && !diff.hunks.empty()
                && diff.hunks[0].state == code_editor_widget::diff_hunk_state_t::accepted;
        }
        code_editor_widget::cancel_agent_edit();
        diff_cancelled = !code_editor_widget::has_pending_diff();
    }
    ck.require(diff_begin && diff_propose && diff_pending && diff_accept && diff_resolved && diff_cancelled,
        "agent diff propose/accept/cancel roundtrip");

    editor.reset();

    bool document_reaped = true;
    if (document != nullptr) {
        document_reaped = false;
        if (close_document_when_idle(model, controller, doc_id)) {
            if (document_guard.isNull()) {
                document_reaped = true;
            } else {
                QSignalSpy destroyed_spy(document_guard.data(), &QObject::destroyed);
                document_reaped = spy_pump_until(destroyed_spy, [&] { return document_guard.isNull(); }, ui_now_ms() + 5000);
            }
        }
    }
    ck.require(document_reaped, "closed document object deferred deletion observed");
    ck.require(model.recordCount() == baseline_count, "document tab closed back to baseline");

    if (previous_actions_document != 0 && registry.find(previous_actions_document) != nullptr)
        (void)code_editor_widget::select_document_for_actions(previous_actions_document);

    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (ck.ok) {
        pass(hf, passed, "ui_editor", "off-screen Qt editor keystrokes, document find, async save-to-disk, and agent diff hunk accept/cancel all completed (content_signals=%d hunks=%d elapsed_us=%lld)",
            content_signals,
            hunk_count,
            us);
    } else {
        fail(hf, failed, "ui_editor", "checked=%d failures=%s save_detail=%s last_error=%s content_signals=%d find_matches=%d hunks=%d elapsed_us=%lld",
            ck.checked,
            ck.failures.c_str(),
            save_detail.c_str(),
            code_editor_widget::last_error().c_str(),
            content_signals,
            find_matches,
            hunk_count,
            us);
    }
}

static void test_activity_search_recent(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed) {
    auto t0 = std::chrono::steady_clock::now();
    temp_workspace_t tmp;
    auto p = tmp.write_file("recent_target.exe", "MZ");

    const std::string recent_before = g_sa_settings.recent_workspaces_json;
    nlohmann::json recent_json = nlohmann::json::array();
    recent_json.push_back(p.string());
    g_sa_settings.recent_workspaces_json = recent_json.dump();

    check_accum_t ck;
    qt_workbench::QtRecentView recent_view(nullptr);
    QTableView* recent_table = recent_view.findChild<QTableView*>();
    ck.require(recent_table != nullptr && recent_table->model() != nullptr, "recent view exposes its table model");
    int recent_rows = 0;
    bool recent_path_seen = false;
    if (recent_table && recent_table->model()) {
        recent_rows = recent_table->model()->rowCount();
        const QString wanted = QString::fromStdString(p.string());
        for (int row = 0; row < recent_rows; ++row) {
            const QVariant value = recent_table->model()->data(recent_table->model()->index(row, 1));
            if (value.toString() == wanted) {
                recent_path_seen = true;
                break;
            }
        }
    }
    ck.require(recent_rows >= 1 && recent_path_seen, "recent view reflects recent_workspaces_json");
    g_sa_settings.recent_workspaces_json = recent_before;

    qt_workbench::QtSessionsModel sessions_model;
    QSignalSpy sessions_reset_spy(&sessions_model, &QAbstractItemModel::modelReset);
    analysis_session::session_summary_t summary;
    summary.id = "uitest-session";
    summary.path = p.string();
    summary.filename = "recent_target.exe";
    summary.pid = 1234;
    summary.process_name = "recent_target.exe";
    summary.is_alive = true;
    summary.load_state = analysis_session::session_load_state_t::ready;
    sessions_model.setRows({ summary }, 0, 0);
    const bool sessions_ok = sessions_model.rowCount() == 1
        && sessions_model.columnCount() == static_cast<int>(qt_workbench::QtSessionsModel::Column::column_count)
        && sessions_model.rowAt(0) != nullptr
        && sessions_model.rowAt(0)->filename == "recent_target.exe"
        && sessions_model.sessionIndexFor(0) == 0
        && !sessions_model.data(sessions_model.index(0, 0)).toString().isEmpty();
    ck.require(sessions_ok, "sessions model exposes the injected session row");
    ck.require(sessions_reset_spy.isValid() && sessions_reset_spy.count() >= 1, "sessions model reset signal observed");

    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (ck.ok) {
        pass(hf, passed, "ui_activity", "Qt recent view reflects recent_workspaces_json and the sessions model accepts injected rows (recent_rows=%d elapsed_us=%lld)",
            recent_rows,
            us);
    } else {
        fail(hf, failed, "ui_activity", "checked=%d failures=%s recent_rows=%d recent_json=%s elapsed_us=%lld",
            ck.checked,
            ck.failures.c_str(),
            recent_rows,
            g_sa_settings.recent_workspaces_json.c_str(),
            us);
    }
}

static void test_command_palette_and_center_views(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed) {
    auto t0 = std::chrono::steady_clock::now();
    check_accum_t ck;

    const auto commands = aida::commands::list();
    ck.require(!commands.empty(), "command registry populated");

    qt_ai::AidaCommandModel command_model;
    const int total_rows = command_model.rowCount();
    command_model.setQuery(QStringLiteral("network"));
    const int filtered_rows = command_model.rowCount();
    const aida::commands::command_t* first_command =
        command_model.commandAt(command_model.firstCommandRow());
    ck.require(total_rows > 0, "command model lists registered commands");
    ck.require(filtered_rows > 0 && filtered_rows <= total_rows, "command model fuzzy filter narrows rows");
    ck.require(first_command != nullptr && !first_command->name.empty(), "first command row resolves");

    qt_ai::AidaCommandPaletteDialog dialog(nullptr);
    QLineEdit* palette_input = dialog.findChild<QLineEdit*>();
    ck.require(palette_input != nullptr, "command palette exposes its input line edit");
    int palette_text_signals = 0;
    bool palette_typing_ok = false;
    if (palette_input) {
        QSignalSpy text_spy(palette_input, &QLineEdit::textChanged);
        QTest::keyClicks(palette_input, QStringLiteral("network"));
        palette_text_signals = text_spy.count();
        palette_typing_ok = palette_input->text() == QStringLiteral("network") && palette_text_signals >= 7;
    }
    ck.require(palette_typing_ok, "QTest keystrokes drive the palette query");

    qt_reg::qt_view_registry_t registry;
    const std::size_t registered = registry.register_catalog(qt_reg::qt_view_factory_t{});
    ck.require(registered == qt_reg::k_catalog_size, "registry registers the full catalog");
    ck.require(registry.descriptor_count() == qt_reg::k_catalog_size, "registry descriptor count matches catalog");
    ck.require(registry.find_descriptor(qt_reg::stable_view_id_t{ std::string("view.test_lab") }) != nullptr,
        "test_lab descriptor present");
    ck.require(registry.find_descriptor(qt_reg::stable_view_id_t{ std::string("document.code") }) != nullptr,
        "code document descriptor present");

    ck.require(qt_reg::hub_member_count(qt_reg::hub_kind_t::analysis) == 5, "analysis hub subview count");
    ck.require(qt_reg::hub_member_count(qt_reg::hub_kind_t::scan) == 7, "scan hub subview count");
    ck.require(qt_reg::hub_member_count(qt_reg::hub_kind_t::types) == 7, "types hub subview count");
    ck.require(qt_reg::hub_member_count(qt_reg::hub_kind_t::debugger) == 15, "debugger hub subview count");
    ck.require(qt_reg::hub_member_count(qt_reg::hub_kind_t::network) == 36, "network hub subview count");

    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    log_msg(hf, "ui_commands_routes", "STATE -- ck_checks=%d ck_ok=%d commands=%zu total_rows=%d filtered_rows=%d palette_text_signals=%d catalog=%zu elapsed_us=%lld failures=\"%s\"",
        ck.checked,
        ck.ok ? 1 : 0,
        commands.size(),
        total_rows,
        filtered_rows,
        palette_text_signals,
        registered,
        us,
        ck.failures.c_str());
    if (ck.ok) {
        pass(hf, passed, "ui_palette_views", "Qt command palette model/dialog and the Qt view registry catalog passed (%d checks, %zu catalog views, %zu registered commands, elapsed_us=%lld)",
            ck.checked,
            registered,
            commands.size(),
            us);
    } else {
        fail(hf, failed, "ui_palette_views", "checks_ok=%d checked=%d failures=%s commands=%zu total_rows=%d filtered_rows=%d elapsed_us=%lld",
            ck.ok ? 1 : 0,
            ck.checked,
            ck.failures.c_str(),
            commands.size(),
            total_rows,
            filtered_rows,
            us);
    }
}

static void test_bottom_log_tabs(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed) {
    auto t0 = std::chrono::steady_clock::now();
    check_accum_t ck;

    qt_programming::AidaOutputLogChannel channel(bottom_tab_t::output);
    QSignalSpy reset_spy(&channel, &qt_programming::AidaOutputLogChannel::resetAll);
    QSignalSpy content_spy(&channel, &qt_programming::AidaOutputLogChannel::contentStateChanged);
    ck.require(reset_spy.isValid() && content_spy.isValid(), "output channel spies bound");
    bool snapshot_ok = false;
    for (int attempt = 0; attempt < 3 && !snapshot_ok; ++attempt) {
        channel.tick();
        snapshot_ok = channel.snapshotAvailable();
    }
    const std::size_t total_lines = channel.totalLines();
    ck.require(snapshot_ok, "output channel snapshot acquired");

    channel.setFilterText(QStringLiteral("aida-ui-bottom-probe"));
    const bool filter_set = channel.filterText() == QStringLiteral("aida-ui-bottom-probe");
    channel.setChannelFilter("uitest-channel");
    const bool channel_filter_set = channel.channelFilter() == "uitest-channel";
    channel.setFilterText(QString());
    channel.setChannelFilter(std::string());
    const bool filters_cleared = channel.filterText().isEmpty() && channel.channelFilter().empty();
    ck.require(filter_set && channel_filter_set && filters_cleared, "channel filter state roundtrip");
    ck.require(reset_spy.count() >= 4, "refilter emitted resetAll per change");

    QPointer<qt_programming::AidaOutputPane> pane(new qt_programming::AidaOutputPane(bottom_tab_t::mcp_log, QStringLiteral("uitest"), nullptr));
    ck.require(!pane.isNull(), "output pane constructed off-screen");

    const bool controller_ok = qt_programming::AidaOutputController::exists()
        && qt_programming::AidaOutputController::instance().channel(bottom_tab_t::output) != nullptr
        && qt_programming::AidaOutputController::instance().channel(bottom_tab_t::mcp_log) != nullptr
        && qt_programming::AidaOutputController::instance().channel(bottom_tab_t::terminal) == nullptr
        && qt_programming::AidaOutputController::instance().supports_filter(bottom_tab_t::output)
        && !qt_programming::AidaOutputController::instance().supports_filter(bottom_tab_t::terminal);
    ck.require(controller_ok, "output controller channels exist and the terminal tab stays out of the log panes");

    bool pane_reaped = false;
    int pane_destroyed_signals = 0;
    if (!pane.isNull()) {
        QSignalSpy destroyed_spy(pane.data(), &QObject::destroyed);
        pane->deleteLater();
        pane_reaped = spy_pump_until(destroyed_spy, [&] { return pane.isNull(); }, ui_now_ms() + 5000);
        pane_destroyed_signals = destroyed_spy.count();
    }
    ck.require(pane_reaped, "output pane deferred deletion observed");

    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    log_msg(hf, "ui_bottom", "STATE -- snapshot_ok=%d total_lines=%zu reset_signals=%d content_signals=%d controller_ok=%d pane_reaped=%d elapsed_us=%lld",
        snapshot_ok ? 1 : 0,
        total_lines,
        static_cast<int>(reset_spy.count()),
        static_cast<int>(content_spy.count()),
        controller_ok ? 1 : 0,
        pane_reaped ? 1 : 0,
        us);
    if (ck.ok) {
        pass(hf, passed, "ui_bottom", "Qt output log channel tick/filter/reset signals, controller channel inventory (terminal excluded), and pane deferred deletion passed (elapsed_us=%lld)", us);
    } else {
        fail(hf, failed, "ui_bottom", "checked=%d failures=%s total_lines=%zu reset_signals=%d controller_ok=%d pane_reaped=%d elapsed_us=%lld",
            ck.checked,
            ck.failures.c_str(),
            total_lines,
            static_cast<int>(reset_spy.count()),
            controller_ok ? 1 : 0,
            pane_reaped ? 1 : 0,
            us);
    }
}

static void test_terminal_buffer_lifecycle(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed) {
    auto t0 = std::chrono::steady_clock::now();
    aida::terminal::TerminalSession session;
    session.cols = 32;
    session.rows_vis = 8;
    const char* payload = "one\r\ntwo\x1b[31m red\x1b[0m\r\n";
    aida::terminal::process_output(session, payload, std::strlen(payload));
    std::string first = row_text(session, 0);
    std::string second = row_text(session, 1);
    bool parsed = first.find("one") != std::string::npos
        && second.find("two red") != std::string::npos
        && session.lines.size() >= 2
        && session.cursor_row >= 1;
    aida::terminal::clear_session(session);
    bool cleared = session.lines.empty()
        && session.line_entrance_time.empty()
        && session.cursor_row == 0
        && session.cursor_col == 0
        && session.scroll_y == 0.f
        && session.scroll_to_bottom
        && session.auto_follow
        && session.prev_line_count == 0;
    aida::terminal::TerminalManager mgr;
    bool manager_empty = !mgr.has_active() && mgr.current() == nullptr && mgr.active_tab == -1;
    mgr.shutdown();
    bool manager_shutdown = mgr.sessions.empty() && mgr.active_tab == -1;

    qt_programming::AidaTerminalSearchBar search_bar(nullptr);
    QLineEdit* query_edit = search_bar.findChild<QLineEdit*>();
    QSignalSpy query_spy(&search_bar, &qt_programming::AidaTerminalSearchBar::queryChanged);
    if (query_edit)
        QTest::keyClicks(query_edit, QStringLiteral("err"));
    const bool search_ok = query_edit != nullptr
        && search_bar.query() == QStringLiteral("err")
        && query_spy.isValid() && query_spy.count() >= 3;
    search_bar.setCounter(1, 2);
    QLabel* counter = search_bar.findChild<QLabel*>();
    const bool counter_ok = counter != nullptr && counter->text() == QStringLiteral("1/2");

    bool view_ok = false;
    const bool controller_present = qt_programming::AidaTerminalController::exists();
    if (controller_present) {
        qt_programming::AidaTerminalView view(nullptr);
        view_ok = view.findChild<QTabBar*>() != nullptr;
    }

    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    log_msg(hf, "ui_terminal", "STATE -- parsed=%d cleared=%d manager_empty=%d manager_shutdown=%d search_ok=%d counter_ok=%d view_ok=%d controller_present=%d lines=%zu cursor_row=%d elapsed_us=%lld",
        parsed ? 1 : 0,
        cleared ? 1 : 0,
        manager_empty ? 1 : 0,
        manager_shutdown ? 1 : 0,
        search_ok ? 1 : 0,
        counter_ok ? 1 : 0,
        view_ok ? 1 : 0,
        controller_present ? 1 : 0,
        session.lines.size(),
        session.cursor_row,
        us);
    if (parsed && cleared && manager_empty && manager_shutdown && search_ok && counter_ok && (!controller_present || view_ok)) {
        pass(hf, passed, "ui_terminal", "terminal session parser, clear_session, empty manager lifecycle, and the Qt search bar input path work without spawning a shell (qt_terminal_widget=%s elapsed_us=%lld)",
            controller_present ? (view_ok ? "constructed" : "failed") : "not_constructed",
            us);
    } else {
        fail(hf, failed, "ui_terminal", "parsed=%d cleared=%d manager_empty=%d manager_shutdown=%d search_ok=%d counter_ok=%d view_ok=%d controller_present=%d first=\"%s\" second=\"%s\" elapsed_us=%lld",
            parsed ? 1 : 0,
            cleared ? 1 : 0,
            manager_empty ? 1 : 0,
            manager_shutdown ? 1 : 0,
            search_ok ? 1 : 0,
            counter_ok ? 1 : 0,
            view_ok ? 1 : 0,
            controller_present ? 1 : 0,
            first.c_str(),
            second.c_str(),
            us);
    }
}

static void test_settings_sandbox_mcp_roundtrip(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed) {
    auto t0 = std::chrono::steady_clock::now();
    const auto workspace_before = g_sa_settings.workspace;
    const auto sandbox_before = g_sa_settings.sandbox;
    const auto mcp_before = g_sa_settings.mcp_client_servers;
    const bool mcp_enabled_before = g_sa_settings.mcp_enabled;
    const int mcp_port_before = g_sa_settings.mcp_port;
    const std::string marketplace_before = g_sa_settings.marketplace_installed_json;

    g_sa_settings.workspace.view_visibility_json = R"({"left":false,"bottom":true})";
    g_sa_settings.workspace.right_visible = true;
    g_sa_settings.workspace.legacy_bottom_visible = true;
    g_sa_settings.workspace.active_view = "network";
    g_sa_settings.sandbox.enabled = true;
    g_sa_settings.sandbox.timeout_ms = 45000;
    g_sa_settings.sandbox.memory_limit_mb = 512;
    g_sa_settings.sandbox.network_mode = "off";
    g_sa_settings.mcp_enabled = true;
    g_sa_settings.mcp_port = 29117;
    g_sa_settings.mcp_client_servers.clear();
    g_sa_settings.mcp_client_servers.push_back({ "ui-local", "http://127.0.0.1:29117/mcp", "http_sse", "", "", "", true, false });

    mcp_marketplace::load_installed(R"([{"package_name":"aida-ui-test-mcp","version":"1.0.0","registry":"npm","install_path":"C:/AiDA/Test","transport":"stdio","command":"node","args":["server.js"],"env":{"AIDA_TEST":"1"},"enabled":true,"auto_connect":false}])");
    std::string serialized_market = mcp_marketplace::save_installed();
    auto installed = mcp_marketplace::get_installed();

    const auto workspace_visibility = nlohmann::json::parse(
        g_sa_settings.workspace.view_visibility_json, nullptr, false);
    bool workspace_ok = !workspace_visibility.is_discarded()
        && workspace_visibility.value("left", true) == false
        && workspace_visibility.value("bottom", false) == true
        && g_sa_settings.workspace.right_visible
        && g_sa_settings.workspace.legacy_bottom_visible
        && g_sa_settings.workspace.active_view == "network";
    bool sandbox_ok = g_sa_settings.sandbox.enabled
        && g_sa_settings.sandbox.timeout_ms == 45000
        && g_sa_settings.sandbox.memory_limit_mb == 512
        && g_sa_settings.sandbox.network_mode == "off";
    bool mcp_ok = g_sa_settings.mcp_enabled
        && g_sa_settings.mcp_port == 29117
        && g_sa_settings.mcp_client_servers.size() == 1
        && g_sa_settings.mcp_client_servers[0].name == "ui-local"
        && !g_sa_settings.mcp_client_servers[0].auto_connect;
    bool marketplace_ok = installed.size() == 1
        && installed[0].package_name == "aida-ui-test-mcp"
        && installed[0].args.size() == 1
        && serialized_market.find("aida-ui-test-mcp") != std::string::npos;

    qt_settings::AidaMcpServerDraftModel draft_model;
    draft_model.loadFromSettings();
    const bool draft_loaded = draft_model.rowCount() == 1 && !draft_model.dirty();
    QSignalSpy draft_edited_spy(&draft_model, &qt_settings::AidaMcpServerDraftModel::draftEdited);
    QSignalSpy draft_rows_spy(&draft_model, &qt_settings::AidaMcpServerDraftModel::rowsInserted);
    const int appended_row = draft_model.appendNewServer();
    const bool draft_appended = appended_row == 1 && draft_model.rowCount() == 2 && draft_model.dirty()
        && draft_rows_spy.count() >= 1;
    bool draft_edited = false;
    if (appended_row >= 0 && appended_row < draft_model.rowCount()) {
        mcp_client_server_t edited = draft_model.draft()[static_cast<std::size_t>(appended_row)];
        edited.name = "ui-local-edited";
        edited.url = "http://127.0.0.1:29117/mcp";
        draft_model.updateRow(appended_row, edited);
        draft_edited = draft_edited_spy.count() >= 1
            && draft_model.draft()[static_cast<std::size_t>(appended_row)].name == "ui-local-edited";
    }
    draft_model.discardChanges();
    const bool draft_discarded = !draft_model.dirty() && draft_model.rowCount() == 1;
    const bool draft_model_ok = draft_loaded && draft_appended && draft_edited && draft_discarded
        && draft_edited_spy.isValid() && draft_rows_spy.isValid();

    qt_settings::AidaSettingsMcpPage page(nullptr);
    QListView* servers_list = page.findChild<QListView*>();
    const bool page_ok = servers_list != nullptr && servers_list->model() != nullptr
        && servers_list->model()->rowCount() == 1;

    g_sa_settings.workspace = workspace_before;
    g_sa_settings.sandbox = sandbox_before;
    g_sa_settings.mcp_client_servers = mcp_before;
    g_sa_settings.mcp_enabled = mcp_enabled_before;
    g_sa_settings.mcp_port = mcp_port_before;
    g_sa_settings.marketplace_installed_json = marketplace_before;
    mcp_marketplace::load_installed(marketplace_before.empty() ? "[]" : marketplace_before);

    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    log_msg(hf, "ui_settings_mcp", "STATE -- workspace_ok=%d sandbox_ok=%d mcp_ok=%d marketplace_ok=%d draft_model_ok=%d page_ok=%d installed=%zu elapsed_us=%lld",
        workspace_ok ? 1 : 0,
        sandbox_ok ? 1 : 0,
        mcp_ok ? 1 : 0,
        marketplace_ok ? 1 : 0,
        draft_model_ok ? 1 : 0,
        page_ok ? 1 : 0,
        installed.size(),
        us);
    if (workspace_ok && sandbox_ok && mcp_ok && marketplace_ok && draft_model_ok && page_ok) {
        pass(hf, passed, "ui_settings_mcp", "workspace, sandbox, MCP client settings, marketplace storage, Qt MCP draft model, and the off-screen settings MCP page round-tripped in memory (elapsed_us=%lld)", us);
    } else {
        fail(hf, failed, "ui_settings_mcp", "workspace_ok=%d sandbox_ok=%d mcp_ok=%d marketplace_ok=%d draft_model_ok=%d page_ok=%d installed=%zu serialized=%s elapsed_us=%lld",
            workspace_ok ? 1 : 0,
            sandbox_ok ? 1 : 0,
            mcp_ok ? 1 : 0,
            marketplace_ok ? 1 : 0,
            draft_model_ok ? 1 : 0,
            page_ok ? 1 : 0,
            installed.size(),
            serialized_market.c_str(),
            us);
    }
}

static void test_testlab_view_state(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed) {
    auto t0 = std::chrono::steady_clock::now();
    check_accum_t ck;

    const auto& features = test_lab::all_features();
    ck.require(!features.empty(), "test registry non-empty");
    std::size_t malformed = 0;
    for (const auto& feature : features)
        if (!feature.name || !feature.category || !feature.run)
            ++malformed;
    ck.require(malformed == 0, "every registered feature carries name/category/run");

    auto* controller = qt_testlab::TestLabController::instance();
    ck.require(controller != nullptr, "testlab controller instance available");
    const int previous_selection = controller ? controller->selectedFeature() : -1;

    QPointer<QWidget> widget;
    if (controller)
        widget = qt_testlab::createTestLabWidget(nullptr);
    ck.require(!widget.isNull(), "testlab widget constructed off-screen");

    int selection_signals = 0;
    int result_signals = 0;
    bool selection_ok = false;
    if (controller && !widget.isNull() && !features.empty()) {
        QSignalSpy selection_spy(controller, &qt_testlab::TestLabController::selectionChanged);
        QSignalSpy result_spy(controller, &qt_testlab::TestLabController::resultChanged);
        controller->selectFeature(0);
        selection_signals = selection_spy.count();
        if (selection_signals == 0)
            selection_signals = selection_spy.wait(1000) ? 1 : 0;
        result_signals = result_spy.count();
        if (result_signals == 0)
            result_signals = result_spy.wait(1000) ? 1 : 0;
        selection_ok = selection_spy.isValid()
            && controller->selectedFeature() == 0
            && selection_signals >= 1
            && controller->featureAt(0) != nullptr
            && controller->featureAt(0) == &features.front();
    }
    ck.require(selection_ok, "controller selection roundtrip through the Qt signal");

    nlohmann::json evidence;
    evidence["features"] = features.size();
    evidence["malformed"] = malformed;
    if (controller) {
        evidence["summaries"] = controller->cachedSummaries().size();
        const auto& cached = controller->cachedResult();
        evidence["result_state"] = static_cast<int>(cached.state.load(std::memory_order_acquire));
        evidence["result_outcome"] = static_cast<int>(cached.outcome);
        evidence["result_ok"] = cached.ok;
        evidence["run_all_active"] = controller->cachedRunAll().active;
        evidence["selected"] = controller->selectedFeature();
    }
    evidence["selection_signals"] = selection_signals;
    evidence["result_signals"] = result_signals;

    set_progress_step("ui coverage phase");
    char snap[1200] = {};
    format_debug_snapshot(snap, sizeof(snap));
    const bool snapshot_ok = std::strstr(snap, "ui coverage phase") != nullptr
        && std::strstr(snap, "running=") != nullptr
        && std::strstr(snap, "pass=") != nullptr
        && std::strstr(snap, "fail=") != nullptr;
    const bool running_ok = is_running() && std::strstr(snap, "running=1") != nullptr;
    ck.require(snapshot_ok, "debug snapshot carries the progress step");
    ck.require(running_ok, "full-test running state visible");

    bool widget_reaped = true;
    if (!widget.isNull()) {
        widget_reaped = false;
        QSignalSpy destroyed_spy(widget.data(), &QObject::destroyed);
        widget->deleteLater();
        widget_reaped = spy_pump_until(destroyed_spy, [&] { return widget.isNull(); }, ui_now_ms() + 5000);
    }
    ck.require(widget_reaped, "testlab widget deferred deletion observed");
    evidence["widget_reaped"] = widget_reaped;

    if (controller && controller->selectedFeature() != previous_selection)
        controller->selectFeature(previous_selection);

    const std::string evidence_text = evidence.dump();
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    log_msg(hf, "ui_testlab", "STATE -- ck_checks=%d ck_ok=%d selection_signals=%d result_signals=%d widget_reaped=%d elapsed_us=%lld",
        ck.checked,
        ck.ok ? 1 : 0,
        selection_signals,
        result_signals,
        widget_reaped ? 1 : 0,
        us);
    if (ck.ok) {
        pass(hf, passed, "ui_testlab", "Qt Test Lab registry/controller/widget contract, result JSON evidence, progress step, and running-state snapshot passed evidence=%s elapsed_us=%lld",
            evidence_text.c_str(),
            us);
    } else {
        fail(hf, failed, "ui_testlab", "checked=%d failures=%s snapshot=%s evidence=%s elapsed_us=%lld",
            ck.checked,
            ck.failures.c_str(),
            snap,
            evidence_text.c_str(),
            us);
    }
}

static void test_surface_retirements(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed) {
    auto t0 = std::chrono::steady_clock::now();
    const char* tag = "ui_surface_retirements";
    const QString ledger_rel = QStringLiteral("src/standalone/tests/analysis_workspace/surface_retirements_qt.json");

    QString repo_root;
    auto probe = [&](QString start) -> bool {
        QDir dir(start);
        for (int depth = 0; depth < 16; ++depth) {
            if (QFileInfo::exists(dir.absoluteFilePath(ledger_rel))) {
                repo_root = dir.absolutePath();
                return true;
            }
            if (!dir.cdUp())
                break;
        }
        return false;
    };
    const bool found = probe(QCoreApplication::applicationDirPath()) || probe(QDir::currentPath());
    if (!found) {
        skip(hf, failed, tag, "retirement ledger not found relative to applicationDirPath or currentPath; repo root unresolved");
        return;
    }

    const QDir root_dir(repo_root);
    QFile ledger_file(root_dir.absoluteFilePath(ledger_rel));
    if (!ledger_file.open(QIODevice::ReadOnly)) {
        fail(hf, failed, tag, "ledger open failed path=%s", ledger_file.fileName().toStdString().c_str());
        return;
    }
    const std::string ledger_text = QString::fromUtf8(ledger_file.readAll()).toStdString();
    const auto ledger = nlohmann::json::parse(ledger_text, nullptr, false);
    if (ledger.is_discarded() || !ledger.is_object() || !ledger.contains("retirements") || !ledger["retirements"].is_array()) {
        fail(hf, failed, tag, "ledger schema invalid: root object with retirements array required");
        return;
    }

    static const char* const k_allowed_kinds[] = {
        "mcp_registration", "mcp_resource", "center_view", "ui_action",
        "ui_shortcut_key", "source_contract", "test_lab_feature"
    };
    bool schema_ok = true;
    std::string schema_failure;
    std::set<std::pair<std::string, std::string>> seen;
    std::vector<std::string> retirement_ids;
    std::size_t row_index = 0;
    for (const auto& row : ledger["retirements"]) {
        ++row_index;
        auto field = [&](const char* name) -> std::string {
            if (!row.is_object() || !row.contains(name) || !row[name].is_string())
                return {};
            return row[name].get<std::string>();
        };
        const std::string kind = field("kind");
        const std::string id = field("id");
        const std::string reason = field("reason");
        const std::string plan = field("plan");
        const std::string replacement = field("replacement");
        auto bad = [&](const char* what) {
            schema_ok = false;
            if (schema_failure.empty())
                schema_failure = "row " + std::to_string(row_index) + ": " + what;
        };
        if (kind.empty() || id.empty() || reason.empty() || plan.empty() || replacement.empty()) {
            bad("missing or empty kind/id/reason/plan/replacement");
            continue;
        }
        bool kind_ok = false;
        for (const char* allowed : k_allowed_kinds)
            if (kind == allowed) kind_ok = true;
        if (!kind_ok)
            bad("kind outside the allowed enum");
        if (!seen.insert({ kind, id }).second)
            bad("duplicate kind+id");
        retirement_ids.push_back(id);
        if (replacement != "none") {
            QString rep = QString::fromStdString(replacement);
            rep.replace('\\', '/');
            const bool prefix_ok = rep.startsWith(QStringLiteral("src/standalone/src/qt/"))
                || rep.startsWith(QStringLiteral("src/standalone/src/core/testlab/qt/"));
            const QFileInfo replacement_file(root_dir.absoluteFilePath(rep));
            if (!prefix_ok || !replacement_file.exists() || !replacement_file.isFile())
                bad("replacement must be \"none\" or an existing file under the Qt source trees");
        }
    }
    if (retirement_ids.empty()) {
        schema_ok = false;
        if (schema_failure.empty())
            schema_failure = "retirements array is empty";
    }

    static const char* const k_legacy_tokens[] = {
        "Im" "Gui::", "img" "ui.h", "img" "ui_internal.h", "core/ui/design_system", "core/ui/theme.hpp",
        "core/ui/components.hpp", "core/ui/empty_state", "core/ui/ui_anim", "core/ui/quick_open",
        "core/ui/hub_strip", "core/ui/skeleton", "core/ui/responsive", "core/ui/brand",
        "core/ui/avatar", "core/ui/application_view_registry", "core/ui/view_registry",
        "core/ui/workspace_layout", "preview/"
    };
    std::vector<std::string> needles;
    needles.reserve(retirement_ids.size() + sizeof(k_legacy_tokens) / sizeof(k_legacy_tokens[0]));
    for (const auto& id : retirement_ids)
        needles.push_back(id);
    for (const char* token : k_legacy_tokens)
        needles.push_back(token);

    const QString scan_roots[] = {
        root_dir.absoluteFilePath(QStringLiteral("src/standalone/src/qt")),
        root_dir.absoluteFilePath(QStringLiteral("src/standalone/src/core/testlab/qt"))
    };
    std::size_t files_scanned = 0;
    std::size_t hit_count = 0;
    std::string first_hits;
    for (const QString& scan_root : scan_roots) {
        QDirIterator it(scan_root,
            QStringList{ QStringLiteral("*.cpp"), QStringLiteral("*.hpp"), QStringLiteral("*.h") },
            QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file_path = it.next();
            QFile file(file_path);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QString text = QString::fromUtf8(file.readAll());
            ++files_scanned;
            for (const auto& needle : needles) {
                const QString qneedle = QString::fromStdString(needle);
                qsizetype pos = 0;
                int hits_in_file = 0;
                while ((pos = text.indexOf(qneedle, pos)) != -1) {
                    ++hits_in_file;
                    pos += qneedle.size();
                }
                if (hits_in_file > 0) {
                    hit_count += static_cast<std::size_t>(hits_in_file);
                    if (first_hits.size() < 480) {
                        if (!first_hits.empty()) first_hits += "; ";
                        first_hits += root_dir.relativeFilePath(file_path).toStdString() + "<-" + needle;
                    }
                }
            }
        }
    }
    const bool sweep_ok = hit_count == 0;

    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (schema_ok && sweep_ok && files_scanned > 0) {
        pass(hf, passed, tag, "retirement ledger schema valid and zero-reference sweep clean rows=%zu ids=%zu files_scanned=%zu hits=%zu elapsed_us=%lld",
            row_index,
            retirement_ids.size(),
            files_scanned,
            hit_count,
            us);
    } else {
        fail(hf, failed, tag, "schema_ok=%d sweep_ok=%d rows=%zu ids=%zu files_scanned=%zu hits=%zu schema_failure=%s first_hits=%s elapsed_us=%lld",
            schema_ok ? 1 : 0,
            sweep_ok ? 1 : 0,
            row_index,
            retirement_ids.size(),
            files_scanned,
            hit_count,
            schema_failure.c_str(),
            first_hits.c_str(),
            us);
    }
}

}

using ui_phase_step_fn_t = void(*)(HANDLE, std::atomic<int>&, std::atomic<int>&);

struct ui_phase_step_t {
    const char* tag;
    ui_phase_step_fn_t fn;
};

static constexpr ui_phase_step_t k_ui_phase_steps[] = {
    { "ui_center", test_center_view_file_open },
    { "ui_file_browser", test_file_browser_directory_and_routes },
    { "ui_tabs", test_file_tab_lifecycle },
    { "ui_editor", test_code_editor_save_find_and_diff },
    { "ui_activity", test_activity_search_recent },
    { "ui_commands_routes", test_command_palette_and_center_views },
    { "ui_bottom", test_bottom_log_tabs },
    { "ui_terminal", test_terminal_buffer_lifecycle },
    { "ui_settings_mcp", test_settings_sandbox_mcp_roundtrip },
    { "ui_testlab", test_testlab_view_state },
    { "ui_surface_retirements", test_surface_retirements },
};

static constexpr std::size_t k_ui_phase_step_count = sizeof(k_ui_phase_steps) / sizeof(k_ui_phase_steps[0]);

static bool run_ui_phase_step(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed, std::size_t step_index, bool(*cancelled)(), std::uint64_t job_id) {
    if (step_index >= k_ui_phase_step_count)
        return false;
    const auto& step = k_ui_phase_steps[step_index];
    const int ordinal = static_cast<int>(step_index + 1);
    g_ui_phase_active_step_index.store(ordinal, std::memory_order_release);
    g_ui_phase_active_step_name.store(step.tag, std::memory_order_release);
    if (cancelled && cancelled()) {
        log_msg(hf, "ui_phase", "cancel before job=%llu idx=%d name=%s tid=%lu",
            static_cast<unsigned long long>(job_id),
            ordinal,
            step.tag,
            static_cast<unsigned long>(GetCurrentThreadId()));
        return false;
    }
    const int pass_before = passed.load(std::memory_order_acquire);
    const int fail_before = failed.load(std::memory_order_acquire);
    const std::uint64_t started = ui_now_ms();
    log_msg(hf, "ui_phase", "BEGIN job=%llu idx=%d name=%s tid=%lu pass=%d fail=%d",
        static_cast<unsigned long long>(job_id),
        ordinal,
        step.tag,
        static_cast<unsigned long>(GetCurrentThreadId()),
        pass_before,
        fail_before);
    step.fn(hf, passed, failed);
    const std::uint64_t elapsed = ui_now_ms() - started;
    g_ui_phase_last_job_run_ms.store(elapsed, std::memory_order_release);
    const int pass_after = passed.load(std::memory_order_acquire);
    const int fail_after = failed.load(std::memory_order_acquire);
    log_msg(hf, "ui_phase", "END job=%llu idx=%d name=%s tid=%lu elapsed_ms=%llu pass_delta=%d fail_delta=%d pass=%d fail=%d",
        static_cast<unsigned long long>(job_id),
        ordinal,
        step.tag,
        static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long long>(elapsed),
        pass_after - pass_before,
        fail_after - fail_before,
        pass_after,
        fail_after);
    return true;
}

static void phase_ui_tests_inline(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed, std::atomic<int>& skipped, bool(*cancelled)()) {
    (void)skipped;
    const DWORD tid = GetCurrentThreadId();
    log_msg(hf, "ui_phase", "running standalone UI state/workflow tests tid=%lu",
        static_cast<unsigned long>(tid));

    g_ui_phase_active_job_id.store(0, std::memory_order_release);
    g_ui_phase_active_worker_tid.store(0, std::memory_order_release);
    for (std::size_t i = 0; i < k_ui_phase_step_count; ++i) {
        const bool ran = run_ui_phase_step(hf, passed, failed, i, cancelled, 0);
        if (!ran)
            break;
        g_ui_phase_steps_processed_total.fetch_add(1u, std::memory_order_acq_rel);
    }
    g_ui_phase_active_step_index.store(-1, std::memory_order_release);
    g_ui_phase_active_step_name.store("<idle>", std::memory_order_release);
}

static void fail_pending_ui_dispatch(HANDLE hf, std::atomic<int>& failed, const char* reason, DWORD worker_tid, DWORD ui_tid, std::uint64_t elapsed_ms) {
    char snap[1200] = {};
    test_all_features::format_debug_snapshot(snap, sizeof(snap));
    fail(hf, failed, "ui_dispatch", "render-thread dispatch failed reason=%s worker_tid=%lu ui_tid=%lu elapsed_ms=%llu snapshot=%s",
        reason ? reason : "unknown",
        static_cast<unsigned long>(worker_tid),
        static_cast<unsigned long>(ui_tid),
        static_cast<unsigned long long>(elapsed_ms),
        snap);
    failed.fetch_add(9, std::memory_order_acq_rel);
}

static void decrement_ui_phase_pending()
{
    std::size_t current = g_ui_phase_pending_jobs.load(std::memory_order_acquire);
    while (current != 0) {
        if (g_ui_phase_pending_jobs.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel))
            break;
    }
    g_ui_phase_last_pending_count.store(g_ui_phase_pending_jobs.load(std::memory_order_acquire), std::memory_order_release);
}

static void mark_ui_phase_job_done_locked(const std::shared_ptr<ui_phase_job_t>& job, DWORD ui_tid)
{
    if (!job || job->done)
        return;
    job->done = true;
    job->finished_ms = ui_now_ms();
    if (ui_tid != 0)
        job->ui_tid = ui_tid;
    decrement_ui_phase_pending();
}

static void run_ui_phase_dispatch_job(std::shared_ptr<ui_phase_job_t> job);

static bool post_ui_phase_dispatch_job(const std::shared_ptr<ui_phase_job_t>& job, const char* phase)
{
    if (!job)
        return false;
    aida::ui_thread::post_options_t options;
    options.subsystem = "testlab";
    options.label = "ui_phase";
    options.phase = phase ? phase : "<none>";
    options.owner = "testlab";
    options.priority = aida::ui_thread::priority_t::high;
    options.deadline_ms = ui_now_ms() + 15000ULL;
    options.cancelled = [job]() {
        std::lock_guard<std::mutex> lk(g_ui_phase_mtx);
        return !job || job->dispatch_cancelled || job->done || (job->cancelled && job->cancelled());
    };
    const aida::ui_thread::enqueue_result_t result = aida::ui_thread::post([job]() {
        run_ui_phase_dispatch_job(job);
    }, std::move(options));
    const bool posted = result == aida::ui_thread::enqueue_result_t::accepted;
    std::uint64_t id = 0;
    DWORD worker_tid = 0;
    DWORD ui_tid = 0;
    bool started = false;
    bool done = false;
    bool dispatch_cancelled = false;
    {
        std::lock_guard<std::mutex> lk(g_ui_phase_mtx);
        id = job->id;
        worker_tid = job->worker_tid;
        ui_tid = job->ui_tid;
        started = job->started;
        done = job->done;
        dispatch_cancelled = job->dispatch_cancelled;
    }
    diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
        "post phase=%s posted=%d result=%s job=%llu worker_tid=%lu ui_tid=%lu pending=%llu dispatcher_pending=%zu started=%d done=%d cancelled=%d",
        phase ? phase : "<none>",
        posted ? 1 : 0,
        aida::ui_thread::result_name(result),
        static_cast<unsigned long long>(id),
        static_cast<unsigned long>(worker_tid),
        static_cast<unsigned long>(ui_tid),
        static_cast<unsigned long long>(g_ui_phase_pending_jobs.load(std::memory_order_acquire)),
        aida::ui_thread::pending_count(),
        started ? 1 : 0,
        done ? 1 : 0,
        dispatch_cancelled ? 1 : 0);
    return posted;
}

static void reset_ui_phase_active_state()
{
    g_ui_phase_active_job_id.store(0, std::memory_order_release);
    g_ui_phase_active_worker_tid.store(0, std::memory_order_release);
    g_ui_phase_active_step_index.store(-1, std::memory_order_release);
    g_ui_phase_active_step_name.store("<idle>", std::memory_order_release);
}

static void run_ui_phase_dispatch_job(std::shared_ptr<ui_phase_job_t> job)
{
    const DWORD ui_tid = GetCurrentThreadId();
    g_ui_phase_thread_id.store(ui_tid, std::memory_order_release);
    aida::ui_thread::capture_owner_tid(ui_tid, "testlab", "ui_phase", "dispatch_run");
    if (!aida::ui_thread::require_owner("testlab", "ui_phase", "dispatch_run"))
        return;
    const std::uint64_t pump_seq = g_ui_phase_last_pump_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::uint64_t pump_start = ui_now_ms();
    if (!job) {
        g_ui_phase_skipped_no_job_count.fetch_add(1u, std::memory_order_acq_rel);
        diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
            "discard_null seq=%llu ui_tid=%lu",
            static_cast<unsigned long long>(pump_seq),
            static_cast<unsigned long>(ui_tid));
        return;
    }

    std::size_t step_index = 0;
    std::size_t remaining_before = 0;
    std::uint64_t wait_ms = 0;
    {
        std::unique_lock<std::mutex> lk(g_ui_phase_mtx);
        remaining_before = g_ui_phase_pending_jobs.load(std::memory_order_acquire);
        g_ui_phase_last_pending_count.store(remaining_before, std::memory_order_release);
        if (job->done) {
            lk.unlock();
            g_ui_phase_skipped_no_job_count.fetch_add(1u, std::memory_order_acq_rel);
            diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
                "discard_stale_done seq=%llu job=%llu ui_tid=%lu pending=%llu",
                static_cast<unsigned long long>(pump_seq),
                static_cast<unsigned long long>(job->id),
                static_cast<unsigned long>(ui_tid),
                static_cast<unsigned long long>(remaining_before));
            return;
        }
        if (job->dispatch_cancelled || (job->cancelled && job->cancelled())) {
            job->dispatch_cancelled = true;
            mark_ui_phase_job_done_locked(job, ui_tid);
            reset_ui_phase_active_state();
            lk.unlock();
            g_ui_phase_cv.notify_all();
            diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
                "discard_cancelled_before_start seq=%llu job=%llu ui_tid=%lu worker_tid=%lu pending=%llu",
                static_cast<unsigned long long>(pump_seq),
                static_cast<unsigned long long>(job->id),
                static_cast<unsigned long>(ui_tid),
                static_cast<unsigned long>(job->worker_tid),
                static_cast<unsigned long long>(g_ui_phase_pending_jobs.load(std::memory_order_acquire)));
            return;
        }
        if (!job->started) {
            job->started = true;
            job->started_ms = ui_now_ms();
        }
        job->ui_tid = ui_tid;
        wait_ms = job->started_ms >= job->queued_ms ? job->started_ms - job->queued_ms : 0;
        g_ui_phase_last_job_wait_ms.store(wait_ms, std::memory_order_release);
        g_ui_phase_active_job_id.store(job->id, std::memory_order_release);
        g_ui_phase_active_worker_tid.store(job->worker_tid, std::memory_order_release);
        step_index = job->next_step;
        if (step_index < k_ui_phase_step_count) {
            g_ui_phase_active_step_index.store(static_cast<int>(step_index + 1), std::memory_order_release);
            g_ui_phase_active_step_name.store(k_ui_phase_steps[step_index].tag, std::memory_order_release);
        } else {
            g_ui_phase_active_step_index.store(-1, std::memory_order_release);
            g_ui_phase_active_step_name.store("<complete>", std::memory_order_release);
        }
    }
    g_ui_phase_cv.notify_all();

    log_msg(job->hf, "ui_phase", "dispatcher job step start seq=%llu job=%llu ui_tid=%lu worker_tid=%lu wait_ms=%llu pending=%llu next_step=%llu",
        static_cast<unsigned long long>(pump_seq),
        static_cast<unsigned long long>(job->id),
        static_cast<unsigned long>(ui_tid),
        static_cast<unsigned long>(job->worker_tid),
        static_cast<unsigned long long>(wait_ms),
        static_cast<unsigned long long>(remaining_before),
        static_cast<unsigned long long>(step_index + 1));
    diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
        "step_start seq=%llu job=%llu ui_tid=%lu worker_tid=%lu wait_ms=%llu pending=%llu step=%llu/%llu name=%s dispatcher_pending=%zu",
        static_cast<unsigned long long>(pump_seq),
        static_cast<unsigned long long>(job->id),
        static_cast<unsigned long>(ui_tid),
        static_cast<unsigned long>(job->worker_tid),
        static_cast<unsigned long long>(wait_ms),
        static_cast<unsigned long long>(remaining_before),
        static_cast<unsigned long long>(step_index + 1),
        static_cast<unsigned long long>(k_ui_phase_step_count),
        step_index < k_ui_phase_step_count ? k_ui_phase_steps[step_index].tag : "<complete>",
        aida::ui_thread::pending_count());

    bool finished = false;
    bool requeue = false;
    bool ran_step = false;
    const std::uint64_t run_start = ui_now_ms();
    if (job->passed && job->failed && job->skipped) {
        try {
            aida::diagnostic_exception_scope::scope_t exception_scope("test_all_features.ui_dispatcher.run_ui_phase_step");
            if (step_index < k_ui_phase_step_count) {
                ran_step = run_ui_phase_step(job->hf, *job->passed, *job->failed, step_index, job->cancelled, job->id);
            } else {
                finished = true;
            }
        } catch (...) {
            fail_pending_ui_dispatch(job->hf, *job->failed, "cpp_exception", job->worker_tid, ui_tid, 0);
            diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
                "step_cpp_exception seq=%llu job=%llu ui_tid=%lu worker_tid=%lu step=%llu",
                static_cast<unsigned long long>(pump_seq),
                static_cast<unsigned long long>(job->id),
                static_cast<unsigned long>(ui_tid),
                static_cast<unsigned long>(job->worker_tid),
                static_cast<unsigned long long>(step_index + 1));
            finished = true;
        }
    } else {
        if (job->failed)
            fail_pending_ui_dispatch(job->hf, *job->failed, "bad_job_state", job->worker_tid, ui_tid, 0);
        finished = true;
    }

    const std::uint64_t run_elapsed = ui_now_ms() - run_start;
    g_ui_phase_last_job_run_ms.store(run_elapsed, std::memory_order_release);
    if (!finished) {
        std::unique_lock<std::mutex> lk(g_ui_phase_mtx);
        if (job->done) {
            finished = true;
        } else if (job->dispatch_cancelled || (job->cancelled && job->cancelled())) {
            job->dispatch_cancelled = true;
            mark_ui_phase_job_done_locked(job, ui_tid);
            reset_ui_phase_active_state();
            finished = true;
        } else {
            if (ran_step && job->next_step == step_index) {
                ++job->next_step;
                ++job->processed_steps;
                g_ui_phase_steps_processed_total.fetch_add(1u, std::memory_order_acq_rel);
            } else if (!ran_step) {
                finished = true;
            }
            if (!finished && job->next_step >= k_ui_phase_step_count)
                finished = true;
            if (finished) {
                mark_ui_phase_job_done_locked(job, ui_tid);
                reset_ui_phase_active_state();
            } else {
                requeue = true;
                g_ui_phase_skipped_by_budget_count.fetch_add(1u, std::memory_order_acq_rel);
                if (job->next_step < k_ui_phase_step_count) {
                    g_ui_phase_active_step_index.store(static_cast<int>(job->next_step + 1), std::memory_order_release);
                    g_ui_phase_active_step_name.store(k_ui_phase_steps[job->next_step].tag, std::memory_order_release);
                }
                g_ui_phase_active_job_id.store(0, std::memory_order_release);
                g_ui_phase_active_worker_tid.store(0, std::memory_order_release);
            }
        }
    } else {
        std::unique_lock<std::mutex> lk(g_ui_phase_mtx);
        mark_ui_phase_job_done_locked(job, ui_tid);
        reset_ui_phase_active_state();
    }
    g_ui_phase_cv.notify_all();

    if (requeue && !post_ui_phase_dispatch_job(job, "requeue")) {
        std::unique_lock<std::mutex> lk(g_ui_phase_mtx);
        if (!job->done) {
            if (job->failed)
                fail_pending_ui_dispatch(job->hf, *job->failed, "dispatcher_requeue_failed", job->worker_tid, ui_tid, 0);
            mark_ui_phase_job_done_locked(job, ui_tid);
            reset_ui_phase_active_state();
        }
        lk.unlock();
        g_ui_phase_cv.notify_all();
    }

    const std::uint64_t pump_wall = ui_now_ms() - pump_start;
    g_ui_phase_last_pump_wall_ms.store(pump_wall, std::memory_order_release);
    log_msg(job->hf, "ui_phase", "dispatcher job step end seq=%llu job=%llu ui_tid=%lu worker_tid=%lu run_ms=%llu wall_ms=%llu finished=%d requeued=%d ran_step=%d next_step=%llu pending=%llu",
        static_cast<unsigned long long>(pump_seq),
        static_cast<unsigned long long>(job->id),
        static_cast<unsigned long>(ui_tid),
        static_cast<unsigned long>(job->worker_tid),
        static_cast<unsigned long long>(run_elapsed),
        static_cast<unsigned long long>(pump_wall),
        finished ? 1 : 0,
        requeue ? 1 : 0,
        ran_step ? 1 : 0,
        static_cast<unsigned long long>(job->next_step + 1),
        static_cast<unsigned long long>(g_ui_phase_pending_jobs.load(std::memory_order_acquire)));
    diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
        "step_end seq=%llu job=%llu ui_tid=%lu worker_tid=%lu run_ms=%llu wall_ms=%llu finished=%d requeued=%d ran_step=%d next_step=%llu pending=%llu dispatcher_pending=%zu skipped_budget=%llu processed_total=%llu",
        static_cast<unsigned long long>(pump_seq),
        static_cast<unsigned long long>(job->id),
        static_cast<unsigned long>(ui_tid),
        static_cast<unsigned long>(job->worker_tid),
        static_cast<unsigned long long>(run_elapsed),
        static_cast<unsigned long long>(pump_wall),
        finished ? 1 : 0,
        requeue ? 1 : 0,
        ran_step ? 1 : 0,
        static_cast<unsigned long long>(job->next_step + 1),
        static_cast<unsigned long long>(g_ui_phase_pending_jobs.load(std::memory_order_acquire)),
        aida::ui_thread::pending_count(),
        static_cast<unsigned long long>(g_ui_phase_skipped_by_budget_count.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_phase_steps_processed_total.load(std::memory_order_acquire)));
}

void phase_ui_tests(HANDLE hf, std::atomic<int>& passed, std::atomic<int>& failed, std::atomic<int>& skipped, bool(*cancelled)()) {
    const DWORD current_tid = GetCurrentThreadId();
    DWORD ui_tid = aida::ui_thread::owner_tid();
    if (ui_tid == 0)
        ui_tid = g_ui_phase_thread_id.load(std::memory_order_acquire);
    if (ui_tid != 0 && ui_tid == current_tid) {
        g_ui_phase_thread_id.store(current_tid, std::memory_order_release);
        aida::ui_thread::capture_owner_tid(current_tid, "testlab", "ui_phase", "inline");
        if (!aida::ui_thread::require_owner("testlab", "ui_phase", "inline"))
            return;
        log_msg(hf, "ui_phase", "dispatch inline on ui thread tid=%lu",
            static_cast<unsigned long>(current_tid));
        phase_ui_tests_inline(hf, passed, failed, skipped, cancelled);
        return;
    }

    auto job = std::make_shared<ui_phase_job_t>();
    job->id = g_ui_phase_next_job_id.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    job->hf = hf;
    job->passed = &passed;
    job->failed = &failed;
    job->skipped = &skipped;
    job->cancelled = cancelled;
    job->worker_tid = current_tid;
    job->ui_tid = ui_tid;
    job->queued_ms = ui_now_ms();

    const std::size_t pending_after_add = g_ui_phase_pending_jobs.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    g_ui_phase_last_pending_count.store(pending_after_add, std::memory_order_release);
    const bool posted = post_ui_phase_dispatch_job(job, "initial");
    if (!posted) {
        decrement_ui_phase_pending();
        fail_pending_ui_dispatch(hf, failed, "dispatcher_post_failed", current_tid, ui_tid, 0);
        diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
            "post_failed job=%llu worker_tid=%lu known_ui_tid=%lu pending=%llu dispatcher_pending=%zu",
            static_cast<unsigned long long>(job->id),
            static_cast<unsigned long>(current_tid),
            static_cast<unsigned long>(ui_tid),
            static_cast<unsigned long long>(g_ui_phase_pending_jobs.load(std::memory_order_acquire)),
            aida::ui_thread::pending_count());
        return;
    }

    constexpr std::uint64_t kDispatchPickupTimeoutMs = 15000;
    log_msg(hf, "ui_phase", "queued dispatcher UI tests job=%llu worker_tid=%lu known_ui_tid=%lu pending=%llu pickup_timeout_ms=%llu",
        static_cast<unsigned long long>(job->id),
        static_cast<unsigned long>(current_tid),
        static_cast<unsigned long>(ui_tid),
        static_cast<unsigned long long>(pending_after_add),
        static_cast<unsigned long long>(kDispatchPickupTimeoutMs));
    diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
        "queued job=%llu worker_tid=%lu known_ui_tid=%lu pending=%llu dispatcher_pending=%zu pickup_timeout_ms=%llu",
        static_cast<unsigned long long>(job->id),
        static_cast<unsigned long>(current_tid),
        static_cast<unsigned long>(ui_tid),
        static_cast<unsigned long long>(pending_after_add),
        aida::ui_thread::pending_count(),
        static_cast<unsigned long long>(kDispatchPickupTimeoutMs));

    std::unique_lock<std::mutex> lk(g_ui_phase_mtx);
    for (;;) {
        if (job->done) {
            std::uint64_t elapsed = job->finished_ms >= job->queued_ms ? job->finished_ms - job->queued_ms : 0;
            DWORD actual_ui_tid = job->ui_tid;
            const std::uint64_t steps = job->processed_steps;
            lk.unlock();
            log_msg(hf, "ui_phase", "dispatcher UI tests complete job=%llu worker_tid=%lu ui_tid=%lu elapsed_ms=%llu steps=%llu",
                static_cast<unsigned long long>(job->id),
                static_cast<unsigned long>(current_tid),
                static_cast<unsigned long>(actual_ui_tid),
                static_cast<unsigned long long>(elapsed),
                static_cast<unsigned long long>(steps));
            diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
                "complete job=%llu worker_tid=%lu ui_tid=%lu elapsed_ms=%llu steps=%llu pending=%llu dispatcher_pending=%zu",
                static_cast<unsigned long long>(job->id),
                static_cast<unsigned long>(current_tid),
                static_cast<unsigned long>(actual_ui_tid),
                static_cast<unsigned long long>(elapsed),
                static_cast<unsigned long long>(steps),
                static_cast<unsigned long long>(g_ui_phase_pending_jobs.load(std::memory_order_acquire)),
                aida::ui_thread::pending_count());
            return;
        }

        if (cancelled && cancelled() && !job->started) {
            job->dispatch_cancelled = true;
            mark_ui_phase_job_done_locked(job, ui_tid);
            lk.unlock();
            g_ui_phase_cv.notify_all();
            log_msg(hf, "ui_phase", "dispatcher UI tests cancelled before pickup job=%llu worker_tid=%lu known_ui_tid=%lu",
                static_cast<unsigned long long>(job->id),
                static_cast<unsigned long>(current_tid),
                static_cast<unsigned long>(ui_tid));
            diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
                "cancel_before_pickup job=%llu worker_tid=%lu known_ui_tid=%lu pending=%llu dispatcher_pending=%zu",
                static_cast<unsigned long long>(job->id),
                static_cast<unsigned long>(current_tid),
                static_cast<unsigned long>(ui_tid),
                static_cast<unsigned long long>(g_ui_phase_pending_jobs.load(std::memory_order_acquire)),
                aida::ui_thread::pending_count());
            return;
        }

        std::uint64_t now = ui_now_ms();
        std::uint64_t elapsed = now >= job->queued_ms ? now - job->queued_ms : 0;
        if (!job->started && elapsed >= kDispatchPickupTimeoutMs) {
            job->dispatch_cancelled = true;
            DWORD last_ui_tid = g_ui_phase_thread_id.load(std::memory_order_acquire);
            if (last_ui_tid == 0)
                last_ui_tid = aida::ui_thread::owner_tid();
            mark_ui_phase_job_done_locked(job, last_ui_tid);
            lk.unlock();
            g_ui_phase_cv.notify_all();
            fail_pending_ui_dispatch(hf, failed, "pickup_timeout", current_tid, last_ui_tid, elapsed);
            diag::log_tagged_critical_fmt("TESTLAB-UI-DISPATCH",
                "pickup_timeout job=%llu worker_tid=%lu known_ui_tid=%lu elapsed_ms=%llu pending=%llu dispatcher_pending=%zu",
                static_cast<unsigned long long>(job->id),
                static_cast<unsigned long>(current_tid),
                static_cast<unsigned long>(last_ui_tid),
                static_cast<unsigned long long>(elapsed),
                static_cast<unsigned long long>(g_ui_phase_pending_jobs.load(std::memory_order_acquire)),
                aida::ui_thread::pending_count());
            return;
        }

        g_ui_phase_cv.wait_for(lk, std::chrono::milliseconds(job->started ? 250 : 25));
    }
}

void format_ui_phase_snapshot(char* out, std::size_t cap) {
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    std::size_t pending = g_ui_phase_pending_jobs.load(std::memory_order_acquire);
    bool lock_busy = false;
    std::uint64_t lock_start = ui_now_ms();
    std::unique_lock<std::mutex> lk(g_ui_phase_mtx, std::try_to_lock);
    const std::uint64_t lock_wait = ui_now_ms() - lock_start;
    g_ui_phase_last_lock_wait_ms.store(lock_wait, std::memory_order_release);
    if (lk.owns_lock()) {
        g_ui_phase_last_pending_count.store(pending, std::memory_order_release);
        lk.unlock();
    } else {
        lock_busy = true;
        g_ui_phase_lock_busy_count.fetch_add(1u, std::memory_order_acq_rel);
    }
    const char* step_name = g_ui_phase_active_step_name.load(std::memory_order_acquire);
    _snprintf_s(out, cap, _TRUNCATE,
        "ui_pending=%zu ui_lock_busy=%d ui_active_job=%llu ui_active_worker_tid=%lu ui_tid=%lu ui_step_idx=%d ui_step=\"%.96s\" ui_last_lock_wait_ms=%llu ui_snapshot_lock_wait_ms=%llu ui_last_job_wait_ms=%llu ui_last_job_run_ms=%llu ui_last_pump_wall_ms=%llu ui_last_pump_seq=%llu ui_skipped_budget=%llu ui_skipped_no_job=%llu ui_lock_busy_total=%llu ui_steps_processed=%llu",
        pending,
        lock_busy ? 1 : 0,
        static_cast<unsigned long long>(g_ui_phase_active_job_id.load(std::memory_order_acquire)),
        static_cast<unsigned long>(g_ui_phase_active_worker_tid.load(std::memory_order_acquire)),
        static_cast<unsigned long>(g_ui_phase_thread_id.load(std::memory_order_acquire)),
        g_ui_phase_active_step_index.load(std::memory_order_acquire),
        step_name ? step_name : "<null>",
        static_cast<unsigned long long>(g_ui_phase_last_lock_wait_ms.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(lock_wait),
        static_cast<unsigned long long>(g_ui_phase_last_job_wait_ms.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_phase_last_job_run_ms.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_phase_last_pump_wall_ms.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_phase_last_pump_seq.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_phase_skipped_by_budget_count.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_phase_skipped_no_job_count.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_phase_lock_busy_count.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_ui_phase_steps_processed_total.load(std::memory_order_acquire)));
}

}
