#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::net {

class BoundedPlainTextEdit;
class QtHumanRequestEditor;

// OffensivePane ports render_offensive plus the offensive workflow helpers
// (network_view.cpp:5429-6005 pre-migration). offensive_redact_json runs in
// the worker before any display or log; the result reaches the pane only
// through a queued invokeMethod guarded by the run id.
class OffensivePane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit OffensivePane(QWidget* parent = nullptr);

private:
    void runClicked();
    void clearClicked();
    void stopJobClicked();
    void refreshRunControls();
    void rebuildIssueLinks(const std::string& resultJson);

    QComboBox* workflow_combo_ = nullptr;
    QCheckBox* scope_check_ = nullptr;
    QSpinBox* timeout_spin_ = nullptr;
    QSpinBox* payloads_spin_ = nullptr;
    QSpinBox* requests_spin_ = nullptr;
    QLineEdit* url_edit_ = nullptr;
    QLineEdit* param_edit_ = nullptr;
    BoundedPlainTextEdit* payload_edit_ = nullptr;
    QtHumanRequestEditor* raw_editor_ = nullptr;
    widgets::AidaButton* run_button_ = nullptr;
    widgets::AidaButton* clear_button_ = nullptr;
    widgets::AidaButton* stop_job_button_ = nullptr;
    widgets::AidaButton* cancel_button_ = nullptr;
    QLabel* run_state_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QWidget* issue_row_host_ = nullptr;
    QHBoxLayout* issue_row_ = nullptr;
    QPlainTextEdit* result_view_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> run_id_{0};
    std::atomic<std::uint64_t> active_fuzz_job_id_{0};
    std::string status_ = "Idle";
    std::string result_;
};

}
