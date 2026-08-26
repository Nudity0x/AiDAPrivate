#include "qt/network/sequencer/sequencer_view.hpp"

#include <QCheckBox>
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
#include <QTimer>
#include <QVBoxLayout>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/network/burp/sequencer_view.hpp"
#include "helpers/diag_log.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/burp_review_dialog.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

SequencerCollectionsModel::SequencerCollectionsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int SequencerCollectionsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int SequencerCollectionsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant SequencerCollectionsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* row = rowAt(index.row());
    if (!row)
        return {};
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Id: return QString::number(static_cast<unsigned long long>(row->id));
        case Name: return QString::fromStdString(row->name.empty() ? row->url : row->name);
        case Progress: return QStringLiteral("%1/%2").arg(row->collected).arg(row->target);
        case State: return row->running ? QStringLiteral("RUN")
            : (row->error ? QStringLiteral("ERR") : QStringLiteral("DONE"));
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        const auto& t = theme::tokens();
        if (index.column() == State)
            return row->running ? t.accent : (row->error ? t.error : t.success);
        return t.text_primary;
    }
    return {};
}

QVariant SequencerCollectionsModel::headerData(int section, Qt::Orientation orientation,
                                               int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Id: return QStringLiteral("ID");
    case Name: return QStringLiteral("Name");
    case Progress: return QStringLiteral("Progress");
    case State: return QStringLiteral("State");
    default: return {};
    }
}

void SequencerCollectionsModel::multiData(const QModelIndex& index,
                                          QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

void SequencerCollectionsModel::adopt(aida::burp::sequencer::registry_snapshot_t snapshot) {
    beginResetModel();
    generation_ = snapshot.generation;
    capacity_ = snapshot.capacity;
    rows_ = std::move(snapshot.collections);
    endResetModel();
}

const aida::burp::sequencer::collection_status_t*
SequencerCollectionsModel::rowAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

const aida::burp::sequencer::collection_status_t*
SequencerCollectionsModel::findById(std::uint64_t id) const noexcept {
    for (const auto& row : rows_) {
        if (row.id == id)
            return &row;
    }
    return nullptr;
}

SequencerHistogram::SequencerHistogram(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumHeight(theme::tokens().grid * 30);
    setMaximumHeight(theme::tokens().grid * 30);
}

void SequencerHistogram::setAnalysis(
    const std::shared_ptr<const aida::burp::sequencer::analysis_result_t>& analysis) {
    if (analysis_ == analysis)
        return;
    analysis_ = analysis;
    update();
}

QSize SequencerHistogram::sizeHint() const {
    const auto& t = theme::tokens();
    return QSize(t.grid * 80, t.grid * 30);
}

void SequencerHistogram::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    const auto& t = theme::tokens();
    const QRect rect = this->rect();
    painter.fillRect(rect, t.panel_header);
    if (!analysis_)
        return;
    std::size_t maxFreq = 0;
    for (std::size_t i = 0; i < 256; ++i)
        maxFreq = (std::max)(maxFreq, analysis_->byte_frequency[i]);
    if (maxFreq == 0)
        maxFreq = 1;
    const qreal barWidth = static_cast<qreal>(rect.width()) / 256.0;
    QColor barColor = t.accent;
    barColor.setAlphaF(0.85);
    painter.setPen(Qt::NoPen);
    painter.setBrush(barColor);
    const int height = rect.height();
    for (int i = 0; i < 256; ++i) {
        const qreal barHeight = height *
            (static_cast<qreal>(analysis_->byte_frequency[static_cast<std::size_t>(i)]) /
             static_cast<qreal>(maxFreq));
        if (barHeight <= 0.0)
            continue;
        painter.fillRect(QRectF(rect.left() + i * barWidth,
                                rect.bottom() - barHeight,
                                (std::max)(barWidth - 0.5, 0.5), barHeight),
                         barColor);
    }
    (void)event;
}

SequencerView::SequencerView(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.sequencer"));
    const auto& t = theme::tokens();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);

    auto* configPanel = new QWidget(splitter);
    auto* configLayout = new QVBoxLayout(configPanel);
    configLayout->setContentsMargins(0, 0, 0, 0);
    configLayout->setSpacing(t.spacing.sm);
    auto* configTitle = new QLabel(QStringLiteral("Collection"), configPanel);
    configTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    configLayout->addWidget(configTitle);
    auto* form = new QFormLayout();
    form->setSpacing(t.spacing.sm);
    name_edit_ = new QLineEdit(QStringLiteral("Collection"), configPanel);
    name_edit_->setMaxLength(127);
    form->addRow(QStringLiteral("Name"), name_edit_);
    url_edit_ = new QLineEdit(QStringLiteral("https://example.com/login"), configPanel);
    url_edit_->setMaxLength(1023);
    form->addRow(QStringLiteral("URL"), url_edit_);
    auto* hostRow = new QWidget(configPanel);
    auto* hostRowLayout = new QHBoxLayout(hostRow);
    hostRowLayout->setContentsMargins(0, 0, 0, 0);
    hostRowLayout->setSpacing(t.spacing.sm);
    host_edit_ = new QLineEdit(QStringLiteral("example.com"), hostRow);
    host_edit_->setMaxLength(255);
    hostRowLayout->addWidget(host_edit_, 1);
    port_spin_ = new QSpinBox(hostRow);
    port_spin_->setRange(1, 65535);
    port_spin_->setValue(443);
    hostRowLayout->addWidget(port_spin_);
    tls_check_ = new QCheckBox(QStringLiteral("TLS"), hostRow);
    tls_check_->setChecked(true);
    hostRowLayout->addWidget(tls_check_);
    form->addRow(QStringLiteral("Host"), hostRow);
    regex_edit_ = new QLineEdit(QStringLiteral("Set-Cookie:\\s*session=([A-Za-z0-9]+)"), configPanel);
    regex_edit_->setMaxLength(511);
    form->addRow(QStringLiteral("Extract regex"), regex_edit_);
    auto* numbersRow = new QWidget(configPanel);
    auto* numbersLayout = new QHBoxLayout(numbersRow);
    numbersLayout->setContentsMargins(0, 0, 0, 0);
    numbersLayout->setSpacing(t.spacing.sm);
    numbersLayout->addWidget(new QLabel(QStringLiteral("Group"), numbersRow));
    group_spin_ = new QSpinBox(numbersRow);
    group_spin_->setRange(0, 1000000);
    group_spin_->setValue(1);
    numbersLayout->addWidget(group_spin_);
    numbersLayout->addWidget(new QLabel(QStringLiteral("Target"), numbersRow));
    target_spin_ = new QSpinBox(numbersRow);
    target_spin_->setRange(1, 250000);
    target_spin_->setValue(200);
    numbersLayout->addWidget(target_spin_);
    numbersLayout->addWidget(new QLabel(QStringLiteral("Threads"), numbersRow));
    threads_spin_ = new QSpinBox(numbersRow);
    threads_spin_->setRange(1, 64);
    threads_spin_->setValue(4);
    numbersLayout->addWidget(threads_spin_);
    numbersLayout->addWidget(new QLabel(QStringLiteral("Throttle ms"), numbersRow));
    throttle_spin_ = new QSpinBox(numbersRow);
    throttle_spin_->setRange(0, 60000);
    throttle_spin_->setValue(0);
    numbersLayout->addWidget(throttle_spin_);
    numbersLayout->addStretch(1);
    form->addRow(numbersRow);
    configLayout->addLayout(form);

    use_raw_check_ = new QCheckBox(QStringLiteral("Use raw request"), configPanel);
    configLayout->addWidget(use_raw_check_);
    raw_editor_ = new QtHumanRequestEditor(configPanel);
    QtHumanRequestEditor::Config rawConfig;
    rawConfig.stableId = QStringLiteral("sequencer-raw-request");
    rawConfig.maxBytes = 8191;
    rawConfig.editable = true;
    raw_editor_->setConfig(rawConfig);
    raw_editor_->setVisible(false);
    raw_editor_->setMinimumHeight(editor_min_height_lines(raw_editor_, 5));
    configLayout->addWidget(raw_editor_);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(t.spacing.sm);
    start_button_ = new widgets::AidaButton(QStringLiteral("Start"), configPanel);
    start_button_->setKind(widgets::AidaButton::Kind::Primary);
    start_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(start_button_);
    stop_button_ = new widgets::AidaButton(QStringLiteral("Stop"), configPanel);
    stop_button_->setKind(widgets::AidaButton::Kind::Destructive);
    stop_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(stop_button_);
    analyze_button_ = new widgets::AidaButton(QStringLiteral("Analyze"), configPanel);
    analyze_button_->setKind(widgets::AidaButton::Kind::Secondary);
    analyze_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(analyze_button_);
    delete_button_ = new widgets::AidaButton(QStringLiteral("Delete"), configPanel);
    delete_button_->setKind(widgets::AidaButton::Kind::Ghost);
    delete_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    buttonRow->addWidget(delete_button_);
    buttonRow->addStretch(1);
    configLayout->addLayout(buttonRow);

    capacity_label_ = new QLabel(configPanel);
    capacity_label_->setProperty("aidaTone", QStringLiteral("accent"));
    configLayout->addWidget(capacity_label_);
    start_error_label_ = new QLabel(configPanel);
    start_error_label_->setProperty("aidaTone", QStringLiteral("error"));
    start_error_label_->setWordWrap(true);
    start_error_label_->setVisible(false);
    configLayout->addWidget(start_error_label_);

    model_ = new SequencerCollectionsModel(configPanel);
    auto* tableHost = new QWidget(configPanel);
    table_stack_ = new QStackedLayout(tableHost);
    table_stack_->setStackingMode(QStackedLayout::StackOne);
    table_stack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.sequencer.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setModel(model_);
    table_stack_->addWidget(table_);
    empty_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No collections"),
        QStringLiteral("Configure a target above and click Start to capture tokens for analysis."),
        tableHost);
    empty_view_->setObjectName(QStringLiteral("aida.view.network.sequencer.empty"));
    table_stack_->addWidget(empty_view_);
    configLayout->addWidget(tableHost, 1);
    connect(model_, &QAbstractItemModel::modelReset, this, [this] { updateEmptyState(); });
    splitter->addWidget(configPanel);

    auto* rightPanel = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(t.spacing.sm);
    status_label_ = new QLabel(rightPanel);
    status_label_->setProperty("aidaTone", QStringLiteral("secondary"));
    rightLayout->addWidget(status_label_);
    error_label_ = new QLabel(rightPanel);
    error_label_->setProperty("aidaTone", QStringLiteral("error"));
    error_label_->setWordWrap(true);
    error_label_->setVisible(false);
    rightLayout->addWidget(error_label_);
    verdict_label_ = new QLabel(rightPanel);
    verdict_label_->setProperty("aidaTone", QStringLiteral("titleAccent"));
    rightLayout->addWidget(verdict_label_);
    fips_label_ = new QLabel(rightPanel);
    fips_label_->setProperty("aidaTone", QStringLiteral("dim"));
    rightLayout->addWidget(fips_label_);

    auto* statsWidget = new QWidget(rightPanel);
    stats_form_ = new QFormLayout(statsWidget);
    stats_form_->setSpacing(t.spacing.xs);
    const char* metricNames[9] = {
        "Shannon entropy bits/byte", "Chi-square", "Chi-square p-value",
        "Monobit p-value", "Poker p-value", "Runs p-value", "Long-run p-value",
        "Maurer's Universal", "Autocorrelation (lag-1)"
    };
    for (int i = 0; i < 9; ++i) {
        stats_values_[i] = new QLabel(statsWidget);
        stats_form_->addRow(QString::fromLatin1(metricNames[i]), stats_values_[i]);
    }
    stats_bits_ = new QLabel(statsWidget);
    stats_form_->addRow(QStringLiteral("Bitstream length (bits)"), stats_bits_);
    stats_ones_ = new QLabel(statsWidget);
    stats_form_->addRow(QStringLiteral("Ones / Zeros"), stats_ones_);
    rightLayout->addWidget(statsWidget);

    auto* histogramTitle = new QLabel(QStringLiteral("Byte frequency"), rightPanel);
    histogramTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    rightLayout->addWidget(histogramTitle);
    histogram_ = new SequencerHistogram(rightPanel);
    rightLayout->addWidget(histogram_);
    notes_label_ = new QLabel(rightPanel);
    notes_label_->setWordWrap(true);
    rightLayout->addWidget(notes_label_);
    auto* samplesTitle = new QLabel(QStringLiteral("Recent samples"), rightPanel);
    samplesTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    rightLayout->addWidget(samplesTitle);
    samples_ = new QPlainTextEdit(rightPanel);
    samples_->setReadOnly(true);
    samples_->setFont(theme::fonts::codeRegular());
    samples_->setPlaceholderText(QStringLiteral("Captured token samples appear after collection"));
    rightLayout->addWidget(samples_, 1);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter, 1);

    connect(use_raw_check_, &QCheckBox::toggled, this, [this](bool checked) {
        raw_editor_->setVisible(checked);
        refreshStartGating();
    });
    connect(raw_editor_, &QtHumanRequestEditor::validityChanged, this,
        [this](bool, const QString&) { refreshStartGating(); });
    connect(raw_editor_, &QtHumanRequestEditor::hasUnappliedPrettyChanged, this,
        [this](bool) { refreshStartGating(); });

    connect(start_button_, &QAbstractButton::clicked, this, [this] { startPressed(); });
    connect(stop_button_, &QAbstractButton::clicked, this, [this] { stopSelected(); });
    connect(analyze_button_, &QAbstractButton::clicked, this, [this] { analyzeSelected(); });
    connect(delete_button_, &QAbstractButton::clicked, this, [this] { deleteSelected(); });

    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* row = model_->rowAt(current.isValid() ? current.row() : -1);
            const std::uint64_t id = row ? row->id : 0;
            if (id == selected_id_)
                return;
            selected_id_ = id;
            analysis_valid_ = false;
            if (row) {
                diag::log_tagged_fmt("sequencer_v", "collection_selected id=%llu name='%s'",
                    static_cast<unsigned long long>(row->id),
                    row->name.empty() ? row->url.c_str() : row->name.c_str());
            }
            refreshRightSide();
            refreshSamples();
            refreshStartGating();
        });

    collections_timer_ = new QTimer(this);
    collections_timer_->setInterval(250);
    connect(collections_timer_, &QTimer::timeout, this, [this] { refreshCollections(); });
    samples_timer_ = new QTimer(this);
    samples_timer_->setInterval(500);
    connect(samples_timer_, &QTimer::timeout, this, [this] { refreshSamples(); });

    aida::burp::sequencer_view::set_new_collection_staged_hook(
        [pane = QPointer<SequencerView>(this)] {
            if (!pane)
                return;
            QMetaObject::invokeMethod(pane.data(), [pane] { pane->drainStaged(); },
                Qt::QueuedConnection);
        });
    hooks_installed_ = true;
    drainStaged();

    refreshCollections();
    refreshRightSide();
    refreshStartGating();
    updateEmptyState();
    setContent(content);
}

SequencerView::~SequencerView() {
    if (hooks_installed_)
        aida::burp::sequencer_view::set_new_collection_staged_hook(nullptr);
}

void SequencerView::updateEmptyState() {
    if (!table_stack_ || !empty_view_ || !table_ || !model_)
        return;
    table_stack_->setCurrentWidget(model_->rowCount() == 0
        ? static_cast<QWidget*>(empty_view_) : static_cast<QWidget*>(table_));
}

void SequencerView::onPaneShown() {
    refreshCollections();
    collections_timer_->start();
    samples_timer_->start();
}

void SequencerView::onPaneHidden() {
    collections_timer_->stop();
    samples_timer_->stop();
}

void SequencerView::drainStaged() {
    aida::burp::sequencer_view::staged_new_collection_t staged;
    if (!aida::burp::sequencer_view::take_staged_new_collection(staged))
        return;
    openNewCollectionWith(QString::fromStdString(staged.url),
        QString::fromStdString(staged.host), static_cast<int>(staged.port),
        staged.use_tls, QString::fromStdString(staged.raw_request));
}

void SequencerView::openNewCollectionWith(const QString& url, const QString& host, int port,
                                          bool useTls, const QString& rawRequest) {
    url_edit_->setText(url.left(1023));
    host_edit_->setText(host.left(255));
    port_spin_->setValue(qBound(1, port, 65535));
    tls_check_->setChecked(useTls);
    use_raw_check_->setChecked(true);
    if (++staged_generation_ == 0)
        ++staged_generation_;
    raw_editor_->setAuthority(
        QStringLiteral("sequencer.raw-request.%1").arg(staged_generation_), rawRequest);
    refreshStartGating();
}

void SequencerView::refreshCollections() {
    const std::uint64_t generation = aida::burp::sequencer::registry_generation();
    if (generation == cached_generation_)
        return;
    auto snapshot = aida::burp::sequencer::snapshot_collections();
    cached_generation_ = snapshot.generation;
    const QSignalBlocker blocker(table_->selectionModel());
    model_->adopt(std::move(snapshot));
    if (selected_id_ != 0) {
        const auto* selected = model_->findById(selected_id_);
        if (!selected) {
            selected_id_ = 0;
            analysis_valid_ = false;
            analysis_for_id_ = 0;
            histogram_->setAnalysis(nullptr);
            refreshRightSide();
        } else {
            for (int row = 0; row < model_->rowCount(); ++row) {
                const auto* candidate = model_->rowAt(row);
                if (candidate && candidate->id == selected_id_) {
                    table_->setCurrentIndex(model_->index(row, 0));
                    break;
                }
            }
        }
    }
    const auto& capacity = model_->capacity();
    capacity_label_->setText(QStringLiteral("Collections  %1 / %2    Retained %3 / %4 MiB")
        .arg(capacity.collection_count)
        .arg(capacity.collection_limit)
        .arg(QString::number(static_cast<double>(capacity.retained_sample_bytes) /
            (1024.0 * 1024.0), 'f', 1))
        .arg(QString::number(static_cast<double>(capacity.retained_sample_limit) /
            (1024.0 * 1024.0), 'f', 1)));
    refreshStartGating();
    refreshRightSide();
}

void SequencerView::refreshStartGating() {
    const auto& capacity = model_->capacity();
    const bool collectionFull = capacity.collection_count >= capacity.collection_limit;
    const bool sampleFull = capacity.retained_sample_bytes >= capacity.retained_sample_limit;
    const bool requestUnready = use_raw_check_->isChecked() &&
        (!raw_editor_->isValid() || raw_editor_->hasUnappliedPretty());
    start_button_->setEnabled(!collectionFull && !sampleFull && !requestUnready);
    const auto startError = std::atomic_load_explicit(&start_error_, std::memory_order_acquire);
    if (collectionFull || sampleFull || (startError && !startError->empty())) {
        const QString message = collectionFull
            ? QStringLiteral("Collection capacity reached. Delete a retained collection before starting another.")
            : sampleFull
                ? QStringLiteral("Retained sample capacity reached. Delete a collection before starting another.")
                : QString::fromStdString(*startError);
        start_error_label_->setText(message);
        start_error_label_->setVisible(true);
    } else {
        start_error_label_->clear();
        start_error_label_->setVisible(false);
    }
    const bool hasSelection = selected_id_ != 0;
    stop_button_->setEnabled(hasSelection);
    analyze_button_->setEnabled(hasSelection);
    delete_button_->setEnabled(hasSelection);
}

void SequencerView::refreshRightSide() {
    if (selected_id_ == 0) {
        status_label_->setText(QStringLiteral(
            "Configure a URL and an extraction regex on the left, click Start, then Analyze."));
        error_label_->clear();
        error_label_->setVisible(false);
        verdict_label_->clear();
        fips_label_->clear();
        set_label_tone(fips_label_, "dim");
        for (auto* label : stats_values_)
            label->clear();
        stats_bits_->clear();
        stats_ones_->clear();
        notes_label_->clear();
        histogram_->setAnalysis(nullptr);
        return;
    }
    const auto status = aida::burp::sequencer::status(selected_id_);
    status_label_->setText(QStringLiteral("Collection %1  '%2'  collected=%3/%4  running=%5")
        .arg(static_cast<unsigned long long>(status.id))
        .arg(QString::fromStdString(status.name.empty() ? status.url : status.name))
        .arg(status.collected)
        .arg(status.target)
        .arg(status.running ? 1 : 0));
    if (!status.error_message.empty()) {
        diag::log_tagged_fmt("sequencer_v", "collection_error id=%llu msg='%s'",
            static_cast<unsigned long long>(status.id), status.error_message.c_str());
        error_label_->setText(QStringLiteral("Error: %1")
            .arg(QString::fromStdString(status.error_message)));
        error_label_->setVisible(true);
    } else {
        error_label_->clear();
        error_label_->setVisible(false);
    }

    if (analysis_valid_ && analysis_for_id_ == selected_id_) {
        const auto& a = last_analysis_;
        verdict_label_->setText(QStringLiteral("Verdict: %1")
            .arg(QString::fromStdString(a.verdict)));
        fips_label_->setText(QStringLiteral("FIPS 140-2: %1    samples=%2 length_mode=%3")
            .arg(a.passes_fips_140_2 ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
            .arg(a.samples_count)
            .arg(a.token_length_mode));
        set_label_tone(fips_label_, a.passes_fips_140_2 ? "success" : "error");
        const double values[9] = {
            a.shannon_entropy_bits, a.chi_square, a.chi_square_p_value,
            a.monobit_p_value, a.poker_p_value, a.runs_p_value, a.long_run_p_value,
            a.maurer_universal, a.autocorrelation
        };
        const char* formats[9] = { "%.4f", "%.2f", "%.4f", "%.4f", "%.4f", "%.4f",
            "%.4f", "%.4f", "%.4f" };
        for (int i = 0; i < 9; ++i)
            stats_values_[i]->setText(std::isnan(values[i])
                ? QStringLiteral("NaN") : QString::asprintf(formats[i], values[i]));
        stats_bits_->setText(QString::number(static_cast<qulonglong>(a.total_bits)));
        stats_ones_->setText(QStringLiteral("%1 / %2").arg(a.monobit_ones).arg(a.monobit_zeros));
        notes_label_->setText(QString::fromStdString(a.notes));
        samples_->clear();
    } else {
        verdict_label_->setText(QStringLiteral(
            "Click Analyze to compute entropy and FIPS 140-2 metrics over the captured tokens."));
        fips_label_->clear();
        set_label_tone(fips_label_, "dim");
        for (auto* label : stats_values_)
            label->clear();
        stats_bits_->clear();
        stats_ones_->clear();
        notes_label_->clear();
    }
}

void SequencerView::refreshSamples() {
    if (selected_id_ == 0 || (analysis_valid_ && analysis_for_id_ == selected_id_))
        return;
    const auto snap = aida::burp::sequencer::samples(selected_id_, 32);
    QString text;
    for (std::size_t i = snap.size(); i > 0; --i) {
        text += QStringLiteral("%1: ").arg(i);
        text += QString::fromStdString(snap[i - 1]);
        text += QLatin1Char('\n');
    }
    if (text != samples_->toPlainText())
        samples_->setPlainText(text);
}

bool SequencerView::submitViewTask(aida::infra::executor::submission_t submission,
                                   const char* action) {
    try {
        const auto result = aida::infra::executor::submit(std::move(submission));
        if (result.submitted)
            return true;
        publishViewError(std::string(action) + " was rejected by the executor");
    } catch (const std::exception& exception) {
        publishViewError(std::string(action) + " failed: " + exception.what());
    } catch (...) {
        publishViewError(std::string(action) + " failed with an unknown exception");
    }
    return false;
}

void SequencerView::publishViewError(std::string error) {
    std::atomic_store_explicit(&start_error_,
        std::make_shared<const std::string>(std::move(error)), std::memory_order_release);
}

void SequencerView::startPressed() {
    aida::burp::sequencer::collection_config_t cfg;
    cfg.url = url_edit_->text().toStdString();
    cfg.use_tls = tls_check_->isChecked();
    cfg.host = host_edit_->text().toStdString();
    cfg.port = static_cast<std::uint16_t>(port_spin_->value());
    cfg.extract_regex = regex_edit_->text().toStdString();
    cfg.capture_group = group_spin_->value();
    cfg.target_count = static_cast<size_t>(std::max(1, target_spin_->value()));
    cfg.concurrency = static_cast<size_t>(std::max(1, threads_spin_->value()));
    cfg.throttle_ms = static_cast<size_t>(std::max(0, throttle_spin_->value()));
    cfg.name = name_edit_->text().toStdString();
    if (use_raw_check_->isChecked()) {
        const std::string raw = raw_editor_->authority().toStdString();
        if (!raw.empty())
            cfg.raw_request.assign(raw.begin(), raw.end());
    }
    QPointer<SequencerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.sequencer_view";
    submission.label = "sequencer.start_collection";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, cfg = std::move(cfg)]() {
        const std::uint64_t id = aida::burp::sequencer::start_collection(cfg);
        if (!pane)
            return;
        if (id != 0) {
            pane->started_id_.store(id, std::memory_order_release);
            std::atomic_store_explicit(&pane->start_error_,
                std::make_shared<const std::string>(), std::memory_order_release);
        } else {
            std::atomic_store_explicit(&pane->start_error_,
                std::make_shared<const std::string>(aida::burp::sequencer::last_error()),
                std::memory_order_release);
        }
        QMetaObject::invokeMethod(pane.data(), [pane]() {
            const std::uint64_t startedId = pane->started_id_.exchange(0, std::memory_order_acq_rel);
            if (startedId != 0) {
                pane->selected_id_ = startedId;
                pane->analysis_valid_ = false;
                pane->refreshCollections();
                pane->refreshRightSide();
                pane->refreshSamples();
            }
            pane->refreshStartGating();
        }, Qt::QueuedConnection);
    };
    static_cast<void>(submitViewTask(std::move(submission), "Start collection"));
}

void SequencerView::stopSelected() {
    if (selected_id_ == 0)
        return;
    diag::log_tagged_fmt("sequencer_v", "stop_collection id=%llu",
        static_cast<unsigned long long>(selected_id_));
    const std::uint64_t id = selected_id_;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.sequencer_view";
    submission.label = "sequencer.stop_collection";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [id]() { aida::burp::sequencer::stop_collection(id); };
    static_cast<void>(submitViewTask(std::move(submission), "Stop collection"));
}

void SequencerView::analyzeSelected() {
    if (selected_id_ == 0)
        return;
    diag::log_tagged_fmt("sequencer_v", "analyze_collection id=%llu",
        static_cast<unsigned long long>(selected_id_));
    const std::uint64_t id = selected_id_;
    QPointer<SequencerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.sequencer_view";
    submission.label = "sequencer.analyze_collection";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, id]() {
        auto publication = std::make_shared<const std::pair<std::uint64_t,
            aida::burp::sequencer::analysis_result_t>>(id, aida::burp::sequencer::analyze(id));
        if (!pane)
            return;
        std::atomic_store_explicit(&pane->analysis_publication_,
            std::move(publication), std::memory_order_release);
        QMetaObject::invokeMethod(pane.data(), [pane]() {
            const auto analysis = std::atomic_load_explicit(&pane->analysis_publication_,
                std::memory_order_acquire);
            if (analysis->first != 0 && analysis->first == pane->selected_id_) {
                pane->analysis_for_id_ = analysis->first;
                pane->last_analysis_ = analysis->second;
                pane->analysis_valid_ = analysis->second.valid;
                pane->histogram_->setAnalysis(
                    std::shared_ptr<const aida::burp::sequencer::analysis_result_t>(
                        analysis, &analysis->second));
                pane->refreshRightSide();
                pane->refreshSamples();
            }
        }, Qt::QueuedConnection);
    };
    static_cast<void>(submitViewTask(std::move(submission), "Analyze collection"));
}

void SequencerView::deleteSelected() {
    const auto* selected = model_->findById(selected_id_);
    if (!selected) {
        publishViewError("The selected collection changed; select it again before deletion");
        refreshStartGating();
        return;
    }
    const std::uint64_t id = selected->id;
    const std::uint64_t startedMs = selected->started_ms;
    const std::uint64_t instanceRevision = selected->instance_revision;
    const std::string name = selected->name.empty() ? selected->url : selected->name;

    auto* dialog = new BurpReviewDialog(
        QStringLiteral("Delete Sequencer Collection"),
        { QStringLiteral("Delete Sequencer collection '%1' (ID %2)?")
            .arg(QString::fromStdString(name))
            .arg(static_cast<unsigned long long>(id)),
          QStringLiteral("Scope: retained collection configuration and every captured token in this collection."),
          QStringLiteral("Effect: active collection work is stopped and retained samples are permanently removed."),
          QStringLiteral("This operation cannot be undone after confirmation.") },
        QStringLiteral("Delete Collection"), true, this);
    dialog->setRevalidator([id, startedMs, instanceRevision, name](QString& reasonOut) {
        const auto current = aida::burp::sequencer::status(id);
        const bool exact = current.id == id && current.started_ms == startedMs &&
            (current.name.empty() ? current.url : current.name) == name &&
            current.instance_revision == instanceRevision;
        if (!exact) {
            reasonOut = QStringLiteral(
                "The collection or registry changed after review. Cancel and select it again.");
            return false;
        }
        return true;
    });
    QPointer<SequencerView> pane(this);
    dialog->setSubmitCallback([pane, id, startedMs, instanceRevision] {
        if (!pane)
            return;
        aida::infra::executor::submission_t submission;
        submission.owner_subsystem = "burp.sequencer_view";
        submission.label = "sequencer.delete_collection_exact";
        submission.thread_class = "bounded_task";
        submission.domain = aida::infra::executor::domain_t::external_tool;
        submission.priority = 3;
        submission.body = [pane, id, startedMs, instanceRevision]() {
            if (!pane)
                return;
            if (!aida::burp::sequencer::delete_collection_exact(id, startedMs, instanceRevision))
                pane->publishViewError(aida::burp::sequencer::last_error());
            QMetaObject::invokeMethod(pane.data(), [pane]() {
                pane->analysis_valid_ = false;
                pane->refreshStartGating();
                pane->refreshCollections();
                pane->refreshRightSide();
            }, Qt::QueuedConnection);
        };
        static_cast<void>(pane->submitViewTask(std::move(submission), "Delete collection"));
    });
    dialog->open();
}

}
