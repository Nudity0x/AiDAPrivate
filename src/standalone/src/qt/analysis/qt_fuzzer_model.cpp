#include "qt/analysis/qt_fuzzer_model.hpp"

#include "qt/theme/aida_tokens.hpp"

#include <cstdio>

namespace aida::qt::analysis {

QtFuzzerCrashModel::QtFuzzerCrashModel(QObject* parent) : QAbstractTableModel(parent) {}

void QtFuzzerCrashModel::setSnapshot(
    std::shared_ptr<const fuzzer_engine::render_snapshot_t> snapshot) {
    beginResetModel();
    snapshot_ = std::move(snapshot);
    endResetModel();
}

const fuzzer_engine::crash_info_t* QtFuzzerCrashModel::rowAt(int row) const noexcept {
    if (!snapshot_ || row < 0 ||
        static_cast<std::size_t>(row) >= snapshot_->unique_crashes.size())
        return nullptr;
    return &snapshot_->unique_crashes[static_cast<std::size_t>(row)];
}

int QtFuzzerCrashModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() || !snapshot_
        ? 0 : static_cast<int>(snapshot_->unique_crashes.size());
}

int QtFuzzerCrashModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
}

QVariant QtFuzzerCrashModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.parent().isValid()) return {};
    const auto* crash = rowAt(index.row());
    if (!crash) return {};
    const auto& tokens = aida::qt::theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (static_cast<Column>(index.column())) {
        case Column::index: return QStringLiteral("#%1").arg(index.row() + 1);
        case Column::score:
            return QString::fromLatin1(
                fuzzer_engine::exploit_score_name(crash->score));
        case Column::type:
            return QString::fromLatin1(fuzzer_engine::crash_type_name(crash->type));
        case Column::address: {
            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "0x%llX",
                static_cast<unsigned long long>(crash->instruction_address));
            return QString::fromLatin1(buf);
        }
        case Column::instruction: {
            std::string trim = crash->crashing_instruction;
            if (trim.size() > 30) trim = trim.substr(0, 28) + "..";
            return QString::fromStdString(trim);
        }
        case Column::description: {
            std::string desc = crash->description;
            if (desc.size() > 40) desc = desc.substr(0, 38) + "..";
            return QString::fromStdString(desc);
        }
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (static_cast<Column>(index.column())) {
        case Column::index: return tokens.text_dim;
        case Column::type: return tokens.error;
        case Column::address: return tokens.text_address;
        case Column::instruction: return tokens.text_dim;
        case Column::description: return tokens.text_secondary;
        default: return {};
        }
    }
    if (role == Qt::UserRole)
        return static_cast<int>(crash->score);
    if (role == Qt::ToolTipRole) {
        switch (static_cast<Column>(index.column())) {
        case Column::instruction:
            return QString::fromStdString(crash->crashing_instruction);
        case Column::description:
            return QString::fromStdString(crash->description);
        default:
            return data(index, Qt::DisplayRole);
        }
    }
    return {};
}

void QtFuzzerCrashModel::multiData(const QModelIndex& index,
                                   QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan) {
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

QVariant QtFuzzerCrashModel::headerData(int section, Qt::Orientation orientation,
                                        int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (static_cast<Column>(section)) {
    case Column::index: return QStringLiteral("#");
    case Column::score: return QStringLiteral("Score");
    case Column::type: return QStringLiteral("Type");
    case Column::address: return QStringLiteral("Address");
    case Column::instruction: return QStringLiteral("Instruction");
    case Column::description: return QStringLiteral("Description");
    default: return {};
    }
}

}
