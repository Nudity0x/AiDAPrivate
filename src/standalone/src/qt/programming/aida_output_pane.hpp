#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "helpers/globals.h"
#include "qt/programming/programming_host_hooks.hpp"

class QLineEdit;
class QPlainTextEdit;
class QStackedLayout;
class QTimer;
class QToolBar;
class QToolButton;

namespace aida::ui {
enum class context_menu_open_origin_t : std::uint8_t;
}

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::programming {

class AidaTaskControlsStrip;

// Per-bottom_tab_t log model: owns the version-counter copy-on-change snapshot
// of output_log::lines[tab], the in-model channel + substring filters, and the
// delta computation (appended vs resetAll) consumed by AidaOutputPane.
class AidaOutputLogChannel : public QObject {
    Q_OBJECT
public:
    AidaOutputLogChannel(bottom_tab_t tab, QObject* parent = nullptr);

    bottom_tab_t tab() const noexcept { return tab_; }
    bool tick();
    void setFilterText(const QString& text);
    QString filterText() const { return QString::fromStdString(filter_input_); }
    void setChannelFilter(const std::string& channel);
    std::string channelFilter() const { return selected_channel_; }
    bool followsTail() const noexcept { return follow_cached_; }
    bool hasContent() const noexcept { return has_content_; }
    bool snapshotAvailable() const noexcept { return snapshot_available_; }
    std::size_t totalLines() const noexcept { return total_; }
    bool snapshotEmpty() const noexcept { return snapshot_.empty(); }

Q_SIGNALS:
    void appended(const QStringList& lines);
    void resetAll(const QString& text);
    void followChanged(bool following);
    void contentStateChanged();

private:
    bool passesFilter(const output_log::entry_t& entry, QString* display) const;
    void refilterAndReset();

    bottom_tab_t tab_;
    std::uint64_t known_version_ = 0;
    std::vector<output_log::entry_t> snapshot_;
    std::size_t total_ = 0;
    std::string filter_input_;
    std::string normalized_filter_;
    std::string selected_channel_;
    bool follow_cached_ = true;
    bool has_content_ = false;
    bool snapshot_available_ = false;
};

// Toolbar + read-only QPlainTextEdit log pane (the documented
// setMaximumBlockCount + appendPlainText log pattern). Reusable: docked in the
// bottom-panel views and embedded (source debug console).
class AidaOutputPane : public QWidget {
    Q_OBJECT
public:
    AidaOutputPane(bottom_tab_t tab, const QString& instanceKey,
                 QWidget* parent = nullptr);

    void setChannel(AidaOutputLogChannel* channel);
    void focusFilterField();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildToolbar();
    void refreshPresentations();
    void updateAdaptiveFilter();
    void updateStatePage();
    void appendLines(const QStringList& lines);
    void resetText(const QString& text);
    void openContextMenu(aida::ui::context_menu_open_origin_t origin,
                         const QPoint& global_pos);

    bottom_tab_t tab_;
    AidaOutputLogChannel* channel_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QLineEdit* filter_edit_ = nullptr;
    QToolButton* filter_button_ = nullptr;
    QToolButton* wrap_button_ = nullptr;
    QPlainTextEdit* edit_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    QVector<QAction*> actions_;
    QAction* follow_action_ = nullptr;
    int adaptive_wide_width_ = 0;
};

// Registry-facing host: bottom_tab_t::output composes the programming task
// controls strip above the pane; the other tabs host the pane alone.
class AidaOutputViewHost : public QWidget {
    Q_OBJECT
public:
    AidaOutputViewHost(bottom_tab_t tab, const QString& instanceKey,
                       QWidget* parent = nullptr);

private:
    AidaOutputPane* pane_ = nullptr;
};

// Process-wide GUI-thread controller: owns the four channels, the 50 ms poll
// timer, the capability/operation surface used by the host facade (the
// output_views replacement), and the export worker.
class AidaOutputController : public QObject {
    Q_OBJECT
public:
    static AidaOutputController& instance();
    static bool exists() noexcept;

    void install(docking::AidaDockHost* host);
    docking::AidaDockHost* host() const noexcept { return host_; }

    AidaOutputLogChannel* channel(bottom_tab_t tab) const;

    bool has_content(bottom_tab_t tab);
    bool supports_filter(bottom_tab_t tab) noexcept;
    bool follows_tail(bottom_tab_t tab);
    bool source_available(bottom_tab_t tab) noexcept;

    host::operation_result_t copy_all(bottom_tab_t tab);
    host::operation_result_t clear(bottom_tab_t tab);
    host::operation_result_t select_all(bottom_tab_t tab);
    host::operation_result_t toggle_follow(bottom_tab_t tab);
    host::operation_result_t focus_filter(bottom_tab_t tab);
    host::operation_result_t export_all(bottom_tab_t tab);
    host::operation_result_t exportText(const std::string& log_label,
                                        const std::string& owner_view, std::string text);

    void noteExternalChannelChange();

Q_SIGNALS:
    void polled();
    void selectAllRequested(int tab);
    void focusFilterRequested(int tab);

private:
    explicit AidaOutputController(QObject* parent = nullptr);
    bool snapshotText(bottom_tab_t tab, std::string& text);

    docking::AidaDockHost* host_ = nullptr;
    AidaOutputLogChannel* channels_[static_cast<int>(bottom_tab_t::COUNT)] = {};
    QTimer* poll_timer_ = nullptr;
};

}
