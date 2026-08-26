#pragma once

#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QWidget>

#include <QString>

#include <cstdint>
#include <string>
#include <vector>

#include "core/ai/skill_manager_service.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSplitter;
class QTabBar;
class QTextBrowser;
class QTimer;
class QVBoxLayout;

namespace aida::qt::ai {

class AidaSkillListModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AidaSkillListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void setTab(int tab);
    void setSearch(const QString& text);
    void setAgentFilter(const QString& agent_name);
    void reloadFrom(const aida::skill_manager_service::snapshot_ptr& publication);

    const aida::skills::skill_metadata_t* skillAt(int row) const;
    bool enabledAt(int row) const;

private:
    std::vector<aida::skills::skill_metadata_t> rows_;
    int tab_ = 0;
    QString search_;
    QString agent_filter_;
    aida::skill_manager_service::snapshot_ptr publication_;
};

class AidaSkillListDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit AidaSkillListDelegate(AidaSkillListModel* model, QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;

Q_SIGNALS:
    void enableToggled(const QString& skill_name, bool enabled);

private:
    QRect toggleRect(const QRect& row) const;

    AidaSkillListModel* model_ = nullptr;
};

class AidaSkillManagerView : public QWidget {
    Q_OBJECT
public:
    explicit AidaSkillManagerView(QWidget* parent = nullptr);
    ~AidaSkillManagerView() override;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void pollService();
    void applyPublication(const aida::skill_manager_service::snapshot_ptr& publication);
    void refreshDetail();
    void refreshPreview();
    void refreshRemotePanel();
    void openContextMenu(const QModelIndex& index, const QPoint& global_pos);
    void onUninstallRequested(const QString& name, quint64 generation);
    void onResolveRequested(const QString& name);
    bool serviceRequest(bool accepted, const std::string& error);

    AidaSkillListModel* model_ = nullptr;
    AidaSkillListDelegate* delegate_ = nullptr;
    QListView* list_ = nullptr;
    QLineEdit* search_edit_ = nullptr;
    QComboBox* agent_combo_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QLineEdit* url_edit_ = nullptr;
    QPushButton* add_url_button_ = nullptr;
    QTabBar* tabs_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QWidget* remote_panel_ = nullptr;
    QVBoxLayout* remote_layout_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* detail_title_ = nullptr;
    QLabel* detail_source_ = nullptr;
    QLabel* detail_description_ = nullptr;
    QLabel* detail_path_ = nullptr;
    QLabel* detail_agents_ = nullptr;
    QLabel* detail_hints_ = nullptr;
    QPushButton* reveal_button_ = nullptr;
    QPushButton* open_file_button_ = nullptr;
    QPushButton* enable_button_ = nullptr;
    QTextBrowser* preview_ = nullptr;
    QPushButton* render_toggle_ = nullptr;
    QTimer* poll_timer_ = nullptr;

    std::string selected_skill_name_;
    std::string last_error_;
    bool preview_rendered_ = true;
    std::uint64_t observed_service_generation_ = 0;
    std::string requested_resolve_name_;
};

}
