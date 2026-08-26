#pragma once

#include "qt/debugger/debugger_pane_base.hpp"

#include "core/runtime/standalone_driver.hpp"

class QTabWidget;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaLineEdit;
class AidaPill;
}

namespace aida::qt::debugger {

class ModulesModel;
class ModuleExportsModel;
class ModuleImportsModel;

// Modules pane: modules table (identity by base) + imports/exports detail
// tabs + Dump Selected + selection-stale pill. Refresh: 5000ms CAS auto +
// dll_loaded/process_created events (backend subscriptions; the pane polls the
// backend store's data_generation).
class ModulesPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit ModulesPane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onHidden() override;
    void onSessionTick() override;
    bool hasContentRows() const override;
    bool isContentLoading() const override;
    bool contentError(QString* detail) const override;

private:
    void pollModel();
    void onModuleSelected(int row);
    void dumpSelected();
    void updateSelectionPill();

    ModulesModel* modules_model_ = nullptr;
    QTableView* modules_view_ = nullptr;
    ModuleExportsModel* exports_model_ = nullptr;
    ModuleImportsModel* imports_model_ = nullptr;
    QTableView* exports_view_ = nullptr;
    QTableView* imports_view_ = nullptr;
    QTabWidget* detail_tabs_ = nullptr;
    widgets::AidaLineEdit* filter_edit_ = nullptr;
    widgets::AidaButton* dump_button_ = nullptr;
    widgets::AidaButton* refresh_button_ = nullptr;
    widgets::AidaPill* selection_pill_ = nullptr;
    QTimer* poll_timer_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    QString last_error_;
    std::uint64_t last_data_generation_ = 0;
};

}
