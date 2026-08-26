#pragma once

#include <QHash>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QSplitter;
class QTabWidget;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class QtHumanRequestEditor;

// RepeaterEntryWidget is defined in the .cpp (no Q_OBJECT needed: it declares
// no signals/slots of its own and uses functor connects; only the entry
// widget layout and the request/response split live there).
class RepeaterEntryWidget;

// RepeaterEntryController owns one repeater_entry_t shared_ptr and the Send
// operation. The retained-revision contract (plan 11 section 6): the send
// click snapshots host/port/tls/request bytes plus the current
// request_revision on the GUI thread; the worker runs
// mitm_proxy::repeat_request verbatim (timing + net_audit logs); the
// completion is delivered to the pane through a queued invokeMethod carrying
// (entry id, sent revision, result) and is applied only when the entry still
// exists and the revision still matches; in_progress is cleared regardless.
class RepeaterEntryController : public QObject {
    Q_OBJECT
public:
    RepeaterEntryController(std::shared_ptr<network_view::repeater_entry_t> entry,
                            QObject* parent = nullptr);

    std::uint64_t id() const noexcept { return entry_->id; }
    network_view::repeater_entry_t* entry() const noexcept { return entry_.get(); }
    std::shared_ptr<network_view::repeater_entry_t> entryPtr() const { return entry_; }
    bool inProgress() const noexcept { return entry_->in_progress.load(std::memory_order_acquire); }

    void send(const QString& requestText);

Q_SIGNALS:
    void sendCompleted(std::uint64_t entryId);

private:
    std::shared_ptr<network_view::repeater_entry_t> entry_;
};

class RepeaterPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit RepeaterPane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void syncTabs();
    void newEntry();
    void closeEntry(int tabIndex);
    RepeaterEntryWidget* findEntryWidget(std::uint64_t entryId) const;
    void onEntrySendCompleted(std::uint64_t entryId);

    QLineEdit* host_edit_ = nullptr;
    QSpinBox* port_spin_ = nullptr;
    QCheckBox* tls_check_ = nullptr;
    widgets::AidaButton* new_button_ = nullptr;
    widgets::AidaStateView* empty_view_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTimer* sync_timer_ = nullptr;
    QHash<std::uint64_t, RepeaterEntryController*> controllers_;
    QHash<std::uint64_t, network_view::artifact_kind_t> selected_kinds_;
    bool syncing_ = false;
};

}
