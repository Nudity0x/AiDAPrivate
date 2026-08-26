#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QVariant>

#include <memory>

#include "core/network/burp/csp_analyzer.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QSplitter;
class QTableView;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class QtCspDirectiveModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Directive = 0, Values, ColumnCount };

    explicit QtCspDirectiveModel(QObject* parent = nullptr);

    void adopt(std::shared_ptr<const std::vector<aida::burp::csp::csp_directive_t>> rows);
    const aida::burp::csp::csp_directive_t* rowAt(int row) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    std::shared_ptr<const std::vector<aida::burp::csp::csp_directive_t>> rows_;
};

class QtCspFindingModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Severity = 0, Title, Description, Evidence, ColumnCount };

    explicit QtCspFindingModel(QObject* parent = nullptr);

    void adopt(std::shared_ptr<const std::vector<aida::burp::csp::csp_finding_t>> rows);
    const aida::burp::csp::csp_finding_t* rowAt(int row) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    std::shared_ptr<const std::vector<aida::burp::csp::csp_finding_t>> rows_;
};

// QtCspView ports burp/csp_view.cpp. csp::analyze is pure bounded CPU over at
// most 8 KiB of pasted text (no I/O), so Analyze runs synchronously on the GUI
// thread exactly as the ImGui version did per-frame; no worker is introduced.
class QtCspView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit QtCspView(QWidget* parent = nullptr);

private:
    void analyzeNow();
    void clearNow();
    void presentResult();

    QPlainTextEdit* input_ = nullptr;
    QCheckBox* reportOnly_ = nullptr;
    widgets::AidaButton* analyzeButton_ = nullptr;
    widgets::AidaButton* clearButton_ = nullptr;
    QLabel* scoreLabel_ = nullptr;
    QLabel* reportOnlyTag_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QTableView* directivesView_ = nullptr;
    QTableView* findingsView_ = nullptr;
    QtCspDirectiveModel* directiveModel_ = nullptr;
    QtCspFindingModel* findingModel_ = nullptr;
    QLabel* directivesHeader_ = nullptr;
    QLabel* findingsHeader_ = nullptr;
    widgets::AidaStateView* findingsEmpty_ = nullptr;
    QWidget* resultsHost_ = nullptr;

    aida::burp::csp::csp_result_t result_;
    bool have_result_ = false;
};

}
