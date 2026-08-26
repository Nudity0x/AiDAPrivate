#pragma once

#include <QColor>
#include <QString>

#include <cstdint>
#include <string>

#include "qt/bridge/aida_dialog.hpp"
#include "qt/chrome/aida_theme_catalog.hpp"

class QLabel;
class QLineEdit;
class QPushButton;

namespace aida::qt::dialogs {

class AidaThemeEditorDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    explicit AidaThemeEditorDialog(QWidget* parent = nullptr);

    void editTheme(int catalog_index);
    void createTheme();

Q_SIGNALS:
    void saved();

private:
    void syncFromDraft();
    void onSave();
    void onExport();
    void onDeleteReview();
    void onPickAccent();
    void onPickSwatch(std::uint32_t chrome::AidaCatalogTheme::*field, const QString& label);
    void onPickIcon();
    void refreshSwatches();
    void refreshTransferStrip();
    bool validateIconPath(const std::string& path, QString& error);

    QLineEdit* name_edit_ = nullptr;
    QPushButton* accent_button_ = nullptr;
    QPushButton* bg_button_ = nullptr;
    QPushButton* panel_button_ = nullptr;
    QPushButton* header_button_ = nullptr;
    QPushButton* title_button_ = nullptr;
    QLabel* icon_label_ = nullptr;
    QPushButton* icon_pick_ = nullptr;
    QPushButton* icon_clear_ = nullptr;
    QLabel* transfer_label_ = nullptr;
    QPushButton* retry_button_ = nullptr;
    QWidget* transfer_strip_ = nullptr;
    QLabel* error_label_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* export_button_ = nullptr;
    QPushButton* delete_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    chrome::AidaCatalogTheme draft_;
    int editing_index_ = -1;
};

}
