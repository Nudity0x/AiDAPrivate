#include "qt/ai/qt_chat_inject.hpp"

#include <QMetaObject>

#include "core/ai/standalone_chat.hpp"

namespace aida::qt::ai {

AidaChatInjectBridge::AidaChatInjectBridge(QObject* parent) : QObject(parent) {}

AidaChatInjectBridge& AidaChatInjectBridge::instance() {
    static AidaChatInjectBridge* bridge = [] {
        auto* created = new AidaChatInjectBridge();
        created->installBackendHooks();
        return created;
    }();
    return *bridge;
}

void AidaChatInjectBridge::installBackendHooks() {
    aida::automation_ui::set_chat_inject_notify_hook([this] {
        scheduleDrain();
    });
}

void AidaChatInjectBridge::post(const std::string& text) {
    aida::automation_ui::post_chat_inject(text);
}

void AidaChatInjectBridge::post(const QString& text) {
    aida::automation_ui::post_chat_inject(text.toStdString());
}

void AidaChatInjectBridge::requestClearComposer() {
    aida::automation_ui::request_chat_composer_clear();
}

void AidaChatInjectBridge::scheduleDrain() {
    if (drain_pending_.exchange(true, std::memory_order_acq_rel))
        return;
    QMetaObject::invokeMethod(this, [this] {
        drain_pending_.store(false, std::memory_order_release);
        drainNow();
    }, Qt::QueuedConnection);
}

void AidaChatInjectBridge::drainNow() {
    const std::uint64_t clear_seq =
        aida::automation_ui::chat_composer_clear_sequence();
    if (clear_seq != observed_clear_seq_) {
        observed_clear_seq_ = clear_seq;
        Q_EMIT clearComposerRequested();
    }
    auto texts = aida::automation_ui::drain_chat_inject();
    for (const auto& text : texts) {
        if (!text.empty())
            Q_EMIT appendToComposer(QString::fromStdString(text));
    }
    if (!texts.empty()) {
        auto remaining = aida::automation_ui::drain_chat_inject();
        if (!remaining.empty())
            scheduleDrain();
    }
}

}
