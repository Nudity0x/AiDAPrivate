#pragma once

#include <QModelIndex>
#include <QString>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/network/burp/report_generator.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class QtReportHistoryModel : public SnapshotTableModel<aida::burp::report::generated_report_t> {
    Q_OBJECT
public:
    enum Column { Id = 0, Title, Format, Issues, Path, ColumnCount };

    explicit QtReportHistoryModel(QObject* parent = nullptr);

    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const aida::burp::report::generated_report_t& row, int column,
                      int role) const override;
};

class QtReportController : public QObject {
    Q_OBJECT
public:
    explicit QtReportController(QObject* parent = nullptr);

    void generate(const aida::burp::report::report_config_t& config);
    void clearHistory();
    void refreshHistory();

    bool generating() const noexcept { return generating_.load(); }
    QString lastAction() const { return QString::fromStdString(last_action_); }
    QString lastActionKind() const { return QString::fromStdString(last_action_kind_); }
    std::shared_ptr<const std::vector<aida::burp::report::generated_report_t>> history() const;

Q_SIGNALS:
    void historyChanged();
    void actionChanged();
    void generatingChanged(bool generating);

private:
    std::atomic<bool> generating_{false};
    std::string last_action_;
    std::string last_action_kind_;
    std::shared_ptr<const std::vector<aida::burp::report::generated_report_t>> history_ =
        std::make_shared<const std::vector<aida::burp::report::generated_report_t>>();
};

// QtReportView ports burp/report_view.cpp. Generate/Clear run on executor
// workers with queued completion; the history list refreshes on a 1s QTimer
// while the pane is visible (replacing the per-frame ImGui read).
class QtReportView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit QtReportView(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void browseOutputPath();
    void generateNow();
    void refreshStatusLine();

    QtReportController* controller_ = nullptr;
    QLineEdit* titleEdit_ = nullptr;
    QLineEdit* clientEdit_ = nullptr;
    QPlainTextEdit* scopeEdit_ = nullptr;
    QLineEdit* outputPathEdit_ = nullptr;
    QComboBox* formatCombo_ = nullptr;
    QCheckBox* evidenceCheck_ = nullptr;
    QCheckBox* remediationCheck_ = nullptr;
    QLabel* issuesAvailable_ = nullptr;
    widgets::AidaButton* generateButton_ = nullptr;
    widgets::AidaButton* clearHistoryButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QTableView* historyView_ = nullptr;
    QtReportHistoryModel* historyModel_ = nullptr;
    widgets::AidaStateView* emptyLabel_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
};

}
