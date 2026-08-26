#pragma once

#include "qt/bridge/aida_dialog.hpp"

#include <QPointer>

#include <cstdint>

#include "core/debugger/debugger_interaction_context.hpp"

class QComboBox;
class QLabel;
class QPushButton;

namespace aida::qt::debugger {

// "Change Protection" modal (ports the memory-map protection dialog): the 8
// PAGE_* entries verbatim, the capability gate line via
// debugger_interaction::evaluate(change_memory_protection), Apply gated on
// gate && !pending; Apply queues the worker (protect + readback verify) through
// DebuggerMutationQueue::changeProtection.
class ChangeProtectionDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    static void openFor(const debugger_interaction::context_t& context,
                        std::uint64_t address, std::uint64_t size,
                        std::uint32_t old_protect, QWidget* parent);

private:
    ChangeProtectionDialog(const debugger_interaction::context_t& context,
                           std::uint64_t address, std::uint64_t size,
                           std::uint32_t old_protect, QWidget* parent);

    void apply();

    debugger_interaction::context_t context_;
    std::uint64_t address_ = 0;
    std::uint64_t size_ = 0;
    std::uint32_t old_protect_ = 0;
    QComboBox* protect_combo_ = nullptr;
    QPushButton* apply_button_ = nullptr;
    QLabel* gate_label_ = nullptr;

    static QPointer<ChangeProtectionDialog> active_;
};

}
