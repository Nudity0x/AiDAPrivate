#include "qt/net/qt_csp_view.hpp"

#include <QCheckBox>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <string>
#include <utility>
#include <vector>

#include "helpers/diag_log.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

QColor cspSeverityColor(const std::string& severity, const QColor& fallback)
{
    const auto& t = theme::tokens();
    if (severity == "high")   return t.error;
    if (severity == "medium") return t.warning;
    if (severity == "low")    return t.info;
    if (severity == "info")   return t.text_secondary;
    return fallback;
}

}

QtCspDirectiveModel::QtCspDirectiveModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtCspDirectiveModel::adopt(
    std::shared_ptr<const std::vector<aida::burp::csp::csp_directive_t>> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const aida::burp::csp::csp_directive_t* QtCspDirectiveModel::rowAt(int row) const noexcept
{
    if (!rows_ || row < 0 || row >= static_cast<int>(rows_->size()))
        return nullptr;
    return &rows_->at(static_cast<std::size_t>(row));
}

int QtCspDirectiveModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() || !rows_ ? 0 : static_cast<int>(rows_->size());
}

int QtCspDirectiveModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant QtCspDirectiveModel::data(const QModelIndex& index, int role) const
{
    const auto* row = rowAt(index.isValid() ? index.row() : -1);
    if (!row)
        return {};
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Directive: return QString::fromStdString(row->name);
        case Values: {
            if (row->values.empty())
                return QStringLiteral("(empty)");
            QString joined;
            for (const auto& value : row->values) {
                if (!joined.isEmpty())
                    joined += QLatin1Char(' ');
                joined += QString::fromStdString(value);
            }
            return joined;
        }
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (index.column()) {
        case Directive: return t.accent;
        case Values: return row->values.empty() ? t.text_dim : t.text_primary;
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole && index.column() == Values && !row->values.empty())
        return data(index, Qt::DisplayRole);
    return {};
}

QVariant QtCspDirectiveModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Directive: return QStringLiteral("Directive");
    case Values:    return QStringLiteral("Values");
    default: return {};
    }
}

QtCspFindingModel::QtCspFindingModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtCspFindingModel::adopt(
    std::shared_ptr<const std::vector<aida::burp::csp::csp_finding_t>> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const aida::burp::csp::csp_finding_t* QtCspFindingModel::rowAt(int row) const noexcept
{
    if (!rows_ || row < 0 || row >= static_cast<int>(rows_->size()))
        return nullptr;
    return &rows_->at(static_cast<std::size_t>(row));
}

int QtCspFindingModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() || !rows_ ? 0 : static_cast<int>(rows_->size());
}

int QtCspFindingModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant QtCspFindingModel::data(const QModelIndex& index, int role) const
{
    const auto* row = rowAt(index.isValid() ? index.row() : -1);
    if (!row)
        return {};
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Severity:    return QStringLiteral("[%1]").arg(QString::fromStdString(row->severity));
        case Title:       return QString::fromStdString(row->title);
        case Description: return QString::fromStdString(row->description);
        case Evidence:    return row->evidence.empty() ? QVariant{}
            : QStringLiteral("evidence: %1").arg(QString::fromStdString(row->evidence));
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (index.column()) {
        case Severity:    return cspSeverityColor(row->severity, t.text_primary);
        case Title:       return cspSeverityColor(row->severity, t.text_primary);
        case Description: return t.text_secondary;
        case Evidence:    return t.text_dim;
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (index.column()) {
        case Title:       return QString::fromStdString(row->title);
        case Description: return QString::fromStdString(row->description);
        case Evidence:    return row->evidence.empty() ? QVariant{}
            : QString::fromStdString(row->evidence);
        default: return {};
        }
    }
    return {};
}

QVariant QtCspFindingModel::headerData(int section, Qt::Orientation orientation,
                                       int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Severity:    return QStringLiteral("Severity");
    case Title:       return QStringLiteral("Finding");
    case Description: return QStringLiteral("Description");
    case Evidence:    return QStringLiteral("Evidence");
    default: return {};
    }
}

QtCspView::QtCspView(QWidget* parent)
    : NetworkPaneBase(parent)
{
    setObjectName(QStringLiteral("view.network.csp"));
    setRequiresTarget(false);

    const auto& t = theme::tokens();
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* header = new QLabel(QStringLiteral("CSP Analyzer"), content);
    header->setProperty("aidaTone", QStringLiteral("title"));
    layout->addWidget(header);

    auto* hint = new QLabel(QStringLiteral("Paste Content-Security-Policy header value:"),
        content);
    hint->setProperty("aidaTone", QStringLiteral("secondary"));
    layout->addWidget(hint);

    auto* input = new QtByteCappedPlainTextEdit(content);
    input->setObjectName(QStringLiteral("view.network.csp.input"));
    input->setMaxBytes(8191);
    input->setTabChangesFocus(false);
    input->setFont(theme::fonts::codeRegular());
    input->setMinimumHeight(editor_min_height_lines(input, 4));
    input->setMaximumHeight(editor_min_height_lines(input, 6));
    layout->addWidget(input);
    input_ = input;

    auto* controls = new QHBoxLayout();
    controls->setSpacing(t.spacing.sm);
    reportOnly_ = new QCheckBox(QStringLiteral("Report-only"), content);
    controls->addWidget(reportOnly_);
    analyzeButton_ = new widgets::AidaButton(QStringLiteral("Analyze"), content);
    analyzeButton_->setKind(widgets::AidaButton::Kind::Primary);
    analyzeButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controls->addWidget(analyzeButton_);
    clearButton_ = new widgets::AidaButton(QStringLiteral("Clear"), content);
    clearButton_->setKind(widgets::AidaButton::Kind::Secondary);
    clearButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controls->addWidget(clearButton_);
    controls->addStretch(1);
    layout->addLayout(controls);

    resultsHost_ = new QWidget(content);
    auto* resultsLayout = new QVBoxLayout(resultsHost_);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    resultsLayout->setSpacing(t.spacing.sm);

    auto* scoreRow = new QHBoxLayout();
    scoreRow->setSpacing(t.spacing.xs);
    scoreLabel_ = new QLabel(resultsHost_);
    scoreLabel_->setProperty("aidaTone", QStringLiteral("title"));
    scoreRow->addWidget(scoreLabel_);
    reportOnlyTag_ = new QLabel(QStringLiteral("[report-only]"), resultsHost_);
    reportOnlyTag_->setProperty("aidaTone", QStringLiteral("warning"));
    scoreRow->addWidget(reportOnlyTag_);
    scoreRow->addStretch(1);
    resultsLayout->addLayout(scoreRow);

    splitter_ = new QSplitter(Qt::Horizontal, resultsHost_);
    splitter_->setOpaqueResize(true);
    splitter_->setChildrenCollapsible(false);

    auto* directivesPane = new QWidget(splitter_);
    auto* directivesLayout = new QVBoxLayout(directivesPane);
    directivesLayout->setContentsMargins(0, 0, 0, 0);
    directivesLayout->setSpacing(t.spacing.xs);
    directivesHeader_ = new QLabel(directivesPane);
    directivesHeader_->setObjectName(QStringLiteral("view.network.csp.directives_header"));
    directivesHeader_->setProperty("aidaTone", QStringLiteral("primary"));
    directivesLayout->addWidget(directivesHeader_);
    directiveModel_ = new QtCspDirectiveModel(directivesPane);
    directivesView_ = new QTableView(directivesPane);
    directivesView_->setObjectName(QStringLiteral("view.network.csp.directives"));
    directivesView_->setModel(directiveModel_);
    directivesView_->verticalHeader()->hide();
    directivesView_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    directivesView_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    directivesView_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    directivesView_->setAlternatingRowColors(true);
    directivesView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    directivesView_->setSelectionMode(QAbstractItemView::NoSelection);
    directivesLayout->addWidget(directivesView_, 1);

    auto* findingsPane = new QWidget(splitter_);
    auto* findingsLayout = new QVBoxLayout(findingsPane);
    findingsLayout->setContentsMargins(0, 0, 0, 0);
    findingsLayout->setSpacing(t.spacing.xs);
    findingsHeader_ = new QLabel(findingsPane);
    findingsHeader_->setObjectName(QStringLiteral("view.network.csp.findings_header"));
    findingsHeader_->setProperty("aidaTone", QStringLiteral("primary"));
    findingsLayout->addWidget(findingsHeader_);
    findingsEmpty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Success,
        QStringLiteral("No findings. Looks tight."), QString(), findingsPane);
    findingsLayout->addWidget(findingsEmpty_);
    findingModel_ = new QtCspFindingModel(findingsPane);
    findingsView_ = new QTableView(findingsPane);
    findingsView_->setObjectName(QStringLiteral("view.network.csp.findings"));
    findingsView_->setModel(findingModel_);
    findingsView_->verticalHeader()->hide();
    findingsView_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    findingsView_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    findingsView_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    findingsView_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    findingsView_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    findingsView_->setAlternatingRowColors(true);
    findingsView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    findingsView_->setSelectionMode(QAbstractItemView::NoSelection);
    findingsView_->setWordWrap(true);
    findingsLayout->addWidget(findingsView_, 1);

    splitter_->addWidget(directivesPane);
    splitter_->addWidget(findingsPane);
    splitter_->setSizes({ 450, 550 });
    resultsLayout->addWidget(splitter_, 1);
    layout->addWidget(resultsHost_, 1);
    resultsHost_->setVisible(false);

    connect(analyzeButton_, &QAbstractButton::clicked, this, &QtCspView::analyzeNow);
    connect(clearButton_, &QAbstractButton::clicked, this, &QtCspView::clearNow);

    setContent(content);
}

void QtCspView::analyzeNow()
{
    const std::string source = input_->toPlainText().toStdString();
    const bool reportOnly = reportOnly_->isChecked();
    ::diag::log_tagged_fmt("csp_v", "analyze report_only=%d input_len=%zu",
        reportOnly ? 1 : 0, source.size());
    result_ = aida::burp::csp::analyze(source, reportOnly);
    have_result_ = true;
    ::diag::log_tagged_fmt("csp_v",
        "analyze_result score=%d has_csp=%d directives=%zu findings=%zu",
        result_.score, result_.has_csp ? 1 : 0, result_.directives.size(),
        result_.findings.size());
    presentResult();
}

void QtCspView::clearNow()
{
    ::diag::log_tagged("csp_v", "clear");
    input_->clear();
    result_ = aida::burp::csp::csp_result_t{};
    have_result_ = false;
    presentResult();
}

void QtCspView::presentResult()
{
    resultsHost_->setVisible(have_result_);
    if (!have_result_)
        return;

    const int score = result_.score;
    const char* scoreTone = "titleSuccess";
    if (score < 70 && score >= 40)
        scoreTone = "titleWarning";
    if (score < 40)
        scoreTone = "titleError";
    if (!result_.has_csp)
        scoreTone = "titleError";
    scoreLabel_->setText(QStringLiteral("Score: %1 / 100").arg(score));
    set_label_tone(scoreLabel_, scoreTone);
    reportOnlyTag_->setVisible(result_.is_report_only);

    const auto directives = std::make_shared<const std::vector<aida::burp::csp::csp_directive_t>>(
        result_.directives);
    directiveModel_->adopt(directives);
    directivesHeader_->setText(QStringLiteral("Directives (%1)")
        .arg(static_cast<quint64>(directives->size())));

    const auto findings = std::make_shared<const std::vector<aida::burp::csp::csp_finding_t>>(
        result_.findings);
    findingModel_->adopt(findings);
    findingsHeader_->setText(QStringLiteral("Findings (%1)")
        .arg(static_cast<quint64>(findings->size())));
    findingsEmpty_->setVisible(findings->empty());
    findingsView_->setVisible(!findings->empty());
}

}
