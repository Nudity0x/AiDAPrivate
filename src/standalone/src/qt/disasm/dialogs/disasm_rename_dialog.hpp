#pragma once

#include "core/disasm/disasm_view.hpp"
#include "qt/bridge/aida_dialog.hpp"

#include <QString>

class QLineEdit;

namespace aida::qt::widgets {
class AidaNotice;
}

namespace aida::qt::disasm::dialogs {

class AidaDisasmRenameDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    AidaDisasmRenameDialog(disasm_view::workspace_context_t context,
                           aida::analysis::address_t address,
                           QWidget* parent = nullptr);

private:
    void apply();

    disasm_view::workspace_context_t context_;
    aida::analysis::address_t address_;
    QLineEdit* editor_ = nullptr;
    widgets::AidaNotice* error_ = nullptr;
};

}
