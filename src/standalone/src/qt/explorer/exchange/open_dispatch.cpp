#include "qt/explorer/exchange/open_dispatch.hpp"

#include <QDialog>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLabel>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>
#include <system_error>

#include "core/analysis/workspace/byte_provider.hpp"
#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/analysis/workspace/zip_container.hpp"
#include "core/session/analysis_session.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/task_center.hpp"
#include "helpers/diag_log.hpp"
#include "helpers/globals.h"
#include "qt/bridge/aida_dialog.hpp"
#include "qt/documents/aida_document_controller.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/editor/aida_hex_document.hpp"
#include "qt/editor/aida_image_view.hpp"
#include "qt/explorer/aida_explorer_model.hpp"

namespace fs = std::filesystem;

namespace {

QString elide_path_middle(const QString& path, const QFont& font)
{
    const QFontMetricsF fm(font);
    return fm.elidedText(path, Qt::ElideMiddle, qRound(fm.averageCharWidth() * 72.0));
}

}

namespace aida::qt::explorer {

namespace ext_classify {

static const char* k_text_exts[] = {
    ".cpp", ".c", ".h", ".hpp", ".hxx", ".cxx", ".cc",
    ".py", ".js", ".ts", ".json", ".xml", ".yaml", ".yml",
    ".md", ".txt", ".log", ".cfg", ".ini", ".toml",
    ".java", ".cs", ".rs", ".go", ".rb", ".php",
    ".html", ".css", ".scss", ".lua", ".sh", ".bat", ".ps1",
    ".cmake", ".asm", ".s", ".inc", ".def", ".rules",
    ".vcxproj", ".vcproj", ".filters", ".props", ".targets",
    ".sln", ".csproj", ".proj", ".gradle", ".gn", ".gni",
    ".diff", ".patch", ".gitignore", ".gitattributes",
    ".srt", ".vtt", ".tsv", ".csv",
    ".env", ".rc", ".pbxproj", ".plist",
};

static const char* k_binary_exts[] = {
    ".exe", ".dll", ".sys", ".efi", ".scr", ".cpl",
    ".ocx", ".ax", ".drv", ".mui", ".tsp", ".node",
    ".bin", ".lib", ".obj", ".o", ".a", ".so", ".dylib",
    ".elf", ".out", ".com", ".ko", ".kext", ".dmp",
    ".pdb", ".rom", ".img", ".uefi",
    ".class", ".jar",
    ".pyc", ".pyo",
};

static const char* k_archive_exts[] = {
    ".rar", ".zip", ".7z", ".tar", ".gz", ".bz2", ".xz",
    ".cab", ".iso", ".apk", ".ipa", ".jar",
};

inline std::string lower_ext(const std::string& filename)
{
    std::string ext;
    const auto dot = filename.rfind('.');
    if (dot != std::string::npos)
        ext = filename.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

inline bool matches(const std::string& ext, const char* const* table, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        if (ext == table[i]) return true;
    return false;
}

inline bool is_text(const std::string& ext)
{
    return matches(ext, k_text_exts, sizeof(k_text_exts)/sizeof(k_text_exts[0]));
}

inline bool is_binary(const std::string& ext)
{
    return matches(ext, k_binary_exts, sizeof(k_binary_exts)/sizeof(k_binary_exts[0]));
}

inline bool is_archive(const std::string& ext)
{
    return matches(ext, k_archive_exts, sizeof(k_archive_exts)/sizeof(k_archive_exts[0]));
}

}

AidaOpenDispatch& AidaOpenDispatch::instance()
{
    static AidaOpenDispatch* dispatch = new AidaOpenDispatch();
    return *dispatch;
}

AidaOpenDispatch::AidaOpenDispatch(QObject* parent) : QObject(parent)
{
    confirm_poll_timer_ = new QTimer(this);
    confirm_poll_timer_->setInterval(250);
    connect(confirm_poll_timer_, &QTimer::timeout, this,
            &AidaOpenDispatch::onConfirmPollTimer);
    confirm_poll_timer_->start();
}

AidaOpenDispatch::~AidaOpenDispatch() = default;

void AidaOpenDispatch::setViewFocusHook(std::function<void(const std::string&)> hook)
{
    view_focus_hook_ = std::move(hook);
}

void AidaOpenDispatch::setDocumentController(documents::AidaDocumentController* controller) noexcept
{
    documents_ = controller;
}

void AidaOpenDispatch::setImageView(editor::AidaImageView* view) noexcept
{
    image_view_ = view;
}

void AidaOpenDispatch::setExplorerModel(AidaExplorerModel* model) noexcept
{
    explorer_ = model;
}

void AidaOpenDispatch::openPath(const std::string& path)
{
    if (path.empty())
        return;
    if (thread() && QThread::currentThread() != thread()) {
        const bool posted = QMetaObject::invokeMethod(this, [this, path] {
            openPath(path);
        }, Qt::QueuedConnection);
        if (!posted)
            diag::log_tagged_fmt("qt_open_dispatch", "open_path_dispatch_failed path=%s",
                path.c_str());
        return;
    }

    std::error_code ec;
    if (fs::is_directory(path, ec) && !ec) {
        diag::log_tagged_fmt("qt_open_dispatch", "open_path directory=%s", path.c_str());
        if (explorer_)
            explorer_->refresh(path);
        else
            file_browser::refresh(path);
        if (view_focus_hook_)
            view_focus_hook_("view.project_explorer");
        return;
    }

    std::string fname;
    {
        const std::size_t sl = path.find_last_of("/\\");
        fname = (sl != std::string::npos) ? path.substr(sl + 1) : path;
    }
    const std::string ext = ext_classify::lower_ext(fname);

    diag::log_tagged_fmt("qt_open_dispatch", "open_path begin path=%s ext=%s",
        path.c_str(), ext.c_str());

    if (!ext.empty() && editor::AidaImageView::isImageAdmission(
            QString::fromStdString(ext).mid(1))) {
        if (image_view_)
            image_view_->load(QString::fromStdString(path));
        if (view_focus_hook_)
            view_focus_hook_("document.image");
        diag::log_tagged_fmt("qt_open_dispatch", "open_path -> image_view path=%s", path.c_str());
        file_browser::record_recent_workspace(path);
        return;
    }

    if (ext_classify::is_text(ext)) {
        if (documents_ && documents_->openDocument(path, fname)) {
            if (view_focus_hook_)
                view_focus_hook_("document.code");
            diag::log_tagged_fmt("qt_open_dispatch", "open_path -> code_editor path=%s", path.c_str());
            file_browser::record_recent_workspace(path);
            return;
        }
        diag::log_tagged_fmt("qt_open_dispatch", "open_path text open_failed path=%s", path.c_str());
    }

    if (ext_classify::is_archive(ext)) {
        asyncHexFallback(path, true);
        diag::log_tagged_fmt("qt_open_dispatch", "open_path -> hex_view archive path=%s", path.c_str());
        return;
    }

    std::uint64_t file_size_bytes = 0;
    {
        std::error_code fec;
        const auto sz = fs::file_size(path, fec);
        if (!fec) file_size_bytes = static_cast<std::uint64_t>(sz);
    }
    diag::log_tagged_fmt("qt_open_dispatch", "open_path binary_branch path=%s ext=%s size=%llu",
        path.c_str(), ext.c_str(), static_cast<unsigned long long>(file_size_bytes));

    std::size_t existing_idx = static_cast<std::size_t>(-1);
    const bool found = analysis_session::find_session_by_path(path, &existing_idx);
    if (found) {
        if (analysis_session::switch_session(existing_idx)) {
            if (view_focus_hook_)
                view_focus_hook_("document.disassembly");
            file_browser::record_recent_workspace(path);
            diag::log_tagged_fmt("qt_open_dispatch", "open_path -> existing_session idx=%llu",
                static_cast<unsigned long long>(existing_idx));
            return;
        }
        diag::log_tagged_fmt("qt_open_dispatch", "open_path switch_existing_failed idx=%llu err=%s",
            static_cast<unsigned long long>(existing_idx),
            analysis_session::last_error() ? analysis_session::last_error() : "(null)");
    }

    if (analysis_session::session_count() >= analysis_session::kMaxSessions)
        analysis_session::prune_lru(analysis_session::kMaxSessions - 1);

    const bool started = analysis_session::open_session(path);
    if (started) {
        if (view_focus_hook_)
            view_focus_hook_("document.disassembly");
        file_browser::record_recent_workspace(path);
        diag::log_tagged_fmt("qt_open_dispatch", "open_path -> new_session path=%s ext=%s",
            path.c_str(), ext.c_str());
        return;
    }

    const char* err = analysis_session::last_error();
    const bool err_says_not_pe = err && (
        std::strstr(err, "not a PE") != nullptr ||
        std::strstr(err, "not_pe")   != nullptr ||
        std::strstr(err, "PE header") != nullptr ||
        std::strstr(err, "magic") != nullptr);

    if (ext_classify::is_binary(ext) || err_says_not_pe) {
        asyncHexFallback(path, false);
        diag::log_tagged_fmt("qt_open_dispatch", "open_path -> hex_view fallback path=%s err=%s",
            path.c_str(), err ? err : "(null)");
        return;
    }

    diag::log_tagged_fmt("qt_open_dispatch", "open_path failed path=%s err=%s", path.c_str(),
        err ? err : "(null)");
}

void AidaOpenDispatch::requestOpenConfirmation(const std::string& path)
{
    file_browser::request_open_confirmation(path);
}

void AidaOpenDispatch::onConfirmPollTimer()
{
    if (!file_browser::pending_open_modal_visible)
        return;
    if (file_browser::pending_open_path.empty())
        return;
    const std::string path = file_browser::pending_open_path;
    const std::string filename = file_browser::pending_open_filename;
    file_browser::pending_open_modal_visible = false;
    file_browser::pending_open_should_open = false;
    presentOpenConfirmation(path, filename);
}

void AidaOpenDispatch::presentOpenConfirmation(const std::string& path,
    const std::string& filename)
{
    std::size_t existing_idx = static_cast<std::size_t>(-1);
    const bool already_open = analysis_session::find_session_by_path(path, &existing_idx);

    auto* dialog = new bridge::AidaDialog(dialog_parent_);
    dialog->setWindowTitle(QStringLiteral("Load file?"));
    dialog->setObjectName(QStringLiteral("aida.open_binary_confirm"));
    dialog->setModal(true);
    auto* layout = new QVBoxLayout(dialog);
    auto* name_label = new QLabel(QString::fromStdString(filename), dialog);
    name_label->setObjectName(QStringLiteral("aida.open_binary_confirm.name"));
    name_label->setFont(theme::fonts::strong());
    layout->addWidget(name_label);
    const QString path_full = QString::fromStdString(path);
    auto* path_label = new QLabel(elide_path_middle(path_full, dialog->font()), dialog);
    path_label->setObjectName(QStringLiteral("aida.open_binary_confirm.path"));
    path_label->setProperty("aidaVariant", QStringLiteral("secondary"));
    path_label->setToolTip(path_full);
    path_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(path_label);
    auto* prompt_label = new QLabel(QStringLiteral("Do you want to load this file?"), dialog);
    prompt_label->setObjectName(QStringLiteral("aida.open_binary_confirm.prompt"));
    layout->addWidget(prompt_label);
    if (already_open) {
        auto* state_label = new QLabel(
            QStringLiteral("Already open in a tab — click 'Switch' to focus it."), dialog);
        state_label->setObjectName(QStringLiteral("aida.open_binary_confirm.state"));
        layout->addWidget(state_label);
    } else if (analysis_session::session_count() >= analysis_session::kMaxSessions) {
        auto* state_label = new QLabel(QStringLiteral(
            "Already at %1 open binaries. The oldest will be closed to make room.")
                .arg(static_cast<qulonglong>(analysis_session::kMaxSessions)), dialog);
        state_label->setObjectName(QStringLiteral("aida.open_binary_confirm.state"));
        layout->addWidget(state_label);
    }
    auto* buttons = new QHBoxLayout();
    auto* confirm = new widgets::AidaButton(
        QString::fromLatin1(already_open ? "Switch" : "Load"), dialog);
    confirm->setObjectName(QStringLiteral("aida.open_binary_confirm.confirm"));
    confirm->setKind(widgets::AidaButton::Kind::Primary);
    auto* cancel = new widgets::AidaButton(QStringLiteral("Cancel"), dialog);
    cancel->setObjectName(QStringLiteral("aida.open_binary_confirm.cancel"));
    buttons->addStretch(1);
    buttons->addWidget(confirm);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    connect(confirm, &QAbstractButton::clicked, this, [this, path, already_open, existing_idx, dialog] {
        if (already_open) {
            if (analysis_session::switch_session(existing_idx) && view_focus_hook_)
                view_focus_hook_("document.disassembly");
        } else {
            openPath(path);
        }
        dialog->accept();
    });
    connect(cancel, &QAbstractButton::clicked, dialog, &QDialog::reject);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
    Q_EMIT openConfirmationRequested(QString::fromStdString(path),
        QString::fromStdString(filename));
}

bool AidaOpenDispatch::hexFallbackLoading() const
{
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(hex_mutex_));
    return hex_loading_;
}

QString AidaOpenDispatch::hexFallbackPath() const
{
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(hex_mutex_));
    return QString::fromStdString(hex_path_);
}

QString AidaOpenDispatch::hexFallbackError() const
{
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(hex_mutex_));
    return QString::fromStdString(hex_error_);
}

void open_path(const std::string& path)
{
    AidaOpenDispatch::instance().openPath(path);
}

bool image_active()
{
    auto* view = AidaOpenDispatch::instance().imageView();
    return view && view->active();
}

struct HexPreviewOperation {
    std::mutex mutex;
    std::condition_variable completion;
    std::atomic<bool> terminal{false};
    std::atomic<bool> cancellation{false};
    std::shared_ptr<aida::analysis::cancellation_source_t> provider_cancellation;
    std::optional<aida::analysis::workspace_admission_handle_t> admission;
};

namespace {

bool claimTerminal(const std::shared_ptr<HexPreviewOperation>& operation)
{
    return operation && !operation->terminal.exchange(true, std::memory_order_acq_rel);
}

bool hasSuffix(const std::string& value, const char* suffix)
{
    const std::size_t suffix_size = std::strlen(suffix);
    return value.size() >= suffix_size && value.compare(value.size() - suffix_size,
        suffix_size, suffix) == 0;
}

int archiveMemberPriority(const aida::analysis::zip_member_t& member)
{
    if (member.kind != aida::analysis::zip_member_kind_t::regular_file ||
        member.uncompressed_size == 0)
        return (std::numeric_limits<int>::max)();
    std::string path = member.normalized_path;
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (path == "classes.dex" || hasSuffix(path, "/classes.dex"))
        return 0;
    if (hasSuffix(path, ".dex") || hasSuffix(path, ".odex") || hasSuffix(path, ".vdex"))
        return 1;
    if (hasSuffix(path, ".so") || hasSuffix(path, ".elf") || hasSuffix(path, ".exe") ||
        hasSuffix(path, ".dll") || hasSuffix(path, ".sys") || hasSuffix(path, ".dylib"))
        return 2;
    if (hasSuffix(path, ".o") || hasSuffix(path, ".obj") || hasSuffix(path, ".a") ||
        hasSuffix(path, ".lib") || hasSuffix(path, ".class"))
        return 3;
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (path.find(".app/") != std::string::npos &&
        (dot == std::string::npos || (slash != std::string::npos && dot < slash)))
        return 4;
    return (std::numeric_limits<int>::max)();
}

bool openArchiveMemberProvider(
    const std::shared_ptr<const aida::analysis::byte_provider_t>& root,
    std::shared_ptr<const aida::analysis::byte_provider_t>& member_provider,
    const aida::analysis::cancellation_token_t& cancel, std::string& error)
{
    auto archive = aida::analysis::zip_container_t::open(root, {}, cancel);
    if (!archive) {
        error = archive.error().stable_code() + ": " + archive.error().message;
        return false;
    }
    auto integrity = archive.value()->verify_integrity(cancel);
    if (!integrity) {
        error = integrity.error().stable_code() + ": " + integrity.error().message;
        return false;
    }
    const auto& members = archive.value()->members();
    std::size_t selected = members.size();
    int priority = (std::numeric_limits<int>::max)();
    for (std::size_t index = 0; index < members.size(); ++index) {
        const int candidate = archiveMemberPriority(members[index]);
        if (candidate < priority || (candidate == priority && selected < members.size() &&
            members[index].normalized_path < members[selected].normalized_path)) {
            priority = candidate;
            selected = index;
        }
    }
    if (selected == members.size() || priority == (std::numeric_limits<int>::max)()) {
        error = "UNSUPPORTED_FORMAT: archive has no supported static member for the hex workspace";
        return false;
    }
    auto opened = archive.value()->open_member_provider(selected, cancel);
    if (!opened) {
        error = opened.error().stable_code() + ": " + opened.error().message;
        return false;
    }
    member_provider = std::static_pointer_cast<const aida::analysis::byte_provider_t>(
        opened.take_value());
    return true;
}

}

void AidaOpenDispatch::completeHexFallbackSuccess(
    std::shared_ptr<HexPreviewOperation> operation, std::uint64_t serial, std::string path,
    aida::analysis::workspace_result_t<std::shared_ptr<aida::analysis::analysis_workspace_t>> result)
{
    if (!claimTerminal(operation))
        return;
    const std::string fallback_path = path;
    const bool posted = QMetaObject::invokeMethod(this, [this, serial, path = std::move(path),
            result = std::move(result)]() mutable {
        {
            std::lock_guard<std::mutex> lock(hex_mutex_);
            if (hex_serial_ != serial)
                return;
        }
        if (!result) {
            std::lock_guard<std::mutex> lock(hex_mutex_);
            if (hex_serial_ != serial)
                return;
            hex_loading_ = false;
            hex_cancellation_requested_ = false;
            hex_cancelled_ = false;
            hex_error_ = result.error().stable_code() + ": " + result.error().message;
            hex_task_ = {};
            hex_admission_.reset();
            Q_EMIT hexFallbackStateChanged();
            return;
        }
        auto workspace = result.take_value();
        const auto selected = aida::analysis::workspace_registry().select_for_ui(
            workspace->identity().binary_id());
        const auto context = disasm_view::capture_workspace(workspace);
        std::lock_guard<std::mutex> lock(hex_mutex_);
        if (hex_serial_ != serial)
            return;
        hex_loading_ = false;
        hex_cancellation_requested_ = false;
        hex_cancelled_ = false;
        hex_task_ = {};
        hex_admission_.reset();
        if (!selected || !context) {
            hex_error_ = !selected ? selected.error().stable_code() + ": " + selected.error().message :
                "TARGET_NOT_FOUND: admitted workspace context is unavailable";
            Q_EMIT hexFallbackStateChanged();
            return;
        }
        auto hex_document = editor::AidaHexDocumentRegistry::instance().stateFor(context);
        if (hex_document)
            hex_document->activate(context);
        if (view_focus_hook_)
            view_focus_hook_("document.hex");
        file_browser::record_recent_workspace(path);
        hex_error_.clear();
        diag::log_tagged_fmt("qt_open_dispatch", "hex_fallback_complete path=%s target=%s",
            path.c_str(), workspace->identity().binary_id().to_hex().c_str());
        Q_EMIT hexFallbackStateChanged();
    }, Qt::QueuedConnection);
    if (!posted) {
        completeHexFallbackFailure(serial, fallback_path,
            "SERVICE_CONFLICT: admitted workspace completion could not reach the UI owner");
        diag::log_tagged_fmt("qt_open_dispatch", "hex_fallback_ui_post_failed serial=%llu",
            static_cast<unsigned long long>(serial));
    }
    operation->completion.notify_all();
}

void AidaOpenDispatch::asyncHexFallback(const std::string& path, bool archive)
{
    std::optional<aida::analysis::workspace_admission_handle_t> previous;
    aida::infra::taskflow_runtime::job_handle_t previous_task;
    const auto operation = std::make_shared<HexPreviewOperation>();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    operation->provider_cancellation =
        std::make_shared<aida::analysis::cancellation_source_t>(deadline);
    std::uint64_t serial = 0;
    {
        std::lock_guard<std::mutex> lock(hex_mutex_);
        previous = std::move(hex_admission_);
        previous_task = hex_task_;
        serial = ++hex_serial_;
        hex_loading_ = true;
        hex_cancellation_requested_ = false;
        hex_cancelled_ = false;
        hex_archive_ = archive;
        hex_path_ = path;
        hex_error_.clear();
        hex_task_ = {};
    }
    Q_EMIT hexFallbackStateChanged();
    presentHexFallbackDialog();
    if (previous_task.valid())
        static_cast<void>(aida::infra::taskflow_runtime::cancel(previous_task));
    if (previous)
        aida::analysis::workspace_registry_t::cancel_admission(*previous);

    aida::infra::taskflow_runtime::task_descriptor_t task;
    task.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
    task.owner_subsystem = "file_browser";
    task.label = "file_browser.hex_provider_admission";
    task.thread_class = "bounded_provider_open";
    task.ui_access_policy = "none";
    task.failure_policy = "structured_completion";
    task.shutdown_policy = "cancel_and_drain";
    task.deadline_ms = 65000;
    task.cancel_hook = [operation, serial, path, this] {
        cancelHexFallbackOperation(operation, serial, path);
    };
    task.cancellable_body = [this, path, archive, serial, operation, deadline](
        const aida::infra::taskflow_runtime::cancellation_token_t& cancel) {
        if (cancel.requested.load(std::memory_order_acquire) || !hexFallbackIsCurrent(serial)) {
            cancelHexFallbackOperation(operation, serial, path);
            return;
        }
        auto mapped = aida::analysis::mapped_file_provider_t::open(path);
        if (!mapped) {
            finishHexFallbackFailure(operation, serial, path,
                mapped.error().stable_code() + ": " + mapped.error().message);
            return;
        }
        std::shared_ptr<const aida::analysis::byte_provider_t> provider =
            std::static_pointer_cast<const aida::analysis::byte_provider_t>(mapped.take_value());
        if (archive) {
            std::string member_error;
            std::shared_ptr<const aida::analysis::byte_provider_t> member;
            if (!openArchiveMemberProvider(provider, member,
                    operation->provider_cancellation->token(), member_error)) {
                finishHexFallbackFailure(operation, serial, path, std::move(member_error));
                return;
            }
            provider = std::move(member);
        }
        if (cancel.requested.load(std::memory_order_acquire) || !hexFallbackIsCurrent(serial)) {
            cancelHexFallbackOperation(operation, serial, path);
            return;
        }
        aida::analysis::baseline_analysis_settings_t settings;
        const std::string profile = "aida-pe-workspace-engine-1|" + settings.canonical_json();
        aida::analysis::open_provider_workspace_request_t request;
        request.provider = provider;
        request.bin_name = fs::path(path).filename().string();
        if (const auto& member = provider->member_metadata())
            request.member_metadata = *member;
        request.load_profile.assign(profile.begin(), profile.end());
        request.analysis_settings = settings;
        auto admitted = aida::analysis::workspace_registry().admit_verified_provider_async(
            std::move(request), [this, operation, serial, path](auto result) mutable {
                completeHexFallbackSuccess(operation, serial, path, std::move(result));
            }, deadline);
        if (!admitted) {
            finishHexFallbackFailure(operation, serial, path,
                admitted.error().stable_code() + ": " + admitted.error().message);
            return;
        }
        auto handle = admitted.take_value();
        {
            std::lock_guard<std::mutex> lock(operation->mutex);
            operation->admission = handle;
        }
        {
            std::lock_guard<std::mutex> lock(hex_mutex_);
            if (hex_serial_ == serial && !operation->terminal.load(std::memory_order_acquire))
                hex_admission_ = handle;
        }
        if (operation->terminal.load(std::memory_order_acquire)) {
            if (operation->cancellation.load(std::memory_order_acquire))
                aida::analysis::workspace_registry_t::cancel_admission(handle);
            return;
        }
        std::unique_lock<std::mutex> lock(operation->mutex);
        while (!operation->terminal.load(std::memory_order_acquire)) {
            if (cancel.requested.load(std::memory_order_acquire) ||
                !hexFallbackIsCurrent(serial)) {
                lock.unlock();
                cancelHexFallbackOperation(operation, serial, path);
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                lock.unlock();
                timeoutHexFallbackOperation(operation, serial, path);
                return;
            }
            operation->completion.wait_for(lock, std::chrono::milliseconds(50));
        }
    };
    const auto submitted = aida::infra::taskflow_runtime::submit(std::move(task));
    if (!submitted.submitted) {
        {
            std::lock_guard<std::mutex> lock(hex_mutex_);
            if (hex_serial_ == serial) {
                hex_loading_ = false;
                hex_cancellation_requested_ = false;
                hex_cancelled_ = false;
                hex_error_ = "SERVICE_CONFLICT: provider admission task was rejected: " +
                    submitted.reject_reason;
            }
        }
        operation->terminal.store(true, std::memory_order_release);
        operation->completion.notify_all();
        Q_EMIT hexFallbackStateChanged();
    } else {
        std::lock_guard<std::mutex> lock(hex_mutex_);
        if (hex_serial_ == serial && hex_loading_)
            hex_task_ = submitted.handle;
        else
            static_cast<void>(aida::infra::taskflow_runtime::cancel(submitted.handle));
    }
}

bool AidaOpenDispatch::hexFallbackIsCurrent(std::uint64_t serial)
{
    std::lock_guard<std::mutex> lock(hex_mutex_);
    return hex_serial_ == serial;
}

void AidaOpenDispatch::completeHexFallbackFailure(std::uint64_t serial, std::string path,
    std::string error)
{
    auto publish = [this, serial, path = std::move(path), error = std::move(error)]() mutable {
        std::lock_guard<std::mutex> lock(hex_mutex_);
        if (hex_serial_ != serial)
            return;
        hex_loading_ = false;
        hex_cancellation_requested_ = false;
        hex_cancelled_ = false;
        hex_path_ = std::move(path);
        hex_error_ = std::move(error);
        hex_task_ = {};
        hex_admission_.reset();
        Q_EMIT hexFallbackStateChanged();
    };
    if (!QMetaObject::invokeMethod(this, publish, Qt::QueuedConnection)) {
        publish();
        diag::log_tagged_fmt("qt_open_dispatch", "hex_fallback_ui_post_failed serial=%llu",
            static_cast<unsigned long long>(serial));
    }
}

void AidaOpenDispatch::completeHexFallbackCancelled(std::uint64_t serial, std::string path)
{
    auto publish = [this, serial, path = std::move(path)]() mutable {
        std::lock_guard<std::mutex> lock(hex_mutex_);
        if (hex_serial_ != serial)
            return;
        hex_loading_ = false;
        hex_cancellation_requested_ = false;
        hex_cancelled_ = true;
        hex_path_ = std::move(path);
        hex_error_.clear();
        hex_task_ = {};
        hex_admission_.reset();
        Q_EMIT hexFallbackStateChanged();
    };
    if (!QMetaObject::invokeMethod(this, publish, Qt::QueuedConnection)) {
        publish();
        diag::log_tagged_fmt("qt_open_dispatch", "hex_fallback_cancel_ui_post_failed serial=%llu",
            static_cast<unsigned long long>(serial));
    }
}

void AidaOpenDispatch::finishHexFallbackFailure(
    const std::shared_ptr<HexPreviewOperation>& operation, std::uint64_t serial,
    const std::string& path, std::string error)
{
    if (!claimTerminal(operation))
        return;
    completeHexFallbackFailure(serial, path, std::move(error));
    operation->completion.notify_all();
}

void AidaOpenDispatch::cancelHexFallbackOperation(
    const std::shared_ptr<HexPreviewOperation>& operation, std::uint64_t serial,
    const std::string& path)
{
    if (!claimTerminal(operation))
        return;
    operation->cancellation.store(true, std::memory_order_release);
    std::optional<aida::analysis::workspace_admission_handle_t> admission;
    std::shared_ptr<aida::analysis::cancellation_source_t> provider_cancellation;
    {
        std::lock_guard<std::mutex> lock(operation->mutex);
        admission = operation->admission;
        provider_cancellation = operation->provider_cancellation;
    }
    if (provider_cancellation)
        provider_cancellation->request_cancel();
    if (admission)
        aida::analysis::workspace_registry_t::cancel_admission(*admission);
    completeHexFallbackCancelled(serial, path);
    operation->completion.notify_all();
}

void AidaOpenDispatch::timeoutHexFallbackOperation(
    const std::shared_ptr<HexPreviewOperation>& operation, std::uint64_t serial,
    const std::string& path)
{
    if (!claimTerminal(operation))
        return;
    std::optional<aida::analysis::workspace_admission_handle_t> admission;
    std::shared_ptr<aida::analysis::cancellation_source_t> provider_cancellation;
    {
        std::lock_guard<std::mutex> lock(operation->mutex);
        admission = operation->admission;
        provider_cancellation = operation->provider_cancellation;
    }
    if (provider_cancellation)
        provider_cancellation->request_cancel();
    if (admission)
        aida::analysis::workspace_registry_t::cancel_admission(*admission);
    completeHexFallbackFailure(serial, path,
        "TIMEOUT: provider admission did not complete within 60 seconds");
    operation->completion.notify_all();
}

void AidaOpenDispatch::presentHexFallbackDialog()
{
    auto* dialog = new bridge::AidaDialog(dialog_parent_);
    dialog->setWindowTitle(QStringLiteral("Open as Hex"));
    dialog->setObjectName(QStringLiteral("aida.hex_fallback_operation"));
    auto* layout = new QVBoxLayout(dialog);
    auto* title = new QLabel(dialog);
    title->setObjectName(QStringLiteral("aida.hex_fallback_operation.title"));
    title->setFont(theme::fonts::strong());
    auto* message = new QLabel(dialog);
    message->setObjectName(QStringLiteral("aida.hex_fallback_operation.message"));
    message->setWordWrap(true);
    auto* target = new QLabel(dialog);
    target->setObjectName(QStringLiteral("aida.hex_fallback_operation.path"));
    target->setProperty("aidaVariant", QStringLiteral("secondary"));
    target->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* hint = new QLabel(dialog);
    hint->setObjectName(QStringLiteral("aida.hex_fallback_operation.hint"));
    hint->setWordWrap(true);
    hint->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(title);
    layout->addWidget(message);
    layout->addWidget(target);
    layout->addWidget(hint);
    auto* buttons = new QHBoxLayout();
    auto* primary = new widgets::AidaButton(dialog);
    primary->setObjectName(QStringLiteral("aida.hex_fallback_operation.primary"));
    auto* secondary = new widgets::AidaButton(QStringLiteral("Dismiss"), dialog);
    secondary->setObjectName(QStringLiteral("aida.hex_fallback_operation.secondary"));
    buttons->addStretch(1);
    buttons->addWidget(primary);
    buttons->addWidget(secondary);
    layout->addLayout(buttons);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    const auto refresh = [this, dialog, title, message, target, hint, primary, secondary] {
        const bool loading = hexFallbackLoading();
        const bool terminal = !loading;
        const QString path = hexFallbackPath();
        const QString error = hexFallbackError();
        bool cancelled = false;
        bool cancellation_requested = false;
        {
            std::lock_guard<std::mutex> lock(hex_mutex_);
            cancelled = hex_cancelled_;
            cancellation_requested = hex_cancellation_requested_;
        }
        if (loading) {
            title->setText(cancellation_requested
                ? QStringLiteral("Cancelling file open") : QStringLiteral("Opening file as Hex"));
            message->setText(cancellation_requested
                ? QStringLiteral("Cancellation was requested. The current provider admission is being stopped.")
                : QStringLiteral("AiDA is validating and admitting a bounded read-only provider for this file."));
            hint->setText(QStringLiteral("The operation is owned by Task Center and can be cancelled safely."));
        } else if (cancelled) {
            title->setText(QStringLiteral("File open cancelled"));
            message->setText(QStringLiteral("No workspace state was changed."));
            hint->setText(QStringLiteral("Retry the exact file or dismiss this result and continue using the IDE."));
        } else {
            title->setText(QStringLiteral("Hex fallback unavailable"));
            message->setText(error);
            hint->setText(QStringLiteral("Retry the exact file or dismiss this result and continue using the IDE."));
        }
        target->setText(elide_path_middle(path, target->font()));
        target->setToolTip(path);
        primary->setText(terminal ? QStringLiteral("Retry") : QStringLiteral("Cancel Operation"));
        primary->setEnabled(terminal || !cancellation_requested);
        secondary->setVisible(terminal);
        secondary->setEnabled(terminal);
        dialog->adjustSize();
    };
    connect(primary, &QAbstractButton::clicked, this, [this, dialog] {
        if (hexFallbackLoading())
            cancelHexFallback();
        else
            retryHexFallback();
        if (!hexFallbackLoading())
            dialog->accept();
    });
    connect(secondary, &QAbstractButton::clicked, this, [this, dialog] {
        dismissHexFallback();
        dialog->accept();
    });
    connect(this, &AidaOpenDispatch::hexFallbackStateChanged, dialog, [refresh] {
        refresh();
    });
    refresh();
    dialog->open();
}

void AidaOpenDispatch::cancelHexFallback()
{
    aida::infra::taskflow_runtime::job_handle_t task;
    std::shared_ptr<HexPreviewOperation> operation;
    std::optional<aida::analysis::workspace_admission_handle_t> admission;
    std::uint64_t serial = 0;
    {
        std::lock_guard<std::mutex> lock(hex_mutex_);
        if (hex_loading_ && !hex_cancellation_requested_) {
            hex_cancellation_requested_ = true;
            serial = hex_serial_;
            task = hex_task_;
            admission = hex_admission_;
        }
    }
    if (task.valid())
        static_cast<void>(aida::infra::taskflow_runtime::cancel(task));
    if (admission)
        aida::analysis::workspace_registry_t::cancel_admission(*admission);
}

void AidaOpenDispatch::retryHexFallback()
{
    std::string path;
    bool archive = false;
    {
        std::lock_guard<std::mutex> lock(hex_mutex_);
        path = hex_path_;
        archive = hex_archive_;
    }
    if (!path.empty())
        asyncHexFallback(path, archive);
}

void AidaOpenDispatch::dismissHexFallback()
{
    aida::infra::taskflow_runtime::job_handle_t task;
    std::optional<aida::analysis::workspace_admission_handle_t> admission;
    {
        std::lock_guard<std::mutex> lock(hex_mutex_);
        ++hex_serial_;
        task = hex_task_;
        admission = hex_admission_;
        hex_loading_ = false;
        hex_cancellation_requested_ = false;
        hex_cancelled_ = false;
        hex_archive_ = false;
        hex_path_.clear();
        hex_error_.clear();
        hex_task_ = {};
        hex_admission_.reset();
    }
    if (task.valid())
        static_cast<void>(aida::infra::taskflow_runtime::cancel(task));
    if (admission)
        aida::analysis::workspace_registry_t::cancel_admission(*admission);
    Q_EMIT hexFallbackStateChanged();
}

}
