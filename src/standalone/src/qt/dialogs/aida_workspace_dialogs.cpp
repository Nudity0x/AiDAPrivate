#include "qt/dialogs/aida_workspace_dialogs.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/layout/workspace_persistence.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_line_edit.hpp"

namespace aida::qt::dialogs {

namespace {

class WorkspaceCatalogModel : public QAbstractListModel {
public:
    explicit WorkspaceCatalogModel(QObject* parent) : QAbstractListModel(parent) {}

    void setCatalog(std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>> catalog)
    {
        beginResetModel();
        catalog_ = std::move(catalog);
        endResetModel();
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : (catalog_ ? static_cast<int>(catalog_->size()) : 0);
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!catalog_ || !index.isValid() || index.row() < 0 ||
            index.row() >= static_cast<int>(catalog_->size()))
            return {};
        const auto& item = (*catalog_)[static_cast<std::size_t>(index.row())];
        if (role == Qt::DisplayRole) {
            QString name = QString::fromStdString(item.name);
            if (item.active)
                name += QStringLiteral("  [Active]");
            return name;
        }
        if (role == Qt::UserRole)
            return QString::fromStdString(item.name);
        if (role == Qt::UserRole + 1)
            return QVariant::fromValue<qulonglong>(item.generation);
        if (role == Qt::UserRole + 2)
            return item.active;
        return {};
    }

    const docking::user_workspace_descriptor_t* at(int row) const
    {
        if (!catalog_ || row < 0 || row >= static_cast<int>(catalog_->size()))
            return nullptr;
        return &(*catalog_)[static_cast<std::size_t>(row)];
    }

private:
    std::shared_ptr<const std::vector<docking::user_workspace_descriptor_t>> catalog_;
};

QString preset_display_name(docking::workspace_preset_t preset)
{
    const auto& descriptor = docking::preset_descriptor(preset);
    return QString::fromLatin1(descriptor.display_name.data(),
                               static_cast<qsizetype>(descriptor.display_name.size()));
}

}

std::string workspace_request_message(docking::workspace_request_result_t result,
                                      std::string_view operation)
{
    using result_t = docking::workspace_request_result_t;
    if (result == result_t::completed) return std::string(operation) + " completed.";
    if (result == result_t::queued) return std::string(operation) + " queued in Background Tasks.";
    if (result == result_t::unchanged) return std::string(operation) + " made no changes.";
    if (result == result_t::invalid_name)
        return "Use 1-64 ASCII letters, numbers, spaces, hyphens or underscores; spaces cannot lead, trail or repeat.";
    if (result == result_t::already_exists)
        return "A saved workspace with this exact name already exists.";
    if (result == result_t::not_found)
        return "The selected saved workspace no longer exists. The catalog will refresh automatically.";
    if (result == result_t::unavailable)
        return "This operation is unavailable until the DockSpace and persistence service are ready.";
    if (result == result_t::busy)
        return "Another workspace transaction is already running.";
    return std::string(operation) + " failed. Open Background Tasks or Diagnostics for the retained failure evidence.";
}

bool workspace_request_succeeded(docking::workspace_request_result_t result) noexcept
{
    using result_t = docking::workspace_request_result_t;
    return result == result_t::completed || result == result_t::queued ||
        result == result_t::unchanged;
}

namespace {

aida::ui::capability_state_t named_workspace_load_capability(
    const docking::user_workspace_descriptor_t& retained,
    layout::WorkspacePersistenceController* persistence)
{
    using aida::ui::capability_state_t;
    if (!persistence)
        return capability_state_t::unavailable("The workspace persistence service is not available");
    if (!persistence->user_layout_catalog_ready())
        return capability_state_t::unavailable("The saved workspace catalog is still loading");
    const auto catalog = persistence->user_layout_catalog();
    const auto current = catalog ? std::find_if(catalog->begin(), catalog->end(),
        [&](const auto& item) {
            return item.name == retained.name && item.generation == retained.generation;
        }) : std::vector<docking::user_workspace_descriptor_t>::const_iterator{};
    if (!catalog || current == catalog->end())
        return capability_state_t::unavailable(
            "This saved workspace changed after it was presented; reopen the catalog");
    if (current->active)
        return capability_state_t::unavailable("This saved workspace is already active");
    if (persistence->operation_pending()) {
        const std::string status = persistence->operation_status();
        return capability_state_t::unavailable(status.empty()
            ? "Another workspace transaction is already running" : status);
    }
    return capability_state_t::available();
}

aida::ui::application_ui::retained_entity_context_t named_workspace_load_context(
    const docking::user_workspace_descriptor_t& retained,
    layout::WorkspacePersistenceController* persistence)
{
    aida::ui::application_ui::retained_entity_context_t context;
    context.owner_id = "workspace.saved";
    context.entity_id = retained.name;
    context.entity_generation = retained.generation;
    context.validate_identity = [retained, persistence] {
        return named_workspace_load_capability(retained, persistence);
    };
    aida::ui::application_ui::retained_entity_action_t action;
    action.action_id = "workspace.load_named";
    action.capability = named_workspace_load_capability(retained, persistence);
    action.invoke = [retained, persistence] {
        const auto capability = named_workspace_load_capability(retained, persistence);
        if (!capability.enabled)
            return aida::ui::action_handler_result_t::failed(capability.disabled_reason);
        const auto result = persistence
            ? persistence->load_user_layout_exact(retained.name, retained.generation)
            : docking::workspace_request_result_t::unavailable;
        std::string detail = workspace_request_message(result, "Load workspace");
        if (result == docking::workspace_request_result_t::unavailable && persistence &&
            persistence->user_layout_catalog_ready()) {
            const auto catalog = persistence->user_layout_catalog();
            const bool exact_identity_exists = catalog && std::any_of(
                catalog->begin(), catalog->end(), [&](const auto& item) {
                    return item.name == retained.name && item.generation == retained.generation;
                });
            if (!exact_identity_exists)
                detail = "The selected saved workspace changed after it was presented. Reopen the catalog and select its current generation.";
        }
        return workspace_request_succeeded(result)
            ? aida::ui::action_handler_result_t::completed(detail)
            : aida::ui::action_handler_result_t::failed(detail);
    };
    context.actions.push_back(std::move(action));
    return context;
}

}

AidaWorkspaceSaveAsDialog::AidaWorkspaceSaveAsDialog(
    layout::WorkspacePersistenceController* persistence, AidaWorkspaceController* controller,
    QWidget* parent)
    : bridge::AidaDialog(parent), persistence_(persistence), controller_(controller)
{
    setObjectName(QStringLiteral("aida.workspace.save_as"));
    setWindowTitle(QStringLiteral("Save Workspace As"));
    setModal(false);

    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    root->setSpacing(t.spacing.sm);

    auto* caption = new QLabel(QStringLiteral("Create a named derivative of the current workspace"), this);
    caption->setFont(theme::fonts::body());
    root->addWidget(caption);

    base_label_ = new QLabel(this);
    base_label_->setFont(theme::fonts::caption());
    base_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(base_label_);

    name_edit_ = new widgets::AidaLineEdit(this);
    name_edit_->setObjectName(QStringLiteral("aida.workspace.save_as.name"));
    name_edit_->setMaxLength(64);
    name_edit_->setPlaceholderText(QStringLiteral("Workspace name"));
    bridge::InteractionContextProvider::mark_text_input(name_edit_);
    connect(name_edit_, &QLineEdit::textChanged, this, [this](const QString&) {
        refreshButtons();
    });
    root->addWidget(name_edit_);

    auto* rule = new QLabel(QStringLiteral("1-64 letters, numbers, spaces, hyphens or underscores"), this);
    rule->setFont(theme::fonts::caption());
    rule->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(rule);

    catalog_note_ = new QLabel(QStringLiteral("Loading saved workspace catalog..."), this);
    catalog_note_->setFont(theme::fonts::caption());
    catalog_note_->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(catalog_note_);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.workspace.save_as.status"));
    status_label_->setFont(theme::fonts::caption());
    status_label_->setWordWrap(true);
    root->addWidget(status_label_);

    overwrite_section_ = new QWidget(this);
    overwrite_section_->setObjectName(QStringLiteral("aida.workspace.save_as.overwrite"));
    auto* overwrite_layout = new QVBoxLayout(overwrite_section_);
    overwrite_layout->setContentsMargins(0, 0, 0, 0);
    overwrite_layout->setSpacing(t.spacing.xs);
    overwrite_text_ = new QLabel(overwrite_section_);
    overwrite_text_->setWordWrap(true);
    overwrite_text_->setFont(theme::fonts::body());
    overwrite_layout->addWidget(overwrite_text_);
    auto* review_row = new QHBoxLayout();
    auto* overwrite_button = new QPushButton(QStringLiteral("Overwrite"), overwrite_section_);
    overwrite_button->setObjectName(QStringLiteral("aida.workspace.save_as.overwrite_confirm"));
    overwrite_button->setProperty("aidaVariant", QStringLiteral("destructive"));
    connect(overwrite_button, &QPushButton::clicked, this, [this] { attemptSave(true); });
    review_row->addWidget(overwrite_button);
    keep_button_ = new QPushButton(QStringLiteral("Keep Existing"), overwrite_section_);
    keep_button_->setObjectName(QStringLiteral("aida.workspace.save_as.overwrite_keep"));
    connect(keep_button_, &QPushButton::clicked, this, [this] {
        review_overwrite_ = false;
        overwrite_section_->setVisible(false);
        status_label_->clear();
        name_edit_->setFocus(Qt::OtherFocusReason);
        name_edit_->selectAll();
        refreshButtons();
    });
    review_row->addWidget(keep_button_);
    review_row->addStretch(1);
    overwrite_layout->addLayout(review_row);
    overwrite_section_->setVisible(false);
    root->addWidget(overwrite_section_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    if (auto* save = buttons_->button(QDialogButtonBox::Save))
        save->setDefault(true);
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        if (!review_overwrite_)
            attemptSave(false);
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, [this] { reject(); });
    root->addWidget(buttons_);

    pending_timer_ = new QTimer(this);
    pending_timer_->setInterval(250);
    pending_timer_->setTimerType(Qt::CoarseTimer);
    connect(pending_timer_, &QTimer::timeout, this, [this] { refreshButtons(); });

    setMinimumSize(380, 220);
    resize(500, 260);
}

void AidaWorkspaceSaveAsDialog::openFresh()
{
    review_overwrite_ = false;
    overwrite_section_->setVisible(false);
    status_label_->clear();
    if (persistence_) {
        const auto identity = persistence_->active_identity();
        base_label_->setText(QStringLiteral("Base preset: %1")
            .arg(preset_display_name(identity.preset)));
        name_edit_->setText(identity.kind == docking::workspace_identity_kind_t::user
            ? QString::fromStdString(identity.user_name) : QString());
    }
    pending_timer_->start();
    refreshButtons();
    aida::ui::application_ui::set_shortcut_capture_active(true);
    open();
    name_edit_->setFocus(Qt::OtherFocusReason);
    name_edit_->selectAll();
    connect(this, &QDialog::finished, this, [this](int) {
        pending_timer_->stop();
        aida::ui::application_ui::set_shortcut_capture_active(false);
    }, Qt::SingleShotConnection);
}

void AidaWorkspaceSaveAsDialog::refreshButtons()
{
    const bool pending = persistence_ && persistence_->operation_pending();
    const bool catalog_ready = persistence_ && persistence_->user_layout_catalog_ready();
    catalog_note_->setVisible(!catalog_ready);
    if (auto* save = buttons_->button(QDialogButtonBox::Save))
        save->setEnabled(!pending && catalog_ready && !name_edit_->text().isEmpty() &&
            !review_overwrite_);
    if (review_overwrite_) {
        if (keep_button_)
            keep_button_->setDefault(true);
    } else if (auto* save = buttons_->button(QDialogButtonBox::Save)) {
        save->setDefault(true);
    }
}

void AidaWorkspaceSaveAsDialog::attemptSave(bool overwrite)
{
    if (!persistence_)
        return;
    const std::string name = name_edit_->text().toStdString();
    const auto result = persistence_->save_user_layout(name, overwrite);
    if (!overwrite && result == docking::workspace_request_result_t::already_exists) {
        review_overwrite_ = true;
        overwrite_text_->setText(QStringLiteral("Overwrite saved workspace \"%1\"? This cannot be undone.")
            .arg(name_edit_->text()));
        status_label_->setText(QStringLiteral(
            "This exact name already exists. Overwrite replaces its saved layout and visibility snapshot."));
        overwrite_section_->setVisible(true);
        refreshButtons();
        if (keep_button_)
            keep_button_->setFocus(Qt::OtherFocusReason);
        return;
    }
    status_label_->setText(QString::fromStdString(
        workspace_request_message(result, overwrite ? "Workspace overwrite" : "Save Workspace As")));
    if (workspace_request_succeeded(result))
        accept();
}

AidaWorkspaceManagerDialog::AidaWorkspaceManagerDialog(
    layout::WorkspacePersistenceController* persistence, AidaWorkspaceController* controller,
    QWidget* parent)
    : bridge::AidaDialog(parent), persistence_(persistence), controller_(controller)
{
    setObjectName(QStringLiteral("aida.workspace.manager"));
    setWindowTitle(QStringLiteral("Saved Workspaces"));
    setModal(false);

    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
    root->setSpacing(t.spacing.sm);

    auto* title = new QLabel(QStringLiteral("Saved Workspaces"), this);
    title->setFont(theme::fonts::strong());
    root->addWidget(title);
    active_label_ = new QLabel(this);
    active_label_->setFont(theme::fonts::caption());
    active_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(active_label_);

    auto* split = new QSplitter(this);
    model_ = new WorkspaceCatalogModel(this);
    catalog_view_ = new QListView(split);
    catalog_view_->setObjectName(QStringLiteral("aida.workspace.manager.catalog"));
    catalog_view_->setModel(model_);
    catalog_view_->setUniformItemSizes(true);
    catalog_view_->setAlternatingRowColors(true);
    split->addWidget(catalog_view_);

    auto* details = new QWidget(split);
    auto* details_layout = new QVBoxLayout(details);
    details_layout->setContentsMargins(t.spacing.sm, 0, 0, 0);
    details_layout->setSpacing(t.spacing.xs);
    detail_name_ = new QLabel(details);
    detail_name_->setFont(theme::fonts::strong());
    detail_name_->setWordWrap(true);
    details_layout->addWidget(detail_name_);
    detail_preset_ = new QLabel(details);
    detail_preset_->setFont(theme::fonts::caption());
    detail_preset_->setProperty("aidaVariant", QStringLiteral("secondary"));
    details_layout->addWidget(detail_preset_);
    detail_generation_ = new QLabel(details);
    detail_generation_->setFont(theme::fonts::caption());
    detail_generation_->setProperty("aidaVariant", QStringLiteral("secondary"));
    details_layout->addWidget(detail_generation_);

    rename_edit_ = new widgets::AidaLineEdit(details);
    rename_edit_->setObjectName(QStringLiteral("aida.workspace.manager.rename"));
    rename_edit_->setMaxLength(64);
    rename_edit_->setPlaceholderText(QStringLiteral("New workspace name"));
    bridge::InteractionContextProvider::mark_text_input(rename_edit_);
    connect(rename_edit_, &QLineEdit::textChanged, this, [this](const QString&) {
        onSelectionChanged();
    });
    details_layout->addWidget(rename_edit_);

    auto* action_row = new QHBoxLayout();
    load_button_ = new QPushButton(QStringLiteral("Load"), details);
    load_button_->setObjectName(QStringLiteral("aida.workspace.manager.load"));
    connect(load_button_, &QPushButton::clicked, this, [this] { onLoadClicked(); });
    action_row->addWidget(load_button_);
    save_copy_button_ = new QPushButton(QStringLiteral("Save As Copy"), details);
    save_copy_button_->setObjectName(QStringLiteral("aida.workspace.manager.save_copy"));
    connect(save_copy_button_, &QPushButton::clicked, this, [this] { onSaveAsCopy(); });
    action_row->addWidget(save_copy_button_);
    rename_button_ = new QPushButton(QStringLiteral("Rename"), details);
    rename_button_->setObjectName(QStringLiteral("aida.workspace.manager.rename_apply"));
    connect(rename_button_, &QPushButton::clicked, this, [this] { onRename(); });
    action_row->addWidget(rename_button_);
    delete_button_ = new QPushButton(QStringLiteral("Delete..."), details);
    delete_button_->setObjectName(QStringLiteral("aida.workspace.manager.delete"));
    connect(delete_button_, &QPushButton::clicked, this, [this] { onDeleteReview(); });
    action_row->addWidget(delete_button_);
    action_row->addStretch(1);
    details_layout->addLayout(action_row);

    delete_section_ = new QWidget(details);
    delete_section_->setObjectName(QStringLiteral("aida.workspace.manager.delete_section"));
    auto* delete_layout = new QVBoxLayout(delete_section_);
    delete_layout->setContentsMargins(0, t.spacing.xs, 0, 0);
    delete_text_ = new QLabel(delete_section_);
    delete_text_->setWordWrap(true);
    delete_text_->setFont(theme::fonts::body());
    delete_layout->addWidget(delete_text_);
    auto* delete_row = new QHBoxLayout();
    delete_confirm_ = new QPushButton(QStringLiteral("Delete Permanently"), delete_section_);
    delete_confirm_->setObjectName(QStringLiteral("aida.workspace.manager.delete_confirm"));
    delete_confirm_->setProperty("aidaVariant", QStringLiteral("destructive"));
    connect(delete_confirm_, &QPushButton::clicked, this, [this] { onDeleteConfirm(); });
    delete_row->addWidget(delete_confirm_);
    delete_cancel_ = new QPushButton(QStringLiteral("Cancel Delete"), delete_section_);
    delete_cancel_->setObjectName(QStringLiteral("aida.workspace.manager.delete_cancel"));
    connect(delete_cancel_, &QPushButton::clicked, this, [this] {
        review_delete_ = false;
        delete_section_->setVisible(false);
        updateDefaultButton();
    });
    delete_row->addWidget(delete_cancel_);
    delete_row->addStretch(1);
    delete_layout->addLayout(delete_row);
    delete_section_->setVisible(false);
    details_layout->addWidget(delete_section_);
    details_layout->addStretch(1);
    split->addWidget(details);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    root->addWidget(split, 1);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.workspace.manager.status"));
    status_label_->setFont(theme::fonts::caption());
    status_label_->setWordWrap(true);
    root->addWidget(status_label_);

    reset_section_ = new QWidget(this);
    reset_section_->setObjectName(QStringLiteral("aida.workspace.manager.reset_section"));
    auto* reset_layout = new QVBoxLayout(reset_section_);
    reset_layout->setContentsMargins(0, t.spacing.xs, 0, 0);
    auto* reset_text = new QLabel(QStringLiteral(
        "Reset all layouts? Every built-in customization and every named workspace will be removed, then the factory Analysis workspace will open. Documents, analysis sessions and background jobs remain intact."),
        reset_section_);
    reset_text->setWordWrap(true);
    reset_text->setFont(theme::fonts::body());
    reset_layout->addWidget(reset_text);
    auto* reset_row = new QHBoxLayout();
    reset_confirm_ = new QPushButton(QStringLiteral("Reset All Layouts"), reset_section_);
    reset_confirm_->setObjectName(QStringLiteral("aida.workspace.manager.reset_all_confirm"));
    reset_confirm_->setProperty("aidaVariant", QStringLiteral("destructive"));
    connect(reset_confirm_, &QPushButton::clicked, this, [this] { onResetAllConfirm(); });
    reset_row->addWidget(reset_confirm_);
    reset_cancel_ = new QPushButton(QStringLiteral("Cancel Reset"), reset_section_);
    reset_cancel_->setObjectName(QStringLiteral("aida.workspace.manager.reset_cancel"));
    connect(reset_cancel_, &QPushButton::clicked, this, [this] {
        review_reset_all_ = false;
        reset_section_->setVisible(false);
        save_as_button_->setVisible(true);
        close_button_->setVisible(true);
        updateDefaultButton();
    });
    reset_row->addWidget(reset_cancel_);
    reset_row->addStretch(1);
    reset_layout->addLayout(reset_row);
    reset_section_->setVisible(false);
    root->addWidget(reset_section_);

    auto* footer = new QHBoxLayout();
    save_as_button_ = new QPushButton(QStringLiteral("Save Workspace As..."), this);
    save_as_button_->setObjectName(QStringLiteral("aida.workspace.manager.save_as"));
    connect(save_as_button_, &QPushButton::clicked, this, [this] {
        if (controller_)
            controller_->openSaveAs();
    });
    footer->addWidget(save_as_button_);
    footer->addStretch(1);
    close_button_ = new QPushButton(QStringLiteral("Close"), this);
    close_button_->setObjectName(QStringLiteral("aida.workspace.manager.close"));
    connect(close_button_, &QPushButton::clicked, this, [this] { reject(); });
    footer->addWidget(close_button_);
    root->addLayout(footer);

    pending_timer_ = new QTimer(this);
    pending_timer_->setInterval(250);
    pending_timer_->setTimerType(Qt::CoarseTimer);
    connect(pending_timer_, &QTimer::timeout, this, [this] {
        revalidateSelection();
        onSelectionChanged();
    });

    connect(catalog_view_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (auto* model = static_cast<WorkspaceCatalogModel*>(model_)) {
            if (const auto* item = model->at(current.row())) {
                selected_name_ = QString::fromStdString(item->name);
                selected_generation_ = item->generation;
                selection_requires_reselection_ = false;
                rename_edit_->setText(selected_name_);
                review_delete_ = false;
                delete_section_->setVisible(false);
                status_label_->clear();
            }
        }
        onSelectionChanged();
    });
    connect(catalog_view_, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!index.isValid())
            return;
        onLoadClicked();
    });

    setMinimumSize(520, 360);
    resize(760, 500);
}

void AidaWorkspaceManagerDialog::openFresh(const QString& selected, quint64 generation)
{
    review_delete_ = false;
    review_reset_all_ = false;
    delete_section_->setVisible(false);
    reset_section_->setVisible(false);
    save_as_button_->setVisible(true);
    close_button_->setVisible(true);
    status_label_->clear();
    selected_name_.clear();
    selected_generation_ = 0;
    selection_requires_reselection_ = false;
    if (!selected.isEmpty()) {
        selected_name_ = selected;
        selected_generation_ = generation;
        rename_edit_->setText(selected);
    }
    refreshCatalog();
    revalidateSelection();
    pending_timer_->start();
    if (persistence_)
        connect(persistence_, &layout::WorkspacePersistenceController::userCatalogChanged,
                this, [this] {
            refreshCatalog();
            revalidateSelection();
        }, Qt::UniqueConnection);
    aida::ui::application_ui::set_shortcut_capture_active(true);
    open();
    connect(this, &QDialog::finished, this, [this](int) {
        pending_timer_->stop();
        aida::ui::application_ui::set_shortcut_capture_active(false);
    }, Qt::SingleShotConnection);
}

void AidaWorkspaceManagerDialog::openResetAllReview()
{
    openFresh();
    review_reset_all_ = true;
    reset_section_->setVisible(true);
    save_as_button_->setVisible(false);
    close_button_->setVisible(false);
    updateDefaultButton();
    reset_cancel_->setFocus(Qt::OtherFocusReason);
}

void AidaWorkspaceManagerDialog::refreshCatalog()
{
    auto* model = static_cast<WorkspaceCatalogModel*>(model_);
    if (!model)
        return;
    model->setCatalog(persistence_ ? persistence_->user_layout_catalog() : nullptr);
    if (persistence_) {
        const auto active = persistence_->active_identity();
        active_label_->setText(active.kind == docking::workspace_identity_kind_t::user
            ? QStringLiteral("Active: %1 / %2")
                .arg(preset_display_name(active.preset), QString::fromStdString(active.user_name))
            : QStringLiteral("Active: %1 / Built-in").arg(preset_display_name(active.preset)));
    }
    if (selected_name_.isEmpty() && !selection_requires_reselection_ && model->rowCount() > 0) {
        int initial = 0;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->at(row) && model->at(row)->active) {
                initial = row;
                break;
            }
        }
        catalog_view_->setCurrentIndex(model->index(initial, 0));
        if (const auto* item = model->at(initial)) {
            selected_name_ = QString::fromStdString(item->name);
            selected_generation_ = item->generation;
            rename_edit_->setText(selected_name_);
        }
    }
    onSelectionChanged();
}

void AidaWorkspaceManagerDialog::revalidateSelection()
{
    if (selected_name_.isEmpty() || !persistence_ || !persistence_->user_layout_catalog_ready())
        return;
    const auto catalog = persistence_->user_layout_catalog();
    const auto found = catalog ? std::find_if(catalog->begin(), catalog->end(),
        [&](const auto& item) { return item.name == selected_name_.toStdString(); })
        : std::vector<docking::user_workspace_descriptor_t>::const_iterator{};
    const bool missing = !catalog || found == catalog->end();
    const bool replaced = !missing &&
        (selected_generation_ == 0 || found->generation != selected_generation_);
    if (missing || replaced) {
        if (!review_reset_all_) {
            setStatus(missing
                ? QStringLiteral("The selected saved workspace no longer exists. Select another workspace.")
                : QStringLiteral("The selected saved workspace changed after selection. Select its current generation explicitly."));
        }
        selected_name_.clear();
        selected_generation_ = 0;
        selection_requires_reselection_ = true;
        catalog_view_->clearSelection();
        onSelectionChanged();
    }
}

void AidaWorkspaceManagerDialog::onSelectionChanged()
{
    const bool pending = persistence_ && persistence_->operation_pending();
    const bool has_selection = !selected_name_.isEmpty();
    auto* model = static_cast<WorkspaceCatalogModel*>(model_);
    const docking::user_workspace_descriptor_t* selected = nullptr;
    if (model && has_selection) {
        for (int row = 0; row < model->rowCount(); ++row) {
            const auto* item = model->at(row);
            if (item && item->name == selected_name_.toStdString() &&
                item->generation == selected_generation_) {
                selected = item;
                break;
            }
        }
    }
    detail_name_->setText(selected ? selected_name_ : QStringLiteral("Select a saved workspace to load, rename or delete it."));
    detail_preset_->setText(selected
        ? QStringLiteral("Base preset: %1").arg(preset_display_name(selected->base_preset))
        : QString());
    detail_generation_->setText(selected
        ? QStringLiteral("Saved generation: %1").arg(selected->generation)
        : QString());
    if (selected) {
        const auto capability = named_workspace_load_capability(*selected, persistence_);
        load_button_->setEnabled(capability.enabled);
        load_button_->setToolTip(QString::fromStdString(capability.disabled_reason));
    } else {
        load_button_->setEnabled(false);
        load_button_->setToolTip(QString());
    }
    rename_button_->setEnabled(!pending && selected &&
        !rename_edit_->text().isEmpty() && rename_edit_->text() != selected_name_);
    delete_button_->setEnabled(!pending && selected);
    save_copy_button_->setEnabled(selected != nullptr);
    updateDefaultButton();
}

void AidaWorkspaceManagerDialog::updateDefaultButton()
{
    QPushButton* target = load_button_;
    if (review_reset_all_)
        target = reset_cancel_;
    else if (review_delete_)
        target = delete_cancel_;
    else if (rename_button_->isEnabled())
        target = rename_button_;
    if (target)
        target->setDefault(true);
}

void AidaWorkspaceManagerDialog::onLoadClicked()
{
    auto* model = static_cast<WorkspaceCatalogModel*>(model_);
    if (!model)
        return;
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto* item = model->at(row);
        if (item && item->name == selected_name_.toStdString() &&
            item->generation == selected_generation_) {
            auto context = named_workspace_load_context(*item, persistence_);
            const auto result = aida::ui::application_ui::execute_retained_entity_action(
                "workspace.load_named", aida::ui::action_invocation_source_t::toolbar, context);
            setStatus(QString::fromStdString(result.message));
            if (result.executed())
                accept();
            return;
        }
    }
}

void AidaWorkspaceManagerDialog::onSaveAsCopy()
{
    if (selected_name_.isEmpty())
        return;
    AidaWorkspaceController* controller = controller_;
    reject();
    if (controller)
        controller->openSaveAs();
}

void AidaWorkspaceManagerDialog::onRename()
{
    if (!persistence_ || selected_name_.isEmpty())
        return;
    const std::string old_name = selected_name_.toStdString();
    const auto result = persistence_->rename_user_layout(old_name,
        rename_edit_->text().toStdString());
    setStatus(QString::fromStdString(workspace_request_message(result, "Rename workspace")));
    if (workspace_request_succeeded(result)) {
        selected_name_.clear();
        selected_generation_ = 0;
        selection_requires_reselection_ = true;
        refreshCatalog();
    }
}

void AidaWorkspaceManagerDialog::onDeleteReview()
{
    if (selected_name_.isEmpty())
        return;
    review_delete_ = true;
    delete_text_->setText(QStringLiteral(
        "Delete \"%1\"? The saved layout and its visibility snapshot will be removed. Open documents and jobs are not deleted.")
        .arg(selected_name_));
    delete_section_->setVisible(true);
    updateDefaultButton();
    delete_cancel_->setFocus(Qt::OtherFocusReason);
}

void AidaWorkspaceManagerDialog::onDeleteConfirm()
{
    if (!persistence_ || selected_name_.isEmpty())
        return;
    const auto result = persistence_->delete_user_layout(selected_name_.toStdString());
    setStatus(QString::fromStdString(workspace_request_message(result, "Delete workspace")));
    if (workspace_request_succeeded(result)) {
        selected_name_.clear();
        selected_generation_ = 0;
        selection_requires_reselection_ = true;
        review_delete_ = false;
        delete_section_->setVisible(false);
        refreshCatalog();
    }
}

void AidaWorkspaceManagerDialog::onResetAllConfirm()
{
    if (!persistence_)
        return;
    const auto result = persistence_->reset_all();
    setStatus(QString::fromStdString(workspace_request_message(result, "Reset all layouts")));
    if (workspace_request_succeeded(result))
        accept();
}

void AidaWorkspaceManagerDialog::setStatus(const QString& text)
{
    status_label_->setText(text);
    if (!text.isEmpty() && controller_)
        controller_->showStatusMessage(text, 5000);
}

void AidaWorkspaceManagerDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Delete && catalog_view_->hasFocus() &&
        delete_button_->isEnabled() && !review_delete_ && !review_reset_all_) {
        onDeleteReview();
        event->accept();
        return;
    }
    bridge::AidaDialog::keyPressEvent(event);
}

AidaWorkspaceController::AidaWorkspaceController(docking::AidaDockHost* host,
                                                 QWidget* dialog_parent, QObject* parent)
    : QObject(parent), host_(host), dialog_parent_(dialog_parent)
{
}

void AidaWorkspaceController::openSaveAs()
{
    auto* persistence = host_ ? host_->persistence() : nullptr;
    if (!persistence)
        return;
    if (!save_as_)
        save_as_ = new AidaWorkspaceSaveAsDialog(persistence, this, dialog_parent_);
    save_as_->openFresh();
}

void AidaWorkspaceController::openManager(const QString& name, quint64 generation)
{
    auto* persistence = host_ ? host_->persistence() : nullptr;
    if (!persistence)
        return;
    if (!manager_)
        manager_ = new AidaWorkspaceManagerDialog(persistence, this, dialog_parent_);
    manager_->openFresh(name, generation);
}

void AidaWorkspaceController::openResetAllReview()
{
    auto* persistence = host_ ? host_->persistence() : nullptr;
    if (!persistence)
        return;
    if (!manager_)
        manager_ = new AidaWorkspaceManagerDialog(persistence, this, dialog_parent_);
    manager_->openResetAllReview();
}

void AidaWorkspaceController::showStatusMessage(const QString& message, int timeout_ms)
{
    Q_EMIT statusMessage(message, timeout_ms);
}

}
