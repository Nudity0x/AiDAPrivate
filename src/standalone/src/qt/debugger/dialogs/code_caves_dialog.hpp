#pragma once

#include "qt/bridge/aida_dialog.hpp"

#include <QPointer>
#include <QString>

#include <cstdint>
#include <memory>
#include <string>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTimer;

namespace aida::qt::debugger {

class CodeCavesModel;

// "Find Code Caves" dialog (ports g_code_cave_search modal). The search worker
// stays backend-side (single-flight pending + publication with exact
// process/stop-generation identity); the dialog observes the publication on a
// 250ms timer and revalidates target currency before staging.
class CodeCavesDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    static void present(QWidget* parent);

private:
    explicit CodeCavesDialog(QWidget* parent);

    void pollPublication();
    void startSearch();
    void stageSelected();

    QLineEdit* module_filter_edit_ = nullptr;
    QLineEdit* minimum_edit_ = nullptr;
    QPushButton* search_button_ = nullptr;
    QTableView* results_view_ = nullptr;
    CodeCavesModel* results_model_ = nullptr;
    QLabel* publication_label_ = nullptr;
    QLabel* notice_label_ = nullptr;
    QLabel* error_label_ = nullptr;
    QPushButton* stage_button_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    std::uint64_t visible_generation_ = 0;
    QString dialog_error_;

    static QPointer<CodeCavesDialog> active_;
};

}
