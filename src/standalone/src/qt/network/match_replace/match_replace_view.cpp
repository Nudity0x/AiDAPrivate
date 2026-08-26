#include "qt/network/match_replace/match_replace_view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QPointer>
#include <QSplitter>
#include <QStackedLayout>
#include <QTableView>
#include <QVBoxLayout>
#include <QItemSelectionModel>

#include <algorithm>
#include <utility>

#include "core/infra/executor.hpp"
#include "core/network/burp/match_replace_view.hpp"
#include "helpers/diag_log.hpp"
#include "qt/network/bounded_plain_text_edit.hpp"
#include "qt/network/burp_operation.hpp"
#include "qt/network/burp_review_dialog.hpp"
#include "qt/network/reviewed_context_banner.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

const char* k_target_labels[] = {
    "request_url", "request_headers", "request_body",
    "response_headers", "response_body", "all"
};

const char* k_scheme_labels[] = { "(any)", "http", "https" };

bool same_rule_definition(const aida::burp::match_replace::rule_t& left,
    const aida::burp::match_replace::rule_t& right) {
    return left.id == right.id && left.label == right.label && left.target == right.target &&
        left.match_regex == right.match_regex && left.replacement == right.replacement &&
        left.regex == right.regex && left.case_insensitive == right.case_insensitive &&
        left.active == right.active && left.host_filter == right.host_filter &&
        left.scheme_filter == right.scheme_filter;
}

}

MatchReplaceRulesModel::MatchReplaceRulesModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int MatchReplaceRulesModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rules_->size());
}

int MatchReplaceRulesModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant MatchReplaceRulesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* row = rowAt(index.row());
    if (!row)
        return {};
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Label: {
            std::string label = row->label;
            if (label.empty())
                label = "(rule #" + std::to_string(row->id) + ")";
            if (label.size() > 30)
                label = label.substr(0, 30) + "...";
            return QString::fromStdString(label);
        }
        case Target: return QString::fromLatin1(
            aida::burp::match_replace::target_label(row->target));
        case Match: {
            std::string preview = row->match_regex;
            if (preview.size() > 32)
                preview = preview.substr(0, 32) + "...";
            return QString::fromStdString(preview);
        }
        case Hits: return QString::number(static_cast<unsigned long long>(row->hit_count));
        case Active: return row->active ? QStringLiteral("yes") : QStringLiteral("no");
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        const auto& t = theme::tokens();
        switch (index.column()) {
        case Match: return t.text_secondary;
        case Hits: return t.text_dim;
        case Active: return row->active ? t.success : t.text_dim;
        default: return t.text_primary;
        }
    }
    return {};
}

QVariant MatchReplaceRulesModel::headerData(int section, Qt::Orientation orientation,
                                            int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Label: return QStringLiteral("Label");
    case Target: return QStringLiteral("Target");
    case Match: return QStringLiteral("Match");
    case Hits: return QStringLiteral("Hits");
    case Active: return QStringLiteral("Active");
    default: return {};
    }
}

void MatchReplaceRulesModel::multiData(const QModelIndex& index,
                                       QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

void MatchReplaceRulesModel::adopt(
    std::shared_ptr<const std::vector<aida::burp::match_replace::rule_t>> rules) {
    beginResetModel();
    rules_ = std::move(rules);
    endResetModel();
}

const aida::burp::match_replace::rule_t* MatchReplaceRulesModel::rowAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(rules_->size()))
        return nullptr;
    return &(*rules_)[static_cast<std::size_t>(row)];
}

const aida::burp::match_replace::rule_t* MatchReplaceRulesModel::findById(
    std::uint64_t id) const noexcept {
    for (const auto& rule : *rules_) {
        if (rule.id == id)
            return &rule;
    }
    return nullptr;
}

std::shared_ptr<const std::vector<aida::burp::match_replace::rule_t>>
MatchReplaceRulesModel::rules() const {
    return rules_;
}

MatchReplaceView::MatchReplaceView(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.match_replace"));
    const auto& t = theme::tokens();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(t.spacing.sm);
    auto* title = new QLabel(QStringLiteral("Match and Replace"), content);
    title->setProperty("aidaTone", QStringLiteral("titleAccent"));
    headerRow->addWidget(title);
    op_status_label_ = new QLabel(content);
    headerRow->addWidget(op_status_label_, 1);
    retry_init_button_ = new widgets::AidaButton(QStringLiteral("Retry initialization"), content);
    retry_init_button_->setKind(widgets::AidaButton::Kind::Secondary);
    retry_init_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    retry_init_button_->setVisible(false);
    headerRow->addWidget(retry_init_button_);
    layout->addLayout(headerRow);

    banner_ = new ReviewedContextBanner(content);
    banner_->setStaleText(QStringLiteral("STALE RULE DRAFT SOURCE"));
    banner_->setIdentitySuffix(QStringLiteral(" | disabled until reviewed"));
    layout->addWidget(banner_);

    auto* toolbarRow = new QHBoxLayout();
    toolbarRow->setSpacing(t.spacing.sm);
    new_target_ = new QComboBox(content);
    for (const char* label : k_target_labels)
        new_target_->addItem(QString::fromLatin1(label));
    toolbarRow->addWidget(new_target_);
    new_scheme_ = new QComboBox(content);
    for (const char* label : k_scheme_labels)
        new_scheme_->addItem(QString::fromLatin1(label));
    toolbarRow->addWidget(new_scheme_);
    new_regex_ = new QCheckBox(QStringLiteral("regex"), content);
    new_regex_->setChecked(true);
    toolbarRow->addWidget(new_regex_);
    new_ci_ = new QCheckBox(QStringLiteral("ci"), content);
    toolbarRow->addWidget(new_ci_);
    new_active_ = new QCheckBox(QStringLiteral("active"), content);
    new_active_->setChecked(true);
    toolbarRow->addWidget(new_active_);
    add_button_ = new widgets::AidaButton(QStringLiteral("Add rule"), content);
    add_button_->setKind(widgets::AidaButton::Kind::Primary);
    add_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    toolbarRow->addWidget(add_button_);
    toolbarRow->addStretch(1);
    layout->addLayout(toolbarRow);

    new_label_ = new QLineEdit(content);
    new_label_->setMaxLength(127);
    new_label_->setPlaceholderText(QStringLiteral("Optional label"));
    layout->addWidget(new_label_);
    auto* matchRow = new QHBoxLayout();
    matchRow->setSpacing(t.spacing.sm);
    new_match_ = new QLineEdit(content);
    new_match_->setMaxLength(2047);
    new_match_->setPlaceholderText(QStringLiteral("regex or literal"));
    matchRow->addWidget(new_match_, 1);
    new_replace_ = new QLineEdit(content);
    new_replace_->setMaxLength(2047);
    new_replace_->setPlaceholderText(QStringLiteral("replacement"));
    matchRow->addWidget(new_replace_, 1);
    layout->addLayout(matchRow);
    new_host_filter_ = new QLineEdit(content);
    new_host_filter_->setMaxLength(255);
    new_host_filter_->setPlaceholderText(QStringLiteral("(optional regex)"));
    layout->addWidget(new_host_filter_);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);
    model_ = new MatchReplaceRulesModel(splitter);
    auto* tableHost = new QWidget(splitter);
    table_stack_ = new QStackedLayout(tableHost);
    table_stack_->setStackingMode(QStackedLayout::StackOne);
    table_stack_->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableView(tableHost);
    table_->setObjectName(QStringLiteral("aida.view.network.match_replace.table"));
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
        QStringLiteral("No match/replace rules"),
        QStringLiteral("Add a rule above; it applies to proxy traffic through the reviewed pipeline."),
        tableHost);
    empty_view_->setObjectName(QStringLiteral("aida.view.network.match_replace.empty"));
    table_stack_->addWidget(empty_view_);
    connect(model_, &QAbstractItemModel::modelReset, this, [this] { updateEmptyState(); });
    splitter->addWidget(tableHost);

    auto* rightPanel = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(t.spacing.sm);
    auto* editFormRow1 = new QHBoxLayout();
    editFormRow1->setSpacing(t.spacing.sm);
    edit_label_ = new QLineEdit(rightPanel);
    edit_label_->setMaxLength(127);
    edit_label_->setPlaceholderText(QStringLiteral("Label"));
    editFormRow1->addWidget(edit_label_, 1);
    edit_target_ = new QComboBox(rightPanel);
    for (const char* label : k_target_labels)
        edit_target_->addItem(QString::fromLatin1(label));
    editFormRow1->addWidget(edit_target_);
    edit_scheme_ = new QComboBox(rightPanel);
    for (const char* label : k_scheme_labels)
        edit_scheme_->addItem(QString::fromLatin1(label));
    editFormRow1->addWidget(edit_scheme_);
    rightLayout->addLayout(editFormRow1);
    auto* editFlagsRow = new QHBoxLayout();
    editFlagsRow->setSpacing(t.spacing.sm);
    edit_regex_ = new QCheckBox(QStringLiteral("regex"), rightPanel);
    edit_regex_->setChecked(true);
    editFlagsRow->addWidget(edit_regex_);
    edit_ci_ = new QCheckBox(QStringLiteral("ci"), rightPanel);
    editFlagsRow->addWidget(edit_ci_);
    edit_active_ = new QCheckBox(QStringLiteral("active"), rightPanel);
    edit_active_->setChecked(true);
    editFlagsRow->addWidget(edit_active_);
    editFlagsRow->addStretch(1);
    rightLayout->addLayout(editFlagsRow);
    edit_match_ = new QLineEdit(rightPanel);
    edit_match_->setMaxLength(2047);
    edit_match_->setPlaceholderText(QStringLiteral("Match"));
    rightLayout->addWidget(edit_match_);
    edit_replace_ = new QLineEdit(rightPanel);
    edit_replace_->setMaxLength(2047);
    edit_replace_->setPlaceholderText(QStringLiteral("Replace"));
    rightLayout->addWidget(edit_replace_);
    edit_host_filter_ = new QLineEdit(rightPanel);
    edit_host_filter_->setMaxLength(255);
    edit_host_filter_->setPlaceholderText(QStringLiteral("Host filter"));
    rightLayout->addWidget(edit_host_filter_);
    auto* editButtons = new QHBoxLayout();
    editButtons->setSpacing(t.spacing.sm);
    save_button_ = new widgets::AidaButton(QStringLiteral("Save"), rightPanel);
    save_button_->setKind(widgets::AidaButton::Kind::Primary);
    save_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    editButtons->addWidget(save_button_);
    delete_button_ = new widgets::AidaButton(QStringLiteral("Delete"), rightPanel);
    delete_button_->setKind(widgets::AidaButton::Kind::Destructive);
    delete_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    editButtons->addWidget(delete_button_);
    up_button_ = new widgets::AidaButton(QStringLiteral("Up"), rightPanel);
    up_button_->setKind(widgets::AidaButton::Kind::Ghost);
    up_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    editButtons->addWidget(up_button_);
    down_button_ = new widgets::AidaButton(QStringLiteral("Down"), rightPanel);
    down_button_->setKind(widgets::AidaButton::Kind::Ghost);
    down_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    editButtons->addWidget(down_button_);
    clear_all_button_ = new widgets::AidaButton(QStringLiteral("Clear all"), rightPanel);
    clear_all_button_->setKind(widgets::AidaButton::Kind::Ghost);
    clear_all_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    editButtons->addWidget(clear_all_button_);
    editButtons->addStretch(1);
    rightLayout->addLayout(editButtons);

    auto* testTitle = new QLabel(QStringLiteral("Test against sample"), rightPanel);
    testTitle->setProperty("aidaTone", QStringLiteral("secondary"));
    rightLayout->addWidget(testTitle);
    test_sample_ = new BoundedPlainTextEdit(4095, rightPanel);
    test_sample_->setFont(theme::fonts::codeRegular());
    test_sample_->setMinimumHeight(editor_min_height_lines(test_sample_, 4));
    rightLayout->addWidget(test_sample_);
    run_test_button_ = new widgets::AidaButton(QStringLiteral("Run test"), rightPanel);
    run_test_button_->setKind(widgets::AidaButton::Kind::Secondary);
    run_test_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    rightLayout->addWidget(run_test_button_, 0, Qt::AlignLeft);
    test_result_ = new QPlainTextEdit(rightPanel);
    test_result_->setReadOnly(true);
    test_result_->setFont(theme::fonts::codeRegular());
    test_result_->setMinimumHeight(editor_min_height_lines(test_result_, 4));
    test_result_->setPlaceholderText(QStringLiteral("Run a test to see the rewritten sample"));
    rightLayout->addWidget(test_result_, 1);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 11);
    splitter->setStretchFactor(1, 9);
    layout->addWidget(splitter, 1);

    runner_ = new BurpOperationRunner(QStringLiteral("burp_ui"), this);

    connect(retry_init_button_, &QAbstractButton::clicked, this, [this] { submitInitialize(); });
    connect(add_button_, &QAbstractButton::clicked, this, [this] { addRule(); });
    connect(save_button_, &QAbstractButton::clicked, this, [this] { saveRule(); });
    connect(delete_button_, &QAbstractButton::clicked, this, [this] { openReview(3); });
    connect(up_button_, &QAbstractButton::clicked, this, [this] { moveSelected(-1); });
    connect(down_button_, &QAbstractButton::clicked, this, [this] { moveSelected(1); });
    connect(clear_all_button_, &QAbstractButton::clicked, this, [this] { openReview(5); });
    connect(run_test_button_, &QAbstractButton::clicked, this, [this] { runTest(); });

    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* rule = model_->rowAt(current.isValid() ? current.row() : -1);
            if (rule)
                loadIntoEditor(*rule);
            refreshEditorButtons();
        });

    connect(runner_, &BurpOperationRunner::completed, this,
        [this](quint64, bool success, bool, const QString&) {
            if (initialization_requested_) {
                initialized_ = success;
                initialization_requested_ = false;
            }
            const auto completion = runner_->completion();
            if (completion) {
                op_status_label_->setText(QString::fromStdString(completion->result.message));
                set_label_tone(op_status_label_,
                    completion->result.success ? "success" : "error");
            }
            retry_init_button_->setVisible(!initialized_ && !runner_->pending() &&
                completion && !completion->result.success);
            if (success)
                requestRulesRefresh();
            refreshEditorButtons();
        });
    connect(runner_, &BurpOperationRunner::submitted, this, [this](quint64) {
        op_status_label_->setText(QStringLiteral("Operation running in Task Center"));
        set_label_tone(op_status_label_, "info");
        refreshEditorButtons();
    });

    aida::burp::match_replace_view::set_reviewed_context_staged_hook(
        [pane = QPointer<MatchReplaceView>(this)] {
            if (!pane)
                return;
            QMetaObject::invokeMethod(pane.data(), [pane] { pane->drainStaged(); },
                Qt::QueuedConnection);
        });
    hooks_installed_ = true;
    drainStaged();

    refreshEditorButtons();
    updateEmptyState();
    setContent(content);
}

void MatchReplaceView::updateEmptyState() {
    if (!table_stack_ || !empty_view_ || !table_ || !model_)
        return;
    table_stack_->setCurrentWidget(model_->rowCount() == 0
        ? static_cast<QWidget*>(empty_view_) : static_cast<QWidget*>(table_));
}

MatchReplaceView::~MatchReplaceView() {
    if (hooks_installed_)
        aida::burp::match_replace_view::set_reviewed_context_staged_hook(nullptr);
}

void MatchReplaceView::onPaneShown() {
    if (!initialized_ && !initialization_requested_ && !runner_->pending())
        submitInitialize();
    requestRulesRefresh();
}

void MatchReplaceView::submitInitialize() {
    initialization_requested_ = true;
    BurpRequest request;
    request.owner = QStringLiteral("burp.match_replace");
    request.ownerView = QStringLiteral("view.network.match_replace");
    request.ownerAction = QStringLiteral("network.match_replace.initialize");
    request.label = QStringLiteral("Load Match and Replace rules");
    request.target = QStringLiteral("Match and Replace catalog");
    request.affectedEntity = QStringLiteral("Persisted rule catalog");
    request.execute = []() {
        aida::burp::ui_operation::result_t result;
        result.success = aida::burp::match_replace::initialize();
        result.message = result.success ? "Match and Replace rules loaded."
                                        : aida::burp::match_replace::last_error();
        return result;
    };
    if (!runner_->submit(std::move(request)))
        initialization_requested_ = false;
}

void MatchReplaceView::submitRuleChange(int operation,
    aida::burp::match_replace::rule_t rule,
    std::vector<aida::burp::match_replace::rule_t> reviewed, int delta) {
    BurpRequest request;
    request.owner = QStringLiteral("burp.match_replace");
    request.ownerView = QStringLiteral("view.network.match_replace");
    request.ownerAction = QStringLiteral("network.match_replace.mutate");
    request.label = operation == 1 ? QStringLiteral("Add Match and Replace rule") :
        operation == 2 ? QStringLiteral("Update Match and Replace rule") :
        operation == 3 ? QStringLiteral("Delete Match and Replace rule") :
        operation == 4 ? QStringLiteral("Move Match and Replace rule")
                       : QStringLiteral("Clear Match and Replace rules");
    request.target = operation == 5
        ? QString::fromStdString(std::to_string(reviewed.size()) + " rules")
        : QStringLiteral("Rule %1").arg(static_cast<unsigned long long>(rule.id));
    request.affectedEntity = request.target;
    request.execute = [operation, rule = std::move(rule), reviewed = std::move(reviewed),
                       delta]() mutable {
        aida::burp::ui_operation::result_t result;
        auto current = aida::burp::match_replace::list();
        if (operation != 1) {
            if (operation == 5) {
                if (current.size() != reviewed.size() ||
                    !std::equal(current.begin(), current.end(), reviewed.begin(),
                        [](const auto& left, const auto& right) {
                            return same_rule_definition(left, right);
                        })) {
                    result.message = "The rule catalog changed after review; no rules were cleared.";
                    return result;
                }
            } else {
                const auto found = std::find_if(current.begin(), current.end(),
                    [&](const auto& item) { return item.id == rule.id; });
                const auto reviewed_found = std::find_if(reviewed.begin(), reviewed.end(),
                    [&](const auto& item) { return item.id == rule.id; });
                if (found == current.end() || reviewed_found == reviewed.end() ||
                    !same_rule_definition(*found, *reviewed_found)) {
                    result.message = "The selected rule changed after review; no mutation was applied.";
                    return result;
                }
                rule.hit_count = found->hit_count;
            }
        }
        if (operation == 1) result.success = aida::burp::match_replace::add(std::move(rule)) != 0;
        else if (operation == 2) result.success = aida::burp::match_replace::update(rule);
        else if (operation == 3) result.success = aida::burp::match_replace::remove(rule.id);
        else if (operation == 4) result.success = aida::burp::match_replace::move(rule.id, delta);
        else {
            aida::burp::match_replace::clear();
            result.success = aida::burp::match_replace::list().empty();
        }
        result.message = result.success ? "Match and Replace catalog updated."
                                        : aida::burp::match_replace::last_error();
        return result;
    };
    static_cast<void>(runner_->submit(std::move(request)));
}

void MatchReplaceView::requestRulesRefresh() {
    bool expected = false;
    if (!refresh_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    QPointer<MatchReplaceView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.match_replace";
    submission.label = "match_replace.refresh_rules";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 4;
    submission.body = [pane]() {
        std::shared_ptr<const std::vector<aida::burp::match_replace::rule_t>> publication =
            std::make_shared<const std::vector<aida::burp::match_replace::rule_t>>(
                aida::burp::match_replace::list());
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(),
            [pane, publication = std::move(publication)]() mutable {
                const QSignalBlocker blocker(pane->table_->selectionModel());
                pane->model_->adopt(std::move(publication));
                if (pane->selected_id_ != 0 &&
                    !pane->model_->findById(pane->selected_id_))
                    pane->selected_id_ = 0;
                if (pane->selected_id_ != 0) {
                    for (int row = 0; row < pane->model_->rowCount(); ++row) {
                        const auto* rule = pane->model_->rowAt(row);
                        if (rule && rule->id == pane->selected_id_) {
                            pane->table_->setCurrentIndex(pane->model_->index(row, 0));
                            break;
                        }
                    }
                }
                pane->refresh_pending_.store(false, std::memory_order_release);
                pane->refreshEditorButtons();
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted)
        refresh_pending_.store(false, std::memory_order_release);
}

void MatchReplaceView::loadIntoEditor(const aida::burp::match_replace::rule_t& rule) {
    selected_id_ = rule.id;
    edit_label_->setText(QString::fromStdString(rule.label));
    edit_match_->setText(QString::fromStdString(rule.match_regex));
    edit_replace_->setText(QString::fromStdString(rule.replacement));
    edit_host_filter_->setText(QString::fromStdString(rule.host_filter));
    edit_target_->setCurrentIndex(static_cast<int>(rule.target));
    edit_scheme_->setCurrentIndex(rule.scheme_filter == "http" ? 1
        : rule.scheme_filter == "https" ? 2 : 0);
    edit_regex_->setChecked(rule.regex);
    edit_ci_->setChecked(rule.case_insensitive);
    edit_active_->setChecked(rule.active);
}

void MatchReplaceView::addRule() {
    aida::burp::match_replace::rule_t rule;
    rule.label = new_label_->text().toStdString();
    rule.target = static_cast<aida::burp::match_replace::match_kind_t>(new_target_->currentIndex());
    rule.match_regex = new_match_->text().toStdString();
    rule.replacement = new_replace_->text().toStdString();
    rule.regex = new_regex_->isChecked();
    rule.case_insensitive = new_ci_->isChecked();
    rule.active = new_active_->isChecked();
    rule.host_filter = new_host_filter_->text().toStdString();
    if (new_scheme_->currentIndex() == 1) rule.scheme_filter = "http";
    else if (new_scheme_->currentIndex() == 2) rule.scheme_filter = "https";
    submitRuleChange(1, std::move(rule), {});
}

void MatchReplaceView::saveRule() {
    const auto rules = model_->rules();
    aida::burp::match_replace::rule_t rule;
    rule.id = selected_id_;
    rule.label = edit_label_->text().toStdString();
    rule.target = static_cast<aida::burp::match_replace::match_kind_t>(edit_target_->currentIndex());
    rule.match_regex = edit_match_->text().toStdString();
    rule.replacement = edit_replace_->text().toStdString();
    rule.regex = edit_regex_->isChecked();
    rule.case_insensitive = edit_ci_->isChecked();
    rule.active = edit_active_->isChecked();
    rule.host_filter = edit_host_filter_->text().toStdString();
    if (edit_scheme_->currentIndex() == 1) rule.scheme_filter = "http";
    else if (edit_scheme_->currentIndex() == 2) rule.scheme_filter = "https";
    for (const auto& current : *rules) {
        if (current.id == selected_id_) {
            rule.hit_count = current.hit_count;
            break;
        }
    }
    submitRuleChange(2, std::move(rule), *rules);
}

void MatchReplaceView::openReview(int operation) {
    const auto rules = model_->rules();
    aida::burp::match_replace::rule_t reviewedRule{};
    std::vector<aida::burp::match_replace::rule_t> reviewedRules = *rules;
    if (operation == 3) {
        const auto* found = model_->findById(selected_id_);
        if (!found)
            return;
        reviewedRule = *found;
    }
    const bool clearAll = operation == 5;
    auto* dialog = new BurpReviewDialog(
        QStringLiteral("Review Match and Replace mutation"),
        { clearAll ? QStringLiteral("Permanently clear all Match and Replace rules?")
                   : QStringLiteral("Permanently delete the selected rule?"),
          QStringLiteral("Affected rules: %1").arg(clearAll ? reviewedRules.size() : 1U),
          QStringLiteral("The exact reviewed catalog will be revalidated before persistence.") },
        clearAll ? QStringLiteral("Clear all rules") : QStringLiteral("Delete rule"),
        true, this);
    dialog->setRunner(runner_);
    dialog->setRevalidator([operation, reviewedRule, reviewedRules](QString& reasonOut) {
        if (operation == 5 && reviewedRules.empty())
            return false;
        if (operation == 3 && reviewedRule.id == 0)
            return false;
        auto current = aida::burp::match_replace::list();
        if (operation == 5) {
            if (current.size() != reviewedRules.size() ||
                !std::equal(current.begin(), current.end(), reviewedRules.begin(),
                    [](const auto& left, const auto& right) {
                        return same_rule_definition(left, right);
                    })) {
                reasonOut = QStringLiteral(
                    "The rule catalog changed after review; cancel and select again.");
                return false;
            }
            return true;
        }
        const auto found = std::find_if(current.begin(), current.end(),
            [&](const auto& item) { return item.id == reviewedRule.id; });
        if (found == current.end() || !same_rule_definition(*found, reviewedRule)) {
            reasonOut = QStringLiteral(
                "The selected rule changed after review; cancel and select again.");
            return false;
        }
        return true;
    });
    dialog->setSubmitCallback([this, operation, reviewedRule, reviewedRules]() {
        submitRuleChange(operation, reviewedRule, reviewedRules);
    });
    dialog->open();
}

void MatchReplaceView::moveSelected(int delta) {
    const auto* found = model_->findById(selected_id_);
    if (!found)
        return;
    submitRuleChange(4, *found, *model_->rules(), delta);
}

void MatchReplaceView::runTest() {
    aida::burp::match_replace::rule_t rule;
    rule.id = selected_id_;
    rule.label = edit_label_->text().toStdString();
    rule.target = static_cast<aida::burp::match_replace::match_kind_t>(edit_target_->currentIndex());
    rule.match_regex = edit_match_->text().toStdString();
    rule.replacement = edit_replace_->text().toStdString();
    rule.regex = edit_regex_->isChecked();
    rule.case_insensitive = edit_ci_->isChecked();
    rule.active = true;
    std::string result;
    const bool ok = aida::burp::match_replace::test_rule(rule,
        test_sample_->toPlainText().toStdString(), result);
    if (!ok)
        result = "(error: " + aida::burp::match_replace::last_error() + ")";
    test_result_->setPlainText(QString::fromStdString(result));
}

void MatchReplaceView::refreshEditorButtons() {
    const bool pending = runner_->pending();
    const bool enabled = initialized_ && !pending;
    add_button_->setEnabled(enabled);
    const bool hasSelection = selected_id_ != 0;
    save_button_->setEnabled(enabled && hasSelection);
    delete_button_->setEnabled(enabled && hasSelection);
    up_button_->setEnabled(enabled && hasSelection);
    down_button_->setEnabled(enabled && hasSelection);
    clear_all_button_->setEnabled(enabled);
}

void MatchReplaceView::drainStaged() {
    aida::burp::match_replace_view::staged_reviewed_context_t staged;
    if (!aida::burp::match_replace_view::take_staged_reviewed_context(staged))
        return;
    stageReviewedContext(staged.identity, staged.response_target);
}

void MatchReplaceView::stageReviewedContext(
    const network_view::artifact_identity_t& identity, bool responseTarget) {
    banner_->setContext(identity);
    new_host_filter_->setText(QString::fromStdString(identity.target_host).left(255));
    new_scheme_->setCurrentIndex(identity.use_tls ? 2 : 1);
    new_target_->setCurrentIndex(responseTarget ? 3 : 1);
    new_active_->setChecked(false);
    refreshEditorButtons();
}

}
