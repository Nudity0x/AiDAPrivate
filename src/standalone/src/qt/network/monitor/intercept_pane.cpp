#include "qt/network/monitor/intercept_pane.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QEvent>
#include <QItemSelectionModel>
#include <QWidget>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QEasingCurve>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <algorithm>
#include <string>
#include <vector>

#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/shell_host_contract.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/shared/exchange_context_menu.hpp"
#include "qt/network/shared/monitor_bridge.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"
#include "qt/widgets/aida_toggle.hpp"

namespace aida::qt::net {

InterceptModel::InterceptModel(QObject* parent)
    : SnapshotTableModel(parent) {}

int InterceptModel::rowForExchangeId(std::uint64_t id) const noexcept {
    const auto* rowsData = rows();
    if (!rowsData)
        return -1;
    for (int row = 0; row < rowsData->size(); ++row) {
        if (rowsData->at(row).id == id)
            return row;
    }
    return -1;
}

int InterceptModel::rowCount(const QModelIndex& parent) const {
    return SnapshotTableModel::rowCount(parent);
}

int InterceptModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant InterceptModel::cellData(const mitm_proxy::http_exchange& row, int column,
                                  int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Id:     return QString::number(static_cast<quint64>(row.id));
        case Method: return QString::fromStdString(row.request.method);
        case Host:   return QString::fromStdString(row.target_host);
        case Path:   return QString::fromStdString(row.request.uri);
        case Size:   return QStringLiteral("%1 B").arg(static_cast<quint64>(row.raw_request.size()));
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (column) {
        case Host: return QString::fromStdString(row.target_host);
        case Path: return QString::fromStdString(row.request.uri);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (column) {
        case Method: return http_method_color(row.request.method.c_str());
        case Size:   return t.text_dim;
        default:     return t.text_secondary;
        }
    }
    return {};
}

QVariant InterceptModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Id:     return QStringLiteral("ID");
    case Method: return QStringLiteral("Method");
    case Host:   return QStringLiteral("Host");
    case Path:   return QStringLiteral("Path");
    case Size:   return QStringLiteral("Size");
    default: return {};
    }
}

InterceptDropReviewDialog::InterceptDropReviewDialog(
    network_view::intercept_drop_review_t review, QWidget* parent)
    : AidaDialog(parent), review_(std::move(review)) {
    setWindowTitle("Review Intercept Drop");
    auto* layout = new QVBoxLayout(this);
    bodyLabel_ = new QLabel(this);
    bodyLabel_->setWordWrap(true);
    layout->addWidget(bodyLabel_);
    auto* note = new QLabel("Dropped exchanges are not forwarded to their upstream destination.", this);
    note->setWordWrap(true);
    layout->addWidget(note);

    const bool retainedTarget = review_.reviewed_publication &&
        review_.reviewed_count != 0 &&
        (review_.all || review_.target.valid());
    RevalidateScope::hooks_t hooks;
    const auto retainedPublication = review_.reviewed_publication;
    hooks.generation_fn = [retainedPublication]() -> quint64 {
        const auto current = network_view::intercept_runtime_snapshot();
        if (!retainedPublication || !current)
            return 0;
        return current->generation == retainedPublication->generation ? 1 : 0;
    };
    add_revalidate_scope(std::move(hooks),
        "The reviewed Intercept publication changed; the drop review was closed.");

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    confirmButton_ = buttons->button(QDialogButtonBox::Ok);
    confirmButton_->setText("Confirm Drop");
    confirmButton_->setEnabled(retainedTarget);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (network_view::confirm_intercept_drop_review()) {
            accept();
            return;
        }
        notify_error("The Intercept executor rejected the reviewed drop operation");
    });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        network_view::cancel_intercept_drop_review();
        reject();
    });
    layout->addWidget(buttons);
    setMinimumWidth(dialog_min_width_chars(this, 48));

    bodyLabel_->setText(review_.all
        ? QStringLiteral("Drop all %1 currently held exchanges?").arg(review_.reviewed_count)
        : QStringLiteral("Drop held exchange %1?")
            .arg(static_cast<quint64>(review_.target.exchange_id)));
}

InterceptPane::InterceptPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.intercept"));
    setRequiresTarget(false);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* controlRow = new QHBoxLayout();
    controlRow->setSpacing(t.spacing.sm);
    enableToggle_ = new widgets::AidaToggleSwitch(content);
    enableToggle_->setToolTip(QStringLiteral("Hold matching requests at the proxy for review"));
    controlRow->addWidget(enableToggle_);
    auto* enableLabel = new QLabel("Intercept Enabled", content);
    controlRow->addWidget(enableLabel);
    forwardAllButton_ = new widgets::AidaButton("Forward All", content);
    forwardAllButton_->setKind(widgets::AidaButton::Kind::Primary);
    forwardAllButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controlRow->addWidget(forwardAllButton_);
    dropAllButton_ = new widgets::AidaButton("Drop All", content);
    dropAllButton_->setKind(widgets::AidaButton::Kind::Destructive);
    dropAllButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controlRow->addWidget(dropAllButton_);
    interceptingPill_ = new QLabel("INTERCEPTING", content);
    interceptingPill_->setProperty("aidaVariant", QStringLiteral("accent"));
    interceptingPill_->setToolTip(QStringLiteral(
        "Intercept is enabled: matching requests are held until forwarded or dropped"));
    interceptingPill_->setVisible(false);
    controlRow->addWidget(interceptingPill_);
    controlRow->addStretch(1);
    layout->addLayout(controlRow);

    heldLabel_ = new QLabel("Held: 0", content);
    heldLabel_->setProperty("aidaTone", QStringLiteral("secondary"));
    layout->addWidget(heldLabel_);

    model_ = new InterceptModel(content);
    auto* tableHost = new QWidget(content);
    tableStack_ = new QStackedLayout(tableHost);
    tableStack_->setStackingMode(QStackedLayout::StackOne);
    tableStack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.intercept.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    table_->horizontalHeader()->setDefaultSectionSize(table_column_width_chars(table_, 10));
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setModel(model_);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    tableStack_->addWidget(table_);
    emptyView_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No held requests"),
        QStringLiteral("Enable Intercept to hold matching requests here for review, edit, forward, or drop."),
        tableHost);
    emptyView_->setObjectName(QStringLiteral("aida.view.network.intercept.empty"));
    tableStack_->addWidget(emptyView_);
    connect(model_, &QAbstractItemModel::modelReset, this, [this] { updateEmptyState(); });
    layout->addWidget(tableHost, 2);
    updateEmptyState();

    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(t.spacing.sm);
    forwardButton_ = new widgets::AidaButton("Forward", content);
    forwardButton_->setKind(widgets::AidaButton::Kind::Primary);
    forwardButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionRow->addWidget(forwardButton_);
    dropButton_ = new widgets::AidaButton("Drop", content);
    dropButton_->setKind(widgets::AidaButton::Kind::Destructive);
    dropButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionRow->addWidget(dropButton_);
    forwardModifiedButton_ = new widgets::AidaButton("Forward Modified", content);
    forwardModifiedButton_->setKind(widgets::AidaButton::Kind::Secondary);
    forwardModifiedButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionRow->addWidget(forwardModifiedButton_);
    sendRepeaterButton_ = new widgets::AidaButton("Send to Repeater", content);
    sendRepeaterButton_->setKind(widgets::AidaButton::Kind::Ghost);
    sendRepeaterButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionRow->addWidget(sendRepeaterButton_);
    sendFuzzerButton_ = new widgets::AidaButton("Send to Fuzzer", content);
    sendFuzzerButton_->setKind(widgets::AidaButton::Kind::Ghost);
    sendFuzzerButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionRow->addWidget(sendFuzzerButton_);
    actionRow->addStretch(1);
    layout->addLayout(actionRow);

    editorSplitter_ = new QSplitter(Qt::Horizontal, content);
    editorSplitter_->setOpaqueResize(true);
    editorSplitter_->setChildrenCollapsible(false);

    auto* originalPane = new QWidget(editorSplitter_);
    auto* originalLayout = new QVBoxLayout(originalPane);
    originalLayout->setContentsMargins(0, 0, 0, 0);
    originalLayout->setSpacing(t.spacing.xs);
    auto* originalTitle = new QLabel("Original Request", originalPane);
    originalTitle->setProperty("aidaTone", QStringLiteral("secondary"));
    originalLayout->addWidget(originalTitle);
    originalBytesLabel_ = new QLabel(originalPane);
    originalBytesLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    originalLayout->addWidget(originalBytesLabel_);
    QtHumanRequestEditor::Config originalConfig;
    originalConfig.stableId = QStringLiteral("intercept-original-request");
    originalConfig.maxBytes = static_cast<qsizetype>(network_view::k_intercept_editor_capacity - 1);
    originalConfig.editable = false;
    originalEditor_ = new QtHumanRequestEditor(originalPane);
    originalEditor_->setConfig(originalConfig);
    originalLayout->addWidget(originalEditor_, 1);
    editorSplitter_->addWidget(originalPane);

    auto* modifiedPane = new QWidget(editorSplitter_);
    auto* modifiedLayout = new QVBoxLayout(modifiedPane);
    modifiedLayout->setContentsMargins(0, 0, 0, 0);
    modifiedLayout->setSpacing(t.spacing.xs);
    auto* modifiedTitle = new QLabel("Modified Request", modifiedPane);
    modifiedTitle->setProperty("aidaTone", QStringLiteral("accent"));
    modifiedLayout->addWidget(modifiedTitle);
    modifiedNoticeLabel_ = new QLabel(modifiedPane);
    modifiedNoticeLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    modifiedLayout->addWidget(modifiedNoticeLabel_);
    QtHumanRequestEditor::Config modifiedConfig;
    modifiedConfig.stableId = QStringLiteral("intercept-modified-request");
    modifiedConfig.maxBytes = static_cast<qsizetype>(network_view::k_intercept_editor_capacity - 1);
    modifiedConfig.editable = true;
    modifiedEditor_ = new QtHumanRequestEditor(modifiedPane);
    modifiedEditor_->setConfig(modifiedConfig);
    modifiedLayout->addWidget(modifiedEditor_, 1);
    modifiedUnavailableLabel_ = new QLabel(modifiedPane);
    modifiedUnavailableLabel_->setWordWrap(true);
    modifiedUnavailableLabel_->setProperty("aidaTone", QStringLiteral("error"));
    modifiedUnavailableLabel_->setVisible(false);
    modifiedLayout->addWidget(modifiedUnavailableLabel_);
    editorSplitter_->addWidget(modifiedPane);
    editorSplitter_->setStretchFactor(0, 1);
    editorSplitter_->setStretchFactor(1, 1);
    layout->addWidget(editorSplitter_, 3);

    connect(enableToggle_, &QAbstractButton::toggled, this, [](bool) {
        aida::ui::application_ui::execute_action("network.intercept.toggle_enabled",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(forwardAllButton_, &QAbstractButton::clicked, this, [] {
        aida::ui::application_ui::execute_action("network.intercept.forward_all",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(dropAllButton_, &QAbstractButton::clicked, this, [] {
        aida::ui::application_ui::execute_action("network.intercept.drop_all",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(forwardButton_, &QAbstractButton::clicked, this, [] {
        aida::ui::application_ui::execute_action("network.intercept.forward_selected",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(dropButton_, &QAbstractButton::clicked, this, [] {
        aida::ui::application_ui::execute_action("network.intercept.drop_selected",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(forwardModifiedButton_, &QAbstractButton::clicked, this, [] {
        aida::ui::application_ui::execute_action("network.intercept.forward_modified",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(sendRepeaterButton_, &QAbstractButton::clicked, this, [this] {
        const auto snapshot = network_view::intercept_runtime_snapshot();
        if (!snapshot)
            return;
        const int row = model_->rowForExchangeId(selectedExchangeId_);
        const auto* exchange = model_->rowAt(row);
        if (!exchange)
            return;
        std::string unavailable;
        network_view::execute_retained_exchange_toolbar_action("network.exchange.repeater",
            network_view::exchange_artifact_identity(*exchange,
                network_view::artifact_kind_t::intercept_request), {}, unavailable);
    });
    connect(sendFuzzerButton_, &QAbstractButton::clicked, this, [this] {
        const auto snapshot = network_view::intercept_runtime_snapshot();
        if (!snapshot)
            return;
        const int row = model_->rowForExchangeId(selectedExchangeId_);
        const auto* exchange = model_->rowAt(row);
        if (!exchange)
            return;
        std::string unavailable;
        network_view::execute_retained_exchange_toolbar_action("network.exchange.fuzzer",
            network_view::exchange_artifact_identity(*exchange,
                network_view::artifact_kind_t::intercept_request), {}, unavailable);
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex&, const QModelIndex&) {
            onSelectionChanged();
        });
    connect(table_, &QWidget::customContextMenuRequested, this, [this](const QPoint& viewportPos) {
        const QModelIndex index = table_->indexAt(viewportPos);
        if (index.isValid())
            table_->setCurrentIndex(index);
        if (index.isValid())
            showContextForRow(index.row(), table_->viewport()->mapToGlobal(viewportPos),
                aida::ui::context_menu_open_origin_t::pointer);
    });
    table_->viewport()->installEventFilter(this);

    connect(modifiedEditor_, &QtHumanRequestEditor::authorityChanged, this,
        &InterceptPane::onModifiedAuthorityChanged);
    connect(modifiedEditor_, &QtHumanRequestEditor::hasUnappliedPrettyChanged, this,
        [this](bool) { syncEditorMirror(); refreshCommandButtons(); });
    connect(modifiedEditor_, &QtHumanRequestEditor::validityChanged, this,
        [this](bool, const QString&) { syncEditorMirror(); refreshCommandButtons(); });
    connect(modifiedEditor_, &QtHumanRequestEditor::dirtyChanged, this,
        [this](bool) { syncEditorMirror(); refreshCommandButtons(); });

    if (auto* bridge = NetworkMonitorBridge::instance())
        connect(bridge, &NetworkMonitorBridge::interceptSnapshot, this, &InterceptPane::onSnapshot);

    snapshotTimer_ = new QTimer(this);
    snapshotTimer_->setInterval(200);
    connect(snapshotTimer_, &QTimer::timeout, this, [] {
        network_view::request_intercept_runtime_snapshot();
    });

    flashAnimation_ = new QVariantAnimation(this);
    flashAnimation_->setStartValue(1.0);
    flashAnimation_->setEndValue(0.0);
    flashAnimation_->setDuration(theme::AidaMotion::reducedMotion()
        ? 0 : theme::tokens().motion.emphasized);
    flashAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(flashAnimation_, &QVariantAnimation::finished, this, [this] {
        set_label_tone(heldLabel_, "secondary");
    });

    setContent(content);
    refreshCommandButtons();
}

InterceptPane::~InterceptPane() = default;

void InterceptPane::installDropReviewDisplay() {
    network_view::set_intercept_drop_review_display([](network_view::intercept_drop_review_t review) {
        presentDropReview(review);
    });
}

void InterceptPane::presentDropReview(const network_view::intercept_drop_review_t& review) {
    auto* dialog = new InterceptDropReviewDialog(review, nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void InterceptPane::onPaneShown() {
    network_view::request_intercept_runtime_snapshot(true);
    snapshotTimer_->start();
    refreshCommandButtons();
}

void InterceptPane::onPaneHidden() {
    snapshotTimer_->stop();
}

bool InterceptPane::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_->viewport() && event->type() == QEvent::ContextMenu) {
        auto* contextEvent = static_cast<QContextMenuEvent*>(event);
        if (contextEvent->reason() == QContextMenuEvent::Keyboard) {
            const QModelIndex current = table_->selectionModel()->currentIndex();
            if (!current.isValid())
                return false;
            const auto* row = model_->rowAt(current.row());
            if (!row)
                return false;
            const QRect rect = table_->visualRect(current);
            showContextForRow(current.row(),
                table_->viewport()->mapToGlobal(rect.center()),
                aida::ui::context_menu_open_origin_t::menu_key);
            return true;
        }
    }
    return NetworkPaneBase::eventFilter(watched, event);
}

void InterceptPane::onSnapshot(
    std::shared_ptr<const network_view::intercept_runtime_snapshot_t> snapshot) {
    if (!snapshot)
        return;
    running_ = snapshot->running;
    enabled_ = snapshot->enabled;
    currentGeneration_ = snapshot->generation;

    if (enableToggle_->isChecked() != enabled_) {
        const QSignalBlocker blocker(enableToggle_);
        enableToggle_->setChecked(enabled_);
    }
    interceptingPill_->setVisible(enabled_);

    const int heldCount = static_cast<int>(snapshot->held.size());
    heldLabel_->setText(QStringLiteral("Held: %1").arg(heldCount));
    if (heldCount > prevHeldCount_) {
        set_label_tone(heldLabel_, "accent");
        flashAnimation_->start();
    }
    prevHeldCount_ = heldCount;

    model_->adopt(std::make_shared<const QVector<mitm_proxy::http_exchange>>(
        snapshot->held.begin(), snapshot->held.end()), snapshot->generation);

    if (selectedExchangeId_ != 0) {
        const int row = model_->rowForExchangeId(selectedExchangeId_);
        if (row >= 0) {
            table_->setCurrentIndex(model_->index(row, 0));
        } else {
            selectedExchangeId_ = 0;
            network_view::set_intercept_selected_exchange(0);
            network_view::clear_stale_network_selection("view.network.intercept");
            syncEditorsToSelection();
        }
    }
    if (!running_)
        network_view::clear_stale_network_selection("view.network.intercept");
    refreshCommandButtons();
}

void InterceptPane::updateEmptyState() {
    if (!tableStack_ || !emptyView_ || !table_ || !model_)
        return;
    tableStack_->setCurrentWidget(model_->rowCount() == 0
        ? static_cast<QWidget*>(emptyView_) : static_cast<QWidget*>(table_));
}

void InterceptPane::onSelectionChanged() {
    const QModelIndex current = table_->selectionModel()->currentIndex();
    const auto* exchange = model_->rowAt(current.isValid() ? current.row() : -1);
    selectedExchangeId_ = exchange ? exchange->id : 0;
    network_view::set_intercept_selected_exchange(selectedExchangeId_);
    if (exchange) {
        network_view::publish_network_selection(
            network_view::exchange_artifact_identity(*exchange,
                network_view::artifact_kind_t::intercept_request), true);
        retainDraftForSelection();
        syncEditorsToSelection();
    } else {
        network_view::clear_stale_network_selection("view.network.intercept");
    }
    refreshCommandButtons();
}

void InterceptPane::retainDraftForSelection() {
    const auto snapshot = network_view::intercept_runtime_snapshot();
    if (!snapshot)
        return;
    const int row = model_->rowForExchangeId(selectedExchangeId_);
    const auto* exchange = model_->rowAt(row);
    if (!exchange)
        return;
    network_view::retain_intercept_modified_draft(*snapshot, *exchange);
}

void InterceptPane::syncEditorsToSelection() {
    const int row = model_->rowForExchangeId(selectedExchangeId_);
    const auto* exchange = model_->rowAt(row);
    if (!exchange) {
        originalBytesLabel_->clear();
        modifiedNoticeLabel_->clear();
        modifiedUnavailableLabel_->setVisible(false);
        return;
    }
    const auto draft = network_view::intercept_modified_draft();
    originalBytesLabel_->setText(QStringLiteral("%1 bytes")
        .arg(static_cast<quint64>(exchange->raw_request.size())));

    const QString originalIdentity = QStringLiteral("intercept.original.%1.%2.%3")
        .arg(static_cast<quint64>(exchange->id))
        .arg(static_cast<quint64>(exchange->timestamp))
        .arg(static_cast<quint64>(network_view::artifact_content_hash(exchange->raw_request)));
    originalEditor_->setAuthority(originalIdentity,
        QByteArray(reinterpret_cast<const char*>(exchange->raw_request.data()),
            static_cast<qsizetype>(exchange->raw_request.size())));

    if (!draft.editable) {
        modifiedNoticeLabel_->setText("Text editing unavailable for this retained request");
        modifiedUnavailableLabel_->setText(QString::fromStdString(draft.unavailable_reason));
        modifiedUnavailableLabel_->setVisible(!draft.unavailable_reason.empty());
        return;
    }
    modifiedNoticeLabel_->setText("Bounded reviewed text draft");
    modifiedUnavailableLabel_->setVisible(false);
    const QString modifiedIdentity = QStringLiteral("intercept.modified.%1.%2.%3")
        .arg(static_cast<quint64>(exchange->id))
        .arg(static_cast<quint64>(exchange->timestamp))
        .arg(static_cast<quint64>(network_view::artifact_content_hash(exchange->raw_request)));
    modifiedEditor_->setAuthority(modifiedIdentity, QString::fromStdString(draft.raw_request));
    syncEditorMirror();
}

void InterceptPane::onModifiedAuthorityChanged() {
    network_view::set_intercept_modified_draft_text(
        modifiedEditor_->authority().toStdString());
    std::string reason;
    network_view::refresh_intercept_modified_draft(reason);
    syncEditorMirror();
    refreshCommandButtons();
}

void InterceptPane::syncEditorMirror() {
    network_view::set_intercept_editor_state(
        modifiedEditor_->hasUnappliedPretty(),
        modifiedEditor_->isOversized(), modifiedEditor_->isBinary(),
        modifiedEditor_->errorString().toStdString());
}

void InterceptPane::refreshCommandButtons() {
    const bool pending = network_view::intercept_operation_pending();
    const auto capabilityFor = [](network_view::intercept_command_t command) {
        return network_view::intercept_command_capability(command);
    };
    const auto forwardAll = capabilityFor(network_view::intercept_command_t::forward_all);
    const auto dropAll = capabilityFor(network_view::intercept_command_t::drop_all);
    const auto forwardSel = capabilityFor(network_view::intercept_command_t::forward_selected);
    const auto dropSel = capabilityFor(network_view::intercept_command_t::drop_selected);
    const auto forwardMod = capabilityFor(network_view::intercept_command_t::forward_modified);

    enableToggle_->setEnabled(!pending);
    const auto applyState = [this, pending](widgets::AidaButton* button,
            const network_view::intercept_command_capability_t& capability) {
        button->setEnabled(capability.enabled && !pending);
        button->setToolTip(capability.enabled ? QString()
            : QString::fromStdString(capability.disabled_reason));
    };
    applyState(forwardAllButton_, forwardAll);
    applyState(dropAllButton_, dropAll);
    applyState(forwardButton_, forwardSel);
    applyState(dropButton_, dropSel);
    applyState(forwardModifiedButton_, forwardMod);
    const bool hasSelection = selectedExchangeId_ != 0;
    sendRepeaterButton_->setEnabled(hasSelection);
    sendFuzzerButton_->setEnabled(hasSelection);
}

void InterceptPane::showContextForRow(int row, const QPoint& globalPos,
                                      aida::ui::context_menu_open_origin_t origin) {
    const auto* exchange = model_->rowAt(row);
    if (!exchange)
        return;
    selectedExchangeId_ = exchange->id;
    network_view::set_intercept_selected_exchange(exchange->id);
    const auto snapshot = network_view::intercept_runtime_snapshot();
    if (snapshot)
        network_view::retain_intercept_modified_draft(*snapshot, *exchange);
    network_view::publish_network_selection(
        network_view::exchange_artifact_identity(*exchange,
            network_view::artifact_kind_t::intercept_request), true);
    exchange_context_host().show(table_, globalPos,
        network_view::exchange_artifact_identity(*exchange,
            network_view::artifact_kind_t::intercept_request), {},
        origin == aida::ui::context_menu_open_origin_t::pointer
            ? network_view::exchange_context_origin_t::pointer
            : network_view::exchange_context_origin_t::menu_key,
        true);
}

}
