#pragma once

#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "core/session/analysis_session.hpp"
#include "qt/workbench/qt_sessions_model.hpp"

class QLabel;
class QPushButton;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::workbench {

// Sessions view (07 sec. 8.4), view.sessions. The reattach flow (executor
// submission + task-center registration + feedback label) ports verbatim.
class QtSessionsView : public QWidget {
    Q_OBJECT
public:
    explicit QtSessionsView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct reattach_operation_t {
        std::uint64_t task_id = 0;
        std::uint32_t pid = 0;
        std::uint64_t process_creation_time_100ns = 0;
        std::string session_id;
        std::shared_ptr<aida::analysis::cancellation_source_t> cancellation;
        std::mutex result_mutex;
        std::string detail;
        std::atomic<bool> attached{false};
        std::atomic<bool> cancelled{false};
        std::atomic<bool> completed{false};
    };

    void poll();
    void requestReattach(const analysis_session::session_summary_t& summary);
    void requestDetach(const std::string& session_id);
    void requestClose(const std::string& session_id);
    void showRowMenu(const QPoint& global_pos, int view_row);
    void refreshPresentation();
    void presentFeedback(bool dismiss_visible);

    QtSessionsModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QPushButton* open_button_ = nullptr;
    QPushButton* attach_button_ = nullptr;
    QPushButton* run_button_ = nullptr;
    QPushButton* reattach_button_ = nullptr;
    QPushButton* detach_button_ = nullptr;
    QPushButton* close_button_ = nullptr;
    QLabel* feedback_label_ = nullptr;
    QPushButton* feedback_dismiss_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    QTimer* timer_ = nullptr;
    std::shared_ptr<reattach_operation_t> reattach_;
    std::string feedback_;
    std::string selection_error_key_;
    std::string dismissed_error_key_;
    bool feedback_error_ = false;
    bool feedback_is_selection_error_ = false;
    QMetaObject::Connection poll_connection_;
};

}
