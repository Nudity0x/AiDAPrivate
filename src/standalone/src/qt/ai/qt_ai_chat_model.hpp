#pragma once

#include <QAbstractListModel>

#include <cstddef>
#include <cstdint>

#include "core/ai/standalone_chat.hpp"

namespace aida::qt::ai {

class AidaChatMessageModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AidaChatMessageModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    bool messageAt(int row, aida::automation_ui::chat_message_snapshot_t& out) const;
    aida::automation_ui::message_identity_t identityAt(int row) const;
    std::size_t absoluteIndexForRow(int row) const;

    std::size_t pageStart() const noexcept { return page_start_; }
    bool followLatest() const noexcept { return follow_latest_; }
    void setWindowState(std::size_t page_start, bool follow_latest);
    aida::automation_ui::message_window_t window() const;
    void refreshWindow();
    void resetWindow();
    enum class window_update_t : int { unchanged, grew, reset };
    window_update_t applyBackendChange(std::size_t previous_total);
    void notifyAppended(int old_rows, int new_rows);
    void touchLastRow();
    int rowForAbsoluteIndex(std::size_t absolute) const;

private:
    std::size_t page_start_ = 0;
    bool follow_latest_ = true;
    aida::automation_ui::message_window_t window_;
};

}
