#include "qt/bridge/shortcut_bridge.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <chrono>

#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/action_bridge.hpp"
#include "qt/bridge/interaction_context_provider.hpp"

namespace aida::qt::bridge {

namespace {

std::optional<legacy_chord_t> base_key_from_qt(int key, bool keypad) noexcept {
    using namespace legacy_chord;
    if (keypad) {
        if (key >= Qt::Key_0 && key <= Qt::Key_9)
            return static_cast<legacy_chord_t>(612 + (key - Qt::Key_0));
        switch (key) {
        case Qt::Key_Period: return 622;
        case Qt::Key_Slash: return 623;
        case Qt::Key_Asterisk: return 624;
        case Qt::Key_Minus: return 625;
        case Qt::Key_Plus: return 626;
        case Qt::Key_Enter: return 627;
        case Qt::Key_Equal: return 628;
        default: return std::nullopt;
        }
    }
    switch (key) {
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return 512;
    case Qt::Key_Left: return 513;
    case Qt::Key_Right: return 514;
    case Qt::Key_Up: return 515;
    case Qt::Key_Down: return 516;
    case Qt::Key_PageUp: return 517;
    case Qt::Key_PageDown: return 518;
    case Qt::Key_Home: return 519;
    case Qt::Key_End: return 520;
    case Qt::Key_Insert: return 521;
    case Qt::Key_Delete: return 522;
    case Qt::Key_Backspace: return 523;
    case Qt::Key_Space: return 524;
    case Qt::Key_Return: return 525;
    case Qt::Key_Enter: return 627;
    case Qt::Key_Escape: return 526;
    case Qt::Key_Menu: return 535;
    case Qt::Key_CapsLock: return 607;
    case Qt::Key_ScrollLock: return 608;
    case Qt::Key_NumLock: return 609;
    case Qt::Key_Print: return 610;
    case Qt::Key_Pause: return 611;
    case Qt::Key_Back: return 629;
    case Qt::Key_Forward: return 630;
    case Qt::Key_Apostrophe: return 596;
    case Qt::Key_Comma: return 597;
    case Qt::Key_Minus: return 598;
    case Qt::Key_Period: return 599;
    case Qt::Key_Slash: return 600;
    case Qt::Key_Semicolon: return 601;
    case Qt::Key_Equal: return 602;
    case Qt::Key_BracketLeft: return 603;
    case Qt::Key_Backslash: return 604;
    case Qt::Key_BracketRight: return 605;
    case Qt::Key_QuoteLeft: return 606;
    case Qt::Key_Asterisk: return 624;
    case Qt::Key_Plus: return 626;
    default: break;
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<legacy_chord_t>(536 + (key - Qt::Key_0));
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<legacy_chord_t>(546 + (key - Qt::Key_A));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return static_cast<legacy_chord_t>(572 + (key - Qt::Key_F1));
    return std::nullopt;
}

legacy_chord_t modifiers_from_qt(Qt::KeyboardModifiers modifiers) noexcept {
    legacy_chord_t result = 0;
    if (modifiers & Qt::ControlModifier)
        result |= legacy_chord::k_mod_ctrl;
    if (modifiers & Qt::ShiftModifier)
        result |= legacy_chord::k_mod_shift;
    if (modifiers & Qt::AltModifier)
        result |= legacy_chord::k_mod_alt;
    if (modifiers & Qt::MetaModifier)
        result |= legacy_chord::k_mod_super;
    return result;
}

}

ShortcutBridge::ShortcutBridge(InteractionContextProvider* context,
                               ActionBridge* actions, QObject* parent)
    : QObject(parent), context_(context), actions_(actions) {
    chord_timer_ = new QTimer(this);
    chord_timer_->setSingleShot(true);
    connect(chord_timer_, &QTimer::timeout, this, [this] {
        auto& resolver = aida::ui::application_ui::shortcut_resolver();
        auto& registry = aida::ui::application_ui::action_registry();
        const auto context = context_->current();
        handle_resolution(resolver.poll(now_ms(), context, registry));
    });
}

void ShortcutBridge::install() {
    if (installed_)
        return;
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) {
        diag::log_tagged_critical("qt_shortcuts",
            "shortcut_bridge install before QApplication");
        return;
    }
    app->installEventFilter(this);
    installed_ = true;
    diag::log_tagged("qt_shortcuts", "shortcut_bridge_installed");
}

std::uint64_t ShortcutBridge::now_ms() const {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::optional<legacy_chord_t> ShortcutBridge::chord_from_key_event(
    const QKeyEvent* event) {
    if (!event)
        return std::nullopt;
    return chord_from_combination(event->keyCombination());
}

std::optional<legacy_chord_t> ShortcutBridge::chord_from_combination(
    QKeyCombination combination) {
    const int key = combination.key();
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_unknown:
        return std::nullopt;
    default:
        break;
    }
    const bool keypad = (modifiers & Qt::KeypadModifier) != 0;
    const auto base = base_key_from_qt(key, keypad);
    if (!base)
        return std::nullopt;
    return static_cast<legacy_chord_t>(*base | modifiers_from_qt(modifiers));
}

bool ShortcutBridge::binding_applies(
    const aida::ui::shortcut_binding_t& binding,
    const aida::ui::interaction_context_t& context) const {
    using namespace aida::ui;
    if (!binding.enabled)
        return false;
    if (context.modal_active) {
        if (binding.modal_policy == shortcut_modal_policy_t::suppress_when_modal)
            return false;
    } else if (binding.modal_policy == shortcut_modal_policy_t::modal_only) {
        return false;
    }
    if (context.text_input_active) {
        if (binding.text_input_policy == shortcut_text_input_policy_t::suppress)
            return false;
    } else if (binding.text_input_policy == shortcut_text_input_policy_t::text_input_only) {
        return false;
    }
    if (binding.scope_kind == focus_scope_kind_t::global)
        return true;
    const auto* scope = context.find_scope(binding.scope);
    return scope && scope->kind == binding.scope_kind;
}

bool ShortcutBridge::gates_blocked() const {
    if (capture_active_)
        return true;
    if (palette_open_)
        return true;
    if (palette_gate_hook_ && palette_gate_hook_())
        return true;
    return QApplication::activePopupWidget() != nullptr;
}

bool ShortcutBridge::probe_consumes(legacy_chord_t chord, bool repeated) {
    auto& resolver = aida::ui::application_ui::shortcut_resolver();
    auto& registry = aida::ui::application_ui::action_registry();
    aida::ui::shortcut_resolver_t probe = resolver;
    const auto context = context_->current();
    const auto resolution = probe.feed(chord, repeated, now_ms(), context, registry);
    return resolution.status != aida::ui::shortcut_resolution_status_t::none;
}

void ShortcutBridge::rearm_chord_timer() {
    using namespace aida::ui;
    auto& resolver = application_ui::shortcut_resolver();
    const auto context = context_->current();
    const std::size_t matched = pending_strokes_.size();
    std::uint32_t timeout = 0;
    bool any = false;

    const auto collect = [&](std::size_t prefix_length) {
        timeout = 5000;
        any = false;
        resolver.for_each([&](const shortcut_binding_t& binding) {
            if (binding.sequence.strokes.size() < prefix_length ||
                !binding_applies(binding, context))
                return;
            for (std::size_t index = 0; index < prefix_length; ++index) {
                if (binding.sequence.strokes[index] != pending_strokes_[index])
                    return;
            }
            any = true;
            timeout = (std::min)(timeout, binding.chord_timeout_ms);
        });
    };

    collect(matched);
    if (!any && matched > 0) {
        pending_strokes_ = {pending_strokes_.back()};
        collect(1);
    }
    if (!any) {
        resolver.cancel_pending();
        pending_strokes_.clear();
        Q_EMIT chord_pending_changed(QString{});
        return;
    }
    const std::uint64_t now = now_ms();
    const std::uint64_t deadline = last_feed_ms_ + timeout;
    const std::uint64_t remaining = deadline > now ? deadline - now + 1 : 1;
    chord_timer_->start(static_cast<int>(
        (std::min)(remaining, static_cast<std::uint64_t>(5000))));
}

void ShortcutBridge::cancel_pending() {
    aida::ui::application_ui::shortcut_resolver().cancel_pending();
    chord_timer_->stop();
    if (!pending_strokes_.empty()) {
        pending_strokes_.clear();
        Q_EMIT chord_pending_changed(QString{});
    }
}

void ShortcutBridge::handle_resolution(
    const aida::ui::shortcut_resolution_t& resolution) {
    using namespace aida::ui;
    if (resolution.status == shortcut_resolution_status_t::pending_chord) {
        const QString pending_text = QString::fromStdString(
            application_ui::format_shortcut_sequence(pending_strokes_));
        Q_EMIT chord_pending_changed(pending_text);
        rearm_chord_timer();
        return;
    }
    chord_timer_->stop();
    if (!pending_strokes_.empty()) {
        pending_strokes_.clear();
        Q_EMIT chord_pending_changed(QString{});
    }
    if (resolution.resolved()) {
        actions_->dispatch(QString::fromStdString(resolution.action.value()),
                           action_invocation_source_t::shortcut,
                           context_->current());
    }
}

bool ShortcutBridge::eventFilter(QObject* watched, QEvent* event) {
    Q_UNUSED(watched);
    const auto type = event->type();
    if (type == QEvent::ShortcutOverride) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        const auto chord = chord_from_key_event(key_event);
        if (!chord)
            return false;
        if (gates_blocked()) {
            cancel_pending();
            return false;
        }
        if (probe_consumes(*chord, key_event->isAutoRepeat())) {
            pending_consume_.key = key_event->key();
            pending_consume_.modifiers = key_event->modifiers();
            pending_consume_.window = QGuiApplication::focusWindow();
            pending_consume_.armed = true;
            key_event->accept();
            return true;
        }
        if (!pending_strokes_.empty()) {
            auto& resolver = aida::ui::application_ui::shortcut_resolver();
            auto& registry = aida::ui::application_ui::action_registry();
            const auto context = context_->current();
            const auto resolution = resolver.feed(*chord, key_event->isAutoRepeat(),
                                                  now_ms(), context, registry);
            if (resolution.status != aida::ui::shortcut_resolution_status_t::none) {
                if (resolution.status == aida::ui::shortcut_resolution_status_t::pending_chord)
                    pending_strokes_.push_back(*chord);
                handle_resolution(resolution);
                return true;
            }
            cancel_pending();
        }
        return false;
    }
    if (type == QEvent::KeyPress) {
        if (!pending_consume_.armed)
            return false;
        auto* key_event = static_cast<QKeyEvent*>(event);
        if (key_event->key() != pending_consume_.key ||
            key_event->modifiers() != pending_consume_.modifiers ||
            QGuiApplication::focusWindow() != pending_consume_.window)
            return false;
        pending_consume_ = pending_consume_t{};
        const auto chord = chord_from_key_event(key_event);
        if (chord) {
            auto& resolver = aida::ui::application_ui::shortcut_resolver();
            auto& registry = aida::ui::application_ui::action_registry();
            const auto context = context_->current();
            last_feed_ms_ = now_ms();
            const auto resolution = resolver.feed(*chord, key_event->isAutoRepeat(),
                                                  last_feed_ms_, context, registry);
            if (resolution.status == aida::ui::shortcut_resolution_status_t::pending_chord)
                pending_strokes_.push_back(*chord);
            handle_resolution(resolution);
        }
        return true;
    }
    return false;
}

void ShortcutBridge::set_capture_active(bool active) {
    capture_active_ = active;
    aida::ui::application_ui::set_shortcut_capture_active(active);
    if (active)
        cancel_pending();
}

void ShortcutBridge::set_palette_open(bool open) {
    palette_open_ = open;
    if (open)
        cancel_pending();
}

void ShortcutBridge::set_palette_gate_hook(std::function<bool()> hook) {
    palette_gate_hook_ = std::move(hook);
}

ShortcutRecorderWidget::ShortcutRecorderWidget(ShortcutBridge* bridge,
                                               QWidget* parent)
    : QWidget(parent), bridge_(bridge) {
    setObjectName(QStringLiteral("aida.shortcut.recorder"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    edit_ = new QKeySequenceEdit(this);
    edit_->setObjectName(QStringLiteral("aida.shortcut.recorder.edit"));
    edit_->setMaximumSequenceLength(4);
    edit_->setFinishingKeyCombinations({});
    edit_->setToolTip(QStringLiteral(
        "Press the new shortcut. Recording finishes shortly after the last key is "
        "released. Multi-stroke chords up to four keys are supported."));
    layout->addWidget(edit_);

    edit_->installEventFilter(this);
    connect(edit_, &QKeySequenceEdit::editingFinished, this, [this] {
        chords_.clear();
        valid_ = false;
        const QKeySequence sequence = edit_->keySequence();
        const int count = sequence.count();
        bool converted = count > 0;
        for (int index = 0; index < count; ++index) {
            const auto chord =
                ShortcutBridge::chord_from_combination(sequence[static_cast<uint>(index)]);
            if (!chord) {
                converted = false;
                break;
            }
            chords_.push_back(*chord);
        }
        valid_ = converted;
        Q_EMIT capture_finished();
    });
}

void ShortcutRecorderWidget::clear() {
    edit_->clear();
    chords_.clear();
    valid_ = false;
}

std::vector<legacy_chord_t> ShortcutRecorderWidget::chords() const {
    return chords_;
}

bool ShortcutRecorderWidget::has_valid_sequence() const {
    return valid_ && !chords_.empty();
}

void ShortcutRecorderWidget::focusOutEvent(QFocusEvent* event) {
    end_capture();
    QWidget::focusOutEvent(event);
}

void ShortcutRecorderWidget::hideEvent(QHideEvent* event) {
    end_capture();
    QWidget::hideEvent(event);
}

bool ShortcutRecorderWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == edit_) {
        if (event->type() == QEvent::FocusIn) {
            if (!capturing_) {
                capturing_ = true;
                if (bridge_)
                    bridge_->set_capture_active(true);
                Q_EMIT capture_started();
            }
        } else if (event->type() == QEvent::FocusOut) {
            end_capture();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ShortcutRecorderWidget::end_capture() {
    if (!capturing_)
        return;
    capturing_ = false;
    if (bridge_)
        bridge_->set_capture_active(false);
}

}
