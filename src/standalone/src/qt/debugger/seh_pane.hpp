#pragma once

#include "qt/debugger/debugger_pane_base.hpp"

class QLabel;
class QTableView;

namespace aida::qt::widgets {
class AidaButton;
class AidaPill;
}

namespace aida::qt::debugger {

class SehModel;

// SEH chain pane: entries table over the backend seh_view store + the
// diagnostics panel (TEB query/chain-walk fields verbatim). Refresh submits
// the backend single-flight worker (TEB query + chain walk).
class SehPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit SehPane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onSessionTick() override;
    bool hasContentRows() const override;
    bool isContentLoading() const override;
    bool contentError(QString* detail) const override;

private:
    void pollModel();

    SehModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    widgets::AidaButton* refresh_button_ = nullptr;
    widgets::AidaPill* scope_pill_ = nullptr;
    QLabel* diagnostics_label_ = nullptr;
    QString last_error_;
    std::uint64_t last_generation_ = 0;
    std::uint64_t last_diag_signature_ = 0;
};

}
