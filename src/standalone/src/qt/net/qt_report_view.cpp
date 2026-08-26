#include "qt/net/qt_report_view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

#include "core/infra/executor.hpp"
#include "core/network/burp/issue.hpp"
#include "helpers/diag_log.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

const char* k_format_items[] = { "html", "markdown", "json", "sarif_2_1_0", "csv" };

aida::burp::report::report_format_t formatFromIndex(int index)
{
    switch (index) {
    case 0: return aida::burp::report::report_format_t::html;
    case 1: return aida::burp::report::report_format_t::markdown;
    case 2: return aida::burp::report::report_format_t::json;
    case 3: return aida::burp::report::report_format_t::sarif_2_1;
    case 4: return aida::burp::report::report_format_t::csv;
    }
    return aida::burp::report::report_format_t::html;
}

}

QtReportHistoryModel::QtReportHistoryModel(QObject* parent)
    : SnapshotTableModel(parent) {}

int QtReportHistoryModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant QtReportHistoryModel::cellData(
    const aida::burp::report::generated_report_t& row, int column, int role) const
{
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Id:     return QStringLiteral("[%1]").arg(static_cast<quint64>(row.id));
        case Title:  return row.title.empty() ? QStringLiteral("(untitled)")
                                              : QString::fromStdString(row.title);
        case Format: return QString::fromStdString(
            aida::burp::report::format_label(row.format));
        case Issues: return QStringLiteral("%1 issues")
            .arg(static_cast<quint64>(row.issue_count));
        case Path:   return QString::fromStdString(row.output_path);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole)
        return column == Path ? t.text_dim : t.text_secondary;
    if (role == Qt::ToolTipRole && column == Path)
        return QString::fromStdString(row.output_path);
    return {};
}

QVariant QtReportHistoryModel::headerData(int section, Qt::Orientation orientation,
                                          int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Id:     return QStringLiteral("ID");
    case Title:  return QStringLiteral("Title");
    case Format: return QStringLiteral("Format");
    case Issues: return QStringLiteral("Issues");
    case Path:   return QStringLiteral("Output");
    default: return {};
    }
}

QtReportController::QtReportController(QObject* parent)
    : QObject(parent) {}

std::shared_ptr<const std::vector<aida::burp::report::generated_report_t>>
QtReportController::history() const
{
    return std::atomic_load_explicit(&history_, std::memory_order_acquire);
}

void QtReportController::refreshHistory()
{
    auto rows = std::make_shared<const std::vector<aida::burp::report::generated_report_t>>(
        aida::burp::report::list_reports());
    std::atomic_store_explicit(&history_, std::move(rows), std::memory_order_release);
    Q_EMIT historyChanged();
}

void QtReportController::generate(const aida::burp::report::report_config_t& config)
{
    if (generating_.exchange(true, std::memory_order_acq_rel))
        return;
    Q_EMIT generatingChanged(true);
    QPointer<QtReportController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.report_view";
    submission.label = "report.generate";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [guard, config]() {
        std::string out;
        const bool ok = aida::burp::report::generate(config, out);
        ::diag::log_tagged_fmt("report_v", "generate_result ok=%d path='%s'",
            ok ? 1 : 0, out.c_str());
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(), [guard, ok, out]() mutable {
            auto* self = guard.data();
            if (!self)
                return;
            self->last_action_kind_ = ok ? "ok" : "error";
            self->last_action_ = ok ? std::string("Generated: ") + out : out;
            self->generating_.store(false, std::memory_order_release);
            Q_EMIT self->actionChanged();
            Q_EMIT self->generatingChanged(false);
            self->refreshHistory();
        }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        generating_.store(false, std::memory_order_release);
        last_action_kind_ = "error";
        last_action_ = "The bounded operation queue rejected the request.";
        Q_EMIT actionChanged();
        Q_EMIT generatingChanged(false);
    }
}

void QtReportController::clearHistory()
{
    ::diag::log_tagged("report_v", "clear_history");
    QPointer<QtReportController> guard(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.report_view";
    submission.label = "report.clear_history";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [guard]() {
        aida::burp::report::clear_history();
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(), [guard]() {
            if (auto* self = guard.data())
                self->refreshHistory();
        }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

QtReportView::QtReportView(QWidget* parent)
    : NetworkPaneBase(parent)
{
    setObjectName(QStringLiteral("view.network.reports"));
    setRequiresTarget(false);

    const auto& t = theme::tokens();
    controller_ = new QtReportController(this);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);

    auto* left = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    leftLayout->setSpacing(t.spacing.xs);
    auto* leftTitle = new QLabel(QStringLiteral("Report configuration:"), left);
    leftTitle->setProperty("aidaTone", QStringLiteral("secondary"));
    leftLayout->addWidget(leftTitle);

    const auto fieldLabel = [](QWidget* parent, const QString& text) {
        auto* label = new QLabel(text, parent);
        label->setProperty("aidaTone", QStringLiteral("secondary"));
        return label;
    };
    leftLayout->addWidget(fieldLabel(left, QStringLiteral("Title")));
    titleEdit_ = new QLineEdit(left);
    titleEdit_->setObjectName(QStringLiteral("view.network.reports.title"));
    titleEdit_->setMaxLength(255);
    titleEdit_->setPlaceholderText(QStringLiteral("Engagement title"));
    leftLayout->addWidget(titleEdit_);
    leftLayout->addWidget(fieldLabel(left, QStringLiteral("Client")));
    clientEdit_ = new QLineEdit(left);
    clientEdit_->setObjectName(QStringLiteral("view.network.reports.client"));
    clientEdit_->setMaxLength(255);
    clientEdit_->setPlaceholderText(QStringLiteral("Client name"));
    leftLayout->addWidget(clientEdit_);
    leftLayout->addWidget(fieldLabel(left, QStringLiteral("Scope summary")));
    auto* scope = new QtByteCappedPlainTextEdit(left);
    scope->setObjectName(QStringLiteral("view.network.reports.scope"));
    scope->setMaxBytes(2047);
    scope->setMinimumHeight(editor_min_height_lines(scope, 3));
    scope->setMaximumHeight(editor_min_height_lines(scope, 5));
    leftLayout->addWidget(scope);
    scopeEdit_ = scope;
    leftLayout->addWidget(fieldLabel(left, QStringLiteral("Output path")));
    auto* pathRow = new QHBoxLayout();
    pathRow->setSpacing(t.spacing.xs);
    outputPathEdit_ = new QLineEdit(left);
    outputPathEdit_->setObjectName(QStringLiteral("view.network.reports.path"));
    outputPathEdit_->setMaxLength(1023);
    outputPathEdit_->setPlaceholderText(QStringLiteral("C:\\reports\\engagement.html"));
    pathRow->addWidget(outputPathEdit_, 1);
    auto* browseButton = new widgets::AidaButton(QStringLiteral("Browse..."), left);
    browseButton->setKind(widgets::AidaButton::Kind::Secondary);
    browseButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    pathRow->addWidget(browseButton);
    leftLayout->addLayout(pathRow);
    leftLayout->addWidget(fieldLabel(left, QStringLiteral("Format")));
    formatCombo_ = new QComboBox(left);
    formatCombo_->setObjectName(QStringLiteral("view.network.reports.format"));
    for (const char* item : k_format_items)
        formatCombo_->addItem(QString::fromLatin1(item));
    formatCombo_->setMaximumWidth(field_width_chars(formatCombo_, 20));
    leftLayout->addWidget(formatCombo_);
    auto* flagsRow = new QHBoxLayout();
    flagsRow->setSpacing(t.spacing.sm);
    evidenceCheck_ = new QCheckBox(QStringLiteral("Include evidence"), left);
    evidenceCheck_->setChecked(true);
    flagsRow->addWidget(evidenceCheck_);
    remediationCheck_ = new QCheckBox(QStringLiteral("Include remediation"), left);
    remediationCheck_->setChecked(true);
    flagsRow->addWidget(remediationCheck_);
    flagsRow->addStretch(1);
    leftLayout->addLayout(flagsRow);
    issuesAvailable_ = new QLabel(left);
    issuesAvailable_->setProperty("aidaTone", QStringLiteral("dim"));
    leftLayout->addWidget(issuesAvailable_);
    generateButton_ = new widgets::AidaButton(QStringLiteral("Generate report"), left);
    generateButton_->setObjectName(QStringLiteral("view.network.reports.generate"));
    generateButton_->setKind(widgets::AidaButton::Kind::Primary);
    generateButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    leftLayout->addWidget(generateButton_);
    statusLabel_ = new QLabel(left);
    statusLabel_->setWordWrap(true);
    leftLayout->addWidget(statusLabel_);
    leftLayout->addStretch(1);

    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    rightLayout->setSpacing(t.spacing.xs);
    auto* rightHeader = new QHBoxLayout();
    rightHeader->setSpacing(t.spacing.sm);
    auto* rightTitle = new QLabel(QStringLiteral("Generated reports:"), right);
    rightTitle->setProperty("aidaTone", QStringLiteral("secondary"));
    rightHeader->addWidget(rightTitle);
    rightHeader->addStretch(1);
    clearHistoryButton_ = new widgets::AidaButton(QStringLiteral("Clear history"), right);
    clearHistoryButton_->setKind(widgets::AidaButton::Kind::Ghost);
    clearHistoryButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    rightHeader->addWidget(clearHistoryButton_);
    rightLayout->addLayout(rightHeader);

    historyModel_ = new QtReportHistoryModel(right);
    historyView_ = new QTableView(right);
    historyView_->setModel(historyModel_);
    historyView_->verticalHeader()->hide();
    historyView_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    historyView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    historyView_->horizontalHeader()->setStretchLastSection(true);
    historyView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    historyView_->setSelectionMode(QAbstractItemView::SingleSelection);
    historyView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    historyView_->setAlternatingRowColors(true);
    rightLayout->addWidget(historyView_, 1);

    emptyLabel_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No reports yet"),
        QStringLiteral("Use the left panel to configure and generate a report."), right);
    rightLayout->addWidget(emptyLabel_);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 45);
    splitter->setStretchFactor(1, 55);
    layout->addWidget(splitter, 1);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(1000);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        controller_->refreshHistory();
        issuesAvailable_->setText(QStringLiteral("Issues available: %1")
            .arg(static_cast<quint64>(aida::burp::issue_store::count())));
    });

    connect(browseButton, &QAbstractButton::clicked, this,
        &QtReportView::browseOutputPath);
    connect(generateButton_, &QAbstractButton::clicked, this, &QtReportView::generateNow);
    connect(clearHistoryButton_, &QAbstractButton::clicked, this,
        [this] { controller_->clearHistory(); });
    connect(controller_, &QtReportController::historyChanged, this, [this] {
        const auto history = controller_->history();
        historyModel_->adopt(std::make_shared<const QVector<aida::burp::report::generated_report_t>>(
            history->begin(), history->end()), historyModel_->generation() + 1);
        emptyLabel_->setVisible(history->empty());
        historyView_->setVisible(!history->empty());
    });
    connect(controller_, &QtReportController::actionChanged, this,
        &QtReportView::refreshStatusLine);
    connect(controller_, &QtReportController::generatingChanged, this, [this](bool) {
        refreshStatusLine();
    });

    setContent(content);
    controller_->refreshHistory();
    issuesAvailable_->setText(QStringLiteral("Issues available: %1")
        .arg(static_cast<quint64>(aida::burp::issue_store::count())));
}

void QtReportView::onPaneShown()
{
    controller_->refreshHistory();
    issuesAvailable_->setText(QStringLiteral("Issues available: %1")
        .arg(static_cast<quint64>(aida::burp::issue_store::count())));
    refreshTimer_->start();
}

void QtReportView::onPaneHidden()
{
    refreshTimer_->stop();
}

void QtReportView::browseOutputPath()
{
    const int formatIndex = formatCombo_->currentIndex();
    const auto format = formatFromIndex(formatIndex);
    const QString extension = QString::fromLatin1(
        aida::burp::report::default_extension(format));
    const QString filter = QStringLiteral("%1 files (*.%2);;All files (*)")
        .arg(QString::fromLatin1(k_format_items[formatIndex]), extension);
    const QString selected = QFileDialog::getSaveFileName(this,
        QStringLiteral("Select report output"), outputPathEdit_->text(), filter);
    if (!selected.isEmpty())
        outputPathEdit_->setText(selected);
}

void QtReportView::generateNow()
{
    aida::burp::report::report_config_t config;
    config.title = titleEdit_->text().toStdString();
    config.client = clientEdit_->text().toStdString();
    config.scope_summary = scopeEdit_->toPlainText().toStdString();
    config.output_path = outputPathEdit_->text().toStdString();
    config.format = formatFromIndex(formatCombo_->currentIndex());
    config.include_evidence = evidenceCheck_->isChecked();
    config.include_remediation = remediationCheck_->isChecked();
    ::diag::log_tagged_fmt("report_v",
        "generate_report title='%s' format=%d path='%s' issues=%zu",
        config.title.c_str(), formatCombo_->currentIndex(), config.output_path.c_str(),
        aida::burp::issue_store::count());
    controller_->generate(config);
}

void QtReportView::refreshStatusLine()
{
    generateButton_->setEnabled(!controller_->generating());
    const QString message = controller_->lastAction();
    statusLabel_->setText(message);
    if (message.isEmpty()) {
        set_label_tone(statusLabel_, "secondary");
        return;
    }
    const bool error = controller_->lastActionKind() == QStringLiteral("error");
    set_label_tone(statusLabel_, error ? "error" : "success");
}

}
