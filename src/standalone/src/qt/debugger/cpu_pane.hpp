#pragma once

#include "qt/debugger/debugger_pane_base.hpp"

#include <QPointer>

#include <cstdint>

class QSplitter;
class QTableView;
class QTimer;

namespace aida::qt::debugger {

class RegistersModel;
class DisasmSliceWidget;
class RflagsChipsWidget;
class StackQuadWidget;
class DebuggerRunToolBar;
class DebuggerStatusStrip;

// CPU pane: QSplitter LEFT [RegistersTable over RflagsChipsWidget] RIGHT
// [DisasmSliceWidget over StackQuadWidget]. The integrated surface hosts the
// run toolbar + status strip; registers_only/stack_only serve the standalone
// Registers/Stack dock views (ports render_cpu_surface_pane).
class CpuPaneWidget : public DebuggerPaneBase {
    Q_OBJECT
public:
    enum class Surface { integrated, registers_only, stack_only };

    explicit CpuPaneWidget(Surface surface, QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onHidden() override;
    bool hasTargetContent() const override;
    bool hasContentRows() const override;
    bool isContentLoading() const override;
    void onSessionStateChanged(int status, quint32 pid,
                               quint64 stopGeneration) override;
    void onSessionTick() override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void refreshRegisters();
    void openRegisterEditor(int row);
    void openRflagsEditor(const QString& flagName, std::uint64_t current,
                          std::uint64_t toggled);
    void updateNarrowGuard();

    Surface surface_;
    QSplitter* root_splitter_ = nullptr;
    QSplitter* right_splitter_ = nullptr;
    QTableView* registers_view_ = nullptr;
    RegistersModel* registers_model_ = nullptr;
    RflagsChipsWidget* flags_widget_ = nullptr;
    DisasmSliceWidget* disasm_widget_ = nullptr;
    StackQuadWidget* stack_widget_ = nullptr;
    DebuggerRunToolBar* run_toolbar_ = nullptr;
    DebuggerStatusStrip* status_strip_ = nullptr;
    QTimer* registers_timer_ = nullptr;
    QTimer* disasm_timer_ = nullptr;
    QTimer* stack_timer_ = nullptr;
    QWidget* narrow_notice_ = nullptr;
    bool narrow_logged_ = false;
    bool registers_seen_ = false;
    quint32 last_seen_pid_ = 0;
};

}
