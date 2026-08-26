#include "qt/network/burp/cookie_jar_pane.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QDialog>
#include <QItemSelectionModel>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QSplitter>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <memory>
#include <vector>

#include "core/network/burp/cookie_jar.hpp"
#include "qt/network/shared/event_bus_bridge.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

CookieModel::CookieModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void CookieModel::adopt(
    std::shared_ptr<const std::vector<aida::burp::cookie_jar::parsed_cookie_t>> cookies,
    std::uint64_t generation) {
    beginResetModel();
    cookies_ = std::move(cookies);
    generation_ = generation;
    endResetModel();
}

const aida::burp::cookie_jar::parsed_cookie_t* CookieModel::rowAt(int row) const noexcept {
    if (!cookies_ || row < 0 || row >= static_cast<int>(cookies_->size()))
        return nullptr;
    return &cookies_->at(static_cast<std::size_t>(row));
}

int CookieModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : (cookies_ ? static_cast<int>(cookies_->size()) : 0);
}

int CookieModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant CookieModel::cellData(const aida::burp::cookie_jar::parsed_cookie_t& row, int column,
                               int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case Name: return QString::fromStdString(row.name);
        case Value: {
            QString value = QString::fromStdString(row.value);
            if (value.size() > 96)
                value = value.left(96) + QStringLiteral("...");
            return value;
        }
        case Secure:   return row.secure ? QStringLiteral("yes") : QStringLiteral("no");
        case HttpOnly: return row.http_only ? QStringLiteral("yes") : QStringLiteral("no");
        case SameSite: {
            const std::string ss = aida::burp::cookie_jar::same_site_str(row.same_site);
            return ss.empty() ? QStringLiteral("-") : QString::fromStdString(ss);
        }
        case Expires:
            if (row.has_expires && row.expires_unix_ms > 0) {
                const time_t when = static_cast<time_t>(row.expires_unix_ms / 1000);
                std::tm tmv = {};
                gmtime_s(&tmv, &when);
                char buf[64];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tmv);
                return QString::fromLatin1(buf);
            }
            return QStringLiteral("session");
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (column) {
        case Secure:
        case HttpOnly: return t.text_primary;
        case Expires:  return t.text_dim;
        case Value:    return t.text_secondary;
        default:       return t.text_primary;
        }
    }
    return {};
}

QVariant CookieModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* row = rowAt(index.row());
    if (!row)
        return {};
    return cellData(*row, index.column(), role);
}

void CookieModel::multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const {
    if (!index.isValid()) {
        for (auto& roleData : roleDataSpan)
            roleData.clearData();
        return;
    }
    const auto* row = rowAt(index.row());
    if (!row) {
        for (auto& roleData : roleDataSpan)
            roleData.clearData();
        return;
    }
    for (auto& roleData : roleDataSpan)
        roleData.setData(cellData(*row, index.column(), roleData.role()));
}

QVariant CookieModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Name:     return QStringLiteral("Name");
    case Value:    return QStringLiteral("Value");
    case Secure:   return QStringLiteral("Secure");
    case HttpOnly: return QStringLiteral("HttpOnly");
    case SameSite: return QStringLiteral("SameSite");
    case Expires:  return QStringLiteral("Expires");
    default: return {};
    }
}

CookieEditDialog::CookieEditDialog(
    const QString& host, const aida::burp::cookie_jar::parsed_cookie_t& cookie, QWidget* parent)
    : AidaDialog(parent) {
    setWindowTitle("Edit cookie");
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    hostEdit_ = new QLineEdit(host, this);
    hostEdit_->setMaxLength(255);
    form->addRow("Host", hostEdit_);
    nameEdit_ = new QLineEdit(QString::fromStdString(cookie.name), this);
    nameEdit_->setMaxLength(127);
    form->addRow("Name", nameEdit_);
    valueEdit_ = new QLineEdit(QString::fromStdString(cookie.value), this);
    valueEdit_->setMaxLength(2047);
    form->addRow("Value", valueEdit_);
    domainEdit_ = new QLineEdit(QString::fromStdString(cookie.domain), this);
    domainEdit_->setMaxLength(255);
    form->addRow("Domain", domainEdit_);
    pathEdit_ = new QLineEdit(QString::fromStdString(cookie.path), this);
    pathEdit_->setMaxLength(255);
    form->addRow("Path", pathEdit_);
    QString expiresText;
    if (cookie.has_expires && cookie.expires_unix_ms > 0) {
        const time_t when = static_cast<time_t>(cookie.expires_unix_ms / 1000);
        std::tm tmv = {};
        gmtime_s(&tmv, &when);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tmv);
        expiresText = QString::fromLatin1(buf);
    }
    expiresEdit_ = new QLineEdit(expiresText, this);
    expiresEdit_->setMaxLength(63);
    form->addRow("Expires", expiresEdit_);
    secureCheck_ = new QCheckBox("Secure", this);
    secureCheck_->setChecked(cookie.secure);
    httpOnlyCheck_ = new QCheckBox("HttpOnly", this);
    httpOnlyCheck_->setChecked(cookie.http_only);
    auto* flagsRow = new QWidget(this);
    auto* flagsLayout = new QHBoxLayout(flagsRow);
    flagsLayout->setContentsMargins(0, 0, 0, 0);
    flagsLayout->addWidget(secureCheck_);
    flagsLayout->addWidget(httpOnlyCheck_);
    flagsLayout->addStretch(1);
    form->addRow(flagsRow);
    sameSiteCombo_ = new QComboBox(this);
    sameSiteCombo_->addItems({"Unset", "Lax", "Strict", "None"});
    sameSiteCombo_->setCurrentIndex(static_cast<int>(cookie.same_site));
    form->addRow("SameSite", sameSiteCombo_);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        aida::burp::cookie_jar::parsed_cookie_t updated;
        updated.name = nameEdit_->text().toStdString();
        updated.value = valueEdit_->text().toStdString();
        std::string domain = domainEdit_->text().toStdString();
        std::transform(domain.begin(), domain.end(), domain.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        updated.domain = domain;
        updated.path = pathEdit_->text().isEmpty() ? "/" : pathEdit_->text().toStdString();
        updated.secure = secureCheck_->isChecked();
        updated.http_only = httpOnlyCheck_->isChecked();
        updated.same_site = static_cast<aida::burp::cookie_jar::same_site_t>(sameSiteCombo_->currentIndex());
        updated.created_unix_ms = 0;
        if (!expiresEdit_->text().isEmpty()) {
            updated.has_expires = true;
            updated.expires_unix_ms =
                aida::burp::cookie_jar::parse_cookie_expires(expiresEdit_->text().toStdString());
        }
        aida::burp::cookie_jar::set_cookie(hostEdit_->text().toStdString(), updated);
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    setMinimumWidth(dialog_min_width_chars(this, 48));
}

CookieJarPane::CookieJarPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.cookies"));
    setRequiresTarget(false);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* titleLabel = new QLabel("Cookie jar", content);
    titleLabel->setProperty("aidaTone", QStringLiteral("title"));
    layout->addWidget(titleLabel);

    banner_ = new QFrame(content);
    banner_->setProperty("aidaRole", QStringLiteral("notice"));
    banner_->setProperty("aidaVariant", QStringLiteral("warning"));
    auto* bannerLayout = new QVBoxLayout(banner_);
    auto* bannerTopRow = new QHBoxLayout();
    bannerTopRow->setSpacing(t.spacing.sm);
    bannerStatus_ = new QLabel(banner_);
    bannerStatus_->setProperty("aidaTone", QStringLiteral("titleWarning"));
    bannerTopRow->addWidget(bannerStatus_);
    bannerRecheck_ = new widgets::AidaButton("Recheck", banner_);
    bannerRecheck_->setKind(widgets::AidaButton::Kind::Ghost);
    bannerRecheck_->setControlSize(widgets::AidaButton::ControlSize::Small);
    bannerRecheck_->setToolTip(QStringLiteral("Revalidate the reviewed cookie-filter context"));
    bannerTopRow->addWidget(bannerRecheck_);
    bannerClear_ = new widgets::AidaButton("Clear", banner_);
    bannerClear_->setKind(widgets::AidaButton::Kind::Ghost);
    bannerClear_->setControlSize(widgets::AidaButton::ControlSize::Small);
    bannerClear_->setToolTip(QStringLiteral("Dismiss the reviewed-context banner"));
    bannerTopRow->addWidget(bannerClear_);
    bannerTopRow->addStretch(1);
    bannerLabel_ = new QLabel(banner_);
    bannerTopRow->addWidget(bannerLabel_, 1);
    bannerLayout->addLayout(bannerTopRow);
    bannerReason_ = new QLabel(banner_);
    bannerReason_->setWordWrap(true);
    bannerReason_->setProperty("aidaTone", QStringLiteral("secondary"));
    bannerLayout->addWidget(bannerReason_);
    banner_->setVisible(false);
    layout->addWidget(banner_);

    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(t.spacing.sm);
    toolbar->addWidget(new QLabel("Filter host:", content));
    filterEdit_ = new QLineEdit(content);
    filterEdit_->setPlaceholderText("example.com");
    filterEdit_->setMaxLength(255);
    filterEdit_->setMaximumWidth(field_width_chars(filterEdit_, 24));
    toolbar->addWidget(filterEdit_);
    editButton_ = new widgets::AidaButton("Edit selected", content);
    editButton_->setKind(widgets::AidaButton::Kind::Secondary);
    editButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    editButton_->setEnabled(false);
    toolbar->addWidget(editButton_);
    deleteButton_ = new widgets::AidaButton("Delete selected", content);
    deleteButton_->setKind(widgets::AidaButton::Kind::Secondary);
    deleteButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    deleteButton_->setEnabled(false);
    toolbar->addWidget(deleteButton_);
    clearAllButton_ = new widgets::AidaButton("Clear all", content);
    clearAllButton_->setKind(widgets::AidaButton::Kind::Destructive);
    clearAllButton_->setControlSize(widgets::AidaButton::ControlSize::Small);
    toolbar->addWidget(clearAllButton_);
    toolbar->addStretch(1);
    layout->addLayout(toolbar);

    splitter_ = new QSplitter(Qt::Horizontal, content);
    splitter_->setOpaqueResize(true);
    splitter_->setChildrenCollapsible(false);
    hostModel_ = new QStringListModel(content);
    hostList_ = new QListView(splitter_);
    hostList_->setModel(hostModel_);
    hostList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    hostList_->setAlternatingRowColors(true);
    hostList_->setUniformItemSizes(true);
    splitter_->addWidget(hostList_);

    cookieModel_ = new CookieModel(splitter_);
    auto* cookieHost = new QWidget(splitter_);
    cookieStack_ = new QStackedLayout(cookieHost);
    cookieStack_->setStackingMode(QStackedLayout::StackOne);
    cookieStack_->setContentsMargins(0, 0, 0, 0);
    cookieTable_ = new QTableView(cookieHost);
    cookieTable_->setObjectName(QStringLiteral("aida.view.network.cookies.table"));
    cookieTable_->verticalHeader()->hide();
    cookieTable_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    cookieTable_->horizontalHeader()->setDefaultSectionSize(table_column_width_chars(cookieTable_, 9));
    cookieTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    cookieTable_->horizontalHeader()->setStretchLastSection(true);
    cookieTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    cookieTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    cookieTable_->setAlternatingRowColors(true);
    cookieTable_->setShowGrid(false);
    cookieTable_->setModel(cookieModel_);
    cookieTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    cookieStack_->addWidget(cookieTable_);
    cookieEmptyView_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No cookies for this host"),
        QStringLiteral("Cookies captured by the proxy appear here; select another host or keep browsing."),
        cookieHost);
    cookieEmptyView_->setObjectName(QStringLiteral("aida.view.network.cookies.empty"));
    cookieStack_->addWidget(cookieEmptyView_);
    splitter_->addWidget(cookieHost);
    splitter_->setStretchFactor(0, 11);
    splitter_->setStretchFactor(1, 35);
    layout->addWidget(splitter_, 1);

    connect(filterEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        refreshFromStore();
    });
    connect(hostList_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            currentHost_ = current.isValid() ? current.data(Qt::DisplayRole).toString() : QString();
            refreshFromStore();
        });
    connect(editButton_, &QAbstractButton::clicked, this, [this] { editSelected(); });
    connect(deleteButton_, &QAbstractButton::clicked, this, [this] { deleteSelected(); });
    connect(clearAllButton_, &QAbstractButton::clicked, this, [] {
        aida::burp::cookie_jar::clear_all();
    });
    connect(cookieTable_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const bool has = current.isValid() && cookieModel_->rowAt(current.row());
            editButton_->setEnabled(has);
            deleteButton_->setEnabled(has);
        });
    connect(cookieTable_, &QTableView::doubleClicked, this, [this](const QModelIndex&) {
        editSelected();
    });
    connect(cookieTable_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = cookieTable_->indexAt(pos);
        if (!index.isValid())
            return;
        cookieTable_->setCurrentIndex(index);
        QMenu menu(this);
        auto* editAction = menu.addAction(QStringLiteral("Edit selected"));
        auto* deleteAction = menu.addAction(QStringLiteral("Delete selected"));
        connect(editAction, &QAction::triggered, this, [this] { editSelected(); });
        connect(deleteAction, &QAction::triggered, this, [this] { deleteSelected(); });
        menu.popup(cookieTable_->viewport()->mapToGlobal(pos));
    });
    connect(bannerRecheck_, &QAbstractButton::clicked, this, [] {
        aida::burp::cookie_jar::revalidate_reviewed_context();
    });
    connect(bannerClear_, &QAbstractButton::clicked, this, [this] {
        aida::burp::cookie_jar::clear_reviewed_context();
        banner_->setVisible(false);
    });

    if (auto* bridge = NetworkEventBusBridge::instance()) {
        connect(bridge, &NetworkEventBusBridge::cookieChanged, this, [this](const aida::burp::cookie_changed_t&) {
            refreshFromStore();
            refreshBanner();
        });
    }

    bannerTimer_ = new QTimer(this);
    bannerTimer_->setInterval(250);
    connect(bannerTimer_, &QTimer::timeout, this, [this] {
        refreshBanner();
    });

    setContent(content);
    refreshFromStore();
    refreshBanner();
}

void CookieJarPane::onPaneShown() {
    bannerClock_.start();
    bannerTimer_->start();
    refreshFromStore();
    refreshBanner();
}

void CookieJarPane::onPaneHidden() {
    bannerTimer_->stop();
}

void CookieJarPane::refreshFromStore() {
    const auto all = aida::burp::cookie_jar::list_all();
    QStringList hosts;
    std::map<std::string, int> counts;
    for (const auto& cookie : all)
        counts[cookie.domain.empty() ? "" : cookie.domain]++;
    for (const auto& kv : counts)
        hosts << QString::fromStdString(kv.first);

    const QString filter = filterEdit_ ? filterEdit_->text().toLower() : QString();
    QStringList filtered;
    for (const auto& host : hosts) {
        if (filter.isEmpty() || host.toLower().contains(filter))
            filtered << host;
    }
    hostModel_->setStringList(filtered);

    if (!currentHost_.isEmpty() && !filtered.contains(currentHost_))
        currentHost_.clear();
    if (currentHost_.isEmpty() && !filtered.isEmpty())
        currentHost_ = filtered.first();
    if (!currentHost_.isEmpty()) {
        const QModelIndex index = hostModel_->index(filtered.indexOf(currentHost_), 0);
        if (index.isValid())
            hostList_->setCurrentIndex(index);
    }

    std::shared_ptr<std::vector<aida::burp::cookie_jar::parsed_cookie_t>> cookies;
    if (!currentHost_.isEmpty()) {
        cookies = std::make_shared<std::vector<aida::burp::cookie_jar::parsed_cookie_t>>(
            aida::burp::cookie_jar::list_for_host(currentHost_.toStdString()));
    } else {
        cookies = std::make_shared<std::vector<aida::burp::cookie_jar::parsed_cookie_t>>();
    }
    ++generation_;
    cookieModel_->adopt(std::move(cookies), generation_);
    updateEmptyState();
}

void CookieJarPane::updateEmptyState() {
    if (!cookieStack_ || !cookieEmptyView_ || !cookieTable_ || !cookieModel_)
        return;
    cookieStack_->setCurrentWidget(cookieModel_->rowCount() == 0
        ? static_cast<QWidget*>(cookieEmptyView_) : static_cast<QWidget*>(cookieTable_));
}

void CookieJarPane::refreshBanner() {
    aida::burp::cookie_jar::reviewed_context_view_t view;
    if (!aida::burp::cookie_jar::reviewed_context_snapshot(view)) {
        banner_->setVisible(false);
        return;
    }
    if (bannerClock_.isValid() && bannerClock_.elapsed() >= 2000) {
        bannerClock_.restart();
        aida::burp::cookie_jar::revalidate_reviewed_context();
        aida::burp::cookie_jar::reviewed_context_snapshot(view);
    }
    banner_->setVisible(true);
    bannerStatus_->setText(view.current ? "CURRENT AT LAST CHECK" : "STALE FILTER CONTEXT");
    set_label_tone(bannerStatus_, view.current ? "titleSuccess" : "titleError");
    set_aida_property(banner_, "aidaVariant", view.current
        ? QStringLiteral("success") : QStringLiteral("error"));
    bannerLabel_->setText(view.identity.label.empty()
        ? QString::fromStdString(view.identity.id)
        : QString::fromStdString(view.identity.label));
    bannerReason_->setText(QStringLiteral("%1://%2:%3%4 | rev %5 | hash %6 | %7 bytes")
        .arg(view.identity.use_tls ? "https" : "http")
        .arg(QString::fromStdString(view.identity.target_host))
        .arg(view.identity.target_port)
        .arg(QString::fromStdString(view.path))
        .arg(static_cast<quint64>(view.identity.revision))
        .arg(static_cast<quint64>(view.identity.content_hash), 16, QLatin1Char('0'))
        .arg(static_cast<quint64>(view.identity.content_size)));
    if (!view.current && !view.reason.empty()) {
        bannerReason_->setText(bannerReason_->text() + QStringLiteral("\n") +
            QString::fromStdString(view.reason));
    }
}

void CookieJarPane::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete &&
        cookieTable_->selectionModel()->currentIndex().isValid() &&
        cookieTable_->hasFocus()) {
        deleteSelected();
        return;
    }
    if (event->key() == Qt::Key_Menu && cookieTable_->hasFocus()) {
        const QModelIndex current = cookieTable_->selectionModel()->currentIndex();
        if (!current.isValid())
            return;
        QMenu menu(this);
        auto* editAction = menu.addAction(QStringLiteral("Edit selected"));
        auto* deleteAction = menu.addAction(QStringLiteral("Delete selected"));
        connect(editAction, &QAction::triggered, this, [this] { editSelected(); });
        connect(deleteAction, &QAction::triggered, this, [this] { deleteSelected(); });
        menu.popup(cookieTable_->viewport()->mapToGlobal(
            cookieTable_->visualRect(current).center()));
        return;
    }
    NetworkPaneBase::keyPressEvent(event);
}

void CookieJarPane::editSelected() {
    const auto current = cookieTable_->selectionModel()->currentIndex();
    const auto* cookie = cookieModel_->rowAt(current.isValid() ? current.row() : -1);
    if (!cookie)
        return;
    showEditFor(currentHost_, *cookie);
}

void CookieJarPane::deleteSelected() {
    const auto current = cookieTable_->selectionModel()->currentIndex();
    const auto* cookie = cookieModel_->rowAt(current.isValid() ? current.row() : -1);
    if (!cookie || currentHost_.isEmpty())
        return;
    aida::burp::cookie_jar::delete_cookie(currentHost_.toStdString(), cookie->name, cookie->path);
    refreshFromStore();
}

void CookieJarPane::showEditFor(const QString& host,
                                const aida::burp::cookie_jar::parsed_cookie_t& cookie) {
    auto* dialog = new CookieEditDialog(host, cookie, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this] {
        refreshFromStore();
    });
    dialog->open();
}

}
