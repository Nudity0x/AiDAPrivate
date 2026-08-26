#pragma once

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/terminal/terminal_session.hpp"
#include "qt/programming/programming_host_hooks.hpp"

class QKeyEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QPointF;
class QResizeEvent;
class QSplitter;
class QTabBar;
class QTimer;
class QToolButton;
class QVariantAnimation;
class QWheelEvent;

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::programming {

class AidaTerminalController;
class AidaTerminalViewport;

// One custom grid canvas per terminal session (QTermWidget-style architecture:
// QAbstractScrollArea subclass owning scrollbar range/value/pageStep, painting
// a GUI-side snapshot — paintEvent never touches TerminalSession::buffer_mtx).
class AidaTerminalViewport : public QAbstractScrollArea {
    Q_OBJECT
public:
    AidaTerminalViewport(aida::terminal::TerminalSession* session,
                         AidaTerminalController* controller, QWidget* parent = nullptr);
    ~AidaTerminalViewport() override;

    std::uint64_t sessionId() const noexcept { return session_id_; }
    void detach();          // session is closing; stop touching it
    void refreshSnapshot(); // try-lock grid copy; lock-busy keeps old snapshot
    void integrityCheck();  // generation compare vs painted
    void updateMetrics();   // font/geometry derived values
    void setReducedMotion(bool reduced);
    void selectAll();
    void clearSelectAll();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct grid_snapshot_t {
        quint64 generation = 0;
        int total_lines = 0;
        int first_line = 0;
        QVector<QVector<aida::terminal::Cell>> rows;
        int cursor_row = 0;
        int cursor_col = 0;
        bool alive = false;
        quint32 exit_code = 0;
        int cols = 0;
        QVector<aida::terminal::TerminalSession::search_match_t> matches;
        QVector<int> match_global_indices;
        int active_match = -1;
        std::string input_error;
    };

    int visibleRows() const;
    void applyScrollState();
    void propagateResize();
    void startBell();
    void startCaretTimer();
    void stopCaretTimer();
    qreal caretAlpha() const;
    void copyAllText();
    void copyRangeSelection();
    void clearRangeSelection();
    bool mapToCell(const QPointF& pos, int& row, int& col) const;
    void sendBytes(const char* data, std::size_t length);

    AidaTerminalController* controller_ = nullptr;
    aida::terminal::TerminalSession* session_ = nullptr;
    std::uint64_t session_id_ = 0;
    grid_snapshot_t snapshot_;
    QFont font_;
    QFont bold_font_;
    qreal char_w_ = 0.0;
    qreal line_h_ = 0.0;
    qreal ascent_ = 0.0;
    int prev_line_count_ = 0;
    QVector<double> entrance_times_;
    QElapsedTimer clock_;
    QVariantAnimation* bell_anim_ = nullptr;
    QTimer* caret_timer_ = nullptr;
    bool caret_visible_ = true;
    bool select_all_ = false;
    bool reduced_motion_ = false;
    bool resize_retry_pending_ = false;
    bool selecting_ = false;
    bool has_range_selection_ = false;
    int sel_anchor_row_ = 0;
    int sel_anchor_col_ = 0;
    int sel_caret_row_ = 0;
    int sel_caret_col_ = 0;
};

class AidaTerminalSearchBar : public QWidget {
    Q_OBJECT
public:
    explicit AidaTerminalSearchBar(QWidget* parent = nullptr);
    void setCounter(int active, int total);
    void setQuery(const QString& text);
    QString query() const;
    void focusQuery();

Q_SIGNALS:
    void queryChanged(const QString& text);
    void nextRequested();
    void previousRequested();
    void closeRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLineEdit* query_ = nullptr;
    QLabel* counter_ = nullptr;
};

// Process-wide GUI-thread terminal owner: TerminalManager (ConPTY backend),
// profiles, persistence, the 250 ms reap timer and the 100 ms integrity timer.
class AidaTerminalController : public QObject {
    Q_OBJECT
public:
    static AidaTerminalController& instance();
    static bool exists() noexcept;

    void install(docking::AidaDockHost* host);
    void shutdown();
    docking::AidaDockHost* host() const noexcept { return host_; }

    aida::terminal::TerminalManager& manager() noexcept { return manager_; }
    const std::vector<aida::terminal::profile_t>& profiles();
    int profileIndex() const noexcept { return profile_index_; }
    std::string profileCwd() const { return cwd_; }
    void setProfileCwd(const std::string& cwd);

    aida::terminal::TerminalSession* sessionById(std::uint64_t id) const;
    aida::terminal::TerminalSession* focusedSession();   // focused viewport's session, else active
    void noteViewportFocused(std::uint64_t session_id);
    void noteViewportRemoved(std::uint64_t session_id);

    host::operation_result_t terminalNew();
    host::operation_result_t terminalNewAt(const std::string& working_directory);
    host::operation_result_t terminalClose();
    host::operation_result_t terminalRestart();
    host::operation_result_t terminalNext();
    host::operation_result_t terminalPrevious();
    host::operation_result_t terminalSplit(aida::terminal::split_mode_t mode);
    host::operation_result_t terminalUnsplit();
    host::operation_result_t terminalFocusSearch();
    host::operation_result_t terminalPaste();
    host::operation_result_t terminalCopyAll();
    host::operation_result_t terminalClear();
    host::operation_result_t terminalSelectAll();
    host::operation_result_t terminalToggleFollow();
    host::operation_result_t terminalExport();

    bool hasActiveContent();
    bool followsTail();
    bool sourceAvailable() noexcept;
    std::size_t sessionCount() const noexcept { return manager_.sessions.size(); }
    bool isSplit() const noexcept { return manager_.split_mode != aida::terminal::split_mode_t::none; }
    std::string persistenceError() const { return persistence_error_; }
    std::string startError() const { return start_error_; }

    void createFromProfile(int profile_index, const std::string& cwd);
    void closeSessionAt(int index);
    void selectSessionAt(int index);
    void retryStart();
    void restoreTerminalState();
    bool startAttempted() const noexcept { return start_attempted_; }

Q_SIGNALS:
    void sessionsChanged();
    void sessionClosing(quint64 id);
    void sessionOutputChanged(quint64 id);
    void integrityTick();
    void searchRequested();
    void selectAllRequested();
    void clearSelectionRequested();
    void persistenceStateChanged();

private:
    explicit AidaTerminalController(QObject* parent = nullptr);
    void onSessionOutput(std::uint64_t id);
    void onReapTick();
    void onIntegrityTick();
    void ensureProfiles();
    aida::terminal::TerminalSession* createSelectedTerminal();
    void persistTerminalState();
    void scheduleTerminalPersistence();
    void pollPersistence();

    docking::AidaDockHost* host_ = nullptr;
    aida::terminal::TerminalManager manager_;
    std::vector<aida::terminal::profile_t> profiles_;
    int profile_index_ = 0;
    std::string cwd_;
    bool start_attempted_ = false;
    std::string start_error_;
    bool restored_ = false;
    std::uint64_t persistence_generation_ = 0;
    std::uint64_t settings_generation_ = 0;
    bool persistence_in_flight_ = false;
    std::string persistence_payload_;
    std::string persistence_profile_;
    std::string persistence_cwd_;
    std::string persistence_error_;
    std::uint64_t focused_session_id_ = 0;
    QTimer* reap_timer_ = nullptr;
    QTimer* integrity_timer_ = nullptr;
};

// Registry view: tab bar + "+" profile menu + action row + optional split grid.
class AidaTerminalView : public QWidget {
    Q_OBJECT
public:
    explicit AidaTerminalView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildTabs();
    void refreshTabLabels();
    void rebuildContent();
    void refreshPresentations();
    void syncSearchTarget();
    void updateActionRowMode();
    AidaTerminalViewport* viewportFor(std::uint64_t id) const;
    AidaTerminalViewport* focusedViewport() const;
    void openTabContextMenu(int index, const QPoint& global_pos, bool keyboard_origin);

    QTabBar* tabs_ = nullptr;
    QToolButton* new_button_ = nullptr;
    QWidget* action_row_ = nullptr;
    QToolButton* overflow_button_ = nullptr;
    QMenu* overflow_menu_ = nullptr;
    QVector<QToolButton*> collapsible_buttons_;
    QVector<QToolButton*> action_buttons_;
    QLabel* persistence_label_ = nullptr;
    AidaTerminalSearchBar* search_bar_ = nullptr;    QWidget* content_host_ = nullptr;
    QSplitter* splitter_ = nullptr;
    AidaTerminalViewport* primary_ = nullptr;
    AidaTerminalViewport* secondary_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QVector<QPointer<AidaTerminalViewport>> viewports_;
    QVector<QAction*> actions_;
    bool auto_started_ = false;
    int action_row_wide_width_ = 0;
    bool action_row_compact_ = false;
};

}
