#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QString>
#include <QStyledItemDelegate>

#include <string>
#include <vector>

#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/chord_stroke.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTimer;

namespace aida::qt::bridge {
class ShortcutBridge;
class ShortcutRecorderWidget;
}

namespace aida::qt::dialogs {

class AidaShortcutTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ActionColumn = 0,
        BindingColumn,
        ScopeColumn,
        ButtonsColumn,
        ColumnCount
    };

    explicit AidaShortcutTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void setShortcuts(std::vector<aida::ui::application_ui::shortcut_presentation_t> shortcuts);
    void setFilter(const QString& filter);
    bool isCategoryRow(int row) const;
    const aida::ui::application_ui::shortcut_presentation_t* shortcutAt(int row) const;
    int visibleCount() const noexcept { return visible_count_; }

private:
    void rebuildRows();

    struct row_t {
        bool category = false;
        std::string category_name;
        std::size_t shortcut_index = 0;
    };

    std::vector<aida::ui::application_ui::shortcut_presentation_t> shortcuts_;
    std::vector<row_t> rows_;
    QString filter_;
    int visible_count_ = 0;
};

class AidaShortcutButtonsDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit AidaShortcutButtonsDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option, const QModelIndex& index) override;

Q_SIGNALS:
    void editClicked(int row);
    void disableClicked(int row);
    void resetClicked(int row);
};

class AidaShortcutsDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    AidaShortcutsDialog(bridge::ShortcutBridge* shortcuts, QWidget* parent = nullptr);
    ~AidaShortcutsDialog() override;

    void openFresh();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void reload();
    void beginEdit(int row);
    void cancelEdit(const QString& status);
    void applyEdit(bool replace_conflicts);
    void onCaptureFinished();
    void updateCaptureUi();
    void setCaptureGates(bool active);
    void refreshFilter();

    bridge::ShortcutBridge* shortcuts_ = nullptr;
    QLineEdit* filter_edit_ = nullptr;
    QPushButton* reset_all_button_ = nullptr;
    QPushButton* reset_all_confirm_ = nullptr;
    QPushButton* reset_all_cancel_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTableView* table_ = nullptr;
    AidaShortcutTableModel* model_ = nullptr;
    AidaShortcutButtonsDelegate* buttons_delegate_ = nullptr;
    QWidget* edit_panel_ = nullptr;
    QLabel* edit_title_ = nullptr;
    QLabel* edit_draft_ = nullptr;
    bridge::ShortcutRecorderWidget* recorder_ = nullptr;
    QPushButton* apply_button_ = nullptr;
    QPushButton* replace_button_ = nullptr;
    QPushButton* cancel_edit_button_ = nullptr;
    QLabel* conflict_label_ = nullptr;
    QString edit_binding_;
    QString edit_label_;
    std::vector<aida::ui::chord_stroke_t> edit_strokes_;
    std::vector<std::string> edit_conflicts_;
    bool reset_all_armed_ = false;
    bool filter_status_active_ = false;
};

}
