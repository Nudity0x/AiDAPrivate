#pragma once

#include "core/disasm/disasm_view.hpp"
#include "qt/analysis_bridge/revision_poller.hpp"

#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QHBoxLayout;
class QStackedWidget;
class QTabBar;
class QTimer;
class QToolButton;

namespace aida::qt::widgets {
class AidaBadge;
class AidaNotice;
class AidaStateView;
}

namespace aida::qt::pseudocode {

class PseudocodeLinesWidget;

class AidaPseudocodeView : public QWidget {
    Q_OBJECT
public:
    explicit AidaPseudocodeView(QWidget* parent = nullptr);
    ~AidaPseudocodeView() override;

    disasm_view::workspace_context_t context() const { return context_; }
    void refreshContext();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private Q_SLOTS:
    void onRevisionChanged(quint64 combined, quint64 overlayRevision);
    void onDecompileTick();

private:
    void recaptureContext();
    analysis_bridge::revision_sample_t sampleRevisions();
    void refreshTabs();
    void refreshToolbar();
    void refreshDiagnostics();
    void syncContentState();
    PseudocodeLinesWidget* currentLines() const;
    PseudocodeLinesWidget* linesForIdentity(const QString& identity);
    void openLocalRename(const std::string& old_name);

    disasm_view::workspace_context_t context_;
    analysis_bridge::AidaRevisionPoller* poller_ = nullptr;
    QTimer* decompile_timer_ = nullptr;
    QTabBar* tabs_ = nullptr;
    QWidget* toolbar_ = nullptr;
    QToolButton* refresh_button_ = nullptr;
    QToolButton* graph_button_ = nullptr;
    QToolButton* disasm_button_ = nullptr;
    QToolButton* copy_button_ = nullptr;
    QToolButton* cancel_button_ = nullptr;
    QToolButton* retry_button_ = nullptr;
    QToolButton* acknowledge_button_ = nullptr;
    widgets::AidaBadge* status_badge_ = nullptr;
    QToolButton* diagnostics_toggle_ = nullptr;
    QWidget* diagnostics_panel_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    std::vector<QString> tab_identities_;
    bool syncing_tabs_ = false;
};

}
