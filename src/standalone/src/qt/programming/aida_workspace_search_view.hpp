#pragma once

#include <QAbstractItemModel>
#include <QModelIndex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/workspace_search.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QTreeView;

namespace aida::ui {
enum class context_menu_open_origin_t : std::uint8_t;
}

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::programming {

// Two-level incremental model over workspace_search::g_search: file groups ->
// match rows. The 100 ms poll (running only while searching, plus one final
// completion tick) copies at most 500 results under results_mtx inside the
// tick — never in data()/paint — and appends groups/rows incrementally inside
// one search generation.
class AidaWorkspaceSearchModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit AidaWorkspaceSearchModel(QObject* parent = nullptr);

    void pollOnce();
    void reset();

    QModelIndex index(int row, int column,
                    const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    bool isGroup(const QModelIndex& index) const;
    const workspace_search::match_result_t* matchAt(const QModelIndex& index) const;
    int sourceIndexAt(const QModelIndex& index) const;

Q_SIGNALS:
    void groupsAppended(int first_group);

private:
    struct group_t {
        QString path;
        QString label;
        int first_match = 0;
        int match_count = 0;
    };
    std::vector<workspace_search::match_result_t> results_;
    std::vector<group_t> groups_;
    std::uint64_t generation_ = 0;
};

class AidaWorkspaceSearchView : public QWidget {
    Q_OBJECT
public:
    explicit AidaWorkspaceSearchView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void startSearch();
    void onPollTick();
    void refreshStatus();
    void openResultContext(const QModelIndex& index,
        aida::ui::context_menu_open_origin_t origin, const QPoint& global_pos);

    AidaWorkspaceSearchModel* model_ = nullptr;
    QTreeView* tree_ = nullptr;
    QLineEdit* query_edit_ = nullptr;
    QCheckBox* case_box_ = nullptr;
    QCheckBox* word_box_ = nullptr;
    QCheckBox* regex_box_ = nullptr;
    QLineEdit* include_edit_ = nullptr;
    QLineEdit* exclude_edit_ = nullptr;
    QPushButton* search_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QLabel* scope_label_ = nullptr;
    QPushButton* clear_scope_button_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* cap_label_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    QSet<QString> collapsed_paths_;
    QString scope_path_;
};

}
