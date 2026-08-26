#pragma once

#include <QWidget>

#include <string>
#include <vector>

class QTableView;
class QAbstractTableModel;

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::workbench {

// Recent view (07 sec. 8.5), view.recent. Open sessions + recent_workspaces_json
// (parsed on demand, cached by string identity).
class QtRecentView : public QWidget {
    Q_OBJECT
public:
    explicit QtRecentView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void reload();
    void showRowMenu(const QPoint& global_pos, int view_row);

    QAbstractTableModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    std::string cached_json_identity_;
};

}
