#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QSpinBox;

namespace aida::qt::settings {

class AidaSettingsEditorPage : public QWidget {
    Q_OBJECT
public:
    explicit AidaSettingsEditorPage(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void loadOnce();
    void persist();

    QSpinBox* tab_size_spin_ = nullptr;
    QDoubleSpinBox* font_size_spin_ = nullptr;
    QCheckBox* full_line_select_check_ = nullptr;
    QComboBox* theme_combo_ = nullptr;
    QComboBox* density_combo_ = nullptr;
    QCheckBox* reduced_motion_check_ = nullptr;
    QCheckBox* diagnostics_check_ = nullptr;
    bool loaded_ = false;
    bool loading_ = false;
};

}
