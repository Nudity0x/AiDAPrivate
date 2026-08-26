#include "qt/ai/qt_ai_chat_model.hpp"

namespace aida::qt::ai {

namespace {

constexpr std::size_t k_render_window = 256;

}

AidaChatMessageModel::AidaChatMessageModel(QObject* parent) : QAbstractListModel(parent) {
    refreshWindow();
}

int AidaChatMessageModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(window_.last - window_.first);
}

QVariant AidaChatMessageModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || role != Qt::DisplayRole)
        return {};
    automation_ui::chat_message_snapshot_t snapshot;
    if (!messageAt(index.row(), snapshot))
        return {};
    return QString::fromStdString(snapshot.text.substr(0, 160));
}

bool AidaChatMessageModel::messageAt(
    int row, aida::automation_ui::chat_message_snapshot_t& out) const {
    if (row < 0 || row >= rowCount())
        return false;
    return aida::automation_ui::message_snapshot(window_.first +
        static_cast<std::size_t>(row), out);
}

aida::automation_ui::message_identity_t AidaChatMessageModel::identityAt(int row) const {
    if (row < 0 || row >= rowCount())
        return {};
    return aida::automation_ui::message_identity(window_.first +
        static_cast<std::size_t>(row));
}

std::size_t AidaChatMessageModel::absoluteIndexForRow(int row) const {
    return window_.first + static_cast<std::size_t>(row);
}

void AidaChatMessageModel::setWindowState(std::size_t page_start, bool follow_latest) {
    page_start_ = page_start;
    follow_latest_ = follow_latest;
}

aida::automation_ui::message_window_t AidaChatMessageModel::window() const {
    return window_;
}

void AidaChatMessageModel::refreshWindow() {
    const auto total = aida::automation_ui::message_count();
    if (total > window_.total && follow_latest_)
        page_start_ = total > k_render_window ? total - k_render_window : 0;
    if (page_start_ >= total)
        page_start_ = total > k_render_window ? total - k_render_window : 0;
    window_ = aida::automation_ui::bounded_message_window(page_start_, k_render_window, 0);
    page_start_ = window_.first;
}

void AidaChatMessageModel::resetWindow() {
    beginResetModel();
    refreshWindow();
    endResetModel();
}

AidaChatMessageModel::window_update_t AidaChatMessageModel::applyBackendChange(
    std::size_t previous_total) {
    const std::size_t total = aida::automation_ui::message_count();
    if (total < previous_total) {
        resetWindow();
        return window_update_t::reset;
    }
    if (total > previous_total) {
        const int old_rows = rowCount();
        const auto old_first = window_.first;
        refreshWindow();
        if (window_.first != old_first) {
            beginResetModel();
            endResetModel();
            return window_update_t::reset;
        }
        const int new_rows = rowCount();
        if (new_rows > old_rows) {
            beginInsertRows({}, old_rows, new_rows - 1);
            endInsertRows();
        }
        return window_update_t::grew;
    }
    refreshWindow();
    return window_update_t::unchanged;
}

void AidaChatMessageModel::touchLastRow() {
    const int rows = rowCount();
    if (rows <= 0)
        return;
    const QModelIndex last = index(rows - 1);
    Q_EMIT dataChanged(last, last);
}

void AidaChatMessageModel::notifyAppended(int old_rows, int new_rows) {
    if (new_rows <= old_rows)
        return;
    beginInsertRows({}, old_rows, new_rows - 1);
    endInsertRows();
}

int AidaChatMessageModel::rowForAbsoluteIndex(std::size_t absolute) const {
    if (absolute < window_.first || absolute >= window_.last)
        return -1;
    return static_cast<int>(absolute - window_.first);
}

}
