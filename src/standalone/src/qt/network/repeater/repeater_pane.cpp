#include "qt/network/repeater/repeater_pane.hpp"

#include <QCheckBox>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/infra/executor.hpp"
#include "core/network/mitm_proxy.hpp"
#include "core/ui/task_center.hpp"
#include "helpers/diag_log.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/network/shared/http_highlighter.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace network_view {
std::uint64_t repeater_next_entry_id();
void repeater_publish_request_artifacts();
artifact_identity_t repeater_entry_identity(const repeater_entry_t& entry,
                                            artifact_kind_t kind);
}

namespace aida::qt::net {

namespace {

constexpr std::size_t k_max_repeater_entries = 128;

}

class RepeaterEntryWidget : public QWidget {
public:
    explicit RepeaterEntryWidget(QWidget* parent = nullptr) : QWidget(parent) {
        const auto& t = theme::tokens();
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(t.spacing.xs);

        banner_ = new QLabel(QStringLiteral("AI REVIEWED DRAFT"), this);
        banner_->setProperty("aidaVariant", QStringLiteral("warning"));
        banner_->setVisible(false);
        layout->addWidget(banner_);

        splitter_ = new QSplitter(Qt::Horizontal, this);
        splitter_->setOpaqueResize(true);
        splitter_->setChildrenCollapsible(false);

        auto* requestPanel = new QWidget(splitter_);
        auto* requestLayout = new QVBoxLayout(requestPanel);
        requestLayout->setContentsMargins(0, 0, 0, 0);
        requestLayout->setSpacing(t.spacing.xs);
        auto* requestTitle = new QLabel(QStringLiteral("Request"), requestPanel);
        requestTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
        requestLayout->addWidget(requestTitle);
        editor_ = new QtHumanRequestEditor(requestPanel);
        QtHumanRequestEditor::Config editorConfig;
        editorConfig.stableId = QStringLiteral("repeater-request");
        editorConfig.maxBytes = (1 << 18) - 1;
        editor_->setConfig(editorConfig);
        requestLayout->addWidget(editor_, 1);
        auto* sendRow = new QHBoxLayout();
        sendRow->setSpacing(t.spacing.sm);
        send_button_ = new widgets::AidaButton(QStringLiteral("Send"), requestPanel);
        send_button_->setKind(widgets::AidaButton::Kind::Primary);
        send_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
        sendRow->addWidget(send_button_);
        sending_label_ = new QLabel(QStringLiteral("Sending..."), requestPanel);
        sending_label_->setProperty("aidaTone", QStringLiteral("titleAccent"));
        sending_label_->setVisible(false);
        sendRow->addWidget(sending_label_);
        sendRow->addStretch(1);
        requestLayout->addLayout(sendRow);
        splitter_->addWidget(requestPanel);

        auto* responsePanel = new QWidget(splitter_);
        auto* responseLayout = new QVBoxLayout(responsePanel);
        responseLayout->setContentsMargins(0, 0, 0, 0);
        responseLayout->setSpacing(t.spacing.xs);
        auto* responseHeader = new QHBoxLayout();
        responseHeader->setSpacing(t.spacing.xs);
        auto* responseTitle = new QLabel(QStringLiteral("Response"), responsePanel);
        responseTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
        responseHeader->addWidget(responseTitle);
        status_label_ = new QLabel(responsePanel);
        responseHeader->addWidget(status_label_);
        latency_label_ = new QLabel(responsePanel);
        latency_label_->setProperty("aidaTone", QStringLiteral("dim"));
        responseHeader->addWidget(latency_label_);
        responseHeader->addStretch(1);
        responseLayout->addLayout(responseHeader);
        response_view_ = new QPlainTextEdit(responsePanel);
        response_view_->setReadOnly(true);
        response_view_->setFont(theme::fonts::codeRegular());
        response_view_->setPlaceholderText(QStringLiteral("Send the request to see the response"));
        attach_http_highlighter(response_view_);
        responseLayout->addWidget(response_view_, 1);
        splitter_->addWidget(responsePanel);
        splitter_->setStretchFactor(0, 1);
        splitter_->setStretchFactor(1, 1);
        layout->addWidget(splitter_, 1);
    }

    QtHumanRequestEditor* editor() const noexcept { return editor_; }
    QPlainTextEdit* responseView() const noexcept { return response_view_; }
    widgets::AidaButton* sendButton() const noexcept { return send_button_; }
    QLabel* sendingLabel() const noexcept { return sending_label_; }
    QLabel* banner() const noexcept { return banner_; }

    void refreshResponse(const network_view::repeater_entry_t& entry) {
        if (entry.status_code > 0) {
            status_label_->setText(QStringLiteral(" %1").arg(entry.status_code));
            const auto& t = theme::tokens();
            const QColor code = status_code_color(entry.status_code);
            if (code == t.success) set_label_tone(status_label_, "success");
            else if (code == t.info) set_label_tone(status_label_, "info");
            else if (code == t.warning) set_label_tone(status_label_, "warning");
            else if (code == t.error) set_label_tone(status_label_, "error");
            else set_label_tone(status_label_, "dim");
            latency_label_->setText(QStringLiteral(" %1ms")
                .arg(static_cast<unsigned long long>(entry.latency_ms)));
        } else {
            status_label_->clear();
            latency_label_->clear();
        }
        response_view_->setPlainText(QString::fromStdString(entry.raw_response));
    }

    void refreshReviewBanner(const network_view::repeater_entry_t& entry) {
        banner_->setVisible(entry.reviewed_draft);
        if (entry.reviewed_draft) {
            banner_->setToolTip(QStringLiteral("Source hash: 0x%1\nProvenance: %2")
                .arg(QString::number(
                    static_cast<unsigned long long>(entry.reviewed_source_hash), 16)
                    .toUpper().rightJustified(16, QLatin1Char('0')))
                .arg(QString::fromStdString(entry.review_provenance)));
        }
    }

    void setInProgress(bool inProgress) {
        sending_label_->setVisible(inProgress);
        send_button_->setVisible(!inProgress);
        editor_->setEditable(!inProgress);
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        const auto& grid = theme::fonts::monoGrid();
        const qreal unit = grid.cell_w > 0.0 ? grid.cell_w
            : QFontMetricsF(theme::fonts::codeRegular()).horizontalAdvance(QLatin1Char('M'));
        const Qt::Orientation wanted = width() < qCeil(unit * 88.0)
            ? Qt::Vertical : Qt::Horizontal;
        if (splitter_->orientation() != wanted)
            splitter_->setOrientation(wanted);
    }

private:
    QLabel* banner_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QtHumanRequestEditor* editor_ = nullptr;
    widgets::AidaButton* send_button_ = nullptr;
    QLabel* sending_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* latency_label_ = nullptr;
    QPlainTextEdit* response_view_ = nullptr;
};

RepeaterEntryController::RepeaterEntryController(
    std::shared_ptr<network_view::repeater_entry_t> entry, QObject* parent)
    : QObject(parent), entry_(std::move(entry)) {}

void RepeaterEntryController::send(const QString& requestText) {
    if (entry_->in_progress.load(std::memory_order_acquire))
        return;
    const std::string host = entry_->host;
    const std::uint16_t port = entry_->port;
    const bool useTls = entry_->use_tls;
    const std::uint64_t sentRevision = entry_->request_revision;
    const std::string raw = requestText.toStdString();
    entry_->in_progress.store(true, std::memory_order_release);
    diag::log_tagged_fmt("network", "repeater_send_clicked host=%s:%u tls=%d req_size=%zu",
        entry_->host.c_str(), entry_->port, entry_->use_tls ? 1 : 0,
        entry_->raw_request.size());
    diag::log_tagged("net_audit",
        (std::string("[net_audit] repeater send host=") + entry_->host + ":" +
         std::to_string(entry_->port) + " tls=" + (entry_->use_tls ? "1" : "0")).c_str());

    QPointer<RepeaterEntryController> controller(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "repeater_send";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [controller, host, port, useTls, sentRevision, raw]() {
        std::vector<std::uint8_t> rawBytes(raw.begin(), raw.end());
        const auto t0 = GetTickCount64();
        auto result = mitm_proxy::repeat_request(host, port, useTls, rawBytes);
        const std::uint64_t elapsed = GetTickCount64() - t0;
        std::string rawResponse;
        int statusCode = 0;
        std::uint64_t latencyMs = 0;
        std::uint64_t responseTimestamp = 0;
        if (result.success) {
            rawResponse = std::string(result.exchange.raw_response.begin(),
                result.exchange.raw_response.end());
            statusCode = result.exchange.response.status_code;
            latencyMs = result.exchange.latency_ms;
            responseTimestamp = result.exchange.timestamp;
            diag::log_tagged_fmt("network", "repeater_send_ok host=%s:%u status=%d size=%zu latency_ms=%llu wall_ms=%llu",
                host.c_str(), port, statusCode,
                rawResponse.size(),
                static_cast<unsigned long long>(latencyMs),
                static_cast<unsigned long long>(elapsed));
        } else {
            rawResponse = "Error: " + result.error;
            statusCode = 0;
            diag::log_tagged_fmt("network", "repeater_send_failed host=%s:%u err='%s' wall_ms=%llu",
                host.c_str(), port, result.error.c_str(),
                static_cast<unsigned long long>(elapsed));
            diag::log_tagged("net_audit",
                (std::string("[net_audit] repeater send FAILED err='") + result.error + "'").c_str());
        }
        if (!controller)
            return;
        QMetaObject::invokeMethod(controller.data(),
            [controller, sentRevision, rawResponse = std::move(rawResponse), statusCode,
             latencyMs, responseTimestamp]() mutable {
                auto& entry = *controller->entry_;
                entry.in_progress.store(false, std::memory_order_release);
                if (entry.request_revision == sentRevision) {
                    entry.raw_response = std::move(rawResponse);
                    entry.status_code = statusCode;
                    entry.latency_ms = latencyMs;
                    entry.response_timestamp = responseTimestamp;
                    entry.response_hash = network_view::artifact_content_hash(
                        std::vector<std::uint8_t>(entry.raw_response.begin(),
                            entry.raw_response.end()));
                }
                Q_EMIT controller->sendCompleted(controller->id());
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        entry_->raw_response = "Error: executor rejected repeater_send";
        entry_->status_code = 0;
        entry_->in_progress.store(false, std::memory_order_release);
        Q_EMIT sendCompleted(id());
    }
}

RepeaterPane::RepeaterPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.repeater"));
    const auto& t = theme::tokens();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(t.spacing.sm);
    headerRow->addWidget(new QLabel(QStringLiteral("Host:"), content));
    host_edit_ = new QLineEdit(content);
    host_edit_->setMaxLength(255);
    host_edit_->setPlaceholderText(QStringLiteral("example.com"));
    headerRow->addWidget(host_edit_, 1);
    headerRow->addWidget(new QLabel(QStringLiteral("Port:"), content));
    port_spin_ = new QSpinBox(content);
    port_spin_->setRange(1, 65535);
    port_spin_->setValue(443);
    headerRow->addWidget(port_spin_);
    tls_check_ = new QCheckBox(QStringLiteral("TLS"), content);
    tls_check_->setChecked(true);
    headerRow->addWidget(tls_check_);
    new_button_ = new widgets::AidaButton(QStringLiteral("New"), content);
    new_button_->setKind(widgets::AidaButton::Kind::Secondary);
    new_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    new_button_->setToolTip(QStringLiteral("Create a blank repeater entry for the host above"));
    headerRow->addWidget(new_button_);
    layout->addLayout(headerRow);

    empty_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No repeater entries"),
        QStringLiteral("Use New to create one, or Send to Repeater from proxy history."),
        content);
    empty_view_->setObjectName(QStringLiteral("aida.view.network.repeater.empty"));
    empty_view_->setActionLabel(QStringLiteral("New"));
    connect(empty_view_, &widgets::AidaStateView::actionTriggered, this, [this] {
        if (new_button_->isEnabled())
            new_button_->click();
    });
    layout->addWidget(empty_view_, 1);

    tabs_ = new QTabWidget(content);
    tabs_->setTabsClosable(true);
    tabs_->setVisible(false);
    layout->addWidget(tabs_, 1);

    connect(new_button_, &QAbstractButton::clicked, this, [this] { newEntry(); });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        closeEntry(index);
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (syncing_)
            return;
        QWidget* widget = tabs_->widget(index);
        if (!widget)
            return;
        auto* controller = controllers_.value(static_cast<std::uint64_t>(
            widget->property("entryId").toULongLong()));
        if (!controller)
            return;
        auto& entry = *controller->entry();
        selected_kinds_[entry.id] = network_view::artifact_kind_t::repeater_request;
        network_view::g_state.repeater_selected = index;
        network_view::publish_network_selection(network_view::repeater_entry_identity(
            entry, network_view::artifact_kind_t::repeater_request), true);
    });

    sync_timer_ = new QTimer(this);
    sync_timer_->setInterval(500);
    connect(sync_timer_, &QTimer::timeout, this, [this] { syncTabs(); });

    syncTabs();
    setContent(content);
}

void RepeaterPane::onPaneShown() {
    syncTabs();
    sync_timer_->start();
}

void RepeaterPane::onPaneHidden() {
    sync_timer_->stop();
}

void RepeaterPane::syncTabs() {
    auto& entries = network_view::g_state.repeater_entries;
    std::vector<std::uint64_t> wanted;
    wanted.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry)
            wanted.push_back(entry->id);
    }
    std::vector<std::uint64_t> current;
    current.reserve(static_cast<std::size_t>(tabs_->count()));
    for (int i = 0; i < tabs_->count(); ++i)
        current.push_back(static_cast<std::uint64_t>(
            tabs_->widget(i)->property("entryId").toULongLong()));
    if (wanted == current) {
        new_button_->setEnabled(wanted.size() < k_max_repeater_entries);
        const bool empty = wanted.empty();
        empty_view_->setVisible(empty);
        tabs_->setVisible(!empty);
        return;
    }
    syncing_ = true;
    for (int i = tabs_->count() - 1; i >= 0; --i) {
        const std::uint64_t id = static_cast<std::uint64_t>(
            tabs_->widget(i)->property("entryId").toULongLong());
        if (std::find(wanted.begin(), wanted.end(), id) == wanted.end()) {
            QWidget* widget = tabs_->widget(i);
            tabs_->removeTab(i);
            controllers_.remove(id);
            selected_kinds_.remove(id);
            delete widget;
        }
    }
    for (std::size_t position = 0; position < wanted.size(); ++position) {
        const std::uint64_t id = wanted[position];
        const auto found = std::find_if(entries.begin(), entries.end(),
            [id](const auto& entry) { return entry && entry->id == id; });
        if (found == entries.end())
            continue;
        const int existingIndex = [this, id]() {
            for (int i = 0; i < tabs_->count(); ++i) {
                if (static_cast<std::uint64_t>(
                        tabs_->widget(i)->property("entryId").toULongLong()) == id)
                    return i;
            }
            return -1;
        }();
        RepeaterEntryWidget* widget = nullptr;
        if (existingIndex >= 0) {
            widget = static_cast<RepeaterEntryWidget*>(tabs_->widget(existingIndex));
            if (existingIndex != static_cast<int>(position)) {
                tabs_->removeTab(existingIndex);
                tabs_->insertTab(static_cast<int>(position), widget,
                    QStringLiteral("#%1").arg(position + 1));
            }
        } else {
            auto* controller = new RepeaterEntryController(*found, this);
            controllers_.insert(id, controller);
            selected_kinds_.insert(id, network_view::artifact_kind_t::repeater_request);
            widget = new RepeaterEntryWidget();
            widget->setProperty("entryId", static_cast<qulonglong>(id));
            controller->setParent(widget);

            auto& entry = *controller->entry();
            widget->editor()->setAuthority(
                QStringLiteral("repeater.%1").arg(static_cast<unsigned long long>(entry.id)),
                QString::fromStdString(entry.raw_request));
            widget->refreshResponse(entry);
            widget->refreshReviewBanner(entry);
            widget->setInProgress(entry.in_progress.load(std::memory_order_acquire));

            QPointer<RepeaterPane> pane(this);
            QPointer<RepeaterEntryWidget> widgetGuard(widget);
            const std::uint64_t entryId = id;
            connect(widget->editor(), &QtHumanRequestEditor::authorityChanged, widget,
                [pane, widgetGuard, entryId] {
                    if (!pane || !widgetGuard)
                        return;
                    auto* controller = pane->controllers_.value(entryId);
                    if (!controller)
                        return;
                    auto& entry = *controller->entry();
                    entry.raw_request = widgetGuard->editor()->authority().toStdString();
                    ++entry.request_revision;
                    entry.request_hash = network_view::artifact_content_hash(
                        std::vector<std::uint8_t>(entry.raw_request.begin(),
                            entry.raw_request.end()));
                    entry.reviewed_draft = false;
                    entry.reviewed_source_hash = 0;
                    entry.review_provenance.clear();
                    network_view::repeater_publish_request_artifacts();
                    pane->selected_kinds_[entry.id] =
                        network_view::artifact_kind_t::repeater_request;
                    const auto changedIdentity = network_view::repeater_entry_identity(
                        entry, network_view::artifact_kind_t::repeater_request);
                    if (changedIdentity.valid())
                        network_view::publish_network_selection(changedIdentity, true);
                    else
                        network_view::clear_stale_network_selection("view.network.repeater");
                    widgetGuard->refreshReviewBanner(entry);
                });
            connect(widget->editor(), &QtHumanRequestEditor::validityChanged, widget,
                [widget](bool, const QString&) {
                    if (!widget->sendButton()->isVisible())
                        return;
                    widget->sendButton()->setEnabled(widget->editor()->isValid() &&
                        !widget->editor()->hasUnappliedPretty());
                });
            connect(widget->editor(), &QtHumanRequestEditor::hasUnappliedPrettyChanged, widget,
                [widget](bool) {
                    if (!widget->sendButton()->isVisible())
                        return;
                    widget->sendButton()->setEnabled(widget->editor()->isValid() &&
                        !widget->editor()->hasUnappliedPretty());
                });
            connect(widget->sendButton(), &QAbstractButton::clicked, widget,
                [pane, entryId] {
                    if (!pane)
                        return;
                    auto* controller = pane->controllers_.value(entryId);
                    if (!controller)
                        return;
                    auto& entry = *controller->entry();
                    pane->selected_kinds_[entry.id] =
                        network_view::artifact_kind_t::repeater_request;
                    network_view::publish_network_selection(
                        network_view::repeater_entry_identity(
                            entry, network_view::artifact_kind_t::repeater_request), true);
                    auto* widget = pane->findEntryWidget(entryId);
                    const QString text = widget
                        ? widget->editor()->authority() : QString();
                    if (widget) {
                        widget->editor()->markClean();
                        widget->setInProgress(true);
                    }
                    controller->send(text);
                });
            connect(controller, &RepeaterEntryController::sendCompleted, widget,
                [pane, entryId](std::uint64_t) {
                    if (!pane)
                        return;
                    pane->onEntrySendCompleted(entryId);
                });
            widget->setObjectName(QStringLiteral("aida.view.network.repeater.entry.%1").arg(entryId));
            widget->installEventFilter(this);
            widget->responseView()->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(widget->responseView(), &QWidget::customContextMenuRequested, widget,
                [pane, entryId](const QPoint& pos) {
                    if (!pane)
                        return;
                    auto* controller = pane->controllers_.value(entryId);
                    if (!controller)
                        return;
                    auto& entry = *controller->entry();
                    const auto responseIdentity = network_view::repeater_entry_identity(
                        entry, network_view::artifact_kind_t::repeater_response);
                    if (responseIdentity.valid())
                        network_view::publish_network_selection(responseIdentity, true);
                    pane->selected_kinds_[entry.id] =
                        network_view::artifact_kind_t::repeater_response;
                    auto* widget = pane->findEntryWidget(entryId);
                    exchange_context_host().show(widget,
                        widget ? widget->responseView()->viewport()->mapToGlobal(pos)
                               : QPoint(),
                        responseIdentity,
                        network_view::repeater_entry_identity(
                            entry, network_view::artifact_kind_t::repeater_request),
                        network_view::exchange_context_origin_t::pointer);
                });
            tabs_->insertTab(static_cast<int>(position), widget,
                QStringLiteral("#%1").arg(position + 1));
        }
    }
    for (int i = 0; i < tabs_->count(); ++i)
        tabs_->setTabText(i, QStringLiteral("#%1").arg(i + 1));
    syncing_ = false;
    new_button_->setEnabled(wanted.size() < k_max_repeater_entries);
    const bool empty = wanted.empty();
    empty_view_->setVisible(empty);
    tabs_->setVisible(!empty);
    if (!empty) {
        const int backendSelected = network_view::g_state.repeater_selected;
        if (backendSelected >= 0 && backendSelected < tabs_->count() &&
            tabs_->currentIndex() != backendSelected)
            tabs_->setCurrentIndex(backendSelected);
        else if (tabs_->currentIndex() < 0)
            tabs_->setCurrentIndex(0);
    }
    const auto& selectedEntries = network_view::g_state.repeater_entries;
    if (selectedEntries.empty())
        network_view::clear_stale_network_selection("view.network.repeater");
}

void RepeaterPane::newEntry() {
    auto& entries = network_view::g_state.repeater_entries;
    if (entries.size() >= k_max_repeater_entries)
        return;
    auto rep = std::make_shared<network_view::repeater_entry_t>();
    rep->id = network_view::repeater_next_entry_id();
    rep->host = host_edit_->text().toStdString();
    rep->port = static_cast<std::uint16_t>(port_spin_->value());
    rep->use_tls = tls_check_->isChecked();
    rep->raw_request = "GET / HTTP/1.1\r\nHost: " + rep->host + "\r\n\r\n";
    rep->request_hash = network_view::artifact_content_hash(
        std::vector<std::uint8_t>(rep->raw_request.begin(), rep->raw_request.end()));
    diag::log_tagged_fmt("network", "repeater_new_entry host=%s:%d tls=%d",
        rep->host.c_str(), rep->port, rep->use_tls ? 1 : 0);
    entries.push_back(std::move(rep));
    network_view::repeater_publish_request_artifacts();
    syncTabs();
    if (tabs_->count() > 0)
        tabs_->setCurrentIndex(tabs_->count() - 1);
}

void RepeaterPane::closeEntry(int tabIndex) {
    QWidget* widget = tabs_->widget(tabIndex);
    if (!widget)
        return;
    const std::uint64_t removedId = static_cast<std::uint64_t>(
        widget->property("entryId").toULongLong());
    diag::log_tagged_fmt("network", "repeater_entry_closed idx=%d", tabIndex);
    auto& entries = network_view::g_state.repeater_entries;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (*it && (*it)->id == removedId) {
            entries.erase(it);
            break;
        }
    }
    controllers_.remove(removedId);
    selected_kinds_.remove(removedId);
    network_view::repeater_publish_request_artifacts();
    syncTabs();
}

RepeaterEntryWidget* RepeaterPane::findEntryWidget(std::uint64_t entryId) const {
    for (int i = 0; i < tabs_->count(); ++i) {
        QWidget* widget = tabs_->widget(i);
        if (widget && widget->property("entryId").toULongLong() ==
            static_cast<qulonglong>(entryId))
            return static_cast<RepeaterEntryWidget*>(widget);
    }
    return nullptr;
}

void RepeaterPane::onEntrySendCompleted(std::uint64_t entryId) {
    auto* controller = controllers_.value(entryId);
    if (!controller)
        return;
    auto* widget = findEntryWidget(entryId);
    if (!widget)
        return;
    auto& entry = *controller->entry();
    widget->setInProgress(false);
    widget->refreshResponse(entry);
    widget->sendButton()->setEnabled(widget->editor()->isValid() &&
        !widget->editor()->hasUnappliedPretty());
}

bool RepeaterPane::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::ContextMenu)
        return NetworkPaneBase::eventFilter(watched, event);
    if (!watched->property("entryId").isValid())
        return false;
    auto* widget = static_cast<RepeaterEntryWidget*>(watched);
    auto* contextEvent = static_cast<QContextMenuEvent*>(event);
    const std::uint64_t entryId = static_cast<std::uint64_t>(
        widget->property("entryId").toULongLong());
    auto* controller = controllers_.value(entryId);
    if (!controller)
        return false;
    auto& entry = *controller->entry();
    const auto requestIdentity = network_view::repeater_entry_identity(
        entry, network_view::artifact_kind_t::repeater_request);
    selected_kinds_[entry.id] = network_view::artifact_kind_t::repeater_request;
    network_view::publish_network_selection(requestIdentity, true);
    exchange_context_host().show(widget, contextEvent->globalPos(),
        requestIdentity,
        network_view::repeater_entry_identity(
            entry, network_view::artifact_kind_t::repeater_response),
        contextEvent->reason() == QContextMenuEvent::Keyboard
            ? network_view::exchange_context_origin_t::menu_key
            : network_view::exchange_context_origin_t::pointer);
    return true;
}

}
