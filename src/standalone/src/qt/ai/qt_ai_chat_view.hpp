#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/ai/conversation_history.hpp"
#include "core/ai/standalone_chat.hpp"
#include "qt/ai/qt_ai_chat_delegate.hpp"
#include "qt/ai/qt_ai_chat_model.hpp"

class QHBoxLayout;
class QLabel;
class QListView;
class QPushButton;
class QSplitter;
class QStackedLayout;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace aida::qt::widgets { class AidaStateView; }
namespace aida::qt::docking { class AidaDockHost; }
namespace aida::qt::bridge { class MenuBridge; class ActionBridge; }

namespace aida::qt::ai {

class AidaChatComposer;
class AidaConversationListView;
class AidaModelPill;
class AidaAgentPill;
class AidaSkillsPill;
class AidaMcpPill;
class AidaChatView;

class AidaChatController : public QObject {
    Q_OBJECT
public:
    static AidaChatController& instance();

    void installBackendHooks();

    AidaChatMessageModel* model() const noexcept { return model_; }
    AidaChatDocumentCache* cache() const noexcept { return cache_; }

    void attachView(AidaChatView* view, AidaChatMessageDelegate* delegate);
    void detachView(AidaChatView* view);

    void submitUserText(const QString& text);
    void appendUserMessage(const std::string& text);
    void cancelActive();

    void onInjectAppend(const QString& text);
    void onInjectClear();
    bool injectAccepts(const QString& text) const;

    void pageOlder();
    void pageNewer();
    void pageLatest();

    void drain();

    bool thinkingActive() const noexcept { return thinking_active_; }

Q_SIGNALS:
    void scrollToBottomRequested();
    void thinkingActiveChanged(bool active);
    void sessionReset();
    void stateChanged();
    void editMessageRequested(const QString& text);
    void toggleAgentPickerRequested();
    void openSettingsForProviderRequested(const QString& provider_id);

private:
    explicit AidaChatController(QObject* parent = nullptr);

    void scheduleDrain();
    void updateModelAfterChange(const ai_chat_poll_result_t& result);
    void onSessionIdentityChanged();
    void updateThinking(bool active);

    AidaChatMessageModel* model_ = nullptr;
    AidaChatDocumentCache* cache_ = nullptr;
    AidaChatView* view_ = nullptr;
    AidaChatMessageDelegate* delegate_ = nullptr;
    QTimer* reveal_timer_ = nullptr;
    QTimer* mcp_poll_timer_ = nullptr;
    QTimer* catalog_fallback_timer_ = nullptr;
    std::atomic<bool> drain_queued_{false};
    std::uint64_t last_scroll_seq_ = 0;
    std::size_t previous_total_ = 0;
    std::string session_id_;
    bool thinking_active_ = false;
    QStringList pending_composer_appends_;
    bool pending_composer_clear_ = false;
};

class AidaChatView : public QWidget {
    Q_OBJECT
public:
    explicit AidaChatView(QWidget* parent = nullptr);
    ~AidaChatView() override;

    AidaChatController* controller() const noexcept { return controller_; }

    void appendComposerText(const QString& text);
    void clearComposerText();
    int composerTextSize() const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void wireController();
    void onSubmit();
    void refreshChromeState();
    void refreshPager();
    void onModelReset();
    void onRowsInserted(int first, int last);
    void onMessageAction(const aida::automation_ui::message_identity_t& identity,
                         aida::automation_ui::message_action_t action);
    void openMessageMenu(const aida::automation_ui::message_identity_t& identity,
                         const QPoint& global_pos);
    void onEditRequested();
    void onDeleteReview(const aida::automation_ui::message_identity_t& identity);
    void onApplyReview(const aida::automation_ui::message_identity_t& identity);
    void onDeleteConversationReview(const QString& id, quint64 revision);

    AidaChatController* controller_ = nullptr;
    AidaChatMessageDelegate* delegate_ = nullptr;
    QListView* list_ = nullptr;
    AidaChatComposer* composer_ = nullptr;
    QPushButton* send_button_ = nullptr;
    QLabel* status_hint_ = nullptr;
    QLabel* persistence_label_ = nullptr;
    QPushButton* persistence_retry_ = nullptr;
    QWidget* pager_bar_ = nullptr;
    QLabel* pager_label_ = nullptr;
    QPushButton* pager_older_ = nullptr;
    QPushButton* pager_newer_ = nullptr;
    QPushButton* pager_latest_ = nullptr;
    QSplitter* splitter_ = nullptr;
    AidaConversationListView* history_ = nullptr;
    QStackedLayout* list_stack_ = nullptr;
    widgets::AidaStateView* empty_state_ = nullptr;
    AidaModelPill* model_pill_ = nullptr;
    AidaAgentPill* agent_pill_ = nullptr;
    AidaSkillsPill* skills_pill_ = nullptr;
    AidaMcpPill* mcp_pill_ = nullptr;
    QToolButton* sign_in_button_ = nullptr;
    QTimer* chrome_timer_ = nullptr;
};

void install_ai_domain(aida::qt::docking::AidaDockHost* host,
                       aida::qt::bridge::MenuBridge* menus,
                       aida::qt::bridge::ActionBridge* actions);

}
