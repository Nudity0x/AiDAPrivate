#include "qt/network/monitor/filters_pane.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QItemSelectionModel>
#include <QComboBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <cstdio>
#include <memory>
#include <vector>

#include <QVector>

#include "core/ui/application_ui_runtime.hpp"
#include "qt/network/shared/monitor_bridge.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

FiltersModel::FiltersModel(QObject* parent)
    : SnapshotTableModel(parent) {}

int FiltersModel::rowCount(const QModelIndex& parent) const {
    return SnapshotTableModel::rowCount(parent);
}

int FiltersModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant FiltersModel::cellData(const network_view::filter_entry_t& row, int column,
                                int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Rule:      return QStringLiteral("#%1").arg(row.rule_id);
        case ActionCol: return row.action == 0 ? QStringLiteral("BLOCK") : QStringLiteral("ALLOW");
        case Direction: return row.direction == 0 ? QStringLiteral("IN")
            : row.direction == 1 ? QStringLiteral("OUT") : QStringLiteral("BOTH");
        case Protocol:  return row.protocol == 6 ? QStringLiteral("TCP")
            : row.protocol == 17 ? QStringLiteral("UDP") : QStringLiteral("ANY");
        case Pid:       return static_cast<quint32>(row.pid);
        case Port:      return static_cast<quint32>(row.port);
        case Ip:        return QString::fromStdString(row.ip_addr);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == ActionCol)
            return row.action == 0 ? t.error : t.success;
        return t.text_primary;
    }
    return {};
}

QVariant FiltersModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Rule:      return QStringLiteral("Rule");
    case ActionCol: return QStringLiteral("Action");
    case Direction: return QStringLiteral("Direction");
    case Protocol:  return QStringLiteral("Protocol");
    case Pid:       return QStringLiteral("PID");
    case Port:      return QStringLiteral("Port");
    case Ip:        return QStringLiteral("IP");
    default: return {};
    }
}

FiltersPane::FiltersPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.filters"));
    setRequiresTarget(true);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* formTitle = new QLabel("Add Filter Rule", content);
    formTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    layout->addWidget(formTitle);

    auto* form = new QHBoxLayout();
    form->setSpacing(t.spacing.sm);
    form->addWidget(new QLabel("Action:", content));
    actionCombo_ = new QComboBox(content);
    actionCombo_->addItems({"Block", "Allow"});
    form->addWidget(actionCombo_);
    form->addWidget(new QLabel("Direction:", content));
    directionCombo_ = new QComboBox(content);
    directionCombo_->addItems({"In", "Out", "Both"});
    directionCombo_->setCurrentIndex(2);
    form->addWidget(directionCombo_);
    form->addWidget(new QLabel("Protocol:", content));
    protocolCombo_ = new QComboBox(content);
    protocolCombo_->addItems({"Any", "TCP", "UDP"});
    form->addWidget(protocolCombo_);
    pidEdit_ = new QLineEdit(content);
    pidEdit_->setPlaceholderText("PID");
    pidEdit_->setMaxLength(15);
    pidEdit_->setMaximumWidth(field_width_chars(pidEdit_, 10));
    form->addWidget(pidEdit_);
    portEdit_ = new QLineEdit(content);
    portEdit_->setPlaceholderText("Port");
    portEdit_->setMaxLength(15);
    portEdit_->setMaximumWidth(field_width_chars(portEdit_, 10));
    form->addWidget(portEdit_);
    ipEdit_ = new QLineEdit(content);
    ipEdit_->setPlaceholderText("IP Address");
    ipEdit_->setMaxLength(63);
    ipEdit_->setMaximumWidth(field_width_chars(ipEdit_, 18));
    form->addWidget(ipEdit_);
    addButton_ = new widgets::AidaButton("Add Rule", content);
    addButton_->setKind(widgets::AidaButton::Kind::Primary);
    addButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    form->addWidget(addButton_);
    clearAllButton_ = new widgets::AidaButton("Clear All", content);
    clearAllButton_->setKind(widgets::AidaButton::Kind::Destructive);
    clearAllButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    form->addWidget(clearAllButton_);
    form->addStretch(1);
    layout->addLayout(form);

    validationLabel_ = new QLabel(content);
    validationLabel_->setObjectName(QStringLiteral("aida.view.network.filters.validation"));
    set_label_variant(validationLabel_, "error");
    validationLabel_->setVisible(false);
    layout->addWidget(validationLabel_);

    auto* rulesTitle = new QLabel("Active Rules:", content);
    rulesTitle->setProperty("aidaTone", QStringLiteral("secondary"));
    layout->addWidget(rulesTitle);

    auto* actionBar = new QHBoxLayout();
    removeButton_ = new widgets::AidaButton("Remove Selected", content);
    removeButton_->setKind(widgets::AidaButton::Kind::Ghost);
    removeButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    removeButton_->setEnabled(false);
    actionBar->addWidget(removeButton_);
    actionBar->addStretch(1);
    layout->addLayout(actionBar);

    model_ = new FiltersModel(content);
    auto* tableHost = new QWidget(content);
    tableStack_ = new QStackedLayout(tableHost);
    tableStack_->setStackingMode(QStackedLayout::StackOne);
    tableStack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.filters.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    table_->horizontalHeader()->setDefaultSectionSize(table_column_width_chars(table_, 8));
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
        QStringLiteral("No filter rules"),
        QStringLiteral("Add a driver-enforced allow/block rule above to shape the captured traffic."),
        tableHost);
    emptyView_->setObjectName(QStringLiteral("aida.view.network.filters.empty"));
    tableStack_->addWidget(emptyView_);
    connect(table_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = table_->indexAt(pos);
        if (!index.isValid())
            return;
        table_->setCurrentIndex(index);
        QMenu menu(this);
        auto* removeAction = menu.addAction(QStringLiteral("Remove Selected"));
        connect(removeAction, &QAction::triggered, this, [this] { removeSelected(); });
        menu.popup(table_->viewport()->mapToGlobal(pos));
    });
    layout->addWidget(tableHost, 1);

    connect(addButton_, &QAbstractButton::clicked, this, [this] {
        syncDraftToState();
        aida::ui::application_ui::execute_action("network.filters.add",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(clearAllButton_, &QAbstractButton::clicked, this, [] {
        aida::ui::application_ui::execute_action("network.filters.clear",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(removeButton_, &QAbstractButton::clicked, this, [this] {
        removeSelected();
    });
    const auto draftChanged = [this] {
        syncDraftToState();
        refreshButtons();
    };
    connect(actionCombo_, &QComboBox::currentIndexChanged, this, draftChanged);
    connect(directionCombo_, &QComboBox::currentIndexChanged, this, draftChanged);
    connect(protocolCombo_, &QComboBox::currentIndexChanged, this, draftChanged);
    connect(pidEdit_, &QLineEdit::textChanged, this, draftChanged);
    connect(portEdit_, &QLineEdit::textChanged, this, draftChanged);
    connect(ipEdit_, &QLineEdit::textChanged, this, draftChanged);
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            network_view::g_state.filter_selected = current.isValid() ? current.row() : -1;
            removeButton_->setEnabled(current.isValid() &&
                !network_view::filter_mutation_pending());
        });

    if (auto* bridge = NetworkMonitorBridge::instance())
        connect(bridge, &NetworkMonitorBridge::filtersChanged, this, [this] {
            refreshFromStore();
            refreshButtons();
        });

    statePoll_ = new QTimer(this);
    statePoll_->setInterval(250);
    connect(statePoll_, &QTimer::timeout, this, [this] {
        driverSettled_ = true;
        refreshButtons();
        updateEmptyState();
    });

    setContent(content);
    refreshFromStore();
    refreshButtons();
    updateEmptyState();
}

void FiltersPane::updateEmptyState() {
    if (!tableStack_ || !emptyView_ || !table_ || !model_)
        return;
    const bool empty = model_->rowCount() == 0;
    if (empty) {
        const bool driverOk = network_view::driver_available_snapshot();
        if (!driverOk && driverSettled_) {
            emptyView_->setState(widgets::AidaStateView::State::Error);
            emptyView_->setTitle(QStringLiteral("Kernel driver not attached"));
            emptyView_->setMessage(QStringLiteral(
                "Filter rules are enforced by the kernel driver; load it to manage rules."));
        } else {
            emptyView_->setState(widgets::AidaStateView::State::Empty);
            emptyView_->setTitle(QStringLiteral("No filter rules"));
            emptyView_->setMessage(QStringLiteral(
                "Add a driver-enforced allow/block rule above to shape the captured traffic."));
        }
    }
    tableStack_->setCurrentWidget(empty
        ? static_cast<QWidget*>(emptyView_) : static_cast<QWidget*>(table_));
}

void FiltersPane::onPaneShown() {
    network_view::request_driver_available_snapshot(true);
    driverSettled_ = network_view::driver_available_snapshot();
    statePoll_->start();
    refreshFromStore();
    refreshButtons();
}

void FiltersPane::onPaneHidden() {
    statePoll_->stop();
}

void FiltersPane::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete && table_->hasFocus() &&
        table_->selectionModel()->currentIndex().isValid()) {
        removeSelected();
        return;
    }
    if (event->key() == Qt::Key_Menu && table_->hasFocus()) {
        const QModelIndex current = table_->selectionModel()->currentIndex();
        if (!current.isValid())
            return;
        table_->setCurrentIndex(current);
        QMenu menu(this);
        auto* removeAction = menu.addAction(QStringLiteral("Remove Selected"));
        connect(removeAction, &QAction::triggered, this, [this] { removeSelected(); });
        menu.popup(table_->viewport()->mapToGlobal(table_->visualRect(current).center()));
        return;
    }
    NetworkPaneBase::keyPressEvent(event);
}

void FiltersPane::refreshFromStore() {
    ++generation_;
    model_->adopt(std::make_shared<const QVector<network_view::filter_entry_t>>(
        network_view::g_state.filters.begin(), network_view::g_state.filters.end()), generation_);
    updateEmptyState();
}

void FiltersPane::syncDraftToState() {
    std::snprintf(network_view::g_state.nf_pid, sizeof(network_view::g_state.nf_pid),
        "%s", pidEdit_->text().toUtf8().constData());
    std::snprintf(network_view::g_state.nf_port, sizeof(network_view::g_state.nf_port),
        "%s", portEdit_->text().toUtf8().constData());
    std::snprintf(network_view::g_state.nf_ip, sizeof(network_view::g_state.nf_ip),
        "%s", ipEdit_->text().toUtf8().constData());
    network_view::g_state.nf_action = actionCombo_->currentIndex();
    network_view::g_state.nf_direction = directionCombo_->currentIndex();
    network_view::g_state.nf_protocol = protocolCombo_->currentIndex() == 0 ? 0
        : protocolCombo_->currentIndex() == 1 ? 6 : 17;
}

void FiltersPane::refreshButtons() {
    const bool driverOk = network_view::driver_available_snapshot();
    const bool pending = network_view::filter_mutation_pending();
    const std::string validation = network_view::filter_draft_error();
    addButton_->setEnabled(driverOk && !pending && validation.empty());
    addButton_->setText(pending ? "Applying..." : "Add Rule");
    clearAllButton_->setEnabled(driverOk && !pending && model_->rowCount() > 0);
    const bool hasSelection = table_->selectionModel()->currentIndex().isValid();
    removeButton_->setEnabled(hasSelection && !pending);
    const QString validationText = QString::fromStdString(validation);
    validationLabel_->setText(validationText);
    validationLabel_->setVisible(!validationText.isEmpty());
}

void FiltersPane::removeSelected() {
    const auto current = table_->selectionModel()->currentIndex();
    if (!current.isValid())
        return;
    network_view::g_state.filter_selected = current.row();
    aida::ui::application_ui::execute_action("network.filters.remove_selected",
        aida::ui::action_invocation_source_t::toolbar);
}

}
