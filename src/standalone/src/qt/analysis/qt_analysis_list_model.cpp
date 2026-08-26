#include "qt/analysis/qt_analysis_list_model.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

namespace aida::qt::analysis {

std::string analysis_list_row_identity(const analysis_list_row_t& row) {
    return std::string(row.has_address ? "address:" : "entity:") +
        std::to_string(row.address) + ":" + row.name + ":" + row.context + ":" +
        row.detail;
}

std::string analysis_list_address_text(std::uint64_t address) {
    char value[32]{};
    std::snprintf(value, sizeof(value), "0x%016llX",
        static_cast<unsigned long long>(address));
    return value;
}

const analysis_list_descriptor_t& analysis_list_descriptor(
    analysis_list_domain_t domain) noexcept {
    static constexpr std::array<analysis_list_descriptor_t, analysis_list_domain_count>
        values{{
            {"view.analysis.imports", "Imports", "No imports",
                "The analyzed image does not publish imported symbols."},
            {"view.analysis.exports", "Exports", "No exports",
                "The analyzed image does not publish exported symbols."},
            {"view.analysis.names", "Names", "No names",
                "Analysis has not published named symbols for this target."},
            {"view.analysis.strings", "Strings", "No strings",
                "Analysis has not discovered strings for this target."},
            {"view.analysis.segments", "Segments", "No segments",
                "The normalized image does not publish segment records."},
            {"view.analysis.local_types", "Local Types", "No local types",
                "Analysis has not published local type candidates."}
        }};
    return values[static_cast<std::size_t>(domain)];
}

namespace {

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains_case_insensitive(const std::string& value, const std::string& query) {
    if (query.empty()) return true;
    if (query.size() > value.size()) return false;
    return std::search(value.begin(), value.end(), query.begin(), query.end(),
        [](unsigned char left, unsigned char right) {
            return std::tolower(left) == std::tolower(right);
        }) != value.end();
}

int compare_text(const std::string& left, const std::string& right) {
    const std::size_t shared = (std::min)(left.size(), right.size());
    for (std::size_t index = 0; index < shared; ++index) {
        const int lhs = std::tolower(static_cast<unsigned char>(left[index]));
        const int rhs = std::tolower(static_cast<unsigned char>(right[index]));
        if (lhs < rhs) return -1;
        if (lhs > rhs) return 1;
    }
    if (left.size() < right.size()) return -1;
    if (left.size() > right.size()) return 1;
    return 0;
}

const char* string_encoding_name(aida::analysis::string_encoding_t encoding) {
    switch (encoding) {
    case aida::analysis::string_encoding_t::ascii: return "ASCII";
    case aida::analysis::string_encoding_t::utf8: return "UTF-8";
    case aida::analysis::string_encoding_t::utf16_le: return "UTF-16 LE";
    default: return "Unknown";
    }
}

const char* symbol_kind_name(aida::analysis::symbol_kind_t kind) {
    switch (kind) {
    case aida::analysis::symbol_kind_t::function: return "Function";
    case aida::analysis::symbol_kind_t::data: return "Data";
    case aida::analysis::symbol_kind_t::import_symbol: return "Import";
    case aida::analysis::symbol_kind_t::export_symbol: return "Export";
    case aida::analysis::symbol_kind_t::debug_symbol: return "Debug";
    case aida::analysis::symbol_kind_t::type_symbol: return "Type";
    case aida::analysis::symbol_kind_t::metadata: return "Metadata";
    default: return "Unknown";
    }
}

const char* type_kind_name(aida::analysis::symbol_type_candidate_kind_t kind) {
    using kind_t = aida::analysis::symbol_type_candidate_kind_t;
    switch (kind) {
    case kind_t::function_prototype: return "Function prototype";
    case kind_t::import_prototype: return "Import prototype";
    case kind_t::global_object: return "Global object";
    case kind_t::pointer_object: return "Pointer object";
    case kind_t::rtti_type: return "RTTI type";
    case kind_t::virtual_table: return "Virtual table";
    case kind_t::type_information: return "Type information";
    case kind_t::objective_c_class: return "Objective-C class";
    case kind_t::objective_c_protocol: return "Objective-C protocol";
    case kind_t::objective_c_selector: return "Objective-C selector";
    case kind_t::swift_type: return "Swift type";
    case kind_t::swift_protocol: return "Swift protocol";
    case kind_t::managed_type: return "Managed type";
    case kind_t::managed_method: return "Managed method";
    case kind_t::managed_field: return "Managed field";
    case kind_t::debug_type: return "Debug type";
    case kind_t::metadata_region: return "Metadata region";
    default: return "Unknown";
    }
}

std::string segment_permissions(std::uint32_t permissions) {
    std::string value;
    value.push_back((permissions & aida::analysis::image_permission_read) != 0 ? 'R' : '-');
    value.push_back((permissions & aida::analysis::image_permission_write) != 0 ? 'W' : '-');
    value.push_back((permissions & aida::analysis::image_permission_execute) != 0 ? 'X' : '-');
    return value;
}

std::uint64_t runtime_address_value(const disasm_view::workspace_context_t& context,
                                    const aida::analysis::address_t& address,
                                    const std::optional<std::uint64_t>& display_base) {
    return disasm_view::runtime_address_with_base(context, address, display_base)
        .value_or(address.value);
}

}

std::vector<analysis_list_row_t> analysis_list_project_rows(
    analysis_list_domain_t domain,
    const disasm_view::workspace_context_t& context,
    const std::optional<std::uint64_t>& display_base) {
    const auto publication = context.publication;
    std::vector<analysis_list_row_t> rows;
    if (!publication || !publication->snapshot) return rows;
    const auto& snapshot = *publication->snapshot;
    const auto normalized = snapshot.normalized_image;
    if (domain == analysis_list_domain_t::imports && normalized) {
        rows.reserve(normalized->imports.size());
        for (const auto& item : normalized->imports) {
            const auto& source = item.address.value != 0 ? item.address : item.lookup_address;
            analysis_list_row_t row;
            row.address = runtime_address_value(context, source, display_base);
            row.has_address = row.address != 0;
            row.name = item.name.value_or(item.ordinal
                ? "Ordinal " + std::to_string(*item.ordinal) : "Unnamed import");
            row.context = item.library;
            row.detail = item.delayed ? "Delay-loaded" : "Import";
            rows.push_back(std::move(row));
        }
    } else if (domain == analysis_list_domain_t::exports && normalized) {
        rows.reserve(normalized->exports.size());
        for (const auto& item : normalized->exports) {
            analysis_list_row_t row;
            row.address = runtime_address_value(context, item.address, display_base);
            row.has_address = row.address != 0;
            row.name = item.name.value_or("Ordinal " + std::to_string(item.ordinal));
            row.context = "Ordinal " + std::to_string(item.ordinal);
            row.detail = item.forwarder.value_or("Export");
            rows.push_back(std::move(row));
        }
    } else if (domain == analysis_list_domain_t::names) {
        rows.reserve(snapshot.symbols.size());
        for (const auto& item : snapshot.symbols) {
            analysis_list_row_t row;
            row.address = runtime_address_value(context, item.address, display_base);
            row.has_address = row.address != 0;
            row.name = disasm_view::resolve_name(context, item.address);
            if (row.name.empty()) row.name = item.name.empty() ? "Unnamed symbol" : item.name;
            row.context = symbol_kind_name(item.kind);
            row.detail = "Confidence " + std::to_string(item.confidence) + "%";
            rows.push_back(std::move(row));
        }
    } else if (domain == analysis_list_domain_t::strings) {
        rows.reserve(snapshot.strings.size());
        for (const auto& item : snapshot.strings) {
            analysis_list_row_t row;
            row.address = runtime_address_value(context, item.address, display_base);
            row.has_address = row.address != 0;
            row.name = item.value;
            row.context = string_encoding_name(item.encoding);
            row.detail = std::to_string(item.byte_length) + " bytes";
            rows.push_back(std::move(row));
        }
    } else if (domain == analysis_list_domain_t::segments && normalized) {
        rows.reserve(normalized->segments.size());
        for (const auto& item : normalized->segments) {
            analysis_list_row_t row;
            row.address = normalized->image_base + item.virtual_address;
            row.has_address = true;
            row.name = item.name.empty() ? "Segment " + std::to_string(item.index) : item.name;
            row.context = segment_permissions(item.permissions);
            char detail[96]{};
            std::snprintf(detail, sizeof(detail), "VA size 0x%llX | file 0x%llX + 0x%llX",
                static_cast<unsigned long long>(item.virtual_size),
                static_cast<unsigned long long>(item.file_offset),
                static_cast<unsigned long long>(item.file_size));
            row.detail = detail;
            rows.push_back(std::move(row));
        }
    } else if (domain == analysis_list_domain_t::local_types) {
        rows.reserve(snapshot.rich_facts.type_candidates.size());
        for (const auto& item : snapshot.rich_facts.type_candidates) {
            analysis_list_row_t row;
            if (item.address) {
                row.address = runtime_address_value(context, *item.address, display_base);
                row.has_address = row.address != 0;
            }
            row.name = item.display_name.empty() ? item.canonical_type : item.display_name;
            if (row.name.empty()) row.name = "Unnamed type";
            row.context = type_kind_name(item.kind);
            row.detail = item.canonical_type.empty()
                ? "Confidence " + std::to_string(item.confidence) + "%"
                : item.canonical_type;
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

std::vector<std::size_t> analysis_list_compute_visible(
    const std::vector<analysis_list_row_t>& rows,
    const std::string& filter_lower, int column, bool ascending) {
    std::vector<std::size_t> visible;
    visible.reserve(rows.size());
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        const bool match = filter_lower.empty() ||
            contains_case_insensitive(row.name, filter_lower) ||
            contains_case_insensitive(row.context, filter_lower) ||
            contains_case_insensitive(row.detail, filter_lower) ||
            (row.has_address &&
                lower_copy(analysis_list_address_text(row.address)).find(filter_lower) !=
                    std::string::npos);
        if (match) visible.push_back(index);
    }
    std::stable_sort(visible.begin(), visible.end(), [&](std::size_t left, std::size_t right) {
        const auto& lhs = rows[left];
        const auto& rhs = rows[right];
        int result = 0;
        if (column == 0) {
            if (lhs.address < rhs.address) result = -1;
            else if (lhs.address > rhs.address) result = 1;
        } else if (column == 1) result = compare_text(lhs.name, rhs.name);
        else if (column == 2) result = compare_text(lhs.context, rhs.context);
        else result = compare_text(lhs.detail, rhs.detail);
        if (result == 0) result = left < right ? -1 : (left > right ? 1 : 0);
        return ascending ? result < 0 : result > 0;
    });
    return visible;
}

QtAnalysisListModel::QtAnalysisListModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtAnalysisListModel::setSnapshot(
    std::shared_ptr<const analysis_list_snapshot_t> snapshot) {
    beginResetModel();
    snapshot_ = std::move(snapshot);
    if (!snapshot_) snapshot_ = std::make_shared<const analysis_list_snapshot_t>();
    endResetModel();
}

int QtAnalysisListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(snapshot_->visible.size());
}

int QtAnalysisListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
}

const analysis_list_row_t* QtAnalysisListModel::rowAt(int view_row) const noexcept {
    if (view_row < 0 || static_cast<std::size_t>(view_row) >= snapshot_->visible.size())
        return nullptr;
    const std::size_t source = snapshot_->visible[static_cast<std::size_t>(view_row)];
    if (source >= snapshot_->rows.size()) return nullptr;
    return &snapshot_->rows[source];
}

std::size_t QtAnalysisListModel::sourceIndexForViewRow(int view_row) const noexcept {
    if (view_row < 0 || static_cast<std::size_t>(view_row) >= snapshot_->visible.size())
        return static_cast<std::size_t>(-1);
    return snapshot_->visible[static_cast<std::size_t>(view_row)];
}

QVariant QtAnalysisListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.parent().isValid()) return {};
    const auto* row = rowAt(index.row());
    if (!row) return {};
    const auto column = static_cast<Column>(index.column());
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Column::address:
            return row->has_address
                ? QString::fromStdString(analysis_list_address_text(row->address))
                : QStringLiteral("-");
        case Column::name: return QString::fromStdString(row->name);
        case Column::kind: return QString::fromStdString(row->context);
        case Column::details: return QString::fromStdString(row->detail);
        default: return {};
        }
    }
    if (role == Qt::TextAlignmentRole) {
        if (column == Column::address)
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (role == Qt::ToolTipRole) {
        if (column == Column::name && !row->detail.empty())
            return QString::fromStdString(row->name) + QStringLiteral("\n") +
                QString::fromStdString(row->detail);
        return data(index, Qt::DisplayRole);
    }
    if (role == Qt::FontRole && column == Column::address)
        return aida::qt::theme::fonts::codeRegular();
    if (role == Qt::ForegroundRole && column == Column::address)
        return aida::qt::theme::tokens().syn_address;
    return {};
}

void QtAnalysisListModel::multiData(const QModelIndex& index,
                                     QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan) {
        switch (roleData.role()) {
        case Qt::DisplayRole:
        case Qt::TextAlignmentRole:
        case Qt::ToolTipRole:
        case Qt::FontRole:
        case Qt::ForegroundRole:
            roleData.setData(data(index, roleData.role()));
            break;
        default:
            roleData.clearData();
            break;
        }
    }
}

QVariant QtAnalysisListModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (static_cast<Column>(section)) {
    case Column::address: return QStringLiteral("Address");
    case Column::name: return QStringLiteral("Name");
    case Column::kind: return QStringLiteral("Kind / Source");
    case Column::details: return QStringLiteral("Details");
    default: return {};
    }
}

}
