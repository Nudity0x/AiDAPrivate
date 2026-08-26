#include "qt/network/burp/upstream_pane.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QItemSelectionModel>
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QSpinBox>
#include <QStackedLayout>
#include <QTableView>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <string>
#include <vector>

#include "core/infra/executor.hpp"
#include "core/network/burp/upstream_chain.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

UpstreamModel::UpstreamModel(QObject* parent)
    : SnapshotTableModel(parent) {}

int UpstreamModel::rowCount(const QModelIndex& parent) const {
    return SnapshotTableModel::rowCount(parent);
}

int UpstreamModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant UpstreamModel::cellData(const aida::burp::upstream::upstream_chain_t& row, int column,
                                 int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Label: {
            QString label = QString::fromStdString(row.label);
            if (row.id == activeChainId)
                label += QStringLiteral("  (active)");
            return label;
        }
        case Hops: {
            QString concat;
            for (std::size_t k = 0; k < row.hops.size(); ++k) {
                if (k > 0)
                    concat += QStringLiteral(" -> ");
                concat += QString::fromStdString(row.hops[k].type) + QStringLiteral("://") +
                    QString::fromStdString(row.hops[k].host) + QStringLiteral(":") +
                    QString::number(row.hops[k].port);
            }
            return concat;
        }
        case Active: return row.id == activeChainId ? QStringLiteral("active") : QString();
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Label && row.id == activeChainId)
            return t.success;
        if (column == Hops)
            return t.text_secondary;
        return t.text_primary;
    }
    return {};
}

QVariant UpstreamModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Label:  return QStringLiteral("Label");
    case Hops:   return QStringLiteral("Hops");
    case Active: return QStringLiteral("State");
    default: return {};
    }
}

HopRowWidget::HopRowWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    const auto& t = theme::tokens();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(t.spacing.xs);
    typeCombo_ = new QComboBox(this);
    typeCombo_->addItems({"http_connect", "socks5"});
    layout->addWidget(typeCombo_);
    hostEdit_ = new QLineEdit(this);
    hostEdit_->setPlaceholderText("host");
    hostEdit_->setMaxLength(255);
    layout->addWidget(hostEdit_, 1);
    portSpin_ = new QSpinBox(this);
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(443);
    layout->addWidget(portSpin_);
    userEdit_ = new QLineEdit(this);
    userEdit_->setPlaceholderText("user");
    userEdit_->setMaxLength(127);
    layout->addWidget(userEdit_);
    passEdit_ = new QLineEdit(this);
    passEdit_->setPlaceholderText("pass");
    passEdit_->setMaxLength(127);
    passEdit_->setEchoMode(QLineEdit::Password);
    layout->addWidget(passEdit_);
    auto* upButton = new widgets::AidaButton("Up", this);
    upButton->setKind(widgets::AidaButton::Kind::Ghost);
    upButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    layout->addWidget(upButton);
    auto* downButton = new widgets::AidaButton("Down", this);
    downButton->setKind(widgets::AidaButton::Kind::Ghost);
    downButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    layout->addWidget(downButton);
    auto* removeButton = new widgets::AidaButton("Remove", this);
    removeButton->setKind(widgets::AidaButton::Kind::Ghost);
    removeButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    layout->addWidget(removeButton);
    connect(upButton, &QAbstractButton::clicked, this, [this] { Q_EMIT moveUpRequested(this); });
    connect(downButton, &QAbstractButton::clicked, this, [this] { Q_EMIT moveDownRequested(this); });
    connect(removeButton, &QAbstractButton::clicked, this, [this] { Q_EMIT removeRequested(this); });
}

aida::burp::upstream::upstream_hop_t HopRowWidget::toHop() const {
    aida::burp::upstream::upstream_hop_t hop;
    hop.type = typeCombo_->currentText().toStdString();
    hop.host = hostEdit_->text().toStdString();
    hop.port = static_cast<uint16_t>(portSpin_->value());
    hop.username = userEdit_->text().toStdString();
    hop.password = passEdit_->text().toStdString();
    return hop;
}

bool HopRowWidget::hasHost() const {
    return !hostEdit_->text().isEmpty();
}

UpstreamPane::UpstreamPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.upstream"));
    setRequiresTarget(false);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* titleLabel = new QLabel("Upstream proxy chain", content);
    titleLabel->setProperty("aidaTone", QStringLiteral("title"));
    layout->addWidget(titleLabel);

    auto* formRow = new QHBoxLayout();
    formRow->setSpacing(t.spacing.sm);
    formRow->addWidget(new QLabel("Label:", content));
    labelEdit_ = new QLineEdit(content);
    labelEdit_->setPlaceholderText("my-chain");
    labelEdit_->setMaxLength(127);
    formRow->addWidget(labelEdit_, 1);
    addHopButton_ = new widgets::AidaButton("Add hop", content);
    addHopButton_->setKind(widgets::AidaButton::Kind::Secondary);
    addHopButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    formRow->addWidget(addHopButton_);
    saveChainButton_ = new widgets::AidaButton("Save chain", content);
    saveChainButton_->setKind(widgets::AidaButton::Kind::Primary);
    saveChainButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    formRow->addWidget(saveChainButton_);
    layout->addLayout(formRow);

    hopsContainer_ = new QWidget(content);
    hopsLayout_ = new QVBoxLayout(hopsContainer_);
    hopsLayout_->setContentsMargins(0, 0, 0, 0);
    hopsLayout_->setSpacing(t.spacing.xs);
    layout->addWidget(hopsContainer_);

    auto* actionBar = new QHBoxLayout();
    actionBar->setSpacing(t.spacing.sm);
    activateButton_ = new widgets::AidaButton("Activate", content);
    activateButton_->setKind(widgets::AidaButton::Kind::Ghost);
    activateButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionBar->addWidget(activateButton_);
    deactivateButton_ = new widgets::AidaButton("Deactivate", content);
    deactivateButton_->setKind(widgets::AidaButton::Kind::Ghost);
    deactivateButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionBar->addWidget(deactivateButton_);
    testButton_ = new widgets::AidaButton("Test", content);
    testButton_->setKind(widgets::AidaButton::Kind::Secondary);
    testButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionBar->addWidget(testButton_);
    removeButton_ = new widgets::AidaButton("Remove", content);
    removeButton_->setKind(widgets::AidaButton::Kind::Ghost);
    removeButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    actionBar->addWidget(removeButton_);
    actionBar->addStretch(1);
    layout->addLayout(actionBar);

    model_ = new UpstreamModel(content);
    auto* tableHost = new QWidget(content);
    tableStack_ = new QStackedLayout(tableHost);
    tableStack_->setStackingMode(QStackedLayout::StackOne);
    tableStack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.upstream.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h + t.spacing.xxs);
    table_->horizontalHeader()->setDefaultSectionSize(table_column_width_chars(table_, 16));
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setModel(model_);
    tableStack_->addWidget(table_);
    emptyView_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No upstream chains configured"),
        QStringLiteral("Click 'Add hop' then 'Save chain' to route traffic through an upstream proxy chain."),
        tableHost);
    emptyView_->setObjectName(QStringLiteral("aida.view.network.upstream.empty"));
    tableStack_->addWidget(emptyView_);
    layout->addWidget(tableHost, 1);

    auto* testRow = new QHBoxLayout();
    testRow->setSpacing(t.spacing.sm);
    testRow->addWidget(new QLabel("Test target host:port:", content));
    testHostEdit_ = new QLineEdit("example.com", content);
    testHostEdit_->setMaxLength(255);
    testRow->addWidget(testHostEdit_, 1);
    testPortSpin_ = new QSpinBox(content);
    testPortSpin_->setRange(1, 65535);
    testPortSpin_->setValue(443);
    testRow->addWidget(testPortSpin_);
    layout->addLayout(testRow);

    testResultLabel_ = new QLabel(content);
    testResultLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    testResultLabel_->setWordWrap(true);
    layout->addWidget(testResultLabel_);

    connect(addHopButton_, &QAbstractButton::clicked, this, [this] { addHopRow(); });
    connect(saveChainButton_, &QAbstractButton::clicked, this, [this] { saveChain(); });
    connect(activateButton_, &QAbstractButton::clicked, this, [this] {
        const auto current = table_->selectionModel()->currentIndex();
        const auto* chain = model_->rowAt(current.isValid() ? current.row() : -1);
        if (chain)
            aida::burp::upstream::set_active_chain(chain->id);
        refreshFromStore();
    });
    connect(deactivateButton_, &QAbstractButton::clicked, this, [this] {
        aida::burp::upstream::set_active_chain(0);
        refreshFromStore();
    });
    connect(testButton_, &QAbstractButton::clicked, this, [this] { testSelectedChain(); });
    connect(removeButton_, &QAbstractButton::clicked, this, [this] {
        const auto current = table_->selectionModel()->currentIndex();
        const auto* chain = model_->rowAt(current.isValid() ? current.row() : -1);
        if (!chain)
            return;
        aida::burp::upstream::remove_chain(chain->id);
        refreshFromStore();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex&, const QModelIndex&) {
            updateActionBar();
        });

    setContent(content);
    refreshFromStore();
    updateActionBar();
}

void UpstreamPane::onPaneShown() {
    refreshFromStore();
    updateActionBar();
}

void UpstreamPane::refreshFromStore() {
    const auto chains = aida::burp::upstream::list_chains();
    model_->activeChainId = aida::burp::upstream::get_active_chain_id();
    ++generation_;
    model_->adopt(std::make_shared<const QVector<aida::burp::upstream::upstream_chain_t>>(
        chains.begin(), chains.end()), generation_);
    tableStack_->setCurrentWidget(model_->rowCount() == 0
        ? static_cast<QWidget*>(emptyView_) : static_cast<QWidget*>(table_));
    updateActionBar();
}

void UpstreamPane::addHopRow() {
    auto* row = new HopRowWidget(hopsContainer_);
    hopsLayout_->addWidget(row);
    connect(row, &HopRowWidget::moveUpRequested, this, [this](HopRowWidget* target) {
        const int index = hopsLayout_->indexOf(target);
        if (index > 0) {
            hopsLayout_->removeWidget(target);
            hopsLayout_->insertWidget(index - 1, target);
        }
    });
    connect(row, &HopRowWidget::moveDownRequested, this, [this](HopRowWidget* target) {
        const int index = hopsLayout_->indexOf(target);
        if (index >= 0 && index < hopsLayout_->count() - 1) {
            hopsLayout_->removeWidget(target);
            hopsLayout_->insertWidget(index + 1, target);
        }
    });
    connect(row, &HopRowWidget::removeRequested, this, [this](HopRowWidget* target) {
        hopsLayout_->removeWidget(target);
        target->deleteLater();
    });
}

void UpstreamPane::saveChain() {
    const QString label = labelEdit_->text();
    if (label.isEmpty())
        return;
    aida::burp::upstream::upstream_chain_t chain;
    chain.label = label.toStdString();
    for (int i = 0; i < hopsLayout_->count(); ++i) {
        auto* row = qobject_cast<HopRowWidget*>(hopsLayout_->itemAt(i)->widget());
        if (!row || !row->hasHost())
            continue;
        chain.hops.push_back(row->toHop());
    }
    if (chain.hops.empty())
        return;
    aida::burp::upstream::add_chain(chain);
    labelEdit_->clear();
    while (QLayoutItem* item = hopsLayout_->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    refreshFromStore();
}

void UpstreamPane::testSelectedChain() {
    const auto current = table_->selectionModel()->currentIndex();
    const auto* chain = model_->rowAt(current.isValid() ? current.row() : -1);
    if (!chain || testPending_)
        return;
    testPending_ = true;
    testButton_->setEnabled(false);
    testButton_->setText("Testing...");
    testResultLabel_->setText(QString());
    set_label_tone(testResultLabel_, "dim");

    const std::uint64_t chainId = chain->id;
    const std::string host = testHostEdit_->text().toStdString();
    const auto port = static_cast<uint16_t>(testPortSpin_->value());
    const std::uint64_t serial = ++testSerial_;

    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.upstream_view";
    submission.label = "upstream.test_chain";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [this, chainId, host, port, serial]() mutable {
        std::string error;
        const bool ok = aida::burp::upstream::test_chain(chainId, host, port, error);
        QMetaObject::invokeMethod(this,
            [this, chainId, ok, error = std::move(error), serial]() mutable {
                applyTestResult(chainId, ok, QString::fromStdString(error), serial);
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        testPending_ = false;
        testButton_->setEnabled(true);
        testButton_->setText("Test");
        testResultLabel_->setText("The test could not be scheduled.");
    }
}

void UpstreamPane::applyTestResult(std::uint64_t chainId, bool success, QString detail,
                                   std::uint64_t serial) {
    if (serial != testSerial_)
        return;
    testPending_ = false;
    testButton_->setEnabled(true);
    testButton_->setText("Test");
    testResultLabel_->setText(QStringLiteral("[chain %1] %2%3")
        .arg(static_cast<quint64>(chainId))
        .arg(success ? QStringLiteral("ok") : QStringLiteral("FAIL "))
        .arg(success ? QString() : detail));
    set_label_tone(testResultLabel_, success ? "success" : "error");
}

void UpstreamPane::updateActionBar() {
    const auto current = table_->selectionModel()->currentIndex();
    const auto* chain = model_->rowAt(current.isValid() ? current.row() : -1);
    const bool has = chain != nullptr;
    const bool isActive = has && chain->id == model_->activeChainId;
    activateButton_->setEnabled(has && !isActive && !testPending_);
    deactivateButton_->setEnabled(has && isActive && !testPending_);
    testButton_->setEnabled(has && !testPending_);
    removeButton_->setEnabled(has && !testPending_);
}

}
