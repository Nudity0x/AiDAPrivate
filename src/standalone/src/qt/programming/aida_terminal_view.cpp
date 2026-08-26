#include "qt/programming/aida_terminal_view.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyleHints>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidgetAction>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <string>
#include <utility>

#include "core/settings/standalone_settings.hpp"
#include "core/settings/settings_persistence_service.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/programming/aida_output_pane.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::programming {
namespace {

QPointer<AidaTerminalController> g_terminal_controller;

constexpr int k_terminal_reap_ms = 250;
constexpr int k_terminal_integrity_ms = 100;

qreal terminal_pad_x() {
    return static_cast<qreal>(theme::tokens().spacing.sm);
}

qreal terminal_pad_y() {
    return static_cast<qreal>(theme::tokens().spacing.xs);
}

double entrance_seconds() {
    return theme::tokens().motion.instant / 1000.0;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size)
        return {};
    return result;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size) != size)
        return {};
    return result;
}

void terminal_lock_busy(const char* operation, std::uint64_t session_id) {
    static std::atomic<unsigned long long> last_log_ms{0};
    static std::atomic<unsigned long long> busy_count{0};
    const unsigned long long now = aida::shell_platform::tick_ms();
    const unsigned long long count = busy_count.fetch_add(1, std::memory_order_acq_rel) + 1ULL;
    unsigned long long last = last_log_ms.load(std::memory_order_acquire);
    if (count != 1ULL && now - last < 500ULL)
        return;
    if (count != 1ULL && !last_log_ms.compare_exchange_strong(last, now, std::memory_order_acq_rel))
        return;
    diag::log_tagged_fmt("ui",
        "TERMINAL_VIEW_LOCK_BUSY op=%s session=%llu busy_count=%llu tid=%lu",
        operation ? operation : "<null>", static_cast<unsigned long long>(session_id), count,
        static_cast<unsigned long>(aida::shell_platform::thread_id()));
}

QColor cell_color_from(std::uint32_t packed) {
    return QColor(static_cast<int>(packed & 0xFF),
        static_cast<int>((packed >> 8) & 0xFF),
        static_cast<int>((packed >> 16) & 0xFF),
        static_cast<int>((packed >> 24) & 0xFF));
}

QColor with_alpha_factor(QColor color, qreal factor) {
    const qreal clamped = factor < 0.0 ? 0.0 : (factor > 1.0 ? 1.0 : factor);
    color.setAlphaF(color.alphaF() * clamped);
    return color;
}

QFont resolve_terminal_font() {
    QFont font = theme::fonts::codeRegular();
    if (QFontInfo(font).fixedPitch())
        return font;
    diag::log_tagged_fmt("terminal", "terminal_font_not_fixed_pitch family=%s retrying",
        font.family().toUtf8().constData());
    const QStringList fallbacks = {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas")};
    for (const QString& family : fallbacks) {
        QFont candidate(family);
        candidate.setPointSizeF(font.pointSizeF());
        if (QFontInfo(candidate).fixedPitch())
            return candidate;
    }
    diag::log_tagged("terminal", "terminal_font_fallback_to_system_fixed");
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

} 

AidaTerminalViewport::AidaTerminalViewport(aida::terminal::TerminalSession* session,
                                           AidaTerminalController* controller, QWidget* parent)
    : QAbstractScrollArea(parent), controller_(controller), session_(session) {
    session_id_ = session_ ? session_->id : 0;
    setObjectName(QStringLiteral("aida.view.terminal.viewport.") +
        QString::number(static_cast<qulonglong>(session_id_)));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    verticalScrollBar()->setSingleStep(3);
    reduced_motion_ = theme::AidaMotion::reducedMotion();
    clock_.start();
    connect(QGuiApplication::styleHints(), &QStyleHints::cursorFlashTimeChanged,
            this, [this](int) {
        if (caret_timer_) {
            stopCaretTimer();
            caret_visible_ = true;
            if (hasFocus() && snapshot_.alive && !reduced_motion_)
                startCaretTimer();
        }
        update();
    });
    updateMetrics();
    refreshSnapshot();
}

AidaTerminalViewport::~AidaTerminalViewport() {
    if (controller_)
        controller_->noteViewportRemoved(session_id_);
}

void AidaTerminalViewport::detach() {
    session_ = nullptr;
    snapshot_ = {};
    stopCaretTimer();
    update();
}

void AidaTerminalViewport::setReducedMotion(bool reduced) {
    reduced_motion_ = reduced;
}

void AidaTerminalViewport::updateMetrics() {
    font_ = resolve_terminal_font();
    bold_font_ = font_;
    bold_font_.setBold(true);
    const QFontMetricsF fm(font_);
    char_w_ = fm.horizontalAdvance(QLatin1Char('M'));
    line_h_ = fm.lineSpacing();
    ascent_ = fm.ascent();
    if (char_w_ < 1.0) char_w_ = 8.0;
    if (line_h_ < 1.0) line_h_ = 16.0;
}

int AidaTerminalViewport::visibleRows() const {
    const qreal available = viewport()->height() - terminal_pad_y() * 2.0;
    return (std::max)(1, static_cast<int>(available / line_h_));
}

void AidaTerminalViewport::applyScrollState() {
    auto* bar = verticalScrollBar();
    const int maximum = (std::max)(0, snapshot_.total_lines - visibleRows());
    if (bar->maximum() != maximum) {
        bar->setMaximum(maximum);
        bar->setPageStep(visibleRows());
    }
    if (session_ && session_->auto_follow) {
        bar->setValue(maximum);
    } else if (bar->value() > maximum) {
        bar->setValue(maximum);
    }
}

void AidaTerminalViewport::refreshSnapshot() {
    if (!session_)
        return;
    std::unique_lock<std::mutex> lock(session_->buffer_mtx, std::try_to_lock);
    if (!lock.owns_lock()) {
        terminal_lock_busy("snapshot", session_id_);
        return;
    }
    auto& session = *session_;
    if (session.scroll_to_bottom) {
        session.auto_follow = true;
        session.scroll_to_bottom = false;
    }
    const int total = static_cast<int>(session.lines.size());
    const int vis = visibleRows();
    const int max_scroll = (std::max)(0, total - vis);
    if (session.auto_follow) {
        session.scroll_y = static_cast<float>(max_scroll);
    } else if (session.scroll_y > static_cast<float>(max_scroll)) {
        session.scroll_y = static_cast<float>(max_scroll);
    }
    const int first = static_cast<int>(session.scroll_y);

    const int raw_delta = total - prev_line_count_;
    if (raw_delta >= 0) {
        const bool burst = raw_delta > 100;
        if (burst) {
            entrance_times_.clear();
        } else if (raw_delta > 0 && !reduced_motion_) {
            const double now = clock_.elapsed() / 1000.0;
            for (int index = 0; index < raw_delta; ++index)
                entrance_times_.push_back(now);
            while (entrance_times_.size() > total)
                entrance_times_.removeFirst();
        }
    } else {
        int popped = -raw_delta;
        while (popped-- > 0 && !entrance_times_.isEmpty())
            entrance_times_.removeFirst();
    }
    prev_line_count_ = total;
    session.prev_line_count = total;

    snapshot_.generation = session.buffer_generation.load(std::memory_order_acquire);
    snapshot_.total_lines = total;
    snapshot_.first_line = first;
    snapshot_.cursor_row = session.cursor_row;
    snapshot_.cursor_col = session.cursor_col;
    snapshot_.alive = session.alive.load(std::memory_order_acquire);
    snapshot_.exit_code = session.exit_code.load(std::memory_order_acquire);
    snapshot_.cols = session.cols;
    snapshot_.active_match = session.active_search_match;
    snapshot_.rows.clear();
    const int rows_to_copy = (std::min)(vis, total - first);
    snapshot_.rows.reserve(rows_to_copy > 0 ? rows_to_copy : 0);
    for (int row = first; row < first + rows_to_copy; ++row) {
        const auto& source = session.lines[static_cast<std::size_t>(row)];
        snapshot_.rows.push_back(QVector<aida::terminal::Cell>(source.begin(), source.end()));
    }
    snapshot_.matches.clear();
    snapshot_.match_global_indices.clear();
    for (int match_index = 0; match_index < static_cast<int>(session.search_matches.size());
         ++match_index) {
        const auto& match = session.search_matches[static_cast<std::size_t>(match_index)];
        if (match.line >= first && match.line < first + vis) {
            snapshot_.matches.push_back(match);
            snapshot_.match_global_indices.push_back(match_index);
        }
    }
    {
        std::unique_lock<std::mutex> input_lock(session.input_mtx, std::try_to_lock);
        if (input_lock.owns_lock())
            snapshot_.input_error = session.input_error;
    }
    if (session.bell_pending.exchange(false, std::memory_order_acq_rel))
        startBell();
    lock.unlock();
    applyScrollState();
    update();
}

void AidaTerminalViewport::integrityCheck() {
    if (!session_)
        return;
    const quint64 live = session_->buffer_generation.load(std::memory_order_acquire);
    if (live != snapshot_.generation)
        refreshSnapshot();
}

void AidaTerminalViewport::startBell() {
    if (reduced_motion_)
        return;
    if (!bell_anim_) {
        bell_anim_ = new QVariantAnimation(this);
        bell_anim_->setDuration(theme::tokens().motion.emphasized);
        bell_anim_->setStartValue(1.0);
        bell_anim_->setEndValue(0.0);
        bell_anim_->setEasingCurve(theme::easingFor(theme::Ease::OutCubic));
        connect(bell_anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant&) {
            update();
        });
    }
    bell_anim_->stop();
    bell_anim_->start();
}

void AidaTerminalViewport::startCaretTimer() {
    if (reduced_motion_ || caret_timer_)
        return;
    const int flash = QGuiApplication::styleHints()->cursorFlashTime();
    if (flash < 2)
        return;
    caret_visible_ = true;
    caret_timer_ = new QTimer(this);
    caret_timer_->setInterval(flash / 2);
    connect(caret_timer_, &QTimer::timeout, this, [this] {
        caret_visible_ = !caret_visible_;
        update();
    });
    caret_timer_->start();
}

void AidaTerminalViewport::stopCaretTimer() {
    if (caret_timer_) {
        caret_timer_->stop();
        caret_timer_->deleteLater();
        caret_timer_ = nullptr;
    }
    caret_visible_ = true;
}

qreal AidaTerminalViewport::caretAlpha() const {
    if (!hasFocus())
        return 0.30;
    if (reduced_motion_ || caret_visible_)
        return 1.0;
    return 0.0;
}

void AidaTerminalViewport::paintEvent(QPaintEvent* event) {
    static_cast<void>(event);
    QPainter painter(viewport());
    const auto& tokens = theme::tokens();
    const qreal pad_x = terminal_pad_x();
    const qreal pad_y = terminal_pad_y();
    painter.fillRect(rect(), with_alpha_factor(tokens.bg_base, 0.9));
    if (!session_)
        return;

    painter.setFont(font_);
    const int vis = visibleRows();
    const int first = snapshot_.first_line;
    const double now = clock_.elapsed() / 1000.0;
    const int entrance_offset = snapshot_.total_lines - static_cast<int>(entrance_times_.size());

    int sel_r0 = 0, sel_c0 = 0, sel_r1 = 0, sel_c1 = 0;
    if (has_range_selection_) {
        sel_r0 = sel_anchor_row_; sel_c0 = sel_anchor_col_;
        sel_r1 = sel_caret_row_; sel_c1 = sel_caret_col_;
        if (sel_r1 < sel_r0 || (sel_r1 == sel_r0 && sel_c1 < sel_c0)) {
            std::swap(sel_r0, sel_r1);
            std::swap(sel_c0, sel_c1);
        }
    }

    for (int vi = 0; vi < vis && vi < snapshot_.rows.size(); ++vi) {
        const auto& row = snapshot_.rows[vi];
        const int line_index = first + vi;
        qreal row_alpha = 1.0;
        qreal row_y_off = 0.0;
        if (!reduced_motion_ && line_index >= entrance_offset && line_index >= 0) {
            const int entrance_index = line_index - entrance_offset;
            if (entrance_index >= 0 && entrance_index < entrance_times_.size()) {
                const double age = now - entrance_times_[entrance_index];
                const double entrance = entrance_seconds();
                if (age >= 0.0 && age < entrance) {
                    qreal t01 = static_cast<qreal>(age / entrance);
                    const qreal eased = 1.0 - (1.0 - t01) * (1.0 - t01) * (1.0 - t01);
                    row_alpha = eased;
                    row_y_off = (1.0 - eased) * (line_h_ * 0.5);
                }
            }
        }
        const qreal y = pad_y + static_cast<qreal>(vi) * line_h_ + row_y_off;

        if (has_range_selection_ && line_index >= sel_r0 && line_index <= sel_r1) {
            const int row_cells = static_cast<int>(row.size());
            const int cell_from = line_index == sel_r0 ? sel_c0 : 0;
            const int cell_to = line_index == sel_r1
                ? (std::min)(sel_c1, row_cells) : row_cells;
            if (cell_to > cell_from) {
                painter.fillRect(QRectF(pad_x + static_cast<qreal>(cell_from) * char_w_, y,
                    static_cast<qreal>(cell_to - cell_from) * char_w_, line_h_),
                    tokens.selection);
            }
        }

        for (int match_row = 0; match_row < snapshot_.matches.size(); ++match_row) {
            const auto& match = snapshot_.matches[match_row];
            if (match.line != line_index)
                continue;
            const QColor color = snapshot_.match_global_indices[match_row] == snapshot_.active_match
                ? with_alpha_factor(tokens.accent, 0.55)
                : with_alpha_factor(tokens.warning, 0.30);
            const qreal left = pad_x + static_cast<qreal>(match.column) * char_w_;
            const qreal right = left + static_cast<qreal>(match.length) * char_w_;
            painter.fillRect(QRectF(left, y, right - left, line_h_), color);
        }

        const int rendered_columns = (std::min)(static_cast<int>(row.size()),
            (std::max)(0, snapshot_.cols));
        int column = 0;
        while (column < rendered_columns) {
            const auto& cell = row[column];
            int run_end = column + 1;
            while (run_end < rendered_columns &&
                   row[run_end].fg == cell.fg && row[run_end].bg == cell.bg &&
                   row[run_end].bold == cell.bold)
                ++run_end;
            const qreal x = pad_x + static_cast<qreal>(column) * char_w_;
            const qreal run_width = static_cast<qreal>(run_end - column) * char_w_;
            const QColor bg = cell_color_from(cell.bg);
            if (bg.alpha() != 0)
                painter.fillRect(QRectF(x, y, run_width, line_h_),
                    with_alpha_factor(bg, row_alpha));
            QString text;
            text.reserve(run_end - column);
            for (int index = column; index < run_end; ++index)
                text.push_back(row[index].ch > ' ' ? QLatin1Char(row[index].ch) : QLatin1Char(' '));
            const QColor fg = with_alpha_factor(cell_color_from(cell.fg), row_alpha);
            painter.setPen(fg);
            painter.setFont(cell.bold ? bold_font_ : font_);
            painter.drawText(QPointF(x, y + ascent_), text);
            column = run_end;
        }
    }

    if (snapshot_.alive && snapshot_.cursor_row >= first &&
        snapshot_.cursor_row < first + vis) {
        const qreal cx = pad_x + static_cast<qreal>(snapshot_.cursor_col) * char_w_;
        const qreal cy = pad_y +
            static_cast<qreal>(snapshot_.cursor_row - first) * line_h_;
        const qreal blink = caretAlpha();
        painter.fillRect(QRectF(cx, cy, char_w_, line_h_),
            with_alpha_factor(tokens.accent, blink));
        if (hasFocus() && blink > 0.0) {
            painter.fillRect(QRectF(cx - 1.0, cy - 1.0, char_w_ + 2.0, line_h_ + 2.0),
                with_alpha_factor(tokens.accent_glow, blink * 0.7));
        }
    }

    if (bell_anim_ && bell_anim_->state() == QAbstractAnimation::Running) {
        const qreal value = bell_anim_->currentValue().toReal();
        if (value > 0.001) {
            const QColor border = with_alpha_factor(tokens.accent, value);
            painter.setPen(QPen(border, 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0));
        }
    }

    if (select_all_) {
        painter.setPen(QPen(with_alpha_factor(tokens.accent, 0.9), 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0));
    }

    if (hasFocus()) {
        const qreal ring = static_cast<qreal>(tokens.control.focus_ring);
        const qreal inset = ring * 0.5;
        painter.setPen(QPen(tokens.border_focus, ring));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset));
    }
}

void AidaTerminalViewport::scrollContentsBy(int dx, int dy) {
    static_cast<void>(dx);
    static_cast<void>(dy);
    if (!session_)
        return;
    {
        std::unique_lock<std::mutex> lock(session_->buffer_mtx, std::try_to_lock);
        if (lock.owns_lock()) {
            session_->scroll_y = static_cast<float>(verticalScrollBar()->value());
            session_->auto_follow = verticalScrollBar()->value() >=
                verticalScrollBar()->maximum();
        }
    }
    refreshSnapshot();
}

void AidaTerminalViewport::propagateResize() {
    if (!session_)
        return;
    const int cols = (std::max)(1,
        static_cast<int>((viewport()->width() - terminal_pad_x() * 2.0) / char_w_));
    const int rows = visibleRows();
    {
        std::unique_lock<std::mutex> lock(session_->buffer_mtx, std::try_to_lock);
        if (!lock.owns_lock()) {
            terminal_lock_busy("resize", session_id_);
            if (!resize_retry_pending_) {
                resize_retry_pending_ = true;
                const std::uint64_t expected_session = session_id_;
                QTimer::singleShot(theme::tokens().motion.instant, this, [this, expected_session] {
                    resize_retry_pending_ = false;
                    if (session_ && session_id_ == expected_session)
                        propagateResize();
                });
            }
            return;
        }
        if (cols != session_->cols || rows != session_->rows_vis)
            aida::terminal::resize_pty(*session_, cols, rows);
    }
}

void AidaTerminalViewport::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    updateMetrics();
    propagateResize();
    refreshSnapshot();
}

void AidaTerminalViewport::wheelEvent(QWheelEvent* event) {
    if (!session_) {
        event->ignore();
        return;
    }
    const int notches = event->angleDelta().y() / 120;
    if (notches != 0) {
        auto* bar = verticalScrollBar();
        const int next = bar->value() - notches * 3;
        bar->setValue((std::max)(0, (std::min)(next, bar->maximum())));
    }
    event->accept();
}

void AidaTerminalViewport::sendBytes(const char* data, std::size_t length) {
    if (!session_)
        return;
    aida::terminal::send_input(*session_, data, length);
}

void AidaTerminalViewport::copyAllText() {
    if (controller_)
        static_cast<void>(controller_->terminalCopyAll());
}

void AidaTerminalViewport::keyPressEvent(QKeyEvent* event) {
    if (!session_) {
        event->ignore();
        return;
    }
    auto& session = *session_;
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const bool alt = event->modifiers().testFlag(Qt::AltModifier);
    if (ctrl && event->key() == Qt::Key_A) {
        clearRangeSelection();
        select_all_ = true;
        update();
        event->accept();
        return;
    }
    if (ctrl && event->key() == Qt::Key_C) {
        if (has_range_selection_) {
            copyRangeSelection();
            clearRangeSelection();
            update();
        } else if (select_all_) {
            copyAllText();
            select_all_ = false;
            update();
        } else {
            sendBytes("\x03", 1);
        }
        event->accept();
        return;
    }
    if (!ctrl && !alt) {
        const QByteArray text = event->text().toUtf8();
        if (!text.isEmpty()) {
            sendBytes(text.constData(), static_cast<std::size_t>(text.size()));
            if (select_all_ || has_range_selection_) {
                select_all_ = false;
                has_range_selection_ = false;
                selecting_ = false;
                update();
            }
        }
    }
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter: sendBytes("\r", 1); break;
    case Qt::Key_Backspace: sendBytes("\x7f", 1); break;
    case Qt::Key_Tab: sendBytes("\t", 1); break;
    case Qt::Key_Escape: sendBytes("\x1b", 1); break;
    case Qt::Key_Up: sendBytes("\x1b[A", 3); break;
    case Qt::Key_Down: sendBytes("\x1b[B", 3); break;
    case Qt::Key_Right: sendBytes("\x1b[C", 3); break;
    case Qt::Key_Left: sendBytes("\x1b[D", 3); break;
    case Qt::Key_Home: sendBytes("\x1b[H", 3); break;
    case Qt::Key_End: sendBytes("\x1b[F", 3); break;
    case Qt::Key_Delete: sendBytes("\x1b[3~", 4); break;
    case Qt::Key_D: if (ctrl) sendBytes("\x04", 1); break;
    case Qt::Key_Z: if (ctrl) sendBytes("\x1a", 1); break;
    default: break;
    }
    event->accept();
}

void AidaTerminalViewport::inputMethodEvent(QInputMethodEvent* event) {
    const QByteArray commit = event->commitString().toUtf8();
    if (!commit.isEmpty())
        sendBytes(commit.constData(), static_cast<std::size_t>(commit.size()));
    event->accept();
}

QVariant AidaTerminalViewport::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImCursorRectangle)
        return QRectF(terminal_pad_x() + snapshot_.cursor_col * char_w_,
            terminal_pad_y() + (snapshot_.cursor_row - snapshot_.first_line) * line_h_,
            char_w_, line_h_).toRect();
    return QAbstractScrollArea::inputMethodQuery(query);
}

bool AidaTerminalViewport::mapToCell(const QPointF& pos, int& row, int& col) const {
    if (!session_ || char_w_ < 1.0 || line_h_ < 1.0)
        return false;
    const int total = (std::max)(1, snapshot_.total_lines);
    row = snapshot_.first_line +
        static_cast<int>((pos.y() - terminal_pad_y()) / line_h_);
    row = (std::max)(0, (std::min)(row, total - 1));
    col = static_cast<int>((pos.x() - terminal_pad_x()) / char_w_);
    col = (std::max)(0, (std::min)(col, (std::max)(0, snapshot_.cols)));
    return true;
}

void AidaTerminalViewport::clearRangeSelection() {
    has_range_selection_ = false;
    selecting_ = false;
}

void AidaTerminalViewport::copyRangeSelection() {
    if (!session_ || !has_range_selection_)
        return;
    int r0 = sel_anchor_row_, c0 = sel_anchor_col_;
    int r1 = sel_caret_row_, c1 = sel_caret_col_;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }
    std::unique_lock<std::mutex> lock(session_->buffer_mtx, std::try_to_lock);
    if (!lock.owns_lock()) {
        terminal_lock_busy("selection_copy", session_id_);
        return;
    }
    const int total = static_cast<int>(session_->lines.size());
    if (r0 >= total)
        return;
    r1 = (std::min)(r1, total - 1);
    std::string text;
    for (int row_index = r0; row_index <= r1; ++row_index) {
        const auto& source = session_->lines[static_cast<std::size_t>(row_index)];
        const int row_cells = static_cast<int>(source.size());
        const int from = row_index == r0 ? (std::min)(c0, row_cells) : 0;
        const int to = row_index == r1 ? (std::min)(c1, row_cells) : row_cells;
        std::string line;
        line.reserve(static_cast<std::size_t>((std::max)(0, to - from)));
        for (int cell_index = from; cell_index < to; ++cell_index)
            line.push_back(source[static_cast<std::size_t>(cell_index)].ch);
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        text += line;
        if (row_index != r1)
            text += '\n';
    }
    lock.unlock();
    if (!text.empty())
        clipboard::set_text(QString::fromStdString(text));
}

void AidaTerminalViewport::mousePressEvent(QMouseEvent* event) {
    if (select_all_ || has_range_selection_) {
        select_all_ = false;
        has_range_selection_ = false;
        update();
    }
    if (event->button() == Qt::LeftButton) {
        int row = 0, col = 0;
        if (mapToCell(event->position(), row, col)) {
            selecting_ = true;
            has_range_selection_ = false;
            sel_anchor_row_ = sel_caret_row_ = row;
            sel_anchor_col_ = sel_caret_col_ = col;
        }
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void AidaTerminalViewport::mouseMoveEvent(QMouseEvent* event) {
    if (selecting_ && event->buttons().testFlag(Qt::LeftButton)) {
        int row = 0, col = 0;
        if (mapToCell(event->position(), row, col)) {
            sel_caret_row_ = row;
            sel_caret_col_ = col;
            const bool ranged = sel_caret_row_ != sel_anchor_row_ ||
                sel_caret_col_ != sel_anchor_col_;
            if (ranged != has_range_selection_ || ranged) {
                has_range_selection_ = ranged;
                update();
            }
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void AidaTerminalViewport::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && selecting_) {
        selecting_ = false;
        if (!has_range_selection_)
            update();
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void AidaTerminalViewport::focusInEvent(QFocusEvent* event) {
    if (controller_)
        controller_->noteViewportFocused(session_id_);
    if (snapshot_.alive && !reduced_motion_)
        startCaretTimer();
    update();
    QAbstractScrollArea::focusInEvent(event);
}

void AidaTerminalViewport::focusOutEvent(QFocusEvent* event) {
    stopCaretTimer();
    update();
    QAbstractScrollArea::focusOutEvent(event);
}

void AidaTerminalViewport::changeEvent(QEvent* event) {
    if (event->type() == QEvent::FontChange ||
        event->type() == QEvent::ApplicationFontChange) {
        updateMetrics();
        refreshSnapshot();
    }
    QAbstractScrollArea::changeEvent(event);
}

void AidaTerminalViewport::selectAll() {
    clearRangeSelection();
    select_all_ = true;
    update();
}

void AidaTerminalViewport::clearSelectAll() {
    if (select_all_ || has_range_selection_) {
        select_all_ = false;
        clearRangeSelection();
        update();
    }
}

AidaTerminalSearchBar::AidaTerminalSearchBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.terminal.search"));
    const auto& tokens = theme::tokens();
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(tokens.spacing.xs, tokens.spacing.xxs,
        tokens.spacing.xs, tokens.spacing.xxs);
    row->setSpacing(tokens.spacing.xs);
    query_ = new QLineEdit(this);
    query_->setObjectName(QStringLiteral("aida.terminal.search.query"));
    query_->setPlaceholderText(QStringLiteral("Search terminal output"));
    query_->setToolTip(QStringLiteral(
        "Search the scrollback buffer (Enter = next, Shift+Enter = previous, Esc = close)"));
    query_->setClearButtonEnabled(true);
    query_->installEventFilter(this);
    row->addWidget(query_, 1);
    counter_ = new QLabel(QStringLiteral("0/0"), this);
    counter_->setObjectName(QStringLiteral("aida.terminal.search.counter"));
    counter_->setAlignment(Qt::AlignCenter);
    counter_->setMinimumWidth(counter_->fontMetrics().horizontalAdvance(
        QStringLiteral("0000/0000")));
    counter_->setToolTip(QStringLiteral("Active match / total matches"));
    row->addWidget(counter_);
    auto* previous = new QToolButton(this);
    previous->setObjectName(QStringLiteral("aida.terminal.search.previous"));
    previous->setText(QStringLiteral("Previous"));
    previous->setToolTip(QStringLiteral("Jump to the previous match (Shift+Enter)"));
    connect(previous, &QToolButton::clicked, this, &AidaTerminalSearchBar::previousRequested);
    row->addWidget(previous);
    auto* next = new QToolButton(this);
    next->setObjectName(QStringLiteral("aida.terminal.search.next"));
    next->setText(QStringLiteral("Next"));
    next->setToolTip(QStringLiteral("Jump to the next match (Enter)"));
    connect(next, &QToolButton::clicked, this, &AidaTerminalSearchBar::nextRequested);
    row->addWidget(next);
    auto* close = new QToolButton(this);
    close->setObjectName(QStringLiteral("aida.terminal.search.close"));
    close->setText(QStringLiteral("Close"));
    close->setToolTip(QStringLiteral("Close the search bar (Esc)"));
    connect(close, &QToolButton::clicked, this, &AidaTerminalSearchBar::closeRequested);
    row->addWidget(close);
    connect(query_, &QLineEdit::textChanged, this, &AidaTerminalSearchBar::queryChanged);
    connect(query_, &QLineEdit::returnPressed, this, [this] {
        if (QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
            Q_EMIT previousRequested();
        else
            Q_EMIT nextRequested();
    });
}

bool AidaTerminalSearchBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == query_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            Q_EMIT closeRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AidaTerminalSearchBar::setCounter(int active, int total) {
    counter_->setText(QStringLiteral("%1/%2").arg(active).arg(total));
}

void AidaTerminalSearchBar::setQuery(const QString& text) {
    query_->setText(text);
}

QString AidaTerminalSearchBar::query() const {
    return query_->text();
}

void AidaTerminalSearchBar::focusQuery() {
    query_->setFocus();
    query_->selectAll();
}

AidaTerminalController& AidaTerminalController::instance() {
    if (!g_terminal_controller)
        g_terminal_controller = new AidaTerminalController();
    return *g_terminal_controller;
}

bool AidaTerminalController::exists() noexcept {
    return g_terminal_controller != nullptr;
}

AidaTerminalController::AidaTerminalController(QObject* parent) : QObject(parent) {
    manager_.output_notify = [this](aida::terminal::TerminalSession& session) {
        const std::uint64_t id = session.id;
        QMetaObject::invokeMethod(this, [this, id] { onSessionOutput(id); },
            Qt::QueuedConnection);
    };
    reap_timer_ = new QTimer(this);
    reap_timer_->setInterval(k_terminal_reap_ms);
    reap_timer_->setTimerType(Qt::CoarseTimer);
    connect(reap_timer_, &QTimer::timeout, this, &AidaTerminalController::onReapTick);
    reap_timer_->start();
    integrity_timer_ = new QTimer(this);
    integrity_timer_->setInterval(k_terminal_integrity_ms);
    integrity_timer_->setTimerType(Qt::CoarseTimer);
    connect(integrity_timer_, &QTimer::timeout, this, &AidaTerminalController::onIntegrityTick);
    integrity_timer_->start();
}

void AidaTerminalController::install(docking::AidaDockHost* host) {
    host_ = host;
    if (qApp) {
        connect(qApp, &QCoreApplication::aboutToQuit, this,
            &AidaTerminalController::shutdown, Qt::DirectConnection);
    }
}

void AidaTerminalController::shutdown() {
    if (reap_timer_)
        reap_timer_->stop();
    if (integrity_timer_)
        integrity_timer_->stop();
    manager_.shutdown();
}

const std::vector<aida::terminal::profile_t>& AidaTerminalController::profiles() {
    ensureProfiles();
    return profiles_;
}

void AidaTerminalController::setProfileCwd(const std::string& cwd) {
    cwd_ = cwd;
}

aida::terminal::TerminalSession* AidaTerminalController::sessionById(std::uint64_t id) const {
    for (auto* session : manager_.sessions)
        if (session && session->id == id)
            return session;
    return nullptr;
}

aida::terminal::TerminalSession* AidaTerminalController::focusedSession() {
    if (focused_session_id_ != 0) {
        if (auto* session = sessionById(focused_session_id_))
            return session;
    }
    return manager_.current();
}

void AidaTerminalController::noteViewportFocused(std::uint64_t session_id) {
    focused_session_id_ = session_id;
}

void AidaTerminalController::noteViewportRemoved(std::uint64_t session_id) {
    if (focused_session_id_ == session_id)
        focused_session_id_ = 0;
}

void AidaTerminalController::ensureProfiles() {
    if (!profiles_.empty())
        return;
    profiles_ = aida::terminal::available_profiles(g_sa_settings.terminal_shell);
    if (profiles_.empty())
        return;
    const auto selected = std::find_if(profiles_.begin(), profiles_.end(),
        [](const aida::terminal::profile_t& profile) {
            return profile.id == g_sa_settings.terminal_profile_id;
        });
    profile_index_ = selected == profiles_.end() ? 0 :
        static_cast<int>(std::distance(profiles_.begin(), selected));
    cwd_ = g_sa_settings.terminal_default_cwd.substr(0, 1023);
}

aida::terminal::TerminalSession* AidaTerminalController::createSelectedTerminal() {
    ensureProfiles();
    if (profile_index_ < 0 || profile_index_ >= static_cast<int>(profiles_.size())) {
        start_error_ = "No available terminal profile is selected";
        return nullptr;
    }
    const auto& profile = profiles_[static_cast<std::size_t>(profile_index_)];
    const std::wstring cwd = widen(cwd_);
    auto* session = manager_.create_terminal(profile.command.c_str(),
        cwd.empty() ? nullptr : cwd.c_str(), profile.id.c_str(), profile.label.c_str());
    if (!session) {
        start_error_ = manager_.last_error;
        Q_EMIT sessionsChanged();
        return nullptr;
    }
    session->max_lines = (std::clamp)(g_sa_settings.terminal_scrollback, 1000, 100000);
    start_error_.clear();
    Q_EMIT sessionsChanged();
    return session;
}

void AidaTerminalController::createFromProfile(int profile_index, const std::string& cwd) {
    ensureProfiles();
    if (profile_index >= 0 && profile_index < static_cast<int>(profiles_.size()))
        profile_index_ = profile_index;
    cwd_ = cwd;
    if (createSelectedTerminal())
        persistTerminalState();
}

void AidaTerminalController::closeSessionAt(int index) {
    auto& sessions = manager_.sessions;
    if (index < 0 || index >= static_cast<int>(sessions.size()))
        return;
    auto* session = sessions[static_cast<std::size_t>(index)];
    if (session)
        Q_EMIT sessionClosing(session->id);
    manager_.close_terminal(index);
    start_attempted_ = true;
    persistTerminalState();
    Q_EMIT sessionsChanged();
}

void AidaTerminalController::selectSessionAt(int index) {
    auto& sessions = manager_.sessions;
    if (index < 0 || index >= static_cast<int>(sessions.size()))
        return;
    if (manager_.active_tab == index)
        return;
    manager_.active_tab = index;
    persistTerminalState();
    Q_EMIT sessionsChanged();
}

void AidaTerminalController::retryStart() {
    start_attempted_ = true;
    if (createSelectedTerminal())
        persistTerminalState();
}

host::operation_result_t AidaTerminalController::terminalNew() {
    if (!createSelectedTerminal())
        return {false, start_error_};
    start_attempted_ = true;
    persistTerminalState();
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalNewAt(const std::string& working_directory) {
    if (working_directory.empty())
        return {false, "Select a workspace directory first"};
    if (working_directory.size() >= 1024)
        return {false, "The terminal working directory exceeds the 1023-byte UTF-8 limit"};
    if (widen(working_directory).empty())
        return {false, "The terminal working directory is not valid UTF-8"};
    cwd_ = working_directory;
    if (!createSelectedTerminal())
        return {false, start_error_};
    start_attempted_ = true;
    persistTerminalState();
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalClose() {
    auto* focused = focusedSession();
    if (!focused)
        return {false, "There is no active terminal session"};
    int index = -1;
    for (int i = 0; i < static_cast<int>(manager_.sessions.size()); ++i) {
        if (manager_.sessions[static_cast<std::size_t>(i)] == focused) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return {false, "There is no active terminal session"};
    closeSessionAt(index);
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalRestart() {
    auto* focused = focusedSession();
    if (!focused)
        return {false, "There is no active terminal session"};
    int index = -1;
    for (int i = 0; i < static_cast<int>(manager_.sessions.size()); ++i) {
        if (manager_.sessions[static_cast<std::size_t>(i)] == focused) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return {false, "There is no active terminal session"};
    Q_EMIT sessionClosing(focused->id);
    if (!manager_.restart_terminal(index))
        return {false, manager_.last_error.empty()
            ? "The terminal session could not be restarted" : manager_.last_error};
    Q_EMIT sessionsChanged();
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalNext() {
    if (!manager_.cycle(1))
        return {false, "There are no terminal sessions"};
    persistTerminalState();
    Q_EMIT sessionsChanged();
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalPrevious() {
    if (!manager_.cycle(-1))
        return {false, "There are no terminal sessions"};
    persistTerminalState();
    Q_EMIT sessionsChanged();
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalSplit(aida::terminal::split_mode_t mode) {
    if (!manager_.current())
        return {false, "There is no active terminal session"};
    if (manager_.sessions.size() < 2 && !createSelectedTerminal())
        return {false, start_error_};
    if (!manager_.set_split(mode))
        return {false, "A second terminal session is required for a split"};
    persistTerminalState();
    Q_EMIT sessionsChanged();
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalUnsplit() {
    if (manager_.split_mode == aida::terminal::split_mode_t::none)
        return {false, "The terminal is not split"};
    manager_.set_split(aida::terminal::split_mode_t::none);
    persistTerminalState();
    Q_EMIT sessionsChanged();
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalFocusSearch() {
    if (!manager_.current())
        return {false, "There is no active terminal session"};
    Q_EMIT searchRequested();
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalPaste() {
    auto* session = focusedSession();
    if (!session || !session->alive.load(std::memory_order_acquire))
        return {false, "The active terminal process is not running"};
    const QString clipboard = QApplication::clipboard()->text();
    if (clipboard.isEmpty())
        return {false, "The clipboard has no text"};
    const QByteArray utf8 = clipboard.toUtf8();
    if (static_cast<std::size_t>(utf8.size()) >= aida::terminal::TerminalSession::MAX_INPUT_QUEUE_BYTES)
        return {false, "Clipboard text exceeds the terminal's 1 MiB input limit"};
    aida::terminal::send_input(*session, utf8.constData(),
        static_cast<std::size_t>(utf8.size()));
    Q_EMIT clearSelectionRequested();
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalCopyAll() {
    auto* session = focusedSession();
    if (!session)
        return {false, "The terminal session is unavailable or busy"};
    std::string text;
    if (!aida::terminal::try_copy_all_text(*session, text)) {
        terminal_lock_busy("terminal_snapshot", session->id);
        return {false, "The terminal session is unavailable or busy"};
    }
    if (text.empty())
        return {false, "There is no text to copy"};
    clipboard::set_text(QString::fromStdString(text));
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalClear() {
    auto* session = focusedSession();
    if (!session)
        return {false, "The terminal session is unavailable"};
    if (!aida::terminal::try_clear_session(*session)) {
        terminal_lock_busy("terminal_clear", session->id);
        return {false, "The terminal buffer is busy"};
    }
    Q_EMIT clearSelectionRequested();
    Q_EMIT sessionOutputChanged(session->id);
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalSelectAll() {
    if (!hasActiveContent())
        return {false, "There is no text to select"};
    Q_EMIT selectAllRequested();
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalToggleFollow() {
    auto* session = focusedSession();
    if (!session)
        return {false, "The terminal session is unavailable"};
    session->auto_follow = !session->auto_follow;
    if (session->auto_follow)
        session->scroll_to_bottom = true;
    Q_EMIT sessionOutputChanged(session->id);
    return {true, {}};
}

host::operation_result_t AidaTerminalController::terminalExport() {
    auto* session = focusedSession();
    if (!session)
        return {false, "The terminal session is unavailable or busy"};
    std::string text;
    if (!aida::terminal::try_copy_all_text(*session, text)) {
        terminal_lock_busy("terminal_snapshot", session->id);
        return {false, "The terminal session is unavailable or busy"};
    }
    if (text.empty())
        return {false, "There is no text to export"};
    return AidaOutputController::instance().exportText("Terminal", "view.terminal",
        std::move(text));
}

bool AidaTerminalController::hasActiveContent() {
    auto* session = focusedSession();
    if (!session)
        return false;
    std::unique_lock<std::mutex> lock(session->buffer_mtx, std::try_to_lock);
    if (!lock.owns_lock()) {
        terminal_lock_busy("has_content", session->id);
        return false;
    }
    return !session->lines.empty();
}

bool AidaTerminalController::followsTail() {
    auto* session = focusedSession();
    return session && session->auto_follow;
}

bool AidaTerminalController::sourceAvailable() noexcept {
    return !manager_.sessions.empty();
}

void AidaTerminalController::onSessionOutput(std::uint64_t id) {
    auto* session = sessionById(id);
    if (!session)
        return;
    session->output_notify_pending.store(false, std::memory_order_release);
    Q_EMIT sessionOutputChanged(id);
}

void AidaTerminalController::onReapTick() {
    manager_.reap_retired_sessions();
    pollPersistence();
}

void AidaTerminalController::onIntegrityTick() {
    if (manager_.sessions.empty() && start_attempted_) {
        start_attempted_ = false;
        start_error_.clear();
    }
    Q_EMIT integrityTick();
}

void AidaTerminalController::persistTerminalState() {
    nlohmann::json root = nlohmann::json::object();
    root["version"] = 1;
    root["active"] = manager_.active_tab;
    root["secondary"] = manager_.secondary_tab;
    root["split"] = manager_.split_mode == aida::terminal::split_mode_t::vertical ? "vertical" :
        manager_.split_mode == aida::terminal::split_mode_t::horizontal ? "horizontal" : "none";
    root["sessions"] = nlohmann::json::array();
    for (const auto* session : manager_.sessions) {
        if (!session) continue;
        root["sessions"].push_back({
            {"profile", session->profile_id},
            {"cwd", narrow(session->cwd)}
        });
    }
    persistence_payload_ = root.dump();
    persistence_profile_ = profiles_.empty() || profile_index_ < 0 ||
        profile_index_ >= static_cast<int>(profiles_.size())
        ? std::string{} : profiles_[static_cast<std::size_t>(profile_index_)].id;
    persistence_cwd_ = cwd_;
    ++persistence_generation_;
    scheduleTerminalPersistence();
}

void AidaTerminalController::scheduleTerminalPersistence() {
    if (persistence_in_flight_) return;
    const std::string payload = persistence_payload_;
    const std::string profile = persistence_profile_;
    const std::string cwd = persistence_cwd_;
    g_sa_settings.terminal_sessions_json = payload;
    g_sa_settings.terminal_profile_id = profile;
    g_sa_settings.terminal_default_cwd = cwd;
    std::uint64_t settings_generation = 0;
    const auto requested = aida::settings_persistence::request_save(g_sa_settings,
        &settings_generation);
    if (aida::settings_persistence::accepted(requested)) {
        persistence_in_flight_ = true;
        settings_generation_ = settings_generation;
    } else {
        persistence_in_flight_ = false;
        persistence_error_ =
            "Terminal session persistence could not capture an immutable settings snapshot";
        Q_EMIT persistenceStateChanged();
    }
}

void AidaTerminalController::pollPersistence() {
    if (!persistence_in_flight_)
        return;
    const auto persistence = aida::settings_persistence::status();
    if (persistence.committed_generation >= settings_generation_) {
        persistence_in_flight_ = false;
        if (!persistence_error_.empty()) {
            persistence_error_.clear();
            Q_EMIT persistenceStateChanged();
        }
        if (persistence_generation_ != 0 &&
            persistence_payload_ != g_sa_settings.terminal_sessions_json)
            scheduleTerminalPersistence();
    } else if (!persistence.pending && persistence.failed &&
        persistence.generation >= settings_generation_) {
        persistence_in_flight_ = false;
        persistence_error_ = persistence.error.empty()
            ? "Terminal session layout could not be saved" : persistence.error;
        Q_EMIT persistenceStateChanged();
    }
}

void AidaTerminalController::restoreTerminalState() {
    if (restored_)
        return;
    restored_ = true;
    ensureProfiles();
    if (!g_sa_settings.terminal_restore_sessions || g_sa_settings.terminal_sessions_json.empty())
        return;
    try {
        const auto root = nlohmann::json::parse(g_sa_settings.terminal_sessions_json);
        if (!root.is_object() || root.value("version", 0) != 1 ||
            !root.contains("sessions") || !root["sessions"].is_array())
            return;
        std::size_t restored = 0;
        for (const auto& record : root["sessions"]) {
            if (!record.is_object() || restored >= 12) break;
            const std::string profile_id = record.value("profile", std::string{});
            const auto profile = std::find_if(profiles_.begin(), profiles_.end(),
                [&](const aida::terminal::profile_t& item) { return item.id == profile_id; });
            if (profile == profiles_.end()) continue;
            const std::wstring cwd = widen(record.value("cwd", std::string{}));
            if (auto* session = manager_.create_terminal(profile->command.c_str(),
                    cwd.empty() ? nullptr : cwd.c_str(), profile->id.c_str(),
                    profile->label.c_str())) {
                session->max_lines = (std::clamp)(g_sa_settings.terminal_scrollback, 1000, 100000);
                ++restored;
            }
        }
        if (manager_.sessions.empty()) return;
        manager_.active_tab = (std::clamp)(root.value("active", 0), 0,
            static_cast<int>(manager_.sessions.size()) - 1);
        const std::string split = root.value("split", std::string("none"));
        const int secondary = root.value("secondary", -1);
        if (secondary >= 0 && secondary < static_cast<int>(manager_.sessions.size()) &&
            secondary != manager_.active_tab) {
            manager_.secondary_tab = secondary;
            manager_.split_mode = split == "vertical"
                ? aida::terminal::split_mode_t::vertical : split == "horizontal"
                ? aida::terminal::split_mode_t::horizontal : aida::terminal::split_mode_t::none;
        }
    } catch (const std::exception& error) {
        persistence_error_ = error.what();
    }
    Q_EMIT sessionsChanged();
}

AidaTerminalView::AidaTerminalView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.terminal"));
    const auto& tokens = theme::tokens();
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    auto* tab_row = new QWidget(this);
    tab_row->setObjectName(QStringLiteral("aida.view.terminal.tabrow"));
    auto* tab_layout = new QHBoxLayout(tab_row);
    tab_layout->setContentsMargins(tokens.spacing.xs, tokens.spacing.xxs,
        tokens.spacing.xs, 0);
    tab_layout->setSpacing(tokens.spacing.xs);
    tabs_ = new QTabBar(tab_row);
    tabs_->setObjectName(QStringLiteral("aida.view.terminal.tabs"));
    tabs_->setTabsClosable(true);
    tabs_->setMovable(false);
    tabs_->setExpanding(false);
    tabs_->setElideMode(Qt::ElideRight);
    tabs_->installEventFilter(this);
    tab_layout->addWidget(tabs_, 1);
    new_button_ = new QToolButton(tab_row);
    new_button_->setObjectName(QStringLiteral("aida.view.terminal.new"));
    new_button_->setText(QStringLiteral("+"));
    new_button_->setToolTip(QStringLiteral(
        "New terminal session (choose the shell profile and launch directory)"));
    new_button_->setPopupMode(QToolButton::InstantPopup);
    tab_layout->addWidget(new_button_);
    column->addWidget(tab_row);

    action_row_ = new QWidget(this);
    action_row_->setObjectName(QStringLiteral("aida.view.terminal.actions"));
    auto* action_layout = new QHBoxLayout(action_row_);
    action_layout->setContentsMargins(tokens.spacing.xs, tokens.spacing.xxs,
        tokens.spacing.xs, tokens.spacing.xxs);
    action_layout->setSpacing(tokens.spacing.xs);
    const auto hydrate = [this, action_layout](const char* id, const char* label) {
        auto* action = new QAction(QString::fromLatin1(label), this);
        action->setObjectName(QStringLiteral("aida.") + QString::fromLatin1(id) +
            QStringLiteral(".surface"));
        action->setData(QString::fromLatin1(id));
        action->setAutoRepeat(false);
        connect(action, &QAction::triggered, this, [this, id](bool) {
            static_cast<void>(aida::ui::application_ui::execute_output_action(
                static_cast<int>(bottom_tab_t::terminal), id,
                aida::ui::action_invocation_source_t::toolbar));
        });
        actions_.push_back(action);
        auto* button = new QToolButton(action_row_);
        button->setObjectName(QStringLiteral("aida.") + QString::fromLatin1(id) +
            QStringLiteral(".button"));
        button->setDefaultAction(action);
        action_layout->addWidget(button);
        action_buttons_.push_back(button);
        if (QLatin1String(id).startsWith(QLatin1String("terminal.split")) ||
            QLatin1String(id) == QLatin1String("terminal.unsplit"))
            collapsible_buttons_.push_back(button);
        return action;
    };
    hydrate("terminal.new", "New");
    hydrate("terminal.split_vertical", "Split Right");
    hydrate("terminal.split_horizontal", "Split Down");
    hydrate("terminal.unsplit", "Unsplit");
    hydrate("terminal.search", "Search");
    hydrate("terminal.restart", "Restart");
    hydrate("terminal.close", "Close");
    overflow_button_ = new QToolButton(action_row_);
    overflow_button_->setObjectName(QStringLiteral("aida.view.terminal.actions.overflow"));
    overflow_button_->setText(QStringLiteral("Layout"));
    overflow_button_->setToolTip(QStringLiteral("Terminal layout actions (split and unsplit)"));
    overflow_button_->setPopupMode(QToolButton::InstantPopup);
    overflow_button_->setAutoRaise(true);
    overflow_menu_ = new QMenu(overflow_button_);
    overflow_menu_->setObjectName(QStringLiteral("aida.view.terminal.actions.overflow.menu"));
    overflow_menu_->setToolTipsVisible(true);
    overflow_button_->setMenu(overflow_menu_);
    overflow_button_->setVisible(false);
    for (auto* action : actions_) {
        const QString id = action->data().toString();
        if (id.startsWith(QLatin1String("terminal.split")) ||
            id == QLatin1String("terminal.unsplit"))
            overflow_menu_->addAction(action);
    }
    action_layout->addWidget(overflow_button_);
    action_layout->addStretch(1);
    persistence_label_ = new QLabel(QStringLiteral("Session layout not saved"), action_row_);
    persistence_label_->setObjectName(QStringLiteral("aida.view.terminal.persistence_error"));
    persistence_label_->setVisible(false);
    action_layout->addWidget(persistence_label_);
    column->addWidget(action_row_);

    search_bar_ = new AidaTerminalSearchBar(this);
    search_bar_->setVisible(false);
    column->addWidget(search_bar_);

    content_host_ = new QWidget(this);
    content_host_->setObjectName(QStringLiteral("aida.view.terminal.content"));
    auto* content_layout = new QVBoxLayout(content_host_);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);
    column->addWidget(content_host_, 1);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setState(widgets::AidaStateView::State::Empty);
    state_view_->setTitle(QStringLiteral("Terminal unavailable"));
    state_view_->setActionLabel(QStringLiteral("Retry configured shell"));
    connect(state_view_, &widgets::AidaStateView::actionTriggered, this, [this] {
        AidaTerminalController::instance().retryStart();
    });
    state_view_->setVisible(false);
    column->addWidget(state_view_, 1);

    auto* plus_menu = new QMenu(new_button_);
    plus_menu->setObjectName(QStringLiteral("aida.view.terminal.new.menu"));
    plus_menu->setToolTipsVisible(true);
    auto* cwd_edit = new QLineEdit(plus_menu);
    cwd_edit->setObjectName(QStringLiteral("aida.view.terminal.new.cwd"));
    cwd_edit->setPlaceholderText(QStringLiteral("Launch working directory (optional)"));
    cwd_edit->setText(QString::fromStdString(AidaTerminalController::instance().profileCwd()));
    auto* cwd_action = new QWidgetAction(plus_menu);
    cwd_action->setDefaultWidget(cwd_edit);
    plus_menu->addAction(cwd_action);
    plus_menu->addSeparator();
    connect(plus_menu, &QMenu::aboutToShow, this, [this, cwd_edit, plus_menu] {
        cwd_edit->setText(QString::fromStdString(AidaTerminalController::instance().profileCwd()));
        const auto& profiles = AidaTerminalController::instance().profiles();
        for (auto* action : plus_menu->actions()) {
            if (action->data().isValid())
                plus_menu->removeAction(action);
        }
        int index = 0;
        for (const auto& profile : profiles) {
            auto* action = plus_menu->addAction(QString::fromStdString(profile.label));
            action->setToolTip(QString::fromStdString(narrow(profile.command)));
            action->setData(index);
            connect(action, &QAction::triggered, this, [this, index, cwd_edit](bool) {
                AidaTerminalController::instance().createFromProfile(index,
                    cwd_edit->text().toStdString());
            });
            ++index;
        }
    });
    new_button_->setMenu(plus_menu);

    auto& controller = AidaTerminalController::instance();
    connect(&controller, &AidaTerminalController::sessionsChanged, this, [this] {
        rebuildTabs();
        rebuildContent();
        refreshPresentations();
    });
    connect(&controller, &AidaTerminalController::sessionClosing, this, [this](quint64 id) {
        for (int index = viewports_.size() - 1; index >= 0; --index) {
            auto* viewport = viewports_[index].data();
            if (!viewport || viewport->sessionId() != id)
                continue;
            if (primary_ == viewport)
                primary_ = nullptr;
            if (secondary_ == viewport)
                secondary_ = nullptr;
            viewport->detach();
            viewport->deleteLater();
            viewports_.removeAt(index);
        }
    });
    connect(&controller, &AidaTerminalController::sessionOutputChanged, this, [this](quint64 id) {
        if (primary_ && primary_->sessionId() == id)
            primary_->refreshSnapshot();
        if (secondary_ && secondary_->sessionId() == id)
            secondary_->refreshSnapshot();
    });
    connect(&controller, &AidaTerminalController::integrityTick, this, [this] {
        auto& terminal = AidaTerminalController::instance();
        if (terminal.sessionCount() == 0 && isVisible() && !terminal.startAttempted()) {
            terminal.retryStart();
            rebuildTabs();
            rebuildContent();
        }
        refreshTabLabels();
        if (primary_)
            primary_->integrityCheck();
        if (secondary_)
            secondary_->integrityCheck();
        syncSearchTarget();
        refreshPresentations();
    });
    connect(&controller, &AidaTerminalController::searchRequested, this, [this] {
        search_bar_->setVisible(true);
        search_bar_->focusQuery();
        syncSearchTarget();
    });
    connect(&controller, &AidaTerminalController::selectAllRequested, this, [this] {
        auto* viewport = focusedViewport();
        if (viewport)
            viewport->selectAll();
    });
    connect(&controller, &AidaTerminalController::clearSelectionRequested, this, [this] {
        auto* viewport = focusedViewport();
        if (viewport)
            viewport->clearSelectAll();
    });
    connect(&controller, &AidaTerminalController::persistenceStateChanged, this, [this] {
        const auto error = AidaTerminalController::instance().persistenceError();
        persistence_label_->setVisible(!error.empty());
        if (!error.empty())
            persistence_label_->setToolTip(QString::fromStdString(error));
        action_row_wide_width_ = 0;
        updateActionRowMode();
    });
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        AidaTerminalController::instance().selectSessionAt(index);
    });
    connect(tabs_, &QTabBar::tabCloseRequested, this, [this](int index) {
        AidaTerminalController::instance().closeSessionAt(index);
    });
    connect(search_bar_, &AidaTerminalSearchBar::queryChanged, this, [this](const QString& text) {
        auto* session = AidaTerminalController::instance().focusedSession();
        if (session) {
            aida::terminal::refresh_search(*session, text.toStdString());
            auto* viewport = focusedViewport();
            if (viewport)
                viewport->refreshSnapshot();
        }
        syncSearchTarget();
    });
    connect(search_bar_, &AidaTerminalSearchBar::nextRequested, this, [this] {
        auto* session = AidaTerminalController::instance().focusedSession();
        if (session && aida::terminal::move_search_match(*session, 1)) {
            auto* viewport = focusedViewport();
            if (viewport)
                viewport->refreshSnapshot();
        }
        syncSearchTarget();
    });
    connect(search_bar_, &AidaTerminalSearchBar::previousRequested, this, [this] {
        auto* session = AidaTerminalController::instance().focusedSession();
        if (session && aida::terminal::move_search_match(*session, -1)) {
            auto* viewport = focusedViewport();
            if (viewport)
                viewport->refreshSnapshot();
        }
        syncSearchTarget();
    });
    connect(search_bar_, &AidaTerminalSearchBar::closeRequested, this, [this] {
        search_bar_->setVisible(false);
        auto* session = AidaTerminalController::instance().focusedSession();
        if (session) {
            static_cast<void>(aida::terminal::refresh_search(*session, std::string{}));
            auto* viewport = focusedViewport();
            if (viewport)
                viewport->refreshSnapshot();
        }
    });

    rebuildTabs();
    rebuildContent();
    refreshPresentations();
}

void AidaTerminalView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    auto& controller = AidaTerminalController::instance();
    controller.restoreTerminalState();
    rebuildTabs();
    rebuildContent();
    if (!auto_started_) {
        auto_started_ = true;
        if (controller.sessionCount() == 0)
            controller.retryStart();
    }
}

void AidaTerminalView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateActionRowMode();
}

void AidaTerminalView::updateActionRowMode() {
    if (!overflow_button_)
        return;
    if (action_row_wide_width_ <= 0) {
        for (auto* button : action_buttons_)
            if (button)
                button->setVisible(true);
        overflow_button_->setVisible(true);
        action_row_wide_width_ = action_row_->layout()->totalSizeHint().width();
        overflow_button_->setVisible(false);
    }
    if (width() <= 0)
        return;
    action_row_compact_ = width() < action_row_wide_width_;
    for (auto* button : action_buttons_) {
        if (!button)
            continue;
        auto* action = button->defaultAction();
        const bool collapsible = collapsible_buttons_.contains(button);
        button->setVisible((!action || action->isVisible()) &&
            !(action_row_compact_ && collapsible));
    }
    overflow_button_->setVisible(action_row_compact_);
}

bool AidaTerminalView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tabs_ && event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::MiddleButton) {
            const int index = tabs_->tabAt(mouse->pos());
            if (index >= 0) {
                AidaTerminalController::instance().closeSessionAt(index);
                return true;
            }
        }
    }
    if (watched == tabs_ && event->type() == QEvent::ContextMenu) {
        auto* context_event = static_cast<QContextMenuEvent*>(event);
        const bool keyboard = context_event->reason() == QContextMenuEvent::Keyboard;
        const int index = keyboard ? tabs_->currentIndex()
                                   : tabs_->tabAt(context_event->pos());
        if (index >= 0) {
            tabs_->setCurrentIndex(index);
            const QPoint global_pos = keyboard
                ? tabs_->mapToGlobal(tabs_->tabRect(index).center())
                : context_event->globalPos();
            openTabContextMenu(index, global_pos, keyboard);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AidaTerminalView::openTabContextMenu(int index, const QPoint& global_pos,
    bool keyboard_origin) {
    static_cast<void>(index);
    const auto origin = keyboard_origin ? aida::ui::context_menu_open_origin_t::menu_key
                                        : aida::ui::context_menu_open_origin_t::pointer;
    aida::ui::application_ui::open_output_context_menu(
        static_cast<int>(bottom_tab_t::terminal), origin);
    documents::show_context_menu(
        aida::ui::stable_menu_id_t("menu.output.view"),
        documents::make_menu_snapshot(aida::ui::stable_view_id_t("view.terminal"),
            aida::ui::stable_context_type_id_t("context.output.view")),
        origin, global_pos, this);
}

void AidaTerminalView::refreshTabLabels() {
    auto& manager = AidaTerminalController::instance().manager();
    if (tabs_->count() != static_cast<int>(manager.sessions.size())) {
        rebuildTabs();
        return;
    }
    const QSignalBlocker blocker(tabs_);
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* session = manager.sessions[static_cast<std::size_t>(index)];
        if (!session)
            continue;
        QString label = QString::fromStdString(session->title);
        if (!session->alive.load(std::memory_order_acquire)) {
            const auto code = session->exit_code.load(std::memory_order_acquire);
            label += code == std::numeric_limits<std::uint32_t>::max()
                ? QStringLiteral(" [stopped]")
                : QStringLiteral(" [exit %1]").arg(code);
        }
        if (tabs_->tabText(index) != label)
            tabs_->setTabText(index, label);
    }
}

AidaTerminalViewport* AidaTerminalView::viewportFor(std::uint64_t id) const {
    for (const auto& viewport : viewports_) {
        if (viewport && viewport->sessionId() == id)
            return viewport.data();
    }
    return nullptr;
}

AidaTerminalViewport* AidaTerminalView::focusedViewport() const {
    auto* session = AidaTerminalController::instance().focusedSession();
    return session ? viewportFor(session->id) : nullptr;
}

void AidaTerminalView::rebuildTabs() {
    auto& manager = AidaTerminalController::instance().manager();
    QSignalBlocker blocker(tabs_);
    while (tabs_->count() > static_cast<int>(manager.sessions.size()))
        tabs_->removeTab(tabs_->count() - 1);
    for (int index = 0; index < static_cast<int>(manager.sessions.size()); ++index) {
        auto* session = manager.sessions[static_cast<std::size_t>(index)];
        if (!session)
            continue;
        QString label = QString::fromStdString(session->title);
        if (!session->alive.load(std::memory_order_acquire)) {
            const auto code = session->exit_code.load(std::memory_order_acquire);
            label += code == std::numeric_limits<std::uint32_t>::max()
                ? QStringLiteral(" [stopped]")
                : QStringLiteral(" [exit %1]").arg(code);
        }
        const QString tooltip = QStringLiteral("%1\nCommand: %2\nLaunch directory: %3")
            .arg(QString::fromStdString(session->profile_label),
                QString::fromStdString(narrow(session->command)),
                session->cwd.empty() ? QStringLiteral("Inherited")
                                     : QString::fromStdString(narrow(session->cwd)));
        if (index < tabs_->count()) {
            tabs_->setTabText(index, label);
            tabs_->setTabToolTip(index, tooltip);
            tabs_->setTabData(index, static_cast<qulonglong>(session->id));
        } else {
            const int added = tabs_->addTab(label);
            tabs_->setTabToolTip(added, tooltip);
            tabs_->setTabData(added, static_cast<qulonglong>(session->id));
        }
    }
    if (manager.active_tab >= 0 && manager.active_tab < tabs_->count())
        tabs_->setCurrentIndex(manager.active_tab);
}

void AidaTerminalView::rebuildContent() {
    auto& controller = AidaTerminalController::instance();
    auto& manager = controller.manager();
    auto* active = manager.current();
    auto* secondary = manager.secondary();
    if (manager.secondary_tab == manager.active_tab) {
        if (manager.sessions.size() > 1)
            manager.secondary_tab = manager.active_tab == 0 ? 1 : 0;
        else
            manager.set_split(aida::terminal::split_mode_t::none);
        secondary = manager.secondary();
    }
    if (!active) {
        content_host_->setVisible(false);
        state_view_->setVisible(true);
        const auto start_error = controller.startError();
        state_view_->setState(widgets::AidaStateView::State::Error);
        state_view_->setTitle(start_error.empty() ? QStringLiteral("Terminal unavailable")
                                                  : QStringLiteral("Terminal failed to start"));
        state_view_->setMessage(start_error.empty()
            ? QStringLiteral("The configured terminal session is not running.")
            : QString::fromStdString(start_error) + QStringLiteral(
                " Verify the terminal shell path in Settings. AiDA does not fall back to an unconfigured shell."));
        return;
    }
    state_view_->setVisible(false);
    content_host_->setVisible(true);

    const bool need_split = secondary != nullptr &&
        manager.split_mode != aida::terminal::split_mode_t::none;
    auto* host_layout = content_host_->layout();

    const auto orphan_viewport = [this, host_layout](AidaTerminalViewport* viewport) {
        if (!viewport)
            return;
        if (host_layout)
            host_layout->removeWidget(viewport);
        viewport->setParent(nullptr);
        viewport->setVisible(false);
        const int index = viewports_.indexOf(viewport);
        if (index >= 0)
            viewports_.removeAt(index);
        viewport->deleteLater();
    };

    const auto viewport_for_session = [this, &controller, host_layout](
            aida::terminal::TerminalSession* session, QWidget* parent) {
        if (auto* existing = viewportFor(session->id)) {
            if (host_layout)
                host_layout->removeWidget(existing);
            if (existing->parentWidget() != parent)
                existing->setParent(parent);
            return existing;
        }
        auto* created = new AidaTerminalViewport(session, &controller, parent);
        viewports_.push_back(created);
        return created;
    };

    if (!need_split) {
        if (secondary_) {
            auto* orphan = secondary_;
            secondary_ = nullptr;
            orphan_viewport(orphan);
        }
        if (splitter_) {
            host_layout->removeWidget(splitter_);
            splitter_->deleteLater();
            splitter_ = nullptr;
        }
        if (primary_ && primary_->sessionId() != active->id) {
            auto* orphan = primary_;
            primary_ = nullptr;
            orphan_viewport(orphan);
        }
        if (!primary_ || primary_->sessionId() != active->id) {
            primary_ = viewport_for_session(active, content_host_);
            if (host_layout && host_layout->indexOf(primary_) < 0)
                host_layout->addWidget(primary_);
            primary_->setVisible(true);
        }
        primary_->refreshSnapshot();
        return;
    }
    if (!splitter_) {
        splitter_ = new QSplitter(content_host_);
        splitter_->setObjectName(QStringLiteral("aida.view.terminal.splitter"));
        splitter_->setOpaqueResize(true);
        splitter_->setChildrenCollapsible(false);
        if (host_layout)
            host_layout->addWidget(splitter_);
    }
    splitter_->setOrientation(manager.split_mode == aida::terminal::split_mode_t::vertical
        ? Qt::Horizontal : Qt::Vertical);
    if (primary_ && primary_->sessionId() != active->id) {
        auto* orphan = primary_;
        primary_ = nullptr;
        orphan_viewport(orphan);
    }
    if (secondary_ && secondary_->sessionId() != secondary->id) {
        auto* orphan = secondary_;
        secondary_ = nullptr;
        orphan_viewport(orphan);
    }
    if (!primary_ || primary_->sessionId() != active->id)
        primary_ = viewport_for_session(active, splitter_);
    if (!secondary_ || secondary_->sessionId() != secondary->id)
        secondary_ = viewport_for_session(secondary, splitter_);
    if (splitter_->indexOf(primary_) < 0)
        splitter_->insertWidget(0, primary_);
    if (splitter_->indexOf(secondary_) < 0)
        splitter_->insertWidget(1, secondary_);
    primary_->setVisible(true);
    secondary_->setVisible(true);
    primary_->refreshSnapshot();
    secondary_->refreshSnapshot();
}

void AidaTerminalView::syncSearchTarget() {
    auto* session = AidaTerminalController::instance().focusedSession();
    if (!session || !search_bar_->isVisible())
        return;
    const int total = static_cast<int>(session->search_matches.size());
    search_bar_->setCounter(session->active_search_match < 0 ? 0 : session->active_search_match + 1,
        total);
}

void AidaTerminalView::refreshPresentations() {
    auto& manager = AidaTerminalController::instance().manager();
    for (int index = 0; index < actions_.size(); ++index) {
        auto* action = actions_[index];
        if (!action)
            continue;
        const auto presentation = aida::ui::application_ui::present_output_action(
            static_cast<int>(bottom_tab_t::terminal),
            action->data().toString().toUtf8().constData());
        action->setVisible(presentation.visible);
        action->setEnabled(presentation.enabled);
        QString tooltip = presentation.description.empty()
            ? action->text() : QString::fromStdString(presentation.description);
        if (!presentation.enabled && !presentation.disabled_reason.empty())
            tooltip = QString::fromStdString(presentation.disabled_reason);
        action->setToolTip(tooltip);
        if (action->data().toString() == QLatin1String("terminal.unsplit"))
            action->setVisible(manager.split_mode != aida::terminal::split_mode_t::none);
    }
    updateActionRowMode();
}

}
