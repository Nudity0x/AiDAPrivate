#include "qt/ai/qt_agent_manager_view.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>

#include "core/ai/agent_registry.hpp"
#include "core/ai/provider_catalog.hpp"
#include "core/infra/event_bus.hpp"
#include "qt/ai/qt_ai_chat_dialogs.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_chip.hpp"
#include "qt/widgets/aida_headers.hpp"

namespace aida::qt::ai {

namespace {

QStringList permission_action_labels() {
    return { QStringLiteral("allow"), QStringLiteral("deny"), QStringLiteral("ask") };
}

aida::agent::permission_rule_t::action_t action_from_int(int v) {
    switch (v) {
    case 0: return aida::agent::permission_rule_t::action_t::allow;
    case 1: return aida::agent::permission_rule_t::action_t::deny;
    default: return aida::agent::permission_rule_t::action_t::ask;
    }
}

int int_from_action(aida::agent::permission_rule_t::action_t a) {
    switch (a) {
    case aida::agent::permission_rule_t::action_t::allow: return 0;
    case aida::agent::permission_rule_t::action_t::deny: return 1;
    default: return 2;
    }
}

int mode_from_enum(aida::agent::agent_info_t::mode_t m) {
    switch (m) {
    case aida::agent::agent_info_t::mode_t::primary: return 0;
    case aida::agent::agent_info_t::mode_t::subagent: return 1;
    default: return 2;
    }
}

aida::agent::agent_info_t::mode_t mode_from_int(int v) {
    switch (v) {
    case 0: return aida::agent::agent_info_t::mode_t::primary;
    case 1: return aida::agent::agent_info_t::mode_t::subagent;
    default: return aida::agent::agent_info_t::mode_t::all;
    }
}

bool matches_filter(const aida::agent::agent_info_t& info, const QString& filter_lower) {
    if (filter_lower.isEmpty())
        return true;
    if (QString::fromStdString(info.name).toLower().contains(filter_lower))
        return true;
    return QString::fromStdString(info.description).toLower().contains(filter_lower);
}

}

AidaAgentListModel::AidaAgentListModel(QObject* parent) : QAbstractListModel(parent) {}

int AidaAgentListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(rows_.size());
}

QVariant AidaAgentListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(rows_.size()))
        return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole: {
        QString label = QString::fromStdString(row.name);
        if (row.native)
            label += QStringLiteral("  [native]");
        if (row.hidden)
            label += QStringLiteral("  (hidden)");
        return label;
    }
    case Qt::ToolTipRole:
        return QString::fromStdString(row.description);
    case Qt::UserRole:
        return QString::fromStdString(row.name);
    default:
        return {};
    }
}

void AidaAgentListModel::setFilter(const QString& filter) {
    if (filter_ == filter)
        return;
    filter_ = filter;
    reloadFrom(publication_);
}

void AidaAgentListModel::reloadFrom(const aida::agent_manager_service::snapshot_ptr& publication) {
    beginResetModel();
    publication_ = publication;
    rows_.clear();
    if (publication_) {
        const QString filter_lower = filter_.toLower();
        rows_.reserve(publication_->agents.size());
        for (const auto& agent : publication_->agents) {
            if (!matches_filter(agent, filter_lower))
                continue;
            rows_.push_back(row_t{ agent.name, agent.description, agent.native, agent.hidden });
        }
    }
    endResetModel();
    Q_EMIT modelReplaced();
}

QString AidaAgentListModel::nameAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return {};
    return QString::fromStdString(rows_[static_cast<std::size_t>(row)].name);
}

const aida::agent::agent_info_t* AidaAgentListModel::agentAt(int row) const {
    if (!publication_ || row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return aida::agent_manager_service::find(publication_, rows_[static_cast<std::size_t>(row)].name);
}

AidaAgentRulesModel::AidaAgentRulesModel(QObject* parent) : QAbstractTableModel(parent) {}

int AidaAgentRulesModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(rows_.size());
}

int AidaAgentRulesModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return 3;
}

QVariant AidaAgentRulesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(rows_.size()))
        return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
        case 0: return row.key;
        case 1: return row.pattern;
        case 2: return permission_action_labels().at(row.action);
        default: return {};
        }
    }
    return {};
}

QVariant AidaAgentRulesModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case 0: return QStringLiteral("Permission Key");
    case 1: return QStringLiteral("Pattern");
    case 2: return QStringLiteral("Action");
    default: return {};
    }
}

bool AidaAgentRulesModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::EditRole || !index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(rows_.size()))
        return false;
    auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (index.column()) {
    case 0: row.key = value.toString(); break;
    case 1: row.pattern = value.toString(); break;
    case 2: {
        const int action = permission_action_labels().indexOf(value.toString());
        if (action < 0)
            return false;
        row.action = action;
        break;
    }
    default: return false;
    }
    Q_EMIT dataChanged(index, index, { role });
    Q_EMIT rulesEdited();
    return true;
}

Qt::ItemFlags AidaAgentRulesModel::flags(const QModelIndex& index) const {
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable;
}

void AidaAgentRulesModel::setRules(const aida::agent::ruleset_t& rules) {
    beginResetModel();
    rows_.clear();
    rows_.reserve(rules.size());
    for (const auto& rule : rules) {
        rows_.push_back(rule_row_t{ QString::fromStdString(rule.permission_key),
            QString::fromStdString(rule.pattern), int_from_action(rule.action) });
    }
    endResetModel();
}

aida::agent::ruleset_t AidaAgentRulesModel::rules() const {
    aida::agent::ruleset_t out;
    out.reserve(rows_.size());
    for (const auto& row : rows_) {
        aida::agent::permission_rule_t rule;
        rule.permission_key = row.key.toStdString();
        rule.pattern = row.pattern.toStdString();
        rule.action = action_from_int(row.action);
        if (rule.permission_key.empty())
            continue;
        if (rule.pattern.empty())
            rule.pattern = "*";
        out.push_back(std::move(rule));
    }
    return out;
}

void AidaAgentRulesModel::appendRule(const std::string& key, const std::string& pattern,
                                     aida::agent::permission_rule_t::action_t action) {
    const int row = static_cast<int>(rows_.size());
    beginInsertRows({}, row, row);
    rows_.push_back(rule_row_t{ QString::fromStdString(key),
        QString::fromStdString(pattern), int_from_action(action) });
    endInsertRows();
    Q_EMIT rulesEdited();
}

void AidaAgentRulesModel::removeRule(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return;
    beginRemoveRows({}, row, row);
    rows_.erase(rows_.begin() + row);
    endRemoveRows();
    Q_EMIT rulesEdited();
}

AidaAgentManagerView::AidaAgentManagerView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.ai.agents"));
    buildUi();
    wireService();
}

AidaAgentManagerView::~AidaAgentManagerView() {
    if (sub_changed_.valid())
        aida::events::unsubscribe(sub_changed_);
}

void AidaAgentManagerView::buildUi() {
    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.spacing.sm + t.spacing.xxs, t.spacing.sm,
                             t.spacing.sm + t.spacing.xxs, t.spacing.sm);
    root->setSpacing(t.spacing.xs + t.spacing.xxs);

    auto* header = new QHBoxLayout();
    auto* title_block = new QVBoxLayout();
    title_block->setSpacing(t.spacing.xxs);
    auto* title = new QLabel(QStringLiteral("Agents"), this);
    title->setFont(theme::fonts::h1());
    auto* subtitle = new QLabel(QStringLiteral(
        "Inspect built-in agents or configure custom reverse-engineering roles."), this);
    subtitle->setFont(theme::fonts::caption());
    subtitle->setProperty("aidaVariant", QStringLiteral("secondary"));
    status_label_ = new QLabel(this);
    status_label_->setFont(theme::fonts::caption());
    status_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    status_label_->setVisible(false);
    title_block->addWidget(title);
    title_block->addWidget(subtitle);
    title_block->addWidget(status_label_);
    header->addLayout(title_block, 1);
    reload_button_ = new QPushButton(QStringLiteral("Reload"), this);
    reload_button_->setToolTip(QStringLiteral("Reload the agent catalog from disk"));
    header->addWidget(reload_button_, 0, Qt::AlignTop);
    root->addLayout(header);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter_, 1);

    auto* left = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(t.spacing.xs + t.spacing.xxs);
    filter_edit_ = new QLineEdit(left);
    filter_edit_->setObjectName(QStringLiteral("aida.ai.agents.filter"));
    filter_edit_->setPlaceholderText(QStringLiteral("Filter agents"));
    filter_edit_->setClearButtonEnabled(true);
    left_layout->addWidget(filter_edit_);
    list_model_ = new AidaAgentListModel(this);
    list_ = new QListView(left);
    list_->setObjectName(QStringLiteral("aida.ai.agents.list"));
    list_->setModel(list_model_);
    list_->setUniformItemSizes(true);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    left_layout->addWidget(list_, 1);
    splitter_->addWidget(left);

    editor_scroll_ = new QScrollArea(this);
    editor_scroll_->setObjectName(QStringLiteral("aida.ai.agents.editor_scroll"));
    editor_scroll_->setWidgetResizable(true);
    editor_root_ = new QWidget();
    auto* editor = new QVBoxLayout(editor_root_);
    editor->setContentsMargins(t.panel.padding, t.panel.padding,
                               t.panel.padding, t.panel.padding);
    editor->setSpacing(t.spacing.sm);

    auto* detail_header = new QHBoxLayout();
    detail_title_ = new QLabel(editor_root_);
    detail_title_->setFont(theme::fonts::h2());
    detail_badges_ = new QLabel(editor_root_);
    detail_badges_->setFont(theme::fonts::caption());
    detail_badges_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detail_header->addWidget(detail_title_, 1);
    detail_header->addWidget(detail_badges_, 0, Qt::AlignVCenter);
    editor->addLayout(detail_header);

    native_banner_ = new QLabel(QStringLiteral(
        "This is a built-in native agent. Its definition is read-only; "
        "duplicate it as a custom agent to edit a copy."), editor_root_);
    native_banner_->setObjectName(QStringLiteral("aida.ai.agents.native_banner"));
    native_banner_->setWordWrap(true);
    native_banner_->setFont(theme::fonts::caption());
    native_banner_->setProperty("aidaVariant", QStringLiteral("info"));
    native_banner_->setVisible(false);
    editor->addWidget(native_banner_);

    auto* identity_form = new QFormLayout();
    name_edit_ = new QLineEdit(editor_root_);
    identity_form->addRow(QStringLiteral("Name"), name_edit_);
    editor->addLayout(identity_form);

    auto* description_label = new QLabel(QStringLiteral("Description"), editor_root_);
    description_label->setFont(theme::fonts::bodyEm());
    editor->addWidget(description_label);
    description_edit_ = new QPlainTextEdit(editor_root_);
    description_edit_->setMaximumBlockCount(64);
    description_edit_->setFixedHeight(t.control.input_h * 2 + t.spacing.sm);
    editor->addWidget(description_edit_);

    auto* color_row = new QHBoxLayout();
    color_edit_ = new QLineEdit(editor_root_);
    color_edit_->setPlaceholderText(QStringLiteral("#RRGGBB"));
    color_edit_->setMaxLength(16);
    color_swatch_ = new QPushButton(QStringLiteral("Pick..."), editor_root_);
    color_row->addWidget(new QLabel(QStringLiteral("Color"), editor_root_));
    color_row->addWidget(color_edit_, 1);
    color_row->addWidget(color_swatch_);
    editor->addLayout(color_row);

    auto* mode_form = new QFormLayout();
    mode_combo_ = new QComboBox(editor_root_);
    mode_combo_->addItems({ QStringLiteral("primary"), QStringLiteral("subagent"),
        QStringLiteral("all") });
    mode_form->addRow(QStringLiteral("Mode"), mode_combo_);
    editor->addLayout(mode_form);

    auto* prompt_header = new widgets::AidaSectionHeader(QStringLiteral("System prompt"),
        editor_root_);
    editor->addWidget(prompt_header);
    system_prompt_edit_ = new QPlainTextEdit(editor_root_);
    system_prompt_edit_->setFont(theme::fonts::codeRegular());
    system_prompt_edit_->setMinimumHeight(t.control.input_h * 3 + t.spacing.md);
    editor->addWidget(system_prompt_edit_);
    prompt_header->setActionLabel(QStringLiteral("collapse"));
    connect(prompt_header, &widgets::AidaSectionHeader::actionTriggered, this,
            [this, prompt_header] {
        const bool show = !system_prompt_edit_->isVisible();
        system_prompt_edit_->setVisible(show);
        prompt_header->setActionLabel(show ? QStringLiteral("collapse")
                                           : QStringLiteral("expand"));
    });

    auto* model_header = new widgets::AidaSectionHeader(QStringLiteral("Model override"),
        editor_root_);
    editor->addWidget(model_header);
    auto* model_body = new QWidget(editor_root_);
    auto* model_form = new QFormLayout(model_body);
    model_form->setContentsMargins(0, 0, 0, 0);
    provider_combo_ = new QComboBox(model_body);
    model_combo_ = new QComboBox(model_body);
    temperature_spin_ = new QDoubleSpinBox(model_body);
    temperature_spin_->setRange(0.0, 2.0);
    temperature_spin_->setSingleStep(0.05);
    top_p_spin_ = new QDoubleSpinBox(model_body);
    top_p_spin_->setRange(0.0, 1.0);
    top_p_spin_->setSingleStep(0.05);
    max_steps_spin_ = new QSpinBox(model_body);
    max_steps_spin_->setRange(0, 1000000);
    model_form->addRow(QStringLiteral("Provider"), provider_combo_);
    model_form->addRow(QStringLiteral("Model"), model_combo_);
    model_form->addRow(QStringLiteral("Temperature"), temperature_spin_);
    model_form->addRow(QStringLiteral("Top-p"), top_p_spin_);
    model_form->addRow(QStringLiteral("Max steps"), max_steps_spin_);
    editor->addWidget(model_body);
    model_header->setActionLabel(QStringLiteral("collapse"));
    connect(model_header, &widgets::AidaSectionHeader::actionTriggered, this,
            [this, model_header, model_body] {
        const bool show = !model_body->isVisible();
        model_body->setVisible(show);
        model_header->setActionLabel(show ? QStringLiteral("collapse")
                                          : QStringLiteral("expand"));
    });

    auto* rules_header = new widgets::AidaSectionHeader(QStringLiteral("Permission rules"),
        editor_root_);
    editor->addWidget(rules_header);
    auto* rules_body = new QWidget(editor_root_);
    auto* rules_layout = new QVBoxLayout(rules_body);
    rules_layout->setContentsMargins(0, 0, 0, 0);
    rules_layout->setSpacing(t.spacing.xs);
    rules_model_ = new AidaAgentRulesModel(rules_body);
    rules_table_ = new QTableView(rules_body);
    rules_table_->setObjectName(QStringLiteral("aida.ai.agents.rules"));
    rules_table_->setModel(rules_model_);
    rules_table_->horizontalHeader()->setStretchLastSection(true);
    rules_table_->setMinimumHeight(t.control.input_h * 3 + t.spacing.md);
    rules_layout->addWidget(rules_table_);
    auto* add_rule_row = new QHBoxLayout();
    rule_key_edit_ = new QLineEdit(rules_body);
    rule_key_edit_->setPlaceholderText(QStringLiteral("permission key"));
    rule_pattern_edit_ = new QLineEdit(rules_body);
    rule_pattern_edit_->setPlaceholderText(QStringLiteral("pattern (empty = *)"));
    rule_action_combo_ = new QComboBox(rules_body);
    rule_action_combo_->addItems(permission_action_labels());
    rule_action_combo_->setCurrentIndex(2);
    auto* add_rule = new QPushButton(QStringLiteral("Add rule"), rules_body);
    auto* remove_rule = new QPushButton(QStringLiteral("Remove selected"), rules_body);
    add_rule_row->addWidget(rule_key_edit_, 2);
    add_rule_row->addWidget(rule_pattern_edit_, 2);
    add_rule_row->addWidget(rule_action_combo_, 1);
    add_rule_row->addWidget(add_rule);
    add_rule_row->addWidget(remove_rule);
    rules_layout->addLayout(add_rule_row);
    editor->addWidget(rules_body);
    rules_header->setActionLabel(QStringLiteral("collapse"));
    connect(rules_header, &widgets::AidaSectionHeader::actionTriggered, this,
            [this, rules_header, rules_body] {
        const bool show = !rules_body->isVisible();
        rules_body->setVisible(show);
        rules_header->setActionLabel(show ? QStringLiteral("collapse")
                                          : QStringLiteral("expand"));
    });

    auto* tools_header = new widgets::AidaSectionHeader(QStringLiteral("Tools"), editor_root_);
    editor->addWidget(tools_header);
    auto* tools_body = new QWidget(editor_root_);
    auto* tools_layout = new QVBoxLayout(tools_body);
    tools_layout->setContentsMargins(0, 0, 0, 0);
    tools_layout->setSpacing(t.spacing.xs + t.spacing.xxs);
    auto* allowed_label = new QLabel(QStringLiteral("Tools allowed"), tools_body);
    allowed_label->setFont(theme::fonts::bodyEm());
    tools_layout->addWidget(allowed_label);
    tools_allowed_host_ = new QWidget(tools_body);
    tools_allowed_layout_ = new QVBoxLayout(tools_allowed_host_);
    tools_allowed_layout_->setContentsMargins(0, 0, 0, 0);
    tools_allowed_layout_->setSpacing(t.spacing.xs);
    tools_layout->addWidget(tools_allowed_host_);
    auto* add_allowed_row = new QHBoxLayout();
    new_allowed_edit_ = new QLineEdit(tools_body);
    new_allowed_edit_->setPlaceholderText(QStringLiteral("tool name"));
    auto* add_allowed = new QPushButton(QStringLiteral("Add"), tools_body);
    add_allowed_row->addWidget(new_allowed_edit_, 1);
    add_allowed_row->addWidget(add_allowed);
    tools_layout->addLayout(add_allowed_row);
    auto* denied_label = new QLabel(QStringLiteral("Tools denied"), tools_body);
    denied_label->setFont(theme::fonts::bodyEm());
    tools_layout->addWidget(denied_label);
    tools_denied_host_ = new QWidget(tools_body);
    tools_denied_layout_ = new QVBoxLayout(tools_denied_host_);
    tools_denied_layout_->setContentsMargins(0, 0, 0, 0);
    tools_denied_layout_->setSpacing(t.spacing.xs);
    tools_layout->addWidget(tools_denied_host_);
    auto* add_denied_row = new QHBoxLayout();
    new_denied_edit_ = new QLineEdit(tools_body);
    new_denied_edit_->setPlaceholderText(QStringLiteral("tool name"));
    auto* add_denied = new QPushButton(QStringLiteral("Add"), tools_body);
    add_denied_row->addWidget(new_denied_edit_, 1);
    add_denied_row->addWidget(add_denied);
    tools_layout->addLayout(add_denied_row);
    editor->addWidget(tools_body);
    tools_header->setActionLabel(QStringLiteral("collapse"));
    connect(tools_header, &widgets::AidaSectionHeader::actionTriggered, this,
            [this, tools_header, tools_body] {
        const bool show = !tools_body->isVisible();
        tools_body->setVisible(show);
        tools_header->setActionLabel(show ? QStringLiteral("collapse")
                                          : QStringLiteral("expand"));
    });

    auto* footer = new QHBoxLayout();
    duplicate_button_ = new QPushButton(QStringLiteral("Duplicate as custom"), editor_root_);
    duplicate_button_->setToolTip(QStringLiteral(
        "Create an editable custom copy of this built-in agent"));
    save_button_ = new QPushButton(QStringLiteral("Save"), editor_root_);
    save_button_->setToolTip(QStringLiteral("Save changes to this custom agent"));
    reset_button_ = new QPushButton(QStringLiteral("Reset"), editor_root_);
    reset_button_->setToolTip(QStringLiteral("Discard unsaved editor changes"));
    delete_button_ = new QPushButton(QStringLiteral("Delete"), editor_root_);
    delete_button_->setToolTip(QStringLiteral(
        "Delete this custom agent after a review confirmation"));
    footer->addWidget(duplicate_button_);
    footer->addStretch(1);
    footer->addWidget(save_button_);
    footer->addWidget(reset_button_);
    footer->addWidget(delete_button_);
    editor->addLayout(footer);
    editor->addStretch(1);

    editor_scroll_->setWidget(editor_root_);
    splitter_->addWidget(editor_scroll_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(500);

    connect(reload_button_, &QPushButton::clicked, this, [] {
        std::string error;
        if (!aida::agent_manager_service::request_reload(&error))
            chrome::toast_error(QString::fromStdString(error), 5.0);
    });
    connect(filter_edit_, &QLineEdit::textChanged, list_model_,
            &AidaAgentListModel::setFilter);
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid())
            return;
        const QString name = list_model_->nameAt(current.row());
        if (name.isEmpty() || name.toStdString() == selected_name_)
            return;
        if (dirty_)
            chrome::toast_warning(QStringLiteral("Discarded unsaved changes"), 3.5);
        selected_name_ = name.toStdString();
        loadEditorsForSelection();
    });
    connect(list_, &QListView::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = list_->indexAt(pos);
        if (index.isValid())
            openContextMenu(index, list_->viewport()->mapToGlobal(pos));
    });
    connect(name_edit_, &QLineEdit::textChanged, this, &AidaAgentManagerView::markDirty);
    connect(description_edit_, &QPlainTextEdit::textChanged, this,
            &AidaAgentManagerView::markDirty);
    connect(color_edit_, &QLineEdit::textChanged, this, &AidaAgentManagerView::markDirty);
    connect(mode_combo_, &QComboBox::currentIndexChanged, this,
            &AidaAgentManagerView::markDirty);
    connect(system_prompt_edit_, &QPlainTextEdit::textChanged, this,
            &AidaAgentManagerView::markDirty);
    connect(provider_combo_, &QComboBox::currentIndexChanged, this,
            &AidaAgentManagerView::markDirty);
    connect(model_combo_, &QComboBox::currentIndexChanged, this,
            &AidaAgentManagerView::markDirty);
    connect(temperature_spin_, &QDoubleSpinBox::valueChanged, this,
            &AidaAgentManagerView::markDirty);
    connect(top_p_spin_, &QDoubleSpinBox::valueChanged, this,
            &AidaAgentManagerView::markDirty);
    connect(max_steps_spin_, &QSpinBox::valueChanged, this, &AidaAgentManagerView::markDirty);
    connect(rules_model_, &AidaAgentRulesModel::rulesEdited, this,
            &AidaAgentManagerView::markDirty);
    connect(color_swatch_, &QPushButton::clicked, this, [this] {
        const QColor initial = QColor::fromString(color_edit_->text()).isValid()
            ? QColor::fromString(color_edit_->text()) : theme::tokens().accent;
        const QColor chosen = QColorDialog::getColor(initial, this,
            QStringLiteral("Agent color"));
        if (chosen.isValid()) {
            color_edit_->setText(chosen.name(QColor::HexRgb));
            markDirty();
        }
    });
    connect(add_rule, &QPushButton::clicked, this, [this] {
        const std::string key = rule_key_edit_->text().trimmed().toStdString();
        if (key.empty())
            return;
        rules_model_->appendRule(key, rule_pattern_edit_->text().trimmed().toStdString(),
            action_from_int(rule_action_combo_->currentIndex()));
        rule_key_edit_->clear();
        rule_pattern_edit_->clear();
        rule_action_combo_->setCurrentIndex(2);
    });
    connect(remove_rule, &QPushButton::clicked, this, [this] {
        const QModelIndex current = rules_table_->currentIndex();
        if (current.isValid())
            rules_model_->removeRule(current.row());
    });
    connect(add_allowed, &QPushButton::clicked, this, [this] {
        const std::string tool = new_allowed_edit_->text().trimmed().toStdString();
        if (tool.empty())
            return;
        tools_allowed_.push_back(tool);
        new_allowed_edit_->clear();
        rebuildToolChips();
        markDirty();
    });
    connect(add_denied, &QPushButton::clicked, this, [this] {
        const std::string tool = new_denied_edit_->text().trimmed().toStdString();
        if (tool.empty())
            return;
        tools_denied_.push_back(tool);
        new_denied_edit_->clear();
        rebuildToolChips();
        markDirty();
    });
    connect(save_button_, &QPushButton::clicked, this, &AidaAgentManagerView::onSave);
    connect(reset_button_, &QPushButton::clicked, this, &AidaAgentManagerView::onReset);
    connect(delete_button_, &QPushButton::clicked, this, &AidaAgentManagerView::onDelete);
    connect(duplicate_button_, &QPushButton::clicked, this,
            &AidaAgentManagerView::onDuplicateAsCustom);
    connect(poll_timer_, &QTimer::timeout, this, &AidaAgentManagerView::pollService);
}

void AidaAgentManagerView::wireService() {
    sub_changed_ = aida::events::subscribe(
        aida::events::event_agent_changed,
        std::function<void(const aida::events::agent_changed_t&)>(
            [this](const aida::events::agent_changed_t&) {
                QMetaObject::invokeMethod(this, [this] { pollService(); },
                    Qt::QueuedConnection);
            }));
    aida::agent_manager_service::begin_frame();
    applyPublication(aida::agent_manager_service::snapshot());
    if (selected_name_.empty()) {
        selected_name_ = aida::agent::active_agent_name();
        if (selected_name_.empty())
            selected_name_ = aida::agent::default_agent_name();
    }
    loadEditorsForSelection();
}

void AidaAgentManagerView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    pollService();
    if (!poll_timer_->isActive())
        poll_timer_->start();
}

void AidaAgentManagerView::hideEvent(QHideEvent* event) {
    poll_timer_->stop();
    QWidget::hideEvent(event);
}

void AidaAgentManagerView::selectAgent(const QString& name) {
    if (name.isEmpty())
        return;
    selected_name_ = name.toStdString();
    loadEditorsForSelection();
    for (int row = 0; row < list_model_->rowCount(); ++row) {
        if (list_model_->nameAt(row) == name) {
            list_->setCurrentIndex(list_model_->index(row));
            break;
        }
    }
}

void AidaAgentManagerView::pollService() {
    aida::agent_manager_service::begin_frame();
    applyPublication(aida::agent_manager_service::snapshot());
}

void AidaAgentManagerView::applyPublication(
    const aida::agent_manager_service::snapshot_ptr& publication) {
    if (!publication || publication->generation == observed_service_generation_)
        return;
    observed_service_generation_ = publication->generation;
    const bool operation_pending = publication->state ==
        aida::agent_manager_service::operation_state_t::loading;
    reload_button_->setEnabled(!operation_pending);
    if (operation_pending) {
        status_label_->setText(QString::fromStdString(publication->operation));
        status_label_->setVisible(true);
    }
    if (publication->state == aida::agent_manager_service::operation_state_t::succeeded) {
        const bool selection_applied = !pending_selection_.empty();
        bool selection_invalidated = false;
        if (!pending_selection_.empty() &&
            aida::agent_manager_service::find(publication, pending_selection_))
            selected_name_ = pending_selection_;
        pending_selection_.clear();
        if (!selected_name_.empty() &&
            !aida::agent_manager_service::find(publication, selected_name_)) {
            selection_invalidated = true;
            selected_name_ = aida::agent::default_agent_name();
            if (!aida::agent_manager_service::find(publication, selected_name_) &&
                !publication->agents.empty())
                selected_name_ = publication->agents.front().name;
        }
        err_.clear();
        status_label_->setVisible(false);
        if (!dirty_ || selection_applied || selection_invalidated)
            loadEditorsForSelection();
        if (!publication->operation.empty())
            chrome::toast_info(QString::fromStdString(publication->detail), 3.5);
    } else if (publication->state == aida::agent_manager_service::operation_state_t::failed) {
        pending_selection_.clear();
        err_ = publication->detail;
        status_label_->setText(QString::fromStdString(err_));
        status_label_->setVisible(true);
        chrome::toast_error(QString::fromStdString(publication->detail), 5.0);
    }
    list_model_->reloadFrom(publication);
    refreshDetailHeader();
}

void AidaAgentManagerView::loadEditorsForSelection() {
    loading_editors_ = true;
    dirty_ = false;
    rules_model_->setRules({});
    tools_allowed_.clear();
    tools_denied_.clear();
    preserved_options_ = nlohmann::json::object();
    name_edit_->clear();
    description_edit_->clear();
    color_edit_->clear();
    system_prompt_edit_->clear();
    provider_combo_->setCurrentIndex(0);
    model_combo_->clear();
    temperature_spin_->setValue(1.0);
    top_p_spin_->setValue(1.0);
    max_steps_spin_->setValue(0);
    mode_combo_->setCurrentIndex(0);

    const auto publication = aida::agent_manager_service::snapshot();
    const aida::agent::agent_info_t* info = selected_name_.empty()
        ? nullptr : aida::agent_manager_service::find(publication, selected_name_);
    if (info != nullptr) {
        preserved_options_ = info->options;
        name_edit_->setText(QString::fromStdString(info->name));
        description_edit_->setPlainText(QString::fromStdString(info->description));
        color_edit_->setText(QString::fromStdString(info->color));
        system_prompt_edit_->setPlainText(QString::fromStdString(info->system_prompt));
        temperature_spin_->setValue(info->temperature);
        top_p_spin_->setValue(info->top_p);
        max_steps_spin_->setValue(info->max_steps);
        mode_combo_->setCurrentIndex(mode_from_enum(info->mode));
        provider_combo_->clear();
        provider_combo_->addItem(QStringLiteral("(default provider)"), QString());
        const auto& providers = aida::provider::catalog::list_providers();
        for (const auto& provider : providers)
            provider_combo_->addItem(QString::fromStdString(provider.id),
                QString::fromStdString(provider.id));
        QString override_provider;
        QString override_model;
        if (info->model_override.has_value()) {
            override_provider = QString::fromStdString(info->model_override->provider_id);
            override_model = QString::fromStdString(info->model_override->model_id);
        }
        const int provider_index = provider_combo_->findData(override_provider);
        provider_combo_->setCurrentIndex(provider_index >= 0 ? provider_index : 0);
        model_combo_->clear();
        model_combo_->addItem(QStringLiteral("(provider default)"), QString());
        if (const auto* provider = aida::provider::catalog::get_provider(
                provider_combo_->currentData().toString().toStdString())) {
            for (const auto& model_id : provider->model_ids)
                model_combo_->addItem(QString::fromStdString(model_id),
                    QString::fromStdString(model_id));
        }
        const int model_index = model_combo_->findData(override_model);
        model_combo_->setCurrentIndex(model_index >= 0 ? model_index : 0);
        rules_model_->setRules(info->permissions);
        tools_allowed_ = info->tools_allowed;
        tools_denied_ = info->tools_denied;
    }
    loading_editors_ = false;
    rebuildToolChips();
    updateNativeBanner();
    refreshDetailHeader();
}

aida::agent::agent_info_t AidaAgentManagerView::buildInfoFromEditors(bool keep_native) const {
    aida::agent::agent_info_t info;
    info.name = name_edit_->text().toStdString();
    info.description = description_edit_->toPlainText().toStdString();
    info.color = color_edit_->text().toStdString();
    info.system_prompt = system_prompt_edit_->toPlainText().toStdString();
    info.temperature = temperature_spin_->value();
    info.top_p = top_p_spin_->value();
    info.max_steps = max_steps_spin_->value();
    info.mode = mode_from_int(mode_combo_->currentIndex());
    info.native = keep_native;
    info.hidden = false;
    const QString provider_id = provider_combo_->currentData().toString();
    const QString model_id = model_combo_->currentData().toString();
    if (!provider_id.isEmpty() || !model_id.isEmpty()) {
        aida::agent::agent_model_override_t model_override;
        model_override.provider_id = provider_id.toStdString();
        model_override.model_id = model_id.toStdString();
        info.model_override = model_override;
    }
    info.permissions = rules_model_->rules();
    info.tools_allowed = tools_allowed_;
    info.tools_denied = tools_denied_;
    info.options = preserved_options_;
    return info;
}

void AidaAgentManagerView::markDirty() {
    if (loading_editors_)
        return;
    dirty_ = true;
    refreshDetailHeader();
}

void AidaAgentManagerView::refreshDetailHeader() {
    detail_title_->setText(selected_name_.empty()
        ? QStringLiteral("No agent selected")
        : QString::fromStdString(selected_name_));
    const auto publication = aida::agent_manager_service::snapshot();
    const auto* info = selected_name_.empty()
        ? nullptr : aida::agent_manager_service::find(publication, selected_name_);
    const bool is_native = info != nullptr && info->native;
    QStringList badges;
    badges << (is_native ? QStringLiteral("native") : QStringLiteral("custom"));
    badges << (dirty_ ? QStringLiteral("Unsaved changes") : QStringLiteral("Up to date"));
    detail_badges_->setText(badges.join(QStringLiteral("  ·  ")));
    save_button_->setEnabled(info != nullptr && !is_native);
    reset_button_->setEnabled(info != nullptr);
    delete_button_->setEnabled(info != nullptr && !is_native);
    duplicate_button_->setVisible(info != nullptr && is_native);
    save_button_->setVisible(info == nullptr || !is_native);
    delete_button_->setVisible(info == nullptr || !is_native);
}

void AidaAgentManagerView::updateNativeBanner() {
    const auto publication = aida::agent_manager_service::snapshot();
    const auto* info = selected_name_.empty()
        ? nullptr : aida::agent_manager_service::find(publication, selected_name_);
    const bool is_native = info != nullptr && info->native;
    native_banner_->setVisible(is_native);
    setEditorsReadOnly(is_native);
}

void AidaAgentManagerView::setEditorsReadOnly(bool read_only) {
    name_edit_->setReadOnly(read_only);
    description_edit_->setReadOnly(read_only);
    color_edit_->setReadOnly(read_only);
    color_swatch_->setEnabled(!read_only);
    mode_combo_->setEnabled(!read_only);
    system_prompt_edit_->setReadOnly(read_only);
    provider_combo_->setEnabled(!read_only);
    model_combo_->setEnabled(!read_only);
    temperature_spin_->setEnabled(!read_only);
    top_p_spin_->setEnabled(!read_only);
    max_steps_spin_->setEnabled(!read_only);
    rules_table_->setEnabled(!read_only);
    rule_key_edit_->setEnabled(!read_only);
    rule_pattern_edit_->setEnabled(!read_only);
    rule_action_combo_->setEnabled(!read_only);
    new_allowed_edit_->setEnabled(!read_only);
    new_denied_edit_->setEnabled(!read_only);
}

void AidaAgentManagerView::rebuildToolChips() {
    const auto rebuild = [](QVBoxLayout* layout, std::vector<std::string>& tools,
                            bool enabled, std::function<void()> on_change) {
        while (QLayoutItem* item = layout->takeAt(0)) {
            if (QWidget* widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        for (std::size_t i = 0; i < tools.size(); ++i) {
            auto* chip = new widgets::AidaChip(QString::fromStdString(tools[i]));
            chip->setRemovable(enabled);
            const std::size_t index = i;
            QObject::connect(chip, &widgets::AidaChip::removeRequested, chip,
                [&tools, index, on_change] {
                    if (index < tools.size()) {
                        tools.erase(tools.begin() + static_cast<std::ptrdiff_t>(index));
                        on_change();
                    }
                });
            layout->addWidget(chip);
        }
    };
    const auto publication = aida::agent_manager_service::snapshot();
    const auto* info = selected_name_.empty()
        ? nullptr : aida::agent_manager_service::find(publication, selected_name_);
    const bool is_native = info != nullptr && info->native;
    rebuild(tools_allowed_layout_, tools_allowed_, !is_native, [this] {
        rebuildToolChips();
        markDirty();
    });
    rebuild(tools_denied_layout_, tools_denied_, !is_native, [this] {
        rebuildToolChips();
        markDirty();
    });
}

void AidaAgentManagerView::openContextMenu(const QModelIndex& index, const QPoint& global_pos) {
    const auto publication = aida::agent_manager_service::snapshot();
    const auto* info = list_model_->agentAt(index.row());
    if (info == nullptr)
        return;
    const std::string retained_name = info->name;
    const std::string retained_description = info->description;
    const std::uint64_t retained_generation = publication ? publication->catalog_generation : 0;

    auto* menu = new QMenu(this);
    auto* set_active = menu->addAction(QStringLiteral("Set active"));
    auto* copy_name = menu->addAction(QStringLiteral("Copy name"));
    auto* copy_description = menu->addAction(QStringLiteral("Copy description"));
    copy_description->setEnabled(!retained_description.empty());
    if (copy_description->toolTip().isEmpty() && retained_description.empty())
        copy_description->setToolTip(QStringLiteral("This agent has no description"));
    auto* duplicate = menu->addAction(QStringLiteral("Duplicate as custom"));
    duplicate->setEnabled(publication != nullptr);
    if (publication == nullptr)
        duplicate->setToolTip(QStringLiteral("The agent catalog is unavailable"));

    const auto validate = [retained_name, retained_description, retained_generation]() -> bool {
        const auto live = aida::agent_manager_service::snapshot();
        const auto* current = aida::agent_manager_service::find(live, retained_name);
        return live && live->catalog_generation == retained_generation && current != nullptr &&
            current->description == retained_description;
    };
    connect(set_active, &QAction::triggered, this, [this, validate, retained_name] {
        if (!validate()) {
            chrome::toast_warning(QStringLiteral(
                "The agent catalog changed; select the agent again"), 4.0);
            return;
        }
        const std::string previous = aida::agent::active_agent_name();
        if (aida::agent::set_active_agent(retained_name))
            aida::events::publish(aida::events::event_agent_changed,
                aida::events::agent_changed_t{ std::string{}, previous, retained_name });
    });
    connect(copy_name, &QAction::triggered, this, [validate, retained_name] {
        if (!validate())
            return;
        clipboard::set_text(QString::fromStdString(retained_name));
    });
    connect(copy_description, &QAction::triggered, this, [validate, retained_description] {
        if (!validate() || retained_description.empty())
            return;
        clipboard::set_text(QString::fromStdString(retained_description));
    });
    connect(duplicate, &QAction::triggered, this, [this, validate, retained_name,
                                                  retained_generation] {
        if (!validate()) {
            chrome::toast_warning(QStringLiteral(
                "The agent catalog changed; select the agent again"), 4.0);
            return;
        }
        const std::string new_identity = retained_name + "-custom";
        std::string error;
        if (aida::agent_manager_service::request_duplicate(retained_name, new_identity,
                retained_generation, &error)) {
            pending_selection_ = new_identity;
        } else {
            chrome::toast_error(QString::fromStdString(error.empty()
                ? "Duplicate request was rejected" : error), 5.0);
        }
    });
    menu->popup(global_pos);
}

void AidaAgentManagerView::onSave() {
    const auto publication = aida::agent_manager_service::snapshot();
    const bool operation_pending = publication && publication->state ==
        aida::agent_manager_service::operation_state_t::loading;
    if (operation_pending || !publication)
        return;
    const QString trimmed = name_edit_->text().trimmed();
    if (trimmed.isEmpty()) {
        chrome::toast_error(QStringLiteral("Agent name cannot be empty"), 4.0);
        return;
    }
    aida::agent::agent_info_t info = buildInfoFromEditors(false);
    info.name = trimmed.toStdString();
    std::string error;
    if (aida::agent_manager_service::request_upsert(info, selected_name_,
            publication->catalog_generation, &error)) {
        pending_selection_ = info.name;
        dirty_ = false;
        refreshDetailHeader();
    } else {
        chrome::toast_error(QString::fromStdString(error.empty()
            ? "Save request was rejected" : error), 5.0);
    }
}

void AidaAgentManagerView::onReset() {
    loadEditorsForSelection();
    chrome::toast_info(QStringLiteral("Reverted unsaved changes"), 2.5);
}

void AidaAgentManagerView::onDelete() {
    const auto publication = aida::agent_manager_service::snapshot();
    const auto* selected = aida::agent_manager_service::find(publication, selected_name_);
    const std::uint64_t reviewed_generation = publication ? publication->catalog_generation : 0;
    const std::string pending_identity = selected_name_;
    const bool current = selected != nullptr && !selected->native;

    aida_confirm_request_t request;
    request.verb = QStringLiteral("Delete");
    request.target = pending_identity.empty()
        ? QStringLiteral("the selected custom agent")
        : QString::fromStdString(pending_identity);
    request.scope = QStringLiteral(
        "The exact custom agent definition and its persisted catalog entry");
    request.effect = QStringLiteral(
        "Removes this custom role from AiDA without changing native agents.");
    request.reversibility = QStringLiteral(
        "Recreate or reimport the custom definition to recover it.");
    request.prerequisite = current ? QString() : QStringLiteral(
        "The catalog or selected identity changed; close and review the deletion again.");
    request.confirm_label = QStringLiteral("Delete Agent");
    request.destructive = true;
    request.confirm_enabled = current;
    AidaConfirmDialog::request(request, this, [this, pending_identity, reviewed_generation] {
        std::string error;
        if (aida::agent_manager_service::request_delete(pending_identity,
                reviewed_generation, &error)) {
            pending_selection_ = aida::agent::default_agent_name();
        } else {
            chrome::toast_error(QString::fromStdString(error.empty()
                ? "Delete request was rejected" : error), 5.0);
        }
    });
}

void AidaAgentManagerView::onDuplicateAsCustom() {
    const auto publication = aida::agent_manager_service::snapshot();
    const bool operation_pending = publication && publication->state ==
        aida::agent_manager_service::operation_state_t::loading;
    if (operation_pending || !publication)
        return;
    aida::agent::agent_info_t copy = buildInfoFromEditors(false);
    copy.name = selected_name_ + "-custom";
    copy.native = false;
    std::string error;
    if (aida::agent_manager_service::request_upsert(copy, {},
            publication->catalog_generation, &error)) {
        pending_selection_ = copy.name;
    } else {
        chrome::toast_error(QString::fromStdString(error.empty()
            ? "Duplicate request was rejected" : error), 5.0);
    }
}

}
