#pragma once

#include "core/disasm/disasm_view.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QLineEdit;

namespace aida::qt::widgets {
class AidaNotice;
}

namespace aida::qt::disasm::dialogs {

class AidaDisasmRebaseDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    explicit AidaDisasmRebaseDialog(disasm_view::workspace_context_t context,
                                    QWidget* parent = nullptr);

private:
    void apply();

    disasm_view::workspace_context_t context_;
    QLineEdit* editor_ = nullptr;
    widgets::AidaNotice* error_ = nullptr;
};

}
