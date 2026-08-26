#include "qt/network/intruder/intruder_view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QPointer>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedLayout>
#include <QTableView>
#include <QTextCharFormat>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "core/infra/executor.hpp"
#include "core/network/burp/intruder_view.hpp"
#include "core/network/network_view.hpp"
#include "helpers/diag_log.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/bounded_plain_text_edit.hpp"
#include "qt/network/burp_review_dialog.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/network/shared/http_highlighter.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

std::vector<std::string> split_lines(const std::string& v) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= v.size(); ++i) {
        if (i == v.size() || v[i] == '\n') {
            if (i > start) {
                std::string line = v.substr(start, i - start);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) out.push_back(line);
            }
            start = i + 1;
        }
    }
    return out;
}

void detect_positions_from_markers(const std::string& tmpl,
    std::vector<std::pair<size_t, size_t>>& positions, std::string& clean_out) {
    positions.clear();
    clean_out.clear();
    clean_out.reserve(tmpl.size());
    size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '$') {
            size_t end = tmpl.find('$', i + 1);
            if (end != std::string::npos) {
                size_t pos_in_clean = clean_out.size();
                positions.push_back({ pos_in_clean, 0 });
                i = end + 1;
                continue;
            }
        }
        clean_out.push_back(tmpl[i]);
        ++i;
    }
}

bool same_status(const aida::burp::intruder::status_t& left,
                 const aida::burp::intruder::status_t& right) {
    return left.job_id == right.job_id && left.total == right.total && left.sent == right.sent &&
        left.errors == right.errors && left.running == right.running &&
        left.started_unix_ms == right.started_unix_ms &&
        left.finished_unix_ms == right.finished_unix_ms;
}

network_view::artifact_identity_t response_identity(
    const aida::burp::intruder::result_t& result, std::uint64_t started_ms) {
    network_view::artifact_identity_t identity;
    identity.id = "intruder." + std::to_string(result.job_id) + "." +
        std::to_string(result.index) + ".response";
    identity.parent_id = "intruder." + std::to_string(result.job_id) + "." +
        std::to_string(result.index);
    identity.source_view_id = "view.network.intruder";
    identity.kind = network_view::artifact_kind_t::intruder_response;
    identity.source_id = result.job_id;
    identity.timestamp = started_ms;
    identity.revision = result.index;
    identity.content_size = result.response_raw.size();
    identity.content_hash = network_view::artifact_content_hash(result.response_raw);
    identity.label = "Intruder response #" + std::to_string(result.index);
    return identity;
}

}

IntruderJobsModel::IntruderJobsModel(QObject* parent) : QAbstractTableModel(parent) {}

int IntruderJobsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int IntruderJobsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant IntruderJobsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* row = rowAt(index.row());
    if (!row)
        return {};
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Job: return QStringLiteral("Job %1").arg(static_cast<unsigned long long>(row->job_id));
        case State: return row->running ? QStringLiteral("RUN") : QStringLiteral("DONE");
        case Sent: return QString::number(static_cast<qulonglong>(row->sent));
        case Total: return QString::number(static_cast<qulonglong>(row->total));
        case Rps: return QString::number(row->current_rps, 'f', 0);
        case Errors: return QString::number(static_cast<qulonglong>(row->errors));
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        const auto& t = theme::tokens();
        if (index.column() == State)
            return row->running ? t.accent : t.success;
        if (index.column() == Errors && row->errors > 0)
            return t.error;
        return t.text_primary;
    }
    return {};
}

QVariant IntruderJobsModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Job: return QStringLiteral("Job");
    case State: return QStringLiteral("State");
    case Sent: return QStringLiteral("Sent");
    case Total: return QStringLiteral("Total");
    case Rps: return QStringLiteral("RPS");
    case Errors: return QStringLiteral("Errors");
    default: return {};
    }
}

void IntruderJobsModel::multiData(const QModelIndex& index,
                                  QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

void IntruderJobsModel::adopt(std::vector<aida::burp::intruder::status_t> jobs) {
    beginResetModel();
    rows_ = std::move(jobs);
    endResetModel();
}

const aida::burp::intruder::status_t* IntruderJobsModel::rowAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

const aida::burp::intruder::status_t* IntruderJobsModel::findById(
    std::uint64_t jobId) const noexcept {
    for (const auto& row : rows_) {
        if (row.job_id == jobId)
            return &row;
    }
    return nullptr;
}

IntruderResultsModel::IntruderResultsModel(QObject* parent) : QAbstractTableModel(parent) {}

int IntruderResultsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int IntruderResultsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant IntruderResultsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* row = rowAt(index.row());
    if (!row)
        return {};
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Index: return QString::number(static_cast<qulonglong>(row->index));
        case Payload: {
            QString text;
            for (std::size_t i = 0; i < row->payloads.size(); ++i) {
                if (i > 0)
                    text += QLatin1Char(',');
                const auto& payload = row->payloads[i];
                text += payload.size() > 32
                    ? QString::fromStdString(payload.substr(0, 30) + "..")
                    : QString::fromStdString(payload);
            }
            return text;
        }
        case Status: return row->error ? QStringLiteral("ERR")
            : QString::number(row->status_code);
        case Length: return QString::number(static_cast<qulonglong>(row->response_size));
        case Latency: return QStringLiteral("%1ms")
            .arg(static_cast<unsigned long long>(row->latency_ms));
        case Error: return row->error ? QString::fromStdString(row->error_msg) : QString();
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (index.column()) {
        case Payload: {
            QString text;
            for (std::size_t i = 0; i < row->payloads.size(); ++i) {
                if (i > 0)
                    text += QLatin1Char(',');
                text += QString::fromStdString(row->payloads[i]);
            }
            return text;
        }
        case Error: return row->error ? QString::fromStdString(row->error_msg) : QString();
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        const auto& t = theme::tokens();
        switch (index.column()) {
        case Status: return row->error ? t.error : status_code_color(row->status_code);
        case Error: return t.error;
        case Index:
        case Payload:
        case Length:
        case Latency:
        default: return t.text_secondary;
        }
    }
    if (role == Qt::BackgroundRole) {
        if (row->error)
            return theme::tokens().error_soft;
    }
    return {};
}

QVariant IntruderResultsModel::headerData(int section, Qt::Orientation orientation,
                                          int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Index: return QStringLiteral("#");
    case Payload: return QStringLiteral("Payload");
    case Status: return QStringLiteral("Status");
    case Length: return QStringLiteral("Length");
    case Latency: return QStringLiteral("Latency");
    case Error: return QStringLiteral("Error");
    default: return {};
    }
}

void IntruderResultsModel::multiData(const QModelIndex& index,
                                     QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

bool IntruderResultsModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid())
        return false;
    return job_id_ != 0 && rows_.size() < total_;
}

void IntruderResultsModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || job_id_ == 0 || fetch_pending_.load(std::memory_order_acquire))
        return;
    if (!delivery_context_)
        return;
    const std::uint64_t generation = generation_.load(std::memory_order_acquire);
    const std::uint64_t jobId = job_id_;
    const std::size_t offset = rows_.size();
    fetch_pending_.store(true, std::memory_order_release);
    QPointer<QObject> context = delivery_context_;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.intruder";
    submission.label = "intruder.results_page";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [context, jobId, offset, generation]() {
        std::vector<aida::burp::intruder::result_t> page;
        try {
            page = aida::burp::intruder::results(jobId, offset, 128);
        } catch (...) {
            page.clear();
        }
        if (!context)
            return;
        QMetaObject::invokeMethod(context.data(),
            [context, generation, page = std::move(page)]() mutable {
                auto* model = qobject_cast<IntruderResultsModel*>(context.data());
                if (!model)
                    return;
                model->fetch_pending_.store(false, std::memory_order_release);
                if (model->generation_.load(std::memory_order_acquire) != generation)
                    return;
                if (page.empty())
                    return;
                const int first = static_cast<int>(model->rows_.size());
                const int last = first + static_cast<int>(page.size()) - 1;
                model->beginInsertRows(QModelIndex(), first, last);
                for (auto& row : page)
                    model->rows_.push_back(std::move(row));
                model->endInsertRows();
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted)
        fetch_pending_.store(false, std::memory_order_release);
}

void IntruderResultsModel::setJob(std::uint64_t jobId, std::size_t total) {
    beginResetModel();
    rows_.clear();
    job_id_ = jobId;
    total_ = total;
    generation_.fetch_add(1, std::memory_order_acq_rel);
    fetch_pending_.store(false, std::memory_order_release);
    endResetModel();
}

void IntruderResultsModel::setTotal(std::size_t total) {
    total_ = total;
}

void IntruderResultsModel::clearRows() {
    setJob(0, 0);
}

const aida::burp::intruder::result_t* IntruderResultsModel::rowAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

IntruderMarkerHighlighter::IntruderMarkerHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent) {}

void IntruderMarkerHighlighter::highlightBlock(const QString& text) {
    QTextCharFormat markerFormat;
    markerFormat.setForeground(theme::tokens().accent);
    markerFormat.setFontWeight(QFont::Bold);
    int state = previousBlockState() == 1 ? 1 : 0;
    int index = 0;
    const int length = text.length();
    while (index < length) {
        const int marker = text.indexOf(QLatin1Char('$'), index);
        if (marker < 0)
            break;
        if (state == 0) {
            const int end = text.indexOf(QLatin1Char('$'), marker + 1);
            if (end < 0) {
                setFormat(marker, length - marker, markerFormat);
                state = 1;
                index = length;
                break;
            }
            setFormat(marker, end - marker + 1, markerFormat);
            index = end + 1;
        } else {
            setFormat(0, marker, markerFormat);
            state = 0;
            index = marker + 1;
        }
    }
    if (state == 1)
        setCurrentBlockState(1);
    else
        setCurrentBlockState(0);
}

IntruderNewAttackDialog::IntruderNewAttackDialog(BurpOperationRunner* runner,
                                                 QWidget* parent)
    : aida::qt::bridge::AidaDialog(parent), runner_(runner) {
    setWindowTitle(QStringLiteral("Intruder - New Attack"));
    setModal(true);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(dialog_min_width_chars(this, 80), editor_min_height_lines(this, 26));

    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* targetRow = new QHBoxLayout();
    targetRow->setSpacing(t.spacing.sm);
    targetRow->addWidget(new QLabel(QStringLiteral("Host:"), this));
    host_edit_ = new QLineEdit(QStringLiteral("example.com"), this);
    host_edit_->setMaxLength(255);
    targetRow->addWidget(host_edit_, 1);
    targetRow->addWidget(new QLabel(QStringLiteral("Port:"), this));
    port_spin_ = new QSpinBox(this);
    port_spin_->setRange(1, 65535);
    port_spin_->setValue(443);
    targetRow->addWidget(port_spin_);
    tls_check_ = new QCheckBox(QStringLiteral("TLS"), this);
    tls_check_->setChecked(true);
    targetRow->addWidget(tls_check_);
    layout->addLayout(targetRow);

    auto* form = new QFormLayout();
    form->setSpacing(t.spacing.sm);
    attack_mode_ = new QComboBox(this);
    attack_mode_->addItems({QStringLiteral("sniper"), QStringLiteral("battering_ram"),
        QStringLiteral("pitchfork"), QStringLiteral("clusterbomb"), QStringLiteral("turbo"),
        QStringLiteral("race")});
    form->addRow(QStringLiteral("Attack mode"), attack_mode_);
    engine_mode_ = new QComboBox(this);
    engine_mode_->addItems({QStringLiteral("http1_serial"), QStringLiteral("http1_pipelined"),
        QStringLiteral("http1_pooled"), QStringLiteral("http2_multiplexed"),
        QStringLiteral("http2_single_packet")});
    engine_mode_->setCurrentIndex(2);
    form->addRow(QStringLiteral("Engine mode"), engine_mode_);
    concurrency_ = new QSpinBox(this);
    concurrency_->setRange(1, 1024);
    concurrency_->setValue(32);
    form->addRow(QStringLiteral("Concurrency"), concurrency_);
    rps_cap_ = new QSpinBox(this);
    rps_cap_->setRange(0, 1000000);
    rps_cap_->setValue(0);
    form->addRow(QStringLiteral("Throttle RPS (0=unbounded)"), rps_cap_);
    total_cap_ = new QSpinBox(this);
    total_cap_->setRange(0, 100000000);
    total_cap_->setValue(0);
    form->addRow(QStringLiteral("Total cap (0=all)"), total_cap_);
    timeout_ms_ = new QSpinBox(this);
    timeout_ms_->setRange(500, 600000);
    timeout_ms_->setValue(15000);
    form->addRow(QStringLiteral("Timeout ms"), timeout_ms_);
    race_gate_label_ = new QLabel(QStringLiteral("Race gate size"), this);
    race_gate_ = new QSpinBox(this);
    race_gate_->setRange(1, 100000);
    race_gate_->setValue(30);
    form->addRow(race_gate_label_, race_gate_);
    race_warmup_label_ = new QLabel(QStringLiteral("Race warmup"), this);
    race_warmup_ = new QSpinBox(this);
    race_warmup_->setRange(0, 100000);
    race_warmup_->setValue(0);
    form->addRow(race_warmup_label_, race_warmup_);
    layout->addLayout(form);

    auto* requestHint = new QLabel(
        QStringLiteral("Request template (mark positions with $...$):"), this);
    requestHint->setProperty("aidaTone", QStringLiteral("dim"));
    layout->addWidget(requestHint);
    request_editor_ = new QtHumanRequestEditor(this);
    QtHumanRequestEditor::Config requestConfig;
    requestConfig.stableId = QStringLiteral("intruder-new-attack");
    requestConfig.maxBytes = 65535;
    request_editor_->setConfig(requestConfig);
    request_editor_->setMinimumHeight(editor_min_height_lines(request_editor_, 7));
    layout->addWidget(request_editor_, 1);
    for (auto* edit : request_editor_->findChildren<QPlainTextEdit*>()) {
        if (edit->document() &&
            !edit->document()->findChild<IntruderMarkerHighlighter*>())
            new IntruderMarkerHighlighter(edit->document());
    }

    auto* payloadHint = new QLabel(QStringLiteral("Payload set (one per line):"), this);
    payloadHint->setProperty("aidaTone", QStringLiteral("dim"));
    layout->addWidget(payloadHint);
    payload_set_ = new BoundedPlainTextEdit(8191, this);
    payload_set_->setFont(theme::fonts::codeRegular());
    payload_set_->setPlainText(QStringLiteral("test1\ntest2\ntest3\n"));
    payload_set_->setMinimumHeight(editor_min_height_lines(payload_set_, 5));
    layout->addWidget(payload_set_);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(t.spacing.sm);
    buttons->addStretch(1);
    launch_button_ = new widgets::AidaButton(QStringLiteral("Launch"), this);
    launch_button_->setKind(widgets::AidaButton::Kind::Primary);
    buttons->addWidget(launch_button_);
    auto* cancelButton = new widgets::AidaButton(QStringLiteral("Cancel"), this);
    cancelButton->setKind(widgets::AidaButton::Kind::Ghost);
    buttons->addWidget(cancelButton);
    layout->addLayout(buttons);

    connect(attack_mode_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const bool race = index == 5;
        race_gate_label_->setVisible(race);
        race_gate_->setVisible(race);
        race_warmup_label_->setVisible(race);
        race_warmup_->setVisible(race);
    });
    race_gate_label_->setVisible(false);
    race_gate_->setVisible(false);
    race_warmup_label_->setVisible(false);
    race_warmup_->setVisible(false);

    connect(request_editor_, &QtHumanRequestEditor::validityChanged, this,
        [this](bool valid, const QString&) {
            editor_valid_ = valid;
            refreshLaunch();
        });
    connect(request_editor_, &QtHumanRequestEditor::hasUnappliedPrettyChanged, this,
        [this](bool unapplied) {
            editor_unapplied_ = unapplied;
            refreshLaunch();
        });
    if (runner_) {
        connect(runner_, &BurpOperationRunner::submitted, this, [this] { refreshLaunch(); });
        connect(runner_, &BurpOperationRunner::completed, this, [this] { refreshLaunch(); });
    }
    connect(launch_button_, &QAbstractButton::clicked, this, [this] {
        if (submit_)
            submit_(buildConfig());
        accept();
    });
    connect(cancelButton, &QAbstractButton::clicked, this, [this] { reject(); });

    request_editor_->setAuthority(QStringLiteral("intruder.new-attack.1"),
        QStringLiteral("GET / HTTP/1.1\r\nHost: example.com\r\n"
            "User-Agent: AiDA-Intruder/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n"));
    editor_valid_ = request_editor_->isValid();
    editor_unapplied_ = request_editor_->hasUnappliedPretty();
    refreshLaunch();
}

void IntruderNewAttackDialog::preset(const QString& host, int port, bool tls,
                                     const QString& rawRequest) {
    host_edit_->setText(host.left(255));
    port_spin_->setValue(qBound(1, port, 65535));
    tls_check_->setChecked(tls);
    if (++request_generation_ == 0)
        ++request_generation_;
    preset_request_ = rawRequest;
    request_editor_->setAuthority(
        QStringLiteral("intruder.new-attack.%1").arg(request_generation_), rawRequest);
    editor_valid_ = request_editor_->isValid();
    editor_unapplied_ = request_editor_->hasUnappliedPretty();
    refreshLaunch();
}

void IntruderNewAttackDialog::setSubmitHandler(
    std::function<void(const aida::burp::intruder::config_t&)> submit) {
    submit_ = std::move(submit);
}

void IntruderNewAttackDialog::refreshLaunch() {
    const bool pending = runner_ && runner_->pending();
    request_editor_->setEditable(!pending);
    launch_button_->setEnabled(!pending && editor_valid_ && !editor_unapplied_);
}

aida::burp::intruder::config_t IntruderNewAttackDialog::buildConfig() const {
    aida::burp::intruder::config_t cfg;
    cfg.host = host_edit_->text().toStdString();
    cfg.port = static_cast<uint16_t>(port_spin_->value());
    cfg.scheme = tls_check_->isChecked() ? "https" : "http";
    cfg.attack_mode = static_cast<aida::burp::intruder::attack_mode_t>(attack_mode_->currentIndex());
    cfg.engine_mode = static_cast<aida::burp::intruder::engine_mode_t>(engine_mode_->currentIndex());
    cfg.concurrency = static_cast<size_t>(std::max(1, concurrency_->value()));
    cfg.requests_per_second_cap = static_cast<size_t>(std::max(0, rps_cap_->value()));
    cfg.total_requests_cap = static_cast<size_t>(std::max(0, total_cap_->value()));
    cfg.timeout_ms = std::max(500, timeout_ms_->value());
    cfg.race_gate_size = static_cast<size_t>(std::max(1, race_gate_->value()));
    cfg.race_warmup_count = std::max(0, race_warmup_->value());

    const std::string tmpl = request_editor_->authority().toStdString();
    std::string clean;
    std::vector<std::pair<size_t, size_t>> positions;
    detect_positions_from_markers(tmpl, positions, clean);
    cfg.base_request.assign(clean.begin(), clean.end());
    cfg.positions = std::move(positions);
    if (cfg.positions.empty())
        cfg.positions.push_back({ cfg.base_request.size(), 0 });

    const std::string payloadText = payload_set_->toPlainText().toStdString();
    std::vector<std::string> payloads = split_lines(payloadText);
    cfg.payload_sets.push_back(std::move(payloads));
    return cfg;
}

IntruderView::IntruderView(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.intruder"));
    const auto& t = theme::tokens();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(t.spacing.sm);
    auto* title = new QLabel(QStringLiteral("Intruder / Turbo"), content);
    title->setProperty("aidaTone", QStringLiteral("titleAccent"));
    headerRow->addWidget(title);
    op_status_label_ = new QLabel(content);
    headerRow->addWidget(op_status_label_);
    new_attack_button_ = new widgets::AidaButton(QStringLiteral("New Attack"), content);
    new_attack_button_->setKind(widgets::AidaButton::Kind::Primary);
    new_attack_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    headerRow->addWidget(new_attack_button_);
    auto* hint = new QLabel(QStringLiteral("Mark positions in the request with $value$"), content);
    hint->setProperty("aidaTone", QStringLiteral("dim"));
    headerRow->addWidget(hint, 1);
    layout->addLayout(headerRow);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);

    auto* jobsPanel = new QWidget(splitter);
    jobsPanel->setMinimumWidth(table_column_width_chars(jobsPanel, 26));
    jobsPanel->setMaximumWidth(table_column_width_chars(jobsPanel, 46));
    auto* jobsLayout = new QVBoxLayout(jobsPanel);
    jobsLayout->setContentsMargins(0, 0, 0, 0);
    jobsLayout->setSpacing(t.spacing.xs);
    auto* jobsHeader = new QHBoxLayout();
    jobsHeader->setSpacing(t.spacing.sm);
    auto* jobsTitle = new QLabel(QStringLiteral("Jobs"), jobsPanel);
    jobsTitle->setProperty("aidaTone", QStringLiteral("secondary"));
    jobsHeader->addWidget(jobsTitle);
    jobsHeader->addStretch(1);
    stop_button_ = new widgets::AidaButton(QStringLiteral("Stop"), jobsPanel);
    stop_button_->setKind(widgets::AidaButton::Kind::Destructive);
    stop_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    jobsHeader->addWidget(stop_button_);
    clear_button_ = new widgets::AidaButton(QStringLiteral("Clear"), jobsPanel);
    clear_button_->setKind(widgets::AidaButton::Kind::Ghost);
    clear_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    jobsHeader->addWidget(clear_button_);
    jobsLayout->addLayout(jobsHeader);
    jobs_model_ = new IntruderJobsModel(jobsPanel);
    auto* jobsHost = new QWidget(jobsPanel);
    jobs_stack_ = new QStackedLayout(jobsHost);
    jobs_stack_->setStackingMode(QStackedLayout::StackOne);
    jobs_stack_->setContentsMargins(0, 0, 0, 0);
    jobs_table_ = new QTableView(jobsHost);
    jobs_table_->setObjectName(QStringLiteral("aida.view.network.intruder.jobs"));
    jobs_table_->verticalHeader()->hide();
    jobs_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    jobs_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    jobs_table_->horizontalHeader()->setStretchLastSection(true);
    jobs_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    jobs_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    jobs_table_->setAlternatingRowColors(true);
    jobs_table_->setShowGrid(false);
    jobs_table_->setModel(jobs_model_);
    jobs_stack_->addWidget(jobs_table_);
    jobs_empty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No Intruder jobs"),
        QStringLiteral("Start a New Attack, or send a request here from Proxy or Repeater."),
        jobsHost);
    jobs_empty_->setObjectName(QStringLiteral("aida.view.network.intruder.jobs_empty"));
    jobs_empty_->setActionLabel(QStringLiteral("New Attack"));
    connect(jobs_empty_, &widgets::AidaStateView::actionTriggered, this, [this] {
        if (new_attack_button_->isEnabled())
            new_attack_button_->click();
    });
    jobs_stack_->addWidget(jobs_empty_);
    jobsLayout->addWidget(jobsHost, 1);
    connect(jobs_model_, &QAbstractItemModel::modelReset, this, [this] { updateJobsEmptyState(); });
    splitter->addWidget(jobsPanel);

    auto* resultsPanel = new QWidget(splitter);
    auto* resultsLayout = new QVBoxLayout(resultsPanel);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    resultsLayout->setSpacing(t.spacing.xs);
    results_header_ = new QLabel(QStringLiteral("Select a job, or start a New Attack"),
        resultsPanel);
    results_header_->setProperty("aidaTone", QStringLiteral("dim"));
    resultsLayout->addWidget(results_header_);
    results_model_ = new IntruderResultsModel(resultsPanel);
    results_model_->delivery_context_ = results_model_;
    auto* resultsHost = new QWidget(resultsPanel);
    results_stack_ = new QStackedLayout(resultsHost);
    results_stack_->setStackingMode(QStackedLayout::StackOne);
    results_stack_->setContentsMargins(0, 0, 0, 0);
    results_table_ = new QTableView(resultsHost);
    results_table_->setObjectName(QStringLiteral("aida.view.network.intruder.results"));
    results_table_->verticalHeader()->hide();
    results_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    results_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    results_table_->horizontalHeader()->setStretchLastSection(true);
    results_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_table_->setAlternatingRowColors(true);
    results_table_->setShowGrid(false);
    results_table_->setModel(results_model_);
    results_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    results_stack_->addWidget(results_table_);
    results_empty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No retained results"),
        QStringLiteral("Results appear as the selected job sends requests; large runs page in lazily."),
        resultsHost);
    results_empty_->setObjectName(QStringLiteral("aida.view.network.intruder.results_empty"));
    results_stack_->addWidget(results_empty_);
    resultsLayout->addWidget(resultsHost, 1);
    connect(results_model_, &QAbstractItemModel::rowsInserted, this,
        [this] { updateResultsEmptyState(); });
    connect(results_model_, &QAbstractItemModel::modelReset, this,
        [this] { updateResultsEmptyState(); });
    auto* detailTitle = new QLabel(QStringLiteral("Detail"), resultsPanel);
    detailTitle->setProperty("aidaTone", QStringLiteral("secondary"));
    resultsLayout->addWidget(detailTitle);
    detail_info_ = new QLabel(resultsPanel);
    resultsLayout->addWidget(detail_info_);
    detail_view_ = new QPlainTextEdit(resultsPanel);
    detail_view_->setReadOnly(true);
    detail_view_->setFont(theme::fonts::codeRegular());
    detail_view_->setMinimumHeight(editor_min_height_lines(detail_view_, 5));
    detail_view_->setPlaceholderText(QStringLiteral("Select a result row to view the response"));
    attach_http_highlighter(detail_view_);
    resultsLayout->addWidget(detail_view_);
    splitter->addWidget(resultsPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    runner_ = new BurpOperationRunner(QStringLiteral("burp_ui"), this);

    jobs_timer_ = new QTimer(this);
    jobs_timer_->setInterval(250);
    connect(jobs_timer_, &QTimer::timeout, this, [this] { refreshJobs(); });
    status_timer_ = new QTimer(this);
    status_timer_->setInterval(500);
    connect(status_timer_, &QTimer::timeout, this, [this] {
        refreshStatusLine();
        if (results_model_->canFetchMore(QModelIndex()))
            results_model_->fetchMore(QModelIndex());
    });

    connect(new_attack_button_, &QAbstractButton::clicked, this, [this] { openNewAttack({}, 0, true, {}); });
    connect(stop_button_, &QAbstractButton::clicked, this, [this] {
        const auto* job = jobs_model_->findById(selected_job_id_);
        if (job && job->running)
            submitStop(*job);
    });
    connect(clear_button_, &QAbstractButton::clicked, this, [this] { openClearReview(); });
    connect(jobs_table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* job = jobs_model_->rowAt(current.isValid() ? current.row() : -1);
            const std::uint64_t id = job ? job->job_id : 0;
            if (id == selected_job_id_)
                return;
            selected_job_id_ = id;
            selected_result_index_ = -1;
            const auto status = id != 0 ? aida::burp::intruder::status(id)
                                        : aida::burp::intruder::status_t{};
            selected_job_started_ms_ = status.started_unix_ms;
            results_model_->setJob(id, status.total);
            refreshStatusLine();
            refreshDetail();
            refreshJobsButtons();
        });
    connect(results_table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* row = results_model_->rowAt(current.isValid() ? current.row() : -1);
            selected_result_index_ = row ? static_cast<std::int64_t>(row->index) : -1;
            refreshDetail();
        });
    connect(results_table_, &QTableView::customContextMenuRequested, this,
        &IntruderView::openResultsContextMenu);

    connect(runner_, &BurpOperationRunner::completed, this,
        [this](quint64, bool success, bool, const QString& message) {
            const std::uint64_t started = started_job_id_.exchange(0, std::memory_order_acq_rel);
            if (started != 0) {
                selected_job_id_ = started;
                selected_result_index_ = -1;
                refreshJobs();
                for (int row = 0; row < jobs_model_->rowCount(); ++row) {
                    const auto* job = jobs_model_->rowAt(row);
                    if (job && job->job_id == started) {
                        jobs_table_->setCurrentIndex(jobs_model_->index(row, 0));
                        break;
                    }
                }
                const auto status = aida::burp::intruder::status(started);
                selected_job_started_ms_ = status.started_unix_ms;
                results_model_->setJob(started, status.total);
            }
            if (success && message.contains(QStringLiteral("cleared"))) {
                if (selected_job_id_ == reviewed_clear_.job_id) {
                    selected_job_id_ = 0;
                    selected_result_index_ = -1;
                    results_model_->clearRows();
                }
            }
            refreshJobs();
            refreshStatusLine();
            refreshDetail();
            refreshJobsButtons();
            refreshOpStatus();
        });
    connect(runner_, &BurpOperationRunner::submitted, this, [this](quint64) {
        refreshOpStatus();
        refreshJobsButtons();
    });

    aida::burp::intruder_view::set_new_attack_staged_hook(
        [pane = QPointer<IntruderView>(this)] {
            if (!pane)
                return;
            QMetaObject::invokeMethod(pane.data(), [pane] { pane->drainStaged(); },
                Qt::QueuedConnection);
        });
    hooks_installed_ = true;
    drainStaged();

    refreshJobs();
    refreshOpStatus();
    refreshJobsButtons();
    refreshDetail();
    updateResultsEmptyState();
    setContent(content);
}

IntruderView::~IntruderView() {
    if (hooks_installed_)
        aida::burp::intruder_view::set_new_attack_staged_hook(nullptr);
}

void IntruderView::onPaneShown() {
    refreshJobs();
    jobs_timer_->start();
    status_timer_->start();
}

void IntruderView::onPaneHidden() {
    jobs_timer_->stop();
    status_timer_->stop();
}

void IntruderView::drainStaged() {
    aida::burp::intruder_view::staged_new_attack_t staged;
    if (!aida::burp::intruder_view::take_staged_new_attack(staged))
        return;
    openNewAttack(QString::fromStdString(staged.host), static_cast<int>(staged.port),
        staged.use_tls, QString::fromStdString(staged.raw_request));
}

void IntruderView::openNewAttack(const QString& host, int port, bool tls,
                                 const QString& rawRequest) {
    auto* dialog = new IntruderNewAttackDialog(runner_, this);
    dialog->setSubmitHandler(
        [this](const aida::burp::intruder::config_t& config) { submitStart(config); });
    if (!host.isEmpty() || !rawRequest.isEmpty())
        dialog->preset(host, port, tls, rawRequest);
    dialog->open();
}

void IntruderView::updateJobsEmptyState() {
    if (!jobs_stack_ || !jobs_empty_ || !jobs_table_ || !jobs_model_)
        return;
    jobs_stack_->setCurrentWidget(jobs_model_->rowCount() == 0
        ? static_cast<QWidget*>(jobs_empty_) : static_cast<QWidget*>(jobs_table_));
}

void IntruderView::updateResultsEmptyState() {
    if (!results_stack_ || !results_empty_ || !results_table_ || !results_model_)
        return;
    results_stack_->setCurrentWidget(results_model_->rowCount() == 0
        ? static_cast<QWidget*>(results_empty_) : static_cast<QWidget*>(results_table_));
}

void IntruderView::refreshJobs() {
    const QSignalBlocker blocker(jobs_table_->selectionModel());
    jobs_model_->adopt(aida::burp::intruder::list_jobs());
    if (selected_job_id_ != 0 && !jobs_model_->findById(selected_job_id_)) {
        selected_job_id_ = 0;
        selected_result_index_ = -1;
        results_model_->clearRows();
    }
    if (selected_job_id_ != 0) {
        for (int row = 0; row < jobs_model_->rowCount(); ++row) {
            const auto* job = jobs_model_->rowAt(row);
            if (job && job->job_id == selected_job_id_) {
                jobs_table_->setCurrentIndex(jobs_model_->index(row, 0));
                break;
            }
        }
    }
    refreshJobsButtons();
    updateJobsEmptyState();
}

void IntruderView::refreshJobsButtons() {
    const bool pending = runner_->pending();
    const auto* job = jobs_model_->findById(selected_job_id_);
    stop_button_->setEnabled(job && job->running && !pending);
    clear_button_->setEnabled(job && !job->running && !pending);
}

void IntruderView::refreshOpStatus() {
    if (runner_->pending()) {
        op_status_label_->setText(QStringLiteral("Operation running in Task Center"));
        set_label_tone(op_status_label_, "info");
        return;
    }
    if (const auto completion = runner_->completion()) {
        op_status_label_->setText(QString::fromStdString(completion->result.message));
        set_label_tone(op_status_label_, completion->result.success ? "success" : "error");
        return;
    }
    op_status_label_->clear();
}

void IntruderView::refreshStatusLine() {
    if (selected_job_id_ == 0) {
        results_header_->setText(QStringLiteral("Select a job, or start a New Attack"));
        set_label_tone(results_header_, "dim");
        return;
    }
    const auto status = aida::burp::intruder::status(selected_job_id_);
    selected_job_started_ms_ = status.started_unix_ms;
    results_model_->setTotal(status.total);
    results_header_->setText(QStringLiteral("Job %1  sent=%2/%3  errors=%4  rps=%5  %6")
        .arg(static_cast<unsigned long long>(status.job_id))
        .arg(status.sent)
        .arg(status.total)
        .arg(status.errors)
        .arg(QString::number(status.current_rps, 'f', 1))
        .arg(status.running ? QStringLiteral("RUNNING") : QStringLiteral("FINISHED")));
    set_label_tone(results_header_, status.running ? "accent" : "primary");
}

void IntruderView::refreshDetail() {
    const auto* detail = [this]() -> const aida::burp::intruder::result_t* {
        if (selected_result_index_ < 0)
            return nullptr;
        for (int i = 0; i < results_model_->rowCount(); ++i) {
            const auto* candidate = results_model_->rowAt(i);
            if (candidate && static_cast<std::int64_t>(candidate->index) == selected_result_index_)
                return candidate;
        }
        return nullptr;
    }();
    if (!detail) {
        detail_info_->setText(QStringLiteral("(select a row to view the response)"));
        set_label_tone(detail_info_, "dim");
        detail_view_->clear();
        return;
    }
    QString payJoin;
    for (const auto& payload : detail->payloads) {
        if (!payJoin.isEmpty())
            payJoin += QLatin1Char(',');
        payJoin += QString::fromStdString(payload);
    }
    detail_info_->setText(QStringLiteral("idx=%1 status=%2 len=%3 lat=%4ms payload=%5%6")
        .arg(detail->index)
        .arg(detail->status_code)
        .arg(detail->response_size)
        .arg(static_cast<unsigned long long>(detail->latency_ms))
        .arg(payJoin)
        .arg(detail->error ? QStringLiteral("  [ERR]") : QString()));
    set_label_tone(detail_info_, detail->error ? "error" : "primary");
    detail_view_->setPlainText(QString::fromStdString(detail->response_preview));
}

void IntruderView::submitStart(aida::burp::intruder::config_t config) {
    BurpRequest request;
    request.owner = QStringLiteral("burp.intruder");
    request.ownerView = QStringLiteral("view.network.intruder");
    request.ownerAction = QStringLiteral("network.intruder.start");
    request.label = QStringLiteral("Start Intruder attack");
    request.target = QString::fromStdString(config.host + ":" + std::to_string(config.port));
    request.affectedEntity = request.target;
    QPointer<IntruderView> pane(this);
    request.execute = [pane, config = std::move(config)]() mutable {
        aida::burp::ui_operation::result_t result;
        const std::uint64_t id = aida::burp::intruder::start(std::move(config));
        result.success = id != 0;
        result.message = result.success ? "Intruder attack started."
                                        : aida::burp::intruder::last_error();
        if (id != 0 && pane)
            pane->started_job_id_.store(id, std::memory_order_release);
        return result;
    };
    static_cast<void>(runner_->submit(std::move(request)));
}

void IntruderView::submitStop(aida::burp::intruder::status_t reviewed) {
    BurpRequest request;
    request.owner = QStringLiteral("burp.intruder");
    request.ownerView = QStringLiteral("view.network.intruder");
    request.ownerAction = QStringLiteral("network.intruder.stop");
    request.label = QStringLiteral("Stop Intruder attack");
    request.target = QStringLiteral("Job %1").arg(static_cast<unsigned long long>(reviewed.job_id));
    request.affectedEntity = request.target;
    request.execute = [reviewed]() {
        aida::burp::ui_operation::result_t result;
        const auto current = aida::burp::intruder::status(reviewed.job_id);
        if (current.job_id != reviewed.job_id || current.started_unix_ms != reviewed.started_unix_ms) {
            result.message = "The Intruder job changed before stop; no job was stopped.";
            return result;
        }
        result.success = aida::burp::intruder::stop(reviewed.job_id);
        result.message = result.success ? "Intruder stop requested."
                                        : aida::burp::intruder::last_error();
        return result;
    };
    static_cast<void>(runner_->submit(std::move(request)));
}

void IntruderView::submitClear(aida::burp::intruder::status_t reviewed) {
    BurpRequest request;
    request.owner = QStringLiteral("burp.intruder");
    request.ownerView = QStringLiteral("view.network.intruder");
    request.ownerAction = QStringLiteral("network.intruder.clear");
    request.label = QStringLiteral("Clear Intruder job");
    request.target = QStringLiteral("Job %1").arg(static_cast<unsigned long long>(reviewed.job_id));
    request.affectedEntity = request.target;
    request.execute = [reviewed]() {
        aida::burp::ui_operation::result_t result;
        if (!same_status(aida::burp::intruder::status(reviewed.job_id), reviewed)) {
            result.message = "The Intruder job changed after review; it was not cleared.";
            return result;
        }
        result.success = aida::burp::intruder::clear(reviewed.job_id);
        result.message = result.success ? "Intruder job cleared."
                                        : aida::burp::intruder::last_error();
        return result;
    };
    static_cast<void>(runner_->submit(std::move(request)));
}

void IntruderView::openClearReview() {
    const auto* job = jobs_model_->findById(selected_job_id_);
    if (!job || job->running)
        return;
    reviewed_clear_ = *job;
    const auto reviewed = *job;
    auto* dialog = new BurpReviewDialog(
        QStringLiteral("Review Intruder job clearing"),
        { QStringLiteral("Clear Intruder job %1 and its %2 retained results?")
            .arg(static_cast<unsigned long long>(reviewed.job_id))
            .arg(reviewed.sent),
          QStringLiteral("The exact reviewed job state will be revalidated before clearing.") },
        QStringLiteral("Clear job"), true, this);
    dialog->setRunner(runner_);
    dialog->setRevalidator([reviewed](QString& reasonOut) {
        if (!same_status(aida::burp::intruder::status(reviewed.job_id), reviewed)) {
            reasonOut = QStringLiteral(
                "The Intruder job changed after review; cancel and select again.");
            return false;
        }
        return true;
    });
    dialog->setSubmitCallback([this, reviewed] { submitClear(reviewed); });
    dialog->open();
}

void IntruderView::openResultsContextMenu(const QPoint& viewportPos) {
    const QModelIndex index = results_table_->indexAt(viewportPos);
    if (index.isValid())
        results_table_->setCurrentIndex(index);
    if (!index.isValid())
        return;
    const auto* row = results_model_->rowAt(index.row());
    if (!row)
        return;
    selected_result_index_ = static_cast<std::int64_t>(row->index);
    exchange_context_host().show(results_table_,
        results_table_->viewport()->mapToGlobal(viewportPos),
        response_identity(*row, selected_job_started_ms_), {},
        network_view::exchange_context_origin_t::pointer);
}

}
