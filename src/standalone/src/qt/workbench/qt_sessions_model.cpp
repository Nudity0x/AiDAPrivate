#include "qt/workbench/qt_sessions_model.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

#include <chrono>
#include <cstdio>

namespace aida::qt::workbench {

namespace {

QString relative_last_active(std::uint64_t last_active_steady_ms) {
    if (last_active_steady_ms == 0)
        return QStringLiteral("-");
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    const std::uint64_t age_ms = now > last_active_steady_ms
        ? now - last_active_steady_ms : 0;
    if (age_ms < 5000)
        return QStringLiteral("just now");
    if (age_ms < 60000)
        return QStringLiteral("%1 s ago").arg(age_ms / 1000);
    if (age_ms < 3600000)
        return QStringLiteral("%1 min ago").arg(age_ms / 60000);
    if (age_ms < 86400000)
        return QStringLiteral("%1 h ago").arg(age_ms / 3600000);
    return QStringLiteral("%1 d ago").arg(age_ms / 86400000);
}

}

QtSessionsModel::QtSessionsModel(QObject* parent) : QAbstractTableModel(parent) {}

void QtSessionsModel::setRows(
    std::vector<analysis_session::session_summary_t> rows, std::size_t active_index,
    std::uint32_t active_driver_pid) {
    beginResetModel();
    session_indices_.clear();
    session_indices_.reserve(rows.size());
    for (std::size_t index = 0; index < rows.size(); ++index)
        session_indices_.push_back(static_cast<int>(index));
    rows_ = std::move(rows);
    active_index_ = active_index;
    active_driver_pid_ = active_driver_pid;
    endResetModel();
}

const analysis_session::session_summary_t* QtSessionsModel::rowAt(
    int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

int QtSessionsModel::sessionIndexFor(int view_row) const noexcept {
    if (view_row < 0 || static_cast<std::size_t>(view_row) >= session_indices_.size())
        return -1;
    return session_indices_[static_cast<std::size_t>(view_row)];
}

int QtSessionsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int QtSessionsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
}

QVariant QtSessionsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.parent().isValid()) return {};
    const auto* session = rowAt(index.row());
    if (!session) return {};
    const auto column = static_cast<Column>(index.column());
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Column::name: {
            const std::string name = session->filename.empty()
                ? session->path : session->filename;
            return QString::fromStdString(name.empty() ? "Untitled session" : name);
        }
        case Column::kind:
            return session->kind == analysis_session::session_kind_t::static_file
                ? QStringLiteral("Static") : QStringLiteral("Live");
        case Column::pid:
            return session->pid != 0 ? QString::number(session->pid) : QString();
        case Column::state: {
            switch (session->load_state) {
            case analysis_session::session_load_state_t::opening:
                return QStringLiteral("Opening");
            case analysis_session::session_load_state_t::analyzing:
                return QStringLiteral("Analyzing");
            case analysis_session::session_load_state_t::ready:
                return QStringLiteral("Ready");
            case analysis_session::session_load_state_t::failed:
                return QStringLiteral("Failed");
            case analysis_session::session_load_state_t::closing:
                return QStringLiteral("Closing");
            case analysis_session::session_load_state_t::closed:
                return QStringLiteral("Closed");
            }
            return {};
        }
        case Column::pdb:
            return QString::fromStdString(session->pdb_status);
        case Column::analysis_revision:
            return QString::number(session->analysis_revision);
        case Column::last_active:
            return relative_last_active(session->last_active_steady_ms);
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        if (column == Column::name) {
            const QString name = data(index, Qt::DisplayRole).toString();
            const QString path = QString::fromStdString(session->path);
            return path.isEmpty() ? name : name + QStringLiteral("\n") + path;
        }
        return data(index, Qt::DisplayRole);
    }
    if (role == Qt::FontRole) {
        if (column == Column::pid || column == Column::analysis_revision)
            return aida::qt::theme::fonts::codeRegular();
        return {};
    }
    if (role == Qt::UserRole)
        return sessionIndexFor(index.row());
    if (role == Qt::UserRole + 1) {
        const bool dead = !session->is_alive ||
            session->load_state == analysis_session::session_load_state_t::failed ||
            session->load_state == analysis_session::session_load_state_t::closed;
        const bool loading =
            session->load_state == analysis_session::session_load_state_t::opening ||
            session->load_state == analysis_session::session_load_state_t::analyzing;
        const bool closing =
            session->load_state == analysis_session::session_load_state_t::closing;
        const bool live_attached =
            session->kind == analysis_session::session_kind_t::live_attach &&
            session->pid != 0 && session->pid == active_driver_pid_ &&
            session->is_active;
        if (dead) return QStringLiteral("Dead");
        if (loading) return QStringLiteral("Loading");
        if (closing) return QStringLiteral("Closing");
        if (session->kind == analysis_session::session_kind_t::static_file)
            return QStringLiteral("Static");
        return live_attached ? QStringLiteral("Live") : QStringLiteral("Detached");
    }
    if (role == Qt::ForegroundRole) {
        const auto& tokens = aida::qt::theme::tokens();
        const bool dead = !session->is_alive ||
            session->load_state == analysis_session::session_load_state_t::failed ||
            session->load_state == analysis_session::session_load_state_t::closed;
        if (dead) return tokens.error;
        if (column == Column::state) {
            switch (session->load_state) {
            case analysis_session::session_load_state_t::opening:
            case analysis_session::session_load_state_t::analyzing:
                return tokens.info;
            case analysis_session::session_load_state_t::ready:
                return tokens.success;
            case analysis_session::session_load_state_t::closing:
                return tokens.warning;
            default:
                return {};
            }
        }
        return {};
    }
    return {};
}

void QtSessionsModel::multiData(const QModelIndex& index,
                                 QModelRoleDataSpan roleDataSpan) const {
    for (QModelRoleData& roleData : roleDataSpan) {
        switch (roleData.role()) {
        case Qt::DisplayRole:
        case Qt::ForegroundRole:
        case Qt::ToolTipRole:
        case Qt::FontRole:
        case Qt::UserRole:
        case Qt::UserRole + 1:
            roleData.setData(data(index, roleData.role()));
            break;
        default:
            roleData.clearData();
            break;
        }
    }
}

QVariant QtSessionsModel::headerData(int section, Qt::Orientation orientation,
                                     int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (static_cast<Column>(section)) {
    case Column::name: return QStringLiteral("Name");
    case Column::kind: return QStringLiteral("Kind");
    case Column::pid: return QStringLiteral("PID");
    case Column::state: return QStringLiteral("State");
    case Column::pdb: return QStringLiteral("PDB");
    case Column::analysis_revision: return QStringLiteral("Revision");
    case Column::last_active: return QStringLiteral("Last active");
    default: return {};
    }
}

}
