#pragma once

#include "qt/bridge/aida_dialog.hpp"

#include <QPointer>
#include <QString>

#include <cstdint>
#include <vector>

#include "core/debugger/debugger_view.hpp"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace aida::qt::debugger {

// "Stage Patch Review" modal (ports render_global_dialogs patch staging).
// Stages an INACTIVE definition only -- it does not write target memory.
// Apply gates on parse validity + live target identity (is_current), queues
// the verbatim rollback-byte capture through the backend commit path.
class StagePatchDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    static void present(const debugger_view::patch_stage_review_t& review,
                        QWidget* parent);

private:
    explicit StagePatchDialog(debugger_view::patch_stage_review_t review,
                              QWidget* parent);

    void reparse();
    void stage();

    debugger_view::patch_stage_review_t review_;
    QPlainTextEdit* bytes_edit_ = nullptr;
    QLineEdit* description_edit_ = nullptr;
    QLabel* validation_label_ = nullptr;
    QLabel* target_label_ = nullptr;
    QPushButton* stage_button_ = nullptr;
    std::vector<std::uint8_t> parsed_;
    bool parse_valid_ = false;
};

}
