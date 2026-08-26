#pragma once

#include "core/ui/application_action_registry.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "qt/bridge/aida_dialog.hpp"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>

class QAction;
class QLabel;
class QMenu;
class QPushButton;
class QTimer;

namespace aida::qt::bridge {

class InteractionContextProvider;
class ActionBridge;
class ConfirmationController;

class ActionConfirmationDialog : public AidaDialog {
    Q_OBJECT
public:
    explicit ActionConfirmationDialog(ConfirmationController* controller,
                                      QWidget* parent = nullptr);

private:
    void reevaluate();
    void apply_capability(const aida::ui::action_state_t& state,
                          bool descriptor_present);

    ConfirmationController* controller_ = nullptr;
    QTimer* reevaluate_timer_ = nullptr;
    QPushButton* confirm_button_ = nullptr;
    QLabel* description_label_ = nullptr;
    QLabel* consequence_label_ = nullptr;
    QLabel* unavailable_label_ = nullptr;
    bool finishing_ = false;
};

class ConfirmationController : public QObject {
    Q_OBJECT
public:
    explicit ConfirmationController(QObject* parent = nullptr);

    bool queue(const QString& action_id,
               aida::ui::action_invocation_source_t source,
               const aida::ui::action_execution_result_t& result,
               const aida::ui::interaction_context_t& context);
    void cancel();
    bool active() const noexcept { return state_.active; }

    void confirm_now();
    void capability_lost_now();

Q_SIGNALS:
    void diagnostic_raised(const QString& stable_id, const QString& target,
                           const QString& summary, const QString& details);
    void confirmation_opened(const QString& action_id);
    void confirmation_closed(const QString& action_id);

private:
    friend class ActionConfirmationDialog;

    struct state_t {
        bool active = false;
        std::string action;
        std::string label;
        std::string description;
        std::string consequence;
        aida::ui::action_invocation_source_t source =
            aida::ui::action_invocation_source_t::command_palette;
        aida::ui::interaction_context_t context;
        aida::ui::application_ui::retained_entity_runtime_context_t retained_context;
        bool retained_present = false;
    };

    void clear_state();
    void publish_unavailable_and_close(bool descriptor_present,
                                       const aida::ui::capability_state_t& capability);

    state_t state_;
    QPointer<ActionConfirmationDialog> dialog_;
};

class ActionBridge : public QObject {
    Q_OBJECT
public:
    ActionBridge(InteractionContextProvider* context, QObject* parent = nullptr);

    QAction* action(const QString& id);
    QAction* menu_action(const QString& id, const QString& label_override,
                         const QString& shortcut_hint_override, QMenu* menu);
    QAction* surface_action(const QString& id,
                            aida::ui::action_invocation_source_t source,
                            QObject* parent);
    void refresh(QAction* action) const;
    void ensure_current();
    void dispatch(const QString& id, aida::ui::action_invocation_source_t source);
    void dispatch(const QString& id, aida::ui::action_invocation_source_t source,
                  const aida::ui::interaction_context_t& context);
    QString shortcut_hint(const QString& id,
                          const aida::ui::interaction_context_t& context) const;
    ConfirmationController* confirmations() const noexcept { return confirmations_; }
    std::uint64_t registry_revision() const noexcept { return built_revision_; }

    void finalize(const QString& id,
                  const aida::ui::action_execution_result_t& result,
                  aida::ui::action_invocation_source_t source,
                  const aida::ui::interaction_context_t& context);

Q_SIGNALS:
    void actions_rebuilt();

private:
    QAction* hydrate(const aida::ui::application_action_descriptor_t& descriptor);
    void rebuild();

    InteractionContextProvider* context_ = nullptr;
    ConfirmationController* confirmations_ = nullptr;
    QHash<QString, QPointer<QAction>> actions_;
    QTimer* invalidation_timer_ = nullptr;
    std::uint64_t built_revision_ = 0;
    bool built_ = false;
};

}
