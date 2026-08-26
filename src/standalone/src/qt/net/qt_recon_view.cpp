#include "qt/net/qt_recon_view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedLayout>
#include <QTabBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <limits>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include "core/infra/executor.hpp"
#include "core/network/network_view.hpp"
#include "core/ui/task_center.hpp"
#include "core/ui/toast_notification.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/aida_dialog.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

std::vector<std::string> splitCsv(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = s.c_str(); *p; ++p) {
        if (*p == ',') {
            while (!cur.empty() && (cur.front() == ' ' || cur.front() == '\t'))
                cur.erase(cur.begin());
            while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t'))
                cur.pop_back();
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(*p);
        }
    }
    while (!cur.empty() && (cur.front() == ' ' || cur.front() == '\t'))
        cur.erase(cur.begin());
    while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t'))
        cur.pop_back();
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::vector<int> splitCsvInts(const std::string& s)
{
    std::vector<int> out;
    std::string cur;
    for (const char* p = s.c_str(); ; ++p) {
        if (*p == ',' || *p == '\0') {
            try { if (!cur.empty()) out.push_back(std::stoi(cur)); } catch (...) {}
            cur.clear();
            if (*p == '\0')
                break;
        } else if (*p != ' ' && *p != '\t') {
            cur.push_back(*p);
        }
    }
    return out;
}

const char* crawlPhaseLabel(aida::burp::crawler::crawl_status_phase_t phase)
{
    switch (phase) {
    case aida::burp::crawler::crawl_status_phase_t::pending:  return "pending";
    case aida::burp::crawler::crawl_status_phase_t::running:  return "running";
    case aida::burp::crawler::crawl_status_phase_t::stopping: return "stopping";
    case aida::burp::crawler::crawl_status_phase_t::complete: return "complete";
    case aida::burp::crawler::crawl_status_phase_t::error:    return "error";
    }
    return "?";
}

const char* discPhaseLabel(aida::burp::content_discovery::disc_phase_t phase)
{
    switch (phase) {
    case aida::burp::content_discovery::disc_phase_t::pending:     return "pending";
    case aida::burp::content_discovery::disc_phase_t::calibrating: return "calibrating";
    case aida::burp::content_discovery::disc_phase_t::running:     return "running";
    case aida::burp::content_discovery::disc_phase_t::stopping:    return "stopping";
    case aida::burp::content_discovery::disc_phase_t::complete:    return "complete";
    case aida::burp::content_discovery::disc_phase_t::error:       return "error";
    }
    return "?";
}

const char* subPhaseLabel(aida::burp::subdomain_enum::enum_phase_t phase)
{
    switch (phase) {
    case aida::burp::subdomain_enum::enum_phase_t::pending:  return "pending";
    case aida::burp::subdomain_enum::enum_phase_t::passive:  return "passive";
    case aida::burp::subdomain_enum::enum_phase_t::brute:    return "brute";
    case aida::burp::subdomain_enum::enum_phase_t::stopping: return "stopping";
    case aida::burp::subdomain_enum::enum_phase_t::complete: return "complete";
    case aida::burp::subdomain_enum::enum_phase_t::error:    return "error";
    }
    return "?";
}

std::string win32ErrorText(const char* operation, DWORD error)
{
    return std::string(operation) + " failed with Win32 error " + std::to_string(error);
}

std::filesystem::path subdomainExportPath(std::uint64_t runId)
{
    PWSTR known = nullptr;
    std::filesystem::path directory;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &known)) && known)
        directory.assign(known);
    if (known)
        CoTaskMemFree(known);
    if (directory.empty())
        directory = L"C:\\Users\\Public\\Downloads";
    return directory / (L"subdomains_" + std::to_wstring(runId) + L".csv");
}

bool writeExportAtomically(const std::filesystem::path& destination,
                           std::string_view bytes, std::string& error)
{
    if (destination.empty()) {
        error = "The export destination is empty";
        return false;
    }
    std::error_code filesystemError;
    const auto directory = destination.parent_path();
    if (!directory.empty()) {
        std::filesystem::create_directories(directory, filesystemError);
        if (filesystemError) {
            error = "Creating the export directory failed: " + filesystemError.message();
            return false;
        }
    }
    static std::atomic<std::uint64_t> sequence{1};
    const auto temporary = std::filesystem::path(destination.wstring() + L".tmp." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetCurrentThreadId()) + L"." +
        std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)));
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = win32ErrorText("Creating the export temporary file", GetLastError());
        return false;
    }
    bool succeeded = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr)) {
            error = win32ErrorText("Writing the export temporary file", GetLastError());
            succeeded = false;
            break;
        }
        if (written != chunk) {
            error = "Writing the export temporary file completed with a short write";
            succeeded = false;
            break;
        }
        offset += written;
    }
    if (succeeded && !FlushFileBuffers(file)) {
        error = win32ErrorText("Flushing the export temporary file", GetLastError());
        succeeded = false;
    }
    LARGE_INTEGER size{};
    if (succeeded && (!GetFileSizeEx(file, &size) ||
        size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) != bytes.size())) {
        error = "The export temporary file size did not match the requested payload";
        succeeded = false;
    }
    if (!CloseHandle(file) && succeeded) {
        error = win32ErrorText("Closing the export temporary file", GetLastError());
        succeeded = false;
    }
    if (succeeded && !MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = win32ErrorText("Replacing the export destination", GetLastError());
        succeeded = false;
    }
    if (!succeeded)
        std::filesystem::remove(temporary, filesystemError);
    return succeeded;
}

QLabel* formLabel(QWidget* parent, const QString& text)
{
    auto* label = new QLabel(text, parent);
    label->setProperty("aidaTone", QStringLiteral("secondary"));
    return label;
}

}

QtReconRunModel::QtReconRunModel(Domain domain, QObject* parent)
    : QAbstractTableModel(parent), domain_(domain) {}

void QtReconRunModel::adopt(std::vector<QtReconRunRow> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const QtReconRunRow* QtReconRunModel::rowAt(int row) const noexcept
{
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

int QtReconRunModel::rowForId(std::uint64_t id) const noexcept
{
    for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
        if (rows_[static_cast<std::size_t>(row)].id == id)
            return row;
    }
    return -1;
}

int QtReconRunModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int QtReconRunModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    switch (domain_) {
    case Domain::Crawler:          return 6;
    case Domain::ContentDiscovery: return 7;
    case Domain::Subdomains:       return 5;
    }
    return 0;
}

QVariant QtReconRunModel::data(const QModelIndex& index, int role) const
{
    const auto* row = rowAt(index.isValid() ? index.row() : -1);
    if (!row)
        return {};
    if (role == Qt::DisplayRole) {
        const QString id = QString::number(static_cast<quint64>(row->id));
        switch (domain_) {
        case Domain::Crawler:
            switch (index.column()) {
            case 0: return id;
            case 1: return row->phase;
            case 2: return row->c0;
            case 3: return row->c1;
            case 4: return row->c2;
            case 5: return row->c3;
            }
            break;
        case Domain::ContentDiscovery:
            switch (index.column()) {
            case 0: return id;
            case 1: return row->phase;
            case 2: return row->c0;
            case 3: return row->c1;
            case 4: return row->c2;
            case 5: return row->c3;
            case 6: return row->c4;
            }
            break;
        case Domain::Subdomains:
            switch (index.column()) {
            case 0: return id;
            case 1: return row->phase;
            case 2: return row->c0;
            case 3: return row->c1;
            case 4: return row->c2;
            }
            break;
        }
    }
    if (role == Qt::ForegroundRole)
        return theme::tokens().text_secondary;
    return {};
}

QVariant QtReconRunModel::headerData(int section, Qt::Orientation orientation,
                                     int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (domain_) {
    case Domain::Crawler:
        switch (section) {
        case 0: return QStringLiteral("ID");
        case 1: return QStringLiteral("Phase");
        case 2: return QStringLiteral("Queue");
        case 3: return QStringLiteral("Visited");
        case 4: return QStringLiteral("Failed");
        case 5: return QStringLiteral("Found");
        }
        break;
    case Domain::ContentDiscovery:
        switch (section) {
        case 0: return QStringLiteral("ID");
        case 1: return QStringLiteral("Phase");
        case 2: return QStringLiteral("Attempts");
        case 3: return QStringLiteral("Total");
        case 4: return QStringLiteral("Hits");
        case 5: return QStringLiteral("Errors");
        case 6: return QStringLiteral("Filtered");
        }
        break;
    case Domain::Subdomains:
        switch (section) {
        case 0: return QStringLiteral("ID");
        case 1: return QStringLiteral("Phase");
        case 2: return QStringLiteral("Passive");
        case 3: return QStringLiteral("Brute Tried");
        case 4: return QStringLiteral("Brute Hit");
        }
        break;
    }
    return {};
}

QtReconResultModel::QtReconResultModel(Domain domain, QObject* parent)
    : QAbstractTableModel(parent), domain_(domain) {}

void QtReconResultModel::adopt(std::vector<QtReconResultRow> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

int QtReconResultModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int QtReconResultModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return domain_ == Domain::SubdomainResults ? 4 : 5;
}

QVariant QtReconResultModel::data(const QModelIndex& index, int role) const
{
    if (index.row() < 0 || index.row() >= static_cast<int>(rows_.size()))
        return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole) {
        const QString cols[5] = { row.c0, row.c1, row.c2, row.c3, row.c4 };
        if (index.column() >= 0 && index.column() < 5)
            return cols[index.column()];
    }
    if (role == Qt::ForegroundRole)
        return theme::tokens().text_secondary;
    if (role == Qt::ToolTipRole)
        return domain_ == Domain::SubdomainResults ? row.c0 : row.c4;
    return {};
}

QVariant QtReconResultModel::headerData(int section, Qt::Orientation orientation,
                                        int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (domain_) {
    case Domain::CrawlerUrls:
        switch (section) {
        case 0: return QStringLiteral("Status");
        case 1: return QStringLiteral("Bytes");
        case 2: return QStringLiteral("Depth");
        case 3: return QStringLiteral("Content-Type");
        case 4: return QStringLiteral("URL");
        }
        break;
    case Domain::ContentHits:
        switch (section) {
        case 0: return QStringLiteral("Status");
        case 1: return QStringLiteral("Bytes");
        case 2: return QStringLiteral("Latency");
        case 3: return QStringLiteral("Payload");
        case 4: return QStringLiteral("URL");
        }
        break;
    case Domain::SubdomainResults:
        switch (section) {
        case 0: return QStringLiteral("FQDN");
        case 1: return QStringLiteral("Resolves");
        case 2: return QStringLiteral("IPs");
        case 3: return QStringLiteral("Sources");
        }
        break;
    }
    return {};
}

QtPayloadSetModel::QtPayloadSetModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtPayloadSetModel::adopt(
    std::shared_ptr<const std::vector<aida::burp::payloads::payload_set_t>> sets)
{
    sets_ = std::move(sets);
    setFilter(filter_);
}

void QtPayloadSetModel::setFilter(const QString& needle)
{
    filter_ = needle;
    beginResetModel();
    visible_.clear();
    const QString lowered = needle.toLower();
    for (int i = 0; i < static_cast<int>(sets_->size()); ++i) {
        const auto& set = sets_->at(static_cast<std::size_t>(i));
        if (!lowered.isEmpty() &&
            !QString::fromStdString(set.id).toLower().contains(lowered))
            continue;
        visible_.push_back(i);
    }
    endResetModel();
}

const aida::burp::payloads::payload_set_t* QtPayloadSetModel::rowAt(int row) const noexcept
{
    if (row < 0 || row >= static_cast<int>(visible_.size()))
        return nullptr;
    return &sets_->at(static_cast<std::size_t>(visible_[static_cast<std::size_t>(row)]));
}

int QtPayloadSetModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

int QtPayloadSetModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 1;
}

QVariant QtPayloadSetModel::data(const QModelIndex& index, int role) const
{
    const auto* row = rowAt(index.isValid() ? index.row() : -1);
    if (!row)
        return {};
    if (role == Qt::DisplayRole) {
        return QStringLiteral("%1%2")
            .arg(QString::fromStdString(row->id))
            .arg(row->builtin ? QStringLiteral("  [builtin]")
                              : QStringLiteral("  [custom]"));
    }
    if (role == Qt::ForegroundRole)
        return theme::tokens().text_secondary;
    if (role == Qt::ToolTipRole)
        return QString::fromStdString(row->id);
    return {};
}

QtPayloadEntryModel::QtPayloadEntryModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtPayloadEntryModel::adopt(std::vector<std::string> entries)
{
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
}

int QtPayloadEntryModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

int QtPayloadEntryModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 1;
}

QVariant QtPayloadEntryModel::data(const QModelIndex& index, int role) const
{
    if (index.row() < 0 || index.row() >= static_cast<int>(entries_.size()))
        return {};
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
        return QString::fromStdString(entries_[static_cast<std::size_t>(index.row())]);
    if (role == Qt::ForegroundRole)
        return theme::tokens().text_secondary;
    return {};
}

QtReconController::QtReconController(QObject* parent)
    : QObject(parent) {}

std::shared_ptr<const std::vector<aida::burp::crawler::crawl_status_t>>
QtReconController::crawlerRuns() const
{
    return std::atomic_load_explicit(&crawler_runs_, std::memory_order_acquire);
}

std::shared_ptr<const std::vector<aida::burp::content_discovery::disc_status_t>>
QtReconController::discoveryRuns() const
{
    return std::atomic_load_explicit(&discovery_runs_, std::memory_order_acquire);
}

std::shared_ptr<const std::vector<aida::burp::subdomain_enum::enum_status_t>>
QtReconController::subdomainRuns() const
{
    return std::atomic_load_explicit(&subdomain_runs_, std::memory_order_acquire);
}

std::shared_ptr<const std::vector<aida::burp::payloads::payload_set_t>>
QtReconController::payloadSets() const
{
    return std::atomic_load_explicit(&payload_sets_, std::memory_order_acquire);
}

std::uint64_t QtReconController::takeStartedRun(int& domain) noexcept
{
    domain = started_run_domain_.exchange(0, std::memory_order_acq_rel);
    return started_run_id_.exchange(0, std::memory_order_acq_rel);
}

bool QtReconController::initialize()
{
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true))
        return true;
    ::diag::log_tagged("recon_v", "initialize");
    const bool payloadsReady = aida::burp::payloads::initialize();
    const bool crawlerReady = aida::burp::crawler::initialize();
    const bool discoveryReady = aida::burp::content_discovery::initialize();
    const bool subdomainReady = aida::burp::subdomain_enum::initialize();
    const bool ready = payloadsReady && crawlerReady && discoveryReady && subdomainReady;
    if (!ready)
        initialized_.store(false, std::memory_order_release);
    return ready;
}

void QtReconController::shutdown()
{
    if (!initialized_.exchange(false))
        return;
    ::diag::log_tagged("recon_v", "shutdown");
    aida::burp::crawler::shutdown();
    aida::burp::content_discovery::shutdown();
    aida::burp::subdomain_enum::shutdown();
    aida::burp::payloads::shutdown();
}

bool QtReconController::submitOperation(std::string action, std::string label,
    std::string target, std::function<aida::burp::ui_operation::result_t()> execute)
{
    aida::burp::ui_operation::request_t request;
    request.owner = "burp.recon";
    request.owner_view = "view.network.recon";
    request.owner_action = std::move(action);
    request.label = std::move(label);
    request.target = std::move(target);
    request.affected_entity = request.target;
    request.execute = std::move(execute);
    return operation_.submit(std::move(request));
}

void QtReconController::submitInitialization()
{
    bool expected = false;
    if (!initialization_requested_.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel))
        return;
    if (!submitOperation("network.recon.initialize", "Load Recon state",
        "Recon catalogs", [this]() {
            aida::burp::ui_operation::result_t result;
            result.success = initialize();
            result.message = result.success ? "Recon state loaded."
                                            : "Recon initialization failed.";
            return result;
        }))
        initialization_requested_.store(false, std::memory_order_release);
}

void QtReconController::submitCrawlerStart(const QString& seedText, int maxDepth,
    int maxPages, int concurrency, int ratePerHost, bool sameHost, bool scopeOnly,
    bool respectRobots, bool parseJs, const QString& userAgent,
    const QString& excludeExtensions)
{
    aida::burp::crawler::crawl_config_t config;
    config.start_urls = splitCsv(seedText.toStdString());
    config.max_depth = (std::max)(0, maxDepth);
    config.same_host_only = sameHost;
    config.scope_only = scopeOnly;
    config.respect_robots_txt = respectRobots;
    config.parse_js = parseJs;
    config.max_pages = (std::max)(1, maxPages);
    config.concurrency = (std::max)(1, concurrency);
    config.rate_per_host = (std::max)(1, ratePerHost);
    config.user_agent = userAgent.toStdString();
    config.exclude_extensions = splitCsv(excludeExtensions.toStdString());
    const std::string target = seedText.toStdString();
    submitOperation("network.recon.crawler.start", "Start crawl", target,
        [this, config = std::move(config)]() {
            aida::burp::ui_operation::result_t result;
            const std::uint64_t id = aida::burp::crawler::start(config);
            result.success = id != 0;
            result.message = result.success ? "Crawl started."
                                            : aida::burp::crawler::last_error();
            if (id != 0) {
                started_run_domain_.store(1, std::memory_order_release);
                started_run_id_.store(id, std::memory_order_release);
            }
            return result;
        });
}

void QtReconController::submitDiscoveryStart(const QString& target,
    const QString& wordlistId, const QString& extensions, int concurrency, int delayMs,
    const QString& matchStatus, const QString& filterStatus, bool recurse, int recurseDepth,
    bool autoCalibrate, bool followRedirects, const QString& cookie,
    const QString& userAgent)
{
    aida::burp::content_discovery::config_t config;
    config.target_url = target.toStdString();
    config.wordlist_id = wordlistId.toStdString();
    config.extensions = splitCsv(extensions.toStdString());
    config.concurrency = (std::max)(1, concurrency);
    config.delay_ms = (std::max)(0, delayMs);
    config.match_status = splitCsvInts(matchStatus.toStdString());
    config.filter_status = splitCsvInts(filterStatus.toStdString());
    config.recurse = recurse;
    config.recurse_depth = (std::max)(1, recurseDepth);
    config.auto_calibrate = autoCalibrate;
    config.follow_redirects = followRedirects;
    config.cookie_header = cookie.toStdString();
    config.user_agent = userAgent.toStdString();
    submitOperation("network.recon.discovery.start", "Start content discovery",
        config.target_url, [this, config = std::move(config)]() {
            aida::burp::ui_operation::result_t result;
            const std::uint64_t id = aida::burp::content_discovery::start(config);
            result.success = id != 0;
            result.message = result.success ? "Content discovery started."
                                            : aida::burp::content_discovery::last_error();
            if (id != 0) {
                started_run_domain_.store(2, std::memory_order_release);
                started_run_id_.store(id, std::memory_order_release);
            }
            return result;
        });
}

void QtReconController::submitSubdomainStart(const QString& domain,
    const QString& wordlistId, int concurrency, bool runPassive, bool runBrute,
    bool passiveCrtsh, bool passiveBufferover, bool passiveHackertarget)
{
    aida::burp::subdomain_enum::config_t config;
    config.domain = domain.toStdString();
    config.brute_wordlist_id = wordlistId.toStdString();
    config.run_passive = runPassive;
    config.run_brute = runBrute;
    config.resolver_concurrency = (std::max)(1, concurrency);
    if (passiveCrtsh) config.passive_sources.push_back("crt.sh");
    if (passiveBufferover) config.passive_sources.push_back("bufferover");
    if (passiveHackertarget) config.passive_sources.push_back("hackertarget");
    submitOperation("network.recon.subdomain.start", "Start subdomain enumeration",
        config.domain, [this, config = std::move(config)]() {
            aida::burp::ui_operation::result_t result;
            const std::uint64_t id = aida::burp::subdomain_enum::start(config);
            result.success = id != 0;
            result.message = result.success ? "Subdomain enumeration started."
                                            : aida::burp::subdomain_enum::last_error();
            if (id != 0) {
                started_run_domain_.store(3, std::memory_order_release);
                started_run_id_.store(id, std::memory_order_release);
            }
            return result;
        });
}

void QtReconController::submitStop(int domain, std::uint64_t id, std::uint64_t startedMs)
{
    submitOperation("network.recon.stop", "Stop Recon run",
        "Run " + std::to_string(id), [domain, id, startedMs]() {
            aida::burp::ui_operation::result_t result;
            bool identityMatches = false;
            if (domain == 1)
                identityMatches = aida::burp::crawler::status(id).started_unix_ms == startedMs;
            else if (domain == 2)
                identityMatches =
                    aida::burp::content_discovery::status(id).started_unix_ms == startedMs;
            else if (domain == 3)
                identityMatches =
                    aida::burp::subdomain_enum::status(id).started_unix_ms == startedMs;
            if (!identityMatches) {
                result.message = "The Recon run changed before stop; no run was stopped.";
                return result;
            }
            result.success = domain == 1 ? aida::burp::crawler::stop(id)
                : domain == 2 ? aida::burp::content_discovery::stop(id)
                              : aida::burp::subdomain_enum::stop(id);
            result.message = result.success ? "Recon run stop requested."
                                            : "Recon run stop failed.";
            return result;
        });
}

bool QtReconController::submitReviewedRemove(int domain, std::uint64_t id,
                                             std::uint64_t startedMs)
{
    return submitOperation("network.recon.remove", "Remove Recon run",
        "Run " + std::to_string(id), [domain, id, startedMs]() {
            aida::burp::ui_operation::result_t result;
            bool identityMatches = false;
            if (domain == 1)
                identityMatches = aida::burp::crawler::status(id).started_unix_ms == startedMs;
            else if (domain == 2)
                identityMatches =
                    aida::burp::content_discovery::status(id).started_unix_ms == startedMs;
            else if (domain == 3)
                identityMatches =
                    aida::burp::subdomain_enum::status(id).started_unix_ms == startedMs;
            if (!identityMatches) {
                result.message = "The Recon run changed after review; no run was removed.";
                return result;
            }
            result.success = domain == 1 ? aida::burp::crawler::remove(id)
                : domain == 2 ? aida::burp::content_discovery::remove(id)
                              : aida::burp::subdomain_enum::remove(id);
            result.message = result.success ? "Recon run removed."
                                            : "Recon run removal failed.";
            return result;
        });
}

bool QtReconController::submitPayloadAdd(QString id, QString label, QString description,
                                         std::vector<std::string> entries)
{
    const bool submitted = submitOperation("network.recon.payload.add", "Add payload set",
        id.toStdString(),
        [id = id.toStdString(), label = label.toStdString(),
         description = description.toStdString(), entries = std::move(entries)]() {
            aida::burp::ui_operation::result_t result;
            result.success = aida::burp::payloads::add_custom_set(id, label, description,
                entries);
            result.message = result.success ? "Payload set added."
                                            : aida::burp::payloads::last_error();
            return result;
        });
    if (submitted)
        clear_payload_inputs_after_success_ = true;
    return submitted;
}

bool QtReconController::submitPayloadRemove(QString id, bool reviewedBuiltin)
{
    const std::string idStd = id.toStdString();
    reviewed_payload_id_ = id;
    reviewed_payload_builtin_ = reviewedBuiltin;
    return submitOperation("network.recon.payload.remove", "Remove payload set", idStd,
        [id = std::move(idStd), reviewedBuiltin]() {
            aida::burp::ui_operation::result_t result;
            const auto summaries = aida::burp::payloads::list_summaries();
            const auto found = std::find_if(summaries.begin(), summaries.end(),
                [&id](const auto& set) { return set.id == id; });
            if (found == summaries.end() || found->builtin != reviewedBuiltin ||
                found->builtin) {
                result.message = "The payload set changed after review; no set was removed.";
                return result;
            }
            result.success = aida::burp::payloads::remove_custom_set(id);
            result.message = result.success ? "Payload set removed."
                                            : aida::burp::payloads::last_error();
            return result;
        });
}

bool QtReconController::queueSubdomainExport(std::uint64_t runId)
{
    bool expected = false;
    if (!sub_export_pending_.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel))
        return false;
    static std::atomic<std::uint64_t> sequence{1};
    const std::string taskId = "network.recon.subdomain_export." +
        std::to_string(runId) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    aida::ui::task_center::task_registration_t registration;
    registration.id = taskId;
    registration.source = "network.recon";
    registration.owner = "Recon";
    registration.owner_view = "view.network.recon";
    registration.owner_action = "network.recon.export_subdomains";
    registration.target = "subdomain run " + std::to_string(runId);
    registration.label = "Export subdomain results";
    registration.stage = "Queued for bounded snapshot and atomic export";
    registration.affected_entity = registration.target;
    registration.callbacks.focus = [] {
        static_cast<void>(network_view::open_view("view.network.recon"));
    };
    registration.callbacks.open_log = registration.callbacks.focus;
    if (!aida::ui::task_center::register_task(std::move(registration))) {
        sub_export_pending_.store(false, std::memory_order_release);
        return false;
    }
    QPointer<QtReconController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network_recon";
    submission.label = "network.recon.export_subdomains";
    submission.thread_class = "bounded_file_io";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.session_id = taskId.c_str();
    submission.target_id = taskId.c_str();
    submission.diagnostic_id = taskId.c_str();
    submission.ui_access_policy = "immutable_snapshots_only";
    submission.failure_policy = "typed_diagnostic";
    submission.shutdown_policy = "drain";
    submission.body = [runId, taskId, pending = &sub_export_pending_] {
        auto finishPending = std::unique_ptr<void, void(*)(void*)>(
            static_cast<void*>(pending), [](void* opaque) {
                auto* flag = static_cast<std::atomic<bool>*>(opaque);
                flag->store(false, std::memory_order_release);
            });
        try {
            static_cast<void>(aida::ui::task_center::update_task(taskId,
                aida::ui::task_center::task_state_t::running, -1.0f,
                "Creating a bounded immutable CSV snapshot"));
            std::string csv = aida::burp::subdomain_enum::export_csv(runId);
            constexpr std::size_t maximumExportBytes = 64U * 1024U * 1024U;
            if (csv.size() > maximumExportBytes) {
                static_cast<void>(aida::ui::task_center::update_task(taskId,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "CSV snapshot exceeded the export limit",
                    "The generated CSV exceeded the 64 MiB bounded export limit",
                    "diagnostic." + taskId));
                return;
            }
            const auto destination = subdomainExportPath(runId);
            static_cast<void>(aida::ui::task_center::update_task(taskId,
                aida::ui::task_center::task_state_t::running, -1.0f,
                "Writing a same-directory temporary file"));
            std::string error;
            if (!writeExportAtomically(destination, csv, error)) {
                static_cast<void>(aida::ui::task_center::update_task(taskId,
                    aida::ui::task_center::task_state_t::failed, 1.0f,
                    "Atomic CSV export failed", error, "diagnostic." + taskId));
                return;
            }
            ::diag::log_tagged_fmt("recon_v", "sub_enum_csv_exported run=%llu bytes=%zu",
                static_cast<unsigned long long>(runId), csv.size());
            static_cast<void>(aida::ui::task_center::update_task(taskId,
                aida::ui::task_center::task_state_t::completed, 1.0f,
                "Finished", "Subdomain CSV exported atomically to " +
                    destination.u8string()));
        } catch (const std::exception& exception) {
            static_cast<void>(aida::ui::task_center::update_task(taskId,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "CSV export failed", exception.what(), "diagnostic." + taskId));
        } catch (...) {
            static_cast<void>(aida::ui::task_center::update_task(taskId,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "CSV export failed", "Unknown export failure", "diagnostic." + taskId));
        }
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        sub_export_pending_.store(false, std::memory_order_release);
        static_cast<void>(aida::ui::task_center::update_task(taskId,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Executor rejected CSV export", submitted.reject_reason,
            "diagnostic." + taskId));
        return false;
    }
    return true;
}

void QtReconController::requestRunRefresh()
{
    bool expected = false;
    if (!refresh_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    QPointer<QtReconController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.recon";
    submission.label = "recon.refresh_runs";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 4;
    submission.body = [guard]() {
        std::shared_ptr<const std::vector<aida::burp::crawler::crawl_status_t>> crawls =
            std::make_shared<const std::vector<aida::burp::crawler::crawl_status_t>>(
                aida::burp::crawler::list());
        std::shared_ptr<const std::vector<aida::burp::content_discovery::disc_status_t>> discoveries =
            std::make_shared<const std::vector<aida::burp::content_discovery::disc_status_t>>(
                aida::burp::content_discovery::list());
        std::shared_ptr<const std::vector<aida::burp::subdomain_enum::enum_status_t>> subdomains =
            std::make_shared<const std::vector<aida::burp::subdomain_enum::enum_status_t>>(
                aida::burp::subdomain_enum::list());
        auto payloadValues = aida::burp::payloads::list_summaries();
        for (auto& set : payloadValues)
            set.entries = aida::burp::payloads::entries(set.id);
        std::shared_ptr<const std::vector<aida::burp::payloads::payload_set_t>> payloadSets =
            std::make_shared<const std::vector<aida::burp::payloads::payload_set_t>>(
                std::move(payloadValues));
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(),
            [guard, crawls = std::move(crawls), discoveries = std::move(discoveries),
             subdomains = std::move(subdomains),
             payloadSets = std::move(payloadSets)]() mutable {
                auto* self = guard.data();
                if (!self)
                    return;
                std::atomic_store_explicit(&self->crawler_runs_, std::move(crawls),
                    std::memory_order_release);
                std::atomic_store_explicit(&self->discovery_runs_, std::move(discoveries),
                    std::memory_order_release);
                std::atomic_store_explicit(&self->subdomain_runs_, std::move(subdomains),
                    std::memory_order_release);
                std::atomic_store_explicit(&self->payload_sets_, std::move(payloadSets),
                    std::memory_order_release);
                self->refresh_pending_.store(false, std::memory_order_release);
                Q_EMIT self->runsChanged();
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted)
        refresh_pending_.store(false, std::memory_order_release);
}

QtReconView::QtReconView(QWidget* parent)
    : NetworkPaneBase(parent)
{
    setObjectName(QStringLiteral("view.network.recon"));
    setRequiresTarget(false);

    const auto& t = theme::tokens();
    controller_ = new QtReconController(this);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.xs);

    initStack_ = new QStackedLayout();
    initStack_->setStackingMode(QStackedLayout::StackOne);
    loadingHost_ = new QWidget(content);
    auto* loadingLayout = new QVBoxLayout(loadingHost_);
    loadingLayout->setContentsMargins(0, 0, 0, 0);
    loadingLayout->setSpacing(t.spacing.xs);
    loadingLabel_ = new QLabel(loadingHost_);
    loadingLabel_->setProperty("aidaTone", QStringLiteral("secondary"));
    loadingLayout->addWidget(loadingLabel_);
    initRetryButton_ = new widgets::AidaButton(QStringLiteral("Retry"), loadingHost_);
    initRetryButton_->setKind(widgets::AidaButton::Kind::Secondary);
    initRetryButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    connect(initRetryButton_, &QAbstractButton::clicked, this,
        [this] { controller_->submitInitialization(); });
    loadingLayout->addWidget(initRetryButton_);
    loadingLayout->addStretch(1);
    initStack_->addWidget(loadingHost_);

    tabsHost_ = new QWidget(content);
    auto* tabsLayout = new QVBoxLayout(tabsHost_);
    tabsLayout->setContentsMargins(0, 0, 0, 0);
    tabsLayout->setSpacing(t.spacing.xs);
    operationLabel_ = new QLabel(tabsHost_);
    operationLabel_->setWordWrap(true);
    tabsLayout->addWidget(operationLabel_);
    retryButton_ = new widgets::AidaButton(QStringLiteral("Retry operation"), tabsHost_);
    retryButton_->setKind(widgets::AidaButton::Kind::Secondary);
    retryButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    retryButton_->setVisible(false);
    connect(retryButton_, &QAbstractButton::clicked, this, [this] {
        static_cast<void>(controller_->operation().retry());
    });
    tabsLayout->addWidget(retryButton_);

    tabBar_ = new QTabBar(tabsHost_);
    tabBar_->setObjectName(QStringLiteral("view.network.recon.tabs"));
    tabBar_->addTab(QStringLiteral("Crawler"));
    tabBar_->addTab(QStringLiteral("Content Discovery"));
    tabBar_->addTab(QStringLiteral("Subdomains"));
    tabBar_->addTab(QStringLiteral("Payload Library"));
    tabsLayout->addWidget(tabBar_);
    stack_ = new QStackedLayout();
    stack_->setStackingMode(QStackedLayout::StackOne);
    stack_->addWidget(buildCrawlerTab(tabsHost_));
    stack_->addWidget(buildDiscoveryTab(tabsHost_));
    stack_->addWidget(buildSubdomainsTab(tabsHost_));
    stack_->addWidget(buildPayloadsTab(tabsHost_));
    tabsLayout->addLayout(stack_, 1);
    initStack_->addWidget(tabsHost_);
    layout->addLayout(initStack_, 1);

    connect(tabBar_, &QTabBar::currentChanged, this, [this](int index) {
        static const char* k_names[] = { "Crawler", "Content Discovery", "Subdomains",
            "Payload Library" };
        if (index >= 0 && index < 4)
            ::diag::log_tagged_fmt("recon_v", "tab_switch tab=%s", k_names[index]);
        stack_->setCurrentIndex(index);
    });

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(200);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        if (controller_->initialized())
            controller_->requestRunRefresh();
    });
    completionTimer_ = new QTimer(this);
    completionTimer_->setInterval(100);
    connect(completionTimer_, &QTimer::timeout, this, &QtReconView::observeCompletion);
    connect(controller_, &QtReconController::runsChanged, this, &QtReconView::refreshRuns);

    setContent(content);
    observeCompletion();
    refreshRuns();
}

QtReconView::~QtReconView() = default;

void QtReconView::onPaneShown()
{
    if (!init_kicked_) {
        init_kicked_ = true;
        controller_->submitInitialization();
    }
    controller_->requestRunRefresh();
    refreshTimer_->start();
    completionTimer_->start();
}

void QtReconView::onPaneHidden()
{
    refreshTimer_->stop();
    completionTimer_->stop();
}

void QtReconView::observeCompletion()
{
    const auto completion = controller_->operation().completion();
    const bool pending = controller_->operation().pending();
    const bool initialized = controller_->initialized();

    initStack_->setCurrentIndex(initialized ? 1 : 0);
    if (!initialized) {
        if (pending) {
            loadingLabel_->setText(QStringLiteral("Loading Recon state..."));
            initRetryButton_->setVisible(false);
        } else {
            loadingLabel_->setText(completion
                ? QString::fromStdString(completion->result.message)
                : QStringLiteral("Recon initialization is unavailable."));
            initRetryButton_->setVisible(true);
        }
    }

    if (!completion || completion->generation <= controller_->observedOperationGeneration())
        return;
    controller_->setObservedOperationGeneration(completion->generation);
    controller_->resetInitializationRequested();

    int startedDomain = 0;
    const std::uint64_t startedId = controller_->takeStartedRun(startedDomain);
    if (startedId != 0) {
        if (startedDomain == 1) crawler_selected_ = startedId;
        else if (startedDomain == 2) disc_selected_ = startedId;
        else if (startedDomain == 3) sub_selected_ = startedId;
    }
    if (controller_->clearPayloadInputsAfterSuccess()) {
        if (completion->result.success) {
            payloadNewIdEdit_->clear();
            payloadNewLabelEdit_->clear();
            payloadNewDescEdit_->clear();
            payloadNewEntriesEdit_->clear();
        }
        controller_->clearPayloadInputsConsumed();
    }
    if (controller_->awaitingRemoveCompletion()) {
        if (completion->result.success) {
            if (controller_->reviewDomain() == 1 &&
                crawler_selected_ == controller_->reviewedId())
                crawler_selected_ = 0;
            else if (controller_->reviewDomain() == 2 &&
                     disc_selected_ == controller_->reviewedId())
                disc_selected_ = 0;
            else if (controller_->reviewDomain() == 3 &&
                     sub_selected_ == controller_->reviewedId())
                sub_selected_ = 0;
        }
        controller_->setAwaitingRemoveCompletion(false);
    }
    if (controller_->awaitingPayloadRemoveCompletion()) {
        if (completion->result.success &&
            controller_->reviewedPayloadId().toStdString() == payload_selected_id_)
            payload_selected_id_.clear();
        controller_->setAwaitingPayloadRemoveCompletion(false);
    }
    controller_->requestRunRefresh();

    operationLabel_->setText(QString::fromStdString(completion->result.message));
    set_label_tone(operationLabel_,
        completion->result.success ? "success" : "error");
    retryButton_->setVisible(!completion->result.success && !pending);
}

namespace {

QTableView* makeRunTable(QWidget* parent, QAbstractTableModel* model, int height)
{
    const auto& t = theme::tokens();
    auto* view = new QTableView(parent);
    view->verticalHeader()->hide();
    view->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setStretchLastSection(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setAlternatingRowColors(true);
    view->setMinimumHeight(height);
    view->setMaximumHeight(height + 60);
    view->setModel(model);
    return view;
}

widgets::AidaStateView* makeTableEmptyState(QTableView* view, const QString& objectName,
    const QString& title, const QString& message)
{
    auto* state = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        title, message, view->parentWidget());
    state->setObjectName(objectName);
    state->setMinimumHeight(view->minimumHeight());
    if (view->maximumHeight() != QWIDGETSIZE_MAX)
        state->setMaximumHeight(view->maximumHeight());
    state->setVisible(false);
    return state;
}

void wireTableEmptyState(QTableView* view, QAbstractTableModel* model,
                         widgets::AidaStateView* empty)
{
    QObject::connect(model, &QAbstractItemModel::modelReset, empty,
        [view, model, empty] {
            const bool hasRows = model->rowCount() > 0;
            view->setVisible(hasRows);
            empty->setVisible(!hasRows);
        });
    view->setVisible(false);
    empty->setVisible(true);
}

}

QWidget* QtReconView::buildCrawlerTab(QWidget* parent)
{
    const auto& t = theme::tokens();
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, t.spacing.xs, 0, 0);
    layout->setSpacing(t.spacing.xs);

    layout->addWidget(formLabel(page, QStringLiteral("Seed URLs (comma-separated):")));
    crawlerSeedEdit_ = new QLineEdit(QStringLiteral("https://example.com/"), page);
    crawlerSeedEdit_->setObjectName(QStringLiteral("view.network.recon.crawler.seed"));
    crawlerSeedEdit_->setMaxLength(2047);
    crawlerSeedEdit_->setPlaceholderText(
        QStringLiteral("https://target/, https://target/api"));
    layout->addWidget(crawlerSeedEdit_);

    auto* numbersRow = new QHBoxLayout();
    numbersRow->setSpacing(t.spacing.xs);
    numbersRow->addWidget(formLabel(page, QStringLiteral("Max depth:")));
    crawlerDepthSpin_ = new QSpinBox(page);
    crawlerDepthSpin_->setRange(0, 64);
    crawlerDepthSpin_->setValue(3);
    numbersRow->addWidget(crawlerDepthSpin_);
    numbersRow->addWidget(formLabel(page, QStringLiteral("Max pages:")));
    crawlerPagesSpin_ = new QSpinBox(page);
    crawlerPagesSpin_->setRange(1, 1000000);
    crawlerPagesSpin_->setValue(500);
    numbersRow->addWidget(crawlerPagesSpin_);
    numbersRow->addWidget(formLabel(page, QStringLiteral("Concurrency:")));
    crawlerConcurrencySpin_ = new QSpinBox(page);
    crawlerConcurrencySpin_->setRange(1, 128);
    crawlerConcurrencySpin_->setValue(8);
    numbersRow->addWidget(crawlerConcurrencySpin_);
    numbersRow->addWidget(formLabel(page, QStringLiteral("RPS/host:")));
    crawlerRateSpin_ = new QSpinBox(page);
    crawlerRateSpin_->setRange(1, 10000);
    crawlerRateSpin_->setValue(10);
    numbersRow->addWidget(crawlerRateSpin_);
    numbersRow->addStretch(1);
    layout->addLayout(numbersRow);

    auto* togglesRow = new QHBoxLayout();
    togglesRow->setSpacing(t.spacing.sm);
    crawlerSameHostCheck_ = new QCheckBox(QStringLiteral("Same host only"), page);
    crawlerSameHostCheck_->setChecked(true);
    togglesRow->addWidget(crawlerSameHostCheck_);
    crawlerScopeOnlyCheck_ = new QCheckBox(QStringLiteral("Scope only"), page);
    togglesRow->addWidget(crawlerScopeOnlyCheck_);
    crawlerRobotsCheck_ = new QCheckBox(QStringLiteral("robots.txt"), page);
    crawlerRobotsCheck_->setChecked(true);
    togglesRow->addWidget(crawlerRobotsCheck_);
    crawlerParseJsCheck_ = new QCheckBox(QStringLiteral("Parse JS"), page);
    crawlerParseJsCheck_->setChecked(true);
    togglesRow->addWidget(crawlerParseJsCheck_);
    togglesRow->addStretch(1);
    layout->addLayout(togglesRow);

    auto* uaRow = new QHBoxLayout();
    uaRow->setSpacing(t.spacing.xs);
    uaRow->addWidget(formLabel(page, QStringLiteral("User-Agent:")));
    crawlerUserAgentEdit_ = new QLineEdit(QStringLiteral("AiDA-Crawler/1.0"), page);
    crawlerUserAgentEdit_->setMaxLength(255);
    uaRow->addWidget(crawlerUserAgentEdit_, 1);
    uaRow->addWidget(formLabel(page, QStringLiteral("Exclude ext:")));
    crawlerExcludeEdit_ = new QLineEdit(QStringLiteral(
        ".png,.jpg,.jpeg,.gif,.svg,.ico,.woff,.woff2,.ttf,.eot,.css,.mp4,.webm,.mp3,.pdf"),
        page);
    crawlerExcludeEdit_->setMaxLength(511);
    uaRow->addWidget(crawlerExcludeEdit_, 1);
    layout->addLayout(uaRow);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(t.spacing.xs);
    crawlerStartButton_ = new widgets::AidaButton(QStringLiteral("Start Crawl"), page);
    crawlerStartButton_->setObjectName(QStringLiteral("view.network.recon.crawler.start"));
    crawlerStartButton_->setKind(widgets::AidaButton::Kind::Primary);
    crawlerStartButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(crawlerStartButton_);
    crawlerStopButton_ = new widgets::AidaButton(QStringLiteral("Stop Selected"), page);
    crawlerStopButton_->setKind(widgets::AidaButton::Kind::Destructive);
    crawlerStopButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(crawlerStopButton_);
    crawlerRemoveButton_ = new widgets::AidaButton(QStringLiteral("Remove Selected"), page);
    crawlerRemoveButton_->setKind(widgets::AidaButton::Kind::Secondary);
    crawlerRemoveButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(crawlerRemoveButton_);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    crawlerRunModel_ = new QtReconRunModel(QtReconRunModel::Domain::Crawler, page);
    crawlerRunsView_ = makeRunTable(page, crawlerRunModel_, 220);
    crawlerRunsView_->setObjectName(QStringLiteral("view.network.recon.crawler.runs"));
    layout->addWidget(crawlerRunsView_);
    crawlerRunsEmpty_ = makeTableEmptyState(crawlerRunsView_,
        QStringLiteral("view.network.recon.crawler.runs.empty"),
        QStringLiteral("No crawl runs"),
        QStringLiteral("Start a crawl above to populate the run history."));
    layout->addWidget(crawlerRunsEmpty_);
    wireTableEmptyState(crawlerRunsView_, crawlerRunModel_, crawlerRunsEmpty_);

    crawlerDetailLabel_ = new QLabel(page);
    crawlerDetailLabel_->setProperty("aidaTone", QStringLiteral("accent"));
    layout->addWidget(crawlerDetailLabel_);
    crawlerDetailModel_ = new QtReconResultModel(QtReconResultModel::Domain::CrawlerUrls,
        page);
    crawlerDetailView_ = makeRunTable(page, crawlerDetailModel_, 220);
    crawlerDetailView_->setObjectName(QStringLiteral("view.network.recon.crawler.urls"));
    layout->addWidget(crawlerDetailView_, 1);
    crawlerDetailEmpty_ = makeTableEmptyState(crawlerDetailView_,
        QStringLiteral("view.network.recon.crawler.urls.empty"),
        QStringLiteral("No crawl selected"),
        QStringLiteral("Select a run above to inspect discovered URLs."));
    layout->addWidget(crawlerDetailEmpty_, 1);
    wireTableEmptyState(crawlerDetailView_, crawlerDetailModel_, crawlerDetailEmpty_);

    connect(crawlerStartButton_, &QAbstractButton::clicked, this, [this] {
        controller_->submitCrawlerStart(crawlerSeedEdit_->text(),
            crawlerDepthSpin_->value(), crawlerPagesSpin_->value(),
            crawlerConcurrencySpin_->value(), crawlerRateSpin_->value(),
            crawlerSameHostCheck_->isChecked(), crawlerScopeOnlyCheck_->isChecked(),
            crawlerRobotsCheck_->isChecked(), crawlerParseJsCheck_->isChecked(),
            crawlerUserAgentEdit_->text(), crawlerExcludeEdit_->text());
    });
    connect(crawlerStopButton_, &QAbstractButton::clicked, this, [this] {
        if (crawler_selected_ != 0) {
            const auto crawls = controller_->crawlerRuns();
            const int row = crawlerRunModel_->rowForId(crawler_selected_);
            if (row >= 0 && row < static_cast<int>(crawls->size()))
                controller_->submitStop(1, crawler_selected_,
                    (*crawls)[static_cast<std::size_t>(row)].started_unix_ms);
        }
    });
    connect(crawlerRemoveButton_, &QAbstractButton::clicked, this, [this] {
        if (crawler_selected_ != 0) {
            const auto crawls = controller_->crawlerRuns();
            const int row = crawlerRunModel_->rowForId(crawler_selected_);
            if (row >= 0 && row < static_cast<int>(crawls->size()))
                presentRemoveReview(1, crawler_selected_,
                    (*crawls)[static_cast<std::size_t>(row)].started_unix_ms);
        }
    });
    connect(crawlerRunsView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* row = crawlerRunModel_->rowAt(current.isValid() ? current.row() : -1);
            crawler_selected_ = row ? row->id : 0;
            refreshCrawlerDetail();
        });
    return page;
}

QWidget* QtReconView::buildDiscoveryTab(QWidget* parent)
{
    const auto& t = theme::tokens();
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, t.spacing.xs, 0, 0);
    layout->setSpacing(t.spacing.xs);

    layout->addWidget(formLabel(page, QStringLiteral("Target URL (use FUZZ marker):")));
    discTargetEdit_ = new QLineEdit(QStringLiteral("https://example.com/FUZZ"), page);
    discTargetEdit_->setObjectName(QStringLiteral("view.network.recon.discovery.target"));
    discTargetEdit_->setMaxLength(1023);
    discTargetEdit_->setPlaceholderText(QStringLiteral("https://target/FUZZ"));
    layout->addWidget(discTargetEdit_);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(t.spacing.xs);
    row1->addWidget(formLabel(page, QStringLiteral("Wordlist id:")));
    discWordlistEdit_ = new QLineEdit(QStringLiteral("dirs/common-100"), page);
    discWordlistEdit_->setMaxLength(127);
    discWordlistEdit_->setPlaceholderText(QStringLiteral("dirs/common-100"));
    row1->addWidget(discWordlistEdit_, 1);
    row1->addWidget(formLabel(page, QStringLiteral("Extensions:")));
    discExtensionsEdit_ = new QLineEdit(page);
    discExtensionsEdit_->setMaxLength(255);
    discExtensionsEdit_->setPlaceholderText(QStringLiteral(".php,.bak,.zip"));
    row1->addWidget(discExtensionsEdit_, 1);
    layout->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    row2->setSpacing(t.spacing.xs);
    row2->addWidget(formLabel(page, QStringLiteral("Match status:")));
    discMatchStatusEdit_ = new QLineEdit(QStringLiteral("200,201,204,301,302,401,403,500"),
        page);
    discMatchStatusEdit_->setMaxLength(127);
    row2->addWidget(discMatchStatusEdit_, 1);
    row2->addWidget(formLabel(page, QStringLiteral("Filter status:")));
    discFilterStatusEdit_ = new QLineEdit(QStringLiteral("404"), page);
    discFilterStatusEdit_->setMaxLength(127);
    row2->addWidget(discFilterStatusEdit_, 1);
    layout->addLayout(row2);

    auto* row3 = new QHBoxLayout();
    row3->setSpacing(t.spacing.xs);
    row3->addWidget(formLabel(page, QStringLiteral("Concurrency:")));
    discConcurrencySpin_ = new QSpinBox(page);
    discConcurrencySpin_->setRange(1, 256);
    discConcurrencySpin_->setValue(25);
    row3->addWidget(discConcurrencySpin_);
    row3->addWidget(formLabel(page, QStringLiteral("Delay ms:")));
    discDelaySpin_ = new QSpinBox(page);
    discDelaySpin_->setRange(0, 60000);
    discDelaySpin_->setValue(0);
    row3->addWidget(discDelaySpin_);
    discRecurseCheck_ = new QCheckBox(QStringLiteral("Recurse"), page);
    row3->addWidget(discRecurseCheck_);
    row3->addWidget(formLabel(page, QStringLiteral("Depth:")));
    discRecurseDepthSpin_ = new QSpinBox(page);
    discRecurseDepthSpin_->setRange(1, 16);
    discRecurseDepthSpin_->setValue(1);
    row3->addWidget(discRecurseDepthSpin_);
    discAutoCalibrateCheck_ = new QCheckBox(QStringLiteral("Auto-calibrate"), page);
    discAutoCalibrateCheck_->setChecked(true);
    row3->addWidget(discAutoCalibrateCheck_);
    discFollowRedirectCheck_ = new QCheckBox(QStringLiteral("Follow redir"), page);
    row3->addWidget(discFollowRedirectCheck_);
    row3->addStretch(1);
    layout->addLayout(row3);

    layout->addWidget(formLabel(page, QStringLiteral("Cookie:")));
    discCookieEdit_ = new QLineEdit(page);
    discCookieEdit_->setMaxLength(1023);
    discCookieEdit_->setPlaceholderText(QStringLiteral("session=..."));
    layout->addWidget(discCookieEdit_);
    layout->addWidget(formLabel(page, QStringLiteral("User-Agent:")));
    discUserAgentEdit_ = new QLineEdit(QStringLiteral("AiDA-ContentDiscovery/1.0"), page);
    discUserAgentEdit_->setMaxLength(255);
    layout->addWidget(discUserAgentEdit_);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(t.spacing.xs);
    discStartButton_ = new widgets::AidaButton(QStringLiteral("Start"), page);
    discStartButton_->setObjectName(QStringLiteral("view.network.recon.discovery.start"));
    discStartButton_->setKind(widgets::AidaButton::Kind::Primary);
    discStartButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(discStartButton_);
    discStopButton_ = new widgets::AidaButton(QStringLiteral("Stop Selected"), page);
    discStopButton_->setKind(widgets::AidaButton::Kind::Destructive);
    discStopButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(discStopButton_);
    discRemoveButton_ = new widgets::AidaButton(QStringLiteral("Remove Selected"), page);
    discRemoveButton_->setKind(widgets::AidaButton::Kind::Secondary);
    discRemoveButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(discRemoveButton_);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    discRunModel_ = new QtReconRunModel(QtReconRunModel::Domain::ContentDiscovery, page);
    discRunsView_ = makeRunTable(page, discRunModel_, 200);
    discRunsView_->setObjectName(QStringLiteral("view.network.recon.discovery.runs"));
    layout->addWidget(discRunsView_);
    discRunsEmpty_ = makeTableEmptyState(discRunsView_,
        QStringLiteral("view.network.recon.discovery.runs.empty"),
        QStringLiteral("No discovery runs"),
        QStringLiteral("Start content discovery above to populate the run history."));
    layout->addWidget(discRunsEmpty_);
    wireTableEmptyState(discRunsView_, discRunModel_, discRunsEmpty_);

    discDetailLabel_ = new QLabel(page);
    discDetailLabel_->setProperty("aidaTone", QStringLiteral("accent"));
    layout->addWidget(discDetailLabel_);
    discDetailModel_ = new QtReconResultModel(QtReconResultModel::Domain::ContentHits, page);
    discDetailView_ = makeRunTable(page, discDetailModel_, 220);
    discDetailView_->setObjectName(QStringLiteral("view.network.recon.discovery.hits"));
    layout->addWidget(discDetailView_, 1);
    discDetailEmpty_ = makeTableEmptyState(discDetailView_,
        QStringLiteral("view.network.recon.discovery.hits.empty"),
        QStringLiteral("No discovery run selected"),
        QStringLiteral("Select a run above to inspect content hits."));
    layout->addWidget(discDetailEmpty_, 1);
    wireTableEmptyState(discDetailView_, discDetailModel_, discDetailEmpty_);

    connect(discStartButton_, &QAbstractButton::clicked, this, [this] {
        controller_->submitDiscoveryStart(discTargetEdit_->text(),
            discWordlistEdit_->text(), discExtensionsEdit_->text(),
            discConcurrencySpin_->value(), discDelaySpin_->value(),
            discMatchStatusEdit_->text(), discFilterStatusEdit_->text(),
            discRecurseCheck_->isChecked(), discRecurseDepthSpin_->value(),
            discAutoCalibrateCheck_->isChecked(), discFollowRedirectCheck_->isChecked(),
            discCookieEdit_->text(), discUserAgentEdit_->text());
    });
    connect(discStopButton_, &QAbstractButton::clicked, this, [this] {
        if (disc_selected_ != 0) {
            const auto runs = controller_->discoveryRuns();
            const int row = discRunModel_->rowForId(disc_selected_);
            if (row >= 0 && row < static_cast<int>(runs->size()))
                controller_->submitStop(2, disc_selected_,
                    (*runs)[static_cast<std::size_t>(row)].started_unix_ms);
        }
    });
    connect(discRemoveButton_, &QAbstractButton::clicked, this, [this] {
        if (disc_selected_ != 0) {
            const auto runs = controller_->discoveryRuns();
            const int row = discRunModel_->rowForId(disc_selected_);
            if (row >= 0 && row < static_cast<int>(runs->size()))
                presentRemoveReview(2, disc_selected_,
                    (*runs)[static_cast<std::size_t>(row)].started_unix_ms);
        }
    });
    connect(discRunsView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* row = discRunModel_->rowAt(current.isValid() ? current.row() : -1);
            disc_selected_ = row ? row->id : 0;
            refreshDiscoveryDetail();
        });
    return page;
}

QWidget* QtReconView::buildSubdomainsTab(QWidget* parent)
{
    const auto& t = theme::tokens();
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, t.spacing.xs, 0, 0);
    layout->setSpacing(t.spacing.xs);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(t.spacing.xs);
    row1->addWidget(formLabel(page, QStringLiteral("Domain:")));
    subDomainEdit_ = new QLineEdit(QStringLiteral("example.com"), page);
    subDomainEdit_->setObjectName(QStringLiteral("view.network.recon.subdomains.domain"));
    subDomainEdit_->setMaxLength(255);
    subDomainEdit_->setPlaceholderText(QStringLiteral("example.com"));
    row1->addWidget(subDomainEdit_, 1);
    row1->addWidget(formLabel(page, QStringLiteral("Brute wordlist:")));
    subWordlistEdit_ = new QLineEdit(QStringLiteral("subdomains/top1000"), page);
    subWordlistEdit_->setMaxLength(127);
    row1->addWidget(subWordlistEdit_, 1);
    row1->addWidget(formLabel(page, QStringLiteral("Concurrency:")));
    subConcurrencySpin_ = new QSpinBox(page);
    subConcurrencySpin_->setRange(1, 256);
    subConcurrencySpin_->setValue(32);
    row1->addWidget(subConcurrencySpin_);
    row1->addStretch(1);
    layout->addLayout(row1);

    auto* togglesRow = new QHBoxLayout();
    togglesRow->setSpacing(t.spacing.sm);
    subPassiveCheck_ = new QCheckBox(QStringLiteral("Passive"), page);
    subPassiveCheck_->setChecked(true);
    togglesRow->addWidget(subPassiveCheck_);
    subBruteCheck_ = new QCheckBox(QStringLiteral("Brute"), page);
    subBruteCheck_->setChecked(true);
    togglesRow->addWidget(subBruteCheck_);
    subCrtshCheck_ = new QCheckBox(QStringLiteral("crt.sh"), page);
    subCrtshCheck_->setChecked(true);
    togglesRow->addWidget(subCrtshCheck_);
    subBufferoverCheck_ = new QCheckBox(QStringLiteral("bufferover"), page);
    subBufferoverCheck_->setChecked(true);
    togglesRow->addWidget(subBufferoverCheck_);
    subHackertargetCheck_ = new QCheckBox(QStringLiteral("hackertarget"), page);
    subHackertargetCheck_->setChecked(true);
    togglesRow->addWidget(subHackertargetCheck_);
    togglesRow->addStretch(1);
    layout->addLayout(togglesRow);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(t.spacing.xs);
    subStartButton_ = new widgets::AidaButton(QStringLiteral("Start Enum"), page);
    subStartButton_->setObjectName(QStringLiteral("view.network.recon.subdomains.start"));
    subStartButton_->setKind(widgets::AidaButton::Kind::Primary);
    subStartButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(subStartButton_);
    subStopButton_ = new widgets::AidaButton(QStringLiteral("Stop Selected"), page);
    subStopButton_->setKind(widgets::AidaButton::Kind::Destructive);
    subStopButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(subStopButton_);
    subRemoveButton_ = new widgets::AidaButton(QStringLiteral("Remove Selected"), page);
    subRemoveButton_->setKind(widgets::AidaButton::Kind::Secondary);
    subRemoveButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(subRemoveButton_);
    subExportButton_ = new widgets::AidaButton(QStringLiteral("Export CSV"), page);
    subExportButton_->setObjectName(QStringLiteral("view.network.recon.subdomains.export"));
    subExportButton_->setKind(widgets::AidaButton::Kind::Secondary);
    subExportButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(subExportButton_);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    subRunModel_ = new QtReconRunModel(QtReconRunModel::Domain::Subdomains, page);
    subRunsView_ = makeRunTable(page, subRunModel_, 160);
    subRunsView_->setObjectName(QStringLiteral("view.network.recon.subdomains.runs"));
    layout->addWidget(subRunsView_);
    subRunsEmpty_ = makeTableEmptyState(subRunsView_,
        QStringLiteral("view.network.recon.subdomains.runs.empty"),
        QStringLiteral("No enumeration runs"),
        QStringLiteral("Start subdomain enumeration above to populate the run history."));
    layout->addWidget(subRunsEmpty_);
    wireTableEmptyState(subRunsView_, subRunModel_, subRunsEmpty_);

    subDetailLabel_ = new QLabel(page);
    subDetailLabel_->setProperty("aidaTone", QStringLiteral("accent"));
    layout->addWidget(subDetailLabel_);
    subDetailModel_ = new QtReconResultModel(
        QtReconResultModel::Domain::SubdomainResults, page);
    subDetailView_ = makeRunTable(page, subDetailModel_, 240);
    subDetailView_->setObjectName(QStringLiteral("view.network.recon.subdomains.results"));
    layout->addWidget(subDetailView_, 1);
    subDetailEmpty_ = makeTableEmptyState(subDetailView_,
        QStringLiteral("view.network.recon.subdomains.results.empty"),
        QStringLiteral("No enumeration run selected"),
        QStringLiteral("Select a run above to inspect subdomain results."));
    layout->addWidget(subDetailEmpty_, 1);
    wireTableEmptyState(subDetailView_, subDetailModel_, subDetailEmpty_);

    connect(subStartButton_, &QAbstractButton::clicked, this, [this] {
        controller_->submitSubdomainStart(subDomainEdit_->text(), subWordlistEdit_->text(),
            subConcurrencySpin_->value(), subPassiveCheck_->isChecked(),
            subBruteCheck_->isChecked(), subCrtshCheck_->isChecked(),
            subBufferoverCheck_->isChecked(), subHackertargetCheck_->isChecked());
    });
    connect(subStopButton_, &QAbstractButton::clicked, this, [this] {
        if (sub_selected_ != 0) {
            const auto runs = controller_->subdomainRuns();
            const int row = subRunModel_->rowForId(sub_selected_);
            if (row >= 0 && row < static_cast<int>(runs->size()))
                controller_->submitStop(3, sub_selected_,
                    (*runs)[static_cast<std::size_t>(row)].started_unix_ms);
        }
    });
    connect(subRemoveButton_, &QAbstractButton::clicked, this, [this] {
        if (sub_selected_ != 0) {
            const auto runs = controller_->subdomainRuns();
            const int row = subRunModel_->rowForId(sub_selected_);
            if (row >= 0 && row < static_cast<int>(runs->size()))
                presentRemoveReview(3, sub_selected_,
                    (*runs)[static_cast<std::size_t>(row)].started_unix_ms);
        }
    });
    connect(subExportButton_, &QAbstractButton::clicked, this, [this] {
        if (sub_selected_ == 0)
            return;
        const std::uint64_t runId = sub_selected_;
        ::diag::log_tagged_fmt("recon_v", "sub_enum_export_csv id=%llu",
            static_cast<unsigned long long>(runId));
        if (!controller_->queueSubdomainExport(runId)) {
            toast_notification::push(
                "The CSV export could not be queued; see Task Center",
                toast_notification::toast_type_t::error);
        }
    });
    connect(subRunsView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* row = subRunModel_->rowAt(current.isValid() ? current.row() : -1);
            sub_selected_ = row ? row->id : 0;
            refreshSubdomainDetail();
        });
    return page;
}

QWidget* QtReconView::buildPayloadsTab(QWidget* parent)
{
    const auto& t = theme::tokens();
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, t.spacing.xs, 0, 0);
    layout->setSpacing(t.spacing.xs);

    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(t.spacing.xs);
    filterRow->addWidget(formLabel(page, QStringLiteral("Filter:")));
    payloadFilterEdit_ = new QLineEdit(page);
    payloadFilterEdit_->setObjectName(QStringLiteral("view.network.recon.payloads.filter"));
    payloadFilterEdit_->setMaxLength(127);
    payloadFilterEdit_->setPlaceholderText(QStringLiteral("substring"));
    payloadFilterEdit_->setMaximumWidth(field_width_chars(payloadFilterEdit_, 24));
    filterRow->addWidget(payloadFilterEdit_);
    filterRow->addStretch(1);
    layout->addLayout(filterRow);

    auto* splitter = new QSplitter(Qt::Horizontal, page);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);

    auto* leftPane = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    payloadSetModel_ = new QtPayloadSetModel(leftPane);
    payloadSetsView_ = new QTableView(leftPane);
    payloadSetsView_->setObjectName(QStringLiteral("view.network.recon.payloads.sets"));
    payloadSetsView_->horizontalHeader()->hide();
    payloadSetsView_->horizontalHeader()->setStretchLastSection(true);
    payloadSetsView_->verticalHeader()->hide();
    payloadSetsView_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    payloadSetsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    payloadSetsView_->setSelectionMode(QAbstractItemView::SingleSelection);
    payloadSetsView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    payloadSetsView_->setModel(payloadSetModel_);
    leftLayout->addWidget(payloadSetsView_, 1);
    payloadSetsEmpty_ = makeTableEmptyState(payloadSetsView_,
        QStringLiteral("view.network.recon.payloads.sets.empty"),
        QStringLiteral("No payload sets"),
        QStringLiteral("Adjust the filter or add a custom set below."));
    leftLayout->addWidget(payloadSetsEmpty_, 1);
    wireTableEmptyState(payloadSetsView_, payloadSetModel_, payloadSetsEmpty_);

    auto* rightPane = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(t.spacing.xs);
    payloadLabel_ = new QLabel(rightPane);
    payloadLabel_->setProperty("aidaTone", QStringLiteral("titleAccent"));
    rightLayout->addWidget(payloadLabel_);
    payloadDescription_ = new QLabel(rightPane);
    payloadDescription_->setWordWrap(true);
    payloadDescription_->setProperty("aidaTone", QStringLiteral("secondary"));
    rightLayout->addWidget(payloadDescription_);
    payloadCount_ = new QLabel(rightPane);
    payloadCount_->setProperty("aidaTone", QStringLiteral("secondary"));
    rightLayout->addWidget(payloadCount_);
    payloadEntryModel_ = new QtPayloadEntryModel(rightPane);
    payloadEntriesView_ = new QTableView(rightPane);
    payloadEntriesView_->setObjectName(
        QStringLiteral("view.network.recon.payloads.entries"));
    payloadEntriesView_->horizontalHeader()->hide();
    payloadEntriesView_->horizontalHeader()->setStretchLastSection(true);
    payloadEntriesView_->verticalHeader()->hide();
    payloadEntriesView_->verticalHeader()->setDefaultSectionSize(t.table.compact_row_h);
    payloadEntriesView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    payloadEntriesView_->setSelectionMode(QAbstractItemView::NoSelection);
    payloadEntriesView_->setFont(theme::fonts::codeRegular());
    payloadEntriesView_->setModel(payloadEntryModel_);
    rightLayout->addWidget(payloadEntriesView_, 1);
    payloadEntriesEmpty_ = makeTableEmptyState(payloadEntriesView_,
        QStringLiteral("view.network.recon.payloads.entries.empty"),
        QStringLiteral("No payload set selected"),
        QStringLiteral("Select a set on the left to view its entries."));
    rightLayout->addWidget(payloadEntriesEmpty_, 1);
    wireTableEmptyState(payloadEntriesView_, payloadEntryModel_, payloadEntriesEmpty_);
    payloadDeleteButton_ = new widgets::AidaButton(QStringLiteral("Delete Set"), rightPane);
    payloadDeleteButton_->setKind(widgets::AidaButton::Kind::Destructive);
    payloadDeleteButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    payloadDeleteButton_->setVisible(false);
    rightLayout->addWidget(payloadDeleteButton_);

    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    splitter->setSizes({ 360, 520 });
    layout->addWidget(splitter, 1);

    auto* separator = new QFrame(page);
    separator->setFrameShape(QFrame::HLine);
    layout->addWidget(separator);
    layout->addWidget(formLabel(page, QStringLiteral("Add custom set:")));
    auto* addRow1 = new QHBoxLayout();
    addRow1->setSpacing(t.spacing.xs);
    payloadNewIdEdit_ = new QLineEdit(page);
    payloadNewIdEdit_->setMaxLength(127);
    payloadNewIdEdit_->setPlaceholderText(QStringLiteral("id e.g. custom/xss-fast"));
    addRow1->addWidget(payloadNewIdEdit_);
    payloadNewLabelEdit_ = new QLineEdit(page);
    payloadNewLabelEdit_->setMaxLength(127);
    payloadNewLabelEdit_->setPlaceholderText(QStringLiteral("label"));
    addRow1->addWidget(payloadNewLabelEdit_);
    payloadNewDescEdit_ = new QLineEdit(page);
    payloadNewDescEdit_->setMaxLength(255);
    payloadNewDescEdit_->setPlaceholderText(QStringLiteral("description"));
    addRow1->addWidget(payloadNewDescEdit_, 1);
    layout->addLayout(addRow1);
    layout->addWidget(formLabel(page, QStringLiteral("Entries (one per line):")));
    payloadNewEntriesEdit_ = new QtByteCappedPlainTextEdit(page);
    payloadNewEntriesEdit_->setObjectName(
        QStringLiteral("view.network.recon.payloads.new_entries"));
    payloadNewEntriesEdit_->setMaxBytes(4095);
    payloadNewEntriesEdit_->setFont(theme::fonts::codeRegular());
    payloadNewEntriesEdit_->setMaximumHeight(
        editor_min_height_lines(payloadNewEntriesEdit_, 4));
    layout->addWidget(payloadNewEntriesEdit_);
    payloadAddButton_ = new widgets::AidaButton(QStringLiteral("Add"), page);
    payloadAddButton_->setObjectName(QStringLiteral("view.network.recon.payloads.add"));
    payloadAddButton_->setKind(widgets::AidaButton::Kind::Primary);
    payloadAddButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    layout->addWidget(payloadAddButton_);

    connect(payloadFilterEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        payloadSetModel_->setFilter(text);
    });
    connect(payloadSetsView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* set = payloadSetModel_->rowAt(current.isValid() ? current.row() : -1);
            payload_selected_id_ = set ? set->id : std::string();
            refreshPayloadDetail();
        });
    connect(payloadDeleteButton_, &QAbstractButton::clicked, this, [this] {
        const auto sets = controller_->payloadSets();
        const auto found = std::find_if(sets->begin(), sets->end(),
            [this](const auto& set) { return set.id == payload_selected_id_; });
        if (found != sets->end())
            presentPayloadRemoveReview(QString::fromStdString(found->id), found->builtin);
    });
    connect(payloadAddButton_, &QAbstractButton::clicked, this, [this] {
        std::vector<std::string> entries;
        std::string cur;
        const std::string text = payloadNewEntriesEdit_->toPlainText().toStdString();
        for (const char* p = text.c_str(); *p; ++p) {
            if (*p == '\n') {
                if (!cur.empty())
                    entries.push_back(cur);
                cur.clear();
            } else if (*p != '\r') {
                cur.push_back(*p);
            }
        }
        if (!cur.empty())
            entries.push_back(cur);
        controller_->submitPayloadAdd(payloadNewIdEdit_->text(),
            payloadNewLabelEdit_->text(), payloadNewDescEdit_->text(), std::move(entries));
    });
    return page;
}

void QtReconView::refreshRuns()
{
    {
        const auto crawls = controller_->crawlerRuns();
        std::vector<QtReconRunRow> rows;
        rows.reserve(crawls->size());
        for (const auto& run : *crawls) {
            QtReconRunRow row;
            row.id = run.id;
            row.started_unix_ms = run.started_unix_ms;
            row.phase = QString::fromLatin1(crawlPhaseLabel(run.phase));
            row.c0 = QString::number(run.queue_depth);
            row.c1 = QString::number(run.pages_visited);
            row.c2 = QString::number(run.pages_failed);
            row.c3 = QString::number(run.urls_found);
            rows.push_back(std::move(row));
        }
        crawlerRunModel_->adopt(std::move(rows));
        const int row = crawlerRunModel_->rowForId(crawler_selected_);
        if (row >= 0)
            crawlerRunsView_->setCurrentIndex(crawlerRunModel_->index(row, 0));
        refreshCrawlerDetail();
    }
    {
        const auto runs = controller_->discoveryRuns();
        std::vector<QtReconRunRow> rows;
        rows.reserve(runs->size());
        for (const auto& run : *runs) {
            QtReconRunRow row;
            row.id = run.id;
            row.started_unix_ms = run.started_unix_ms;
            row.phase = QString::fromLatin1(discPhaseLabel(run.phase));
            row.c0 = QString::number(run.attempts);
            row.c1 = QString::number(run.total);
            row.c2 = QString::number(run.hits);
            row.c3 = QString::number(run.errors);
            row.c4 = QString::number(run.filtered);
            rows.push_back(std::move(row));
        }
        discRunModel_->adopt(std::move(rows));
        const int row = discRunModel_->rowForId(disc_selected_);
        if (row >= 0)
            discRunsView_->setCurrentIndex(discRunModel_->index(row, 0));
        refreshDiscoveryDetail();
    }
    {
        const auto runs = controller_->subdomainRuns();
        std::vector<QtReconRunRow> rows;
        rows.reserve(runs->size());
        for (const auto& run : *runs) {
            QtReconRunRow row;
            row.id = run.id;
            row.started_unix_ms = run.started_unix_ms;
            row.phase = QString::fromLatin1(subPhaseLabel(run.phase));
            row.c0 = QString::number(run.passive_count);
            row.c1 = QString::number(run.brute_attempts);
            row.c2 = QString::number(run.brute_resolved);
            rows.push_back(std::move(row));
        }
        subRunModel_->adopt(std::move(rows));
        const int row = subRunModel_->rowForId(sub_selected_);
        if (row >= 0)
            subRunsView_->setCurrentIndex(subRunModel_->index(row, 0));
        refreshSubdomainDetail();
    }
    payloadSetModel_->adopt(controller_->payloadSets());
    refreshPayloadDetail();

    const bool pending = controller_->operation().pending();
    for (auto* button : { crawlerStartButton_, crawlerStopButton_, crawlerRemoveButton_,
                          discStartButton_, discStopButton_, discRemoveButton_,
                          subStartButton_, subStopButton_, subRemoveButton_ })
        button->setEnabled(!pending);
    const bool exportPending = controller_->subExportPending();
    const bool exportAvailable = sub_selected_ != 0 && !exportPending && !pending;
    subExportButton_->setEnabled(exportAvailable);
    subExportButton_->setText(exportPending ? QStringLiteral("Exporting...")
                                            : QStringLiteral("Export CSV"));
    subExportButton_->setToolTip(exportAvailable ? QString()
        : exportPending ? QStringLiteral("A subdomain CSV export is already running")
                        : QStringLiteral("Select a subdomain enumeration run first"));
    payloadAddButton_->setEnabled(!pending);
}

void QtReconView::refreshCrawlerDetail()
{
    const auto crawls = controller_->crawlerRuns();
    const aida::burp::crawler::crawl_status_t* selected = nullptr;
    for (const auto& run : *crawls) {
        if (run.id == crawler_selected_)
            selected = &run;
    }
    if (!selected) {
        crawlerDetailLabel_->clear();
        crawlerDetailEmpty_->setTitle(QStringLiteral("No crawl selected"));
        crawlerDetailEmpty_->setMessage(QStringLiteral(
            "Select a run above to inspect discovered URLs."));
        crawlerDetailModel_->adopt({});
        return;
    }
    crawlerDetailLabel_->setText(QStringLiteral("Discovered (%1) | Last: %2")
        .arg(static_cast<quint64>(selected->discovered.size()))
        .arg(QString::fromStdString(selected->last_url)));
    crawlerDetailEmpty_->setTitle(QStringLiteral("No URLs discovered"));
    crawlerDetailEmpty_->setMessage(QStringLiteral(
        "The selected crawl has not discovered any URLs yet."));
    std::vector<QtReconResultRow> rows;
    rows.reserve(selected->discovered.size());
    for (const auto& discovered : selected->discovered) {
        QtReconResultRow row;
        row.c0 = QString::number(discovered.status);
        row.c1 = QString::number(static_cast<quint64>(discovered.body_bytes));
        row.c2 = QString::number(discovered.depth);
        row.c3 = QString::fromStdString(discovered.content_type);
        row.c4 = QString::fromStdString(discovered.url);
        rows.push_back(std::move(row));
    }
    crawlerDetailModel_->adopt(std::move(rows));
}

void QtReconView::refreshDiscoveryDetail()
{
    const auto runs = controller_->discoveryRuns();
    const aida::burp::content_discovery::disc_status_t* selected = nullptr;
    for (const auto& run : *runs) {
        if (run.id == disc_selected_)
            selected = &run;
    }
    if (!selected) {
        discDetailLabel_->clear();
        discDetailEmpty_->setTitle(QStringLiteral("No discovery run selected"));
        discDetailEmpty_->setMessage(QStringLiteral(
            "Select a run above to inspect content hits."));
        discDetailModel_->adopt({});
        return;
    }
    discDetailLabel_->setText(QStringLiteral("Hits (%1) | Calibrated size range: %2-%3")
        .arg(static_cast<quint64>(selected->hits_list.size()))
        .arg(static_cast<quint64>(selected->calibrated_size_lo))
        .arg(static_cast<quint64>(selected->calibrated_size_hi)));
    discDetailEmpty_->setTitle(QStringLiteral("No content hits"));
    discDetailEmpty_->setMessage(QStringLiteral(
        "The selected run has not matched any content yet."));
    std::vector<QtReconResultRow> rows;
    rows.reserve(selected->hits_list.size());
    for (const auto& hit : selected->hits_list) {
        QtReconResultRow row;
        row.c0 = QString::number(hit.status);
        row.c1 = QString::number(static_cast<quint64>(hit.body_bytes));
        row.c2 = QString::number(static_cast<quint64>(hit.latency_ms));
        row.c3 = QString::fromStdString(hit.payload);
        row.c4 = QString::fromStdString(hit.url);
        rows.push_back(std::move(row));
    }
    discDetailModel_->adopt(std::move(rows));
}

void QtReconView::refreshSubdomainDetail()
{
    const auto runs = controller_->subdomainRuns();
    const aida::burp::subdomain_enum::enum_status_t* selected = nullptr;
    for (const auto& run : *runs) {
        if (run.id == sub_selected_)
            selected = &run;
    }
    if (!selected) {
        subDetailLabel_->clear();
        subDetailEmpty_->setTitle(QStringLiteral("No enumeration run selected"));
        subDetailEmpty_->setMessage(QStringLiteral(
            "Select a run above to inspect subdomain results."));
        subDetailModel_->adopt({});
        return;
    }
    subDetailLabel_->setText(QStringLiteral("Subdomains (%1)")
        .arg(static_cast<quint64>(selected->results.size())));
    subDetailEmpty_->setTitle(QStringLiteral("No subdomains found"));
    subDetailEmpty_->setMessage(QStringLiteral(
        "The selected enumeration has not resolved any subdomains yet."));
    std::vector<QtReconResultRow> rows;
    rows.reserve(selected->results.size());
    for (const auto& result : selected->results) {
        QtReconResultRow row;
        row.c0 = QString::fromStdString(result.fqdn);
        row.c1 = result.resolves ? QStringLiteral("yes") : QStringLiteral("no");
        QString ips;
        for (std::size_t i = 0; i < result.ips.size(); ++i) {
            if (i)
                ips += QStringLiteral(", ");
            ips += QString::fromStdString(result.ips[i]);
        }
        row.c2 = ips;
        QString sources;
        for (std::size_t i = 0; i < result.sources.size(); ++i) {
            if (i)
                sources += QStringLiteral(", ");
            sources += QString::fromStdString(result.sources[i]);
        }
        row.c3 = sources;
        rows.push_back(std::move(row));
    }
    subDetailModel_->adopt(std::move(rows));
}

void QtReconView::refreshPayloadDetail()
{
    const auto sets = controller_->payloadSets();
    const aida::burp::payloads::payload_set_t* selected = nullptr;
    if (!payload_selected_id_.empty()) {
        const auto found = std::find_if(sets->begin(), sets->end(),
            [this](const auto& set) { return set.id == payload_selected_id_; });
        if (found != sets->end())
            selected = &*found;
    }
    if (!selected) {
        payloadLabel_->setText(QStringLiteral("(select a set on the left)"));
        set_label_tone(payloadLabel_, "secondary");
        payloadDescription_->clear();
        payloadCount_->clear();
        payloadEntriesEmpty_->setTitle(QStringLiteral("No payload set selected"));
        payloadEntriesEmpty_->setMessage(QStringLiteral(
            "Select a set on the left to view its entries."));
        payloadEntryModel_->adopt({});
        payloadDeleteButton_->setVisible(false);
        return;
    }
    payloadLabel_->setText(QString::fromStdString(selected->label));
    set_label_tone(payloadLabel_, "titleAccent");
    payloadDescription_->setText(QString::fromStdString(selected->description));
    payloadCount_->setText(QStringLiteral("Entries: %1")
        .arg(static_cast<quint64>(selected->entries.size())));
    payloadEntriesEmpty_->setTitle(QStringLiteral("No entries"));
    payloadEntriesEmpty_->setMessage(QStringLiteral(
        "The selected payload set contains no entries."));
    payloadEntryModel_->adopt(std::vector<std::string>(selected->entries));
    payloadDeleteButton_->setVisible(!selected->builtin);
}

void QtReconView::presentRemoveReview(int domain, std::uint64_t id,
                                      std::uint64_t startedMs)
{
    auto* dialog = new aida::qt::bridge::AidaDialog(this);
    dialog->setWindowTitle(QStringLiteral("Review Recon removal"));
    dialog->setMinimumWidth(dialog_min_width_chars(dialog, 48));
    auto* layout = new QVBoxLayout(dialog);
    auto* title = new QLabel(QStringLiteral("Remove Recon run %1?")
        .arg(static_cast<quint64>(id)), dialog);
    layout->addWidget(title);
    auto* body = new QLabel(QStringLiteral(
        "The selected run and its retained results will be removed after exact identity revalidation."),
        dialog);
    body->setWordWrap(true);
    layout->addWidget(body);
    layout->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        dialog);
    auto* confirmButton = buttons->button(QDialogButtonBox::Ok);
    confirmButton->setText(QStringLiteral("Remove"));
    confirmButton->setEnabled(id != 0 && !controller_->operation().pending());
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, domain, id,
                                                            startedMs] {
        controller_->setAwaitingRemoveCompletion(
            controller_->submitReviewedRemove(domain, id, startedMs));
        dialog->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void QtReconView::presentPayloadRemoveReview(const QString& id, bool builtin)
{
    auto* dialog = new aida::qt::bridge::AidaDialog(this);
    dialog->setWindowTitle(QStringLiteral("Review payload set removal"));
    dialog->setMinimumWidth(dialog_min_width_chars(dialog, 48));
    auto* layout = new QVBoxLayout(dialog);
    auto* title = new QLabel(QStringLiteral("Remove payload set '%1'?").arg(id), dialog);
    layout->addWidget(title);
    auto* body = new QLabel(QStringLiteral(
        "The custom payload set and its persisted file will be removed after exact identity revalidation."),
        dialog);
    body->setWordWrap(true);
    layout->addWidget(body);
    layout->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        dialog);
    auto* confirmButton = buttons->button(QDialogButtonBox::Ok);
    confirmButton->setText(QStringLiteral("Remove set"));
    confirmButton->setEnabled(!id.isEmpty() && !controller_->operation().pending());
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, id, builtin] {
        controller_->setAwaitingPayloadRemoveCompletion(
            controller_->submitPayloadRemove(id, builtin));
        dialog->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

}
