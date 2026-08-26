#pragma once

#include "core/ui/shortcut_resolver.hpp"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <qnamespace.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

class QKeyEvent;
class QKeyCombination;
class QKeySequenceEdit;
class QTimer;
class QWindow;

namespace aida::qt::bridge {

using legacy_chord_t = typename std::decay_t<decltype(
    std::declval<aida::ui::shortcut_sequence_t>().strokes)>::value_type;

namespace legacy_chord {
inline constexpr legacy_chord_t k_mod_ctrl = aida::ui::chord::mod_ctrl;
inline constexpr legacy_chord_t k_mod_shift = aida::ui::chord::mod_shift;
inline constexpr legacy_chord_t k_mod_alt = aida::ui::chord::mod_alt;
inline constexpr legacy_chord_t k_mod_super = aida::ui::chord::mod_super;
inline constexpr legacy_chord_t k_mod_mask = aida::ui::chord::mod_mask;
inline constexpr legacy_chord_t k_key_tab = aida::ui::chord::k_tab;
inline constexpr legacy_chord_t k_key_gamepad_start = aida::ui::chord::k_gamepad_start;
}

class InteractionContextProvider;
class ActionBridge;

class ShortcutBridge : public QObject {
    Q_OBJECT
public:
    ShortcutBridge(InteractionContextProvider* context, ActionBridge* actions,
                   QObject* parent = nullptr);

    void install();
    bool eventFilter(QObject* watched, QEvent* event) override;

    void set_capture_active(bool active);
    bool capture_active() const noexcept { return capture_active_; }
    void set_palette_open(bool open);
    bool palette_open() const noexcept { return palette_open_; }
    void set_palette_gate_hook(std::function<bool()> hook);

    static std::optional<legacy_chord_t> chord_from_key_event(const QKeyEvent* event);
    static std::optional<legacy_chord_t> chord_from_combination(
        QKeyCombination combination);

Q_SIGNALS:
    void chord_pending_changed(const QString& pending_text);

private:
    struct pending_consume_t {
        int key = 0;
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
        QPointer<QWindow> window;
        bool armed = false;
    };

    bool gates_blocked() const;
    bool probe_consumes(legacy_chord_t chord, bool repeated);
    void handle_resolution(const aida::ui::shortcut_resolution_t& resolution);
    void rearm_chord_timer();
    void cancel_pending();
    std::uint64_t now_ms() const;
    bool binding_applies(const aida::ui::shortcut_binding_t& binding,
                         const aida::ui::interaction_context_t& context) const;

    InteractionContextProvider* context_ = nullptr;
    ActionBridge* actions_ = nullptr;
    QTimer* chord_timer_ = nullptr;
    pending_consume_t pending_consume_;
    std::vector<legacy_chord_t> pending_strokes_;
    std::uint64_t last_feed_ms_ = 0;
    bool capture_active_ = false;
    bool palette_open_ = false;
    std::function<bool()> palette_gate_hook_;
    bool installed_ = false;
};

class ShortcutRecorderWidget : public QWidget {
    Q_OBJECT
public:
    explicit ShortcutRecorderWidget(ShortcutBridge* bridge,
                                    QWidget* parent = nullptr);

    std::vector<legacy_chord_t> chords() const;
    bool has_valid_sequence() const;
    void clear();

Q_SIGNALS:
    void capture_started();
    void capture_finished();

protected:
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void end_capture();

    ShortcutBridge* bridge_ = nullptr;
    QKeySequenceEdit* edit_ = nullptr;
    std::vector<legacy_chord_t> chords_;
    bool valid_ = false;
    bool capturing_ = false;
};

}
