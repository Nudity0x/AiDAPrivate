#include "qt/dialogs/aida_shortcuts_dialog.hpp"

#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QPushButton>
#include <QStyleOptionViewItem>
#include <QTableView>

#include <algorithm>
#include <cctype>

#include "helpers/diag_log.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/bridge/shortcut_bridge.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::dialogs {

namespace {

std::string lower_copy(const char* text)
{
    std::string out;
    if (!text)
        return out;
    out.reserve(std::char_traits<char>::length(text));
    for (const char* p = text; *p; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        out.push_back(c);
    }
    return out;
}

QRect chipRect(const QRect& cell, int index, const QFontMetricsF& fm)
{
    const auto& t = theme::tokens();
    static const char* k_labels[] = { "Edit", "Disable", "Reset" };
    qreal x = cell.right() - t.spacing.xs;
    for (int i = 2; i >= 0; --i) {
        const qreal w = fm.horizontalAdvance(QString::fromLatin1(k_labels[i])) +
            2.0 * t.spacing.sm;
        x -= w;
        if (i == index)
            return QRect(static_cast<int>(x), cell.top() + t.spacing.xxs,
                         static_cast<int>(w), cell.height() - 2 * t.spacing.xxs);
        x -= t.spacing.xs;
    }
    return {};
}

}

AidaShortcutTableModel::AidaShortcutTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int AidaShortcutTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int AidaShortcutTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant AidaShortcutTableModel::headerData(int section, Qt::Orientation orientation,
                                            int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ActionColumn: return QStringLiteral("Action");
    case BindingColumn: return QStringLiteral("Binding");
    case ScopeColumn: return QStringLiteral("Scope");
    case ButtonsColumn: return QString();
    }
    return {};
}

QVariant AidaShortcutTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(rows_.size()))
        return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    if (row.category) {
        if (role == Qt::DisplayRole && index.column() == ActionColumn)
            return QString::fromStdString(row.category_name);
        if (role == Qt::UserRole)
            return true;
        return {};
    }
    if (role == Qt::UserRole)
        return false;
    const auto& shortcut = shortcuts_[row.shortcut_index];
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case ActionColumn: return QString::fromStdString(shortcut.label);
        case BindingColumn: return QString::fromStdString(shortcut.shortcut);
        case ScopeColumn: {
            QString metadata = QString::fromStdString(shortcut.scope);
            if (!shortcut.binding_enabled)
                metadata += QStringLiteral(" / Disabled");
            if (shortcut.customized)
                metadata += QStringLiteral(" / Customized");
            if (shortcut.conflict)
                metadata += QStringLiteral(" / Conflict");
            return metadata;
        }
        case ButtonsColumn: break;
        }
        return {};
    case Qt::FontRole:
        if (index.column() == BindingColumn)
            return theme::fonts::codeRegular();
        if (row.category)
            return theme::fonts::strong();
        return {};
    case Qt::ToolTipRole:
        if (index.column() == ButtonsColumn)
            return shortcut.editable
                ? QStringLiteral("Enter or double-click to edit; Disable turns the binding off; Reset restores the default")
                : QVariant{};
        if (shortcut.customized)
            return QStringLiteral("Default: %1")
                .arg(QString::fromStdString(shortcut.default_shortcut));
        if (!shortcut.enabled && !shortcut.disabled_reason.empty())
            return QString::fromStdString(shortcut.disabled_reason);
        return {};
    case Qt::UserRole + 1:
        return shortcut.editable;
    case Qt::UserRole + 2:
        return shortcut.binding_enabled;
    case Qt::UserRole + 3:
        return shortcut.customized;
    case Qt::UserRole + 4:
        return shortcut.conflict;
    case Qt::UserRole + 5:
        return shortcut.enabled;
    }
    return {};
}

Qt::ItemFlags AidaShortcutTableModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags flags = QAbstractTableModel::flags(index);
    if (index.isValid() && isCategoryRow(index.row()))
        flags &= ~Qt::ItemIsSelectable;
    return flags;
}

void AidaShortcutTableModel::setShortcuts(
    std::vector<aida::ui::application_ui::shortcut_presentation_t> shortcuts)
{
    shortcuts_ = std::move(shortcuts);
    rebuildRows();
}

void AidaShortcutTableModel::setFilter(const QString& filter)
{
    filter_ = filter;
    rebuildRows();
}

void AidaShortcutTableModel::rebuildRows()
{
    beginResetModel();
    rows_.clear();
    visible_count_ = 0;
    const std::string filter_lower = lower_copy(filter_.toUtf8().constData());
    const bool has_filter = !filter_lower.empty();
    std::string active_category;
    for (std::size_t index = 0; index < shortcuts_.size(); ++index) {
        const auto& shortcut = shortcuts_[index];
        if (has_filter) {
            const std::string category_lower = lower_copy(shortcut.category.c_str());
            const std::string keys_lower = lower_copy(shortcut.shortcut.c_str());
            const std::string label_lower = lower_copy(shortcut.label.c_str());
            const std::string scope_lower = lower_copy(shortcut.scope.c_str());
            const bool visible =
                category_lower.find(filter_lower) != std::string::npos ||
                keys_lower.find(filter_lower) != std::string::npos ||
                label_lower.find(filter_lower) != std::string::npos ||
                scope_lower.find(filter_lower) != std::string::npos;
            if (!visible)
                continue;
        }
        ++visible_count_;
        if (active_category != shortcut.category) {
            active_category = shortcut.category;
            rows_.push_back(row_t{true, active_category, 0});
        }
        rows_.push_back(row_t{false, {}, index});
    }
    endResetModel();
}

bool AidaShortcutTableModel::isCategoryRow(int row) const
{
    return row >= 0 && row < static_cast<int>(rows_.size()) &&
        rows_[static_cast<std::size_t>(row)].category;
}

const aida::ui::application_ui::shortcut_presentation_t* AidaShortcutTableModel::shortcutAt(
    int row) const
{
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    const auto& entry = rows_[static_cast<std::size_t>(row)];
    if (entry.category)
        return nullptr;
    return &shortcuts_[entry.shortcut_index];
}

AidaShortcutButtonsDelegate::AidaShortcutButtonsDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

QSize AidaShortcutButtonsDelegate::sizeHint(const QStyleOptionViewItem& option,
                                            const QModelIndex& index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    const auto& t = theme::tokens();
    const QFontMetricsF fm(theme::fonts::caption());
    static const char* k_labels[] = { "Edit", "Disable", "Reset" };
    qreal width = 2.0 * t.spacing.xs;
    for (const char* label : k_labels)
        width += fm.horizontalAdvance(QString::fromLatin1(label)) + 2.0 * t.spacing.sm +
            t.spacing.xs;
    return QSize(static_cast<int>(width + 0.5), t.table.row_h);
}

void AidaShortcutButtonsDelegate::paint(QPainter* painter,
                                        const QStyleOptionViewItem& option,
                                        const QModelIndex& index) const
{
    const bool is_category = index.data(Qt::UserRole).toBool();
    if (is_category)
        return;
    const bool editable = index.data(Qt::UserRole + 1).toBool();
    if (!editable)
        return;
    const bool binding_enabled = index.data(Qt::UserRole + 2).toBool();
    const bool customized = index.data(Qt::UserRole + 3).toBool();
    const auto& t = theme::tokens();
    const QFont font = theme::fonts::caption();
    const QFontMetricsF fm(font);
    painter->save();
    painter->setFont(font);
    painter->setRenderHint(QPainter::Antialiasing, true);
    static const char* k_labels[] = { "Edit", "Disable", "Reset" };
    const bool show[3] = { true, binding_enabled, customized };
    for (int i = 0; i < 3; ++i) {
        if (!show[i])
            continue;
        const QRect chip = chipRect(option.rect, i, fm);
        painter->setPen(QPen(t.border_subtle, 1));
        painter->setBrush(widgets::with_alpha(t.panel_header, 0.9));
        painter->drawRoundedRect(chip, t.radius.sm, t.radius.sm);
        painter->setPen(t.text_secondary);
        painter->drawText(chip, Qt::AlignCenter, QString::fromLatin1(k_labels[i]));
    }
    painter->restore();
}

bool AidaShortcutButtonsDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                              const QStyleOptionViewItem& option,
                                              const QModelIndex& index)
{
    if (event->type() != QEvent::MouseButtonRelease)
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton)
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    const bool editable = index.data(Qt::UserRole + 1).toBool();
    if (!editable)
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    const bool binding_enabled = index.data(Qt::UserRole + 2).toBool();
    const bool customized = index.data(Qt::UserRole + 3).toBool();
    const bool show[3] = { true, binding_enabled, customized };
    const QFontMetricsF fm(theme::fonts::caption());
    for (int i = 0; i < 3; ++i) {
        if (!show[i])
            continue;
        if (chipRect(option.rect, i, fm).contains(mouse->pos())) {
            if (i == 0)
                Q_EMIT editClicked(index.row());
            else if (i == 1)
                Q_EMIT disableClicked(index.row());
            else
                Q_EMIT resetClicked(index.row());
            return true;
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

AidaShortcutsDialog::AidaShortcutsDialog(bridge::ShortcutBridge* shortcuts, QWidget* parent)
    : bridge::AidaDialog(parent), shortcuts_(shortcuts)
{
    setObjectName(QStringLiteral("aida.shortcuts"));
    setWindowTitle(QStringLiteral("Keyboard Shortcuts"));
    setModal(false);

    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
                             t.panel.padding);
    root->setSpacing(t.spacing.sm);

    auto* title = new QLabel(QStringLiteral("Keyboard Shortcuts"), this);
    title->setFont(theme::fonts::strong());
    root->addWidget(title);
    auto* hint = new QLabel(QStringLiteral(
        "Type to filter. Enter or double-click edits the selected binding. Esc cancels the edit, then closes."), this);
    hint->setFont(theme::fonts::caption());
    hint->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(hint);

    filter_edit_ = new widgets::AidaLineEdit(QStringLiteral("Search shortcuts..."), this);
    filter_edit_->setObjectName(QStringLiteral("aida.shortcuts.filter"));
    filter_edit_->setClearButtonEnabled(true);
    bridge::InteractionContextProvider::mark_text_input(filter_edit_);
    connect(filter_edit_, &QLineEdit::textChanged, this, [this](const QString&) {
        refreshFilter();
    });
    root->addWidget(filter_edit_);

    auto* reset_row = new QHBoxLayout();
    reset_all_button_ = new QPushButton(QStringLiteral("Reset all shortcuts"), this);
    reset_all_button_->setObjectName(QStringLiteral("aida.shortcuts.reset_all"));
    connect(reset_all_button_, &QPushButton::clicked, this, [this] {
        reset_all_armed_ = true;
        reset_all_button_->setVisible(false);
        reset_all_confirm_->setVisible(true);
        reset_all_cancel_->setVisible(true);
        status_label_->setText(QStringLiteral("Confirm to restore every canonical default binding"));
    });
    reset_row->addWidget(reset_all_button_);
    reset_all_confirm_ = new QPushButton(QStringLiteral("Confirm reset all"), this);
    reset_all_confirm_->setObjectName(QStringLiteral("aida.shortcuts.reset_all.confirm"));
    reset_all_confirm_->setVisible(false);
    connect(reset_all_confirm_, &QPushButton::clicked, this, [this] {
        const auto result = aida::ui::application_ui::reset_all_shortcut_overrides();
        status_label_->setText(QString::fromStdString(result.detail));
        reset_all_armed_ = false;
        reset_all_button_->setVisible(true);
        reset_all_confirm_->setVisible(false);
        reset_all_cancel_->setVisible(false);
        cancelEdit(QString());
        reload();
    });
    reset_row->addWidget(reset_all_confirm_);
    reset_all_cancel_ = new QPushButton(QStringLiteral("Cancel reset"), this);
    reset_all_cancel_->setObjectName(QStringLiteral("aida.shortcuts.reset_all.cancel"));
    reset_all_cancel_->setVisible(false);
    connect(reset_all_cancel_, &QPushButton::clicked, this, [this] {
        reset_all_armed_ = false;
        reset_all_button_->setVisible(true);
        reset_all_confirm_->setVisible(false);
        reset_all_cancel_->setVisible(false);
        status_label_->setText(QStringLiteral("Reset cancelled"));
    });
    reset_row->addWidget(reset_all_cancel_);
    reset_row->addStretch(1);
    root->addLayout(reset_row);

    edit_panel_ = new QWidget(this);
    edit_panel_->setObjectName(QStringLiteral("aida.shortcuts.edit_panel"));
    auto* edit_layout = new QVBoxLayout(edit_panel_);
    edit_layout->setContentsMargins(0, t.spacing.xs, 0, t.spacing.xs);
    edit_layout->setSpacing(t.spacing.xs);
    edit_title_ = new QLabel(edit_panel_);
    edit_title_->setFont(theme::fonts::bodyEm());
    edit_layout->addWidget(edit_title_);
    recorder_ = new bridge::ShortcutRecorderWidget(shortcuts_, edit_panel_);
    recorder_->setObjectName(QStringLiteral("aida.shortcuts.recorder"));
    connect(recorder_, &bridge::ShortcutRecorderWidget::capture_started, this, [this] {
        setCaptureGates(true);
    });
    connect(recorder_, &bridge::ShortcutRecorderWidget::capture_finished, this, [this] {
        setCaptureGates(false);
        onCaptureFinished();
    });
    edit_layout->addWidget(recorder_);
    edit_draft_ = new QLabel(edit_panel_);
    edit_draft_->setObjectName(QStringLiteral("aida.shortcuts.edit_draft"));
    edit_draft_->setFont(theme::fonts::codeRegular());
    edit_layout->addWidget(edit_draft_);
    conflict_label_ = new QLabel(edit_panel_);
    conflict_label_->setObjectName(QStringLiteral("aida.shortcuts.conflict"));
    conflict_label_->setFont(theme::fonts::caption());
    conflict_label_->setWordWrap(true);
    conflict_label_->setProperty("aidaState", QStringLiteral("stale"));
    conflict_label_->setVisible(false);
    edit_layout->addWidget(conflict_label_);
    auto* edit_buttons = new QHBoxLayout();
    apply_button_ = new QPushButton(QStringLiteral("Apply binding"), edit_panel_);
    apply_button_->setObjectName(QStringLiteral("aida.shortcuts.apply"));
    connect(apply_button_, &QPushButton::clicked, this, [this] { applyEdit(false); });
    edit_buttons->addWidget(apply_button_);
    replace_button_ = new QPushButton(QStringLiteral("Replace conflicting bindings"), edit_panel_);
    replace_button_->setObjectName(QStringLiteral("aida.shortcuts.replace_conflicts"));
    replace_button_->setVisible(false);
    connect(replace_button_, &QPushButton::clicked, this, [this] { applyEdit(true); });
    edit_buttons->addWidget(replace_button_);
    cancel_edit_button_ = new QPushButton(QStringLiteral("Cancel edit"), edit_panel_);
    cancel_edit_button_->setObjectName(QStringLiteral("aida.shortcuts.cancel_edit"));
    connect(cancel_edit_button_, &QPushButton::clicked, this, [this] {
        cancelEdit(QStringLiteral("Edit cancelled"));
    });
    edit_buttons->addWidget(cancel_edit_button_);
    edit_buttons->addStretch(1);
    edit_layout->addLayout(edit_buttons);
    edit_panel_->setVisible(false);
    root->addWidget(edit_panel_);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.shortcuts.status"));
    status_label_->setFont(theme::fonts::caption());
    status_label_->setWordWrap(true);
    status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(status_label_);

    model_ = new AidaShortcutTableModel(this);
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("aida.shortcuts.table"));
    table_->setModel(model_);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(AidaShortcutTableModel::ActionColumn,
        QHeaderView::Stretch);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    buttons_delegate_ = new AidaShortcutButtonsDelegate(this);
    table_->setItemDelegateForColumn(AidaShortcutTableModel::ButtonsColumn, buttons_delegate_);
    table_->horizontalHeader()->setSectionResizeMode(AidaShortcutTableModel::BindingColumn,
        QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(AidaShortcutTableModel::ScopeColumn,
        QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(AidaShortcutTableModel::ButtonsColumn,
        QHeaderView::ResizeToContents);
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!index.isValid() || model_->isCategoryRow(index.row()))
            return;
        beginEdit(index.row());
    });
    connect(buttons_delegate_, &AidaShortcutButtonsDelegate::editClicked, this,
            [this](int row) { beginEdit(row); });
    connect(buttons_delegate_, &AidaShortcutButtonsDelegate::disableClicked, this,
            [this](int row) {
        const auto* shortcut = model_->shortcutAt(row);
        if (!shortcut)
            return;
        const auto result = aida::ui::application_ui::disable_shortcut_override(
            shortcut->binding_id.c_str());
        status_label_->setText(QString::fromStdString(result.detail));
        cancelEdit(QString());
        reload();
    });
    connect(buttons_delegate_, &AidaShortcutButtonsDelegate::resetClicked, this,
            [this](int row) {
        const auto* shortcut = model_->shortcutAt(row);
        if (!shortcut)
            return;
        const auto result = aida::ui::application_ui::reset_shortcut_override(
            shortcut->binding_id.c_str());
        status_label_->setText(QString::fromStdString(result.detail));
        cancelEdit(QString());
        reload();
    });
    root->addWidget(table_, 1);

    auto* footer = new QHBoxLayout();
    footer->addStretch(1);
    auto* close_button = new QPushButton(QStringLiteral("Close"), this);
    close_button->setObjectName(QStringLiteral("aida.shortcuts.close"));
    connect(close_button, &QPushButton::clicked, this, [this] { reject(); });
    footer->addWidget(close_button);
    root->addLayout(footer);

    setMinimumSize(560, 420);
    resize(760, 620);
}

AidaShortcutsDialog::~AidaShortcutsDialog() = default;

void AidaShortcutsDialog::openFresh()
{
    filter_edit_->clear();
    cancelEdit(QString());
    reset_all_armed_ = false;
    reset_all_button_->setVisible(true);
    reset_all_confirm_->setVisible(false);
    reset_all_cancel_->setVisible(false);
    status_label_->clear();
    reload();
    diag::log_tagged("chrome", "shortcuts_popup open=true source=qt");
    open();
    filter_edit_->setFocus(Qt::OtherFocusReason);
}

void AidaShortcutsDialog::reload()
{
    model_->setShortcuts(aida::ui::application_ui::list_shortcuts());
    refreshFilter();
}

void AidaShortcutsDialog::refreshFilter()
{
    const QString filter = filter_edit_->text().trimmed();
    model_->setFilter(filter);
    if (filter.isEmpty()) {
        if (filter_status_active_) {
            filter_status_active_ = false;
            status_label_->clear();
        }
        return;
    }
    filter_status_active_ = true;
    status_label_->setText(model_->visibleCount() == 0
        ? QStringLiteral("No bindings match the filter.")
        : QStringLiteral("%1 matching binding%2")
            .arg(model_->visibleCount())
            .arg(model_->visibleCount() == 1 ? QString() : QStringLiteral("s")));
}

void AidaShortcutsDialog::beginEdit(int row)
{
    const auto* shortcut = model_->shortcutAt(row);
    if (!shortcut)
        return;
    edit_binding_ = QString::fromStdString(shortcut->binding_id);
    edit_label_ = QString::fromStdString(shortcut->label);
    edit_strokes_.clear();
    edit_conflicts_.clear();
    recorder_->clear();
    edit_title_->setText(QStringLiteral("Editing %1").arg(edit_label_));
    status_label_->setText(QStringLiteral("Press one or more key combinations"));
    reset_all_armed_ = false;
    reset_all_button_->setVisible(true);
    reset_all_confirm_->setVisible(false);
    reset_all_cancel_->setVisible(false);
    edit_panel_->setVisible(true);
    updateCaptureUi();
    recorder_->setFocus(Qt::OtherFocusReason);
}

void AidaShortcutsDialog::cancelEdit(const QString& status)
{
    if (!edit_binding_.isEmpty() || edit_panel_->isVisible()) {
        edit_binding_.clear();
        edit_label_.clear();
        edit_strokes_.clear();
        edit_conflicts_.clear();
        edit_panel_->setVisible(false);
        setCaptureGates(false);
    }
    if (!status.isEmpty())
        status_label_->setText(status);
}

void AidaShortcutsDialog::onCaptureFinished()
{
    edit_strokes_ = recorder_->chords();
    edit_conflicts_.clear();
    updateCaptureUi();
}

void AidaShortcutsDialog::updateCaptureUi()
{
    const bool capturing = !edit_binding_.isEmpty();
    if (!capturing)
        return;
    const bool has_strokes = !edit_strokes_.empty();
    const QString draft = has_strokes
        ? QString::fromStdString(aida::ui::application_ui::format_shortcut_sequence(edit_strokes_))
        : QStringLiteral("Press a key combination");
    edit_draft_->setText(draft);
    apply_button_->setEnabled(has_strokes);
    replace_button_->setVisible(!edit_conflicts_.empty());
    if (edit_conflicts_.empty()) {
        conflict_label_->clear();
        conflict_label_->setVisible(false);
        return;
    }
    QString text = QStringLiteral("Conflict with %1 binding%2 in this focus scope\n")
        .arg(edit_conflicts_.size())
        .arg(edit_conflicts_.size() == 1 ? QString() : QStringLiteral("s"));
    const std::size_t shown = (std::min)(edit_conflicts_.size(), std::size_t{4});
    for (std::size_t index = 0; index < shown; ++index)
        text += QString::fromStdString(edit_conflicts_[index]) + QLatin1Char('\n');
    if (edit_conflicts_.size() > 4)
        text += QStringLiteral("+%1 more").arg(edit_conflicts_.size() - 4);
    conflict_label_->setText(text.trimmed());
    conflict_label_->setVisible(true);
}

void AidaShortcutsDialog::applyEdit(bool replace_conflicts)
{
    if (edit_binding_.isEmpty() || edit_strokes_.empty())
        return;
    const auto result = aida::ui::application_ui::update_shortcut_override(
        edit_binding_.toUtf8().constData(), edit_strokes_, replace_conflicts);
    status_label_->setText(QString::fromStdString(result.detail));
    if (result.completed()) {
        cancelEdit(QString());
        reload();
        return;
    }
    edit_conflicts_ = result.conflicts;
    updateCaptureUi();
}

void AidaShortcutsDialog::setCaptureGates(bool active)
{
    aida::ui::application_ui::set_shortcut_capture_active(active);
    if (shortcuts_)
        shortcuts_->set_capture_active(active);
}

void AidaShortcutsDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        if (!edit_binding_.isEmpty()) {
            cancelEdit(QStringLiteral("Edit cancelled"));
            event->accept();
            return;
        }
        reject();
        event->accept();
        return;
    }
    if (table_->hasFocus() &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
         event->key() == Qt::Key_F2)) {
        const int row = table_->currentIndex().row();
        if (model_->shortcutAt(row)) {
            beginEdit(row);
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Down && filter_edit_->hasFocus() &&
        model_->rowCount() > 0) {
        table_->setFocus(Qt::OtherFocusReason);
        if (!table_->currentIndex().isValid())
            table_->setCurrentIndex(model_->index(0, 0));
        event->accept();
        return;
    }
    bridge::AidaDialog::keyPressEvent(event);
}

}
