#pragma once

#include "core/disasm/disasm_view.hpp"
#include "qt/bridge/aida_dialog.hpp"

#include <QString>

class QPlainTextEdit;

namespace aida::qt::widgets {
class AidaNotice;
}

namespace aida::qt::disasm::dialogs {

class CommentTextEdit;

class AidaDisasmCommentDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    AidaDisasmCommentDialog(disasm_view::workspace_context_t context,
                            aida::analysis::address_t address,
                            QWidget* parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void apply();

    disasm_view::workspace_context_t context_;
    aida::analysis::address_t address_;
    CommentTextEdit* editor_ = nullptr;
    widgets::AidaNotice* error_ = nullptr;
};

}
