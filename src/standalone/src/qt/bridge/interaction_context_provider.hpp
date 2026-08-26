#pragma once

#include "core/ui/interaction_context.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

class QWidget;

namespace aida::qt::bridge {

class InteractionContextProvider : public QObject {
    Q_OBJECT
public:
    static constexpr const char* k_scope_id_property = "aida.scope.id";
    static constexpr const char* k_scope_kind_property = "aida.scope.kind";
    static constexpr const char* k_text_input_property = "aidaTextInput";

    explicit InteractionContextProvider(QObject* parent = nullptr);
    ~InteractionContextProvider() override;

    aida::ui::interaction_context_t base_context() const;
    aida::ui::interaction_context_t current() const;
    std::uint64_t generation() const;
    void bump_generation();

    bool text_input_active() const;
    bool modal_active() const;

    void set_active_view_hook(
        std::function<std::pair<std::string, std::string>()> hook);

    static void attach_scope(QWidget* widget, const QString& scope_id,
                             aida::ui::focus_scope_kind_t kind);
    static void mark_text_input(QWidget* widget, bool accepts_text = true);
    static const char* scope_kind_token(aida::ui::focus_scope_kind_t kind) noexcept;
    static std::optional<aida::ui::focus_scope_kind_t> scope_kind_from_token(
        const QString& token) noexcept;

Q_SIGNALS:
    void contextChanged();

private:
    void rebuild_focus_path(aida::ui::interaction_context_t& context) const;

    std::function<std::pair<std::string, std::string>()> active_view_hook_;
};

}
