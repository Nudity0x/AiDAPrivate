#include "qt/programming/aida_output_pane.hpp"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QStackedLayout>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "core/infra/executor.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/task_center.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/programming/aida_programming_tasks.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::programming {
namespace {

QPointer<AidaOutputController> g_output_controller;

constexpr std::size_t k_output_ring_lines = 4096;

const char* label(bottom_tab_t tab) noexcept {
    switch (tab) {
    case bottom_tab_t::output: return "Output";
    case bottom_tab_t::mcp_log: return "MCP Activity";
    case bottom_tab_t::driver_log: return "Driver Log";
    case bottom_tab_t::sandbox_log: return "Sandbox Log";
    case bottom_tab_t::terminal: return "Terminal";
    case bottom_tab_t::COUNT: break;
    }
    return "Output";
}

const char* empty_message(bottom_tab_t tab) noexcept {
    switch (tab) {
    case bottom_tab_t::mcp_log:
        return "MCP requests and tool activity appear here when the local MCP service is active.";
    case bottom_tab_t::driver_log:
        return "Driver diagnostics appear here after a driver-backed operation reports status.";
    case bottom_tab_t::sandbox_log:
        return "Sandbox execution and isolation diagnostics appear here when a sandbox task runs.";
    default:
        return "Analysis, file, automation, and IDE diagnostics appear here as work runs.";
    }
}

const char* view_id_for_tab(bottom_tab_t tab) noexcept {
    switch (tab) {
    case bottom_tab_t::mcp_log: return "view.mcp_log";
    case bottom_tab_t::driver_log: return "view.driver_log";
    case bottom_tab_t::sandbox_log: return "view.sandbox_log";
    case bottom_tab_t::terminal: return "view.terminal";
    default: return "view.output";
    }
}

void log_lock_busy(const char* operation, bottom_tab_t tab) {
    static std::atomic<unsigned long long> last_log_ms{0};
    static std::atomic<unsigned long long> busy_count{0};
    const unsigned long long now = aida::shell_platform::tick_ms();
    const unsigned long long count = busy_count.fetch_add(1, std::memory_order_acq_rel) + 1ULL;
    unsigned long long last = last_log_ms.load(std::memory_order_acquire);
    if (count != 1ULL && now - last < 500ULL)
        return;
    if (count != 1ULL && !last_log_ms.compare_exchange_strong(last, now, std::memory_order_acq_rel))
        return;
    unsigned long owner_tid = 0;
    unsigned long long owner_age = 0;
    int owner_tab = -1;
    int owner_op = 0;
    output_log::snapshot_owner(owner_tid, owner_age, owner_tab, owner_op);
    diag::log_tagged_fmt("ui",
        "OUTPUT_VIEW_LOCK_BUSY op=%s tab=%d busy_count=%llu owner_tid=%lu owner_age_ms=%llu owner_tab=%d owner_op=%s owner_op_id=%d tid=%lu",
        operation ? operation : "<null>", static_cast<int>(tab), count, owner_tid, owner_age,
        owner_tab, output_log::op_name(owner_op), owner_op,
        static_cast<unsigned long>(aida::shell_platform::thread_id()));
}

bool contains_case_insensitive(const std::string& value, const std::string& normalized_filter) {
    if (normalized_filter.empty())
        return true;
    return std::search(value.begin(), value.end(), normalized_filter.begin(), normalized_filter.end(),
        [](unsigned char lhs, unsigned char rhs) {
            return std::tolower(lhs) == std::tolower(rhs);
        }) != value.end();
}

std::string normalized(const std::string& value) {
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

QString display_text(const output_log::entry_t& entry) {
    if (entry.channel.empty())
        return QString::fromStdString(entry.text);
    QString text = QStringLiteral("[Task:");
    text += QString::fromStdString(entry.channel);
    text += QStringLiteral("] ");
    text += QString::fromStdString(entry.text);
    return text;
}

std::string display_text_std(const output_log::entry_t& entry) {
    if (entry.channel.empty())
        return entry.text;
    return "[Task:" + entry.channel + "] " + entry.text;
}

bool contains_folded_token(const QString& lowered, const char* token) {
    return lowered.contains(QLatin1String(token));
}

bool negated_count(const QString& lowered, const char* singular) {
    return lowered.contains(QStringLiteral("0 %1").arg(QLatin1String(singular))) ||
        lowered.contains(QStringLiteral("no %1").arg(QLatin1String(singular)));
}

class AidaOutputHighlighter : public QSyntaxHighlighter {
public:
    explicit AidaOutputHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {
        const auto& tokens = theme::tokens();
        error_format_.setForeground(tokens.error);
        warning_format_.setForeground(tokens.warning);
        success_format_.setForeground(tokens.success);
        info_format_.setForeground(tokens.info);
    }

protected:
    void highlightBlock(const QString& text) override {
        if (text.isEmpty())
            return;
        const QString lowered = text.toLower();
        if ((contains_folded_token(lowered, "error") && !negated_count(lowered, "error")) ||
            (contains_folded_token(lowered, "failed") && !negated_count(lowered, "failed")) ||
            contains_folded_token(lowered, "fatal") ||
            contains_folded_token(lowered, "exception") ||
            contains_folded_token(lowered, "panic")) {
            setFormat(0, static_cast<int>(text.size()), error_format_);
            return;
        }
        if (contains_folded_token(lowered, "warning") && !negated_count(lowered, "warning")) {
            setFormat(0, static_cast<int>(text.size()), warning_format_);
            return;
        }
        if (contains_folded_token(lowered, "succeeded") ||
            contains_folded_token(lowered, "successfully") ||
            contains_folded_token(lowered, "finished successfully")) {
            setFormat(0, static_cast<int>(text.size()), success_format_);
            return;
        }
        if (lowered.startsWith(QLatin1String("[info")) ||
            lowered.startsWith(QLatin1String("info:")) ||
            lowered.startsWith(QLatin1String("note:"))) {
            setFormat(0, static_cast<int>(text.size()), info_format_);
        }
    }

private:
    QTextCharFormat error_format_;
    QTextCharFormat warning_format_;
    QTextCharFormat success_format_;
    QTextCharFormat info_format_;
};

} 

AidaOutputLogChannel::AidaOutputLogChannel(bottom_tab_t tab, QObject* parent)
    : QObject(parent), tab_(tab) {
}

void AidaOutputLogChannel::setFilterText(const QString& text) {
    const std::string next = text.toStdString();
    if (filter_input_ == next)
        return;
    filter_input_ = next;
    normalized_filter_ = normalized(filter_input_);
    refilterAndReset();
}

void AidaOutputLogChannel::setChannelFilter(const std::string& channel) {
    if (selected_channel_ == channel)
        return;
    selected_channel_ = channel;
    refilterAndReset();
}

bool AidaOutputLogChannel::passesFilter(const output_log::entry_t& entry, QString* display) const {
    if (tab_ == bottom_tab_t::output && !selected_channel_.empty() &&
        entry.channel != selected_channel_)
        return false;
    const std::string rendered = display_text_std(entry);
    if (!contains_case_insensitive(rendered, normalized_filter_))
        return false;
    if (display)
        *display = QString::fromStdString(rendered);
    return true;
}

void AidaOutputLogChannel::refilterAndReset() {
    QStringList visible;
    visible.reserve(static_cast<qsizetype>(snapshot_.size()));
    bool any_channel_content = false;
    for (const auto& entry : snapshot_) {
        if (tab_ == bottom_tab_t::output &&
            (selected_channel_.empty() || entry.channel == selected_channel_))
            any_channel_content = true;
        QString line;
        if (passesFilter(entry, &line))
            visible.push_back(line);
    }
    const bool had_content = has_content_;
    has_content_ = any_channel_content;
    Q_EMIT resetAll(visible.join(u'\n'));
    if (had_content != has_content_)
        Q_EMIT contentStateChanged();
}

bool AidaOutputLogChannel::tick() {
    bool any_emitted = false;
    bool follow = follow_cached_;
    if (!output_log::try_is_auto_scroll(tab_, follow)) {
        log_lock_busy("follow_state", tab_);
    } else if (follow != follow_cached_) {
        follow_cached_ = follow;
        Q_EMIT followChanged(follow);
        any_emitted = true;
    }
    bool changed = false;
    std::size_t total = total_;
    std::vector<output_log::entry_t> next;
    const bool available = output_log::try_snapshot_tail_if_changed(
        tab_, k_output_ring_lines, known_version_, next, &total, &changed);
    if (!available) {
        log_lock_busy("log_snapshot", tab_);
        return any_emitted;
    }
    const bool first_snapshot = !snapshot_available_;
    snapshot_available_ = true;
    total_ = total;
    if (!changed) {
        if (first_snapshot) {
            Q_EMIT contentStateChanged();
            any_emitted = true;
        }
        return any_emitted;
    }
    const bool had_content = has_content_;
    if (tab_ == bottom_tab_t::output) {
        has_content_ = std::any_of(next.begin(), next.end(), [&](const output_log::entry_t& entry) {
            return selected_channel_.empty() || entry.channel == selected_channel_;
        });
    } else {
        has_content_ = !next.empty();
    }

    std::size_t overlap = 0;
    const std::size_t max_overlap = (std::min)(snapshot_.size(), next.size());
    for (std::size_t k = max_overlap; k >= 1; --k) {
        if (snapshot_[snapshot_.size() - 1].text != next[k - 1].text ||
            snapshot_[snapshot_.size() - 1].channel != next[k - 1].channel)
            continue;
        bool equal = true;
        for (std::size_t i = 0; i < k; ++i) {
            const auto& old_entry = snapshot_[snapshot_.size() - k + i];
            const auto& new_entry = next[i];
            if (old_entry.text != new_entry.text || old_entry.channel != new_entry.channel) {
                equal = false;
                break;
            }
        }
        if (equal) {
            overlap = k;
            break;
        }
    }
    snapshot_ = std::move(next);
    if (overlap > 0) {
        QStringList appended_lines;
        appended_lines.reserve(static_cast<qsizetype>(snapshot_.size() - overlap));
        for (std::size_t index = overlap; index < snapshot_.size(); ++index) {
            QString line;
            if (passesFilter(snapshot_[index], &line))
                appended_lines.push_back(line);
        }
        Q_EMIT appended(appended_lines);
    } else {
        QStringList visible;
        visible.reserve(static_cast<qsizetype>(snapshot_.size()));
        for (const auto& entry : snapshot_) {
            QString line;
            if (passesFilter(entry, &line))
                visible.push_back(line);
        }
        Q_EMIT resetAll(visible.join(u'\n'));
    }
    if (had_content != has_content_)
        Q_EMIT contentStateChanged();
    return true;
}

AidaOutputPane::AidaOutputPane(bottom_tab_t tab, const QString& instanceKey, QWidget* parent)
    : QWidget(parent), tab_(tab) {
    const QString object_name = QStringLiteral("aida.") +
        QString::fromStdString(std::string(view_id_for_tab(tab)));
    setObjectName(instanceKey.isEmpty() ? object_name + QStringLiteral(".pane")
        : object_name + QStringLiteral(".") + instanceKey);

    stack_ = new QStackedLayout(this);
    stack_->setContentsMargins(0, 0, 0, 0);

    auto* content = new QWidget(this);
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    toolbar_ = new QToolBar(content);
    toolbar_->setObjectName(object_name + QStringLiteral(".toolbar"));
    toolbar_->setMovable(false);
    toolbar_->setFloatable(false);
    toolbar_->setIconSize(QSize(theme::tokens().control.icon_glyph,
        theme::tokens().control.icon_glyph));
    column->addWidget(toolbar_);

    edit_ = new QPlainTextEdit(content);
    edit_->setObjectName(object_name + QStringLiteral(".log"));
    edit_->setReadOnly(true);
    edit_->setMaximumBlockCount(static_cast<int>(k_output_ring_lines));
    edit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    edit_->setFont(theme::fonts::codeRegular());
    edit_->setContextMenuPolicy(Qt::NoContextMenu);
    edit_->viewport()->installEventFilter(this);
    edit_->installEventFilter(this);
    new AidaOutputHighlighter(edit_->document());
    column->addWidget(edit_, 1);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(object_name + QStringLiteral(".state"));

    stack_->addWidget(content);
    stack_->addWidget(state_view_);
    stack_->setCurrentIndex(0);

    setChannel(AidaOutputController::instance().channel(tab_));

    connect(&AidaOutputController::instance(), &AidaOutputController::polled,
            this, &AidaOutputPane::refreshPresentations);
    connect(&AidaOutputController::instance(), &AidaOutputController::selectAllRequested,
            this, [this](int tab) {
        if (tab == static_cast<int>(tab_))
            edit_->selectAll();
    });
    connect(&AidaOutputController::instance(), &AidaOutputController::focusFilterRequested,
            this, [this](int tab) {
        if (tab == static_cast<int>(tab_))
            focusFilterField();
    });

    rebuildToolbar();
    updateStatePage();
}

void AidaOutputPane::setChannel(AidaOutputLogChannel* channel) {
    if (channel_ == channel)
        return;
    if (channel_) {
        disconnect(channel_, nullptr, this, nullptr);
    }
    channel_ = channel;
    if (!channel_)
        return;
    connect(channel_, &AidaOutputLogChannel::appended, this, &AidaOutputPane::appendLines);
    connect(channel_, &AidaOutputLogChannel::resetAll, this, &AidaOutputPane::resetText);
    connect(channel_, &AidaOutputLogChannel::followChanged, this, [this](bool) {
        refreshPresentations();
    });
    connect(channel_, &AidaOutputLogChannel::contentStateChanged, this,
            &AidaOutputPane::updateStatePage);
    if (filter_edit_)
        channel_->setFilterText(filter_edit_->text());
    updateStatePage();
}

void AidaOutputPane::focusFilterField() {
    if (!filter_edit_)
        return;
    if (filter_edit_->isVisible()) {
        filter_edit_->setFocus();
        filter_edit_->selectAll();
        return;
    }
    if (filter_button_ && filter_button_->menu()) {
        filter_button_->showMenu();
        if (filter_edit_->isVisible()) {
            filter_edit_->setFocus();
            filter_edit_->selectAll();
        }
    }
}

void AidaOutputPane::appendLines(const QStringList& lines) {
    if (lines.isEmpty())
        return;
    QScrollBar* bar = edit_->verticalScrollBar();
    const bool follow = channel_ && channel_->followsTail();
    const int previous = bar->value();
    edit_->appendPlainText(lines.join(u'\n'));
    if (follow)
        bar->setValue(bar->maximum());
    else
        bar->setValue((std::min)(previous, bar->maximum()));
    updateStatePage();
}

void AidaOutputPane::resetText(const QString& text) {
    edit_->setPlainText(text);
    if (channel_ && channel_->followsTail())
        edit_->verticalScrollBar()->setValue(edit_->verticalScrollBar()->maximum());
    updateStatePage();
}

void AidaOutputPane::updateStatePage() {
    if (!channel_)
        return;
    const bool empty_document = edit_->document()->isEmpty();
    if (!empty_document) {
        stack_->setCurrentIndex(0);
        return;
    }
    auto& view = *state_view_;
    if (!channel_->snapshotAvailable()) {
        view.setState(widgets::AidaStateView::State::Loading);
        view.setTitle(QStringLiteral("Reading output"));
        view.setMessage(QStringLiteral(
            "The output buffer is busy. AiDA will retry without discarding existing data."));
    } else if (channel_->snapshotEmpty()) {
        view.setState(widgets::AidaStateView::State::Empty);
        view.setTitle(QStringLiteral("No output yet"));
        view.setMessage(QString::fromLatin1(empty_message(tab_)));
    } else {
        const bool channel_empty = tab_ == bottom_tab_t::output &&
            !channel_->channelFilter().empty() && channel_->filterText().isEmpty();
        view.setState(widgets::AidaStateView::State::Empty);
        view.setTitle(channel_empty ? QStringLiteral("No output in this channel")
                                    : QStringLiteral("No matching output"));
        view.setMessage(channel_empty
            ? QStringLiteral("The selected task channel has no retained entries.")
            : QStringLiteral("No entries match the current filter."));
    }
    stack_->setCurrentIndex(1);
}

void AidaOutputPane::rebuildToolbar() {
    for (auto* action : actions_) {
        if (action)
            action->deleteLater();
    }
    actions_.clear();
    toolbar_->clear();
    const int tab = static_cast<int>(tab_);
    const auto hydrate = [this, tab](const char* id) {
        const auto presentation = aida::ui::application_ui::present_output_action(tab, id);
        auto* action = new QAction(QString::fromStdString(presentation.label), this);
        action->setObjectName(QStringLiteral("aida.") + QString::fromLatin1(id) +
            QStringLiteral(".surface"));
        action->setData(QString::fromLatin1(id));
        action->setAutoRepeat(false);
        connect(action, &QAction::triggered, this, [this, tab, id](bool) {
            static_cast<void>(aida::ui::application_ui::execute_output_action(tab, id,
                aida::ui::action_invocation_source_t::toolbar));
        });
        actions_.push_back(action);
        return action;
    };
    toolbar_->addAction(hydrate("output.copy_all"));
    toolbar_->addAction(hydrate("output.select_all"));
    toolbar_->addAction(hydrate("output.clear"));
    toolbar_->addAction(hydrate("output.export"));
    follow_action_ = hydrate("output.follow");
    follow_action_->setCheckable(true);
    toolbar_->addAction(follow_action_);
    toolbar_->addAction(hydrate("output.filter"));

    filter_edit_ = new QLineEdit(toolbar_);
    filter_edit_->setObjectName(QStringLiteral("aida.output.filter.field"));
    filter_edit_->setPlaceholderText(QStringLiteral("Filter output"));
    filter_edit_->setToolTip(QStringLiteral(
        "Show only entries containing this text (case-insensitive)"));
    filter_edit_->setClearButtonEnabled(true);
    filter_edit_->setMaximumWidth(filter_edit_->fontMetrics().averageCharWidth() * 44);
    connect(filter_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (channel_)
            channel_->setFilterText(text);
    });
    toolbar_->addWidget(filter_edit_);

    filter_button_ = new QToolButton(toolbar_);
    filter_button_->setObjectName(QStringLiteral("aida.output.filter.button"));
    filter_button_->setText(QStringLiteral("Filter"));
    filter_button_->setToolTip(QStringLiteral("Filter visible output entries"));
    filter_button_->setPopupMode(QToolButton::InstantPopup);
    filter_button_->setAutoRaise(true);
    auto* popup = new QMenu(filter_button_);
    popup->setObjectName(QStringLiteral("aida.output.filter.popup"));
    popup->setToolTipsVisible(true);
    auto* popup_edit = new QLineEdit(popup);
    popup_edit->setPlaceholderText(QStringLiteral("Type to filter visible entries"));
    popup_edit->setClearButtonEnabled(true);
    auto* popup_action = new QWidgetAction(popup);
    popup_action->setDefaultWidget(popup_edit);
    popup->addAction(popup_action);
    connect(popup_edit, &QLineEdit::textChanged, filter_edit_,
            &QLineEdit::setText);
    connect(filter_edit_, &QLineEdit::textChanged, popup_edit,
            &QLineEdit::setText);
    connect(popup, &QMenu::aboutToShow, popup_edit, [popup_edit] {
        popup_edit->setFocus();
        popup_edit->selectAll();
    });
    filter_button_->setMenu(popup);
    toolbar_->addWidget(filter_button_);

    wrap_button_ = new QToolButton(toolbar_);
    wrap_button_->setObjectName(QStringLiteral("aida.output.wrap.button"));
    wrap_button_->setText(QStringLiteral("Wrap"));
    wrap_button_->setToolTip(QStringLiteral(
        "Wrap long lines to the pane width (horizontal scrolling when off)"));
    wrap_button_->setCheckable(true);
    wrap_button_->setAutoRaise(true);
    connect(wrap_button_, &QToolButton::toggled, this, [this](bool checked) {
        edit_->setLineWrapMode(checked ? QPlainTextEdit::WidgetWidth
                                       : QPlainTextEdit::NoWrap);
    });
    toolbar_->addWidget(wrap_button_);

    adaptive_wide_width_ = 0;
    updateAdaptiveFilter();
    refreshPresentations();
}

void AidaOutputPane::refreshPresentations() {
    for (auto* action : actions_) {
        if (!action)
            continue;
        const auto presentation = aida::ui::application_ui::present_output_action(
            static_cast<int>(tab_), action->data().toString().toUtf8().constData());
        action->setVisible(presentation.visible);
        action->setEnabled(presentation.enabled);
        QString tooltip = presentation.description.empty()
            ? QString::fromStdString(presentation.label)
            : QString::fromStdString(presentation.description);
        if (!presentation.enabled && !presentation.disabled_reason.empty())
            tooltip = QString::fromStdString(presentation.disabled_reason);
        if (!presentation.shortcut.empty())
            tooltip += QStringLiteral(" (") +
                QString::fromStdString(presentation.shortcut) + QStringLiteral(")");
        action->setToolTip(tooltip);
    }
    if (follow_action_) {
        const bool following = channel_ && channel_->followsTail();
        follow_action_->setChecked(following);
        follow_action_->setText(following ? QStringLiteral("Following")
                                          : QStringLiteral("Follow tail"));
    }
}

void AidaOutputPane::updateAdaptiveFilter() {
    const bool supports_search = AidaOutputController::instance().supports_filter(tab_);
    if (adaptive_wide_width_ <= 0) {
        filter_edit_->setVisible(true);
        adaptive_wide_width_ = toolbar_->sizeHint().width() +
            theme::tokens().toolbar.padding_x * 2;
        if (!supports_search)
            filter_edit_->setVisible(false);
    }
    const bool wide = supports_search && width() >= adaptive_wide_width_;
    filter_edit_->setVisible(wide);
    filter_button_->setVisible(supports_search && !wide);
}

void AidaOutputPane::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateAdaptiveFilter();
}

bool AidaOutputPane::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == edit_->viewport() || watched == edit_) &&
        event->type() == QEvent::ContextMenu) {
        auto* context_event = static_cast<QContextMenuEvent*>(event);
        const bool keyboard = context_event->reason() == QContextMenuEvent::Keyboard;
        const QPoint global_pos = keyboard
            ? edit_->viewport()->mapToGlobal(edit_->cursorRect().center())
            : context_event->globalPos();
        openContextMenu(keyboard ? aida::ui::context_menu_open_origin_t::menu_key
                                 : aida::ui::context_menu_open_origin_t::pointer,
            global_pos);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void AidaOutputPane::openContextMenu(aida::ui::context_menu_open_origin_t origin,
                                     const QPoint& global_pos) {
    const int tab = static_cast<int>(tab_);
    aida::ui::application_ui::open_output_context_menu(tab, origin);
    documents::show_context_menu(
        aida::ui::stable_menu_id_t("menu.output.view"),
        documents::make_menu_snapshot(
            aida::ui::stable_view_id_t(view_id_for_tab(tab_)),
            aida::ui::stable_context_type_id_t("context.output.view")),
        origin, global_pos, this);
}

AidaOutputViewHost::AidaOutputViewHost(bottom_tab_t tab, const QString& instanceKey,
                                       QWidget* parent)
    : QWidget(parent) {
    const QString base = QStringLiteral("aida.") +
        QString::fromStdString(std::string(view_id_for_tab(tab)));
    setObjectName(instanceKey.isEmpty() ? base : base + QStringLiteral(".") + instanceKey);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    if (tab == bottom_tab_t::output) {
        auto* strip = new AidaTaskControlsStrip(this);
        column->addWidget(strip);
    }
    pane_ = new AidaOutputPane(tab, instanceKey, this);
    column->addWidget(pane_, 1);
}

AidaOutputController& AidaOutputController::instance() {
    if (!g_output_controller)
        g_output_controller = new AidaOutputController();
    return *g_output_controller;
}

bool AidaOutputController::exists() noexcept {
    return g_output_controller != nullptr;
}

AidaOutputController::AidaOutputController(QObject* parent) : QObject(parent) {
    for (int tab = 0; tab < static_cast<int>(bottom_tab_t::COUNT); ++tab) {
        const auto which = static_cast<bottom_tab_t>(tab);
        if (which == bottom_tab_t::terminal)
            continue;
        channels_[tab] = new AidaOutputLogChannel(which, this);
    }
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(100);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, [this] {
        bool any_changed = false;
        for (auto* channel : channels_) {
            if (channel)
                any_changed = channel->tick() || any_changed;
        }
        if (any_changed)
            Q_EMIT polled();
    });
    poll_timer_->start();
}

void AidaOutputController::install(docking::AidaDockHost* host) {
    host_ = host;
}

AidaOutputLogChannel* AidaOutputController::channel(bottom_tab_t tab) const {
    const int index = output_log::tab_index(tab);
    if (index < 0 || index >= static_cast<int>(bottom_tab_t::COUNT))
        return nullptr;
    return channels_[index];
}

bool AidaOutputController::has_content(bottom_tab_t tab) {
    auto* model = channel(tab);
    return model && model->hasContent();
}

bool AidaOutputController::supports_filter(bottom_tab_t tab) noexcept {
    return tab != bottom_tab_t::terminal;
}

bool AidaOutputController::follows_tail(bottom_tab_t tab) {
    bool enabled = true;
    if (!output_log::try_is_auto_scroll(tab, enabled))
        log_lock_busy("follow_state", tab);
    return enabled;
}

bool AidaOutputController::source_available(bottom_tab_t tab) noexcept {
    return tab != bottom_tab_t::terminal;
}

host::operation_result_t AidaOutputController::copy_all(bottom_tab_t tab) {
    std::string text;
    if (!snapshotText(tab, text))
        return {false, "The output buffer is busy"};
    if (text.empty())
        return {false, "There is no text to copy"};
    clipboard::set_text(QString::fromStdString(text));
    return {true, {}};
}

host::operation_result_t AidaOutputController::clear(bottom_tab_t tab) {
    if (!output_log::try_clear(tab)) {
        log_lock_busy("log_clear", tab);
        return {false, "The output buffer is busy"};
    }
    return {true, {}};
}

host::operation_result_t AidaOutputController::select_all(bottom_tab_t tab) {
    if (!has_content(tab))
        return {false, "There is no text to select"};
    Q_EMIT selectAllRequested(static_cast<int>(tab));
    return {true, {}};
}

host::operation_result_t AidaOutputController::toggle_follow(bottom_tab_t tab) {
    bool follow = true;
    if (!output_log::try_is_auto_scroll(tab, follow) ||
        !output_log::try_set_auto_scroll(tab, !follow)) {
        log_lock_busy("toggle_follow", tab);
        return {false, "The output buffer is busy"};
    }
    return {true, {}};
}

host::operation_result_t AidaOutputController::focus_filter(bottom_tab_t tab) {
    if (!supports_filter(tab))
        return {false, "Terminal output cannot be filtered without changing interactive terminal semantics"};
    Q_EMIT focusFilterRequested(static_cast<int>(tab));
    return {true, {}};
}

void AidaOutputController::noteExternalChannelChange() {
    auto* model = channel(bottom_tab_t::output);
    if (!model)
        return;
    model->setChannelFilter(
        aida::ui::programming_tasks::catalog_snapshot().selected_channel);
}

bool AidaOutputController::snapshotText(bottom_tab_t tab, std::string& text) {
    text.clear();
    std::deque<output_log::entry_t> lines;
    if (!output_log::try_snapshot_all(tab, lines)) {
        log_lock_busy("log_snapshot_all", tab);
        return false;
    }
    const std::string channel_filter =
        tab == bottom_tab_t::output
            ? aida::ui::programming_tasks::catalog_snapshot().selected_channel
            : std::string{};
    std::size_t bytes = 0;
    for (const auto& entry : lines) {
        if (!channel_filter.empty() && entry.channel != channel_filter)
            continue;
        bytes += display_text_std(entry).size() + 1;
    }
    text.reserve(bytes);
    for (const auto& entry : lines) {
        if (!channel_filter.empty() && entry.channel != channel_filter)
            continue;
        text.append(display_text_std(entry));
        text.push_back('\n');
    }
    return true;
}

host::operation_result_t AidaOutputController::export_all(bottom_tab_t tab) {
    std::string text;
    if (!snapshotText(tab, text))
        return {false, "The output source is unavailable or busy"};
    if (text.empty())
        return {false, "There is no text to export"};
    return exportText(label(tab), "view.output", std::move(text));
}

host::operation_result_t AidaOutputController::exportText(const std::string& log_label,
        const std::string& owner_view, std::string text) {
    const QString default_name = QString::fromStdString(log_label) + QStringLiteral(".log");
    const QString picked = QFileDialog::getSaveFileName(QApplication::activeWindow(),
        QStringLiteral("Export Output"), default_name,
        QStringLiteral("Log files (*.log);;Text files (*.txt);;All files (*.*)"));
    if (picked.isEmpty())
        return {true, {}};
    const std::string path = picked.toStdString();
    if (file_tabs::find_path_document(path) >= 0)
        return {false, "Choose a destination that is not open in the code editor"};
    struct export_state_t {
        std::string id;
        std::string path;
        std::string text;
    };
    static std::atomic<std::uint64_t> sequence{1};
    auto export_state = std::make_shared<export_state_t>();
    export_state->id = "output.export." + std::to_string(GetTickCount64()) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    export_state->path = path;
    export_state->text = std::move(text);
    aida::ui::task_center::task_registration_t registration;
    registration.id = export_state->id;
    registration.source = "output.export";
    registration.owner = "Output Export";
    registration.owner_view = owner_view;
    registration.owner_action = "output.export";
    registration.target = export_state->path;
    registration.label = std::string("Export ") + log_label;
    registration.stage = "Queued for atomic file export";
    registration.affected_entity = export_state->path;
    registration.callbacks.focus = [controller = this, host = host_, owner_view] {
        QMetaObject::invokeMethod(controller, [host, owner_view] {
            if (host)
                static_cast<void>(host->open_or_focus(
                    registry::stable_view_id_t(owner_view)));
        }, Qt::QueuedConnection);
    };
    registration.callbacks.open_log = registration.callbacks.focus;
    if (!aida::ui::task_center::register_task(std::move(registration)))
        return {false, "The Task Center rejected the output export"};
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "output_export";
    submission.label = "output.atomic_export";
    submission.thread_class = "bounded_file_io";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.session_id = export_state->id.c_str();
    submission.target_id = export_state->path.c_str();
    submission.diagnostic_id = export_state->id.c_str();
    submission.ui_access_policy = "immutable_snapshots_only";
    submission.failure_policy = "typed_diagnostic";
    submission.shutdown_policy = "drain";
    submission.body = [export_state] {
        namespace task_center = aida::ui::task_center;
        try {
            static_cast<void>(task_center::update_task(export_state->id,
                task_center::task_state_t::running, -1.0f, "Writing same-directory temporary file"));
            const auto result = file_tabs::atomic_write_file(export_state->path, export_state->text);
            export_state->text.clear();
            export_state->text.shrink_to_fit();
            if (result.succeeded) {
                static_cast<void>(task_center::update_task(export_state->id,
                    task_center::task_state_t::completed, 1.0f, "Finished",
                    "Output exported atomically"));
            } else {
                static_cast<void>(task_center::update_task(export_state->id,
                    task_center::task_state_t::failed, 1.0f, "Atomic export failed",
                    result.detail, "diagnostic." + export_state->id));
            }
        } catch (const std::exception& exception) {
            static_cast<void>(task_center::update_task(export_state->id,
                task_center::task_state_t::failed, 1.0f, "Atomic export failed",
                exception.what(), "diagnostic." + export_state->id));
        } catch (...) {
            static_cast<void>(task_center::update_task(export_state->id,
                task_center::task_state_t::failed, 1.0f, "Atomic export failed",
                "Unknown export failure", "diagnostic." + export_state->id));
        }
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        static_cast<void>(aida::ui::task_center::update_task(export_state->id,
            aida::ui::task_center::task_state_t::failed, 1.0f, "Executor rejected export",
            submitted.reject_reason, "diagnostic." + export_state->id));
        return {false, "The output export executor rejected the request: " + submitted.reject_reason};
    }
    return {true, "Output export queued"};
}

}
