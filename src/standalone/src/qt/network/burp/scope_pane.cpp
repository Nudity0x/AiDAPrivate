#include "qt/network/burp/scope_pane.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QItemSelectionModel>
#include <QWidget>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QStackedLayout>
#include <QTableView>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <vector>

#include "core/network/burp/scope.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "qt/network/burp/scope_bridge.hpp"
#include "qt/network/shared/event_bus_bridge.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

ScopeRulesModel::ScopeRulesModel(QObject* parent)
    : SnapshotTableModel(parent) {}

int ScopeRulesModel::rowCount(const QModelIndex& parent) const {
    return SnapshotTableModel::rowCount(parent);
}

int ScopeRulesModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ScopeRulesModel::cellData(const aida::burp::scope::rule_t& row, int column,
                                   int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Kind:  return row.kind == aida::burp::scope::rule_kind_t::include
            ? QStringLiteral("include") : QStringLiteral("exclude");
        case Proto: return row.protocol.empty() ? QStringLiteral("*")
            : QString::fromStdString(row.protocol);
        case Host:  return QString::fromStdString(row.host_pattern);
        case Path:  return row.path_prefix.empty() ? QStringLiteral("/")
            : QString::fromStdString(row.path_prefix);
        case Port:  return row.port == 0 ? QStringLiteral("*")
            : QString::number(row.port);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Kind)
            return row.kind == aida::burp::scope::rule_kind_t::include ? t.success : t.error;
        return row.enabled ? t.text_primary : t.text_dim;
    }
    if (role == Qt::CheckStateRole && column == Kind)
        return row.enabled ? Qt::Checked : Qt::Unchecked;
    return {};
}

QVariant ScopeRulesModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Kind:  return QStringLiteral("Kind");
    case Proto: return QStringLiteral("Proto");
    case Host:  return QStringLiteral("Host");
    case Path:  return QStringLiteral("Path");
    case Port:  return QStringLiteral("Port");
    default: return {};
    }
}

ScopePane::ScopePane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.scope"));
    setRequiresTarget(false);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* titleLabel = new QLabel("Scope rules", content);
    titleLabel->setProperty("aidaTone", QStringLiteral("title"));
    layout->addWidget(titleLabel);

    auto* form = new QHBoxLayout();
    form->setSpacing(t.spacing.sm);
    form->addWidget(new QLabel("Kind:", content));
    kindCombo_ = new QComboBox(content);
    kindCombo_->addItems({"include", "exclude"});
    form->addWidget(kindCombo_);
    form->addWidget(new QLabel("Proto:", content));
    protoEdit_ = new QLineEdit(content);
    protoEdit_->setPlaceholderText("https/http/any");
    protoEdit_->setMaxLength(15);
    protoEdit_->setMaximumWidth(field_width_chars(protoEdit_, 8));
    form->addWidget(protoEdit_);
    form->addWidget(new QLabel("Host:", content));
    hostEdit_ = new QLineEdit(content);
    hostEdit_->setPlaceholderText(".example.com or regex");
    hostEdit_->setMaxLength(255);
    form->addWidget(hostEdit_, 1);
    form->addWidget(new QLabel("Port:", content));
    portEdit_ = new QLineEdit(content);
    portEdit_->setPlaceholderText("0=any");
    portEdit_->setMaxLength(15);
    portEdit_->setMaximumWidth(field_width_chars(portEdit_, 6));
    form->addWidget(portEdit_);
    form->addWidget(new QLabel("Path:", content));
    pathEdit_ = new QLineEdit(content);
    pathEdit_->setPlaceholderText("/api/");
    pathEdit_->setMaxLength(511);
    form->addWidget(pathEdit_, 1);
    addButton_ = new widgets::AidaButton("Add rule", content);
    addButton_->setKind(widgets::AidaButton::Kind::Primary);
    addButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    form->addWidget(addButton_);
    layout->addLayout(form);

    auto* actionBar = new QHBoxLayout();
    actionBar->setSpacing(t.spacing.sm);
    toggleButton_ = new widgets::AidaButton("Toggle", content);
    toggleButton_->setKind(widgets::AidaButton::Kind::Ghost);
    toggleButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    toggleButton_->setEnabled(false);
    actionBar->addWidget(toggleButton_);
    removeButton_ = new widgets::AidaButton("Remove", content);
    removeButton_->setKind(widgets::AidaButton::Kind::Ghost);
    removeButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    removeButton_->setEnabled(false);
    actionBar->addWidget(removeButton_);
    actionBar->addStretch(1);
    layout->addLayout(actionBar);

    model_ = new ScopeRulesModel(content);
    auto* tableHost = new QWidget(content);
    tableStack_ = new QStackedLayout(tableHost);
    tableStack_->setStackingMode(QStackedLayout::StackOne);
    tableStack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.scope.table"));
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
        QStringLiteral("No scope rules"),
        QStringLiteral("Add an include or exclude rule above to define what the proxy tools consider in scope."),
        tableHost);
    emptyView_->setObjectName(QStringLiteral("aida.view.network.scope.empty"));
    tableStack_->addWidget(emptyView_);
    layout->addWidget(tableHost, 1);

    auto* testRow = new QHBoxLayout();
    testRow->setSpacing(t.spacing.sm);
    testRow->addWidget(new QLabel("Test URL:", content));
    testUrlEdit_ = new QLineEdit(content);
    testUrlEdit_->setPlaceholderText("https://example.com/path");
    testUrlEdit_->setMaxLength(511);
    testRow->addWidget(testUrlEdit_, 1);
    checkButton_ = new widgets::AidaButton("Check", content);
    checkButton_->setKind(widgets::AidaButton::Kind::Secondary);
    checkButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    testRow->addWidget(checkButton_);
    testResultLabel_ = new QLabel(content);
    testRow->addWidget(testResultLabel_);
    testRow->addStretch(1);
    layout->addLayout(testRow);

    auto* bottomRow = new QHBoxLayout();
    clearAllButton_ = new widgets::AidaButton("Clear all", content);
    clearAllButton_->setKind(widgets::AidaButton::Kind::Destructive);
    clearAllButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    bottomRow->addWidget(clearAllButton_);
    reloadButton_ = new widgets::AidaButton("Reload", content);
    reloadButton_->setKind(widgets::AidaButton::Kind::Secondary);
    reloadButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    bottomRow->addWidget(reloadButton_);
    bottomRow->addStretch(1);
    layout->addLayout(bottomRow);

    connect(addButton_, &QAbstractButton::clicked, this, [this] {
        addRuleFromForm();
    });
    connect(toggleButton_, &QAbstractButton::clicked, this, [this] {
        toggleSelected();
    });
    connect(removeButton_, &QAbstractButton::clicked, this, [this] {
        removeSelected();
    });
    connect(checkButton_, &QAbstractButton::clicked, this, [this] {
        runUrlTest();
    });
    connect(clearAllButton_, &QAbstractButton::clicked, this, [] {
        aida::burp::scope::clear_all();
    });
    connect(reloadButton_, &QAbstractButton::clicked, this, [] {
        aida::burp::scope::load_from_disk();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex&, const QModelIndex&) {
            updateActionBar();
        });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex&) {
        toggleSelected();
    });
    connect(table_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = table_->indexAt(pos);
        if (index.isValid())
            table_->setCurrentIndex(index);
        if (!index.isValid())
            return;
        openContextForRow(index.row(), table_->viewport()->mapToGlobal(pos));
    });

    if (auto* bridge = NetworkEventBusBridge::instance()) {
        connect(bridge, &NetworkEventBusBridge::scopeChanged, this, [this](const aida::burp::scope_changed_t& change) {
            if (change.action == "stage")
                adoptStagedDraft();
            else
                refreshFromStore();
        });
    }

    setContent(content);
    refreshFromStore();
    updateActionBar();
}

void ScopePane::onPaneShown() {
    refreshFromStore();
    adoptStagedDraft();
    updateActionBar();
}

bool ScopePane::eventFilter(QObject* watched, QEvent* event) {
    return NetworkPaneBase::eventFilter(watched, event);
}

void ScopePane::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete && table_->selectionModel()->currentIndex().isValid()) {
        removeSelected();
        return;
    }
    if (event->key() == Qt::Key_Menu && table_->hasFocus()) {
        const QModelIndex current = table_->selectionModel()->currentIndex();
        if (current.isValid())
            openContextForRow(current.row(), table_->viewport()->mapToGlobal(
                table_->visualRect(current).center()));
        return;
    }
    NetworkPaneBase::keyPressEvent(event);
}

void ScopePane::refreshFromStore() {
    const auto rules = aida::burp::scope::list_rules();
    ++generation_;
    model_->adopt(std::make_shared<const QVector<aida::burp::scope::rule_t>>(
        rules.begin(), rules.end()), generation_);
    updateEmptyState();
    updateActionBar();
}

void ScopePane::updateEmptyState() {
    if (!tableStack_ || !emptyView_ || !table_ || !model_)
        return;
    tableStack_->setCurrentWidget(model_->rowCount() == 0
        ? static_cast<QWidget*>(emptyView_) : static_cast<QWidget*>(table_));
}

void ScopePane::addRuleFromForm() {
    const QString host = hostEdit_->text();
    if (host.isEmpty())
        return;
    const QString portText = portEdit_->text();
    int port = 0;
    for (const QChar c : portText) {
        if (!c.isDigit()) {
            port = -1;
            break;
        }
        port = port * 10 + c.digitValue();
        if (port > 65535) {
            port = -1;
            break;
        }
    }
    if (port < 0)
        port = 0;
    if (kindCombo_->currentIndex() == 0) {
        aida::burp::scope::add_include_rule(protoEdit_->text().toStdString(),
            host.toStdString(), port, pathEdit_->text().toStdString());
    } else {
        aida::burp::scope::add_exclude_rule(protoEdit_->text().toStdString(),
            host.toStdString(), port, pathEdit_->text().toStdString());
    }
    hostEdit_->clear();
    pathEdit_->clear();
    portEdit_->clear();
    protoEdit_->clear();
}

void ScopePane::adoptStagedDraft() {
    aida::burp::scope::staged_rule_draft_t draft;
    if (!aida::burp::scope::take_staged_rule_draft(draft))
        return;
    kindCombo_->setCurrentIndex(draft.exclude ? 1 : 0);
    protoEdit_->setText(QString::fromStdString(draft.protocol));
    hostEdit_->setText(QString::fromStdString(draft.host));
    portEdit_->setText(draft.port == 0 ? QString() : QString::number(draft.port));
    pathEdit_->setText(QString::fromStdString(draft.path));
}

void ScopePane::openContextForRow(int row, const QPoint& globalPos) {
    if (!model_->rowAt(row))
        return;
    QMenu menu(this);
    auto* toggleAction = menu.addAction("Toggle");
    auto* removeAction = menu.addAction("Remove");
    connect(toggleAction, &QAction::triggered, this, [this] { toggleSelected(); });
    connect(removeAction, &QAction::triggered, this, [this] { removeSelected(); });
    menu.popup(globalPos);
}

void ScopePane::toggleSelected() {
    const auto current = table_->selectionModel()->currentIndex();
    const auto* rule = model_->rowAt(current.isValid() ? current.row() : -1);
    if (!rule)
        return;
    aida::burp::scope::set_rule_enabled(rule->id, !rule->enabled);
}

void ScopePane::removeSelected() {
    const auto current = table_->selectionModel()->currentIndex();
    const auto* rule = model_->rowAt(current.isValid() ? current.row() : -1);
    if (!rule)
        return;
    aida::burp::scope::remove_rule(rule->id);
}

void ScopePane::runUrlTest() {
    const QString url = testUrlEdit_->text();
    const bool inScope = aida::burp::scope::in_scope(url.toStdString());
    testResultLabel_->setText(inScope ? "in scope" : "out of scope");
    set_label_tone(testResultLabel_, inScope ? "success" : "error");
}

void ScopePane::updateActionBar() {
    const auto current = table_->selectionModel()->currentIndex();
    const auto* rule = model_->rowAt(current.isValid() ? current.row() : -1);
    toggleButton_->setEnabled(rule != nullptr);
    removeButton_->setEnabled(rule != nullptr);
    if (rule)
        toggleButton_->setText(rule->enabled ? "Disable" : "Enable");
}

}
