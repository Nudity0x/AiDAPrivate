#pragma once

#include <QWidget>

class QVBoxLayout;
class QLabel;
class QPushButton;

namespace aida::qt::chrome {

class AidaThemePickerPopup : public QWidget {
    Q_OBJECT
public:
    explicit AidaThemePickerPopup(QWidget* parent = nullptr);

    void openAt(const QPoint& global_pos);

Q_SIGNALS:
    void editThemeRequested(int custom_index);
    void createThemeRequested();

private:
    void rebuild();
    void addBuiltInRows(QVBoxLayout* layout);
    void addCustomRows(QVBoxLayout* layout);
    void addFooter(QVBoxLayout* layout);
    void importTheme();
    void refreshTransferStrip();

    QVBoxLayout* root_ = nullptr;
    QWidget* transfer_strip_ = nullptr;
    QLabel* transfer_label_ = nullptr;
    QPushButton* retry_button_ = nullptr;
};

}
