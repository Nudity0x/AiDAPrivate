#include "qt/analysis/qt_types_catalog_model.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

#include "core/disasm/disasm_view.hpp"
#include "core/session/analysis_session.hpp"
#include "core/analysis/symbol_store.hpp"
#include "core/analysis/workspace/search_index.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>

namespace aida::qt::analysis {

namespace {

std::string provenance_name(aida::analysis::fact_provenance_t provenance) {
    using aida::analysis::fact_provenance_t;
    switch (provenance) {
    case fact_provenance_t::gap_recovery: return "gap recovery";
    case fact_provenance_t::linear_validation: return "validated sweep";
    case fact_provenance_t::recursive_decode: return "recursive traversal";
    case fact_provenance_t::relocation: return "relocation";
    case fact_provenance_t::call_target: return "call target";
    case fact_provenance_t::export_entry: return "export";
    case fact_provenance_t::tls_entry: return "TLS";
    case fact_provenance_t::image_entry: return "entry point";
    case fact_provenance_t::unwind_metadata: return "unwind";
    case fact_provenance_t::debug_symbol: return "debug symbol";
    case fact_provenance_t::user_definition: return "user";
    case fact_provenance_t::decompiler_feedback: return "decompiler";
    case fact_provenance_t::unknown: return "unknown";
    }
    return "unknown";
}

bool contains_case_insensitive(const std::string& text, const std::string& filter) {
    if (filter.empty())
        return true;
    return std::search(text.begin(), text.end(), filter.begin(), filter.end(),
        [](char left, char right) {
            return std::tolower(static_cast<unsigned char>(left)) ==
                   std::tolower(static_cast<unsigned char>(right));
        }) != text.end();
}

}

qt_type_catalog_t qt_build_type_catalog(const disasm_view::workspace_context_t& context) {
    qt_type_catalog_t catalog;
    if (auto symbols = context.workspace
            ? analysis_session::symbols_for_workspace(context.workspace) : nullptr) {
        const auto modules = symbols->modules_snapshot();
        for (const auto& module : modules) {
            const auto& pdb = module.second.pdb;
            if (!pdb.loaded)
                continue;
            for (const auto& definition : pdb.structs) {
                qt_struct_entry_t entry{module.first, definition};
                if (definition.is_union)
                    catalog.unions.push_back(std::move(entry));
                else
                    catalog.structs.push_back(std::move(entry));
            }
            for (const auto& definition : pdb.enums)
                catalog.enums.push_back({module.first, definition});
        }
    }
    if (context.publication && context.publication->snapshot) {
        const auto& snapshot = *context.publication->snapshot;
        catalog.functions.reserve(snapshot.functions.size());
        for (const auto& function : snapshot.functions) {
            qt_function_type_entry_t entry;
            entry.address = function.start;
            entry.size = function.end.value >= function.start.value
                ? function.end.value - function.start.value : 0;
            entry.name = disasm_view::resolve_name(context, function.start);
            if (entry.name.empty()) {
                char generated[48]{};
                std::snprintf(generated, sizeof(generated), "sub_%llX",
                    static_cast<unsigned long long>(
                        disasm_view::runtime_address(context, function.start).value_or(
                            function.start.value)));
                entry.name = generated;
            }
            entry.provenance = provenance_name(function.provenance);
            catalog.functions.push_back(std::move(entry));
        }
    }
    if (context.publication && context.publication->search_index) {
        for (const auto& candidate : context.publication->search_index->types()) {
            qt_typedef_entry_t entry;
            entry.address = candidate.address;
            entry.name = candidate.display_name;
            entry.canonical_type = candidate.canonical_type;
            entry.explicitly_unknown = candidate.explicitly_unknown;
            entry.confidence = candidate.confidence;
            catalog.typedefs.push_back(std::move(entry));
        }
    }
    for (auto& function : catalog.functions) {
        auto evidence = std::find_if(catalog.typedefs.begin(), catalog.typedefs.end(),
            [&](const qt_typedef_entry_t& candidate) {
                return candidate.address == function.address && !candidate.explicitly_unknown &&
                    !candidate.canonical_type.empty();
            });
        function.signature = evidence != catalog.typedefs.end()
            ? evidence->canonical_type : "unknown " + function.name + "(unknown)";
    }
    auto struct_order = [](const qt_struct_entry_t& left, const qt_struct_entry_t& right) {
        if (left.definition.name != right.definition.name)
            return left.definition.name < right.definition.name;
        return left.module < right.module;
    };
    std::sort(catalog.structs.begin(), catalog.structs.end(), struct_order);
    std::sort(catalog.unions.begin(), catalog.unions.end(), struct_order);
    std::sort(catalog.enums.begin(), catalog.enums.end(), [](const auto& left, const auto& right) {
        if (left.definition.name != right.definition.name)
            return left.definition.name < right.definition.name;
        return left.module < right.module;
    });
    return catalog;
}

std::vector<std::size_t> qt_filter_type_catalog(const qt_type_catalog_t& catalog,
    qt_types_tab_t tab, const std::string& filter) {
    std::vector<std::size_t> visible;
    if (tab == qt_types_tab_t::structures || tab == qt_types_tab_t::unions_) {
        const auto& entries = tab == qt_types_tab_t::structures
            ? catalog.structs : catalog.unions;
        visible.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (contains_case_insensitive(entries[index].definition.name, filter) ||
                contains_case_insensitive(entries[index].module, filter))
                visible.push_back(index);
        }
    } else if (tab == qt_types_tab_t::enums) {
        visible.reserve(catalog.enums.size());
        for (std::size_t index = 0; index < catalog.enums.size(); ++index) {
            if (contains_case_insensitive(catalog.enums[index].definition.name, filter))
                visible.push_back(index);
        }
    } else if (tab == qt_types_tab_t::functions) {
        visible.reserve(catalog.functions.size());
        for (std::size_t index = 0; index < catalog.functions.size(); ++index) {
            if (contains_case_insensitive(catalog.functions[index].name, filter))
                visible.push_back(index);
        }
    } else {
        visible.reserve(catalog.typedefs.size());
        for (std::size_t index = 0; index < catalog.typedefs.size(); ++index) {
            if (contains_case_insensitive(catalog.typedefs[index].name, filter) ||
                contains_case_insensitive(catalog.typedefs[index].canonical_type, filter))
                visible.push_back(index);
        }
    }
    return visible;
}

std::vector<qt_type_reference_t> qt_type_references_for(
    const qt_type_catalog_t& catalog, const std::string& type_name) {
    std::vector<qt_type_reference_t> references;
    if (type_name.empty())
        return references;
    for (const auto& function : catalog.functions) {
        if (function.signature.find(type_name) != std::string::npos)
            references.push_back({function.address,
                function.name + "  " + function.signature});
    }
    for (const auto& candidate : catalog.typedefs) {
        if (candidate.canonical_type.find(type_name) != std::string::npos)
            references.push_back({candidate.address,
                candidate.name + "  " + candidate.canonical_type});
    }
    std::sort(references.begin(), references.end(), [](const auto& left, const auto& right) {
        if (left.address != right.address)
            return left.address < right.address;
        return left.label < right.label;
    });
    references.erase(std::unique(references.begin(), references.end(),
        [](const auto& left, const auto& right) {
            return left.address == right.address && left.label == right.label;
        }), references.end());
    return references;
}

std::string qt_struct_to_ida_syntax(const pdb_parser::struct_def_t& definition) {
    constexpr std::size_t maximum_output = 64U * 1024U;
    std::string output = definition.is_union ? "union " : "struct ";
    output += definition.name + "\n{\n";
    if (output.size() > maximum_output)
        return {};
    std::uint64_t last_end = 0;
    int padding_index = 0;
    for (const auto& member : definition.members) {
        if (member.offset > last_end) {
            char padding[96]{};
            std::snprintf(padding, sizeof(padding), "  _BYTE pad_%d[%llu];\n",
                padding_index++, static_cast<unsigned long long>(member.offset - last_end));
            output += padding;
            if (output.size() > maximum_output)
                return {};
        }
        std::string type = member.type_name;
        if (type == "uint8_t" || type == "int8_t" || type == "char" || type == "BYTE")
            type = "_BYTE";
        else if (type == "uint16_t" || type == "int16_t" || type == "WORD" ||
                 type == "USHORT" || type == "short")
            type = "_WORD";
        else if (type == "uint32_t" || type == "int32_t" || type == "DWORD" ||
                 type == "ULONG" || type == "LONG" || type == "long")
            type = "_DWORD";
        else if (type == "uint64_t" || type == "int64_t" || type == "QWORD" ||
                 type == "ULONGLONG" || type == "__int64")
            type = "_QWORD";
        char line[256]{};
        if (member.bit_size >= 0) {
            std::snprintf(line, sizeof(line), "  %s %s : %d;\n",
                type.c_str(), member.name.c_str(), member.bit_size);
        } else if (member.is_array) {
            std::snprintf(line, sizeof(line), "  %s %s[%d];\n",
                type.c_str(), member.name.c_str(), member.array_count);
        } else {
            std::snprintf(line, sizeof(line), "  %s %s;\n",
                type.c_str(), member.name.c_str());
        }
        output += line;
        if (output.size() > maximum_output)
            return {};
        last_end = member.size > (std::numeric_limits<std::uint64_t>::max)() - member.offset
            ? (std::numeric_limits<std::uint64_t>::max)() : member.offset + member.size;
    }
    if (last_end < definition.size) {
        char padding[96]{};
        std::snprintf(padding, sizeof(padding), "  _BYTE pad_%d[%llu];\n", padding_index,
            static_cast<unsigned long long>(definition.size - last_end));
        output += padding;
        if (output.size() > maximum_output)
            return {};
    }
    output += "};\n";
    return output.size() <= maximum_output ? output : std::string{};
}

std::string qt_struct_to_c_bounded(const pdb_parser::struct_def_t& definition) {
    constexpr std::size_t maximum_output = 64U * 1024U;
    if (definition.members.size() > 65536 || definition.name.size() > maximum_output)
        return {};
    std::string declaration = definition.is_union ? "union " : "struct ";
    const auto append = [&declaration](const char* text, std::size_t length) {
        if (length > maximum_output - declaration.size())
            return false;
        declaration.append(text, length);
        return true;
    };
    if (!append(definition.name.data(), definition.name.size()) ||
        !append(" {\n", 3))
        return {};
    std::uint64_t last_end = 0;
    int padding_index = 0;
    for (const auto& member : definition.members) {
        char line[256]{};
        if (member.offset > last_end) {
            const int length = std::snprintf(line, sizeof(line),
                "    uint8_t _pad%d[%llu];\n", padding_index++,
                static_cast<unsigned long long>(member.offset - last_end));
            if (length < 0 || !append(line, (std::min)(
                static_cast<std::size_t>(length), sizeof(line) - 1)))
                return {};
        }
        int length = 0;
        if (member.bit_size >= 0)
            length = std::snprintf(line, sizeof(line), "    %s %s : %d;\n",
                member.type_name.c_str(), member.name.c_str(), member.bit_size);
        else if (member.is_array)
            length = std::snprintf(line, sizeof(line), "    %s %s[%d];\n",
                member.type_name.c_str(), member.name.c_str(), member.array_count);
        else
            length = std::snprintf(line, sizeof(line), "    %s %s;\n",
                member.type_name.c_str(), member.name.c_str());
        if (length < 0 || !append(line, (std::min)(
            static_cast<std::size_t>(length), sizeof(line) - 1)))
            return {};
        last_end = member.size > (std::numeric_limits<std::uint64_t>::max)() - member.offset
            ? (std::numeric_limits<std::uint64_t>::max)() : member.offset + member.size;
    }
    if (last_end < definition.size) {
        char padding[96]{};
        const int length = std::snprintf(padding, sizeof(padding),
            "    uint8_t _pad%d[%llu];\n", padding_index,
            static_cast<unsigned long long>(definition.size - last_end));
        if (length < 0 || !append(padding, (std::min)(
            static_cast<std::size_t>(length), sizeof(padding) - 1)))
            return {};
    }
    char footer[96]{};
    const int footer_length = std::snprintf(footer, sizeof(footer),
        "}; // size: 0x%llX (%llu bytes)\n",
        static_cast<unsigned long long>(definition.size),
        static_cast<unsigned long long>(definition.size));
    if (footer_length < 0 || !append(footer, (std::min)(
        static_cast<std::size_t>(footer_length), sizeof(footer) - 1)))
        return {};
    return declaration;
}

std::string qt_enum_to_c(const pdb_parser::enum_def_t& definition) {
    constexpr std::size_t maximum_output = 64U * 1024U;
    if (definition.name.size() > maximum_output || definition.members.size() > 65536)
        return {};
    std::string declaration;
    const auto append = [&declaration](const std::string& value) {
        if (value.size() > maximum_output - declaration.size())
            return false;
        declaration.append(value);
        return true;
    };
    if (!append("enum ") || !append(definition.name) || !append(" {\n"))
        return {};
    for (const auto& member : definition.members) {
        if (!append("    ") || !append(member.name) || !append(" = ") ||
            !append(std::to_string(member.value)) || !append(",\n"))
            return {};
    }
    return append("};\n") ? declaration : std::string{};
}

std::string qt_canonical_record_name(std::string name) {
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
        name.pop_back();
    while (!name.empty() && name.back() == '*') {
        name.pop_back();
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
            name.pop_back();
    }
    if (name.rfind("struct ", 0) == 0)
        name.erase(0, 7);
    else if (name.rfind("union ", 0) == 0)
        name.erase(0, 6);
    return name;
}

const pdb_parser::struct_def_t* qt_catalog_record(const qt_type_catalog_t& catalog,
                                                  const std::string& name) {
    const std::string canonical = qt_canonical_record_name(name);
    for (const auto& entry : catalog.structs)
        if (entry.definition.name == canonical)
            return &entry.definition;
    for (const auto& entry : catalog.unions)
        if (entry.definition.name == canonical)
            return &entry.definition;
    return nullptr;
}

const pdb_parser::enum_def_t* qt_catalog_enum(const qt_type_catalog_t& catalog,
                                              const std::string& name) {
    const std::string canonical = qt_canonical_record_name(name);
    for (const auto& entry : catalog.enums)
        if (entry.definition.name == canonical)
            return &entry.definition;
    return nullptr;
}

QtTypesCatalogModel::QtTypesCatalogModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtTypesCatalogModel::setTab(qt_types_tab_t tab) {
    if (tab_ == tab) return;
    beginResetModel();
    tab_ = tab;
    visible_.reset();
    endResetModel();
}

void QtTypesCatalogModel::setContent(
    std::shared_ptr<const qt_type_catalog_t> catalog,
    std::shared_ptr<const std::vector<std::size_t>> visible) {
    beginResetModel();
    catalog_ = std::move(catalog);
    visible_ = std::move(visible);
    endResetModel();
}

int QtTypesCatalogModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || !visible_) return 0;
    return static_cast<int>(visible_->size());
}

int QtTypesCatalogModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    switch (tab_) {
    case qt_types_tab_t::structures:
    case qt_types_tab_t::unions_: return 3;
    case qt_types_tab_t::enums: return 3;
    case qt_types_tab_t::functions: return 4;
    case qt_types_tab_t::typedefs: return 3;
    default: return 1;
    }
}

std::string QtTypesCatalogModel::nameAt(int view_row) const {
    if (!catalog_ || !visible_ || view_row < 0 ||
        static_cast<std::size_t>(view_row) >= visible_->size())
        return {};
    const std::size_t index = (*visible_)[static_cast<std::size_t>(view_row)];
    switch (tab_) {
    case qt_types_tab_t::structures:
        return index < catalog_->structs.size()
            ? catalog_->structs[index].definition.name : std::string{};
    case qt_types_tab_t::unions_:
        return index < catalog_->unions.size()
            ? catalog_->unions[index].definition.name : std::string{};
    case qt_types_tab_t::enums:
        return index < catalog_->enums.size()
            ? catalog_->enums[index].definition.name : std::string{};
    case qt_types_tab_t::functions:
        return index < catalog_->functions.size()
            ? catalog_->functions[index].name : std::string{};
    default:
        return index < catalog_->typedefs.size()
            ? catalog_->typedefs[index].name : std::string{};
    }
}

bool QtTypesCatalogModel::addressAt(int view_row,
                                    aida::analysis::address_t& out) const {
    if (!catalog_ || !visible_ || view_row < 0 ||
        static_cast<std::size_t>(view_row) >= visible_->size())
        return false;
    const std::size_t index = (*visible_)[static_cast<std::size_t>(view_row)];
    if (tab_ == qt_types_tab_t::functions) {
        if (index < catalog_->functions.size()) {
            out = catalog_->functions[index].address;
            return true;
        }
        return false;
    }
    if (tab_ == qt_types_tab_t::typedefs) {
        if (index < catalog_->typedefs.size() &&
            catalog_->typedefs[index].address.value != 0) {
            out = catalog_->typedefs[index].address;
            return true;
        }
    }
    return false;
}

std::string QtTypesCatalogModel::entityIdAt(int view_row) const {
    if (!catalog_ || !visible_ || view_row < 0 ||
        static_cast<std::size_t>(view_row) >= visible_->size())
        return {};
    const std::size_t index = (*visible_)[static_cast<std::size_t>(view_row)];
    switch (tab_) {
    case qt_types_tab_t::structures:
        if (index < catalog_->structs.size())
            return "struct:" + catalog_->structs[index].module + ":" +
                catalog_->structs[index].definition.name;
        break;
    case qt_types_tab_t::unions_:
        if (index < catalog_->unions.size())
            return "union:" + catalog_->unions[index].module + ":" +
                catalog_->unions[index].definition.name;
        break;
    case qt_types_tab_t::enums:
        if (index < catalog_->enums.size())
            return "enum:" + catalog_->enums[index].module + ":" +
                catalog_->enums[index].definition.name;
        break;
    case qt_types_tab_t::functions:
        if (index < catalog_->functions.size())
            return "function:" +
                std::to_string(catalog_->functions[index].address.value) + ":" +
                catalog_->functions[index].name;
        break;
    default:
        if (index < catalog_->typedefs.size())
            return (tab_ == qt_types_tab_t::typedefs ? "typedef:" : "inferred:") +
                catalog_->typedefs[index].name + ":" +
                std::to_string(catalog_->typedefs[index].address.value);
        break;
    }
    return {};
}

QVariant QtTypesCatalogModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.parent().isValid() || !catalog_ || !visible_)
        return {};
    if (index.row() < 0 || static_cast<std::size_t>(index.row()) >= visible_->size())
        return {};
    const std::size_t source = (*visible_)[static_cast<std::size_t>(index.row())];
    const auto& tokens = aida::qt::theme::tokens();
    if (role == Qt::DisplayRole || role == Qt::UserRole) {
        switch (tab_) {
        case qt_types_tab_t::structures:
        case qt_types_tab_t::unions_: {
            if (source >= (tab_ == qt_types_tab_t::structures
                    ? catalog_->structs.size() : catalog_->unions.size()))
                return {};
            const auto& entry = tab_ == qt_types_tab_t::structures
                ? catalog_->structs[source] : catalog_->unions[source];
            switch (index.column()) {
            case 0: return QString::fromStdString(entry.definition.name);
            case 1: return QString::fromStdString(entry.module);
            case 2: {
                char size_buf[48]{};
                std::snprintf(size_buf, sizeof(size_buf), "0x%llX  %zu members",
                    static_cast<unsigned long long>(entry.definition.size),
                    entry.definition.members.size());
                return QString::fromLatin1(size_buf);
            }
            default: return {};
            }
        }
        case qt_types_tab_t::enums: {
            if (source >= catalog_->enums.size()) return {};
            const auto& entry = catalog_->enums[source];
            switch (index.column()) {
            case 0: return QString::fromStdString(entry.definition.name);
            case 1: return QString::fromStdString(entry.module);
            case 2:
                return QStringLiteral("%1 values").arg(entry.definition.members.size());
            default: return {};
            }
        }
        case qt_types_tab_t::functions: {
            if (source >= catalog_->functions.size()) return {};
            const auto& entry = catalog_->functions[source];
            switch (index.column()) {
            case 0: return QString::fromStdString(entry.name);
            case 1: return QString::fromStdString(entry.signature);
            case 2: {
                char size_buf[32]{};
                std::snprintf(size_buf, sizeof(size_buf), "0x%llX",
                    static_cast<unsigned long long>(entry.size));
                return QString::fromLatin1(size_buf);
            }
            case 3: return QString::fromStdString(entry.provenance);
            default: return {};
            }
        }
        default: {
            if (source >= catalog_->typedefs.size()) return {};
            const auto& entry = catalog_->typedefs[source];
            switch (index.column()) {
            case 0: return QString::fromStdString(entry.name);
            case 1:
                return entry.explicitly_unknown
                    ? QStringLiteral("<unknown>")
                    : QString::fromStdString(entry.canonical_type);
            case 2:
                return QStringLiteral("confidence %1")
                    .arg(static_cast<unsigned>(entry.confidence));
            default: return {};
            }
        }
        }
    }
    if (role == Qt::ForegroundRole) {
        if (tab_ == qt_types_tab_t::typedefs && index.column() == 1) {
            if (source < catalog_->typedefs.size() &&
                catalog_->typedefs[source].explicitly_unknown)
                return tokens.text_dim;
        }
        if (index.column() == 1) return tokens.text_secondary;
        return {};
    }
    if (role == Qt::FontRole && tab_ == qt_types_tab_t::functions &&
        index.column() == 2)
        return aida::qt::theme::fonts::codeRegular();
    if (role == Qt::ToolTipRole)
        return data(index, Qt::DisplayRole);
    return {};
}

void QtTypesCatalogModel::multiData(const QModelIndex& index,
                                     QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan) {
        switch (roleData.role()) {
        case Qt::DisplayRole:
        case Qt::UserRole:
        case Qt::ForegroundRole:
        case Qt::FontRole:
        case Qt::ToolTipRole:
            roleData.setData(data(index, roleData.role()));
            break;
        default:
            roleData.clearData();
            break;
        }
    }
}

QVariant QtTypesCatalogModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (tab_) {
    case qt_types_tab_t::structures:
    case qt_types_tab_t::unions_:
        if (section == 0) return QStringLiteral("Name");
        if (section == 1) return QStringLiteral("Module");
        if (section == 2) return QStringLiteral("Layout");
        break;
    case qt_types_tab_t::enums:
        if (section == 0) return QStringLiteral("Name");
        if (section == 1) return QStringLiteral("Module");
        if (section == 2) return QStringLiteral("Values");
        break;
    case qt_types_tab_t::functions:
        if (section == 0) return QStringLiteral("Name");
        if (section == 1) return QStringLiteral("Signature");
        if (section == 2) return QStringLiteral("Size");
        if (section == 3) return QStringLiteral("Provenance");
        break;
    default:
        if (section == 0) return QStringLiteral("Name");
        if (section == 1) return QStringLiteral("Canonical type");
        if (section == 2) return QStringLiteral("Confidence");
        break;
    }
    return {};
}

}
