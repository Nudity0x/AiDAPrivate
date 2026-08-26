#include "qt/chrome/aida_status_bar.hpp"

#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/ai/standalone_chat.hpp"
#include "core/debugger/debugger_engine.hpp"
#include "core/editor/code_editor.hpp"
#include "core/mcp/mcp_standalone.hpp"
#include "core/network/network_view.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/session/analysis_session.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/task_center.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/layout/workspace_persistence.hpp"
#include "qt/registry/qt_view_registry.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_theme_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::chrome {

namespace {

const char* debugger_status_label(debugger_engine::dbg_status_t status) noexcept
{
    switch (status) {
    case debugger_engine::dbg_status_t::running: return "Running";
    case debugger_engine::dbg_status_t::paused: return "Paused";
    case debugger_engine::dbg_status_t::stepping: return "Stepping";
    case debugger_engine::dbg_status_t::terminated: return "Terminated";
    case debugger_engine::dbg_status_t::idle: return "Idle";
    }
    return "Unknown";
}

QColor segment_semantic_color(int semantic)
{
    const auto& t = theme::tokens();
    switch (semantic) {
    case 1: return t.success;
    case 2: return t.warning;
    case 3: return t.error;
    case 4: return t.info;
    case 5: return t.accent;
    case 6: return t.live;
    default: return t.text_secondary;
    }
}

struct segment_spec_t {
    AidaStatusSegmentId id;
    int max_chars;
    bool permanent;
};

constexpr segment_spec_t k_segment_specs[10] = {
    {AidaStatusSegmentId::Target, 40, false},
    {AidaStatusSegmentId::Location, 24, false},
    {AidaStatusSegmentId::Workspace, 24, false},
    {AidaStatusSegmentId::Debugger, 18, false},
    {AidaStatusSegmentId::Network, 18, false},
    {AidaStatusSegmentId::Mcp, 18, false},
    {AidaStatusSegmentId::Driver, 26, false},
    {AidaStatusSegmentId::Tasks, 28, true},
    {AidaStatusSegmentId::Diagnostics, 16, true},
    {AidaStatusSegmentId::Frame, 10, true},
};

}

AidaStatusSegment::AidaStatusSegment(AidaStatusSegmentId id, QWidget* parent)
    : QFrame(parent), id_(id)
{
    setObjectName(QStringLiteral("aida.status_bar.segment"));
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(theme::tokens().status_bar.height - theme::tokens().spacing.xxs * 2);
    setFocusPolicy(Qt::StrongFocus);
}

void AidaStatusSegment::setSegmentText(const QString& text, const QString& tooltip,
                                       int semantic)
{
    if (text_ == text && semantic_ == semantic && toolTip() == tooltip)
        return;
    text_ = text;
    semantic_ = semantic;
    setToolTip(tooltip);
    updateGeometry();
    update();
}

void AidaStatusSegment::setSemantic(int semantic)
{
    if (semantic_ == semantic)
        return;
    semantic_ = semantic;
    update();
}

void AidaStatusSegment::setShowsSeparator(bool shows)
{
    if (shows_separator_ == shows)
        return;
    shows_separator_ = shows;
    update();
}

QSize AidaStatusSegment::sizeHint() const
{
    const auto& t = theme::tokens();
    const QFontMetricsF fm(theme::fonts::caption());
    const int dot = semantic_ != 0 ? t.status_bar.dot + t.spacing.xs : 0;
    return QSize(static_cast<int>(std::ceil(fm.horizontalAdvance(text_))) +
                 t.status_bar.padding_x * 2 + dot,
                 t.status_bar.height - t.spacing.xxs * 2);
}

void AidaStatusSegment::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto& t = theme::tokens();
    const QRectF face = rect();
    if (hovered_) {
        const bool dark = widgets::relative_luminance(t.bg_base) < 0.5;
        p.setPen(Qt::NoPen);
        p.setBrush(widgets::with_alpha(t.text_primary, dark ? 0.07 : 0.05));
        p.drawRoundedRect(face.adjusted(1.0, 1.0, -1.0, -1.0), t.radius.xs, t.radius.xs);
    }
    const QFont font = theme::fonts::caption();
    const QFontMetricsF fm(font);
    p.setFont(font);
    qreal x = t.status_bar.padding_x;
    const QColor text_color = segment_semantic_color(semantic_);
    if (semantic_ != 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(text_color);
        const qreal dot = t.status_bar.dot;
        p.drawEllipse(QPointF(x + dot * 0.5, height() * 0.5), dot * 0.5, dot * 0.5);
        x += dot + t.spacing.xs;
    }
    p.setPen(text_color);
    const qreal max_w = width() - x - t.status_bar.padding_x;
    p.drawText(QPointF(x, widgets::text_baseline_centered(face, fm)),
               fm.elidedText(text_, Qt::ElideRight, static_cast<int>((std::max)(1.0, max_w))));
    if (shows_separator_) {
        p.setPen(QPen(t.border_subtle, 1.0));
        p.drawLine(QPointF(width() - 0.5, t.spacing.xs),
                   QPointF(width() - 0.5, height() - t.spacing.xs));
    }
    if (hasFocus() && focus_reason_ != Qt::MouseFocusReason)
        widgets::paint_focus_ring(p, face.adjusted(3.0, 3.0, -3.0, -3.0), t.radius.xs, 1.0);
}

void AidaStatusSegment::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos()))
        Q_EMIT clicked(id_);
    QFrame::mouseReleaseEvent(event);
}

void AidaStatusSegment::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
        event->key() == Qt::Key_Enter) {
        Q_EMIT clicked(id_);
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

void AidaStatusSegment::enterEvent(QEnterEvent* event)
{
    hovered_ = true;
    update();
    QFrame::enterEvent(event);
}

void AidaStatusSegment::leaveEvent(QEvent* event)
{
    hovered_ = false;
    update();
    QFrame::leaveEvent(event);
}

void AidaStatusSegment::focusInEvent(QFocusEvent* event)
{
    focus_reason_ = event->reason();
    update();
    QFrame::focusInEvent(event);
}

AidaStatusBar::AidaStatusBar(docking::AidaDockHost* host, QWidget* parent)
    : QStatusBar(parent), host_(host)
{
    setObjectName(QStringLiteral("aida.status_bar"));
    setSizeGripEnabled(false);
    buildSegments();

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(250);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, [this] { tick(); });
    poll_timer_->start();

    watchdog_timer_ = new QTimer(this);
    watchdog_timer_->setInterval(1000);
    watchdog_timer_->setTimerType(Qt::CoarseTimer);
    connect(watchdog_timer_, &QTimer::timeout, this, [this] { tickDriverWatchdog(); });
    watchdog_timer_->start();

    frame_clock_ = new QElapsedTimer();
    frame_clock_->start();
    connect(this, &QStatusBar::messageChanged, this, [this](const QString& current) {
        if (current.isEmpty())
            raw_message_.clear();
    });
    frame_probe_ = new QTimer(this);
    frame_probe_->setInterval(16);
    frame_probe_->setTimerType(Qt::PreciseTimer);
    connect(frame_probe_, &QTimer::timeout, this, [this] {
        const double ns = static_cast<double>(frame_clock_->nsecsElapsed());
        frame_clock_->restart();
        frame_ms_ = frame_ms_ + (ns / 1.0e6 - frame_ms_) * 0.3;
    });
    if (g_sa_settings.ui_diagnostics_mode)
        frame_probe_->start();
    connect(&theme::AidaThemeController::instance(),
            &theme::AidaThemeController::themeGenerationChanged,
            this, [this](quint64) {
        applySegmentMetrics();
        applyNarrowing();
    });
    tick();
}

AidaStatusBar::~AidaStatusBar()
{
    delete frame_clock_;
}

void AidaStatusBar::buildSegments()
{
    for (const auto& spec : k_segment_specs) {
        auto* segment = new AidaStatusSegment(spec.id, this);
        connect(segment, &AidaStatusSegment::clicked, this,
                [this](AidaStatusSegmentId id) { Q_EMIT segmentActivated(id); });
        const int index = static_cast<int>(spec.id);
        segments_[index] = segment;
        permanent_[index] = spec.permanent;
        if (spec.permanent)
            addPermanentWidget(segment, 0);
        else
            addWidget(segment, 0);
    }
    applySegmentMetrics();
}

void AidaStatusBar::applySegmentMetrics()
{
    const auto& t = theme::tokens();
    const QFontMetricsF fm(theme::fonts::caption());
    const int cap_unit = (std::max)(1,
        static_cast<int>(fm.horizontalAdvance(QLatin1Char('0'))));
    for (const auto& spec : k_segment_specs) {
        AidaStatusSegment* segment = segments_[static_cast<int>(spec.id)];
        if (!segment)
            continue;
        segment->setMinimumWidth(0);
        segment->setMaximumWidth(cap_unit * spec.max_chars + t.status_bar.padding_x * 2 +
            t.status_bar.dot + t.spacing.xs);
        segment->setFixedHeight(t.status_bar.height - t.spacing.xxs * 2);
    }
}

void AidaStatusBar::tick()
{
    const auto status = aida::ui::task_center::status_summary();

    char target[256] = {};
    QString target_tooltip;
    const std::size_t session_index = analysis_session::active_session_idx();
    const auto session = session_index < analysis_session::session_count()
        ? analysis_session::session_handle_at(session_index) : nullptr;
    if (session) {
        const char* name = session->filename.empty() ? session->session_name.c_str()
            : session->filename.c_str();
        if (session->attached_pid != 0)
            std::snprintf(target, sizeof(target), "%s · PID %u", name, session->attached_pid);
        else
            std::snprintf(target, sizeof(target), "%s", name);
        target_tooltip = QString::fromStdString(
            (session->path.empty() ? std::string(name) : session->path) +
            (session->session_name.empty() ? std::string() :
                "\nSession: " + session->session_name));
    } else {
        std::snprintf(target, sizeof(target), "No target");
        target_tooltip = QStringLiteral(
            "No active analysis or process session. Open Sessions to select a target.");
    }

    char location[128] = {};
    QString location_tooltip;
    const auto* registry = host_ ? host_->registry() : nullptr;
    const auto focused = registry ? registry->focused_instance() : std::nullopt;
    const auto editor_document = code_editor_widget::document_state();
    const bool editor_focused = focused && focused->view.value() == "document.code" &&
        editor_document.active;
    if (editor_focused) {
        int line = 0;
        int column = 0;
        code_editor_widget::get_caret(line, column);
        std::snprintf(location, sizeof(location),
            code_editor_widget::has_selection() ? "Ln %d, Col %d · selection" : "Ln %d, Col %d",
            line + 1, column + 1);
        location_tooltip = QStringLiteral("Caret in %1%2")
            .arg(editor_document.filename.empty()
                ? QStringLiteral("active code document")
                : QString::fromStdString(editor_document.filename),
                 code_editor_widget::has_selection()
                     ? QStringLiteral(" with selected text") : QString());
    } else if (const auto workspace_handle = analysis_session::active_workspace()) {
        const auto selection = workspace_handle->view_state().selection;
        if (selection) {
            std::snprintf(location, sizeof(location), "0x%llX",
                static_cast<unsigned long long>(selection->value));
            location_tooltip = QStringLiteral("Current analysis selection: 0x%1")
                .arg(static_cast<unsigned long long>(selection->value), 0, 16).toUpper();
        }
    }

    const auto* persistence = host_ ? host_->persistence() : nullptr;
    const std::string_view workspace_name = persistence
        ? persistence->active_preset_name() : std::string_view{};
    const QString workspace_label = QStringLiteral("Workspace: %1")
        .arg(workspace_name.empty() ? QStringLiteral("Unknown")
            : QString::fromLatin1(workspace_name.data(),
                                  static_cast<qsizetype>(workspace_name.size())));

    char debugger[96] = {};
    const auto debugger_state = debugger_engine::g_state.status.load(std::memory_order_acquire);
    const bool debugger_available = driver_bridge::attached_pid() != 0 ||
        debugger_state != debugger_engine::dbg_status_t::idle;
    if (debugger_available)
        std::snprintf(debugger, sizeof(debugger), "Debugger: %s",
            debugger_status_label(debugger_state));

    char network[96] = {};
    const bool network_available = network_view::g_state.cap_running.load(std::memory_order_acquire) ||
        network_view::g_state.cap_start_pending.load(std::memory_order_acquire) ||
        network_view::g_state.cap_stop_pending.load(std::memory_order_acquire);
    if (network_available)
        std::snprintf(network, sizeof(network), "Capture: %s",
            network_view::g_state.cap_running.load(std::memory_order_acquire) ? "live" : "changing");

    char mcp[96] = {};
    auto& mcp_server = get_local_mcp_server();
    const bool mcp_available = mcp_server.is_running();
    if (mcp_available)
        std::snprintf(mcp, sizeof(mcp), "MCP: localhost:%d", mcp_server.get_port());

    char driver[256] = {};
    const bool driver_loaded = driver_bridge::is_loaded();
    const bool driver_available = driver_loaded || driver_bridge::attached_pid() != 0;
    if (driver_available) {
        const std::string status_text = driver_bridge::status();
        std::snprintf(driver, sizeof(driver), "Driver: %s", status_text.c_str());
        if (driver_degraded_)
            std::snprintf(driver + std::strlen(driver), sizeof(driver) - std::strlen(driver),
                " (watchdog stale)");
    }

    char tasks[128] = {};
    if (status.running == 0 && status.queued == 0 && status.cancellation_requested == 0)
        std::snprintf(tasks, sizeof(tasks), "Tasks: idle");
    else
        std::snprintf(tasks, sizeof(tasks), "Tasks: %u running, %u queued, %u cancelling",
            status.running, status.queued, status.cancellation_requested);

    char diagnostics[96] = {};
    std::snprintf(diagnostics, sizeof(diagnostics), "Diagnostics: %u",
        status.unacknowledged_diagnostics);

    char frame[64] = {};
    const bool diagnostics_mode = g_sa_settings.ui_diagnostics_mode;
    if (diagnostics_mode) {
        std::snprintf(frame, sizeof(frame), "%.1f ms", frame_ms_);
        if (!frame_probe_->isActive())
            frame_probe_->start();
    } else if (frame_probe_->isActive()) {
        frame_probe_->stop();
    }

    struct update_t {
        AidaStatusSegmentId id;
        QString text;
        QString tooltip;
        int semantic;
        bool visible;
    };
    const update_t updates[10] = {
        {AidaStatusSegmentId::Target, QString::fromUtf8(target), target_tooltip, 0, true},
        {AidaStatusSegmentId::Location, QString::fromUtf8(location), location_tooltip, 5,
            location[0] != '\0'},
        {AidaStatusSegmentId::Workspace, workspace_label,
            QStringLiteral("Active workspace. Use the Workspace menu to switch, save, lock, or recover layouts."),
            0, true},
        {AidaStatusSegmentId::Debugger, QString::fromUtf8(debugger),
            QStringLiteral("Open the CPU debugger view"),
            debugger_state == debugger_engine::dbg_status_t::paused ? 2 : 6,
            debugger_available},
        {AidaStatusSegmentId::Network, QString::fromUtf8(network),
            QStringLiteral("Open network capture"), 6, network_available},
        {AidaStatusSegmentId::Mcp, QString::fromUtf8(mcp),
            QStringLiteral("Open MCP activity output"), 1, mcp_available},
        {AidaStatusSegmentId::Driver, QString::fromUtf8(driver),
            QStringLiteral("Open driver diagnostics"),
            driver_degraded_ ? 2 : 4, driver_available},
        {AidaStatusSegmentId::Tasks, QString::fromUtf8(tasks),
            QStringLiteral("Open Background Tasks"),
            status.running || status.queued ? 6 : 0, true},
        {AidaStatusSegmentId::Diagnostics, QString::fromUtf8(diagnostics),
            QStringLiteral("Open persistent diagnostics and recovery actions"),
            status.unacknowledged_diagnostics ? 3 : 0, true},
        {AidaStatusSegmentId::Frame, QString::fromUtf8(frame),
            QStringLiteral("Event-loop tick latency, shown because diagnostics mode is enabled"),
            0, diagnostics_mode},
    };
    for (const auto& update : updates) {
        const int index = static_cast<int>(update.id);
        AidaStatusSegment* segment = segments_[index];
        if (!segment)
            continue;
        wanted_visible_[index] = update.visible;
        if (update.visible)
            segment->setSegmentText(update.text, update.tooltip, update.semantic);
    }
    applyNarrowing();
}

void AidaStatusBar::tickDriverWatchdog()
{
    const std::uint64_t age = driver_bridge::driver_watchdog_age_ms();
    driver_degraded_ = age > 3ULL * 4000ULL;
}

void AidaStatusBar::resizeEvent(QResizeEvent* event)
{
    QStatusBar::resizeEvent(event);
    applyNarrowing();
    if (!raw_message_.isEmpty() && !currentMessage().isEmpty()) {
        const int remaining = message_timeout_ms_ > 0
            ? (std::max)(1, message_timeout_ms_ - static_cast<int>(message_clock_.elapsed()))
            : 0;
        QStatusBar::showMessage(elidedMessage(raw_message_), remaining);
    }
}

void AidaStatusBar::paintEvent(QPaintEvent* event)
{
    QStatusBar::paintEvent(event);
    QPainter p(this);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::tokens().border_subtle);
    p.drawRect(QRectF(0.0, 0.0, qreal(width()), qreal(theme::tokens().panel.border)));
}

void AidaStatusBar::showMessage(const QString& message, int timeout_ms)
{
    raw_message_ = message;
    message_timeout_ms_ = timeout_ms;
    message_clock_.start();
    QStatusBar::showMessage(elidedMessage(message), timeout_ms);
}

void AidaStatusBar::clearMessage()
{
    raw_message_.clear();
    QStatusBar::clearMessage();
}

QString AidaStatusBar::elidedMessage(const QString& message) const
{
    if (message.isEmpty())
        return message;
    const QFontMetricsF fm(font());
    return fm.elidedText(message, Qt::ElideRight, messageBudgetWidth());
}

int AidaStatusBar::messageBudgetWidth() const
{
    const auto& t = theme::tokens();
    int budget = width() - t.status_bar.padding_x * 2;
    for (int i = 0; i < 10; ++i) {
        const AidaStatusSegment* segment = segments_[i];
        if (segment && permanent_[i] && segment->isVisible())
            budget -= segment->sizeHint().width() + t.status_bar.item_gap;
    }
    return (std::max)(budget, static_cast<int>(t.shell.min_panel_w));
}

void AidaStatusBar::applyNarrowing()
{
    const auto& t = theme::tokens();
    const int available = width();
    bool shown[10] = {};
    if (available < static_cast<int>(t.shell.min_panel_w)) {
        for (int i = 0; i < 10; ++i)
            shown[i] = wanted_visible_[i] && segments_[i] != nullptr;
    } else {
        static const int hide_order[5] = {
            static_cast<int>(AidaStatusSegmentId::Driver),
            static_cast<int>(AidaStatusSegmentId::Mcp),
            static_cast<int>(AidaStatusSegmentId::Network),
            static_cast<int>(AidaStatusSegmentId::Debugger),
            static_cast<int>(AidaStatusSegmentId::Frame),
        };
        int total = 0;
        for (int i = 0; i < 10; ++i) {
            shown[i] = wanted_visible_[i] && segments_[i] != nullptr;
            if (shown[i])
                total += segments_[i]->sizeHint().width();
        }
        if (total > available) {
            for (const int index : hide_order) {
                if (!shown[index])
                    continue;
                total -= segments_[index]->sizeHint().width();
                shown[index] = false;
                if (total <= available)
                    break;
            }
        }
    }
    int last_visible = -1;
    for (int i = 0; i < 10; ++i)
        if (shown[i])
            last_visible = i;
    for (int i = 0; i < 10; ++i) {
        if (segments_[i]) {
            segments_[i]->setVisible(shown[i]);
            segments_[i]->setShowsSeparator(shown[i] && i != last_visible);
        }
    }
}

}
