#include "qt/net/qt_h2_editor_view.hpp"

#include <QCheckBox>
#include <QContextMenuEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "core/infra/executor.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/interaction_context.hpp"
#include "helpers/diag_log.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

bool hexChar(char c, std::uint8_t& out)
{
    if (c >= '0' && c <= '9') { out = static_cast<std::uint8_t>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { out = static_cast<std::uint8_t>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { out = static_cast<std::uint8_t>(c - 'A' + 10); return true; }
    return false;
}

std::vector<std::uint8_t> hexDecode(const std::string& s)
{
    std::vector<std::uint8_t> out;
    out.reserve(s.size() / 2);
    std::uint8_t high = 0;
    bool haveHigh = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        std::uint8_t v = 0;
        if (!hexChar(c, v))
            return {};
        if (!haveHigh) { high = v; haveHigh = true; }
        else { out.push_back(static_cast<std::uint8_t>((high << 4) | v)); haveHigh = false; }
    }
    return out;
}

std::string hexEncode(const std::vector<std::uint8_t>& v, std::size_t maxBytes)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    const std::size_t n = v.size() < maxBytes ? v.size() : maxBytes;
    out.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(hex[(v[i] >> 4) & 0xF]);
        out.push_back(hex[v[i] & 0xF]);
        if ((i & 0xF) == 0xF) out.push_back('\n');
        else out.push_back(' ');
    }
    if (v.size() > maxBytes)
        out += "... (truncated)";
    return out;
}

std::vector<std::uint8_t> requestBytes(const aida::burp::h2_editor::request_t& request)
{
    if (request.use_raw_frames) {
        std::vector<std::uint8_t> bytes;
        for (const auto& frame : request.raw_frames) {
            auto encoded = aida::burp::h2_editor::encode_frame(frame);
            bytes.insert(bytes.end(), encoded.begin(), encoded.end());
        }
        return bytes;
    }
    std::string raw = request.pseudo.method + " " + request.pseudo.path + " HTTP/2\r\n";
    raw += "Host: " + request.pseudo.authority + "\r\n";
    raw += ":scheme: " + request.pseudo.scheme + "\r\n";
    for (const auto& header : request.headers)
        raw += header.first + ": " + header.second + "\r\n";
    raw += "\r\n";
    raw.append(reinterpret_cast<const char*>(request.body.data()), request.body.size());
    return std::vector<std::uint8_t>(raw.begin(), raw.end());
}

std::vector<std::uint8_t> responseBytes(const aida::burp::h2_editor::response_t& response)
{
    std::string raw = "HTTP/2 " + std::to_string(response.status_code) + "\r\n";
    for (const auto& header : response.headers)
        raw += header.first + ": " + header.second + "\r\n";
    raw += "\r\n";
    raw.append(reinterpret_cast<const char*>(response.body.data()), response.body.size());
    return std::vector<std::uint8_t>(raw.begin(), raw.end());
}

struct h2_store_t {
    std::shared_ptr<const QtH2RetainedSnapshot> snapshot =
        std::make_shared<const QtH2RetainedSnapshot>();
    std::atomic<bool> accepting{true};
    std::atomic<bool> in_flight{false};
    std::atomic<std::uint64_t> lifetime_generation{1};
    std::uint64_t next_exchange_id = 1;
};

h2_store_t& h2Store()
{
    static h2_store_t store;
    return store;
}

void publishH2(std::shared_ptr<const QtH2RetainedSnapshot> snapshot)
{
    std::atomic_store_explicit(&h2Store().snapshot, std::move(snapshot),
        std::memory_order_release);
}

}

QtH2EditorController::QtH2EditorController(QObject* parent)
    : QObject(parent) {}

void QtH2EditorController::initialize()
{
    h2Store().lifetime_generation.fetch_add(1, std::memory_order_acq_rel);
    h2Store().accepting.store(true, std::memory_order_release);
    h2Store().in_flight.store(false, std::memory_order_release);
    ::diag::log_tagged("h2_v", "initialize");
}

void QtH2EditorController::shutdown()
{
    h2Store().accepting.store(false, std::memory_order_release);
    h2Store().lifetime_generation.fetch_add(1, std::memory_order_acq_rel);
    h2Store().in_flight.store(false, std::memory_order_release);
    ::diag::log_tagged("h2_v", "shutdown");
}

bool QtH2EditorController::inFlight() const
{
    return h2Store().in_flight.load(std::memory_order_acquire);
}

std::shared_ptr<const QtH2RetainedSnapshot> QtH2EditorController::retained() const
{
    return std::atomic_load_explicit(&h2Store().snapshot, std::memory_order_acquire);
}

network_view::artifact_identity_t QtH2EditorController::artifactIdentity(bool response) const
{
    const auto snapshot = retained();
    network_view::artifact_identity_t identity;
    const std::uint64_t hash = response ? snapshot->response_hash : snapshot->request_hash;
    const std::size_t size = response ? snapshot->response_size : snapshot->request_size;
    if (hash == 0 || size == 0)
        return identity;
    identity.kind = response ? network_view::artifact_kind_t::http2_response
                             : network_view::artifact_kind_t::http2_request;
    identity.id = "network.h2." + std::to_string(snapshot->exchange_id) +
        (response ? ".response" : ".request");
    identity.parent_id = "network.h2." + std::to_string(snapshot->exchange_id);
    identity.source_view_id = "view.network.h2_editor";
    identity.source_id = snapshot->exchange_id;
    identity.timestamp = snapshot->generation;
    identity.revision = snapshot->generation;
    identity.content_size = size;
    identity.content_hash = hash;
    identity.label = response ? "HTTP/2 response" : "HTTP/2 request";
    identity.target_host = snapshot->host;
    identity.target_port = snapshot->port;
    identity.use_tls = snapshot->tls;
    identity.raw_protocol = !response && snapshot->raw_protocol;
    return identity;
}

void QtH2EditorController::sendRequest(const aida::burp::h2_editor::request_t& request)
{
    if (h2Store().in_flight.exchange(true, std::memory_order_acq_rel))
        return;

    const std::uint64_t exchangeId = h2Store().next_exchange_id++;
    const std::uint64_t lifetimeGeneration =
        h2Store().lifetime_generation.load(std::memory_order_acquire);
    auto snapshot = std::make_shared<QtH2RetainedSnapshot>();
    snapshot->exchange_id = exchangeId;
    snapshot->generation = exchangeId;
    snapshot->request = requestBytes(request);
    snapshot->request_size = snapshot->request.size();
    snapshot->request_hash = http_text::fnv1a64(snapshot->request);
    snapshot->host = request.host;
    snapshot->port = request.port;
    snapshot->tls = request.pseudo.scheme != "http";
    snapshot->raw_protocol = request.use_raw_frames;
    snapshot->has_response = false;
    publishH2(std::move(snapshot));
    Q_EMIT inFlightChanged(true);

    ::diag::log_tagged_fmt("h2_v", "send host=%s port=%d method=%s path=%s raw=%d",
        request.host.c_str(), request.port, request.pseudo.method.c_str(),
        request.pseudo.path.c_str(), request.use_raw_frames ? 1 : 0);

    QPointer<QtH2EditorController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.h2_view";
    submission.label = "h2.send_request";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [guard, request, exchangeId, lifetimeGeneration]() {
        try {
            aida::burp::h2_editor::response_t response =
                aida::burp::h2_editor::send(request);
            ::diag::log_tagged_fmt("h2_v",
                "send_worker_exit status=%d ok=%d latency=%llums",
                response.status_code, response.ok ? 1 : 0,
                static_cast<unsigned long long>(response.latency_ms));
            if (!guard)
                return;
            QMetaObject::invokeMethod(guard.data(),
                [guard, exchangeId, lifetimeGeneration, response = std::move(response)]() mutable {
                    auto* self = guard.data();
                    if (!self)
                        return;
                    self->applySendResult(exchangeId, lifetimeGeneration, response);
                }, Qt::QueuedConnection);
        } catch (const std::exception& ex) {
            ::diag::log_tagged_fmt("h2_v", "send_worker_exception err=%s", ex.what());
            if (!guard)
                return;
            QMetaObject::invokeMethod(guard.data(),
                [guard, lifetimeGeneration]() {
                    auto* self = guard.data();
                    if (!self)
                        return;
                    if (h2Store().accepting.load(std::memory_order_acquire) &&
                        h2Store().lifetime_generation.load(std::memory_order_acquire) ==
                            lifetimeGeneration) {
                        h2Store().in_flight.store(false, std::memory_order_release);
                        Q_EMIT self->inFlightChanged(false);
                    }
                }, Qt::QueuedConnection);
        } catch (...) {
            ::diag::log_tagged_fmt("h2_v", "send_worker_exception err=unknown");
            if (!guard)
                return;
            QMetaObject::invokeMethod(guard.data(),
                [guard, lifetimeGeneration]() {
                    auto* self = guard.data();
                    if (!self)
                        return;
                    if (h2Store().accepting.load(std::memory_order_acquire) &&
                        h2Store().lifetime_generation.load(std::memory_order_acquire) ==
                            lifetimeGeneration) {
                        h2Store().in_flight.store(false, std::memory_order_release);
                        Q_EMIT self->inFlightChanged(false);
                    }
                }, Qt::QueuedConnection);
        }
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        h2Store().in_flight.store(false, std::memory_order_release);
        ::diag::log_tagged_fmt("h2_v", "send_worker_post_failed host=%s port=%u raw=%d",
            request.host.c_str(), static_cast<unsigned>(request.port),
            request.use_raw_frames ? 1 : 0);
        Q_EMIT inFlightChanged(false);
    }
}

void QtH2EditorController::applySendResult(std::uint64_t exchangeId,
    std::uint64_t lifetimeGeneration, const aida::burp::h2_editor::response_t& response)
{
    if (!h2Store().accepting.load(std::memory_order_acquire) ||
        h2Store().lifetime_generation.load(std::memory_order_acquire) != lifetimeGeneration)
        return;
    const auto current = retained();
    if (current->exchange_id == exchangeId) {
        auto next = std::make_shared<QtH2RetainedSnapshot>(*current);
        next->last_response = response;
        next->response = responseBytes(response);
        next->response_size = next->response.size();
        next->response_hash = http_text::fnv1a64(next->response);
        next->has_response = true;
        publishH2(std::move(next));
    }
    h2Store().in_flight.store(false, std::memory_order_release);
    Q_EMIT inFlightChanged(false);
    Q_EMIT responseChanged();
}

bool QtH2EditorController::resolveRetainedArtifact(std::uint64_t exchangeId,
    std::uint64_t generation, bool response, std::vector<std::uint8_t>& bytes,
    std::string& unavailable_reason)
{
    const auto snapshot = std::atomic_load_explicit(&h2Store().snapshot,
        std::memory_order_acquire);
    if (!h2Store().accepting.load(std::memory_order_acquire) ||
        snapshot->exchange_id != exchangeId || snapshot->generation != generation) {
        unavailable_reason = "The HTTP/2 editor now owns a newer exchange; reopen actions on the current request.";
        return false;
    }
    bytes = response ? snapshot->response : snapshot->request;
    if (bytes.empty()) {
        unavailable_reason = response ? "The HTTP/2 exchange has no retained response."
                                      : "The HTTP/2 exchange has no retained request.";
        return false;
    }
    unavailable_reason.clear();
    return true;
}

QtH2EditorView::QtH2EditorView(QWidget* parent)
    : NetworkPaneBase(parent)
{
    setObjectName(QStringLiteral("view.network.h2_editor"));
    setRequiresTarget(false);

    const auto& t = theme::tokens();
    controller_ = new QtH2EditorController(this);
    controller_->initialize();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.xs);

    auto* title = new QLabel(QStringLiteral("HTTP/2 Frame Editor"), content);
    title->setProperty("aidaTone", QStringLiteral("titleAccent"));
    layout->addWidget(title);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);

    auto* leftPane = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(t.spacing.xs);

    auto* targetLabel = new QLabel(QStringLiteral("Target"), leftPane);
    targetLabel->setProperty("aidaTone", QStringLiteral("secondary"));
    leftLayout->addWidget(targetLabel);
    auto* targetRow = new QHBoxLayout();
    targetRow->setSpacing(t.spacing.xs);
    hostEdit_ = new QLineEdit(QStringLiteral("example.com"), leftPane);
    hostEdit_->setObjectName(QStringLiteral("view.network.h2_editor.host"));
    hostEdit_->setMaxLength(255);
    targetRow->addWidget(hostEdit_, 1);
    portSpin_ = new QSpinBox(leftPane);
    portSpin_->setObjectName(QStringLiteral("view.network.h2_editor.port"));
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(443);
    targetRow->addWidget(portSpin_);
    timeoutSpin_ = new QSpinBox(leftPane);
    timeoutSpin_->setObjectName(QStringLiteral("view.network.h2_editor.timeout"));
    timeoutSpin_->setRange(500, 600000);
    timeoutSpin_->setValue(15000);
    timeoutSpin_->setSuffix(QStringLiteral(" ms"));
    targetRow->addWidget(timeoutSpin_);
    leftLayout->addLayout(targetRow);

    rawModeCheck_ = new QCheckBox(QStringLiteral("Raw frames mode"), leftPane);
    rawModeCheck_->setObjectName(QStringLiteral("view.network.h2_editor.raw_mode"));
    leftLayout->addWidget(rawModeCheck_);

    auto* editHost = new QWidget(leftPane);
    editStack_ = new QStackedLayout(editHost);
    editStack_->setStackingMode(QStackedLayout::StackOne);

    auto* structuredPage = new QWidget(editHost);
    auto* structuredLayout = new QVBoxLayout(structuredPage);
    structuredLayout->setContentsMargins(0, 0, 0, 0);
    structuredLayout->setSpacing(t.spacing.xs);
    auto* pseudoLabel = new QLabel(QStringLiteral("Pseudo-headers"), structuredPage);
    pseudoLabel->setProperty("aidaTone", QStringLiteral("secondary"));
    structuredLayout->addWidget(pseudoLabel);
    auto* pseudoRow1 = new QHBoxLayout();
    pseudoRow1->setSpacing(t.spacing.xs);
    methodEdit_ = new QLineEdit(QStringLiteral("GET"), structuredPage);
    methodEdit_->setObjectName(QStringLiteral("view.network.h2_editor.method"));
    methodEdit_->setMaxLength(15);
    methodEdit_->setMaximumWidth(field_width_chars(methodEdit_, 8));
    pseudoRow1->addWidget(methodEdit_);
    schemeEdit_ = new QLineEdit(QStringLiteral("https"), structuredPage);
    schemeEdit_->setObjectName(QStringLiteral("view.network.h2_editor.scheme"));
    schemeEdit_->setMaxLength(15);
    schemeEdit_->setMaximumWidth(field_width_chars(schemeEdit_, 8));
    pseudoRow1->addWidget(schemeEdit_);
    pseudoRow1->addStretch(1);
    structuredLayout->addLayout(pseudoRow1);
    pathEdit_ = new QLineEdit(QStringLiteral("/"), structuredPage);
    pathEdit_->setObjectName(QStringLiteral("view.network.h2_editor.path"));
    pathEdit_->setMaxLength(1023);
    structuredLayout->addWidget(pathEdit_);
    authorityEdit_ = new QLineEdit(structuredPage);
    authorityEdit_->setObjectName(QStringLiteral("view.network.h2_editor.authority"));
    authorityEdit_->setMaxLength(255);
    authorityEdit_->setPlaceholderText(QStringLiteral(":authority (defaults to host)"));
    structuredLayout->addWidget(authorityEdit_);

    auto* flagsLabel = new QLabel(QStringLiteral("Stream flags"), structuredPage);
    flagsLabel->setProperty("aidaTone", QStringLiteral("secondary"));
    structuredLayout->addWidget(flagsLabel);
    auto* flagsRow = new QHBoxLayout();
    flagsRow->setSpacing(t.spacing.sm);
    endStreamCheck_ = new QCheckBox(QStringLiteral("END_STREAM"), structuredPage);
    endStreamCheck_->setChecked(true);
    flagsRow->addWidget(endStreamCheck_);
    endHeadersCheck_ = new QCheckBox(QStringLiteral("END_HEADERS"), structuredPage);
    endHeadersCheck_->setChecked(true);
    flagsRow->addWidget(endHeadersCheck_);
    paddedCheck_ = new QCheckBox(QStringLiteral("PADDED"), structuredPage);
    flagsRow->addWidget(paddedCheck_);
    priorityCheck_ = new QCheckBox(QStringLiteral("PRIORITY"), structuredPage);
    flagsRow->addWidget(priorityCheck_);
    flagsRow->addStretch(1);
    structuredLayout->addLayout(flagsRow);

    auto* headersLabel = new QLabel(QStringLiteral("Headers"), structuredPage);
    headersLabel->setProperty("aidaTone", QStringLiteral("secondary"));
    structuredLayout->addWidget(headersLabel);
    auto* headerInputRow = new QHBoxLayout();
    headerInputRow->setSpacing(t.spacing.xs);
    headerNameEdit_ = new QLineEdit(structuredPage);
    headerNameEdit_->setObjectName(QStringLiteral("view.network.h2_editor.header_name"));
    headerNameEdit_->setMaxLength(127);
    headerNameEdit_->setPlaceholderText(QStringLiteral("name"));
    headerInputRow->addWidget(headerNameEdit_);
    headerValueEdit_ = new QLineEdit(structuredPage);
    headerValueEdit_->setObjectName(QStringLiteral("view.network.h2_editor.header_value"));
    headerValueEdit_->setMaxLength(511);
    headerValueEdit_->setPlaceholderText(QStringLiteral("value"));
    headerInputRow->addWidget(headerValueEdit_, 1);
    headerAddButton_ = new widgets::AidaButton(QStringLiteral("+"), structuredPage);
    headerAddButton_->setObjectName(QStringLiteral("view.network.h2_editor.header.add"));
    headerAddButton_->setToolTip(QStringLiteral("Add header"));
    headerAddButton_->setKind(widgets::AidaButton::Kind::Secondary);
    headerAddButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    headerInputRow->addWidget(headerAddButton_);
    structuredLayout->addLayout(headerInputRow);
    headerRowsHost_ = new QWidget(structuredPage);
    headerRowsLayout_ = new QVBoxLayout(headerRowsHost_);
    headerRowsLayout_->setContentsMargins(0, 0, 0, 0);
    headerRowsLayout_->setSpacing(t.spacing.xxs);
    structuredLayout->addWidget(headerRowsHost_);

    auto* bodyLabel = new QLabel(QStringLiteral("Body"), structuredPage);
    bodyLabel->setProperty("aidaTone", QStringLiteral("secondary"));
    structuredLayout->addWidget(bodyLabel);
    bodyEdit_ = new QtByteCappedPlainTextEdit(structuredPage);
    bodyEdit_->setObjectName(QStringLiteral("view.network.h2_editor.body"));
    bodyEdit_->setMaxBytes(16383);
    bodyEdit_->setFont(theme::fonts::codeRegular());
    structuredLayout->addWidget(bodyEdit_, 1);

    auto* rawPage = new QWidget(editHost);
    auto* rawLayout = new QVBoxLayout(rawPage);
    rawLayout->setContentsMargins(0, 0, 0, 0);
    rawLayout->setSpacing(t.spacing.xs);
    auto* hexHint = new QLabel(QStringLiteral(
        "Hex frames (length(3) type(1) flags(1) Rbit+stream(4) payload):"), rawPage);
    hexHint->setProperty("aidaTone", QStringLiteral("dim"));
    hexHint->setWordWrap(true);
    rawLayout->addWidget(hexHint);
    rawHexEdit_ = new QtByteCappedPlainTextEdit(rawPage);
    rawHexEdit_->setObjectName(QStringLiteral("view.network.h2_editor.raw_hex"));
    rawHexEdit_->setMaxBytes(16383);
    rawHexEdit_->setFont(theme::fonts::codeRegular());
    rawLayout->addWidget(rawHexEdit_, 1);
    hexErrorLabel_ = new QLabel(rawPage);
    hexErrorLabel_->setProperty("aidaTone", QStringLiteral("error"));
    hexErrorLabel_->setVisible(false);
    rawLayout->addWidget(hexErrorLabel_);

    editStack_->addWidget(structuredPage);
    editStack_->addWidget(rawPage);
    leftLayout->addWidget(editHost, 1);

    auto* sendRow = new QHBoxLayout();
    sendRow->setSpacing(t.spacing.xs);
    sendButton_ = new widgets::AidaButton(QStringLiteral("Send"), leftPane);
    sendButton_->setObjectName(QStringLiteral("view.network.h2_editor.send"));
    sendButton_->setKind(widgets::AidaButton::Kind::Primary);
    sendButton_->setControlSize(widgets::AidaButton::ControlSize::Medium);
    sendRow->addWidget(sendButton_);
    sendingLabel_ = new QLabel(QStringLiteral("Sending..."), leftPane);
    sendingLabel_->setProperty("aidaTone", QStringLiteral("accent"));
    sendingLabel_->setVisible(false);
    sendRow->addWidget(sendingLabel_);
    sendRow->addStretch(1);
    leftLayout->addLayout(sendRow);

    auto* rightPane = new QWidget(splitter);
    responsePane_ = rightPane;
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(t.spacing.xs);

    auto* actionsRow = new QHBoxLayout();
    actionsRow->setSpacing(t.spacing.xs);
    exchangeActionsButton_ = new widgets::AidaButton(QStringLiteral("Exchange actions"),
        rightPane);
    exchangeActionsButton_->setObjectName(QStringLiteral("view.network.h2_editor.actions"));
    exchangeActionsButton_->setKind(widgets::AidaButton::Kind::Secondary);
    exchangeActionsButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionsRow->addWidget(exchangeActionsButton_);
    actionsRow->addStretch(1);
    rightLayout->addLayout(actionsRow);

    statusLabel_ = new QLabel(rightPane);
    statusLabel_->setProperty("aidaTone", QStringLiteral("primary"));
    rightLayout->addWidget(statusLabel_);
    errorLabel_ = new QLabel(rightPane);
    errorLabel_->setProperty("aidaTone", QStringLiteral("error"));
    errorLabel_->setWordWrap(true);
    rightLayout->addWidget(errorLabel_);

    noResponseState_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No response yet"),
        QStringLiteral("Send the request to capture response headers, body, and raw wire hex."),
        rightPane);
    rightLayout->addWidget(noResponseState_, 1);

    responseContent_ = new QWidget(rightPane);
    auto* responseLayout = new QVBoxLayout(responseContent_);
    responseLayout->setContentsMargins(0, 0, 0, 0);
    responseLayout->setSpacing(t.spacing.xs);
    auto* headersCaption = new QLabel(QStringLiteral("Headers"), responseContent_);
    headersCaption->setProperty("aidaTone", QStringLiteral("secondary"));
    responseLayout->addWidget(headersCaption);
    responseHeadersView_ = new QPlainTextEdit(responseContent_);
    responseHeadersView_->setObjectName(QStringLiteral("view.network.h2_editor.resp_headers"));
    responseHeadersView_->setReadOnly(true);
    responseHeadersView_->setFont(theme::fonts::codeRegular());
    responseHeadersView_->setMaximumHeight(
        editor_min_height_lines(responseHeadersView_, 6));
    responseLayout->addWidget(responseHeadersView_);
    responseBodyHeader_ = new QLabel(responseContent_);
    responseBodyHeader_->setProperty("aidaTone", QStringLiteral("secondary"));
    responseLayout->addWidget(responseBodyHeader_);
    responseBodyView_ = new QPlainTextEdit(responseContent_);
    responseBodyView_->setObjectName(QStringLiteral("view.network.h2_editor.resp_body"));
    responseBodyView_->setReadOnly(true);
    responseBodyView_->setFont(theme::fonts::codeRegular());
    responseLayout->addWidget(responseBodyView_, 1);
    auto* wireCaption = new QLabel(QStringLiteral("Raw wire (hex)"), responseContent_);
    wireCaption->setProperty("aidaTone", QStringLiteral("secondary"));
    responseLayout->addWidget(wireCaption);
    rawWireView_ = new QPlainTextEdit(responseContent_);
    rawWireView_->setObjectName(QStringLiteral("view.network.h2_editor.resp_wire"));
    rawWireView_->setReadOnly(true);
    rawWireView_->setFont(theme::fonts::codeRegular());
    rawWireView_->setMaximumHeight(editor_min_height_lines(rawWireView_, 5));
    responseLayout->addWidget(rawWireView_);
    rightLayout->addWidget(responseContent_, 1);

    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 55);
    splitter->setStretchFactor(1, 45);
    layout->addWidget(splitter, 1);

    connect(rawModeCheck_, &QCheckBox::toggled, this, [this](bool raw) {
        editStack_->setCurrentIndex(raw ? 1 : 0);
    });
    connect(headerAddButton_, &QAbstractButton::clicked, this, [this] {
        const QString name = headerNameEdit_->text();
        const QString value = headerValueEdit_->text();
        if (name.isEmpty())
            return;
        ::diag::log_tagged_fmt("h2_v", "header_added name='%s' value_len=%zu",
            name.toUtf8().constData(),
            static_cast<std::size_t>(value.toUtf8().size()));
        addHeaderRow(name, value);
        headerNameEdit_->clear();
        headerValueEdit_->clear();
    });
    connect(sendButton_, &QAbstractButton::clicked, this, &QtH2EditorView::sendNow);
    connect(exchangeActionsButton_, &QAbstractButton::clicked, this, [this] {
        const auto requestIdentity = controller_->artifactIdentity(false);
        const auto responseIdentity = controller_->artifactIdentity(true);
        if (!requestIdentity.valid())
            return;
        exchange_context_host().show(exchangeActionsButton_,
            exchangeActionsButton_->mapToGlobal(QPoint(0, exchangeActionsButton_->height())),
            requestIdentity, responseIdentity,
            network_view::exchange_context_origin_t::pointer);
    });
    connect(controller_, &QtH2EditorController::responseChanged, this,
        &QtH2EditorView::refreshResponse);
    connect(controller_, &QtH2EditorController::inFlightChanged, this,
        [this](bool) { refreshBusy(); });

    rightPane->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(rightPane, &QWidget::customContextMenuRequested, this,
        [this, rightPane](const QPoint& pos) {
            showResponseContext(rightPane->mapToGlobal(pos),
                aida::ui::context_menu_open_origin_t::pointer);
        });
    rightPane->installEventFilter(this);

    setContent(content);
    refreshBusy();
    refreshResponse();
}

QtH2EditorView::~QtH2EditorView()
{
    controller_->shutdown();
}

bool QtH2EditorView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == responsePane_ && event->type() == QEvent::ContextMenu) {
        auto* contextEvent = static_cast<QContextMenuEvent*>(event);
        if (contextEvent->reason() == QContextMenuEvent::Keyboard) {
            showResponseContext(
                responsePane_->mapToGlobal(
                    QPoint(responsePane_->width() / 2, responsePane_->height() / 3)),
                aida::ui::context_menu_open_origin_t::menu_key);
            return true;
        }
    }
    return NetworkPaneBase::eventFilter(watched, event);
}

void QtH2EditorView::showResponseContext(const QPoint& globalPos,
                                         aida::ui::context_menu_open_origin_t origin)
{
    const auto requestIdentity = controller_->artifactIdentity(false);
    const auto responseIdentity = controller_->artifactIdentity(true);
    if (!requestIdentity.valid())
        return;
    exchange_context_host().show(responseContent_, globalPos,
        responseIdentity.valid() ? responseIdentity : requestIdentity,
        responseIdentity.valid() ? requestIdentity : network_view::artifact_identity_t{},
        origin == aida::ui::context_menu_open_origin_t::pointer
            ? network_view::exchange_context_origin_t::pointer
            : network_view::exchange_context_origin_t::menu_key);
}

void QtH2EditorView::addHeaderRow(const QString& name, const QString& value)
{
    const auto& t = theme::tokens();
    headers_.emplace_back(name.toStdString(), value.toStdString());
    auto* row = new QWidget(headerRowsHost_);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(t.spacing.xs);
    auto* nameLabel = new QLabel(name + QStringLiteral(":"), row);
    nameLabel->setProperty("aidaTone", QStringLiteral("dim"));
    nameLabel->setToolTip(name);
    rowLayout->addWidget(nameLabel);
    auto* valueLabel = new QLabel(value, row);
    valueLabel->setProperty("aidaTone", QStringLiteral("primary"));
    valueLabel->setToolTip(value);
    rowLayout->addWidget(valueLabel, 1);
    auto* removeButton = new widgets::AidaButton(QStringLiteral("X"), row);
    removeButton->setObjectName(QStringLiteral("view.network.h2_editor.header.remove"));
    removeButton->setToolTip(QStringLiteral("Remove header"));
    removeButton->setKind(widgets::AidaButton::Kind::Ghost);
    removeButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    rowLayout->addWidget(removeButton);
    connect(removeButton, &QAbstractButton::clicked, this, [this, row] {
        const int index = headerRowsLayout_->indexOf(row);
        if (index >= 0 && index < static_cast<int>(headers_.size())) {
            ::diag::log_tagged_fmt("h2_v", "header_removed idx=%d name='%s'", index,
                headers_[static_cast<std::size_t>(index)].first.c_str());
            headers_.erase(headers_.begin() + index);
        }
        row->deleteLater();
    });
    headerRowsLayout_->addWidget(row);
}

void QtH2EditorView::sendNow()
{
    if (controller_->inFlight())
        return;
    aida::burp::h2_editor::request_t request;
    request.host = hostEdit_->text().toStdString();
    request.port = static_cast<std::uint16_t>(portSpin_->value());
    request.timeout_ms = (std::max)(500, timeoutSpin_->value());
    if (rawModeCheck_->isChecked()) {
        const std::string hexText = rawHexEdit_->toPlainText().toStdString();
        std::vector<std::uint8_t> bytes = hexDecode(hexText);
        if (bytes.empty() && !hexText.empty()) {
            bool onlyWhitespace = true;
            for (char c : hexText) {
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                    onlyWhitespace = false;
                    break;
                }
            }
            if (!onlyWhitespace) {
                hexErrorLabel_->setText(QStringLiteral(
                    "Raw frames must be hexadecimal bytes (whitespace ignored)."));
                hexErrorLabel_->setVisible(true);
                return;
            }
        }
        hexErrorLabel_->setVisible(false);
        request.use_raw_frames = true;
        std::vector<aida::burp::h2_editor::frame_t> frames;
        aida::burp::h2_editor::decode_frames(bytes, frames);
        request.raw_frames = std::move(frames);
    } else {
        request.pseudo.method = methodEdit_->text().toStdString();
        request.pseudo.path = pathEdit_->text().toStdString();
        request.pseudo.scheme = schemeEdit_->text().toStdString();
        const std::string authority = authorityEdit_->text().toStdString();
        request.pseudo.authority = !authority.empty() ? authority : request.host;
        request.headers = headers_;
        const std::string body = bodyEdit_->toPlainText().toStdString();
        request.body.assign(body.begin(), body.end());
        std::uint32_t flags = 0;
        if (endStreamCheck_->isChecked())
            flags |= static_cast<std::uint32_t>(
                aida::burp::h2_editor::send_flags_t::end_stream);
        if (endHeadersCheck_->isChecked())
            flags |= static_cast<std::uint32_t>(
                aida::burp::h2_editor::send_flags_t::end_headers);
        if (paddedCheck_->isChecked())
            flags |= static_cast<std::uint32_t>(
                aida::burp::h2_editor::send_flags_t::padded);
        if (priorityCheck_->isChecked())
            flags |= static_cast<std::uint32_t>(
                aida::burp::h2_editor::send_flags_t::priority);
        request.flags = flags;
    }
    controller_->sendRequest(request);
    refreshBusy();
}

void QtH2EditorView::refreshBusy()
{
    const bool inFlight = controller_->inFlight();
    sendButton_->setEnabled(!inFlight);
    sendingLabel_->setVisible(inFlight);
}

void QtH2EditorView::refreshResponse()
{
    const auto snapshot = controller_->retained();
    const auto& response = snapshot->last_response;
    const bool have = snapshot->has_response;
    noResponseState_->setVisible(!have);
    responseContent_->setVisible(have);
    exchangeActionsButton_->setEnabled(controller_->artifactIdentity(false).valid());
    if (!have)
        return;
    statusLabel_->setText(QStringLiteral("Status: %1  Latency: %2ms  %3")
        .arg(response.status_code)
        .arg(static_cast<quint64>(response.latency_ms))
        .arg(response.ok ? QStringLiteral("OK") : QStringLiteral("ERROR")));
    if (!response.error_msg.empty()) {
        ::diag::log_tagged_fmt("h2_v", "response_error msg='%s'",
            response.error_msg.c_str());
        errorLabel_->setText(QString::fromStdString(response.error_msg));
        errorLabel_->setVisible(true);
    } else {
        errorLabel_->clear();
        errorLabel_->setVisible(false);
    }
    QString headersText;
    for (const auto& header : response.headers) {
        headersText += QString::fromStdString(header.first) + QStringLiteral(": ") +
            QString::fromStdString(header.second) + QStringLiteral("\n");
    }
    responseHeadersView_->setPlainText(headersText);

    const std::size_t bodySize = response.body.size();
    responseBodyHeader_->setText(QStringLiteral("Body (%1 bytes)")
        .arg(static_cast<quint64>(bodySize)));
    const std::size_t cap = bodySize < 4096 ? bodySize : 4096;
    QString preview;
    preview.reserve(static_cast<qsizetype>(cap));
    for (std::size_t i = 0; i < cap; ++i) {
        const std::uint8_t b = response.body[i];
        if (b == '\r' || b == '\n' || b == '\t' || (b >= 0x20 && b < 0x7f))
            preview.push_back(static_cast<char>(b));
        else
            preview.push_back(QLatin1Char('.'));
    }
    responseBodyView_->setPlainText(preview);
    rawWireView_->setPlainText(QString::fromStdString(hexEncode(response.raw_wire_in, 1024)));
}

}
