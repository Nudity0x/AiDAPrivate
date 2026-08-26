#include "qt/overlays/aida_quick_open.hpp"

#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QScreen>
#include <QShowEvent>
#include <QStackedLayout>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/settings/settings_persistence_service.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/chrome/aida_legacy_chrome_bridge.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/overlays/aida_empty_state.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_progress.hpp"

namespace aida::qt::overlays {

namespace quick_open_detail {

std::string lower_ascii(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char v) {
        return static_cast<char>(std::tolower(v));
    });
    return value;
}

static std::filesystem::path path_from_utf8(std::string_view value)
{
#if defined(__cpp_char8_t)
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
#else
    return std::filesystem::u8path(value.begin(), value.end());
#endif
}

static std::string path_to_utf8(const std::filesystem::path& value)
{
    const auto encoded = value.generic_u8string();
#if defined(__cpp_char8_t)
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
    return encoded;
#endif
}

bool normalized_relative_path(std::string_view value, std::string& output)
{
    if (value.empty() || value.size() > k_maximum_path_bytes)
        return false;
    try {
        const auto path = path_from_utf8(value).lexically_normal();
        if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
            return false;
        for (const auto& component : path) {
            if (component == "..")
                return false;
        }
        output = path_to_utf8(path);
        return !output.empty() && output != "." && output.size() <= k_maximum_path_bytes;
    } catch (...) {
        return false;
    }
}

bool normalized_root_path(std::string_view value, std::string& output)
{
    if (value.empty() || value.size() > k_maximum_path_bytes)
        return false;
    try {
        const auto path = path_from_utf8(value).lexically_normal();
        const bool drive_absolute = value.size() >= 3 &&
            std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
            value[1] == ':' && (value[2] == '/' || value[2] == '\\');
        if (path.empty() || (!path.is_absolute() && !drive_absolute))
            return false;
        output = path_to_utf8(path);
        return !output.empty() && output.size() <= k_maximum_path_bytes;
    } catch (...) {
        return false;
    }
}

std::vector<mru_workspace_t> load_mru()
{
    std::vector<mru_workspace_t> output;
    const auto parsed = nlohmann::json::parse(
        g_sa_settings.workspace.quick_open_mru_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() ||
        parsed.value("version", 0) != 1 || !parsed.contains("workspaces") ||
        !parsed["workspaces"].is_array())
        return output;
    for (const auto& item : parsed["workspaces"]) {
        if (output.size() >= k_maximum_mru_workspaces)
            break;
        if (!item.is_object() || !item.contains("root") || !item["root"].is_string() ||
            !item.contains("files") || !item["files"].is_array())
            continue;
        mru_workspace_t workspace;
        if (!normalized_root_path(item["root"].get<std::string>(), workspace.root))
            continue;
        for (const auto& file : item["files"]) {
            if (workspace.files.size() >= k_maximum_mru_files)
                break;
            if (!file.is_string())
                continue;
            std::string relative;
            if (!normalized_relative_path(file.get<std::string>(), relative))
                continue;
            const std::string key = lower_ascii(relative);
            const bool duplicate = std::any_of(workspace.files.begin(), workspace.files.end(),
                [&](const std::string& existing) { return lower_ascii(existing) == key; });
            if (!duplicate)
                workspace.files.push_back(std::move(relative));
        }
        output.push_back(std::move(workspace));
    }
    return output;
}

bool persist_mru(search_state_t& state, std::string_view root, std::string_view relative)
{
    std::string normalized_relative;
    std::string normalized_root;
    if (!normalized_root_path(root, normalized_root) ||
        !normalized_relative_path(relative, normalized_relative))
        return false;
    const std::string root_key = lower_ascii(normalized_root);
    auto found = std::find_if(state.mru.begin(), state.mru.end(),
        [&](const mru_workspace_t& item) { return lower_ascii(item.root) == root_key; });
    mru_workspace_t workspace;
    if (found != state.mru.end()) {
        workspace = std::move(*found);
        state.mru.erase(found);
    } else {
        workspace.root = std::move(normalized_root);
    }
    const std::string path_key = lower_ascii(normalized_relative);
    workspace.files.erase(std::remove_if(workspace.files.begin(), workspace.files.end(),
        [&](const std::string& item) { return lower_ascii(item) == path_key; }),
        workspace.files.end());
    workspace.files.insert(workspace.files.begin(), std::move(normalized_relative));
    if (workspace.files.size() > k_maximum_mru_files)
        workspace.files.resize(k_maximum_mru_files);
    state.mru.insert(state.mru.begin(), std::move(workspace));
    if (state.mru.size() > k_maximum_mru_workspaces)
        state.mru.resize(k_maximum_mru_workspaces);

    auto serialize = [&]() {
        nlohmann::json workspaces = nlohmann::json::array();
        for (const auto& item : state.mru)
            workspaces.push_back({{"root", item.root}, {"files", item.files}});
        return nlohmann::json{{"version", 1},
            {"workspaces", std::move(workspaces)}}.dump();
    };
    std::string encoded = serialize();
    while (encoded.size() > 256U * 1024U && !state.mru.empty()) {
        auto& oldest = state.mru.back();
        if (oldest.files.size() > 1)
            oldest.files.pop_back();
        else
            state.mru.pop_back();
        encoded = serialize();
    }
    if (encoded.size() > 256U * 1024U)
        return false;
    g_sa_settings.workspace.quick_open_mru_json = std::move(encoded);
    return aida::settings_persistence::accepted(
        aida::settings_persistence::request_save(g_sa_settings));
}

int mru_bonus(const search_state_t& state, std::string_view root, std::string_view relative)
{
    const std::string root_key = lower_ascii(std::string(root));
    const std::string path_key = lower_ascii(std::string(relative));
    const auto workspace = std::find_if(state.mru.begin(), state.mru.end(),
        [&](const mru_workspace_t& item) { return lower_ascii(item.root) == root_key; });
    if (workspace == state.mru.end())
        return 0;
    for (std::size_t index = 0; index < workspace->files.size(); ++index) {
        if (lower_ascii(workspace->files[index]) == path_key)
            return 1600 - static_cast<int>((std::min)(index, std::size_t{31})) * 25;
    }
    return 0;
}

int fuzzy_score(std::string_view query, std::string_view label, std::string_view detail)
{
    if (query.empty())
        return -1;
    const std::string needle = lower_ascii(std::string(query));
    const std::string primary = lower_ascii(std::string(label));
    const std::string secondary = lower_ascii(std::string(detail));
    const std::string candidate = primary + " " + secondary;
    const auto exact = candidate.find(needle);
    if (exact != std::string::npos) {
        int score = 5000 - static_cast<int>((std::min)(exact, std::size_t{4000}));
        if (exact == 0)
            score += 1800;
        const auto slash = primary.find_last_of('/');
        if (slash != std::string::npos && exact == slash + 1)
            score += 1400;
        return score;
    }
    std::size_t cursor = 0;
    std::size_t previous = 0;
    int score = 900;
    for (const char character : needle) {
        const auto found = candidate.find(character, cursor);
        if (found == std::string::npos)
            return -1;
        if (cursor != 0)
            score -= static_cast<int>((std::min)(found - previous - 1, std::size_t{32})) * 8;
        if (found == 0 || candidate[found - 1] == '/' || candidate[found - 1] == '_' ||
            candidate[found - 1] == '-' || candidate[found - 1] == ' ')
            score += 90;
        previous = found;
        cursor = found + 1;
    }
    return score;
}

int kind_bonus(result_kind_t kind) noexcept
{
    switch (kind) {
    case result_kind_t::file: return 400;
    case result_kind_t::symbol: return 300;
    case result_kind_t::view: return 200;
    case result_kind_t::command: return 100;
    }
    return 0;
}

void consider(search_state_t& state, result_t result)
{
    if (result.score < 0)
        return;
    result.score += kind_bonus(result.kind);
    const auto duplicate = std::find_if(state.results.begin(), state.results.end(),
        [&](const result_t& existing) { return existing.identity == result.identity; });
    if (duplicate != state.results.end()) {
        if (result.score > duplicate->score)
            *duplicate = std::move(result);
        return;
    }
    if (state.results.size() < k_maximum_results) {
        state.results.push_back(std::move(result));
        return;
    }
    state.result_limit_reached = true;
    const auto lowest = std::min_element(state.results.begin(), state.results.end(),
        [](const result_t& left, const result_t& right) {
            return left.score < right.score;
        });
    if (lowest != state.results.end() && result.score > lowest->score)
        *lowest = std::move(result);
}

std::vector<result_t> ordered_results(const search_state_t& state)
{
    std::vector<result_t> output = state.results;
    std::sort(output.begin(), output.end(), [](const result_t& left, const result_t& right) {
        if (left.score != right.score)
            return left.score > right.score;
        if (left.kind != right.kind)
            return static_cast<unsigned>(left.kind) < static_cast<unsigned>(right.kind);
        if (left.label != right.label)
            return left.label < right.label;
        return left.identity < right.identity;
    });
    return output;
}

const char* kind_label(result_kind_t kind) noexcept
{
    switch (kind) {
    case result_kind_t::file: return "FILE";
    case result_kind_t::symbol: return "SYMBOL";
    case result_kind_t::view: return "VIEW";
    case result_kind_t::command: return "COMMAND";
    }
    return "ITEM";
}

void reset_search(search_state_t& state, std::string query,
                  std::shared_ptr<const code_index::published_index_t> index)
{
    state.query = std::move(query);
    state.index = std::move(index);
    state.results.clear();
    state.selected_identity.clear();
    state.status.clear();
    state.error.clear();
    state.file_cursor = 0;
    state.symbol_cursor = 0;
    state.scanned_candidates = 0;
    state.total_candidates = 0;
    state.auxiliary_complete = false;
    state.result_limit_reached = false;
    if (state.mru.empty())
        state.mru = load_mru();
    if (state.index) {
        if (state.index->file_paths)
            state.total_candidates += state.index->file_paths->size();
        if (state.index->symbols)
            state.total_candidates += state.index->symbols->size();
    }
}

bool scan_complete(const search_state_t& state) noexcept
{
    const bool files_complete = !state.index || !state.index->file_paths ||
        state.file_cursor >= state.index->file_paths->size();
    const bool symbols_complete = !state.index || !state.index->symbols ||
        state.symbol_cursor >= state.index->symbols->size();
    return state.auxiliary_complete && files_complete && symbols_complete;
}

}

namespace {

void scan_auxiliary(quick_open_detail::search_state_t& state, docking::AidaDockHost* host)
{
    using namespace quick_open_detail;
    if (state.auxiliary_complete)
        return;
    if (host) {
        host->for_each_menu_entry([&](const registry::menu_entry_t& entry) {
            result_t result;
            result.kind = result_kind_t::view;
            result.identity = "view:" + entry.id.value();
            result.label = entry.label;
            result.detail = std::string(registry::category_label(entry.category)) +
                (entry.open ? " · open" : " · closed");
            result.target_id = entry.id.value();
            result.enabled = entry.enabled;
            result.disabled_reason = entry.disabled_reason;
            result.score = fuzzy_score(state.query, result.label, result.target_id);
            consider(state, std::move(result));
            ++state.scanned_candidates;
            ++state.total_candidates;
        });
    }
    auto actions = aida::ui::application_ui::list_actions(
        aida::ui::action_surface_t::command_palette);
    for (const auto& action : actions) {
        if (!action.visible || action.id == "file.quick_open" ||
            action.id.compare(0, 11, "view.focus.") == 0)
            continue;
        result_t result;
        result.kind = result_kind_t::command;
        result.identity = "command:" + action.id;
        result.label = action.label;
        result.detail = action.category.empty() ? action.description :
            action.category + " · " + action.description;
        result.target_id = action.id;
        result.enabled = action.enabled;
        result.disabled_reason = action.disabled_reason;
        result.score = fuzzy_score(state.query, result.label,
            result.target_id + " " + result.detail);
        consider(state, std::move(result));
        ++state.scanned_candidates;
        ++state.total_candidates;
    }
    state.auxiliary_complete = true;
}

void scan_index(quick_open_detail::search_state_t& state, docking::AidaDockHost* host)
{
    using namespace quick_open_detail;
    if (state.query.empty())
        return;
    scan_auxiliary(state, host);
    std::size_t budget = k_candidates_per_frame;
    if (state.index && state.index->file_paths) {
        const auto& files = *state.index->file_paths;
        while (state.file_cursor < files.size() && budget != 0) {
            const std::string& path = files[state.file_cursor++];
            --budget;
            ++state.scanned_candidates;
            result_t result;
            result.kind = result_kind_t::file;
            result.identity = "file:" + path;
            result.label = path;
            result.detail = state.index->root_path;
            result.relative_path = path;
            result.root_path = state.index->root_path;
            result.index_generation = state.index->generation;
            result.score = fuzzy_score(state.query, path, {});
            if (result.score >= 0)
                result.score += mru_bonus(state, result.root_path, result.relative_path);
            consider(state, std::move(result));
        }
    }
    if (budget != 0 && state.index && state.index->symbols) {
        const auto& symbols = *state.index->symbols;
        while (state.symbol_cursor < symbols.size() && budget != 0) {
            const auto& symbol = symbols[state.symbol_cursor++];
            --budget;
            ++state.scanned_candidates;
            result_t result;
            result.kind = result_kind_t::symbol;
            result.identity = "symbol:" + symbol.file_path + ":" +
                std::to_string(symbol.line_number) + ":" + symbol.symbol_name;
            result.label = symbol.symbol_name;
            result.detail = symbol.symbol_type + " · " + symbol.file_path + ":" +
                std::to_string(symbol.line_number);
            result.relative_path = symbol.file_path;
            result.root_path = state.index->root_path;
            result.line = (std::max)(1, symbol.line_number);
            result.column = (std::max)(1, symbol.column_number);
            result.index_generation = state.index->generation;
            result.score = fuzzy_score(state.query, result.label,
                symbol.file_path + " " + symbol.symbol_type);
            if (result.score >= 0)
                result.score += mru_bonus(state, result.root_path, result.relative_path);
            consider(state, std::move(result));
        }
    }
}

bool activate_result(docking::AidaDockHost* host, quick_open_detail::search_state_t& state,
                     const quick_open_detail::result_t& result, bool open_to_side)
{
    using namespace quick_open_detail;
    state.error.clear();
    if (!result.enabled) {
        state.error = result.disabled_reason.empty() ?
            "The selected item is unavailable" : result.disabled_reason;
        return false;
    }
    if (result.kind == result_kind_t::file || result.kind == result_kind_t::symbol) {
        const auto current = editor::language_service::workspace_index_snapshot();
        if (!current || current->generation != result.index_generation ||
            lower_ascii(current->root_path) != lower_ascii(result.root_path)) {
            state.error = "The workspace index changed; review the refreshed results before opening this item";
            return false;
        }
        std::string relative;
        if (!normalized_relative_path(result.relative_path, relative)) {
            state.error = "The selected result is no longer a valid workspace-relative path";
            return false;
        }
        editor::language_service::location_t location;
        location.root_path = result.root_path;
        location.file_path = relative;
        location.line = result.line;
        location.column = result.column;
        if (!editor::language_service::open_location(location, open_to_side)) {
            state.error = "The selected file could not be opened inside the indexed workspace";
            return false;
        }
        if (!persist_mru(state, result.root_path, relative))
            chrome::toast_warning(QStringLiteral(
                "The file opened, but Quick Open history could not be queued for persistence"), 6.0);
        return true;
    }
    if (result.kind == result_kind_t::view) {
        if (!host) {
            state.error = "The view host is unavailable";
            return false;
        }
        const auto opened = host->open_or_focus(
            aida::ui::stable_view_id_t(result.target_id));
        if (!opened.ok()) {
            state.error = opened.detail.empty() ? "The selected view could not be opened" : opened.detail;
            return false;
        }
        return true;
    }
    const auto executed = aida::ui::application_ui::execute_action(result.target_id.c_str(),
        aida::ui::action_invocation_source_t::command_palette);
    if (executed.status == aida::ui::action_execution_status_t::confirmation_required ||
        executed.status == aida::ui::action_execution_status_t::review_required)
        return true;
    if (!executed.executed()) {
        state.error = executed.message.empty() ?
            "The selected command could not be executed" : executed.message;
        return false;
    }
    return true;
}

}

AidaQuickOpenModel::AidaQuickOpenModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int AidaQuickOpenModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(results_.size());
}

QVariant AidaQuickOpenModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(results_.size()))
        return {};
    const auto& result = results_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
        return QString::fromStdString(result.label);
    case Qt::ToolTipRole:
        return result.enabled ? QVariant{}
            : QString::fromStdString(result.disabled_reason);
    case IdentityRole:
        return QString::fromStdString(result.identity);
    case DetailRole:
        return QString::fromStdString(result.detail);
    case KindRole:
        return QString::fromLatin1(quick_open_detail::kind_label(result.kind));
    case EnabledRole:
        return result.enabled;
    case DisabledReasonRole:
        return QString::fromStdString(result.disabled_reason);
    case ScoreRole:
        return result.score;
    }
    return {};
}

void AidaQuickOpenModel::setResults(std::vector<quick_open_detail::result_t> results)
{
    beginResetModel();
    results_ = std::move(results);
    endResetModel();
}

const quick_open_detail::result_t* AidaQuickOpenModel::resultAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(results_.size()))
        return nullptr;
    return &results_[static_cast<std::size_t>(row)];
}

int AidaQuickOpenModel::rowForIdentity(const std::string& identity) const
{
    for (std::size_t i = 0; i < results_.size(); ++i)
        if (results_[i].identity == identity)
            return static_cast<int>(i);
    return -1;
}

AidaQuickOpenDelegate::AidaQuickOpenDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

QSize AidaQuickOpenDelegate::sizeHint(const QStyleOptionViewItem& option,
                                      const QModelIndex& index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    const auto& t = theme::tokens();
    const QFontMetricsF label_fm(theme::fonts::body());
    const QFontMetricsF detail_fm(theme::fonts::caption());
    const int height = 2 * t.spacing.xs +
        static_cast<int>(label_fm.height() + 0.5) + t.spacing.xxs +
        static_cast<int>(detail_fm.height() + 0.5);
    return QSize(static_cast<int>(t.shell.min_panel_w * 2.0), height);
}

void AidaQuickOpenDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const
{
    const auto& t = theme::tokens();
    const bool enabled = index.data(AidaQuickOpenModel::EnabledRole).toBool();
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    const bool hovered = (option.state & QStyle::State_MouseOver) != 0;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    if (selected) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(t.selection);
        painter->drawRoundedRect(option.rect.adjusted(t.spacing.xxs, t.spacing.xxs,
            -t.spacing.xxs, -t.spacing.xxs), t.radius.md, t.radius.md);
    } else if (hovered) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(widgets::with_alpha(t.hover_wash, 0.10));
        painter->drawRoundedRect(option.rect.adjusted(t.spacing.xxs, t.spacing.xxs,
            -t.spacing.xxs, -t.spacing.xxs), t.radius.md, t.radius.md);
    }

    const QRectF r = option.rect;
    const QString label = index.data(Qt::DisplayRole).toString();
    const QString detail = index.data(AidaQuickOpenModel::DetailRole).toString();
    const QString kind = index.data(AidaQuickOpenModel::KindRole).toString();

    const QFont label_font = theme::fonts::body();
    const QFont caption_font = theme::fonts::caption();
    const QFontMetricsF label_fm(label_font);
    const QFontMetricsF caption_fm(caption_font);
    const qreal pad_x = t.spacing.sm;
    const qreal pad_v = t.spacing.xs;
    const qreal label_h = label_fm.height();
    const qreal detail_h = caption_fm.height();
    const qreal kind_w = caption_fm.horizontalAdvance(kind);

    painter->setFont(label_font);
    const QColor label_color = enabled ? t.text_primary : t.text_dim;
    painter->setPen(widgets::with_alpha(label_color, 0.92));
    const QRectF label_rect(r.left() + pad_x, r.top() + pad_v,
        r.width() - 2 * pad_x - kind_w - t.spacing.sm, label_h);
    if (label_rect.width() > t.spacing.xl) {
        painter->drawText(label_rect, Qt::AlignLeft | Qt::AlignVCenter,
            label_fm.elidedText(label, Qt::ElideMiddle,
                static_cast<int>(label_rect.width())));
    }

    painter->setFont(caption_font);
    painter->setPen(widgets::with_alpha(t.text_dim, 0.9));
    painter->drawText(QRectF(r.right() - pad_x - kind_w, r.top() + pad_v, kind_w, label_h),
                      Qt::AlignRight | Qt::AlignVCenter, kind);

    if (!detail.isEmpty()) {
        painter->setPen(t.text_secondary);
        const QRectF detail_rect(r.left() + pad_x,
            r.top() + pad_v + label_h + t.spacing.xxs, r.width() - 2 * pad_x, detail_h);
        if (detail_rect.width() > t.spacing.xl) {
            painter->drawText(detail_rect, Qt::AlignLeft | Qt::AlignVCenter,
                caption_fm.elidedText(detail, Qt::ElideMiddle,
                    static_cast<int>(detail_rect.width())));
        }
    }
    painter->restore();
}

static QSize quick_open_minimum_size()
{
    const auto& t = theme::tokens();
    return QSize(4 * t.row.property_label_w + t.panel.overlay_margin,
                 3 * static_cast<int>(t.shell.min_panel_w) + t.panel.overlay_margin);
}

static QSize quick_open_preferred_size()
{
    const auto& t = theme::tokens();
    return QSize(4 * t.row.property_label_w + 2 * static_cast<int>(t.shell.min_panel_w),
                 3 * t.row.property_label_w + 2 * t.panel.overlay_margin);
}

AidaQuickOpenDialog::AidaQuickOpenDialog(docking::AidaDockHost* host, QWidget* parent)
    : bridge::AidaDialog(parent, Qt::FramelessWindowHint | Qt::Dialog),
      host_(host)
{
    setObjectName(QStringLiteral("aida.quick_open"));
    setModal(false);
    setWindowTitle(QStringLiteral("Quick Open"));

    auto* root = new QVBoxLayout(this);
    const auto& t = theme::tokens();
    root->setContentsMargins(t.panel.padding, t.panel.padding,
                             t.panel.padding, t.panel.padding);
    root->setSpacing(t.spacing.xs);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("Quick Open"), this);
    title->setFont(theme::fonts::strong());
    header->addWidget(title);
    auto* subtitle = new QLabel(QStringLiteral("Files, symbols, views and commands"), this);
    subtitle->setFont(theme::fonts::caption());
    subtitle->setProperty("aidaVariant", QStringLiteral("secondary"));
    header->addWidget(subtitle);
    header->addStretch(1);
    auto* shortcut_hint = new QLabel(QStringLiteral("Ctrl+P"), this);
    shortcut_hint->setFont(theme::fonts::caption());
    shortcut_hint->setProperty("aidaVariant", QStringLiteral("secondary"));
    header->addWidget(shortcut_hint);
    root->addLayout(header);

    query_edit_ = new QLineEdit(this);
    query_edit_->setObjectName(QStringLiteral("aida.quick_open.query"));
    query_edit_->setPlaceholderText(QStringLiteral("Type a file, symbol, view or command name"));
    query_edit_->setClearButtonEnabled(true);
    query_edit_->setFont(theme::fonts::body());
    bridge::InteractionContextProvider::mark_text_input(query_edit_);
    connect(query_edit_, &QLineEdit::textChanged, this,
            [this](const QString& text) { onQueryChanged(text); });
    root->addWidget(query_edit_);

    error_label_ = new QLabel(this);
    error_label_->setObjectName(QStringLiteral("aida.quick_open.error"));
    error_label_->setFont(theme::fonts::caption());
    error_label_->setWordWrap(true);
    error_label_->setVisible(false);
    root->addWidget(error_label_);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.quick_open.status"));
    status_label_->setFont(theme::fonts::caption());
    root->addWidget(status_label_);

    progress_ = new widgets::AidaProgressBar(this);
    progress_->setObjectName(QStringLiteral("aida.quick_open.progress"));
    progress_->setBarHeight(t.spacing.xxs + t.panel.border);
    progress_->setVisible(false);
    root->addWidget(progress_);

    model_ = new AidaQuickOpenModel(this);
    delegate_ = new AidaQuickOpenDelegate(this);
    results_view_ = new QListView(this);
    results_view_->setObjectName(QStringLiteral("aida.quick_open.results"));
    results_view_->setModel(model_);
    results_view_->setItemDelegate(delegate_);
    results_view_->setUniformItemSizes(true);
    results_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    results_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(results_view_, &QListView::activated, this, [this](const QModelIndex& index) {
        const auto* result = model_->resultAt(index.row());
        if (result)
            activate(*result, QGuiApplication::keyboardModifiers() & Qt::ControlModifier);
    });
    empty_state_ = new AidaEmptyState(this);
    empty_state_->setObjectName(QStringLiteral("aida.quick_open.empty"));
    results_stack_ = new QStackedLayout();
    results_stack_->addWidget(results_view_);
    results_stack_->addWidget(empty_state_);
    root->addLayout(results_stack_, 1);

    footer_label_ = new QLabel(this);
    footer_label_->setObjectName(QStringLiteral("aida.quick_open.footer"));
    footer_label_->setFont(theme::fonts::caption());
    footer_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(footer_label_);

    scan_timer_ = new QTimer(this);
    scan_timer_->setInterval(0);
    scan_timer_->setSingleShot(false);
    connect(scan_timer_, &QTimer::timeout, this, [this] { onScanChunk(); });

    setMinimumSize(quick_open_minimum_size());
    resize(quick_open_preferred_size());
}

AidaQuickOpenDialog::~AidaQuickOpenDialog() = default;

void AidaQuickOpenDialog::openFresh()
{
    state_.mru = quick_open_detail::load_mru();
    quick_open_detail::reset_search(state_, {},
        editor::language_service::workspace_index_snapshot());
    selected_identity_.clear();
    query_edit_->clear();
    refreshModel();
    updateStatusLine();
    centerOnHost();
    show();
    raise();
    activateWindow();
    query_edit_->setFocus(Qt::OtherFocusReason);
}

void AidaQuickOpenDialog::setQueryText(const QString& text)
{
    query_edit_->setText(text);
    query_edit_->selectAll();
    query_edit_->setFocus(Qt::OtherFocusReason);
}

void AidaQuickOpenDialog::centerOnHost()
{
    const auto& t = theme::tokens();
    const QSize floor = minimumSize();
    const int panel_floor = static_cast<int>(t.shell.min_panel_w * 2.0);
    const int min_w = (std::max)(floor.width(), panel_floor);
    const int min_h = (std::max)(floor.height(), panel_floor);
    QWidget* anchor = parentWidget() ? parentWidget()->window() : nullptr;
    if (!anchor) {
        if (QScreen* screen = QGuiApplication::primaryScreen()) {
            const QRect area = screen->availableGeometry();
            move(area.center().x() - width() / 2,
                 area.top() + (std::max)(t.spacing.lg,
                     static_cast<int>((area.height() - height()) * 0.18)));
        }
        return;
    }
    const QRect host_rect(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
    const QSize preferred = quick_open_preferred_size();
    const int w = (std::min)(preferred.width(),
        (std::max)(min_w, host_rect.width() - t.panel.overlay_margin));
    const int h = (std::min)(preferred.height(), (std::max)(min_h,
        host_rect.height() - t.panel.overlay_margin - t.spacing.lg));
    setFixedSize(w, h);
    move(host_rect.left() + (host_rect.width() - w) / 2,
         host_rect.top() + (std::max)(t.spacing.lg,
             static_cast<int>((host_rect.height() - h) * 0.18)));
}

void AidaQuickOpenDialog::showEvent(QShowEvent* event)
{
    bridge::AidaDialog::showEvent(event);
    raise();
}

bool AidaQuickOpenDialog::sourceChanged() const
{
    const auto current = editor::language_service::workspace_index_snapshot();
    return (current && !state_.index) || (!current && state_.index) ||
        (current && state_.index &&
         (current->generation != state_.index->generation ||
          quick_open_detail::lower_ascii(current->root_path) !=
              quick_open_detail::lower_ascii(state_.index->root_path)));
}

void AidaQuickOpenDialog::onQueryChanged(const QString& text)
{
    const std::string query = text.toStdString();
    if (query == state_.query && !sourceChanged()) {
        if (state_.query.empty()) {
            refreshModel();
            updateStatusLine();
        }
        return;
    }
    quick_open_detail::reset_search(state_, query,
        editor::language_service::workspace_index_snapshot());
    selected_identity_.clear();
    if (!state_.query.empty())
        scan_timer_->start();
    refreshModel();
    updateStatusLine();
}

void AidaQuickOpenDialog::onScanChunk()
{
    if (state_.query.empty()) {
        scan_timer_->stop();
        return;
    }
    scan_index(state_, host_);
    refreshModel();
    updateStatusLine();
    if (quick_open_detail::scan_complete(state_))
        scan_timer_->stop();
}

void AidaQuickOpenDialog::refreshModel()
{
    model_->setResults(quick_open_detail::ordered_results(state_));
    if (model_->rowCount() == 0) {
        selected_identity_.clear();
        updateEmptyState();
        return;
    }
    if (results_stack_)
        results_stack_->setCurrentWidget(results_view_);
    int row = selected_identity_.empty() ? 0 : model_->rowForIdentity(selected_identity_);
    if (row < 0)
        row = 0;
    const QModelIndex index = model_->index(row, 0);
    results_view_->setCurrentIndex(index);
    selected_identity_ = model_->resultAt(row)->identity;
}

void AidaQuickOpenDialog::updateEmptyState()
{
    if (!results_stack_ || !empty_state_)
        return;
    AidaEmptyStateConfig config;
    if (state_.query.empty()) {
        config.glyph = AidaGlyph::Search;
        config.title = QStringLiteral("Quick Open");
        config.body = QStringLiteral(
            "Type to search files, symbols, views and commands.");
        config.kbd_hints = { QString::fromUtf8("\xE2\x86\x91"), QString::fromUtf8("\xE2\x86\x93"),
            QStringLiteral("Enter"), QStringLiteral("Ctrl+Enter"), QStringLiteral("Esc") };
    } else {
        config.glyph = AidaGlyph::Dots;
        config.title = QStringLiteral("No matching results");
        config.body = QStringLiteral(
            "Adjust the query; files, symbols, views and commands remain searchable while indexing runs.");
        config.kbd_hints = { QStringLiteral("Esc") };
    }
    const AidaEmptyStateConfig& current = empty_state_->config();
    if (current.glyph != config.glyph || current.title != config.title ||
        current.body != config.body)
        empty_state_->setConfig(config);
    results_stack_->setCurrentWidget(empty_state_);
}

void AidaQuickOpenDialog::moveSelection(int delta)
{
    const int count = model_->rowCount();
    if (count == 0)
        return;
    int row = results_view_->currentIndex().row();
    row = (row + delta + count) % count;
    results_view_->setCurrentIndex(model_->index(row, 0));
    const auto* result = model_->resultAt(row);
    if (result)
        selected_identity_ = result->identity;
}

void AidaQuickOpenDialog::activateCurrent(bool open_to_side)
{
    const auto* result = model_->resultAt(results_view_->currentIndex().row());
    if (result)
        activate(*result, open_to_side);
}

void AidaQuickOpenDialog::activate(const quick_open_detail::result_t& result, bool open_to_side)
{
    if (activate_result(host_, state_, result, open_to_side)) {
        reject();
        return;
    }
    updateStatusLine();
}

void AidaQuickOpenDialog::updateStatusLine()
{
    if (!state_.error.empty()) {
        error_label_->setText(QString::fromStdString(state_.error));
        error_label_->setVisible(true);
    } else {
        error_label_->clear();
        error_label_->setVisible(false);
    }

    const auto index_state = editor::language_service::workspace_index_state();
    const bool complete = state_.query.empty() ? true : quick_open_detail::scan_complete(state_);
    if (state_.query.empty()) {
        status_label_->setText(QStringLiteral("Type to search. Query text is never persisted."));
        progress_->setVisible(false);
    } else if (!complete) {
        const double progress_value = state_.total_candidates == 0 ? 0.0 :
            static_cast<double>((std::min)(state_.scanned_candidates, state_.total_candidates)) /
            static_cast<double>(state_.total_candidates);
        progress_->setIndeterminate(state_.total_candidates == 0);
        progress_->setProgress(static_cast<qreal>(progress_value));
        progress_->setVisible(true);
        status_label_->setText(QStringLiteral("Filtering immutable workspace index incrementally..."));
    } else if (model_->rowCount() == 0) {
        progress_->setVisible(false);
        if (!state_.index && index_state == code_index::index_state_t::indexing)
            status_label_->setText(QStringLiteral("Workspace indexing is in progress; views and commands remain searchable."));
        else if (!state_.index && index_state == code_index::index_state_t::cancelled)
            status_label_->setText(QStringLiteral("Workspace indexing was cancelled; no file publication is available."));
        else if (!state_.index && index_state == code_index::index_state_t::error)
            status_label_->setText(QString::fromStdString(
                editor::language_service::workspace_index_status()));
        else
            status_label_->setText(QStringLiteral("No matching files, symbols, views or commands."));
    } else {
        progress_->setVisible(false);
        const bool bounded = state_.result_limit_reached ||
            (state_.index && (state_.index->truncated || state_.index->skipped_files != 0));
        status_label_->setText(QStringLiteral("%1 results%2 | Enter opens | Ctrl+Enter opens file to side | Esc closes")
            .arg(model_->rowCount())
            .arg(bounded ? QStringLiteral(" (bounded)") : QString()));
    }
    footer_label_->setText(QStringLiteral("Results are generation-fenced; moved or stale paths fail visibly."));
}

void AidaQuickOpenDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Down) {
        moveSelection(1);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Up) {
        moveSelection(-1);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_PageDown || event->key() == Qt::Key_PageUp) {
        const int row_h = delegate_
            ? delegate_->sizeHint(QStyleOptionViewItem(), QModelIndex()).height() : 0;
        const int page = row_h > 0
            ? (std::max)(1, results_view_->viewport()->height() / row_h) : 1;
        moveSelection((event->key() == Qt::Key_PageDown ? 1 : -1) * page);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (results_view_->hasFocus()) {
            bridge::AidaDialog::keyPressEvent(event);
            return;
        }
        activateCurrent((event->modifiers() & Qt::ControlModifier) != 0);
        event->accept();
        return;
    }
    bridge::AidaDialog::keyPressEvent(event);
}

AidaQuickOpenController::AidaQuickOpenController(docking::AidaDockHost* host,
                                                 QWidget* dialog_parent, QObject* parent)
    : QObject(parent), host_(host), dialog_parent_(dialog_parent)
{
    legacy_poll_ = new QTimer(this);
    legacy_poll_->setInterval(60);
    legacy_poll_->setTimerType(Qt::CoarseTimer);
    connect(legacy_poll_, &QTimer::timeout, this, [this] { pollLegacyRequest(); });
    legacy_poll_->start();
}

AidaQuickOpenController::~AidaQuickOpenController() = default;

void AidaQuickOpenController::open()
{
    if (dialog_) {
        dialog_->raise();
        dialog_->activateWindow();
        return;
    }
    if (chrome::legacy_chrome_hooks().quick_open.close_command_palette)
        chrome::legacy_chrome_hooks().quick_open.close_command_palette();
    dialog_ = new AidaQuickOpenDialog(host_, dialog_parent_);
    connect(dialog_, &QDialog::finished, this, [this](int) {
        if (chrome::legacy_chrome_hooks().quick_open.mark_closed)
            chrome::legacy_chrome_hooks().quick_open.mark_closed();
        dialog_->deleteLater();
        dialog_ = nullptr;
    });
    dialog_->openFresh();
}

bool AidaQuickOpenController::isOpen() const
{
    return dialog_ != nullptr;
}

void AidaQuickOpenController::pollLegacyRequest()
{
    auto& hooks = chrome::legacy_chrome_hooks().quick_open;
    if (!hooks.poll_open_request)
        return;
    std::string query;
    if (!hooks.poll_open_request(query))
        return;
    if (dialog_)
        return;
    open();
    if (dialog_ && !query.empty())
        dialog_->setQueryText(QString::fromStdString(query));
}

}
