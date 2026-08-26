#include "qt/analysis/qt_binary_map_list_model.hpp"

#include "qt/analysis/qt_binary_map_shared.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace aida::qt::analysis {

QtBinaryMapListModel::QtBinaryMapListModel(QObject* parent)
    : QAbstractTableModel(parent) {}

namespace {

bool group_collapsed(QtBinaryMapViewState& s, const std::string& key) {
    return s.collapsed_groups.count(key) != 0;
}

}

void QtBinaryMapListModel::rebuild(
    QtBinaryMapViewState& state,
    std::shared_ptr<const aida::binary_map::map_t> map,
    std::shared_ptr<const qt_binary_map_live_snapshot_t> live,
    qt_binary_map_active_mode_t mode) {
    beginResetModel();
    rows_.clear();
    const std::string& filter = state.filter_lower;
    const bool want_live = mode == qt_binary_map_active_mode_t::live_process ||
        mode == qt_binary_map_active_mode_t::merged;
    const bool want_static = mode == qt_binary_map_active_mode_t::pe_static ||
        mode == qt_binary_map_active_mode_t::merged;
    if (want_live && live) {
        if (state.filtered_live_identity != live.get() ||
            state.filtered_live_query != filter) {
            state.filtered_live_identity = live.get();
            state.filtered_live_query = filter;
            state.filtered_live_indices.clear();
            state.filtered_live_indices.reserve(live->regions.size());
            for (std::size_t i = 0; i < live->regions.size(); ++i) {
                const auto& region = live->regions[i];
                if (filter.empty() ||
                    bm_filter_matches(filter, region.module_name) ||
                    bm_filter_matches(filter, bm_region_kind_label(region)) ||
                    bm_filter_matches(filter, region.info) ||
                    bm_filter_matches(filter, bm_format_protect_word(region.protect))) {
                    if (i <= static_cast<std::size_t>(
                            (std::numeric_limits<int>::max)()))
                        state.filtered_live_indices.push_back(static_cast<int>(i));
                }
            }
        }
        list_row_t header;
        header.kind = row_kind_t::group_header;
        header.title = "Regions";
        header.count = static_cast<int>(state.filtered_live_indices.size());
        header.group_key = "regions";
        header.collapsed = group_collapsed(state, header.group_key);
        rows_.push_back(header);
        if (!header.collapsed) {
            for (const int source : state.filtered_live_indices) {
                if (source < 0 ||
                    static_cast<std::size_t>(source) >= live->regions.size())
                    continue;
                list_row_t row;
                row.kind = row_kind_t::region;
                row.region = live->regions[static_cast<std::size_t>(source)];
                rows_.push_back(std::move(row));
            }
        }
        list_row_t modules_header;
        modules_header.kind = row_kind_t::group_header;
        modules_header.title = "Modules";
        modules_header.count = static_cast<int>(live->modules.size());
        modules_header.group_key = "modules";
        modules_header.collapsed = group_collapsed(state, modules_header.group_key);
        rows_.push_back(modules_header);
        if (!modules_header.collapsed) {
            for (const auto& m : live->modules) {
                if (!bm_filter_matches(filter, m.name)) continue;
                list_row_t row;
                row.kind = row_kind_t::module;
                row.module = m;
                rows_.push_back(std::move(row));
            }
        }
    }
    if (want_static && map) {
        const auto add_header = [&](const char* title, int count,
            const char* key) -> bool {
            list_row_t header;
            header.kind = row_kind_t::group_header;
            header.title = title;
            header.count = count;
            header.group_key = key;
            header.collapsed = group_collapsed(state, header.group_key);
            rows_.push_back(header);
            return !header.collapsed;
        };
        if (add_header("Sections", static_cast<int>(map->sections.size()), "sections")) {
            for (const auto& section : map->sections) {
                if (!bm_filter_matches(filter, section.name)) continue;
                list_row_t row;
                row.kind = row_kind_t::section;
                row.section = section;
                rows_.push_back(std::move(row));
            }
        }
        if (add_header("Functions", static_cast<int>(map->functions.size()),
                "functions")) {
            for (const auto& fn : map->functions) {
                if (!bm_filter_matches(filter, fn.name)) continue;
                list_row_t row;
                row.kind = row_kind_t::function;
                row.function = fn;
                rows_.push_back(std::move(row));
            }
        }
        if (add_header("Globals", static_cast<int>(map->globals.size()), "globals")) {
            for (const auto& global : map->globals) {
                if (!bm_filter_matches(filter, global.name)) continue;
                list_row_t row;
                row.kind = row_kind_t::global;
                row.global = global;
                rows_.push_back(std::move(row));
            }
        }
        if (add_header("Imports", static_cast<int>(map->imports.size()), "imports")) {
            for (const auto& imp : map->imports) {
                const auto colon = imp.find(':');
                const std::string dll = colon == std::string::npos
                    ? imp : imp.substr(0, colon);
                std::string func_list;
                if (colon != std::string::npos && colon + 1 < imp.size()) {
                    std::size_t start = colon + 1;
                    while (start < imp.size() && imp[start] == ' ') ++start;
                    func_list = imp.substr(start);
                }
                std::vector<std::string> funcs;
                std::size_t pos = 0;
                while (pos < func_list.size()) {
                    std::size_t next = func_list.find(',', pos);
                    if (next == std::string::npos) next = func_list.size();
                    std::string token = func_list.substr(pos, next - pos);
                    while (!token.empty() && token.front() == ' ') token.erase(token.begin());
                    while (!token.empty() && token.back() == ' ') token.pop_back();
                    if (!token.empty()) funcs.push_back(std::move(token));
                    pos = next + 1u;
                }
                bool any_match = bm_filter_matches(filter, dll);
                if (!any_match) {
                    for (const auto& fn : funcs) {
                        if (bm_filter_matches(filter, fn)) {
                            any_match = true;
                            break;
                        }
                    }
                }
                if (!any_match) continue;
                const std::string key = std::string("imports::") + dll;
                const bool collapsed = group_collapsed(state, key);
                list_row_t dll_row;
                dll_row.kind = row_kind_t::import_dll;
                dll_row.dll = dll;
                dll_row.count = static_cast<int>(funcs.size());
                dll_row.group_key = key;
                dll_row.collapsed = collapsed;
                rows_.push_back(dll_row);
                if (!collapsed) {
                    for (const auto& fn : funcs) {
                        if (!bm_filter_matches(filter, fn) &&
                            !bm_filter_matches(filter, dll))
                            continue;
                        list_row_t fn_row;
                        fn_row.kind = row_kind_t::import_function;
                        fn_row.dll = dll;
                        fn_row.function_name = fn;
                        rows_.push_back(std::move(fn_row));
                    }
                }
            }
        }
        if (add_header("Exports", static_cast<int>(map->exports.size()), "exports")) {
            for (const auto& ex : map->exports) {
                if (!bm_filter_matches(filter, ex)) continue;
                list_row_t row;
                row.kind = row_kind_t::export_entry;
                row.export_name = ex;
                for (const auto& fn : map->functions) {
                    if (fn.name == ex) {
                        row.export_va = fn.va;
                        break;
                    }
                }
                rows_.push_back(std::move(row));
            }
        }
    }
    endResetModel();
}

const QtBinaryMapListModel::list_row_t* QtBinaryMapListModel::rowAt(
    int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

int QtBinaryMapListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int QtBinaryMapListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 1;
}

QVariant QtBinaryMapListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    const auto* row = rowAt(index.row());
    if (!row) return {};
    if (role == Qt::DisplayRole) {
        switch (row->kind) {
        case row_kind_t::group_header:
            return QStringLiteral("%1  (%2)")
                .arg(QString::fromStdString(row->title)).arg(row->count);
        case row_kind_t::region: {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "0x%012llX  %s  %s",
                static_cast<unsigned long long>(row->region.base),
                bm_format_size_human(row->region.size).c_str(),
                bm_region_kind_label(row->region).c_str());
            return QString::fromLatin1(buf);
        }
        case row_kind_t::module: {
            char buf[24]{};
            std::snprintf(buf, sizeof(buf), "0x%012llX",
                static_cast<unsigned long long>(row->module.base));
            return QStringLiteral("%1  %2  %3")
                .arg(QString::fromLatin1(buf))
                .arg(QString::fromStdString(row->module.name))
                .arg(QString::fromStdString(
                    bm_format_size_human(row->module.size)));
        }
        case row_kind_t::section: {
            char buf[24]{};
            std::snprintf(buf, sizeof(buf), "0x%llX",
                static_cast<unsigned long long>(row->section.va));
            return QStringLiteral("%1  %2  %3")
                .arg(QString::fromStdString(row->section.name))
                .arg(QString::fromLatin1(buf))
                .arg(QString::fromStdString(
                    bm_format_size_human(row->section.size)));
        }
        case row_kind_t::function: {
            char buf[24]{};
            std::snprintf(buf, sizeof(buf), "0x%llX",
                static_cast<unsigned long long>(row->function.va));
            return QStringLiteral("%1  %2  x%3  c%4")
                .arg(QString::fromStdString(row->function.name))
                .arg(QString::fromLatin1(buf))
                .arg(row->function.xref_count)
                .arg(row->function.callee_count);
        }
        case row_kind_t::global: {
            char buf[24]{};
            std::snprintf(buf, sizeof(buf), "0x%llX",
                static_cast<unsigned long long>(row->global.va));
            return QStringLiteral("%1  %2  x%3  %4")
                .arg(QString::fromStdString(row->global.name))
                .arg(QString::fromLatin1(buf))
                .arg(row->global.xref_count)
                .arg(row->global.writable ? QStringLiteral("rw")
                    : QStringLiteral("ro"));
        }
        case row_kind_t::import_dll:
            return QStringLiteral("%1  (%2)")
                .arg(QString::fromStdString(row->dll)).arg(row->count);
        case row_kind_t::import_function:
            return QString::fromStdString(row->function_name);
        case row_kind_t::export_entry:
            return QString::fromStdString(row->export_name);
        }
        return {};
    }
    if (role == Qt::UserRole) return static_cast<int>(row->kind);
    if (role == Qt::UserRole + 1) return row->collapsed;
    if (role == Qt::UserRole + 2)
        return QString::fromStdString(row->group_key);
    if (role == Qt::FontRole) {
        if (row->kind == row_kind_t::group_header ||
            row->kind == row_kind_t::import_dll)
            return aida::qt::theme::fonts::bodyEm();
        return aida::qt::theme::fonts::codeRegular();
    }
    if (role == Qt::ForegroundRole) {
        if (row->kind == row_kind_t::group_header)
            return aida::qt::theme::tokens().text_secondary;
        if (row->kind == row_kind_t::import_dll)
            return aida::qt::theme::tokens().text_primary;
        return {};
    }
    if (role == Qt::ToolTipRole)
        return data(index, Qt::DisplayRole);
    return {};
}

void QtBinaryMapListModel::multiData(const QModelIndex& index,
                                     QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan) {
        switch (roleData.role()) {
        case Qt::DisplayRole:
        case Qt::UserRole:
        case Qt::UserRole + 1:
        case Qt::UserRole + 2:
        case Qt::FontRole:
        case Qt::ForegroundRole:
        case Qt::ToolTipRole:
            roleData.setData(data(index, roleData.role()));
            break;
        default:
            roleData.clearData();
            break;
        }
    }
}

}
