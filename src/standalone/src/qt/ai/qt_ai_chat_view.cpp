#include "qt/ai/qt_ai_chat_view.hpp"

#include <QAction>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedLayout>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "core/ai/conversation_evidence_store.hpp"
#include "core/mcp/mcp_client.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "qt/ai/qt_ai_chat_composer.hpp"
#include "qt/ai/qt_ai_chat_dialogs.hpp"
#include "qt/ai/qt_ai_chat_history.hpp"
#include "qt/ai/qt_ai_chat_pills.hpp"
#include "qt/ai/qt_ai_domain.hpp"
#include "qt/ai/qt_ai_evidence_view.hpp"
#include "qt/ai/qt_agent_manager_view.hpp"
#include "qt/ai/qt_agent_picker_dialog.hpp"
#include "qt/ai/qt_chat_inject.hpp"
#include "qt/ai/qt_command_palette.hpp"
#include "qt/ai/qt_provider_view.hpp"
#include "qt/ai/qt_skill_manager_view.hpp"
#include "qt/ai/qt_tool_approval.hpp"
#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/theme/aida_theme_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::ai {

using aida::automation_ui::message_action_t;
using aida::automation_ui::message_identity_t;

namespace {

constexpr std::size_t k_render_window = 256;

}

AidaChatController::AidaChatController(QObject* parent) : QObject(parent) {
    model_ = new AidaChatMessageModel(this);
    cache_ = new AidaChatDocumentCache();
    reveal_timer_ = new QTimer(this);
    reveal_timer_->setInterval(theme::tokens().motion.instant);
    connect(reveal_timer_, &QTimer::timeout, this, [this] {
        if (delegate_ && delegate_->advanceStreamingReveal())
            model_->touchLastRow();
        const auto total = aida::automation_ui::message_count();
        bool streaming = false;
        if (total > 0) {
            aida::automation_ui::chat_message_snapshot_t snapshot;
            if (aida::automation_ui::message_snapshot(total - 1, snapshot))
                streaming = snapshot.streaming;
        }
        if (!streaming)
            reveal_timer_->stop();
    });
    mcp_poll_timer_ = new QTimer(this);
    mcp_poll_timer_->setInterval(5000);
    connect(mcp_poll_timer_, &QTimer::timeout, this, [] {
        get_mcp_client_manager().poll();
    });
    catalog_fallback_timer_ = new QTimer(this);
    catalog_fallback_timer_->setInterval(500);
    catalog_fallback_timer_->setTimerType(Qt::CoarseTimer);
    connect(catalog_fallback_timer_, &QTimer::timeout, this, [this] {
        conversations::process_store_completion(true);
    });
    catalog_fallback_timer_->start();
}

AidaChatController& AidaChatController::instance() {
    static AidaChatController* controller = [] {
        auto* created = new AidaChatController();
        created->installBackendHooks();
        return created;
    }();
    return *controller;
}

void AidaChatController::installBackendHooks() {
    aida::automation_ui::set_stream_notify_hook([this] {
        scheduleDrain();
    });
    aida::automation_ui::set_chat_clipboard_hook([](const std::string& text) {
        clipboard::set_text(QString::fromStdString(text));
    });
    aida::automation_ui::set_chat_open_view_hook([](const std::string& view_id) {
        open_ai_view(view_id);
    });
    aida::automation_ui::set_agent_picker_toggle_hook([this] {
        QMetaObject::invokeMethod(this, [this] {
            Q_EMIT toggleAgentPickerRequested();
        }, Qt::QueuedConnection);
    });
    aida::automation_ui::set_message_edit_notify_hook([this] {
        QMetaObject::invokeMethod(this, [this] {
            message_identity_t identity;
            std::string text;
            if (aida::automation_ui::consume_pending_message_edit(identity, text))
                Q_EMIT editMessageRequested(QString::fromStdString(text));
        }, Qt::QueuedConnection);
    });
    mcp_poll_timer_->start();
}

void AidaChatController::attachView(AidaChatView* view, AidaChatMessageDelegate* delegate) {
    view_ = view;
    delegate_ = delegate;
    if (pending_composer_clear_) {
        pending_composer_clear_ = false;
        view_->clearComposerText();
    }
    for (const QString& text : pending_composer_appends_)
        view_->appendComposerText(text);
    pending_composer_appends_.clear();
}

void AidaChatController::detachView(AidaChatView* view) {
    if (view_ == view) {
        view_ = nullptr;
        delegate_ = nullptr;
    }
}

void AidaChatController::onInjectAppend(const QString& text) {
    if (text.isEmpty())
        return;
    if (view_ != nullptr) {
        view_->appendComposerText(text);
        return;
    }
    pending_composer_appends_.append(text);
}

void AidaChatController::onInjectClear() {
    pending_composer_appends_.clear();
    if (view_ != nullptr) {
        pending_composer_clear_ = false;
        view_->clearComposerText();
        return;
    }
    pending_composer_clear_ = true;
}

bool AidaChatController::injectAccepts(const QString& text) const {
    if (text.isEmpty())
        return false;
    int size = 0;
    for (const QString& pending : pending_composer_appends_)
        size += pending.size() + 2;
    if (view_ != nullptr)
        size += view_->composerTextSize();
    return size + text.size() < 4095;
}

void AidaChatController::scheduleDrain() {
    if (drain_queued_.exchange(true, std::memory_order_acq_rel))
        return;
    QMetaObject::invokeMethod(this, [this] {
        drain_queued_.store(false, std::memory_order_release);
        drain();
    }, Qt::QueuedConnection);
}

void AidaChatController::onSessionIdentityChanged() {
    cache_->clear();
    if (delegate_)
        delegate_->invalidateHeights();
    model_->setWindowState(0, true);
    previous_total_ = 0;
    model_->resetWindow();
    previous_total_ = model_->window().total;
    Q_EMIT sessionReset();
    Q_EMIT scrollToBottomRequested();
}

void AidaChatController::updateModelAfterChange(const ai_chat_poll_result_t& result) {
    const std::string active = chat_active_session();
    const std::string conversation = conversations::current_id;
    if (!conversation.empty() && conversation != active) {
        chat_bind_session(conversation);
        aida::automation_ui::synchronize_evidence_session();
    }
    const std::string current = chat_active_session();
    if (current != session_id_) {
        session_id_ = current;
        onSessionIdentityChanged();
        return;
    }

    const std::size_t total = result.message_total;
    if (total != previous_total_) {
        const auto update = model_->applyBackendChange(previous_total_);
        if (update == AidaChatMessageModel::window_update_t::reset && delegate_)
            delegate_->invalidateHeights();
        previous_total_ = total;
    } else {
        model_->refreshWindow();
    }

    if (result.content_grew || result.thinking_started) {
        model_->touchLastRow();
        if (!reveal_timer_->isActive())
            reveal_timer_->start();
    }
    if (result.settled) {
        const auto count = aida::automation_ui::message_count();
        if (count > 0) {
            if (delegate_) {
                aida::automation_ui::chat_message_snapshot_t last_snapshot;
                if (aida::automation_ui::message_snapshot(count - 1, last_snapshot))
                    delegate_->setRevealed(count - 1, last_snapshot.text.size());
            }
            model_->touchLastRow();
        }
        if (delegate_)
            delegate_->advanceStreamingReveal();
        reveal_timer_->stop();
    }

    const std::uint64_t scroll_seq = aida::automation_ui::chat_scroll_sequence();
    if (scroll_seq != last_scroll_seq_) {
        last_scroll_seq_ = scroll_seq;
        if (model_->followLatest())
            Q_EMIT scrollToBottomRequested();
    }
    updateThinking(result.ai_busy);
    Q_EMIT stateChanged();
}

void AidaChatController::drain() {
    conversations::process_store_completion(true);
    const auto result = poll_ai_chat();
    if (!result.any) {
        updateThinking(result.ai_busy);
        return;
    }
    updateModelAfterChange(result);
}

void AidaChatController::updateThinking(bool active) {
    if (thinking_active_ == active)
        return;
    thinking_active_ = active;
    Q_EMIT thinkingActiveChanged(active);
}

void AidaChatController::submitUserText(const QString& text) {
    if (text.trimmed().isEmpty())
        return;
    const auto result = aida::automation_ui::append_user_message(text.toStdString());
    if (!result.succeeded) {
        chrome::toast_warning(QString::fromStdString(result.detail), 4.0);
        return;
    }
    model_->setWindowState(model_->window().first, true);
    ai_chat_poll_result_t local;
    local.any = true;
    local.message_total = aida::automation_ui::message_count();
    local.ai_busy = is_ai_busy();
    updateModelAfterChange(local);
    Q_EMIT scrollToBottomRequested();
}

void AidaChatController::appendUserMessage(const std::string& text) {
    const auto result = aida::automation_ui::append_user_message(text);
    if (!result.succeeded) {
        chrome::toast_warning(QString::fromStdString(result.detail), 4.0);
        return;
    }
    ai_chat_poll_result_t local;
    local.any = true;
    local.message_total = aida::automation_ui::message_count();
    local.ai_busy = is_ai_busy();
    updateModelAfterChange(local);
    Q_EMIT scrollToBottomRequested();
}

void AidaChatController::cancelActive() {
    chat_request_cancel();
    Q_EMIT stateChanged();
}

void AidaChatController::pageOlder() {
    const auto window = model_->window();
    model_->setWindowState(window.first > k_render_window
        ? window.first - k_render_window : 0, false);
    model_->resetWindow();
    previous_total_ = window.total;
}

void AidaChatController::pageNewer() {
    const auto window = model_->window();
    const std::size_t next = (std::min)(window.total > 0 ? window.total - 1 : 0,
        window.first + k_render_window);
    const bool follow = next + k_render_window >= window.total;
    model_->setWindowState(next, follow);
    model_->resetWindow();
    previous_total_ = window.total;
}

void AidaChatController::pageLatest() {
    const auto total = aida::automation_ui::message_count();
    model_->setWindowState(total > k_render_window ? total - k_render_window : 0, true);
    model_->resetWindow();
    previous_total_ = total;
    Q_EMIT scrollToBottomRequested();
}

AidaChatView::AidaChatView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.ai_chat"));
    controller_ = &AidaChatController::instance();
    buildUi();
    wireController();
    controller_->attachView(this, delegate_);
}

AidaChatView::~AidaChatView() {
    if (controller_)
        controller_->detachView(this);
}

void AidaChatView::buildUi() {
    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.spacing.xs, t.spacing.xs, t.spacing.xs, t.spacing.xs);
    root->setSpacing(t.spacing.xs);

    auto* toolbar = new QToolBar(this);
    toolbar->setObjectName(QStringLiteral("aida.ai.chat.toolbar"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    const auto store = aida::conversation_store::status();
    const bool store_blocked = store.pending || store.failed;
    auto* new_action = toolbar->addAction(QStringLiteral("New"));
    new_action->setToolTip(QStringLiteral("Start a new analysis conversation"));
    new_action->setEnabled(!store_blocked && !is_ai_busy());
    auto* history_action = toolbar->addAction(QStringLiteral("History"));
    history_action->setToolTip(
        QStringLiteral("Show or hide conversation history beside the chat"));
    auto* evidence_action = toolbar->addAction(QStringLiteral("Evidence"));
    evidence_action->setToolTip(
        QStringLiteral("Review evidence attached to AI workflows"));
    auto* providers_action = toolbar->addAction(QStringLiteral("Providers"));
    providers_action->setToolTip(
        QStringLiteral("Open AI provider and model configuration"));
    auto* agents_action = toolbar->addAction(QStringLiteral("Agents"));
    agents_action->setToolTip(QStringLiteral("Open the agent manager"));
    auto* skills_action = toolbar->addAction(QStringLiteral("Skills"));
    skills_action->setToolTip(QStringLiteral("Open the installed skills manager"));
    auto* mcp_action = toolbar->addAction(QStringLiteral("MCP"));
    mcp_action->setToolTip(
        QStringLiteral("Open the complete MCP request and tool activity log"));
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    auto* settings_action = toolbar->addAction(QStringLiteral("Settings"));
    settings_action->setToolTip(QStringLiteral("Open AiDA settings"));
    root->addWidget(toolbar);

    connect(new_action, &QAction::triggered, this, [this] {
        const auto store_now = aida::conversation_store::status();
        if (!store_now.pending && !store_now.failed && !is_ai_busy())
            conversations::new_chat();
    });
    connect(history_action, &QAction::triggered, this, [this] {
        conversations::browser_open = !conversations::browser_open;
        history_->setVisible(conversations::browser_open);
        if (conversations::browser_open)
            history_->refreshOnce();
    });
    connect(evidence_action, &QAction::triggered, this,
            [] { open_ai_view("view.ai.evidence"); });
    connect(providers_action, &QAction::triggered, this,
            [] { open_ai_view("view.ai.providers"); });
    connect(agents_action, &QAction::triggered, this,
            [] { open_ai_view("view.ai.agents"); });
    connect(skills_action, &QAction::triggered, this,
            [] { open_ai_view("view.ai.skills"); });
    connect(mcp_action, &QAction::triggered, this,
            [] { open_ai_view("view.mcp_log"); });
    connect(settings_action, &QAction::triggered, this,
            [] { open_ai_view("view.settings"); });

    auto* pill_bar = new QHBoxLayout();
    pill_bar->setSpacing(t.spacing.sm);
    model_pill_ = new AidaModelPill(this);
    model_pill_->setObjectName(QStringLiteral("aida.ai.pill.model"));
    agent_pill_ = new AidaAgentPill(this);
    agent_pill_->setObjectName(QStringLiteral("aida.ai.pill.agent"));
    skills_pill_ = new AidaSkillsPill(this);
    skills_pill_->setObjectName(QStringLiteral("aida.ai.pill.skills"));
    mcp_pill_ = new AidaMcpPill(this);
    mcp_pill_->setObjectName(QStringLiteral("aida.ai.pill.mcp"));
    sign_in_button_ = new QToolButton(this);
    sign_in_button_->setObjectName(QStringLiteral("aida.ai.sign_in"));
    sign_in_button_->setText(QStringLiteral("Sign in"));
    sign_in_button_->setToolTip(QStringLiteral("Sign in to the selected provider"));
    pill_bar->addWidget(model_pill_);
    pill_bar->addWidget(sign_in_button_);
    pill_bar->addWidget(agent_pill_);
    pill_bar->addWidget(skills_pill_);
    pill_bar->addWidget(mcp_pill_);
    pill_bar->addStretch(1);
    root->addLayout(pill_bar);
    connect(sign_in_button_, &QToolButton::clicked, this, [this] {
        Q_EMIT controller_->openSettingsForProviderRequested(model_pill_->providerId());
    });
    connect(model_pill_, &AidaModelPill::openSettingsForProvider, this,
            [this](const QString& provider_id) {
        Q_EMIT controller_->openSettingsForProviderRequested(provider_id);
    });

    auto* persistence_row = new QHBoxLayout();
    persistence_row->setSpacing(t.spacing.sm);
    persistence_label_ = new QLabel(this);
    persistence_label_->setObjectName(QStringLiteral("aida.ai.chat.persistence"));
    persistence_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    persistence_label_->setWordWrap(true);
    persistence_retry_ = new QPushButton(QStringLiteral("Retry"), this);
    persistence_retry_->setObjectName(QStringLiteral("aida.ai.chat.persistence_retry"));
    persistence_retry_->setToolTip(QStringLiteral(
        "Retry the failed conversation persistence transaction"));
    persistence_row->addWidget(persistence_label_, 1);
    persistence_row->addWidget(persistence_retry_);
    auto* persistence_widget = new QWidget(this);
    persistence_widget->setObjectName(QStringLiteral("aida.ai.chat.persistence_row"));
    persistence_widget->setLayout(persistence_row);
    root->addWidget(persistence_widget);
    connect(persistence_retry_, &QPushButton::clicked, this, [this] {
        if (aida::conversation_store::request_retry())
            conversations::persistence_error.clear();
    });

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setObjectName(QStringLiteral("aida.ai.chat.splitter"));
    history_ = new AidaConversationListView(this);
    history_->setMinimumWidth(theme::scale_logical(t.shell.min_panel_w, 2.0));
    history_->setVisible(conversations::browser_open);
    auto* chat_column = new QWidget(this);
    chat_column->setMinimumWidth(theme::scale_logical(t.shell.min_panel_w, 3.0));
    auto* chat_layout = new QVBoxLayout(chat_column);
    chat_layout->setContentsMargins(0, 0, 0, 0);
    chat_layout->setSpacing(t.spacing.xs);

    pager_bar_ = new QWidget(chat_column);
    pager_bar_->setObjectName(QStringLiteral("aida.ai.chat.pager"));
    auto* pager_layout = new QHBoxLayout(pager_bar_);
    pager_layout->setContentsMargins(0, 0, 0, 0);
    pager_layout->setSpacing(t.spacing.sm);
    pager_older_ = new QPushButton(QStringLiteral("Older"), pager_bar_);
    pager_older_->setObjectName(QStringLiteral("aida.ai.chat.pager_older"));
    pager_older_->setToolTip(QStringLiteral("Show an older window of messages"));
    pager_newer_ = new QPushButton(QStringLiteral("Newer"), pager_bar_);
    pager_newer_->setObjectName(QStringLiteral("aida.ai.chat.pager_newer"));
    pager_newer_->setToolTip(QStringLiteral("Show a newer window of messages"));
    pager_latest_ = new QPushButton(QStringLiteral("Latest"), pager_bar_);
    pager_latest_->setObjectName(QStringLiteral("aida.ai.chat.pager_latest"));
    pager_latest_->setToolTip(QStringLiteral("Jump to the newest messages"));
    pager_label_ = new QLabel(pager_bar_);
    pager_label_->setObjectName(QStringLiteral("aida.ai.chat.pager_label"));
    pager_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    pager_layout->addWidget(pager_older_);
    pager_layout->addWidget(pager_newer_);
    pager_layout->addWidget(pager_latest_);
    pager_layout->addWidget(pager_label_, 1);
    pager_bar_->setVisible(false);
    chat_layout->addWidget(pager_bar_);
    connect(pager_older_, &QPushButton::clicked, controller_,
            &AidaChatController::pageOlder);
    connect(pager_newer_, &QPushButton::clicked, controller_,
            &AidaChatController::pageNewer);
    connect(pager_latest_, &QPushButton::clicked, controller_,
            &AidaChatController::pageLatest);

    auto* list_host = new QWidget(chat_column);
    list_stack_ = new QStackedLayout(list_host);
    list_ = new QListView(list_host);
    list_->setObjectName(QStringLiteral("aida.ai.chat.list"));
    delegate_ = new AidaChatMessageDelegate(controller_->model(), controller_->cache(),
                                            list_);
    list_->setModel(controller_->model());
    list_->setItemDelegate(delegate_);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setSpacing(t.spacing.sm);
    list_->setResizeMode(QListView::Adjust);
    list_->setUniformItemSizes(false);
    list_->setMouseTracking(true);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setAccessibleName(QStringLiteral("Conversation messages"));
    empty_state_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("Start an analysis conversation"),
        QStringLiteral("Ask about the binary, debugger state, memory findings, network "
                       "evidence, code, or automation tasks."),
        list_host);
    empty_state_->setObjectName(QStringLiteral("aida.ai.chat.empty"));
    list_stack_->addWidget(list_);
    list_stack_->addWidget(empty_state_);
    chat_layout->addWidget(list_host, 1);

    auto* composer_row = new QHBoxLayout();
    composer_row->setSpacing(t.spacing.sm);
    composer_ = new AidaChatComposer(chat_column);
    send_button_ = new QPushButton(QStringLiteral("Send"), chat_column);
    send_button_->setObjectName(QStringLiteral("aida.ai.chat.send"));
    send_button_->setToolTip(QStringLiteral(
        "Send the message (Enter). While the AI runs this becomes Cancel."));
    composer_row->addWidget(composer_, 1);
    composer_row->addWidget(send_button_);
    chat_layout->addLayout(composer_row);
    status_hint_ = new QLabel(QStringLiteral("Enter sends  |  Ctrl+Enter inserts a line break"),
                              chat_column);
    status_hint_->setObjectName(QStringLiteral("aida.ai.chat.status_hint"));
    status_hint_->setProperty("aidaVariant", QStringLiteral("secondary"));
    chat_layout->addWidget(status_hint_);

    splitter_->addWidget(history_);
    splitter_->addWidget(chat_column);
    splitter_->setStretchFactor(1, 1);
    root->addWidget(splitter_, 1);

    connect(composer_, &AidaChatComposer::submitRequested, this, &AidaChatView::onSubmit);
    connect(send_button_, &QPushButton::clicked, this, [this] {
        if (is_ai_busy())
            controller_->cancelActive();
        else
            onSubmit();
    });
    connect(history_, &AidaConversationListView::deleteReviewRequested, this,
            &AidaChatView::onDeleteConversationReview);
    connect(history_, &AidaConversationListView::feedbackMessage, this,
            [this](const QString& message, bool is_error) {
        if (is_error)
            chrome::toast_error(message, 5.0);
        else
            chrome::toast_info(message, 3.0);
    });
    connect(delegate_, &AidaChatMessageDelegate::messageActionRequested, this,
            &AidaChatView::onMessageAction);
    connect(delegate_, &AidaChatMessageDelegate::linkActivated, this,
            [](const QString& url) {
        const QUrl parsed(url);
        if (parsed.scheme() == QLatin1String("http") ||
            parsed.scheme() == QLatin1String("https"))
            QDesktopServices::openUrl(parsed);
    });
    connect(delegate_, &AidaChatMessageDelegate::contextMenuRequested, this,
            [this](const QModelIndex& index, const QPoint& global_pos) {
        openMessageMenu(controller_->model()->identityAt(index.row()), global_pos);
    });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const QModelIndex index = list_->indexAt(pos);
        if (index.isValid())
            openMessageMenu(controller_->model()->identityAt(index.row()),
                            list_->viewport()->mapToGlobal(pos));
    });

    chrome_timer_ = new QTimer(this);
    chrome_timer_->setInterval(500);
    connect(chrome_timer_, &QTimer::timeout, this, &AidaChatView::refreshChromeState);
}

void AidaChatView::wireController() {
    connect(controller_, &AidaChatController::scrollToBottomRequested, this, [this] {
        list_->scrollToBottom();
    });
    connect(controller_, &AidaChatController::sessionReset, this, [this] {
        refreshPager();
        list_->scrollToBottom();
    });
    connect(controller_->model(), &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, int first, int last) { onRowsInserted(first, last); });
    connect(controller_->model(), &QAbstractItemModel::modelReset, this,
            &AidaChatView::onModelReset);
    connect(controller_, &AidaChatController::stateChanged, this,
            &AidaChatView::refreshChromeState);
    connect(controller_, &AidaChatController::editMessageRequested, this,
            &AidaChatView::onEditRequested);
    connect(&AidaConversationBridge::instance(), &AidaConversationBridge::conversationReplaced,
            controller_, [controller = controller_] {
        controller->drain();
    });
    connect(&theme::AidaThemeController::instance(), &theme::AidaThemeController::themeChanged,
            this, [this] {
        controller_->cache()->clear();
        delegate_->invalidateHeights();
        onModelReset();
    });
    refreshChromeState();
    refreshPager();
    onModelReset();
}

void AidaChatView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    chrome_timer_->start();
    refreshChromeState();
}

void AidaChatView::hideEvent(QHideEvent* event) {
    chrome_timer_->stop();
    QWidget::hideEvent(event);
}

void AidaChatView::appendComposerText(const QString& text) {
    if (composer_)
        composer_->appendInjected(text);
}

void AidaChatView::clearComposerText() {
    if (composer_)
        composer_->clearComposer();
}

int AidaChatView::composerTextSize() const {
    return composer_ ? static_cast<int>(composer_->toPlainText().size()) : 0;
}

void AidaChatView::onSubmit() {
    const QString text = composer_->toPlainText();
    controller_->submitUserText(text);
    composer_->clearComposer();
    composer_->setFocus();
}

void AidaChatView::refreshChromeState() {
    const auto store = aida::conversation_store::status();
    const bool busy = is_ai_busy();
    send_button_->setText(busy ? QStringLiteral("Cancel") : QStringLiteral("Send"));
    const bool store_blocked = store.pending || store.failed;
    composer_->setEnabled(!store_blocked);
    if (store.pending)
        status_hint_->setText(QStringLiteral("Conversation transaction in progress"));
    else if (store.failed)
        status_hint_->setText(QStringLiteral("Conversation persistence requires retry"));
    else if (busy)
        status_hint_->setText(QStringLiteral("AI operation in progress"));
    else
        status_hint_->setText(QStringLiteral("Enter sends  |  Ctrl+Enter inserts a line break"));
    const bool store_pending = store.pending;
    QString persistence_text;
    if (store_pending)
        persistence_text = QString::fromStdString(store.stage);
    else if (!conversations::persistence_error.empty())
        persistence_text = QString::fromStdString(conversations::persistence_error);
    else if (store.failed)
        persistence_text = QString::fromStdString(store.error);
    persistence_label_->setText(persistence_text);
    persistence_label_->setToolTip(QString::fromStdString(
        store.failed ? store.error : store.stage));
    persistence_label_->setVisible(!persistence_text.isEmpty());
    persistence_retry_->setVisible(store.failed && store.retryable);
    sign_in_button_->setVisible(model_pill_->hasSelection() && !model_pill_->authed());
    model_pill_->refresh();
    agent_pill_->refresh();
    mcp_pill_->refresh();
    history_->setBusy(busy);
}

void AidaChatView::refreshPager() {
    const auto window = controller_->model()->window();
    pager_bar_->setVisible(window.bounded);
    if (!window.bounded)
        return;
    pager_older_->setEnabled(window.first > 0);
    pager_newer_->setEnabled(window.last < window.total);
    pager_label_->setText(QStringLiteral("%1-%2 of %3")
        .arg(window.first + 1)
        .arg(window.last)
        .arg(window.total));
}

void AidaChatView::onModelReset() {
    refreshPager();
    const bool empty = controller_->model()->rowCount() == 0;
    list_stack_->setCurrentWidget(empty ? static_cast<QWidget*>(empty_state_)
                                        : static_cast<QWidget*>(list_));
}

void AidaChatView::onRowsInserted(int first, int last) {
    Q_UNUSED(first);
    Q_UNUSED(last);
    refreshPager();
    onModelReset();
}

void AidaChatView::onMessageAction(const message_identity_t& identity,
                                   message_action_t action) {
    if (action == message_action_t::delete_message) {
        onDeleteReview(identity);
        return;
    }
    if (action == message_action_t::apply_change) {
        onApplyReview(identity);
        return;
    }
    auto result = aida::automation_ui::execute_message_action(identity, action);
    if (result.succeeded && action == message_action_t::create_evidence_handoff) {
        std::string handoff_reason;
        if (!aida::automation_ui::queue_evidence_for_chat(result.evidence.evidence_id,
                                                          handoff_reason)) {
            result.succeeded = false;
            result.detail = std::move(handoff_reason);
        }
    }
    if (result.succeeded && !result.target_view_id.empty())
        open_ai_view(result.target_view_id);
    if (!result.detail.empty()) {
        if (result.succeeded)
            chrome::toast_info(QString::fromStdString(result.detail), 3.0);
        else
            chrome::toast_warning(QString::fromStdString(result.detail), 4.0);
    }
    refreshChromeState();
}

void AidaChatView::openMessageMenu(const message_identity_t& identity,
                                   const QPoint& global_pos) {
    if (identity.fingerprint == 0)
        return;
    aida::automation_ui::message_selection_t selection;
    std::string reason;
    if (!aida::automation_ui::message_selection(identity, selection, reason)) {
        chrome::toast_warning(QString::fromStdString(reason), 4.0);
        return;
    }

    auto* menu = new QMenu(this);
    struct item_t {
        const char* label;
        message_action_t action;
    };
    static const item_t k_items[] = {
        { "Copy message text", message_action_t::copy_text },
        { "Copy reasoning", message_action_t::copy_reasoning },
        { "Copy tool name", message_action_t::copy_tool_name },
        { "Edit message", message_action_t::edit_message },
        { "Retry from here", message_action_t::retry_from_here },
        { "Delete message...", message_action_t::delete_message },
        { "Send to chat input", message_action_t::send_to_chat_input },
        { "Create evidence handoff", message_action_t::create_evidence_handoff },
        { "Inspect tool activity", message_action_t::inspect_tool_activity },
        { "Review change", message_action_t::review_change },
        { "Apply reviewed change...", message_action_t::apply_change },
        { "Reject change", message_action_t::reject_change },
        { "Cancel active operation", message_action_t::cancel_active_operation },
    };
    for (const auto& item : k_items) {
        const auto capability = aida::automation_ui::message_action_capability(
            identity, item.action);
        if (!capability.visible)
            continue;
        auto* action = menu->addAction(QString::fromLatin1(item.label));
        action->setEnabled(capability.enabled);
        if (!capability.enabled && !capability.disabled_reason.empty())
            action->setToolTip(QString::fromStdString(capability.disabled_reason));
        connect(action, &QAction::triggered, this, [this, identity, action = item.action] {
            aida::automation_ui::message_selection_t check;
            std::string stale_reason;
            if (!aida::automation_ui::message_selection(identity, check, stale_reason)) {
                chrome::toast_warning(QString::fromStdString(stale_reason), 4.0);
                return;
            }
            onMessageAction(identity, action);
        });
    }
    menu->popup(global_pos);
}

void AidaChatView::onEditRequested() {
    message_identity_t identity;
    std::string text;
    if (!aida::automation_ui::consume_pending_message_edit(identity, text))
        return;
    const int row = controller_->model()->rowForAbsoluteIndex(identity.index);
    if (row < 0)
        return;
    list_->setCurrentIndex(controller_->model()->index(row));
    list_->edit(controller_->model()->index(row));
}

void AidaChatView::onDeleteReview(const message_identity_t& identity) {
    std::string reason;
    const auto capability = aida::automation_ui::message_action_capability(
        identity, message_action_t::delete_message);
    aida::automation_ui::message_selection_t selection;
    const bool present = aida::automation_ui::message_selection(identity, selection, reason);
    aida_confirm_request_t request;
    request.verb = QStringLiteral("Delete");
    request.target = !present ? QStringLiteral("the selected message")
        : selection.is_user ? QStringLiteral("the selected user message")
        : selection.is_tool_result ? QStringLiteral("the selected tool result")
        : QStringLiteral("the selected assistant response");
    request.scope = QStringLiteral("Only this message in the current conversation");
    request.effect = QStringLiteral(
        "Removes the message from the visible history and the persisted conversation.");
    request.reversibility = QStringLiteral(
        "This deletion cannot be undone after confirmation.");
    request.prerequisite = capability.enabled ? QString()
        : QString::fromStdString(capability.disabled_reason.empty()
            ? reason : capability.disabled_reason);
    request.confirm_label = QStringLiteral("Delete Message");
    request.destructive = true;
    request.confirm_enabled = present && capability.enabled;
    AidaConfirmDialog::request(request, this, [this, identity] {
        const auto result = aida::automation_ui::delete_message(identity);
        if (!result.succeeded)
            chrome::toast_error(QString::fromStdString(result.detail), 5.0);
        controller_->drain();
    });
}

void AidaChatView::onApplyReview(const message_identity_t& identity) {
    AidaApplyChangeDialog::request(identity, this, [this](const QString& detail) {
        if (!detail.isEmpty())
            chrome::toast_info(detail, 4.0);
        controller_->drain();
    });
}

void AidaChatView::onDeleteConversationReview(const QString& id, quint64 revision) {
    const auto snapshot = conversations::catalog_snapshot();
    const auto found = std::find_if(snapshot->begin(), snapshot->end(),
        [&](const ConversationSummary& conversation) {
            return conversation.id == id.toStdString();
        });
    const bool available = found != snapshot->end();
    const bool reviewed_current = available && found->revision == revision;
    const auto store = aida::conversation_store::status();
    const bool store_blocked = store.pending || store.failed;
    const bool deleting_current = available &&
        found->id == conversations::current_id;
    const bool busy = is_ai_busy();
    const bool can_delete = reviewed_current && !store_blocked && !(deleting_current && busy);

    aida_confirm_request_t request;
    request.verb = QStringLiteral("Delete");
    request.target = available && !found->title.empty()
        ? QString::fromStdString(found->title)
        : QStringLiteral("the selected conversation");
    request.scope = available
        ? QStringLiteral("%1 messages and their stored evidence metadata")
              .arg(found->msg_count)
        : QStringLiteral("The selected conversation and its stored evidence metadata");
    request.effect = QStringLiteral(
        "Deletes the persisted conversation and its associated evidence metadata.");
    request.reversibility = QStringLiteral(
        "This conversation cannot be recovered after confirmation.");
    request.prerequisite = !available
        ? QStringLiteral("The conversation no longer exists; refresh History.")
        : !reviewed_current
            ? QStringLiteral("The conversation changed after review began; close and review the current revision.")
        : store_blocked
            ? QStringLiteral("Wait for the active conversation transaction before deleting another conversation.")
        : deleting_current && busy
            ? QStringLiteral("Cancel or wait for the active AI operation before deleting its conversation.")
            : QString();
    request.confirm_label = QStringLiteral("Delete Conversation");
    request.destructive = true;
    request.confirm_enabled = can_delete;
    AidaConfirmDialog::request(request, this, [id, revision] {
        conversations::delete_conversation(id.toStdString(), revision);
    });
}

void install_ai_domain(aida::qt::docking::AidaDockHost* host,
                       aida::qt::bridge::MenuBridge* menus,
                       aida::qt::bridge::ActionBridge* actions) {
    if (!host)
        return;
    auto& domain = ai_domain();
    domain.host = host;
    domain.menus = menus;
    domain.actions = actions;

    auto* controller = &AidaChatController::instance();
    domain.controller = controller;
    (void)&AidaConversationBridge::instance();

    auto* bridge = &AidaChatInjectBridge::instance();
    QObject::connect(bridge, &AidaChatInjectBridge::appendToComposer,
        controller, [controller](const QString& text) { controller->onInjectAppend(text); });
    QObject::connect(bridge, &AidaChatInjectBridge::clearComposerRequested,
        controller, [controller] { controller->onInjectClear(); });
    analysis::QtAnalysisBridge::instance().setChatAcceptor(
        [controller](const QString& text) { return controller->injectAccepts(text); });
    QObject::connect(&analysis::QtAnalysisBridge::instance(),
        &analysis::QtAnalysisBridge::chatInjectRequested,
        bridge, [](const QString& text) {
            AidaChatInjectBridge::instance().post(text);
        });

    auto* approval = new AidaToolApprovalController(controller);
    approval->installBackendHook(nullptr);

    const auto install = [host](const char* id, registry::qt_view_factory_t factory) {
        const auto result = host->install_view_factory(registry::stable_view_id_t(id),
            std::move(factory));
        if (!result.ok())
            diag::log_tagged_fmt("qt_ai",
                "view_factory_install_failed view=%s status=%d detail=%s",
                id, static_cast<int>(result.status), result.detail.c_str());
    };

    install("view.ai_chat", [](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
        return new AidaChatView(parent);
    });
    install("view.ai.evidence", [](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
        return new AidaEvidenceView(parent);
    });
    install("view.ai.agents", [](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
        return new AidaAgentManagerView(parent);
    });
    install("view.ai.skills", [](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
        return new AidaSkillManagerView(parent);
    });
    install("view.ai.providers", [](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
        return new AidaProviderView(parent);
    });

    QObject::connect(controller, &AidaChatController::toggleAgentPickerRequested,
        controller, [] {
        AidaAgentPickerDialog::toggleInteractive(nullptr);
    });
    aida::ui::application_ui::set_command_palette_toggle_hook([] {
        AidaCommandPaletteDialog::toggleInteractive(nullptr);
    });

    aida::automation_ui::add_ui_shutdown_hook([] {
        diag::log_tagged("qt_ai", "ai_domain_shutdown");
        AidaProviderView::shutdownWorkers();
    });
    diag::log_tagged("qt_ai", "ai_domain_installed");
}

}
