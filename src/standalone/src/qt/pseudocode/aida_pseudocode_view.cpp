#include "qt/pseudocode/aida_pseudocode_view.hpp"

#include "qt/pseudocode/local_rename_dialog.hpp"
#include "qt/pseudocode/pseudocode_lines_widget.hpp"
#include "qt/analysis_bridge/disasm_workspace_model.hpp"
#include "qt/analysis_bridge/pseudocode_session.hpp"
#include "qt/analysis_bridge/revision_combine.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_badge.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_state_view.hpp"

#include "core/analysis/workspace/workspace_registry.hpp"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace aida::qt::pseudocode {

using aida::workbench::pseudocode_document::pseudocode_cache_state_t;

AidaPseudocodeView::AidaPseudocodeView(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.document.pseudocode.primary"));
    bridge::InteractionContextProvider::attach_scope(this,
        QStringLiteral("scope.document.pseudocode"),
        aida::ui::focus_scope_kind_t::document);
    auto* layout = new QVBoxLayout(this);
    const auto& t = theme::tokens();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("aida.pseudocode.tabs"));
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setExpanding(false);
    layout->addWidget(tabs_);

    toolbar_ = new QWidget(this);
    toolbar_->setObjectName(QStringLiteral("aida.pseudocode.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar_);
    toolbar_layout->setContentsMargins(t.toolbar.padding_x, t.toolbar.padding_y,
        t.toolbar.padding_x, t.toolbar.padding_y);
    toolbar_layout->setSpacing(t.toolbar.group_gap);
    const auto make_button = [this](const QString& id, const QString& label,
                                    const QString& tooltip) {
        auto* button = new QToolButton(toolbar_);
        button->setObjectName(QStringLiteral("aida.pseudocode.toolbar.") + id);
        button->setText(label);
        button->setToolTip(tooltip);
        button->setAutoRaise(true);
        return button;
    };
    refresh_button_ = make_button(QStringLiteral("refresh"),
        QStringLiteral("Refresh  F5"),
        QStringLiteral("Regenerate or focus this pseudocode document (F5)"));
    graph_button_ = make_button(QStringLiteral("graph"), QStringLiteral("Graph  Space"),
        QStringLiteral("Open the current function as a control-flow graph (Space)"));
    disasm_button_ = make_button(QStringLiteral("disassembly"),
        QStringLiteral("Disassembly  Enter"),
        QStringLiteral("Open the mapped disassembly location (Enter)"));
    copy_button_ = make_button(QStringLiteral("copy"), QStringLiteral("Copy All"),
        QStringLiteral("Copy the complete pseudocode document"));
    cancel_button_ = make_button(QStringLiteral("cancel"), QStringLiteral("Cancel"),
        QStringLiteral("Cancel the active decompilation"));
    retry_button_ = make_button(QStringLiteral("retry"), QStringLiteral("Retry"),
        QStringLiteral("Run decompilation again"));
    acknowledge_button_ = make_button(QStringLiteral("acknowledge"),
        QStringLiteral("Acknowledge"),
        QStringLiteral("Acknowledge the current diagnostic"));
    status_badge_ = new widgets::AidaBadge(QStringLiteral("Not generated"),
        widgets::AidaSemantic::Neutral, toolbar_);
    toolbar_layout->addWidget(refresh_button_);
    toolbar_layout->addWidget(graph_button_);
    toolbar_layout->addWidget(disasm_button_);
    toolbar_layout->addWidget(copy_button_);
    toolbar_layout->addWidget(cancel_button_);
    toolbar_layout->addWidget(retry_button_);
    toolbar_layout->addWidget(acknowledge_button_);
    toolbar_layout->addStretch(1);
    toolbar_layout->addWidget(status_badge_);
    layout->addWidget(toolbar_);

    diagnostics_toggle_ = new QToolButton(this);
    diagnostics_toggle_->setObjectName(QStringLiteral("aida.pseudocode.diagnostics_toggle"));
    diagnostics_toggle_->setCheckable(true);
    diagnostics_toggle_->hide();
    layout->addWidget(diagnostics_toggle_);
    diagnostics_panel_ = new QWidget(this);
    diagnostics_panel_->setObjectName(QStringLiteral("aida.pseudocode.diagnostics"));
    diagnostics_panel_->setLayout(new QVBoxLayout(diagnostics_panel_));
    diagnostics_panel_->layout()->setContentsMargins(t.spacing.xs, t.spacing.xs,
        t.spacing.xs, t.spacing.xs);
    diagnostics_panel_->layout()->setSpacing(t.spacing.xxs);
    diagnostics_panel_->hide();
    layout->addWidget(diagnostics_panel_);

    stack_ = new QStackedWidget(this);
    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.pseudocode.state_view"));
    stack_->addWidget(state_view_);
    layout->addWidget(stack_, 1);

    poller_ = new analysis_bridge::AidaRevisionPoller(this);
    poller_->set_source([this] { return sampleRevisions(); });
    decompile_timer_ = new QTimer(this);
    decompile_timer_->setInterval(100);
    connect(decompile_timer_, &QTimer::timeout, this,
        &AidaPseudocodeView::onDecompileTick);

    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (syncing_tabs_ || index < 0 ||
            static_cast<std::size_t>(index) >= tab_identities_.size())
            return;
        const auto identity = tab_identities_[static_cast<std::size_t>(index)];
        if (identity.startsWith(QStringLiteral("native:"))) {
            pseudocode_view::activate_tab_by_addr(context_,
                identity.mid(7).toULongLong());
        } else {
            pseudocode_view::activate_tab_by_entity(context_,
                identity.toStdString());
        }
        refreshTabs();
    });
    connect(tabs_, &QTabBar::tabCloseRequested, this, [this](int index) {
        if (static_cast<std::size_t>(index) >= tab_identities_.size())
            return;
        const auto identity = tab_identities_[static_cast<std::size_t>(index)];
        if (identity.startsWith(QStringLiteral("native:"))) {
            pseudocode_view::close_tab_by_addr(context_,
                identity.mid(7).toULongLong());
        } else {
            pseudocode_view::close_tab_by_entity(context_,
                identity.toStdString());
        }
        refreshTabs();
    });
    connect(tabs_, &QTabBar::tabMoved, this, [this](int, int) {
        tab_identities_.clear();
        for (int index = 0; index < tabs_->count(); ++index)
            tab_identities_.push_back(tabs_->tabData(index).toString());
    });
    connect(refresh_button_, &QToolButton::clicked, this, [this] {
        pseudocode_view::refresh_active_tab(context_);
    });
    connect(graph_button_, &QToolButton::clicked, this, [this] {
        const auto address = pseudocode_view::active_tab_address(context_);
        if (address != 0)
            pseudocode_view::navigate_to_graph(context_, address);
    });
    connect(disasm_button_, &QToolButton::clicked, this, [this] {
        const auto address = pseudocode_view::active_tab_address(context_);
        if (address != 0)
            pseudocode_view::navigate_to_disassembly(context_, address);
    });
    connect(copy_button_, &QToolButton::clicked, this, [this] {
        if (auto* lines = currentLines())
            lines->copyAll();
    });
    connect(cancel_button_, &QToolButton::clicked, this, [this] {
        pseudocode_view::cancel_active_decompile(context_);
    });
    connect(retry_button_, &QToolButton::clicked, this, [this] {
        pseudocode_view::refresh_active_tab(context_);
    });
    connect(acknowledge_button_, &QToolButton::clicked, this, [this] {
        pseudocode_view::acknowledge_active_error(context_);
        refreshToolbar();
    });
    connect(diagnostics_toggle_, &QToolButton::toggled, this, [this](bool checked) {
        diagnostics_panel_->setVisible(checked);
    });
    connect(poller_, &analysis_bridge::AidaRevisionPoller::revisionChanged, this,
        &AidaPseudocodeView::onRevisionChanged);
    connect(poller_, &analysis_bridge::AidaRevisionPoller::uiSerialChanged, this, [this] {
        refreshTabs();
        refreshToolbar();
        refreshDiagnostics();
        syncContentState();
        if (pseudocode_view::any_tab_requesting(context_) &&
            !decompile_timer_->isActive())
            decompile_timer_->start();
    });
    connect(poller_, &analysis_bridge::AidaRevisionPoller::sourceInvalidated, this, [this] {
        recaptureContext();
        refreshTabs();
    });
    connect(state_view_, &widgets::AidaStateView::actionTriggered, this, [this] {
        if (context_)
            pseudocode_view::refresh_active_tab(context_);
    });

    recaptureContext();
    refreshTabs();
    syncContentState();
}

AidaPseudocodeView::~AidaPseudocodeView()
{
    poller_->set_polling(false);
    decompile_timer_->stop();
}

void AidaPseudocodeView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    poller_->set_polling(true);
    recaptureContext();
    refreshTabs();
    onDecompileTick();
}

void AidaPseudocodeView::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    poller_->set_polling(false);
    decompile_timer_->stop();
}

void AidaPseudocodeView::recaptureContext()
{
    const auto previous_workspace = context_.workspace;
    context_ = disasm_view::capture_selected_workspace();
    if (context_.workspace == previous_workspace)
        return;
    for (int index = 0; index < stack_->count(); ++index) {
        if (auto* lines = qobject_cast<PseudocodeLinesWidget*>(stack_->widget(index)))
            lines->setContext(context_);
    }
}

analysis_bridge::revision_sample_t AidaPseudocodeView::sampleRevisions()
{
    analysis_bridge::revision_sample_t sample;
    const auto workspace = aida::analysis::workspace_registry().selected_for_ui();
    if (!workspace || workspace->closed())
        return sample;
    if (context_.workspace != workspace)
        recaptureContext();
    if (!context_.workspace)
        return sample;
    sample.workspace = context_.workspace.get();
    sample.generation = context_.workspace->generation();
    sample.analysis_revision = context_.workspace->analysis_revision();
    sample.overlay_revision = context_.workspace->overlay_revision();
    sample.view_revision = context_.workspace->view_state().revision;
    const int tabs = pseudocode_view::tab_count(context_);
    const bool requesting = pseudocode_view::any_tab_requesting(context_);
    const quint64 header = pseudocode_view::active_tab_address(context_);
    sample.ui_serial = (static_cast<quint64>(tabs) & 0xFFFFull) |
        (requesting ? 0x10000ull : 0ull) |
        ((header & 0xFFFFull) << 32);
    sample.valid = true;
    return sample;
}

void AidaPseudocodeView::onRevisionChanged(quint64, quint64)
{
    recaptureContext();
    refreshTabs();
    refreshToolbar();
    refreshDiagnostics();
    syncContentState();
}

void AidaPseudocodeView::onDecompileTick()
{
    if (!context_)
        return;
    pseudocode_view::refresh_tab_states(context_);
    refreshTabs();
    refreshToolbar();
    refreshDiagnostics();
    syncContentState();
    if (auto* lines = currentLines())
        lines->reload();
    if (!pseudocode_view::any_tab_requesting(context_))
        decompile_timer_->stop();
}

PseudocodeLinesWidget* AidaPseudocodeView::currentLines() const
{
    return qobject_cast<PseudocodeLinesWidget*>(stack_->currentWidget());
}

PseudocodeLinesWidget* AidaPseudocodeView::linesForIdentity(const QString& identity)
{
    for (int index = 0; index < stack_->count(); ++index) {
        auto* lines = qobject_cast<PseudocodeLinesWidget*>(stack_->widget(index));
        if (lines && lines->property("aida.tab.identity").toString() == identity)
            return lines;
    }
    return nullptr;
}

void AidaPseudocodeView::refreshTabs()
{
    if (!context_)
        return;
    const auto tabs = pseudocode_view::snapshot_tab_views(context_);
    const int active = pseudocode_view::active_tab_index(context_);
    syncing_tabs_ = true;
    std::vector<QString> identities;
    identities.reserve(tabs.size());
    for (const auto& tab : tabs) {
        identities.push_back(tab.entity_locator.empty()
            ? QStringLiteral("native:%1").arg(tab.address)
            : QString::fromStdString(tab.entity_locator));
    }
    for (int index = tabs_->count() - 1; index >= 0; --index) {
        const auto identity = tabs_->tabData(index).toString();
        if (std::find(identities.begin(), identities.end(), identity) == identities.end()) {
            tabs_->removeTab(index);
        }
    }
    for (int index = stack_->count() - 1; index >= 1; --index) {
        auto* lines = qobject_cast<PseudocodeLinesWidget*>(stack_->widget(index));
        if (!lines)
            continue;
        const auto identity = lines->property("aida.tab.identity").toString();
        if (std::find(identities.begin(), identities.end(), identity) == identities.end()) {
            stack_->removeWidget(lines);
            lines->deleteLater();
        }
    }
    const QFontMetrics tab_metrics(tabs_->font());
    const int tab_text_budget = (std::max)(1,
        qRound(tab_metrics.averageCharWidth() * 48.0));
    for (std::size_t index = 0; index < tabs.size(); ++index) {
        const auto& tab = tabs[index];
        const auto& identity = identities[index];
        const QString label = tab_metrics.elidedText(
            QString::fromStdString(tab.label) +
                (tab.state == pseudocode_cache_state_t::requesting
                    ? QStringLiteral("  *")
                    : (tab.state == pseudocode_cache_state_t::failed ||
                       tab.state == pseudocode_cache_state_t::stale)
                        ? QStringLiteral("  !") : QString()),
            Qt::ElideMiddle, tab_text_budget);
        int tab_index = -1;
        for (int candidate = 0; candidate < tabs_->count(); ++candidate) {
            if (tabs_->tabData(candidate).toString() == identity) {
                tab_index = candidate;
                break;
            }
        }
        if (tab_index < 0) {
            tab_index = tabs_->addTab(label);
            tabs_->setTabData(tab_index, identity);
            tabs_->setTabToolTip(tab_index, QString::fromStdString(tab.label));
            auto* lines = new PseudocodeLinesWidget(this);
            lines->setContext(context_);
            lines->setProperty("aida.tab.identity", identity);
            lines->set_rename_local_handler([this](std::string old_name) {
                openLocalRename(old_name);
            });
            connect(lines, &PseudocodeLinesWidget::navigateToDisassembly, this,
                [this](quint64 address) {
                    pseudocode_view::navigate_to_disassembly(context_, address);
                });
            connect(lines, &PseudocodeLinesWidget::navigateToGraph, this,
                [this](quint64 address) {
                    pseudocode_view::navigate_to_graph(context_, address);
                });
            stack_->addWidget(lines);
        } else {
            tabs_->setTabText(tab_index, label);
            tabs_->setTabToolTip(tab_index, QString::fromStdString(tab.label));
        }
    }
    tab_identities_ = identities;
    syncing_tabs_ = false;
    if (active >= 0 && active < tabs_->count()) {
        if (tabs_->currentIndex() != active) {
            syncing_tabs_ = true;
            tabs_->setCurrentIndex(active);
            syncing_tabs_ = false;
        }
    }
    if (active >= 0 && static_cast<std::size_t>(active) < tab_identities_.size()) {
        if (auto* lines = linesForIdentity(tab_identities_[static_cast<std::size_t>(active)]))
            stack_->setCurrentWidget(lines);
        if (auto* model_ptr = pseudocode_view::document_model(context_)) {
            const auto tab = pseudocode_view::active_tab_view(context_);
            if (tab && tab->has_request)
                static_cast<void>(model_ptr->activate(tab->request));
        }
        if (auto* lines = currentLines())
            lines->reload();
    }
    if (pseudocode_view::any_tab_requesting(context_)) {
        if (!decompile_timer_->isActive())
            decompile_timer_->start();
    }
}

void AidaPseudocodeView::refreshToolbar()
{
    const auto tab = pseudocode_view::active_tab_view(context_);
    const bool has_active = tab.has_value();
    auto* model_ptr = pseudocode_view::document_model(context_);
    const aida::workbench::pseudocode_document::pseudocode_cached_document_t* cached =
        nullptr;
    if (has_active && tab->has_request && model_ptr &&
        model_ptr->activate(tab->request))
        cached = model_ptr->cached_document(tab->request);
    const auto status = cached ? cached->state
        : (has_active ? tab->state : pseudocode_cache_state_t::empty);
    const bool can_copy = cached && cached->document &&
        status == pseudocode_cache_state_t::cached;
    const bool can_cancel = status == pseudocode_cache_state_t::requesting;
    const bool can_retry = status == pseudocode_cache_state_t::failed ||
        status == pseudocode_cache_state_t::stale ||
        status == pseudocode_cache_state_t::cancelled;
    const auto header_address = pseudocode_view::active_tab_address(context_);
    graph_button_->setEnabled(has_active && header_address != 0);
    disasm_button_->setEnabled(has_active && header_address != 0);
    copy_button_->setEnabled(can_copy);
    cancel_button_->setVisible(can_cancel);
    retry_button_->setVisible(can_retry);
    acknowledge_button_->setVisible(can_retry && has_active && !tab->error_acknowledged);
    if (status == pseudocode_cache_state_t::requesting) {
        status_badge_->setText(QStringLiteral("Decompiling"));
        status_badge_->setKind(widgets::AidaSemantic::Info);
    } else if (can_retry) {
        status_badge_->setText(QStringLiteral("Action required"));
        status_badge_->setKind(widgets::AidaSemantic::Error);
    } else if (can_copy) {
        status_badge_->setText(QStringLiteral("Ready"));
        status_badge_->setKind(widgets::AidaSemantic::Success);
    } else {
        status_badge_->setText(QStringLiteral("Not generated"));
        status_badge_->setKind(widgets::AidaSemantic::Neutral);
    }
}

void AidaPseudocodeView::refreshDiagnostics()
{
    auto* model_ptr = pseudocode_view::document_model(context_);
    const auto diagnostics = model_ptr ? model_ptr->diagnostics()
        : std::vector<aida::workbench::pseudocode_document::pseudocode_diagnostic_view_t>();
    auto* panel_layout = diagnostics_panel_->layout();
    while (auto* item = panel_layout->takeAt(0)) {
        if (auto* widget = item->widget())
            delete widget;
        delete item;
    }
    if (diagnostics.empty()) {
        diagnostics_toggle_->hide();
        diagnostics_panel_->hide();
        return;
    }
    diagnostics_toggle_->setText(QStringLiteral("Decompiler diagnostics (%1)")
        .arg(diagnostics.size()));
    diagnostics_toggle_->show();
    int notice_index = 0;
    for (const auto& diagnostic : diagnostics) {
        const auto& message = diagnostic.message.empty()
            ? diagnostic.localization_key : diagnostic.message;
        auto* notice = new widgets::AidaNotice(QStringLiteral("Decompiler diagnostic"),
            message.empty() ? QStringLiteral("No diagnostic details were provided.")
                            : QString::fromStdString(message),
            widgets::AidaSemantic::Warning, diagnostics_panel_);
        notice->setObjectName(QStringLiteral("aida.pseudocode.diagnostics.notice.") +
            QString::number(notice_index++));
        diagnostics_panel_->layout()->addWidget(notice);
    }
    diagnostics_panel_->setVisible(diagnostics_toggle_->isChecked());
}

void AidaPseudocodeView::syncContentState()
{
    tabs_->setVisible(context_ && !tab_identities_.empty());
    if (!context_) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No workspace selected"));
        state_view_->setMessage(QStringLiteral(
            "Open a target and decompile a function to inspect pseudocode."));
        state_view_->setActionLabel(QString());
        stack_->setCurrentWidget(state_view_);
        return;
    }
    const auto tab = pseudocode_view::active_tab_view(context_);
    const bool has_active = tab.has_value() && !tab_identities_.empty();
    if (has_active) {
        const auto status = tab->state;
        auto* lines = currentLines();
        if (status == pseudocode_cache_state_t::cached && lines) {
            stack_->setCurrentWidget(lines);
            return;
        }
        if (status == pseudocode_cache_state_t::empty) {
            state_view_->setState(widgets::AidaStateView::State::Empty);
            state_view_->setTitle(QStringLiteral("Pseudocode not generated"));
            state_view_->setMessage(QStringLiteral(
                "Decompile this function explicitly to produce a source-like view."));
            state_view_->setActionLabel(QStringLiteral("Decompile"));
            stack_->setCurrentWidget(state_view_);
            return;
        }
        if (status == pseudocode_cache_state_t::requesting) {
            state_view_->setState(widgets::AidaStateView::State::Loading);
            state_view_->setTitle(QStringLiteral("Decompiling function"));
            state_view_->setMessage(QStringLiteral(
                "Analysis is reconstructing control flow, expressions, and local variables."));
            state_view_->setActionLabel(QString());
            stack_->setCurrentWidget(state_view_);
            return;
        }
        if (status == pseudocode_cache_state_t::cancelled) {
            state_view_->setState(widgets::AidaStateView::State::Error);
            state_view_->setTitle(QStringLiteral("Decompilation cancelled"));
            state_view_->setMessage(tab->error.empty()
                ? QStringLiteral("The active decompilation request was cancelled.")
                : QString::fromStdString(tab->error));
            state_view_->setActionLabel(QStringLiteral("Retry"));
            stack_->setCurrentWidget(state_view_);
            return;
        }
        state_view_->setState(widgets::AidaStateView::State::Error);
        state_view_->setTitle(QStringLiteral("Decompilation failed"));
        state_view_->setMessage(tab->error_acknowledged
            ? QStringLiteral("The diagnostic was acknowledged. Retry to run decompilation again.")
            : (tab->error.empty()
                ? QStringLiteral("Decompilation failed without a result.")
                : QString::fromStdString(tab->error)));
        state_view_->setActionLabel(QStringLiteral("Retry"));
        stack_->setCurrentWidget(state_view_);
        return;
    }
    state_view_->setState(widgets::AidaStateView::State::Empty);
    state_view_->setTitle(QStringLiteral("No function selected"));
    state_view_->setMessage(QStringLiteral(
        "Press F5 in disassembly or choose Decompile function to open pseudocode."));
    state_view_->setActionLabel(QString());
    stack_->setCurrentWidget(state_view_);
}

void AidaPseudocodeView::openLocalRename(const std::string& old_name)
{
    auto* dialog = new AidaPseudoLocalRenameDialog(context_,
        QString::fromStdString(old_name), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

}
