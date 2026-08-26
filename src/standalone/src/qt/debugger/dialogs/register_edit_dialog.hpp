#pragma once

#include "qt/bridge/aida_dialog.hpp"

#include <QPointer>
#include <QString>

#include <cstdint>

#include "core/debugger/debugger_interaction_context.hpp"

class QLabel;
class QLineEdit;
class QPushButton;

namespace aida::qt::debugger {

// "Edit Register" modal (ports the cpu_edit popup). Apply runs the verbatim
// mutation body: set_register + get_registers readback verify through the
// mutation queue; identity revalidation via RevalidateScope (stop generation +
// register identity).
class RegisterEditDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    static void openFor(const debugger_interaction::context_t& context,
                        const QString& registerName, std::uint64_t initialValue,
                        QWidget* parent);

private:
    RegisterEditDialog(const debugger_interaction::context_t& context,
                       QString registerName, std::uint64_t initialValue,
                       QWidget* parent);

    void apply();

    debugger_interaction::context_t context_;
    QString register_name_;
    std::uint64_t initial_value_ = 0;
    QLabel* current_label_ = nullptr;
    QLineEdit* value_edit_ = nullptr;
    QPushButton* apply_button_ = nullptr;
    QLabel* gate_label_ = nullptr;

    static QPointer<RegisterEditDialog> active_;
};

}
