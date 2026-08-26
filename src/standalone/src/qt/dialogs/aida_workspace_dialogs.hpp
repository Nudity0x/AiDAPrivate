#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qt/bridge/aida_dialog.hpp"
#include "qt/docking/preset_recipes.hpp"

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QTimer;
class QDialogButtonBox;

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::layout {
class WorkspacePersistenceController;
}

namespace aida::qt::dialogs {

class AidaWorkspaceController;

class AidaWorkspaceSaveAsDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    AidaWorkspaceSaveAsDialog(layout::WorkspacePersistenceController* persistence,
                              AidaWorkspaceController* controller, QWidget* parent = nullptr);

    void openFresh();

private:
    void attemptSave(bool overwrite);
    void refreshButtons();

    layout::WorkspacePersistenceController* persistence_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QLabel* base_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* catalog_note_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    QWidget* overwrite_section_ = nullptr;
    QLabel* overwrite_text_ = nullptr;
    QPushButton* keep_button_ = nullptr;
    QTimer* pending_timer_ = nullptr;
    AidaWorkspaceController* controller_ = nullptr;
    bool review_overwrite_ = false;
};

class AidaWorkspaceManagerDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    AidaWorkspaceManagerDialog(layout::WorkspacePersistenceController* persistence,
                               AidaWorkspaceController* controller, QWidget* parent = nullptr);

    void openFresh(const QString& selected = QString(), quint64 generation = 0);
    void openResetAllReview();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void refreshCatalog();
    void updateDefaultButton();
    void revalidateSelection();
    void onSelectionChanged();
    void onLoadClicked();
    void onSaveAsCopy();
    void onRename();
    void onDeleteReview();
    void onDeleteConfirm();
    void onResetAllConfirm();
    void setStatus(const QString& text);

    layout::WorkspacePersistenceController* persistence_ = nullptr;
    QListView* catalog_view_ = nullptr;
    QAbstractListModel* model_ = nullptr;
    QLabel* active_label_ = nullptr;
    QLabel* detail_name_ = nullptr;
    QLabel* detail_preset_ = nullptr;
    QLabel* detail_generation_ = nullptr;
    QLineEdit* rename_edit_ = nullptr;
    QPushButton* load_button_ = nullptr;
    QPushButton* save_copy_button_ = nullptr;
    QPushButton* rename_button_ = nullptr;
    QPushButton* delete_button_ = nullptr;
    QPushButton* delete_confirm_ = nullptr;
    QPushButton* delete_cancel_ = nullptr;
    QWidget* delete_section_ = nullptr;
    QLabel* delete_text_ = nullptr;
    QWidget* reset_section_ = nullptr;
    QPushButton* reset_confirm_ = nullptr;
    QPushButton* reset_cancel_ = nullptr;
    QPushButton* save_as_button_ = nullptr;
    QPushButton* close_button_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTimer* pending_timer_ = nullptr;
    QString selected_name_;
    quint64 selected_generation_ = 0;
    AidaWorkspaceController* controller_ = nullptr;
    bool selection_requires_reselection_ = false;
    bool review_delete_ = false;
    bool review_reset_all_ = false;
};

class AidaWorkspaceController : public QObject {
    Q_OBJECT
public:
    AidaWorkspaceController(docking::AidaDockHost* host, QWidget* dialog_parent,
                            QObject* parent = nullptr);

    void openSaveAs();
    void openManager(const QString& name = QString(), quint64 generation = 0);
    void openResetAllReview();

    void showStatusMessage(const QString& message, int timeout_ms = 5000);

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);

private:
    docking::AidaDockHost* host_ = nullptr;
    QWidget* dialog_parent_ = nullptr;
    QPointer<AidaWorkspaceSaveAsDialog> save_as_;
    QPointer<AidaWorkspaceManagerDialog> manager_;
};

std::string workspace_request_message(docking::workspace_request_result_t result,
                                      std::string_view operation);
bool workspace_request_succeeded(docking::workspace_request_result_t result) noexcept;

}
