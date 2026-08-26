#include "qt/network/burp/logger_pane.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QEvent>
#include <QWidget>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QSpinBox>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <memory>
#include <string>
#include <vector>

#include "core/infra/executor.hpp"
#include "core/network/burp/burp_logger.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

LoggerModel::LoggerModel(QObject* parent)
    : SnapshotTableModel(parent) {}

int LoggerModel::rowCount(const QModelIndex& parent) const {
    return SnapshotTableModel::rowCount(parent);
}

int LoggerModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant LoggerModel::cellData(const aida::burp::logger::log_row_t& row, int column,
                               int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Id:      return QString::number(static_cast<quint64>(row.id));
        case Time:    return QString::number(static_cast<quint64>(row.ts_ms));
        case Method:  return QString::fromStdString(row.method);
        case Url:     return QString::fromStdString(row.url);
        case Status:  return row.status;
        case Length:  return QString::number(static_cast<quint64>(row.response_length));
        case Latency: return QString::number(static_cast<quint64>(row.latency_ms));
        case Source:  return QString::fromLatin1(aida::burp::logger::source_label(row.source));
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole && column == Url)
        return QString::fromStdString(row.url);
    if (role == Qt::ForegroundRole) {
        if (column == Status) {
            switch (status_code_semantic(row.status)) {
            case net_semantic_t::success: return t.success;
            case net_semantic_t::warning: return t.warning;
            case net_semantic_t::error:   return t.error;
            default: return t.text_primary;
            }
        }
        return t.text_primary;
    }
    return {};
}

QVariant LoggerModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Id:      return QStringLiteral("ID");
    case Time:    return QStringLiteral("Time");
    case Method:  return QStringLiteral("Method");
    case Url:     return QStringLiteral("URL");
    case Status:  return QStringLiteral("Status");
    case Length:  return QStringLiteral("Length");
    case Latency: return QStringLiteral("Latency");
    case Source:  return QStringLiteral("Source");
    default: return {};
    }
}

LoggerPane::LoggerPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.logger"));
    setRequiresTarget(false);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding_compact, t.panel.padding_compact,
        t.panel.padding_compact, t.panel.padding_compact);
    layout->setSpacing(t.spacing.xs);

    auto* filtersRow = new QHBoxLayout();
    filtersRow->setSpacing(t.spacing.xs);
    filtersRow->addWidget(new QLabel("Filters:", content));
    methodEdit_ = new QLineEdit(content);
    methodEdit_->setPlaceholderText("method");
    methodEdit_->setMaxLength(15);
    methodEdit_->setMaximumWidth(field_width_chars(methodEdit_, 8));
    filtersRow->addWidget(methodEdit_);
    hostEdit_ = new QLineEdit(content);
    hostEdit_->setPlaceholderText("host regex");
    hostEdit_->setMaxLength(255);
    hostEdit_->setMaximumWidth(field_width_chars(hostEdit_, 22));
    filtersRow->addWidget(hostEdit_);
    urlEdit_ = new QLineEdit(content);
    urlEdit_->setPlaceholderText("url regex");
    urlEdit_->setMaxLength(511);
    filtersRow->addWidget(urlEdit_, 1);
    statusMin_ = new QSpinBox(content);
    statusMin_->setRange(0, 599);
    statusMin_->setValue(0);
    statusMin_->setMaximumWidth(spinbox_width_digits(statusMin_, 5));
    filtersRow->addWidget(statusMin_);
    filtersRow->addWidget(new QLabel("<= status <=", content));
    statusMax_ = new QSpinBox(content);
    statusMax_->setRange(0, 599);
    statusMax_->setValue(599);
    statusMax_->setMaximumWidth(spinbox_width_digits(statusMax_, 5));
    filtersRow->addWidget(statusMax_);
    sourceEdit_ = new QLineEdit(content);
    sourceEdit_->setPlaceholderText("source");
    sourceEdit_->setMaxLength(31);
    sourceEdit_->setMaximumWidth(field_width_chars(sourceEdit_, 12));
    filtersRow->addWidget(sourceEdit_);
    mimeEdit_ = new QLineEdit(content);
    mimeEdit_->setPlaceholderText("mime");
    mimeEdit_->setMaxLength(63);
    mimeEdit_->setMaximumWidth(field_width_chars(mimeEdit_, 16));
    filtersRow->addWidget(mimeEdit_);
    limitSpin_ = new QSpinBox(content);
    limitSpin_->setRange(1, 100000);
    limitSpin_->setValue(1000);
    limitSpin_->setMaximumWidth(spinbox_width_digits(limitSpin_, 7));
    filtersRow->addWidget(limitSpin_);
    clearButton_ = new widgets::AidaButton("Clear", content);
    clearButton_->setKind(widgets::AidaButton::Kind::Destructive);
    clearButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    filtersRow->addWidget(clearButton_);
    layout->addLayout(filtersRow);

    auto* exportRow = new QHBoxLayout();
    exportRow->setSpacing(t.spacing.xs);
    exportPathEdit_ = new QLineEdit(content);
    exportPathEdit_->setPlaceholderText("Export CSV path...");
    exportPathEdit_->setMaxLength(511);
    exportRow->addWidget(exportPathEdit_, 1);
    exportButton_ = new widgets::AidaButton("Export CSV", content);
    exportButton_->setKind(widgets::AidaButton::Kind::Secondary);
    exportButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    exportRow->addWidget(exportButton_);
    statusLabel_ = new QLabel(content);
    statusLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    exportRow->addWidget(statusLabel_, 1);
    layout->addLayout(exportRow);

    model_ = new LoggerModel(content);
    auto* tableHost = new QWidget(content);
    tableStack_ = new QStackedLayout(tableHost);
    tableStack_->setStackingMode(QStackedLayout::StackOne);
    tableStack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.logger.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    table_->horizontalHeader()->setDefaultSectionSize(table_column_width_chars(table_, 9));
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setModel(model_);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->viewport()->installEventFilter(this);
    tableStack_->addWidget(table_);
    emptyView_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No log rows"),
        QStringLiteral("Traffic captured by the proxy or sent by repeater/scanner/API will appear here."),
        tableHost);
    emptyView_->setObjectName(QStringLiteral("aida.view.network.logger.empty"));
    tableStack_->addWidget(emptyView_);
    tableStack_->setCurrentWidget(emptyView_);
    layout->addWidget(tableHost, 1);

    const auto filterEdited = [this] { onFilterEdited(); };
    connect(methodEdit_, &QLineEdit::textChanged, this, filterEdited);
    connect(hostEdit_, &QLineEdit::textChanged, this, filterEdited);
    connect(urlEdit_, &QLineEdit::textChanged, this, filterEdited);
    connect(sourceEdit_, &QLineEdit::textChanged, this, filterEdited);
    connect(mimeEdit_, &QLineEdit::textChanged, this, filterEdited);
    connect(statusMin_, &QSpinBox::valueChanged, this, filterEdited);
    connect(statusMax_, &QSpinBox::valueChanged, this, filterEdited);
    connect(limitSpin_, &QSpinBox::valueChanged, this, filterEdited);
    connect(clearButton_, &QAbstractButton::clicked, this, [this] { clearLog(); });
    connect(exportButton_, &QAbstractButton::clicked, this, [this] { exportCsv(); });
    connect(table_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = table_->indexAt(pos);
        if (index.isValid())
            table_->setCurrentIndex(index);
        if (index.isValid())
            showContextForRow(index.row(), table_->viewport()->mapToGlobal(pos),
                aida::ui::context_menu_open_origin_t::pointer);
    });

    filterDebounce_ = new QTimer(this);
    filterDebounce_->setSingleShot(true);
    filterDebounce_->setInterval(150);
    connect(filterDebounce_, &QTimer::timeout, this, [this] {
        submitQuery();
    });

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(100);
    connect(pollTimer_, &QTimer::timeout, this, [this] {
        pollGeneration();
    });

    setContent(content);
    submitQuery();
}

LoggerPane::~LoggerPane() = default;

void LoggerPane::onPaneShown() {
    pollTimer_->start();
    pollGeneration();
}

void LoggerPane::onPaneHidden() {
    pollTimer_->stop();
    filterDebounce_->stop();
}

bool LoggerPane::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_->viewport() && event->type() == QEvent::ContextMenu) {
        auto* contextEvent = static_cast<QContextMenuEvent*>(event);
        if (contextEvent->reason() == QContextMenuEvent::Keyboard) {
            const QModelIndex current = table_->selectionModel()->currentIndex();
            if (!current.isValid())
                return false;
            const auto* row = model_->rowAt(current.row());
            if (!row)
                return false;
            showContextForRow(current.row(), table_->viewport()->mapToGlobal(
                table_->visualRect(current).center()),
                aida::ui::context_menu_open_origin_t::menu_key);
            return true;
        }
    }
    return NetworkPaneBase::eventFilter(watched, event);
}

void LoggerPane::onFilterEdited() {
    ++filterSerial_;
    filterDebounce_->start();
}

void LoggerPane::pollGeneration() {
    const std::uint64_t generation = aida::burp::logger::generation();
    if (generation != lastSeenGeneration_) {
        lastSeenGeneration_ = generation;
        submitQuery();
    }
}

void LoggerPane::submitQuery() {
    if (inFlightSerial_ != 0 && inFlightSerial_ == filterSerial_)
        return;
    aida::burp::logger::log_filter_t filter;
    filter.method = methodEdit_->text().toStdString();
    filter.host_regex = hostEdit_->text().toStdString();
    filter.url_regex = urlEdit_->text().toStdString();
    filter.source = sourceEdit_->text().toStdString();
    filter.mime_type = mimeEdit_->text().toStdString();
    filter.status_min = statusMin_->value();
    filter.status_max = statusMax_->value();
    const std::size_t limit = static_cast<std::size_t>(limitSpin_->value());
    const std::uint64_t serial = filterSerial_;
    const std::uint64_t generation = lastSeenGeneration_;
    inFlightSerial_ = serial;

    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.logger_view";
    submission.label = "logger.query";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [this, filter = std::move(filter), limit, serial, generation]() mutable {
        auto rows = std::make_shared<const std::vector<aida::burp::logger::log_row_t>>(
            aida::burp::logger::query(filter, limit));
        const std::size_t total = aida::burp::logger::total_rows();
        const std::size_t capacity = aida::burp::logger::capacity();
        QMetaObject::invokeMethod(this,
            [this, rows = std::move(rows), serial, generation, total, capacity]() mutable {
                applyQueryResult(std::move(rows), serial, generation, total, capacity);
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted)
        inFlightSerial_ = 0;
}

void LoggerPane::applyQueryResult(
    std::shared_ptr<const std::vector<aida::burp::logger::log_row_t>> rows,
    std::uint64_t filterSerial, std::uint64_t generation, std::size_t total,
    std::size_t capacity) {
    inFlightSerial_ = 0;
    if (filterSerial != filterSerial_ || generation != lastSeenGeneration_)
        return;
    totalRows_ = total;
    capacity_ = capacity;
    ++queryGeneration_;
    auto qtRows = std::make_shared<QVector<aida::burp::logger::log_row_t>>();
    if (rows) {
        qtRows->reserve(static_cast<qsizetype>(rows->size()));
        for (const auto& row : *rows)
            qtRows->push_back(row);
    }
    model_->adopt(std::move(qtRows), queryGeneration_);
    statusLabel_->setText(QStringLiteral("Total: %1 rows (cap %2)")
        .arg(static_cast<quint64>(totalRows_)).arg(static_cast<quint64>(capacity_)));
    set_label_tone(statusLabel_, "dim");
    tableStack_->setCurrentWidget(model_->rowCount() == 0
        ? static_cast<QWidget*>(emptyView_) : static_cast<QWidget*>(table_));
}

void LoggerPane::exportCsv() {
    const std::string path = exportPathEdit_->text().toStdString();
    if (path.empty())
        return;
    aida::burp::logger::log_filter_t filter;
    filter.method = methodEdit_->text().toStdString();
    filter.host_regex = hostEdit_->text().toStdString();
    filter.url_regex = urlEdit_->text().toStdString();
    filter.source = sourceEdit_->text().toStdString();
    filter.mime_type = mimeEdit_->text().toStdString();
    filter.status_min = statusMin_->value();
    filter.status_max = statusMax_->value();

    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.logger_view";
    submission.label = "logger.export_csv";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [this, path, filter = std::move(filter)]() mutable {
        const bool ok = aida::burp::logger::export_csv(path, filter);
        const QString result = ok ? QStringLiteral("Exported %1").arg(QString::fromStdString(path))
            : QString::fromStdString(aida::burp::logger::last_error());
        QMetaObject::invokeMethod(this, [this, ok, result]() mutable {
            statusLabel_->setText(result);
            set_label_tone(statusLabel_, ok ? "success" : "error");
        }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void LoggerPane::clearLog() {
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.logger_view";
    submission.label = "logger.clear";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [] {
        aida::burp::logger::clear();
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
    pollTimer_->start();
}

void LoggerPane::showContextForRow(int row, const QPoint& globalPos,
                                   aida::ui::context_menu_open_origin_t origin) {
    const auto* logRow = model_->rowAt(row);
    if (!logRow)
        return;
    network_view::artifact_identity_t requestIdentity;
    network_view::artifact_identity_t responseIdentity;
    std::string reason;
    static_cast<void>(network_view::make_sitemap_artifact(logRow->exchange_id,
        network_view::artifact_kind_t::sitemap_request, requestIdentity, reason));
    static_cast<void>(network_view::make_sitemap_artifact(logRow->exchange_id,
        network_view::artifact_kind_t::sitemap_response, responseIdentity, reason));
    exchange_context_host().show(table_, globalPos, std::move(requestIdentity),
        std::move(responseIdentity),
        origin == aida::ui::context_menu_open_origin_t::pointer
            ? network_view::exchange_context_origin_t::pointer
            : network_view::exchange_context_origin_t::menu_key);
}

}
