#pragma once

#include "qt/debugger/debugger_pane_base.hpp"

#include <QString>

#include "core/debugger/source_debug_service.hpp"
#include "core/ui/context_menu_contract.hpp"

class QLabel;
class QSplitter;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::debugger {

class DebuggerTableModelBase;
class DisasmSliceWidget;

// Source / Assembly pane hosting source_debug_service: the toolbar (Open
// Source / Open Assembly / Toggle BP / Rebind), the location status strip,
// the source excerpt with the current-line marker, the disassembly slice at
// the stopped address, and the persistent source-breakpoint table with the
// retained entity menu (debugger.source.breakpoint).
class SourcePane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit SourcePane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onHidden() override;
    void onSessionTick() override;
    bool hasContentRows() const override;
    bool isContentLoading() const override;
    bool contentError(QString* detail) const override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void poll();
    void openCurrentSource();
    void openCurrentDisassembly();
    void toggleCurrentBreakpoint();
    void rebindAll();
    void openDefinitionMenu(const QPoint& pos,
        aida::ui::context_menu_open_origin_t origin =
            aida::ui::context_menu_open_origin_t::pointer);
    void report(bool accepted, const std::string& error,
                const char* accepted_text);

    QLabel* location_label_ = nullptr;
    QLabel* location_detail_ = nullptr;
    widgets::AidaButton* open_source_button_ = nullptr;
    widgets::AidaButton* open_asm_button_ = nullptr;
    widgets::AidaButton* toggle_bp_button_ = nullptr;
    widgets::AidaButton* rebind_button_ = nullptr;
    DebuggerTableModelBase* excerpt_model_ = nullptr;
    QTableView* excerpt_view_ = nullptr;
    DisasmSliceWidget* disasm_widget_ = nullptr;
    DebuggerTableModelBase* definitions_model_ = nullptr;
    QTableView* definitions_view_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    QString snapshot_error_;
    bool snapshot_had_current_ = false;
    bool snapshot_operation_pending_ = false;
    std::uint64_t last_generation_ = 0;
};

}
