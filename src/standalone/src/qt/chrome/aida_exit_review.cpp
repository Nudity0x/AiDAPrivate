#include "qt/chrome/aida_exit_review.hpp"

#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "helpers/diag_log.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/qt_main_window.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::chrome {

AidaDirtyCloseDialog::AidaDirtyCloseDialog(QWidget* parent)
    : bridge::AidaDialog(parent)
{
    setObjectName(QStringLiteral("aida.editor.dirty_close"));
    setWindowTitle(QStringLiteral("Unsaved Changes"));
    setModal(true);

    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
                             t.panel.padding);
    root->setSpacing(t.spacing.sm);

    message_ = new QLabel(this);
    message_->setObjectName(QStringLiteral("aida.editor.dirty_close.message"));
    message_->setWordWrap(true);
    message_->setFont(theme::fonts::body());
    root->addWidget(message_);

    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("aida.editor.dirty_close.notice"));
    notice_->setProperty("aidaVariant", QStringLiteral("warning"));
    notice_->setWordWrap(true);
    notice_->setFont(theme::fonts::caption());
    root->addWidget(notice_);

    queue_header_ = new QLabel(QStringLiteral("Close queue"), this);
    queue_header_->setObjectName(QStringLiteral("aida.editor.dirty_close.queue_header"));
    queue_header_->setFont(theme::fonts::strong());
    queue_header_->setVisible(false);
    root->addWidget(queue_header_);

    queue_list_ = new QListWidget(this);
    queue_list_->setObjectName(QStringLiteral("aida.editor.dirty_close.queue"));
    queue_list_->setVisible(false);
    queue_list_->setMaximumHeight(t.row.standard * 5);
    root->addWidget(queue_list_);

    buttons_ = new QHBoxLayout();
    buttons_->setSpacing(t.spacing.sm);
    buttons_->addStretch(1);
    save_button_ = new QPushButton(QStringLiteral("Save"), this);
    save_button_->setObjectName(QStringLiteral("aida.editor.dirty_close.save"));
    save_button_->setAutoDefault(true);
    save_button_->setDefault(true);
    connect(save_button_, &QPushButton::clicked, this, [this] { Q_EMIT saveRequested(); });
    buttons_->addWidget(save_button_);
    discard_button_ = new QPushButton(QStringLiteral("Don't Save"), this);
    discard_button_->setObjectName(QStringLiteral("aida.editor.dirty_close.discard"));
    discard_button_->setAutoDefault(false);
    connect(discard_button_, &QPushButton::clicked, this, [this] { Q_EMIT discardRequested(); });
    buttons_->addWidget(discard_button_);
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_button_->setObjectName(QStringLiteral("aida.editor.dirty_close.cancel"));
    cancel_button_->setAutoDefault(false);
    connect(cancel_button_, &QPushButton::clicked, this, [this] {
        Q_EMIT cancelRequested();
        reject();
    });
    buttons_->addWidget(cancel_button_);
    root->addLayout(buttons_);

    const QFontMetricsF fm(theme::fonts::body());
    const int em = (std::max)(1, static_cast<int>(fm.horizontalAdvance(QLatin1Char('0'))));
    const int line = (std::max)(1, static_cast<int>(fm.lineSpacing()));
    setMinimumSize(em * 40, line * 15);
    resize(em * 80, line * 24);
}

void AidaDirtyCloseDialog::showFor(const exit_review_snapshot_t& snapshot)
{
    updateFromSnapshot(snapshot);
    if (!isVisible())
        open();
    raise();
}

void AidaDirtyCloseDialog::updateFromSnapshot(const exit_review_snapshot_t& snapshot)
{
    document_id_ = snapshot.current.document_id;
    const QString name = QString::fromStdString(snapshot.current.filename).isEmpty()
        ? QStringLiteral("the reviewed document")
        : QString::fromStdString(snapshot.current.filename);
    message_->setText(QStringLiteral("Save changes to '%1' before closing?").arg(name));

    QStringList notices;
    if (!snapshot.close_error.empty())
        notices << QStringLiteral("Save failed: %1")
            .arg(QString::fromStdString(snapshot.close_error));
    if (!snapshot.current.target_current)
        notices << QStringLiteral(
            "Document changed: The reviewed document is no longer current. Cancel and review the active close request.");
    else if (snapshot.current.save_disabled && !snapshot.current.save_gate_detail.empty())
        notices << QStringLiteral("Save unavailable: %1")
            .arg(QString::fromStdString(snapshot.current.save_gate_detail));
    notice_->setText(notices.join(QStringLiteral("\n")));
    notice_->setVisible(!notices.isEmpty());

    save_button_->setEnabled(!snapshot.current.save_disabled &&
        snapshot.current.target_current);
    discard_button_->setEnabled(!snapshot.current.close_disabled);

    const bool has_queue = !snapshot.queue_names.empty();
    queue_header_->setVisible(has_queue);
    queue_list_->setVisible(has_queue);
    if (has_queue) {
        queue_list_->clear();
        for (const auto& entry : snapshot.queue_names)
            queue_list_->addItem(QStringLiteral("• %1").arg(QString::fromStdString(entry)));
    }
    rebuildButtons();
}

void AidaDirtyCloseDialog::resizeEvent(QResizeEvent* event)
{
    bridge::AidaDialog::resizeEvent(event);
    rebuildButtons();
}

void AidaDirtyCloseDialog::rebuildButtons()
{
    if (!buttons_)
        return;
    const int content = save_button_->sizeHint().width() +
        discard_button_->sizeHint().width() + cancel_button_->sizeHint().width() +
        buttons_->spacing() * 3;
    const int chrome_width = theme::tokens().panel.padding * 2;
    buttons_->setDirection(width() < content + chrome_width
        ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}

AidaExitReviewController::AidaExitReviewController(AidaMainWindow* window, QObject* parent)
    : QObject(parent), window_(window)
{
    timer_ = new QTimer(this);
    timer_->setInterval(100);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, [this] { tick(); });
    timer_->start();
}

AidaExitReviewController::~AidaExitReviewController() = default;

bool AidaExitReviewController::gateHook()
{
    auto& hooks = legacy_chrome_hooks();
    if (!hooks.exit_gate.committed || !hooks.exit_gate.request)
        return true;
    if (hooks.exit_gate.committed())
        return true;
    const auto requested = hooks.exit_gate.request();
    if (!requested.first)
        diag::log_tagged_critical_fmt("chrome",
            "shutdown_review_rejected source=qt_gate reason=%.512s", requested.second.c_str());
    return false;
}

void AidaExitReviewController::onSessionAbort()
{
    auto& hooks = legacy_chrome_hooks();
    if (hooks.exit_gate.cancel)
        hooks.exit_gate.cancel();
    if (hooks.exit_review.cancel)
        hooks.exit_review.cancel();
    close_recommitted_.store(false, std::memory_order_release);
}

void AidaExitReviewController::tick()
{
    auto& hooks = legacy_chrome_hooks();
    if (!hooks.exit_review.poll || !hooks.exit_gate.consume_ready)
        return;

    const auto snapshot = hooks.exit_review.poll();
    last_snapshot_ = snapshot;

    if (dialog_ && !snapshot.dialog_active) {
        dialog_->reject();
        dialog_->deleteLater();
        dialog_ = nullptr;
    }
    if (snapshot.dialog_active && window_) {
        if (!dialog_) {
            dialog_ = new AidaDirtyCloseDialog(window_);
            connect(dialog_, &AidaDirtyCloseDialog::saveRequested, this, [this] {
                auto& h = legacy_chrome_hooks().exit_review;
                if (last_snapshot_.current.filepath_empty) {
                    const auto destination = dialogs::save_file(dialog_,
                        QStringLiteral("Save As"), "All files (*.*)\0*.*\0\0");
                    if (!destination) {
                        if (h.set_close_error)
                            h.set_close_error("Save As was canceled.");
                        return;
                    }
                    if (h.save_current_as)
                        h.save_current_as(*destination);
                    return;
                }
                if (h.save_current)
                    h.save_current();
            });
            connect(dialog_, &AidaDirtyCloseDialog::discardRequested, this, [this] {
                auto& h = legacy_chrome_hooks().exit_review;
                if (h.discard_current)
                    h.discard_current();
            });
            connect(dialog_, &AidaDirtyCloseDialog::cancelRequested, this, [this] {
                auto& h = legacy_chrome_hooks().exit_review;
                if (h.cancel)
                    h.cancel();
            });
        }
        dialog_->showFor(snapshot);
    }

    if (hooks.exit_gate.committed && hooks.exit_gate.committed())
        return;
    if (hooks.exit_gate.consume_ready && hooks.exit_gate.consume_ready()) {
        if (close_recommitted_.exchange(true, std::memory_order_acq_rel))
            return;
        diag::log_tagged_critical("chrome",
            "exit_review_committed source=qt_controller closing=1");
        if (window_)
            window_->close();
    }
}

}
