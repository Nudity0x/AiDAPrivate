#include "qt/network/monitor/keylog_pane.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QItemSelectionModel>
#include <QCheckBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "core/ui/application_ui_runtime.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/network/shared/monitor_bridge.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

KeylogModel::KeylogModel(QObject* parent)
    : SnapshotTableModel(parent) {}

int KeylogModel::rowCount(const QModelIndex& parent) const {
    return SnapshotTableModel::rowCount(parent);
}

int KeylogModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant KeylogModel::cellData(const ssl_keylog::keylog_entry& row, int column, int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Time:  return QString::fromStdString(format_timestamp(row.timestamp));
        case Label: return QString::fromStdString(row.label);
        case ClientRandom: {
            QString value = QString::fromStdString(row.client_random_hex);
            return value.size() > 24 ? value.left(24) + QStringLiteral("...") : value;
        }
        case Secret: {
            QString value = QString::fromStdString(row.secret_hex);
            return value.size() > 24 ? value.left(24) + QStringLiteral("...") : value;
        }
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (column) {
        case Time: return t.text_dim;
        case Label:
            if (row.label == "CLIENT_RANDOM") return t.info;
            if (row.label.find("HANDSHAKE") != std::string::npos) return t.warning;
            return t.success;
        default: return t.text_secondary;
        }
    }
    return {};
}

QVariant KeylogModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Time:         return QStringLiteral("Time");
    case Label:        return QStringLiteral("Label");
    case ClientRandom: return QStringLiteral("Client Random");
    case Secret:       return QStringLiteral("Secret");
    default: return {};
    }
}

KeylogPane::KeylogPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.keylog"));
    setRequiresTarget(true);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* titleLabel = new QLabel("SSL Key Logger", content);
    titleLabel->setProperty("aidaTone", QStringLiteral("titleAccent"));
    layout->addWidget(titleLabel);

    auto* exeRow = new QHBoxLayout();
    exeRow->setSpacing(t.spacing.sm);
    exeRow->addWidget(new QLabel("Executable:", content));
    exePath_ = new QLineEdit(content);
    exePath_->setPlaceholderText("C:\\path\\to\\target.exe");
    exePath_->setMaxLength(511);
    exeRow->addWidget(exePath_, 1);
    browseExeButton_ = new widgets::AidaButton("Browse...", content);
    browseExeButton_->setKind(widgets::AidaButton::Kind::Secondary);
    browseExeButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    exeRow->addWidget(browseExeButton_);
    layout->addLayout(exeRow);

    auto* argsRow = new QHBoxLayout();
    argsRow->setSpacing(t.spacing.sm);
    argsRow->addWidget(new QLabel("Arguments:", content));
    argsEdit_ = new QLineEdit(content);
    argsEdit_->setPlaceholderText("Arguments to pass...");
    argsEdit_->setMaxLength(511);
    argsRow->addWidget(argsEdit_, 1);
    layout->addLayout(argsRow);

    auto* controlRow = new QHBoxLayout();
    controlRow->setSpacing(t.spacing.sm);
    launchButton_ = new widgets::AidaButton("Launch & Watch", content);
    launchButton_->setKind(widgets::AidaButton::Kind::Primary);
    launchButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controlRow->addWidget(launchButton_);
    watchFileButton_ = new widgets::AidaButton("Watch File...", content);
    watchFileButton_->setKind(widgets::AidaButton::Kind::Secondary);
    watchFileButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controlRow->addWidget(watchFileButton_);
    watchPath_ = new QLineEdit(content);
    watchPath_->setPlaceholderText("or paste a keylog path...");
    watchPath_->setMaxLength(511);
    controlRow->addWidget(watchPath_, 1);
    watchTypedButton_ = new widgets::AidaButton("Watch", content);
    watchTypedButton_->setKind(widgets::AidaButton::Kind::Ghost);
    watchTypedButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    controlRow->addWidget(watchTypedButton_);
    stopButton_ = new widgets::AidaButton("Stop Watching", content);
    stopButton_->setKind(widgets::AidaButton::Kind::Destructive);
    stopButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    stopButton_->setVisible(false);
    controlRow->addWidget(stopButton_);
    clearButton_ = new widgets::AidaButton("Clear", content);
    clearButton_->setKind(widgets::AidaButton::Kind::Secondary);
    clearButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    clearButton_->setVisible(false);
    controlRow->addWidget(clearButton_);
    layout->addLayout(controlRow);

    watchingLabel_ = new QLabel(content);
    watchingLabel_->setProperty("aidaTone", QStringLiteral("accent"));
    watchingLabel_->setVisible(false);
    layout->addWidget(watchingLabel_);

    errorNotice_ = new widgets::AidaNotice(QString(), QString(),
        widgets::AidaSemantic::Error, content);
    errorNotice_->setVisible(false);
    layout->addWidget(errorNotice_);

    countLabel_ = new QLabel("Captured Keys: 0", content);
    countLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    layout->addWidget(countLabel_);

    auto* tableHost = new QWidget(content);
    tableStack_ = new QStackedLayout(tableHost);
    tableStack_->setStackingMode(QStackedLayout::StackOne);
    tableStack_->setContentsMargins(0, 0, 0, 0);
    model_ = new KeylogModel(content);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.keylog.table"));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    table_->horizontalHeader()->setDefaultSectionSize(table_column_width_chars(table_, 14));
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setModel(model_);
    tableStack_->addWidget(table_);
    emptyView_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No keys captured"),
        QStringLiteral("Launch a target executable or watch an SSLKEYLOGFILE to start collecting TLS secrets."),
        tableHost);
    emptyView_->setObjectName(QStringLiteral("aida.view.network.keylog.empty"));
    emptyView_->setActionLabel(QStringLiteral("Watch File..."));
    connect(emptyView_, &widgets::AidaStateView::actionTriggered, this, [this] {
        if (watchFileButton_->isEnabled())
            watchFileButton_->click();
    });
    tableStack_->addWidget(emptyView_);
    layout->addWidget(tableHost, 1);

    auto* detailRow = new QHBoxLayout();
    detailRow->setSpacing(t.spacing.sm);
    detailLabel_ = new QLabel(content);
    detailLabel_->setProperty("aidaTone", QStringLiteral("dim"));
    detailRow->addWidget(detailLabel_, 1);
    copyClientRandom_ = new widgets::AidaButton("Copy Client Random", content);
    copyClientRandom_->setKind(widgets::AidaButton::Kind::Ghost);
    copyClientRandom_->setControlSize(widgets::AidaButton::ControlSize::Small);
    copySecret_ = new widgets::AidaButton("Copy Secret", content);
    copySecret_->setKind(widgets::AidaButton::Kind::Ghost);
    copySecret_->setControlSize(widgets::AidaButton::ControlSize::Small);
    copyLine_ = new widgets::AidaButton("Copy Line", content);
    copyLine_->setKind(widgets::AidaButton::Kind::Ghost);
    copyLine_->setControlSize(widgets::AidaButton::ControlSize::Small);
    for (auto* button : { copyClientRandom_, copySecret_, copyLine_ }) {
        button->setVisible(false);
        detailRow->addWidget(button);
    }
    layout->addLayout(detailRow);

    connect(browseExeButton_, &QAbstractButton::clicked, this, [this] {
        static const char k_exeFilter[] =
            "Executable (*.exe)\0*.exe\0"
            "All files (*.*)\0*.*\0\0";
        const auto picked = dialogs::open_file(this, "Select Target Executable", k_exeFilter);
        if (picked)
            exePath_->setText(QString::fromStdString(*picked));
    });
    connect(launchButton_, &QAbstractButton::clicked, this, [this] {
        syncFormToState();
        watchStartPending_ = true;
        lastStartWasLaunch_ = true;
        errorNotice_->setVisible(false);
        aida::ui::application_ui::execute_action("network.keylog.launch",
            aida::ui::action_invocation_source_t::toolbar);
        pendingPoll_->start();
    });
    connect(watchFileButton_, &QAbstractButton::clicked, this, [this] {
        static const char k_keylogFilter[] =
            "SSL Keylog (*.log;*.keylog;*.txt)\0*.log;*.keylog;*.txt\0"
            "All files (*.*)\0*.*\0\0";
        const auto picked = dialogs::open_file(this, "Watch SSLKEYLOGFILE", k_keylogFilter);
        if (!picked)
            return;
        watchPath_->setText(QString::fromStdString(*picked));
        syncFormToState();
        watchStartPending_ = true;
        lastStartWasLaunch_ = false;
        errorNotice_->setVisible(false);
        aida::ui::application_ui::execute_action("network.keylog.watch",
            aida::ui::action_invocation_source_t::toolbar);
        pendingPoll_->start();
    });
    connect(watchTypedButton_, &QAbstractButton::clicked, this, [this] {
        if (watchPath_->text().isEmpty())
            return;
        syncFormToState();
        watchStartPending_ = true;
        lastStartWasLaunch_ = false;
        errorNotice_->setVisible(false);
        aida::ui::application_ui::execute_action("network.keylog.watch",
            aida::ui::action_invocation_source_t::toolbar);
        pendingPoll_->start();
    });
    connect(stopButton_, &QAbstractButton::clicked, this, [this] {
        errorNotice_->setVisible(false);
        aida::ui::application_ui::execute_action("network.keylog.stop",
            aida::ui::action_invocation_source_t::toolbar);
        pendingPoll_->start();
    });
    connect(clearButton_, &QAbstractButton::clicked, this, [this] {
        errorNotice_->setVisible(false);
        aida::ui::application_ui::execute_action("network.keylog.clear",
            aida::ui::action_invocation_source_t::toolbar);
        pendingPoll_->start();
    });
    connect(exePath_, &QLineEdit::textChanged, this, [this] { syncFormToState(); refreshButtons(); });
    connect(argsEdit_, &QLineEdit::textChanged, this, [this] { syncFormToState(); });
    connect(watchPath_, &QLineEdit::textChanged, this, [this] { syncFormToState(); refreshButtons(); });

    const auto copySelection = [this](int part) {
        const auto current = table_->selectionModel()->currentIndex();
        const auto* row = model_->rowAt(current.isValid() ? current.row() : -1);
        if (!row)
            return;
        if (part == 0)
            clipboard::set_text(QString::fromStdString(row->client_random_hex));
        else if (part == 1)
            clipboard::set_text(QString::fromStdString(row->secret_hex));
        else
            clipboard::set_text(QString::fromStdString(
                row->label + " " + row->client_random_hex + " " + row->secret_hex));
    };
    connect(copyClientRandom_, &QAbstractButton::clicked, this, [copySelection] { copySelection(0); });
    connect(copySecret_, &QAbstractButton::clicked, this, [copySelection] { copySelection(1); });
    connect(copyLine_, &QAbstractButton::clicked, this, [copySelection] { copySelection(2); });
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this, [this, copySelection](const QPoint& pos) {
        const QModelIndex index = table_->indexAt(pos);
        if (!index.isValid())
            return;
        table_->setCurrentIndex(index);
        QMenu menu(this);
        auto* copyRandom = menu.addAction(QStringLiteral("Copy Client Random"));
        auto* copySecretAction = menu.addAction(QStringLiteral("Copy Secret"));
        auto* copyLineAction = menu.addAction(QStringLiteral("Copy Line"));
        connect(copyRandom, &QAction::triggered, this, [copySelection] { copySelection(0); });
        connect(copySecretAction, &QAction::triggered, this, [copySelection] { copySelection(1); });
        connect(copyLineAction, &QAction::triggered, this, [copySelection] { copySelection(2); });
        menu.popup(table_->viewport()->mapToGlobal(pos));
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex&, const QModelIndex&) {
            updateDetailRow();
        });

    if (auto* bridge = NetworkMonitorBridge::instance())
        connect(bridge, &NetworkMonitorBridge::keylogSnapshot, this, &KeylogPane::onSnapshot);

    pendingPoll_ = new QTimer(this);
    pendingPoll_->setInterval(200);
    connect(pendingPoll_, &QTimer::timeout, this, [this] {
        refreshButtons();
        const bool pending = network_view::keylog_operation_pending();
        if (!pending && watchStartPending_) {
            watchStartPending_ = false;
            if (!watching_) {
                errorNotice_->setTitle(lastStartWasLaunch_
                    ? QStringLiteral("Launch failed")
                    : QStringLiteral("Watch failed"));
                errorNotice_->setMessage(lastStartWasLaunch_
                    ? QStringLiteral("The target did not start with TLS key logging. "
                        "Check the executable path and arguments.")
                    : QStringLiteral("The keylog file could not be watched. "
                        "Check the path and try again."));
                errorNotice_->setVisible(true);
            }
        }
        updateEmptyState();
        if (!pending)
            pendingPoll_->stop();
    });

    setContent(content);
    refreshButtons();
    updateDetailRow();
    updateEmptyState();
}

void KeylogPane::onPaneShown() {
    network_view::request_driver_available_snapshot(true);
    network_view::request_keylog_runtime_snapshot(true);
    refreshButtons();
    updateEmptyState();
    if (network_view::keylog_operation_pending())
        pendingPoll_->start();
}

void KeylogPane::onSnapshot(
    std::shared_ptr<const network_view::keylog_runtime_snapshot_t> snapshot) {
    if (!snapshot)
        return;
    watching_ = snapshot->watching;
    if (watching_) {
        watchStartPending_ = false;
        errorNotice_->setVisible(false);
    }
    auto rows = std::make_shared<QVector<ssl_keylog::keylog_entry>>();
    rows->reserve(static_cast<qsizetype>(snapshot->entries.size()));
    for (auto it = snapshot->entries.rbegin(); it != snapshot->entries.rend(); ++it)
        rows->push_back(*it);
    ++generation_;
    model_->adopt(std::move(rows), generation_);
    countLabel_->setText(QStringLiteral("Captured Keys: %1")
        .arg(static_cast<quint64>(snapshot->entry_count)));
    watchingLabel_->setText(QStringLiteral("Watching: %1")
        .arg(snapshot->path.empty() ? QStringLiteral("active TLS keylog source")
                                    : QString::fromStdString(snapshot->path)));
    watchingLabel_->setVisible(watching_);
    updateEmptyState();
    refreshButtons();
    updateDetailRow();
}

void KeylogPane::refreshButtons() {
    const bool pending = network_view::keylog_operation_pending();
    launchButton_->setEnabled(!pending && !watching_ && !exePath_->text().isEmpty());
    launchButton_->setText(pending ? "Launching..." : "Launch & Watch");
    watchFileButton_->setEnabled(!pending && !watching_);
    watchTypedButton_->setEnabled(!pending && !watching_ && !watchPath_->text().isEmpty());
    stopButton_->setEnabled(!pending);
    stopButton_->setVisible(watching_);
    stopButton_->setText(pending ? "Stopping..." : "Stop Watching");
    clearButton_->setVisible(watching_);
    clearButton_->setEnabled(!pending && model_->rowCount() > 0);
    launchButton_->setVisible(!watching_);
    watchFileButton_->setVisible(!watching_);
    watchPath_->setVisible(!watching_);
    watchTypedButton_->setVisible(!watching_);
}

void KeylogPane::syncFormToState() {
    std::snprintf(network_view::g_state.kl_exe_path, sizeof(network_view::g_state.kl_exe_path),
        "%s", exePath_->text().toUtf8().constData());
    std::snprintf(network_view::g_state.kl_args, sizeof(network_view::g_state.kl_args),
        "%s", argsEdit_->text().toUtf8().constData());
    std::snprintf(network_view::g_state.kl_watch_path, sizeof(network_view::g_state.kl_watch_path),
        "%s", watchPath_->text().toUtf8().constData());
}

void KeylogPane::updateEmptyState() {
    if (!tableStack_ || !emptyView_ || !table_ || !model_)
        return;
    const bool empty = model_->rowCount() == 0;
    if (empty) {
        if (network_view::keylog_operation_pending()) {
            emptyView_->setState(widgets::AidaStateView::State::Loading);
            emptyView_->setTitle(QStringLiteral("Applying keylog operation"));
            emptyView_->setMessage(QStringLiteral(
                "Waiting for the TLS keylog watcher to update."));
            emptyView_->setActionLabel(QString());
        } else {
            emptyView_->setState(widgets::AidaStateView::State::Empty);
            emptyView_->setTitle(QStringLiteral("No keys captured"));
            emptyView_->setMessage(watching_
                ? QStringLiteral("Watching for TLS secrets; captured keys will appear here.")
                : QStringLiteral("Launch a target executable or watch an SSLKEYLOGFILE to start collecting TLS secrets."));
            emptyView_->setActionLabel(watching_
                ? QString() : QStringLiteral("Watch File..."));
        }
    }
    tableStack_->setCurrentWidget(empty
        ? static_cast<QWidget*>(emptyView_) : static_cast<QWidget*>(table_));
}

void KeylogPane::updateDetailRow() {
    const auto current = table_->selectionModel()->currentIndex();
    const auto* row = model_->rowAt(current.isValid() ? current.row() : -1);
    const bool has = row != nullptr;
    detailLabel_->setText(has
        ? QStringLiteral("Selected: %1  -  %2")
            .arg(QString::fromStdString(row->label))
            .arg(QString::fromStdString(format_timestamp(row->timestamp)))
        : QString());
    for (auto* button : { copyClientRandom_, copySecret_, copyLine_ })
        button->setVisible(has);
}

}
