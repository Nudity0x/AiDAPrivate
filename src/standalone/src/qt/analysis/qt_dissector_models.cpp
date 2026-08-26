#include "qt/analysis/qt_dissector_models.hpp"

#include <algorithm>
#include <cstdio>

#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::analysis {

QtDissectorStructureModel::QtDissectorStructureModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtDissectorStructureModel::syncFromEngine(const QString& filter_lower) {
    auto& state = struct_dissector::g_state;
    std::lock_guard<std::mutex> lock(state.mtx);
    std::vector<std::pair<std::string, std::uint32_t>> entries;
    entries.reserve(state.structs.size());
    entry_indices_.clear();
    for (std::size_t i = 0; i < state.structs.size(); ++i) {
        const auto& definition = state.structs[i];
        if (!filter_lower.isEmpty() &&
            !QString::fromStdString(definition.name).toLower().contains(filter_lower))
            continue;
        entries.emplace_back(definition.name, definition.total_size);
        entry_indices_.push_back(static_cast<int>(i));
    }
    active_struct_ = state.active_struct;
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
}

int QtDissectorStructureModel::engineIndexFor(int view_row) const noexcept {
    if (view_row < 0 || static_cast<std::size_t>(view_row) >= entry_indices_.size())
        return -1;
    return entry_indices_[static_cast<std::size_t>(view_row)];
}

int QtDissectorStructureModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

int QtDissectorStructureModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 1;
}

QVariant QtDissectorStructureModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= entries_.size())
        return {};
    const auto& entry = entries_[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole)
        return QStringLiteral("%1  (%2)")
            .arg(QString::fromStdString(entry.first))
            .arg(entry.second);
    if (role == Qt::ForegroundRole) {
        if (entry_indices_[static_cast<std::size_t>(index.row())] == active_struct_)
            return aida::qt::theme::tokens().accent;
    }
    if (role == Qt::ToolTipRole)
        return QString::fromStdString(entry.first);
    return {};
}

void QtDissectorStructureModel::multiData(const QModelIndex& index,
                                          QModelRoleDataSpan span) const {
    for (QModelRoleData& roleData : span) {
        if (roleData.role() == Qt::DisplayRole || roleData.role() == Qt::ForegroundRole ||
            roleData.role() == Qt::ToolTipRole)
            roleData.setData(data(index, roleData.role()));
        else
            roleData.clearData();
    }
}

QtDissectorFieldModel::QtDissectorFieldModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtDissectorFieldModel::syncFromEngine() {
    auto& state = struct_dissector::g_state;
    std::lock_guard<std::mutex> lock(state.mtx);
    const int active = state.active_struct;
    beginResetModel();
    fields_.clear();
    values_.clear();
    if (struct_dissector::valid_index(active, state.structs.size())) {
        const auto& structure = state.structs[static_cast<std::size_t>(active)];
        fields_ = structure.fields;
        values_ = state.cached_values;
    }
    endResetModel();
}

void QtDissectorFieldModel::syncValues() {
    auto& state = struct_dissector::g_state;
    std::lock_guard<std::mutex> lock(state.mtx);
    const std::size_t count = (std::min)(values_.size(), state.cached_values.size());
    int first = -1;
    int last = -1;
    for (std::size_t i = 0; i < count; ++i) {
        if (values_[i].raw_bytes != state.cached_values[i].raw_bytes ||
            values_[i].display_text != state.cached_values[i].display_text) {
            values_[i] = state.cached_values[i];
            if (first < 0) first = static_cast<int>(i);
            last = static_cast<int>(i);
        }
    }
    if (first >= 0) {
        Q_EMIT dataChanged(index(first, static_cast<int>(Column::value)),
            index(last, static_cast<int>(Column::value)));
    }
}

int QtDissectorFieldModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(fields_.size());
}

int QtDissectorFieldModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
}

const struct_dissector::field_def_t* QtDissectorFieldModel::fieldAt(
    int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= fields_.size()) return nullptr;
    return &fields_[static_cast<std::size_t>(row)];
}

const struct_dissector::live_value_t* QtDissectorFieldModel::valueAt(
    int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= values_.size()) return nullptr;
    return &values_[static_cast<std::size_t>(row)];
}

QVariant QtDissectorFieldModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.parent().isValid()) return {};
    const auto* field = fieldAt(index.row());
    if (!field) return {};
    const auto& tokens = aida::qt::theme::tokens();
    const auto column = static_cast<Column>(index.column());
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Column::offset: {
            char buf[16]{};
            std::snprintf(buf, sizeof(buf), "+0x%03X", field->offset);
            return QString::fromLatin1(buf);
        }
        case Column::name: return QString::fromStdString(field->name);
        case Column::type:
            return QString::fromLatin1(struct_dissector::field_type_name(field->type));
        case Column::value: {
            const auto* value = valueAt(index.row());
            return value ? QString::fromStdString(value->display_text) : QVariant{};
        }
        case Column::description:
            return field->description.empty()
                ? QStringLiteral("(comment)")
                : QString::fromStdString(field->description);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (column) {
        case Column::offset: return tokens.text_address;
        case Column::name: return tokens.text_primary;
        case Column::type: return tokens.syn_number;
        case Column::value: return tokens.text_primary;
        case Column::description: return tokens.text_dim;
        default: return {};
        }
    }
    if (role == Qt::UserRole) {
        const auto* value = valueAt(index.row());
        return value ? QString::fromStdString(value->display_text) : QVariant{};
    }
    if (role == Qt::ToolTipRole) {
        switch (column) {
        case Column::name:
            return QString::fromStdString(field->name);
        case Column::description:
            return field->description.empty()
                ? QVariant{} : QString::fromStdString(field->description);
        case Column::value: {
            const auto* value = valueAt(index.row());
            return value ? QString::fromStdString(value->display_text) : QVariant{};
        }
        default:
            return data(index, Qt::DisplayRole);
        }
    }
    return {};
}

void QtDissectorFieldModel::multiData(const QModelIndex& index,
                                      QModelRoleDataSpan span) const {
    for (QModelRoleData& roleData : span) {
        switch (roleData.role()) {
        case Qt::DisplayRole:
        case Qt::ForegroundRole:
        case Qt::UserRole:
        case Qt::ToolTipRole:
            roleData.setData(data(index, roleData.role()));
            break;
        default:
            roleData.clearData();
            break;
        }
    }
}

QVariant QtDissectorFieldModel::headerData(int section, Qt::Orientation orientation,
                                           int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (static_cast<Column>(section)) {
    case Column::offset: return QStringLiteral("Offset");
    case Column::name: return QStringLiteral("Name");
    case Column::type: return QStringLiteral("Type");
    case Column::value: return QStringLiteral("Value");
    case Column::description: return QStringLiteral("Description");
    default: return {};
    }
}

}
