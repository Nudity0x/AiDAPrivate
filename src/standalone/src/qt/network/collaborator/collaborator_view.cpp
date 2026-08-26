#include "qt/network/collaborator/collaborator_view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
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
#include <algorithm>
#include <utility>
#include "core/infra/executor.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/network/burp_operation.hpp"
#include "qt/network/burp_review_dialog.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

bool same_runtime(const aida::burp::collaborator::status_t& left,
                  const aida::burp::collaborator::status_t& right) {
    return left.running == right.running && left.bind_ip == right.bind_ip &&
        left.http_alive == right.http_alive && left.dns_alive == right.dns_alive &&
        left.smtp_alive == right.smtp_alive &&
        left.http_port == right.http_port && left.dns_port == right.dns_port &&
        left.smtp_port == right.smtp_port && left.public_host == right.public_host &&
        left.public_ip == right.public_ip && left.interaction_count == right.interaction_count &&
        left.token_count == right.token_count && left.started_ms == right.started_ms;
}

}

class CollaboratorView::DetailModel : public QAbstractTableModel {
public:
    explicit DetailModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    void setInteraction(const aida::burp::collaborator::interaction_t* interaction) {
        beginResetModel();
        pairs_.clear();
        if (interaction) {
            for (const auto& kv : interaction->details)
                pairs_.push_back(kv);
        }
        endResetModel();
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(pairs_.size());
    }
    int columnCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : 2;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= static_cast<int>(pairs_.size()))
            return {};
        const auto& kv = pairs_[static_cast<std::size_t>(index.row())];
        if (role == Qt::DisplayRole) {
            return QString::fromStdString(index.column() == 0 ? kv.first : kv.second);
        }
        return {};
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        return section == 0 ? QStringLiteral("Field") : QStringLiteral("Value");
    }
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override {
        for (auto& roleData : roleDataSpan)
            roleData.setData(data(index, roleData.role()));
    }

private:
    std::vector<std::pair<std::string, std::string>> pairs_;
};

CollaboratorInteractionsModel::CollaboratorInteractionsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int CollaboratorInteractionsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(filtered_indices_.size());
}

int CollaboratorInteractionsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant CollaboratorInteractionsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* interaction = interactionAt(index.row());
    if (!interaction)
        return {};
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Id: return QString::number(static_cast<unsigned long long>(interaction->id));
        case Time: {
            std::uint64_t age = (started_ms_ == 0) ? 0
                : (interaction->timestamp_ms > started_ms_
                    ? interaction->timestamp_ms - started_ms_ : 0);
            const std::uint64_t seconds = age / 1000;
            const std::uint64_t ms = age % 1000;
            return QStringLiteral("+%1.%2s")
                .arg(static_cast<unsigned long long>(seconds))
                .arg(static_cast<unsigned long long>(ms), 3, 10, QLatin1Char('0'));
        }
        case Kind: return QString::fromStdString(interaction->kind);
        case Client: return QStringLiteral("%1:%2")
            .arg(QString::fromStdString(interaction->client_ip))
            .arg(static_cast<unsigned>(interaction->client_port));
        case Token: return interaction->payload_token.empty()
            ? QStringLiteral("-") : QString::fromStdString(interaction->payload_token);
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (index.column()) {
        case Token: return interaction->payload_token.empty()
            ? QVariant() : QString::fromStdString(interaction->payload_token);
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole)
        return theme::tokens().text_secondary;
    return {};
}

QVariant CollaboratorInteractionsModel::headerData(int section, Qt::Orientation orientation,
                                                   int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Id: return QStringLiteral("ID");
    case Time: return QStringLiteral("Time");
    case Kind: return QStringLiteral("Kind");
    case Client: return QStringLiteral("Client");
    case Token: return QStringLiteral("Token");
    default: return {};
    }
}

void CollaboratorInteractionsModel::multiData(const QModelIndex& index,
                                              QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

void CollaboratorInteractionsModel::setPublication(
    std::shared_ptr<const std::vector<aida::burp::collaborator::interaction_t>> publication,
    std::uint64_t generation, std::uint64_t started_ms) {
    publication_ = std::move(publication);
    generation_ = generation;
    started_ms_ = started_ms;
    refilter();
}

void CollaboratorInteractionsModel::setFilter(const std::string& kind,
                                              const std::string& token,
                                              const std::string& ip) {
    if (filter_kind_ == kind && filter_token_ == token && filter_ip_ == ip)
        return;
    filter_kind_ = kind;
    filter_token_ = token;
    filter_ip_ = ip;
    refilter();
}

const aida::burp::collaborator::interaction_t*
CollaboratorInteractionsModel::interactionAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(filtered_indices_.size()) || !publication_)
        return nullptr;
    return &(*publication_)[filtered_indices_[static_cast<std::size_t>(row)]];
}

const aida::burp::collaborator::interaction_t*
CollaboratorInteractionsModel::findById(std::uint64_t id) const noexcept {
    if (!publication_)
        return nullptr;
    for (const auto& interaction : *publication_) {
        if (interaction.id == id)
            return &interaction;
    }
    return nullptr;
}

void CollaboratorInteractionsModel::refilter() {
    beginResetModel();
    filtered_indices_.clear();
    if (publication_) {
        filtered_indices_.reserve(publication_->size());
        for (std::size_t index = publication_->size(); index > 0; --index) {
            const auto& interaction = (*publication_)[index - 1];
            if (filter_kind_ != "all" && interaction.kind != filter_kind_)
                continue;
            if (!filter_token_.empty() &&
                interaction.payload_token.find(filter_token_) == std::string::npos)
                continue;
            if (!filter_ip_.empty() &&
                interaction.client_ip.find(filter_ip_) == std::string::npos)
                continue;
            filtered_indices_.push_back(index - 1);
        }
    }
    endResetModel();
}

CollaboratorView::CollaboratorView(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.collaborator"));
    const auto& t = theme::tokens();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    status_card_ = new QFrame(content);
    status_card_->setFrameShape(QFrame::StyledPanel);
    status_card_->setProperty("aidaRole", QStringLiteral("card"));
    auto* cardLayout = new QVBoxLayout(status_card_);
    cardLayout->setContentsMargins(t.panel.padding_compact, t.panel.padding_compact,
        t.panel.padding_compact, t.panel.padding_compact);
    cardLayout->setSpacing(t.spacing.xs);
    auto* cardTopRow = new QHBoxLayout();
    cardTopRow->setSpacing(t.spacing.sm);
    status_label_ = new QLabel(QStringLiteral("STOPPED"), status_card_);
    set_label_tone(status_label_, "title");
    cardTopRow->addWidget(status_label_);
    listeners_label_ = new QLabel(status_card_);
    listeners_label_->setProperty("aidaTone", QStringLiteral("dim"));
    cardTopRow->addWidget(listeners_label_, 1);
    start_stop_button_ = new widgets::AidaButton(QStringLiteral("Start"), status_card_);
    start_stop_button_->setKind(widgets::AidaButton::Kind::Primary);
    start_stop_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    cardTopRow->addWidget(start_stop_button_);
    generate_button_ = new widgets::AidaButton(QStringLiteral("Generate Token"), status_card_);
    generate_button_->setKind(widgets::AidaButton::Kind::Secondary);
    generate_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    cardTopRow->addWidget(generate_button_);
    copy_domain_button_ = new widgets::AidaButton(QStringLiteral("Copy Domain"), status_card_);
    copy_domain_button_->setKind(widgets::AidaButton::Kind::Ghost);
    copy_domain_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    cardTopRow->addWidget(copy_domain_button_);
    clear_button_ = new widgets::AidaButton(QStringLiteral("Clear"), status_card_);
    clear_button_->setKind(widgets::AidaButton::Kind::Ghost);
    clear_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    cardTopRow->addWidget(clear_button_);
    cardLayout->addLayout(cardTopRow);
    public_label_ = new QLabel(status_card_);
    public_label_->setProperty("aidaTone", QStringLiteral("dim"));
    cardLayout->addWidget(public_label_);
    token_label_ = new QLabel(status_card_);
    token_label_->setProperty("aidaTone", QStringLiteral("dim"));
    token_label_->setVisible(false);
    cardLayout->addWidget(token_label_);
    auto* opRow = new QHBoxLayout();
    opRow->setSpacing(t.spacing.sm);
    op_status_label_ = new QLabel(status_card_);
    opRow->addWidget(op_status_label_, 1);
    retry_button_ = new widgets::AidaButton(QStringLiteral("Retry"), status_card_);
    retry_button_->setKind(widgets::AidaButton::Kind::Secondary);
    retry_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    retry_button_->setVisible(false);
    opRow->addWidget(retry_button_);
    cardLayout->addLayout(opRow);
    layout->addWidget(status_card_);

    runner_ = new BurpOperationRunner(QStringLiteral("burp_ui"), this);

    config_form_ = new QWidget(content);
    auto* configLayout = new QHBoxLayout(config_form_);
    configLayout->setContentsMargins(0, 0, 0, 0);
    configLayout->setSpacing(t.spacing.sm);
    configLayout->addWidget(new QLabel(QStringLiteral("Bind IP"), config_form_));
    bind_ip_ = new QLineEdit(QStringLiteral("0.0.0.0"), config_form_);
    bind_ip_->setMaxLength(63);
    configLayout->addWidget(bind_ip_);
    configLayout->addWidget(new QLabel(QStringLiteral("Public host"), config_form_));
    public_host_ = new QLineEdit(QStringLiteral("aidacollab.local"), config_form_);
    public_host_->setMaxLength(255);
    configLayout->addWidget(public_host_);
    configLayout->addWidget(new QLabel(QStringLiteral("Public IP"), config_form_));
    public_ip_ = new QLineEdit(QStringLiteral("127.0.0.1"), config_form_);
    public_ip_->setMaxLength(63);
    configLayout->addWidget(public_ip_);
    enable_http_ = new QCheckBox(QStringLiteral("HTTP"), config_form_);
    enable_http_->setChecked(true);
    configLayout->addWidget(enable_http_);
    http_port_ = new QSpinBox(config_form_);
    http_port_->setRange(1, 65535);
    http_port_->setValue(8444);
    configLayout->addWidget(http_port_);
    enable_dns_ = new QCheckBox(QStringLiteral("DNS"), config_form_);
    enable_dns_->setChecked(true);
    configLayout->addWidget(enable_dns_);
    dns_port_ = new QSpinBox(config_form_);
    dns_port_->setRange(1, 65535);
    dns_port_->setValue(5353);
    configLayout->addWidget(dns_port_);
    enable_smtp_ = new QCheckBox(QStringLiteral("SMTP"), config_form_);
    enable_smtp_->setChecked(true);
    configLayout->addWidget(enable_smtp_);
    smtp_port_ = new QSpinBox(config_form_);
    smtp_port_->setRange(1, 65535);
    smtp_port_->setValue(2525);
    configLayout->addWidget(smtp_port_);
    configLayout->addWidget(new QLabel(QStringLiteral("Canned body"), config_form_));
    canned_body_ = new QLineEdit(config_form_);
    canned_body_->setMaxLength(1023);
    configLayout->addWidget(canned_body_, 1);
    configLayout->addWidget(new QLabel(QStringLiteral("CT"), config_form_));
    canned_ct_ = new QLineEdit(QStringLiteral("text/plain"), config_form_);
    canned_ct_->setMaxLength(127);
    configLayout->addWidget(canned_ct_);
    layout->addWidget(config_form_);

    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(t.spacing.sm);
    filterRow->addWidget(new QLabel(QStringLiteral("Filter:"), content));
    filter_kind_ = new QComboBox(content);
    filter_kind_->addItems({QStringLiteral("all"), QStringLiteral("http"),
        QStringLiteral("dns"), QStringLiteral("smtp")});
    filterRow->addWidget(filter_kind_);
    filterRow->addWidget(new QLabel(QStringLiteral("Token"), content));
    filter_token_ = new QLineEdit(content);
    filter_token_->setMaxLength(63);
    filterRow->addWidget(filter_token_);
    filterRow->addWidget(new QLabel(QStringLiteral("Client IP"), content));
    filter_ip_ = new QLineEdit(content);
    filter_ip_->setMaxLength(63);
    filterRow->addWidget(filter_ip_);
    filterRow->addStretch(1);
    layout->addLayout(filterRow);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);
    auto* listPanel = new QWidget(splitter);
    auto* listLayout = new QVBoxLayout(listPanel);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(t.spacing.xs);
    auto* listTitle = new QLabel(QStringLiteral("Interactions"), listPanel);
    listTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    listLayout->addWidget(listTitle);
    model_ = new CollaboratorInteractionsModel(listPanel);
    auto* tableHost = new QWidget(listPanel);
    table_stack_ = new QStackedLayout(tableHost);
    table_stack_->setStackingMode(QStackedLayout::StackOne);
    table_stack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.collaborator.table"));
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
        QStringLiteral("No interactions"),
        QStringLiteral("Start the listeners and Generate Token; DNS/HTTP/SMTP callbacks appear here."),
        tableHost);
    empty_view_->setObjectName(QStringLiteral("aida.view.network.collaborator.empty"));
    table_stack_->addWidget(empty_view_);
    listLayout->addWidget(tableHost, 1);
    connect(model_, &QAbstractItemModel::modelReset, this, [this] { updateEmptyState(); });
    connect(model_, &QAbstractItemModel::rowsInserted, this, [this] { updateEmptyState(); });
    connect(model_, &QAbstractItemModel::rowsRemoved, this, [this] { updateEmptyState(); });
    splitter->addWidget(listPanel);

    auto* detailPanel = new QWidget(splitter);
    auto* detailLayout = new QVBoxLayout(detailPanel);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(t.spacing.xs);
    auto* detailTitle = new QLabel(QStringLiteral("Detail"), detailPanel);
    detailTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    detailLayout->addWidget(detailTitle);
    detail_header_ = new QLabel(detailPanel);
    detail_header_->setProperty("aidaTone", QStringLiteral("secondary"));
    detail_header_->setWordWrap(true);
    detailLayout->addWidget(detail_header_);
    detail_model_ = new DetailModel(detailPanel);
    detail_table_ = new QTableView(detailPanel);
    detail_table_->verticalHeader()->hide();
    detail_table_->verticalHeader()->setDefaultSectionSize(t.table.compact_row_h);
    detail_table_->horizontalHeader()->setStretchLastSection(true);
    detail_table_->setSelectionMode(QAbstractItemView::NoSelection);
    detail_table_->setShowGrid(false);
    detail_table_->setModel(detail_model_);
    detailLayout->addWidget(detail_table_);
    auto* rawTitle = new QLabel(QStringLiteral("Raw"), detailPanel);
    rawTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    detailLayout->addWidget(rawTitle);
    detail_raw_ = new QPlainTextEdit(detailPanel);
    detail_raw_->setReadOnly(true);
    detail_raw_->setFont(theme::fonts::codeRegular());
    detail_raw_->setPlaceholderText(QStringLiteral("Select an interaction to view its raw payload"));
    detailLayout->addWidget(detail_raw_, 1);
    splitter->addWidget(detailPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    updateEmptyState();

    connect(start_stop_button_, &QAbstractButton::clicked, this, [this] {
        const auto status = aida::burp::collaborator::status();
        if (status.running)
            openReviewDialog(1);
        else
            submitStart();
    });
    connect(generate_button_, &QAbstractButton::clicked, this, [this] { submitGenerateToken(); });
    connect(copy_domain_button_, &QAbstractButton::clicked, this, [this] {
        if (!last_generated_domain_.empty()) {
            clipboard::set_text(QString::fromStdString(last_generated_domain_));
            diag::log_tagged_fmt("collaborator_v", "copy_domain domain=%s",
                last_generated_domain_.c_str());
        }
    });
    connect(clear_button_, &QAbstractButton::clicked, this, [this] { openReviewDialog(2); });
    connect(retry_button_, &QAbstractButton::clicked, this, [this] {
        static_cast<void>(runner_->retry());
    });
    connect(runner_, &BurpOperationRunner::completed, this,
        [this](quint64, bool success, bool, const QString& message) {
            const auto token = std::atomic_load_explicit(&generated_token_,
                std::memory_order_acquire);
            if (token && !token->first.empty() && token->first != last_generated_token_) {
                last_generated_token_ = token->first;
                last_generated_domain_ = token->second;
                token_label_->setText(QStringLiteral("Last token domain: %1")
                    .arg(QString::fromStdString(last_generated_domain_)));
            }
            if (success && message.contains(QStringLiteral("cleared"))) {
                selected_id_ = -1;
                refreshDetail();
            }
            refreshButtons(aida::burp::collaborator::status());
            requestCacheRefresh();
        });
    connect(runner_, &BurpOperationRunner::submitted, this, [this](quint64) {
        refreshButtons(aida::burp::collaborator::status());
    });

    const auto filterChanged = [this] {
        model_->setFilter(filter_kind_->currentText().toStdString(),
            filter_token_->text().toStdString(), filter_ip_->text().toStdString());
        refreshDetail();
    };
    connect(filter_kind_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        diag::log_tagged_fmt("collaborator_v", "filter_kind_changed kind=%s",
            text.toStdString().c_str());
        model_->setFilter(text.toStdString(), filter_token_->text().toStdString(),
            filter_ip_->text().toStdString());
        refreshDetail();
    });
    connect(filter_token_, &QLineEdit::textChanged, this, [filterChanged] { filterChanged(); });
    connect(filter_ip_, &QLineEdit::textChanged, this, [filterChanged] { filterChanged(); });

    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* interaction = model_->interactionAt(current.isValid() ? current.row() : -1);
            selected_id_ = interaction ? static_cast<std::int64_t>(interaction->id) : -1;
            if (interaction) {
                diag::log_tagged_fmt("collaborator_v", "interaction_selected id=%llu kind=%s client=%s",
                    static_cast<unsigned long long>(interaction->id), interaction->kind.c_str(),
                    interaction->client_ip.c_str());
            }
            refreshDetail();
        });

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(200);
    connect(refresh_timer_, &QTimer::timeout, this, [this] { requestCacheRefresh(); });

    refreshStatusCard(aida::burp::collaborator::status());
    refreshButtons(aida::burp::collaborator::status());
    refreshDetail();
    setContent(content);
}

void CollaboratorView::onPaneShown() {
    requestCacheRefresh();
    refresh_timer_->start();
}

void CollaboratorView::onPaneHidden() {
    refresh_timer_->stop();
}

void CollaboratorView::requestCacheRefresh() {
    bool expected = false;
    if (!cache_refresh_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    QPointer<CollaboratorView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.collaborator";
    submission.label = "collaborator.refresh_interactions";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 4;
    submission.body = [pane]() {
        auto publication = std::make_shared<cache_publication_t>();
        try {
            publication->interactions = aida::burp::collaborator::snapshot_all(4096);
            publication->status = aida::burp::collaborator::status();
        } catch (...) {
            publication.reset();
        }
        if (pane) {
            QMetaObject::invokeMethod(pane.data(),
                [pane, publication = std::move(publication)]() mutable {
                    pane->applyCachePublication(std::move(publication));
                }, Qt::QueuedConnection);
        }
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted)
        cache_refresh_pending_.store(false, std::memory_order_release);
}

void CollaboratorView::applyCachePublication(
    const std::shared_ptr<const cache_publication_t>& publication) {
    cache_refresh_pending_.store(false, std::memory_order_release);
    if (!publication)
        return;
    cache_publication_ = publication;
    const std::uint64_t generation = cache_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const QSignalBlocker blocker(table_->selectionModel());
    model_->setPublication(
        std::shared_ptr<const std::vector<aida::burp::collaborator::interaction_t>>(
            publication, &publication->interactions),
        generation, publication->status.started_ms);
    if (selected_id_ >= 0) {
        for (int row = 0; row < model_->rowCount(); ++row) {
            const auto* interaction = model_->interactionAt(row);
            if (interaction && static_cast<std::int64_t>(interaction->id) == selected_id_) {
                table_->setCurrentIndex(model_->index(row, 0));
                break;
            }
        }
    }
    refreshStatusCard(publication->status);
    refreshButtons(publication->status);
    if (publication->status.running != last_running_) {
        last_running_ = publication->status.running;
        diag::log_tagged_fmt("collaborator_v", "render_state_change running=%d interactions=%zu tokens=%zu",
            publication->status.running ? 1 : 0,
            publication->status.interaction_count,
            publication->status.token_count);
    }
    refreshDetail();
}

void CollaboratorView::refreshStatusCard(const aida::burp::collaborator::status_t& status) {
    status_label_->setText(status.running ? QStringLiteral("RUNNING") : QStringLiteral("STOPPED"));
    set_label_tone(status_label_, status.running ? "titleSuccess" : "title");
    listeners_label_->setText(QStringLiteral("  HTTP %1:%2 %3   DNS %4:%5 %6   SMTP %7:%8 %9")
        .arg(QString::fromStdString(status.bind_ip))
        .arg(static_cast<int>(status.http_port))
        .arg(status.http_alive ? QStringLiteral("OK") : QStringLiteral("--"))
        .arg(QString::fromStdString(status.bind_ip))
        .arg(static_cast<int>(status.dns_port))
        .arg(status.dns_alive ? QStringLiteral("OK") : QStringLiteral("--"))
        .arg(QString::fromStdString(status.bind_ip))
        .arg(static_cast<int>(status.smtp_port))
        .arg(status.smtp_alive ? QStringLiteral("OK") : QStringLiteral("--")));
    public_label_->setText(QStringLiteral("Public: %1 -> %2   Interactions=%3 Tokens=%4")
        .arg(QString::fromStdString(status.public_host))
        .arg(QString::fromStdString(status.public_ip))
        .arg(status.interaction_count)
        .arg(status.token_count));
    config_form_->setVisible(!status.running);
    token_label_->setVisible(status.running);
    if (status.running) {
        if (!last_generated_domain_.empty()) {
            token_label_->setText(QStringLiteral("Last token domain: %1")
                .arg(QString::fromStdString(last_generated_domain_)));
            set_label_tone(token_label_, "accent");
        } else {
            token_label_->setText(QStringLiteral("Click Generate Token to issue a callback domain."));
            set_label_tone(token_label_, "dim");
        }
    }
}

void CollaboratorView::refreshButtons(const aida::burp::collaborator::status_t& status) {
    const bool pending = runner_->pending();
    start_stop_button_->setText(status.running ? QStringLiteral("Stop") : QStringLiteral("Start"));
    start_stop_button_->setKind(status.running ? widgets::AidaButton::Kind::Destructive
                                               : widgets::AidaButton::Kind::Primary);
    start_stop_button_->setEnabled(!pending);
    generate_button_->setEnabled(!pending);
    clear_button_->setEnabled(!pending && status.interaction_count != 0);
    if (pending) {
        op_status_label_->setText(QStringLiteral("Operation running in Task Center"));
        set_label_tone(op_status_label_, "info");
        retry_button_->setVisible(false);
    } else if (const auto completion = runner_->completion()) {
        const auto& result = completion->result;
        op_status_label_->setText(QString::fromStdString(result.message));
        set_label_tone(op_status_label_, result.success ? "success" : "error");
        retry_button_->setVisible(!result.success);
    } else {
        op_status_label_->clear();
        retry_button_->setVisible(false);
    }
}

void CollaboratorView::updateEmptyState() {
    if (!table_stack_ || !empty_view_ || !table_ || !model_)
        return;
    table_stack_->setCurrentWidget(model_->rowCount() == 0
        ? static_cast<QWidget*>(empty_view_) : static_cast<QWidget*>(table_));
}

void CollaboratorView::refreshDetail() {
    const auto* interaction = selected_id_ >= 0
        ? model_->findById(static_cast<std::uint64_t>(selected_id_)) : nullptr;
    if (!interaction) {
        detail_header_->setText(QStringLiteral("No interaction selected"));
        detail_model_->setInteraction(nullptr);
        detail_raw_->clear();
        return;
    }
    detail_header_->setText(QStringLiteral("id=%1 kind=%2 client=%3:%4 subdomain=%5 token=%6")
        .arg(static_cast<unsigned long long>(interaction->id))
        .arg(QString::fromStdString(interaction->kind))
        .arg(QString::fromStdString(interaction->client_ip))
        .arg(static_cast<unsigned>(interaction->client_port))
        .arg(QString::fromStdString(interaction->subdomain))
        .arg(interaction->payload_token.empty() ? QStringLiteral("-")
            : QString::fromStdString(interaction->payload_token)));
    detail_model_->setInteraction(interaction);
    detail_raw_->setPlainText(QString::fromStdString(interaction->raw));
}

void CollaboratorView::submitStart() {
    aida::burp::collaborator::collaborator_config_t config;
    config.bind_ip = bind_ip_->text().toStdString();
    config.http_port = static_cast<std::uint16_t>(http_port_->value());
    config.dns_port = static_cast<std::uint16_t>(dns_port_->value());
    config.smtp_port = static_cast<std::uint16_t>(smtp_port_->value());
    config.enable_http = enable_http_->isChecked();
    config.enable_dns = enable_dns_->isChecked();
    config.enable_smtp = enable_smtp_->isChecked();
    config.public_host = public_host_->text().toStdString();
    config.public_ip = public_ip_->text().toStdString();
    config.canned_body = canned_body_->text().toStdString();
    const std::string ct = canned_ct_->text().toStdString();
    config.canned_content_type = ct.empty() ? "text/plain" : ct;

    BurpRequest request;
    request.owner = QStringLiteral("burp.collaborator");
    request.ownerView = QStringLiteral("view.network.collaborator");
    request.ownerAction = QStringLiteral("network.collaborator.start");
    request.label = QStringLiteral("Start Collaborator");
    request.target = QString::fromStdString(config.bind_ip + ":" + std::to_string(config.http_port));
    request.affectedEntity = QStringLiteral("Collaborator listeners");
    request.execute = [config = std::move(config)]() {
        aida::burp::ui_operation::result_t result;
        if (aida::burp::collaborator::status().running) {
            result.message = "Collaborator started before this request executed.";
            return result;
        }
        result.success = aida::burp::collaborator::start(config);
        result.message = result.success ? "Collaborator listeners started."
                                        : aida::burp::collaborator::last_error();
        return result;
    };
    static_cast<void>(runner_->submit(std::move(request)));
}

void CollaboratorView::submitReviewedOperation(int operation,
    aida::burp::collaborator::status_t reviewed) {
    BurpRequest request;
    request.owner = QStringLiteral("burp.collaborator");
    request.ownerView = QStringLiteral("view.network.collaborator");
    request.ownerAction = operation == 1 ? QStringLiteral("network.collaborator.stop")
                                         : QStringLiteral("network.collaborator.clear");
    request.label = operation == 1 ? QStringLiteral("Stop Collaborator")
                                   : QStringLiteral("Clear Collaborator interactions");
    request.target = QString::fromStdString(reviewed.public_host);
    request.affectedEntity = operation == 1
        ? QStringLiteral("Collaborator listeners")
        : QString::fromStdString(std::to_string(reviewed.interaction_count) + " interactions");
    request.execute = [operation, reviewed = std::move(reviewed)]() {
        aida::burp::ui_operation::result_t result;
        const auto current = aida::burp::collaborator::status();
        if (!same_runtime(current, reviewed)) {
            result.message = "Collaborator state changed after review; no operation was applied.";
            return result;
        }
        if (operation == 1) {
            aida::burp::collaborator::stop();
            result.success = !aida::burp::collaborator::status().running;
            result.message = result.success ? "Collaborator listeners stopped."
                                            : "Collaborator did not reach the stopped state.";
        } else {
            aida::burp::collaborator::clear();
            result.success = aida::burp::collaborator::status().interaction_count == 0;
            result.message = result.success ? "Collaborator interactions cleared."
                                            : "Collaborator interactions were not cleared.";
        }
        return result;
    };
    static_cast<void>(runner_->submit(std::move(request)));
}

void CollaboratorView::submitGenerateToken() {
    BurpRequest request;
    request.owner = QStringLiteral("burp.collaborator");
    request.ownerView = QStringLiteral("view.network.collaborator");
    request.ownerAction = QStringLiteral("network.collaborator.generate_token");
    request.label = QStringLiteral("Generate Collaborator token");
    request.target = QStringLiteral("Collaborator token store");
    request.affectedEntity = QStringLiteral("Persisted Collaborator tokens");
    QPointer<CollaboratorView> pane(this);
    request.execute = [pane]() {
        aida::burp::ui_operation::result_t result;
        const std::string token = aida::burp::collaborator::generate_token();
        const auto config = aida::burp::collaborator::current_config();
        result.success = !token.empty();
        result.message = result.success ? "Collaborator token generated and persisted."
                                        : aida::burp::collaborator::last_error();
        if (result.success && pane) {
            std::shared_ptr<const std::pair<std::string, std::string>> publication =
                std::make_shared<const std::pair<std::string, std::string>>(
                    token, token + "." + config.public_host);
            std::atomic_store_explicit(&pane->generated_token_,
                std::move(publication), std::memory_order_release);
        }
        return result;
    };
    static_cast<void>(runner_->submit(std::move(request)));
}

void CollaboratorView::openReviewDialog(int operation) {
    const auto reviewed = cache_publication_ ? cache_publication_->status
                                             : aida::burp::collaborator::status();
    const bool stop = operation == 1;
    auto* dialog = new BurpReviewDialog(
        QStringLiteral("Review Collaborator operation"),
        { stop ? QStringLiteral("Stop all Collaborator listeners?")
               : QStringLiteral("Permanently clear Collaborator interactions?"),
          QStringLiteral("Target: %1").arg(QString::fromStdString(reviewed.public_host)),
          QStringLiteral("Interactions: %1").arg(reviewed.interaction_count),
          QStringLiteral("The exact reviewed listener and interaction state will be revalidated before the operation runs.") },
        stop ? QStringLiteral("Stop listeners") : QStringLiteral("Clear interactions"),
        true, this);
    dialog->setRunner(runner_);
    dialog->setRevalidator([reviewed](QString& reasonOut) {
        if (!same_runtime(aida::burp::collaborator::status(), reviewed)) {
            reasonOut = QStringLiteral(
                "Collaborator state changed after review; cancel and select again.");
            return false;
        }
        return true;
    });
    dialog->setSubmitCallback([this, operation, reviewed] {
        submitReviewedOperation(operation, reviewed);
    });
    dialog->open();
}

}
