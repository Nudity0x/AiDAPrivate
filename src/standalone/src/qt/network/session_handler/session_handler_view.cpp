#include "qt/network/session_handler/session_handler_view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
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
#include <QItemSelectionModel>
#include <QRegularExpression>
#include <QRegularExpressionValidator>

#include <algorithm>
#include <utility>

#include "core/infra/executor.hpp"
#include "core/network/burp/session_handler_view.hpp"
#include "helpers/diag_log.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/reviewed_context_banner.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

MacroListModel::MacroListModel(QObject* parent) : QAbstractTableModel(parent) {}

int MacroListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int MacroListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant MacroListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* row = rowAt(index.row());
    if (!row)
        return {};
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Name: return QString::fromStdString(row->name.empty() ? "(unnamed)" : row->name);
        case Id: return QString::number(static_cast<unsigned long long>(row->id));
        case Steps: return QString::number(static_cast<qulonglong>(row->steps.size()));
        case LastRun: return QString::number(static_cast<unsigned long long>(row->last_run_ms));
        case Ok: return row->ok_last_run ? QStringLiteral("yes") : QStringLiteral("no");
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        const auto& t = theme::tokens();
        if (index.column() == Ok)
            return row->ok_last_run ? t.success : t.text_dim;
        return t.text_primary;
    }
    return {};
}

QVariant MacroListModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Name: return QStringLiteral("Name");
    case Id: return QStringLiteral("ID");
    case Steps: return QStringLiteral("Steps");
    case LastRun: return QStringLiteral("Last run ms");
    case Ok: return QStringLiteral("OK");
    default: return {};
    }
}

void MacroListModel::multiData(const QModelIndex& index,
                               QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

void MacroListModel::adopt(std::vector<aida::burp::session_handler::macro_t> macros) {
    beginResetModel();
    rows_ = std::move(macros);
    endResetModel();
}

const aida::burp::session_handler::macro_t* MacroListModel::rowAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

const aida::burp::session_handler::macro_t* MacroListModel::findById(
    std::uint64_t id) const noexcept {
    for (const auto& row : rows_) {
        if (row.id == id)
            return &row;
    }
    return nullptr;
}

SessionRuleListModel::SessionRuleListModel(QObject* parent) : QAbstractTableModel(parent) {}

int SessionRuleListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int SessionRuleListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant SessionRuleListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* row = rowAt(index.row());
    if (!row)
        return {};
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Name: return QString::fromStdString(row->name);
        case Match: return QString::fromLatin1(
            aida::burp::session_handler::match_label(row->match));
        case Macro: return QStringLiteral("macro #%1")
            .arg(static_cast<unsigned long long>(row->macro_id));
        case Active: return row->active ? QStringLiteral("on") : QStringLiteral("off");
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        const auto& t = theme::tokens();
        if (index.column() == Active)
            return row->active ? t.success : t.text_dim;
        return t.text_primary;
    }
    return {};
}

QVariant SessionRuleListModel::headerData(int section, Qt::Orientation orientation,
                                          int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Name: return QStringLiteral("Name");
    case Match: return QStringLiteral("Match");
    case Macro: return QStringLiteral("Macro");
    case Active: return QStringLiteral("Active");
    default: return {};
    }
}

void SessionRuleListModel::multiData(const QModelIndex& index,
                                     QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

void SessionRuleListModel::adopt(
    std::vector<aida::burp::session_handler::session_rule_t> rules) {
    beginResetModel();
    rows_ = std::move(rules);
    endResetModel();
}

const aida::burp::session_handler::session_rule_t*
SessionRuleListModel::rowAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

MacroStepsModel::MacroStepsModel(QObject* parent) : QAbstractTableModel(parent) {}

int MacroStepsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(steps_.size());
}

int MacroStepsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant MacroStepsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(steps_.size()))
        return {};
    const auto& step = steps_[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Index: return QString::number(index.row() + 1);
        case Label: return QString::fromStdString(step.label.empty() ? "(unlabeled)" : step.label);
        case Target: return QStringLiteral("%1://%2:%3")
            .arg(QString::fromStdString(step.scheme))
            .arg(QString::fromStdString(step.host))
            .arg(static_cast<unsigned>(step.port));
        case Extracts: return QStringLiteral("%1 extracts")
            .arg(static_cast<qulonglong>(step.extracts.size()));
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole)
        return theme::tokens().text_primary;
    return {};
}

QVariant MacroStepsModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Index: return QStringLiteral("#");
    case Label: return QStringLiteral("Label");
    case Target: return QStringLiteral("Target");
    case Extracts: return QStringLiteral("Extracts");
    default: return {};
    }
}

void MacroStepsModel::multiData(const QModelIndex& index,
                                QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

void MacroStepsModel::adopt(const aida::burp::session_handler::macro_t& macro) {
    beginResetModel();
    steps_ = macro.steps;
    endResetModel();
}

SessionHandlerView::SessionHandlerView(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.session"));
    const auto& t = theme::tokens();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* header = new QLabel(QStringLiteral("Session handler / Macros"), content);
    header->setProperty("aidaTone", QStringLiteral("titleAccent"));
    layout->addWidget(header);

    banner_ = new ReviewedContextBanner(content);
    banner_->setStaleText(QStringLiteral("STALE REVIEWED SOURCE"));
    layout->addWidget(banner_);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);

    auto* leftPanel = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(t.spacing.sm);
    auto* macrosTitle = new QLabel(QStringLiteral("Macros"), leftPanel);
    macrosTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    leftLayout->addWidget(macrosTitle);
    macro_model_ = new MacroListModel(leftPanel);
    auto* macroHost = new QWidget(leftPanel);
    macro_stack_ = new QStackedLayout(macroHost);
    macro_stack_->setStackingMode(QStackedLayout::StackOne);
    macro_stack_->setContentsMargins(0, 0, 0, 0);
    macro_table_ = new QTableView(macroHost);
    macro_table_->setObjectName(QStringLiteral("aida.view.network.session.macros"));
    macro_table_->verticalHeader()->hide();
    macro_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    macro_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    macro_table_->horizontalHeader()->setStretchLastSection(true);
    macro_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    macro_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    macro_table_->setAlternatingRowColors(true);
    macro_table_->setShowGrid(false);
    macro_table_->setModel(macro_model_);
    macro_stack_->addWidget(macro_table_);
    macro_empty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No macros"),
        QStringLiteral("Create a macro to replay a recorded request sequence."),
        macroHost);
    macro_empty_->setObjectName(QStringLiteral("aida.view.network.session.macros_empty"));
    macro_stack_->addWidget(macro_empty_);
    connect(macro_model_, &QAbstractItemModel::modelReset, this,
        [this] { updateMacroEmptyState(); });
    connect(macro_model_, &QAbstractItemModel::rowsInserted, this,
        [this] { updateMacroEmptyState(); });
    connect(macro_model_, &QAbstractItemModel::rowsRemoved, this,
        [this] { updateMacroEmptyState(); });
    leftLayout->addWidget(macroHost, 1);
    auto* newMacroRow = new QHBoxLayout();
    newMacroRow->setSpacing(t.spacing.sm);
    new_macro_name_ = new QLineEdit(leftPanel);
    new_macro_name_->setMaxLength(127);
    new_macro_name_->setPlaceholderText(QStringLiteral("Name"));
    newMacroRow->addWidget(new_macro_name_, 1);
    create_macro_button_ = new widgets::AidaButton(QStringLiteral("Create macro"), leftPanel);
    create_macro_button_->setKind(widgets::AidaButton::Kind::Primary);
    create_macro_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    newMacroRow->addWidget(create_macro_button_);
    delete_macro_button_ = new widgets::AidaButton(QStringLiteral("Delete macro"), leftPanel);
    delete_macro_button_->setKind(widgets::AidaButton::Kind::Ghost);
    delete_macro_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    newMacroRow->addWidget(delete_macro_button_);
    leftLayout->addLayout(newMacroRow);

    auto* rulesTitle = new QLabel(QStringLiteral("Session rules"), leftPanel);
    rulesTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    leftLayout->addWidget(rulesTitle);
    rule_model_ = new SessionRuleListModel(leftPanel);
    auto* ruleHost = new QWidget(leftPanel);
    rule_stack_ = new QStackedLayout(ruleHost);
    rule_stack_->setStackingMode(QStackedLayout::StackOne);
    rule_stack_->setContentsMargins(0, 0, 0, 0);
    rule_table_ = new QTableView(ruleHost);
    rule_table_->setObjectName(QStringLiteral("aida.view.network.session.rules"));
    rule_table_->verticalHeader()->hide();
    rule_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    rule_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    rule_table_->horizontalHeader()->setStretchLastSection(true);
    rule_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rule_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    rule_table_->setAlternatingRowColors(true);
    rule_table_->setShowGrid(false);
    rule_table_->setModel(rule_model_);
    rule_stack_->addWidget(rule_table_);
    rule_empty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No session rules"),
        QStringLiteral("Add a rule with the form below to run a macro when traffic matches."),
        ruleHost);
    rule_empty_->setObjectName(QStringLiteral("aida.view.network.session.rules_empty"));
    rule_stack_->addWidget(rule_empty_);
    connect(rule_model_, &QAbstractItemModel::modelReset, this,
        [this] { updateRuleEmptyState(); });
    connect(rule_model_, &QAbstractItemModel::rowsInserted, this,
        [this] { updateRuleEmptyState(); });
    connect(rule_model_, &QAbstractItemModel::rowsRemoved, this,
        [this] { updateRuleEmptyState(); });
    leftLayout->addWidget(ruleHost, 1);

    auto* ruleForm = new QFormLayout();
    ruleForm->setSpacing(t.spacing.xs);
    new_rule_name_ = new QLineEdit(leftPanel);
    new_rule_name_->setMaxLength(127);
    ruleForm->addRow(QStringLiteral("Name"), new_rule_name_);
    new_rule_match_ = new QComboBox(leftPanel);
    new_rule_match_->addItems({QStringLiteral("url_regex"),
        QStringLiteral("response_status"), QStringLiteral("response_regex")});
    ruleForm->addRow(QStringLiteral("Match"), new_rule_match_);
    new_rule_pattern_ = new QLineEdit(leftPanel);
    new_rule_pattern_->setMaxLength(511);
    ruleForm->addRow(QStringLiteral("Pattern"), new_rule_pattern_);
    rule_status_label_ = new QLabel(QStringLiteral("HTTP status"), leftPanel);
    new_rule_status_ = new QSpinBox(leftPanel);
    new_rule_status_->setRange(100, 599);
    new_rule_status_->setValue(200);
    ruleForm->addRow(rule_status_label_, new_rule_status_);
    new_rule_macro_id_ = new QLineEdit(QStringLiteral("0"), leftPanel);
    new_rule_macro_id_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^[0-9]{1,20}$")), new_rule_macro_id_));
    ruleForm->addRow(QStringLiteral("Macro id"), new_rule_macro_id_);
    auto* replRow = new QWidget(leftPanel);
    auto* replLayout = new QHBoxLayout(replRow);
    replLayout->setContentsMargins(0, 0, 0, 0);
    replLayout->setSpacing(t.spacing.sm);
    new_rule_repl_url_ = new QCheckBox(QStringLiteral("URL"), replRow);
    new_rule_repl_url_->setChecked(true);
    replLayout->addWidget(new_rule_repl_url_);
    new_rule_repl_headers_ = new QCheckBox(QStringLiteral("Headers"), replRow);
    new_rule_repl_headers_->setChecked(true);
    replLayout->addWidget(new_rule_repl_headers_);
    new_rule_repl_body_ = new QCheckBox(QStringLiteral("Body"), replRow);
    new_rule_repl_body_->setChecked(true);
    replLayout->addWidget(new_rule_repl_body_);
    replLayout->addStretch(1);
    ruleForm->addRow(replRow);
    leftLayout->addLayout(ruleForm);
    auto* ruleButtons = new QHBoxLayout();
    ruleButtons->setSpacing(t.spacing.sm);
    add_rule_button_ = new widgets::AidaButton(QStringLiteral("Add rule"), leftPanel);
    add_rule_button_->setKind(widgets::AidaButton::Kind::Primary);
    add_rule_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    ruleButtons->addWidget(add_rule_button_);
    delete_rule_button_ = new widgets::AidaButton(QStringLiteral("Delete rule"), leftPanel);
    delete_rule_button_->setKind(widgets::AidaButton::Kind::Ghost);
    delete_rule_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    ruleButtons->addWidget(delete_rule_button_);
    ruleButtons->addStretch(1);
    leftLayout->addLayout(ruleButtons);
    splitter->addWidget(leftPanel);

    auto* rightPanel = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(t.spacing.sm);
    auto* macroEditRow = new QHBoxLayout();
    macroEditRow->setSpacing(t.spacing.sm);
    edit_macro_name_ = new QLineEdit(rightPanel);
    edit_macro_name_->setMaxLength(127);
    macroEditRow->addWidget(edit_macro_name_, 1);
    rename_button_ = new widgets::AidaButton(QStringLiteral("Rename"), rightPanel);
    rename_button_->setKind(widgets::AidaButton::Kind::Secondary);
    rename_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    macroEditRow->addWidget(rename_button_);
    run_button_ = new widgets::AidaButton(QStringLiteral("Run macro now"), rightPanel);
    run_button_->setKind(widgets::AidaButton::Kind::Primary);
    run_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    macroEditRow->addWidget(run_button_);
    rightLayout->addLayout(macroEditRow);

    auto* stepsTitle = new QLabel(QStringLiteral("Steps"), rightPanel);
    stepsTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    rightLayout->addWidget(stepsTitle);
    steps_model_ = new MacroStepsModel(rightPanel);
    auto* stepsHost = new QWidget(rightPanel);
    steps_stack_ = new QStackedLayout(stepsHost);
    steps_stack_->setStackingMode(QStackedLayout::StackOne);
    steps_stack_->setContentsMargins(0, 0, 0, 0);
    steps_table_ = new QTableView(stepsHost);
    steps_table_->setObjectName(QStringLiteral("aida.view.network.session.steps"));
    steps_table_->verticalHeader()->hide();
    steps_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    steps_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    steps_table_->horizontalHeader()->setStretchLastSection(true);
    steps_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    steps_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    steps_table_->setAlternatingRowColors(true);
    steps_table_->setShowGrid(false);
    steps_table_->setModel(steps_model_);
    steps_stack_->addWidget(steps_table_);
    steps_empty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No steps"),
        QStringLiteral("Select a macro to view its steps, or add one with the form below."),
        stepsHost);
    steps_empty_->setObjectName(QStringLiteral("aida.view.network.session.steps_empty"));
    steps_stack_->addWidget(steps_empty_);
    connect(steps_model_, &QAbstractItemModel::modelReset, this,
        [this] { updateStepsEmptyState(); });
    connect(steps_model_, &QAbstractItemModel::rowsInserted, this,
        [this] { updateStepsEmptyState(); });
    connect(steps_model_, &QAbstractItemModel::rowsRemoved, this,
        [this] { updateStepsEmptyState(); });
    rightLayout->addWidget(stepsHost, 1);
    auto* stepButtons = new QHBoxLayout();
    stepButtons->setSpacing(t.spacing.sm);
    delete_step_button_ = new widgets::AidaButton(QStringLiteral("Delete step"), rightPanel);
    delete_step_button_->setKind(widgets::AidaButton::Kind::Ghost);
    delete_step_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    stepButtons->addWidget(delete_step_button_);
    stepButtons->addStretch(1);
    rightLayout->addLayout(stepButtons);

    auto* addStepTitle = new QLabel(QStringLiteral("Add step"), rightPanel);
    addStepTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    rightLayout->addWidget(addStepTitle);
    auto* stepForm = new QFormLayout();
    stepForm->setSpacing(t.spacing.xs);
    new_step_label_ = new QLineEdit(rightPanel);
    new_step_label_->setMaxLength(127);
    stepForm->addRow(QStringLiteral("Label"), new_step_label_);
    new_step_host_ = new QLineEdit(rightPanel);
    new_step_host_->setMaxLength(255);
    stepForm->addRow(QStringLiteral("Host"), new_step_host_);
    auto* schemeRow = new QWidget(rightPanel);
    auto* schemeLayout = new QHBoxLayout(schemeRow);
    schemeLayout->setContentsMargins(0, 0, 0, 0);
    schemeLayout->setSpacing(t.spacing.sm);
    new_step_scheme_ = new QLineEdit(QStringLiteral("https"), schemeRow);
    new_step_scheme_->setMaxLength(15);
    schemeLayout->addWidget(new_step_scheme_, 1);
    schemeLayout->addWidget(new QLabel(QStringLiteral("Port"), schemeRow));
    new_step_port_ = new QSpinBox(schemeRow);
    new_step_port_->setRange(1, 65535);
    new_step_port_->setValue(443);
    schemeLayout->addWidget(new_step_port_);
    schemeLayout->addWidget(new QLabel(QStringLiteral("Timeout(ms)"), schemeRow));
    new_step_timeout_ = new QSpinBox(schemeRow);
    new_step_timeout_->setRange(100, 120000);
    new_step_timeout_->setValue(15000);
    schemeLayout->addWidget(new_step_timeout_);
    stepForm->addRow(QStringLiteral("Scheme"), schemeRow);
    step_editor_ = new QtHumanRequestEditor(rightPanel);
    QtHumanRequestEditor::Config stepConfig;
    stepConfig.stableId = QStringLiteral("session-handler-new-step");
    stepConfig.maxBytes = 8191;
    stepConfig.editable = true;
    step_editor_->setConfig(stepConfig);
    step_editor_->setMinimumHeight(editor_min_height_lines(step_editor_, 5));
    stepForm->addRow(QStringLiteral("Request"), step_editor_);
    rightLayout->addLayout(stepForm);

    auto* extractorTitle = new QLabel(QStringLiteral("Extractor (optional)"), rightPanel);
    extractorTitle->setProperty("aidaTone", QStringLiteral("secondary"));
    rightLayout->addWidget(extractorTitle);
    auto* extractorForm = new QFormLayout();
    extractorForm->setSpacing(t.spacing.xs);
    new_extract_name_ = new QLineEdit(rightPanel);
    new_extract_name_->setMaxLength(63);
    extractorForm->addRow(QStringLiteral("Name"), new_extract_name_);
    new_extract_from_ = new QComboBox(rightPanel);
    new_extract_from_->addItems({QStringLiteral("resp_body"),
        QStringLiteral("resp_headers"), QStringLiteral("resp_url")});
    extractorForm->addRow(QStringLiteral("From"), new_extract_from_);
    new_extract_regex_ = new QLineEdit(rightPanel);
    new_extract_regex_->setMaxLength(511);
    extractorForm->addRow(QStringLiteral("Regex"), new_extract_regex_);
    new_extract_group_ = new QSpinBox(rightPanel);
    new_extract_group_->setRange(0, 64);
    new_extract_group_->setValue(1);
    extractorForm->addRow(QStringLiteral("Group"), new_extract_group_);
    rightLayout->addLayout(extractorForm);
    add_step_button_ = new widgets::AidaButton(QStringLiteral("Add step"), rightPanel);
    add_step_button_->setKind(widgets::AidaButton::Kind::Primary);
    add_step_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    rightLayout->addWidget(add_step_button_);

    auto* extractedTitle = new QLabel(QStringLiteral("Last extracted values"), rightPanel);
    extractedTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    rightLayout->addWidget(extractedTitle);
    extracted_label_ = new QLabel(rightPanel);
    extracted_label_->setProperty("aidaTone", QStringLiteral("dim"));
    rightLayout->addWidget(extracted_label_);
    extracted_view_ = new QPlainTextEdit(rightPanel);
    extracted_view_->setReadOnly(true);
    extracted_view_->setFont(theme::fonts::codeRegular());
    extracted_view_->setMaximumBlockCount(1024);
    extracted_view_->setPlaceholderText(QStringLiteral(
        "Extractor captures appear after the macro runs"));
    rightLayout->addWidget(extracted_view_, 1);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 21);
    splitter->setStretchFactor(1, 29);
    layout->addWidget(splitter, 1);

    connect(new_rule_match_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const bool statusVisible = index == 1;
        rule_status_label_->setVisible(statusVisible);
        new_rule_status_->setVisible(statusVisible);
    });
    new_rule_status_->setVisible(false);
    rule_status_label_->setVisible(false);

    connect(create_macro_button_, &QAbstractButton::clicked, this, [this] { createMacro(); });
    connect(delete_macro_button_, &QAbstractButton::clicked, this, [this] { deleteMacro(); });
    connect(add_rule_button_, &QAbstractButton::clicked, this, [this] { addRule(); });
    connect(delete_rule_button_, &QAbstractButton::clicked, this, [this] { deleteRule(); });
    connect(rename_button_, &QAbstractButton::clicked, this, [this] { renameMacro(); });
    connect(run_button_, &QAbstractButton::clicked, this, [this] { runMacroNow(); });
    connect(delete_step_button_, &QAbstractButton::clicked, this, [this] { deleteSelectedStep(); });
    connect(add_step_button_, &QAbstractButton::clicked, this, [this] { addStep(); });

    connect(macro_table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* macro = macro_model_->rowAt(current.isValid() ? current.row() : -1);
            const std::uint64_t id = macro ? macro->id : 0;
            if (id == selected_macro_id_)
                return;
            selected_macro_id_ = id;
            if (macro) {
                diag::log_tagged_fmt("session_v", "macro_selected id=%llu name='%s' steps=%zu",
                    static_cast<unsigned long long>(macro->id), macro->name.c_str(),
                    macro->steps.size());
                edit_macro_name_->setText(QString::fromStdString(macro->name));
            }
            edit_step_index_ = -1;
            refreshMacroEditor();
            refreshButtons();
        });
    connect(rule_table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* rule = rule_model_->rowAt(current.isValid() ? current.row() : -1);
            selected_rule_id_ = rule ? rule->id : 0;
            refreshButtons();
        });
    connect(steps_table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            edit_step_index_ = current.isValid() ? current.row() : -1;
            refreshButtons();
        });
    connect(step_editor_, &QtHumanRequestEditor::validityChanged, this,
        [this](bool, const QString&) { refreshButtons(); });
    connect(step_editor_, &QtHumanRequestEditor::hasUnappliedPrettyChanged, this,
        [this](bool) { refreshButtons(); });

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(500);
    connect(refresh_timer_, &QTimer::timeout, this, [this] { refreshLists(); });

    aida::burp::session_handler_view::set_reviewed_context_staged_hook(
        [pane = QPointer<SessionHandlerView>(this)] {
            if (!pane)
                return;
            QMetaObject::invokeMethod(pane.data(), [pane] { pane->drainStaged(); },
                Qt::QueuedConnection);
        });
    hooks_installed_ = true;
    drainStaged();

    ensureInitialized();
    refreshLists();
    refreshMacroEditor();
    refreshButtons();
    updateMacroEmptyState();
    updateRuleEmptyState();
    updateStepsEmptyState();
    setContent(content);
}

SessionHandlerView::~SessionHandlerView() {
    if (hooks_installed_)
        aida::burp::session_handler_view::set_reviewed_context_staged_hook(nullptr);
}

void SessionHandlerView::updateMacroEmptyState() {
    if (!macro_stack_ || !macro_empty_ || !macro_table_ || !macro_model_)
        return;
    macro_stack_->setCurrentWidget(macro_model_->rowCount() == 0
        ? static_cast<QWidget*>(macro_empty_) : static_cast<QWidget*>(macro_table_));
}

void SessionHandlerView::updateRuleEmptyState() {
    if (!rule_stack_ || !rule_empty_ || !rule_table_ || !rule_model_)
        return;
    rule_stack_->setCurrentWidget(rule_model_->rowCount() == 0
        ? static_cast<QWidget*>(rule_empty_) : static_cast<QWidget*>(rule_table_));
}

void SessionHandlerView::updateStepsEmptyState() {
    if (!steps_stack_ || !steps_empty_ || !steps_table_ || !steps_model_)
        return;
    steps_stack_->setCurrentWidget(steps_model_->rowCount() == 0
        ? static_cast<QWidget*>(steps_empty_) : static_cast<QWidget*>(steps_table_));
}

void SessionHandlerView::onPaneShown() {
    ensureInitialized();
    refreshLists();
    refresh_timer_->start();
}

void SessionHandlerView::onPaneHidden() {
    refresh_timer_->stop();
}

void SessionHandlerView::ensureInitialized() {
    if (initialized_.load(std::memory_order_acquire) ||
        initialization_requested_.exchange(true, std::memory_order_acq_rel))
        return;
    QPointer<SessionHandlerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.session_handler_view";
    submission.label = "session_handler.initialize";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane]() {
        aida::burp::session_handler::initialize();
        if (!pane)
            return;
        pane->initialized_.store(true, std::memory_order_release);
        pane->initialization_requested_.store(false, std::memory_order_release);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted)
        initialization_requested_.store(false, std::memory_order_release);
}

void SessionHandlerView::refreshLists() {
    const QSignalBlocker macroBlocker(macro_table_->selectionModel());
    const QSignalBlocker ruleBlocker(rule_table_->selectionModel());
    macro_model_->adopt(aida::burp::session_handler::list_macros());
    rule_model_->adopt(aida::burp::session_handler::list_rules());
    if (selected_macro_id_ != 0 && !macro_model_->findById(selected_macro_id_))
        selected_macro_id_ = 0;
    if (selected_macro_id_ != 0) {
        for (int row = 0; row < macro_model_->rowCount(); ++row) {
            const auto* macro = macro_model_->rowAt(row);
            if (macro && macro->id == selected_macro_id_) {
                macro_table_->setCurrentIndex(macro_model_->index(row, 0));
                break;
            }
        }
    }
    if (selected_rule_id_ != 0) {
        bool found = false;
        for (int row = 0; row < rule_model_->rowCount(); ++row) {
            const auto* rule = rule_model_->rowAt(row);
            if (rule && rule->id == selected_rule_id_) {
                rule_table_->setCurrentIndex(rule_model_->index(row, 0));
                found = true;
                break;
            }
        }
        if (!found)
            selected_rule_id_ = 0;
    }
    refreshMacroEditor();
    refreshButtons();
}

void SessionHandlerView::refreshMacroEditor() {
    if (selected_macro_id_ == 0) {
        current_macro_valid_ = false;
        steps_model_->adopt(current_macro_);
        extracted_label_->clear();
        extracted_view_->clear();
        return;
    }
    aida::burp::session_handler::macro_t macro;
    if (!aida::burp::session_handler::get_macro(selected_macro_id_, macro)) {
        current_macro_valid_ = false;
        steps_model_->adopt(current_macro_);
        return;
    }
    current_macro_ = std::move(macro);
    current_macro_valid_ = true;
    steps_model_->adopt(current_macro_);
    if (edit_step_index_ >= static_cast<int>(current_macro_.steps.size()))
        edit_step_index_ = -1;
    refreshExtractedValues();
}

void SessionHandlerView::refreshExtractedValues() {
    std::map<std::string, std::string> values;
    bool ok = false;
    if (current_macro_valid_ && last_run_macro_id_ == current_macro_.id) {
        values = last_run_values_;
        ok = last_run_ok_;
    }
    if (values.empty() && current_macro_valid_) {
        values = current_macro_.last_extracted_values;
        ok = current_macro_.ok_last_run;
    }
    extracted_label_->setText(ok ? QStringLiteral("ok=yes") : QStringLiteral("ok=no"));
    QString text;
    for (const auto& kv : values) {
        const std::string& value = kv.second;
        text += QString::fromStdString(kv.first);
        text += QStringLiteral(" = ");
        text += value.size() > 200
            ? QString::fromStdString(value.substr(0, 200) + "...")
            : QString::fromStdString(value);
        text += QLatin1Char('\n');
    }
    if (text != extracted_view_->toPlainText())
        extracted_view_->setPlainText(text);
}

void SessionHandlerView::refreshButtons() {
    const bool hasMacro = selected_macro_id_ != 0 && current_macro_valid_;
    delete_macro_button_->setEnabled(selected_macro_id_ != 0);
    rename_button_->setEnabled(hasMacro);
    run_button_->setEnabled(hasMacro);
    const bool hasStep = edit_step_index_ >= 0 &&
        edit_step_index_ < static_cast<int>(current_macro_.steps.size());
    delete_step_button_->setEnabled(hasStep);
    add_step_button_->setEnabled(selected_macro_id_ != 0 &&
        step_editor_->isValid() && !step_editor_->hasUnappliedPretty());
    delete_rule_button_->setEnabled(selected_rule_id_ != 0);
}

void SessionHandlerView::createMacro() {
    aida::burp::session_handler::macro_t macro;
    macro.name = new_macro_name_->text().toStdString();
    QPointer<SessionHandlerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.session_handler_view";
    submission.label = "session_handler.add_macro";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, macro = std::move(macro)]() mutable {
        const std::uint64_t id = aida::burp::session_handler::add_macro(std::move(macro));
        if (!pane || id == 0)
            return;
        QMetaObject::invokeMethod(pane.data(), [pane, id]() {
            pane->selected_macro_id_ = id;
            pane->refreshLists();
            for (int row = 0; row < pane->macro_model_->rowCount(); ++row) {
                const auto* candidate = pane->macro_model_->rowAt(row);
                if (candidate && candidate->id == id) {
                    pane->macro_table_->setCurrentIndex(pane->macro_model_->index(row, 0));
                    break;
                }
            }
        }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
    new_macro_name_->clear();
}

void SessionHandlerView::deleteMacro() {
    if (selected_macro_id_ == 0)
        return;
    diag::log_tagged_fmt("session_v", "macro_deleted id=%llu",
        static_cast<unsigned long long>(selected_macro_id_));
    const std::uint64_t id = selected_macro_id_;
    QPointer<SessionHandlerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.session_handler_view";
    submission.label = "session_handler.remove_macro";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, id]() {
        aida::burp::session_handler::remove_macro(id);
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(), [pane]() {
            pane->selected_macro_id_ = 0;
            pane->refreshLists();
        }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void SessionHandlerView::addRule() {
    aida::burp::session_handler::session_rule_t rule;
    rule.name = new_rule_name_->text().toStdString();
    rule.match = static_cast<aida::burp::session_handler::sh_match_t>(new_rule_match_->currentIndex());
    rule.match_pattern = new_rule_pattern_->text().toStdString();
    rule.match_status = new_rule_status_->value();
    rule.macro_id = new_rule_macro_id_->text().toULongLong();
    rule.replace_in_url = new_rule_repl_url_->isChecked();
    rule.replace_in_headers = new_rule_repl_headers_->isChecked();
    rule.replace_in_body = new_rule_repl_body_->isChecked();
    rule.active = true;
    QPointer<SessionHandlerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.session_handler_view";
    submission.label = "session_handler.add_rule";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, rule = std::move(rule)]() mutable {
        const std::string name = rule.name;
        const int match = static_cast<int>(rule.match);
        const std::uint64_t macroId = rule.macro_id;
        const std::uint64_t id = aida::burp::session_handler::add_rule(std::move(rule));
        diag::log_tagged_fmt("session_v", "rule_added name='%s' match=%d macro_id=%llu",
            name.c_str(), match, static_cast<unsigned long long>(macroId));
        if (!pane || id == 0)
            return;
        QMetaObject::invokeMethod(pane.data(), [pane, id]() {
            pane->selected_rule_id_ = id;
            pane->refreshLists();
            for (int row = 0; row < pane->rule_model_->rowCount(); ++row) {
                const auto* candidate = pane->rule_model_->rowAt(row);
                if (candidate && candidate->id == id) {
                    pane->rule_table_->setCurrentIndex(pane->rule_model_->index(row, 0));
                    break;
                }
            }
        }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
    new_rule_name_->clear();
    new_rule_pattern_->clear();
}

void SessionHandlerView::deleteRule() {
    if (selected_rule_id_ == 0)
        return;
    diag::log_tagged_fmt("session_v", "rule_deleted id=%llu",
        static_cast<unsigned long long>(selected_rule_id_));
    const std::uint64_t id = selected_rule_id_;
    QPointer<SessionHandlerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.session_handler_view";
    submission.label = "session_handler.remove_rule";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, id]() {
        aida::burp::session_handler::remove_rule(id);
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(), [pane]() {
            pane->selected_rule_id_ = 0;
            pane->refreshLists();
        }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void SessionHandlerView::renameMacro() {
    if (!current_macro_valid_)
        return;
    auto macro = current_macro_;
    diag::log_tagged_fmt("session_v", "macro_rename id=%llu new_name='%s'",
        static_cast<unsigned long long>(macro.id), edit_macro_name_->text().toStdString().c_str());
    macro.name = edit_macro_name_->text().toStdString();
    queueMacroUpdate(std::move(macro));
}

void SessionHandlerView::runMacroNow() {
    if (!current_macro_valid_)
        return;
    const std::uint64_t macroId = current_macro_.id;
    diag::log_tagged_fmt("session_v", "macro_run id=%llu name='%s'",
        static_cast<unsigned long long>(macroId), current_macro_.name.c_str());
    QPointer<SessionHandlerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.session_view";
    submission.label = "session.macro_run";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, macroId]() {
        std::map<std::string, std::string> values;
        const bool ok = aida::burp::session_handler::run_macro(macroId, values);
        diag::log_tagged_fmt("session_v", "macro_run_result id=%llu ok=%d extracted=%zu",
            static_cast<unsigned long long>(macroId), ok ? 1 : 0, values.size());
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(),
            [pane, macroId, ok, values = std::move(values)]() mutable {
                pane->onMacroRunFinished(macroId, ok, std::move(values));
            }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void SessionHandlerView::onMacroRunFinished(std::uint64_t macroId, bool ok,
                                            std::map<std::string, std::string> values) {
    last_run_values_ = std::move(values);
    last_run_ok_ = ok;
    last_run_macro_id_ = macroId;
    refreshExtractedValues();
}

void SessionHandlerView::deleteSelectedStep() {
    if (!current_macro_valid_ || edit_step_index_ < 0 ||
        edit_step_index_ >= static_cast<int>(current_macro_.steps.size()))
        return;
    const int index = edit_step_index_;
    diag::log_tagged_fmt("session_v", "step_deleted macro_id=%llu step_idx=%d",
        static_cast<unsigned long long>(current_macro_.id), index);
    auto macro = current_macro_;
    macro.steps.erase(macro.steps.begin() + static_cast<ptrdiff_t>(index));
    queueMacroUpdate(std::move(macro));
    edit_step_index_ = -1;
    refreshButtons();
}

void SessionHandlerView::addStep() {
    if (!current_macro_valid_)
        return;
    aida::burp::session_handler::macro_step_t step;
    step.label = new_step_label_->text().toStdString();
    step.host = new_step_host_->text().toStdString();
    step.scheme = new_step_scheme_->text().toStdString();
    step.port = static_cast<std::uint16_t>(new_step_port_->value());
    const std::string raw = step_editor_->authority().toStdString();
    step.raw_request.assign(raw.begin(), raw.end());
    step.timeout_ms = new_step_timeout_->value();
    diag::log_tagged_fmt("session_v", "step_adding macro_id=%llu label='%s' host='%s' port=%d",
        static_cast<unsigned long long>(current_macro_.id), step.label.c_str(),
        step.host.c_str(), step.port);
    if (!new_extract_name_->text().isEmpty() && !new_extract_regex_->text().isEmpty()) {
        aida::burp::session_handler::extract_t extract;
        extract.name = new_extract_name_->text().toStdString();
        extract.regex = new_extract_regex_->text().toStdString();
        extract.from = new_extract_from_->currentText().toStdString();
        extract.group = new_extract_group_->value();
        step.extracts.push_back(std::move(extract));
    }
    auto macro = current_macro_;
    macro.steps.push_back(std::move(step));
    queueMacroUpdate(std::move(macro));
    new_step_label_->clear();
    new_step_host_->clear();
    step_editor_->setAuthority(QStringLiteral("session-handler.new-step.cleared"), QString());
    new_extract_name_->clear();
    new_extract_regex_->clear();
}

void SessionHandlerView::queueMacroUpdate(aida::burp::session_handler::macro_t macro) {
    QPointer<SessionHandlerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.session_handler_view";
    submission.label = "session_handler.update_macro";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, macro = std::move(macro)]() mutable {
        aida::burp::session_handler::update_macro(std::move(macro));
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(), [pane]() {
            pane->refreshLists();
        }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void SessionHandlerView::drainStaged() {
    aida::burp::session_handler_view::staged_reviewed_context_t staged;
    if (!aida::burp::session_handler_view::take_staged_reviewed_context(staged))
        return;
    stageReviewedContext(staged.identity, staged.request_bytes);
}

void SessionHandlerView::stageReviewedContext(
    const network_view::artifact_identity_t& identity,
    const std::vector<std::uint8_t>& requestBytes) {
    banner_->setContext(identity);
    new_step_host_->setText(QString::fromStdString(identity.target_host).left(255));
    new_step_scheme_->setText(identity.use_tls ? QStringLiteral("https")
                                               : QStringLiteral("http"));
    new_step_port_->setValue(identity.target_port);
    const QByteArray bytes(reinterpret_cast<const char*>(requestBytes.data()),
        static_cast<qsizetype>(requestBytes.size()));
    step_editor_->setAuthority(QStringLiteral("session-handler.new-step.staged"),
        QString::fromUtf8(bytes));
    refreshButtons();
}

}
