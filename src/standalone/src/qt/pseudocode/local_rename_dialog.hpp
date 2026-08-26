#pragma once

#include "core/disasm/disasm_view.hpp"
#include "qt/bridge/aida_dialog.hpp"

#include <QString>

class QLineEdit;

namespace aida::qt::widgets {
class AidaNotice;
}

namespace aida::qt::pseudocode {

class AidaPseudoLocalRenameDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    AidaPseudoLocalRenameDialog(disasm_view::workspace_context_t context,
                                QString old_name, QWidget* parent = nullptr);

private:
    void apply();

    disasm_view::workspace_context_t context_;
    QString old_name_;
    QLineEdit* editor_ = nullptr;
    widgets::AidaNotice* error_ = nullptr;
};

}
