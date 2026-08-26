#include "qt/analysis/qt_struct_dissector_view.hpp"

#include <QComboBox>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/ai/entity_evidence_handoff.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/workbench/workbench_shell_integration.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_dissector_dialogs.hpp"
#include "qt/analysis/qt_dissector_models.hpp"
#include "qt/analysis/qt_write_review_dialog.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_search_field.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::analysis {

QtStructDissectorView* QtStructDissectorView::active_instance_ = nullptr;

qt_staged_target_state_t& staged_dissector_target_store() {
    static qt_staged_target_state_t value;
    return value;
}

QtStructDissectorView::QtStructDissectorView(QWidget* parent) : QWidget(parent) {
    active_instance_ = this;
    setObjectName(QStringLiteral("aida.view.types.dissector"));
    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.dissector.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(t.toolbar.padding_x, t.toolbar.padding_y,
        t.toolbar.padding_x, t.toolbar.padding_y);
    toolbar_layout->setSpacing(t.toolbar.group_gap);
    toolbar_layout->addWidget(new QLabel(QStringLiteral("Base"), toolbar));
    addr_edit_ = new QLineEdit(toolbar);
    addr_edit_->setObjectName(QStringLiteral("aida.dissector.address"));
    const QFontMetricsF addr_metrics(addr_edit_->font());
    addr_edit_->setMinimumWidth(static_cast<int>(addr_metrics.horizontalAdvance(
        QStringLiteral("0xDDDDDDDDDDDDDDDD"))) + 2 * t.table.cell_pad_x +
        t.spacing.lg);
    addr_edit_->setPlaceholderText(QStringLiteral("base address (hex)"));
    addr_edit_->setToolTip(QStringLiteral(
        "Base address of the structure instance in the attached process"));
    toolbar_layout->addWidget(addr_edit_);
    auto* go_button = new QToolButton(toolbar);
    go_button->setObjectName(QStringLiteral("aida.dissector.go"));
    go_button->setText(QStringLiteral("Go"));
    go_button->setToolTip(QStringLiteral("Dissect memory at the base address"));
    toolbar_layout->addWidget(go_button);
    auto* refresh_button = new QToolButton(toolbar);
    refresh_button->setObjectName(QStringLiteral("aida.dissector.refresh"));
    refresh_button->setText(QStringLiteral("Refresh"));
    refresh_button->setToolTip(QStringLiteral(
        "Re-read the live field values from the target"));
    toolbar_layout->addWidget(refresh_button);
    auto* auto_check = new QToolButton(toolbar);
    auto_check->setObjectName(QStringLiteral("aida.dissector.auto_refresh"));
    auto_check->setText(QStringLiteral("Auto"));
    auto_check->setCheckable(true);
    auto_check->setToolTip(QStringLiteral(
        "Refresh the live field values automatically"));
    toolbar_layout->addWidget(auto_check);
    auto* undo_button = new QToolButton(toolbar);
    undo_button->setObjectName(QStringLiteral("aida.dissector.undo"));
    undo_button->setText(QStringLiteral("Undo"));
    undo_button->setToolTip(QStringLiteral("Undo the last catalog edit"));
    toolbar_layout->addWidget(undo_button);
    auto* redo_button = new QToolButton(toolbar);
    redo_button->setObjectName(QStringLiteral("aida.dissector.redo"));
    redo_button->setText(QStringLiteral("Redo"));
    redo_button->setToolTip(QStringLiteral("Redo the last undone catalog edit"));
    toolbar_layout->addWidget(redo_button);
    auto* export_button = new QToolButton(toolbar);
    export_button->setObjectName(QStringLiteral("aida.dissector.export_c"));
    export_button->setText(QStringLiteral("Export C"));
    export_button->setToolTip(QStringLiteral(
        "Copy the active structure as a C declaration"));
    toolbar_layout->addWidget(export_button);
    auto* copy_schema = new QToolButton(toolbar);
    copy_schema->setObjectName(QStringLiteral("aida.dissector.copy_schema"));
    copy_schema->setText(QStringLiteral("Copy Schema"));
    copy_schema->setToolTip(QStringLiteral(
        "Copy the versioned structure catalog schema"));
    toolbar_layout->addWidget(copy_schema);
    auto* import_json = new QToolButton(toolbar);
    import_json->setObjectName(QStringLiteral("aida.dissector.import_json"));
    import_json->setText(QStringLiteral("Import JSON"));
    import_json->setToolTip(QStringLiteral(
        "Import a structure catalog schema from the clipboard"));
    toolbar_layout->addWidget(import_json);
    auto* layout_button = new QToolButton(toolbar);
    layout_button->setObjectName(QStringLiteral("aida.dissector.layout"));
    layout_button->setText(QStringLiteral("Layout"));
    layout_button->setToolTip(QStringLiteral(
        "Configure packing, alignment, persistence, and enum management"));
    toolbar_layout->addWidget(layout_button);
    toolbar_layout->addStretch(1);
    layout->addWidget(toolbar);

    staged_strip_ = new QWidget(this);
    staged_strip_->setObjectName(QStringLiteral("aida.dissector.staged_strip"));
    auto* staged_layout = new QHBoxLayout(staged_strip_);
    staged_layout->setContentsMargins(t.toolbar.padding_x, t.spacing.xxs,
        t.toolbar.padding_x, t.spacing.xxs);
    staged_layout->setSpacing(t.toolbar.group_gap);
    staged_label_ = new QLabel(staged_strip_);
    staged_label_->setObjectName(QStringLiteral("aida.dissector.staged_label"));
    staged_layout->addWidget(staged_label_, 1);
    auto* use_button = new QToolButton(staged_strip_);
    use_button->setObjectName(QStringLiteral("aida.dissector.staged_use"));
    use_button->setText(QStringLiteral("Use Target"));
    use_button->setToolTip(QStringLiteral(
        "Replace the base address with the staged target after validation"));
    staged_layout->addWidget(use_button);
    auto* recheck_button = new QToolButton(staged_strip_);
    recheck_button->setObjectName(QStringLiteral("aida.dissector.staged_recheck"));
    recheck_button->setText(QStringLiteral("Recheck"));
    recheck_button->setToolTip(QStringLiteral(
        "Revalidate the staged target against its source"));
    staged_layout->addWidget(recheck_button);
    auto* dismiss_button = new QToolButton(staged_strip_);
    dismiss_button->setObjectName(QStringLiteral("aida.dissector.staged_dismiss"));
    dismiss_button->setText(QStringLiteral("Dismiss"));
    dismiss_button->setToolTip(QStringLiteral("Discard the staged target"));
    staged_layout->addWidget(dismiss_button);
    staged_strip_->setVisible(false);
    layout->addWidget(staged_strip_);

    splitter_ = new QSplitter(this);
    splitter_->setObjectName(QStringLiteral("aida.dissector.splitter"));
    auto* left = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(t.spacing.xs, t.spacing.xs,
        t.spacing.xs, t.spacing.xs);
    left_layout->setSpacing(t.spacing.xs);
    auto* structures_header = new QLabel(QStringLiteral("Structures"), left);
    structures_header->setProperty("aidaVariant", QStringLiteral("secondary"));
    left_layout->addWidget(structures_header);
    struct_filter_ = new widgets::AidaSearchField(QStringLiteral("filter structures"),
        left);
    struct_filter_->setObjectName(QStringLiteral("aida.dissector.filter"));
    struct_filter_->setClearButtonEnabled(true);
    left_layout->addWidget(struct_filter_);
    struct_model_ = new QtDissectorStructureModel(this);
    struct_table_ = new QTableView(left);
    struct_table_->setObjectName(QStringLiteral("aida.dissector.structures"));
    struct_table_->verticalHeader()->setVisible(false);
    struct_table_->horizontalHeader()->setVisible(false);
    struct_table_->horizontalHeader()->setStretchLastSection(true);
    struct_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    struct_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    struct_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    struct_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    struct_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    struct_table_->setShowGrid(false);
    struct_table_->setAlternatingRowColors(true);
    struct_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    struct_table_->setModel(struct_model_);
    left_layout->addWidget(struct_table_, 1);
    auto* rename_row = new QHBoxLayout();
    rename_edit_ = new QLineEdit(left);
    rename_edit_->setObjectName(QStringLiteral("aida.dissector.rename_edit"));
    rename_edit_->setPlaceholderText(QStringLiteral("rename selected"));
    rename_row->addWidget(rename_edit_, 1);
    auto* rename_button = new QToolButton(left);
    rename_button->setObjectName(QStringLiteral("aida.dissector.rename"));
    rename_button->setText(QStringLiteral("Rename"));
    rename_button->setToolTip(QStringLiteral("Rename the selected structure"));
    rename_row->addWidget(rename_button);
    left_layout->addLayout(rename_row);
    auto* create_row = new QHBoxLayout();
    new_name_edit_ = new QLineEdit(left);
    new_name_edit_->setObjectName(QStringLiteral("aida.dissector.new_name"));
    new_name_edit_->setPlaceholderText(QStringLiteral("name"));
    create_row->addWidget(new_name_edit_, 1);
    auto* create_button = new QToolButton(left);
    create_button->setObjectName(QStringLiteral("aida.dissector.create"));
    create_button->setText(QStringLiteral("+"));
    create_button->setToolTip(QStringLiteral("Create a new empty structure"));
    create_row->addWidget(create_button);
    auto* delete_button = new QToolButton(left);
    delete_button->setObjectName(QStringLiteral("aida.dissector.delete"));
    delete_button->setText(QStringLiteral("-"));
    delete_button->setToolTip(QStringLiteral("Delete the selected structure"));
    create_row->addWidget(delete_button);
    left_layout->addLayout(create_row);
    splitter_->addWidget(left);

    auto* right = new QWidget(this);
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(t.spacing.xs, t.spacing.xs,
        t.spacing.xs, t.spacing.xs);
    right_layout->setSpacing(t.spacing.xxs);
    field_model_ = new QtDissectorFieldModel(this);
    field_table_ = new QTableView(right);
    field_table_->setModel(field_model_);
    field_table_->setObjectName(QStringLiteral("aida.dissector.fields"));
    field_table_->verticalHeader()->setVisible(false);
    field_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    field_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* field_header = field_table_->horizontalHeader();
    using FieldColumn = QtDissectorFieldModel::Column;
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const QFontMetricsF ui_metrics(field_table_->font());
    const auto with_cell_pad = [&t](int content) {
        return content + 2 * t.table.cell_pad_x + t.spacing.xs;
    };
    field_header->setSectionResizeMode(static_cast<int>(FieldColumn::offset),
        QHeaderView::Fixed);
    field_table_->setColumnWidth(static_cast<int>(FieldColumn::offset),
        with_cell_pad(static_cast<int>(code_metrics.horizontalAdvance(
            QStringLiteral("+0xFFFF")))));
    field_header->setSectionResizeMode(static_cast<int>(FieldColumn::name),
        QHeaderView::Fixed);
    field_table_->setColumnWidth(static_cast<int>(FieldColumn::name),
        with_cell_pad(static_cast<int>(ui_metrics.averageCharWidth() * 24.0)));
    field_header->setSectionResizeMode(static_cast<int>(FieldColumn::type),
        QHeaderView::Fixed);
    field_table_->setColumnWidth(static_cast<int>(FieldColumn::type),
        with_cell_pad(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("UTF-16 String")))));
    field_header->setSectionResizeMode(static_cast<int>(FieldColumn::value),
        QHeaderView::Stretch);
    field_header->setSectionResizeMode(static_cast<int>(FieldColumn::description),
        QHeaderView::Stretch);
    field_header->setStretchLastSection(false);
    field_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    field_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    field_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    field_table_->setShowGrid(false);
    field_table_->setAlternatingRowColors(true);
    field_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    right_layout->addWidget(field_table_, 1);
    validation_label_ = new QLabel(right);
    validation_label_->setObjectName(QStringLiteral("aida.dissector.validation"));
    operation_label_ = new QLabel(right);
    operation_label_->setObjectName(QStringLiteral("aida.dissector.operation"));
    operation_label_->setWordWrap(true);
    right_layout->addWidget(validation_label_);
    right_layout->addWidget(operation_label_);
    auto* add_row = new QHBoxLayout();
    field_name_edit_ = new QLineEdit(right);
    field_name_edit_->setObjectName(QStringLiteral("aida.dissector.field_name"));
    field_name_edit_->setPlaceholderText(QStringLiteral("field name"));
    add_row->addWidget(field_name_edit_, 1);
    field_offset_edit_ = new QLineEdit(right);
    field_offset_edit_->setObjectName(QStringLiteral("aida.dissector.field_offset"));
    field_offset_edit_->setPlaceholderText(QStringLiteral("+0x?"));
    field_offset_edit_->setMinimumWidth(static_cast<int>(code_metrics.horizontalAdvance(
        QStringLiteral("+0xFFFF"))) + 2 * t.table.cell_pad_x + t.spacing.lg);
    add_row->addWidget(field_offset_edit_);
    field_type_combo_ = new QComboBox(right);
    field_type_combo_->setObjectName(QStringLiteral("aida.dissector.field_type"));
    field_type_combo_->setToolTip(QStringLiteral("Type of the new field"));
    for (int i = 0; i < static_cast<int>(struct_dissector::field_type_t::COUNT); ++i)
        field_type_combo_->addItem(QString::fromLatin1(
            struct_dissector::field_type_name(
                static_cast<struct_dissector::field_type_t>(i))));
    add_row->addWidget(field_type_combo_);
    auto* add_button = new QToolButton(right);
    add_button->setObjectName(QStringLiteral("aida.dissector.field_add"));
    add_button->setText(QStringLiteral("Add"));
    add_button->setToolTip(QStringLiteral("Add the field to the active structure"));
    add_row->addWidget(add_button);
    auto* delete_field_button = new QToolButton(right);
    delete_field_button->setObjectName(QStringLiteral("aida.dissector.field_delete"));
    delete_field_button->setText(QStringLiteral("Del"));
    delete_field_button->setToolTip(QStringLiteral(
        "Remove the selected field from the active structure"));
    add_row->addWidget(delete_field_button);
    right_layout->addLayout(add_row);
    splitter_->addWidget(right);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 3);
    layout->addWidget(splitter_, 1);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.dissector.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(66);
    connect(timer_, &QTimer::timeout, this, [this] { pollEngine(); });

    connect(go_button, &QToolButton::clicked, this, [this] { applyBaseAddress(); });
    connect(addr_edit_, &QLineEdit::returnPressed, this, [this] {
        applyBaseAddress();
    });
    connect(refresh_button, &QToolButton::clicked, this, [] {
        diag::log_tagged_fmt("dissector", "refresh_clicked manual=1");
        struct_dissector::refresh_values();
    });
    connect(auto_check, &QToolButton::toggled, this, [](bool checked) {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        struct_dissector::g_state.auto_refresh = checked;
    });
    connect(undo_button, &QToolButton::clicked, this, [this] {
        static_cast<void>(applyCatalogUndo());
    });
    connect(redo_button, &QToolButton::clicked, this, [this] {
        static_cast<void>(applyCatalogRedo());
    });
    connect(export_button, &QToolButton::clicked, this, [this] {
        int active = -1;
        {
            std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
            active = struct_dissector::g_state.active_struct;
        }
        if (active >= 0) {
            const std::string c_source = struct_dissector::export_to_c(active);
            if (!c_source.empty()) {
                clipboard::set_text(QString::fromStdString(c_source));
                diag::log_tagged_fmt("dissector",
                    "export_to_c_clipboard idx=%d bytes=%zu", active, c_source.size());
            }
        } else {
            diag::log_tagged_fmt("dissector", "export_to_c_clicked_no_active");
        }
    });
    connect(copy_schema, &QToolButton::clicked, this, [this] {
        const std::string schema = struct_dissector::serialize_schema();
        clipboard::set_text(QString::fromStdString(schema));
        operation_error_ = false;
        operation_status_ = "Versioned structure schema copied";
        operation_label_->setText(QString::fromStdString(operation_status_));
    });
    connect(import_json, &QToolButton::clicked, this, [this] {
        const QString content = clipboard::text();
        std::string error;
        const bool imported = !content.isEmpty() && catalogEdit(
            "Import structure catalog", [&]() {
                return struct_dissector::deserialize_schema(content.toStdString(), error);
            });
        operation_error_ = !imported;
        operation_status_ = imported ? "Structure schema imported and validated"
            : (error.empty() ? "Clipboard does not contain a structure schema" : error);
        operation_label_->setText(QString::fromStdString(operation_status_));
    });
    connect(layout_button, &QToolButton::clicked, this, [this] { openLayoutConfig(); });
    connect(rename_button, &QToolButton::clicked, this, [this] {
        int target = -1;
        {
            std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
            target = struct_dissector::g_state.active_struct;
        }
        const std::string name = rename_edit_->text().toStdString();
        if (target >= 0 && !name.empty()) {
            if (catalogEdit("Rename structure", [&] {
                return struct_dissector::rename_struct(target, name);
            }))
                rename_edit_->clear();
        } else {
            diag::log_tagged_fmt("dissector",
                "rename_struct_skipped reason='%s'",
                target < 0 ? "no_active" : "empty_name");
        }
    });
    connect(create_button, &QToolButton::clicked, this, [this] {
        const std::string name = new_name_edit_->text().toStdString();
        if (!name.empty()) {
            const int created = catalogIndexEdit("Create structure", [&] {
                return struct_dissector::create_struct(name);
            });
            if (created >= 0) {
                std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
                struct_dissector::g_state.active_struct = created;
                selected_field_ = -1;
                editing_field_ = -1;
            }
            new_name_edit_->clear();
        } else {
            diag::log_tagged_fmt("dissector", "create_struct_skipped reason='empty_name'");
        }
    });
    connect(delete_button, &QToolButton::clicked, this, [this] {
        std::string deleted_name;
        int removed = -1;
        {
            std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
            if (struct_dissector::valid_index(struct_dissector::g_state.active_struct,
                    struct_dissector::g_state.structs.size())) {
                removed = struct_dissector::g_state.active_struct;
                deleted_name = struct_dissector::g_state.structs[
                    static_cast<std::size_t>(removed)].name;
            }
        }
        std::string error;
        if (removed >= 0 && catalogEdit("Delete structure", [&] {
            return struct_dissector::remove_structure(removed, error);
        })) {
            selected_field_ = -1;
            editing_field_ = -1;
            edit_target_ = 0;
            operation_error_ = false;
            operation_status_ = "Structure removed";
            diag::log_tagged_fmt("dissector", "delete_struct idx=%d name='%s'",
                removed, deleted_name.c_str());
        } else {
            operation_error_ = true;
            operation_status_ = error.empty() ? "No active structure" : error;
        }
        operation_label_->setText(QString::fromStdString(operation_status_));
    });
    connect(struct_filter_, &QLineEdit::textChanged, this, [this](const QString&) {
        rebuildStructureList();
    });
    connect(struct_table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) return;
        auto* model = qobject_cast<QtDissectorStructureModel*>(struct_model_);
        const int engine_index = model ? model->engineIndexFor(current.row()) : -1;
        if (engine_index < 0) return;
        std::string name;
        {
            std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
            if (struct_dissector::valid_index(engine_index,
                    struct_dissector::g_state.structs.size())) {
                struct_dissector::g_state.active_struct = engine_index;
                name = struct_dissector::g_state.structs[
                    static_cast<std::size_t>(engine_index)].name;
            }
        }
        selected_field_ = -1;
        editing_field_ = -1;
        edit_target_ = 0;
        diag::log_tagged_fmt("dissector", "struct_selected idx=%d name='%s'",
            engine_index, name.c_str());
        refreshFieldModel();
    });
    connect(struct_table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = struct_table_->indexAt(pos);
        if (index.isValid())
            showStructureMenu(struct_table_->viewport()->mapToGlobal(pos), index.row());
    });
    connect(field_table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) return;
        selected_field_ = current.row();
        publishFieldSelection(current.row());
    });
    connect(field_table_, &QTableView::activated, this,
            [this](const QModelIndex& index) {
        if (!index.isValid()) return;
        if (index.column() != static_cast<int>(QtDissectorFieldModel::Column::value))
            return;
        const auto* value =
            qobject_cast<QtDissectorFieldModel*>(field_model_)->valueAt(index.row());
        const auto* field =
            qobject_cast<QtDissectorFieldModel*>(field_model_)->fieldAt(index.row());
        if (!field || !value) return;
        std::uint64_t structure_id = 0;
        std::uint64_t structure_revision = 0;
        std::uint64_t base = 0;
        {
            std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
            const int active = struct_dissector::g_state.active_struct;
            if (!struct_dissector::valid_index(active,
                    struct_dissector::g_state.structs.size()))
                return;
            const auto& structure = struct_dissector::g_state.structs[
                static_cast<std::size_t>(active)];
            structure_id = structure.stable_id;
            structure_revision = structure.layout_revision;
            base = struct_dissector::g_state.base_address;
        }
        editing_field_ = index.row();
        edit_value_focus_requested_ = true;
        edit_value_structure_id_ = structure_id;
        edit_value_structure_revision_ = structure_revision;
        edit_value_field_id_ = field->stable_id;
        edit_base_address_ = base;
        edit_value_buf_ = value->display_text;
        showInlineEdit(0, index.row(), value->display_text);
    });
    connect(field_table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = field_table_->indexAt(pos);
        if (index.isValid())
            showFieldMenu(field_table_->viewport()->mapToGlobal(pos), index.row());
    });
    connect(add_button, &QToolButton::clicked, this, [this] { addField(); });
    connect(delete_field_button, &QToolButton::clicked, this, [this] {
        int active = -1;
        {
            std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
            active = struct_dissector::g_state.active_struct;
        }
        if (selected_field_ >= 0 && active >= 0) {
            static_cast<void>(catalogEdit("Delete field", [&] {
                return struct_dissector::remove_field(active, selected_field_);
            }));
            selected_field_ = -1;
            editing_field_ = -1;
        } else {
            diag::log_tagged_fmt("dissector", "remove_field_skipped reason='no_selection'");
        }
    });
    connect(use_button, &QToolButton::clicked, this, [this] {
        // Staged-target accept (ports the "Use Target" action).
        auto& hooks = analysis_host_hooks();
        (void)hooks;
        std::string validation_error;
        auto& staged = staged_dissector_target_store();
        if (!staged.context) return;
        if (!staged.context->validate(validation_error)) {
            staged.stale = true;
            staged.status = validation_error.empty()
                ? "The retained structure target source is stale." : validation_error;
        } else {
            addr_edit_->setText(QString::number(staged.context->address, 16).toUpper());
            addr_seeded_ = true;
            {
                std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
                struct_dissector::g_state.base_address = staged.context->address;
            }
            staged.context.reset();
            staged_strip_->setVisible(false);
            applyBaseAddress();
        }
        refreshStagedStrip();
    });
    connect(recheck_button, &QToolButton::clicked, this, [this] {
        auto& staged = staged_dissector_target_store();
        if (!staged.context) return;
        std::string validation_error;
        staged.stale = !staged.context->validate(validation_error);
        staged.status = staged.stale
            ? (validation_error.empty()
                ? "The retained structure target source is stale." : validation_error)
            : "Exact source current at last check. Review before replacing the base address.";
        refreshStagedStrip();
    });
    connect(dismiss_button, &QToolButton::clicked, this, [this] {
        staged_dissector_target_store().context.reset();
        refreshStagedStrip();
    });

    struct_dissector::ensure_persistence_loaded();
    refreshStagedStrip();
    rebuildStructureList();
    refreshFieldModel();
    refreshPresentation();
}

QtStructDissectorView::~QtStructDissectorView() {
    if (active_instance_ == this) active_instance_ = nullptr;
}

void QtStructDissectorView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    timer_->start();
}

void QtStructDissectorView::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

void QtStructDissectorView::pollEngine() {
    auto& state = struct_dissector::g_state;
    std::uint64_t schema_revision = 0;
    std::uint64_t completed_seq = 0;
    int active_struct = -1;
    bool auto_refresh = false;
    float refresh_interval = 0.5f;
    {
        std::lock_guard<std::mutex> lock(state.mtx);
        schema_revision = state.schema_revision;
        active_struct = state.active_struct;
        auto_refresh = state.auto_refresh;
        refresh_interval = state.refresh_interval;
    }
    completed_seq = state.last_completed_seq.load(std::memory_order_acquire);
    if (schema_revision != last_schema_revision_ || active_struct != last_active_struct_) {
        last_schema_revision_ = schema_revision;
        last_active_struct_ = active_struct;
        rebuildStructureList();
        refreshFieldModel();
    } else if (completed_seq != last_completed_seq_) {
        last_completed_seq_ = completed_seq;
        auto* fields = qobject_cast<QtDissectorFieldModel*>(field_model_);
        if (fields) fields->syncValues();
    }
    if (auto_refresh) {
        auto_refresh_accum_ += 0.066f;
        if (auto_refresh_accum_ >= refresh_interval) {
            auto_refresh_accum_ = 0.f;
            struct_dissector::refresh_values();
        }
    }
    {
        std::lock_guard<std::mutex> lock(state.mtx);
        const int active = state.active_struct;
        if (struct_dissector::valid_index(active, state.structs.size())) {
            const auto& structure = state.structs[static_cast<std::size_t>(active)];
            if (validation_structure_id_ != structure.stable_id ||
                validation_revision_ != structure.layout_revision) {
                validation_ = struct_dissector::validate_structure(active);
                validation_structure_id_ = structure.stable_id;
                validation_revision_ = structure.layout_revision;
            }
        }
    }
    const std::size_t layout_errors = static_cast<std::size_t>(std::count_if(
        validation_.issues.begin(), validation_.issues.end(),
        [](const struct_dissector::layout_issue_t& issue) {
            return issue.severity == struct_dissector::layout_issue_severity_t::error;
        }));
    validation_label_->setText(layout_errors == 0
        ? QStringLiteral("Layout valid | size %1 | align %2")
            .arg(validation_.computed_size).arg(validation_.effective_alignment)
        : QStringLiteral("%1 layout error(s)").arg(layout_errors));
    const auto apply_variant = [](QLabel* label, const char* variant) {
        if (label->property("aidaVariant").toString() == QLatin1String(variant))
            return;
        label->setProperty("aidaVariant", QString::fromLatin1(variant));
        theme::stylesheet::repolish(label);
    };
    apply_variant(validation_label_, layout_errors == 0 ? "success" : "error");
    if (!operation_status_.empty()) {
        apply_variant(operation_label_, operation_error_ ? "error" : "secondary");
        operation_label_->setText(QString::fromStdString(operation_status_));
    }
    refreshPresentation();
}

void QtStructDissectorView::rebuildStructureList() {
    auto* model = qobject_cast<QtDissectorStructureModel*>(struct_model_);
    if (model)
        model->syncFromEngine(struct_filter_->text().toLower());
}

void QtStructDissectorView::refreshFieldModel() {
    auto* model = qobject_cast<QtDissectorFieldModel*>(field_model_);
    if (model) model->syncFromEngine();
    selected_field_ = -1;
    editing_field_ = -1;
}

void QtStructDissectorView::refreshPresentation() {
    const bool driver_loaded = driver_bridge::is_loaded();
    int active = -1;
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        active = struct_dissector::g_state.active_struct;
        count = struct_dissector::g_state.structs.size();
    }
    if (count == 0) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No structs yet"));
        state_view_->setMessage(QStringLiteral(
            "Create or import a structure to begin dissecting memory."));
        state_view_->setVisible(true);
        splitter_->setVisible(false);
        return;
    }
    if (active < 0) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No struct selected"));
        state_view_->setMessage(QStringLiteral(
            "Create or select a struct from the list to begin dissecting memory."));
        state_view_->setVisible(true);
        splitter_->setVisible(true);
        field_table_->setVisible(false);
        return;
    }
    state_view_->setVisible(false);
    splitter_->setVisible(true);
    field_table_->setVisible(true);
    if (!driver_loaded && operation_status_.empty()) {
        operation_status_ =
            "Dissector live values need an attached process. Attach via the debugger to enable Read/Write.";
        operation_error_ = true;
    }
}

void QtStructDissectorView::applyBaseAddress() {
    const std::string text = addr_edit_->text().toStdString();
    std::uint64_t address = 0;
    if (std::sscanf(text.c_str(), "%llx", reinterpret_cast<unsigned long long*>(
            &address)) == 1) {
        std::uint64_t previous = 0;
        {
            std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
            previous = struct_dissector::g_state.base_address;
            struct_dissector::g_state.base_address = address;
        }
        diag::log_tagged_fmt("dissector",
            "base_address_changed prev=0x%llX new=0x%llX",
            static_cast<unsigned long long>(previous),
            static_cast<unsigned long long>(address));
        struct_dissector::refresh_values();
    } else {
        diag::log_tagged_fmt("dissector", "base_address_parse_failed input='%s'",
            text.c_str());
    }
}

void QtStructDissectorView::addField() {
    const std::string name = field_name_edit_->text().toStdString();
    if (name.empty()) {
        diag::log_tagged_fmt("dissector", "add_field_skipped reason='empty_name'");
        return;
    }
    int active = -1;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        active = struct_dissector::g_state.active_struct;
    }
    if (active < 0) return;
    struct_dissector::field_def_t field;
    field.name = name;
    field.type = static_cast<struct_dissector::field_type_t>(
        field_type_combo_->currentIndex());
    std::uint32_t offset = 0;
    std::sscanf(field_offset_edit_->text().toStdString().c_str(), "%x", &offset);
    field.offset = offset;
    const std::size_t type_size = struct_dissector::field_type_size(field.type);
    field.size = static_cast<std::uint32_t>(type_size > 0 ? type_size : 1);
    const int added = catalogIndexEdit(
        pending_insert_index_ >= 0 ? "Insert field" : "Add field", [&] {
            return pending_insert_index_ >= 0
                ? struct_dissector::insert_field(active, pending_insert_index_, field)
                : struct_dissector::add_field(active, field);
        });
    operation_error_ = added < 0;
    operation_status_ = added < 0
        ? "Field insertion failed layout validation" : "Field added";
    operation_label_->setText(QString::fromStdString(operation_status_));
    pending_insert_index_ = -1;
    field_name_edit_->clear();
    field_offset_edit_->clear();
}

bool QtStructDissectorView::catalogEdit(const char* label,
                                        const std::function<bool()>& mutation) {
    std::string error;
    const bool applied = struct_dissector::perform_user_catalog_edit(label,
        [&]() -> bool { return mutation(); }, error);
    if (!applied && !error.empty()) {
        operation_error_ = true;
        operation_status_ = error;
        operation_label_->setText(QString::fromStdString(operation_status_));
    }
    return applied;
}

int QtStructDissectorView::catalogIndexEdit(const char* label,
                                            const std::function<int()>& mutation) {
    int result = -1;
    std::string error;
    const bool applied = struct_dissector::perform_user_catalog_edit(label, [&] {
        result = mutation();
        return result;
    }, [](int value) { return value >= 0; }, error);
    if (!applied) {
        result = -1;
        if (!error.empty()) {
            operation_error_ = true;
            operation_status_ = error;
            operation_label_->setText(QString::fromStdString(operation_status_));
        }
    }
    return result;
}

bool QtStructDissectorView::applyCatalogUndo() {
    std::string label;
    std::string error;
    const bool applied = struct_dissector::user_catalog_undo(label, error);
    operation_error_ = !applied;
    operation_status_ = applied ? "Undid " + label : std::move(error);
    operation_label_->setText(QString::fromStdString(operation_status_));
    if (applied) {
        selected_field_ = -1;
        editing_field_ = -1;
        validation_structure_id_ = 0;
        rebuildStructureList();
        refreshFieldModel();
    }
    return applied;
}

bool QtStructDissectorView::applyCatalogRedo() {
    std::string label;
    std::string error;
    const bool applied = struct_dissector::user_catalog_redo(label, error);
    operation_error_ = !applied;
    operation_status_ = applied ? "Redid " + label : std::move(error);
    operation_label_->setText(QString::fromStdString(operation_status_));
    if (applied) {
        selected_field_ = -1;
        editing_field_ = -1;
        validation_structure_id_ = 0;
        rebuildStructureList();
        refreshFieldModel();
    }
    return applied;
}

void QtStructDissectorView::publishFieldSelection(int field_index) {
    const auto context = disasm_view::capture_selected_workspace();
    if (!context.workspace) return;
    std::string structure_name;
    struct_dissector::field_def_t field;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        const int active = struct_dissector::g_state.active_struct;
        if (!struct_dissector::valid_index(active,
                struct_dissector::g_state.structs.size()))
            return;
        const auto& structure = struct_dissector::g_state.structs[
            static_cast<std::size_t>(active)];
        if (!struct_dissector::valid_index(field_index, structure.fields.size()))
            return;
        structure_name = structure.name;
        field = structure.fields[static_cast<std::size_t>(field_index)];
    }
    aida::workbench::selection_context_t selection;
    selection.kind = aida::workbench::selection_kind_t::entity;
    selection.entity_key = "structure.dissector." + structure_name + ".field." +
        std::to_string(field.offset);
    aida::workbench::document_local_cursor_t cursor;
    cursor.has_position = true;
    cursor.position = field.offset;
    aida::workbench::workbench_shell_workspace_context_t workbench;
    static_cast<void>(aida::workbench::workbench_shell_runtime_t::instance()
        .publish_selection(context.workspace, selection, cursor,
            aida::workbench::navigation_origin_t::inspector, workbench));
}

void QtStructDissectorView::refreshStagedStrip() {
    auto& staged = staged_dissector_target_store();
    const bool visible = staged.context.has_value();
    staged_strip_->setVisible(visible);
    if (!visible) return;
    staged_label_->setText(QStringLiteral("%1  |  generation %2  address 0x%3")
        .arg(QString::fromStdString(staged.status))
        .arg(staged.context->source_generation)
        .arg(staged.context->address, 16, 16, QLatin1Char('0')));
    staged_label_->setToolTip(QStringLiteral("%1\n%2\nGeneration %3  Address 0x%4")
        .arg(QString::fromStdString(staged.status))
        .arg(QString::fromStdString(staged.context->source_view))
        .arg(staged.context->source_generation)
        .arg(staged.context->address, 16, 16, QLatin1Char('0')));
}

bool QtStructDissectorView::stageTarget(staged_dissector_target_t context,
                                        std::string& error) {
    // Verbatim port of struct_dissector_view::stage_target_context.
    if (context.address == 0 || context.source_generation == 0 ||
        context.source_view.empty() || context.source_identity.empty() ||
        !context.validate ||
        (context.live_process && (context.target_pid == 0 || context.target_epoch == 0 ||
            context.process_creation_time_100ns == 0))) {
        error = "Structure target handoff requires an exact source identity, generation, and address.";
        return false;
    }
    if (context.source_view.size() > 128U || context.source_identity.size() > 512U) {
        error = "Structure target handoff metadata exceeds the bounded staging limit.";
        return false;
    }
    std::string validation_error;
    if (!context.validate(validation_error)) {
        error = validation_error.empty() ? "The structure target source is stale."
            : validation_error;
        return false;
    }
    staged_dissector_target_store().context = std::move(context);
    staged_dissector_target_store().status =
        "Review the retained source identity before using this base address.";
    staged_dissector_target_store().stale = false;
    error.clear();
    refreshStagedStrip();
    return true;
}

bool QtStructDissectorView::stageWriteReview(int structure_index, int field_index,
    const struct_dissector::field_def_t& field,
    const struct_dissector::live_value_t& value, std::uint64_t base_address,
    const char* text, std::string& error) {
    const auto context = disasm_view::capture_selected_workspace();
    if (!write_review_) write_review_ = new QtWriteReviewDialog(this);
    return write_review_->stage(context, structure_index, field_index, field, value,
        base_address, text, error);
}

void QtStructDissectorView::openEnumManager(std::uint64_t selected_enum_id) {
    if (!enum_manager_)
        enum_manager_ = new QtEnumManagerDialog(this);
    enum_manager_->setSelectedEnum(selected_enum_id);
    enum_manager_->open();
}

void QtStructDissectorView::openLayoutConfig() {
    if (!layout_dialog_) layout_dialog_ = new QtDissectorLayoutDialog(this);
    layout_dialog_->open();
}

void QtStructDissectorView::showInlineEdit(int target, int field_index,
                                           const std::string& seed) {
    edit_target_ = target;
    edit_target_field_ = field_index;
    edit_structure_id_ = 0;
    edit_structure_revision_ = 0;
    edit_field_id_ = 0;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        const int active = struct_dissector::g_state.active_struct;
        if (struct_dissector::valid_index(active,
                struct_dissector::g_state.structs.size())) {
            const auto& structure = struct_dissector::g_state.structs[
                static_cast<std::size_t>(active)];
            if (struct_dissector::valid_index(field_index, structure.fields.size())) {
                edit_structure_id_ = structure.stable_id;
                edit_structure_revision_ = structure.layout_revision;
                edit_field_id_ = structure.fields[
                    static_cast<std::size_t>(field_index)].stable_id;
            }
        }
    }
    if (inline_edit_dialog_) {
        inline_edit_dialog_->deleteLater();
        inline_edit_dialog_ = nullptr;
    }
    inline_edit_dialog_ = new QtDissectorInlineEditDialog(this);
    auto* dialog = qobject_cast<QtDissectorInlineEditDialog*>(inline_edit_dialog_);
    dialog->setEditTarget(target, seed);
    connect(dialog, &QDialog::accepted, this, [this] { commitInlineEdit(); });
    dialog->open();
    diag::log_tagged_fmt("dissector", "inline_edit_open kind=%d field_idx=%d",
        target, field_index);
}

void QtStructDissectorView::openFieldEdit(int target, int field_index,
    std::string seed, std::uint64_t structure_id, std::uint64_t structure_revision,
    std::uint64_t field_id) {
    edit_target_ = target;
    edit_target_field_ = field_index;
    edit_structure_id_ = structure_id;
    edit_structure_revision_ = structure_revision;
    edit_field_id_ = field_id;
    if (inline_edit_dialog_) {
        inline_edit_dialog_->deleteLater();
        inline_edit_dialog_ = nullptr;
    }
    inline_edit_dialog_ = new QtDissectorInlineEditDialog(this);
    auto* dialog = qobject_cast<QtDissectorInlineEditDialog*>(inline_edit_dialog_);
    dialog->setEditTarget(target, seed);
    connect(dialog, &QDialog::accepted, this, [this] { commitInlineEdit(); });
    dialog->open();
    diag::log_tagged_fmt("dissector", "inline_edit_open kind=%d field_idx=%d",
        target, field_index);
}

void QtStructDissectorView::confirmRemoveField(std::uint64_t structure_id,
    std::uint64_t structure_revision, std::uint64_t field_id) {
    pending_remove_structure_id_ = structure_id;
    pending_remove_structure_revision_ = structure_revision;
    pending_remove_field_id_ = field_id;
    if (remove_dialog_) {
        remove_dialog_->deleteLater();
        remove_dialog_ = nullptr;
    }
    remove_dialog_ = new QtDissectorRemoveFieldDialog(this);
    connect(remove_dialog_, &QDialog::accepted, this, [this] {
        int structure_index = -1;
        int target = -1;
        const auto pending_field_id = pending_remove_field_id_;
        {
            std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
            structure_index = struct_dissector::structure_index_by_id_locked(
                pending_remove_structure_id_);
            if (struct_dissector::valid_index(structure_index,
                    struct_dissector::g_state.structs.size())) {
                const auto& structure = struct_dissector::g_state.structs[
                    static_cast<std::size_t>(structure_index)];
                if (structure.stable_id == pending_remove_structure_id_ &&
                    structure.layout_revision == pending_remove_structure_revision_) {
                    const auto found = std::find_if(structure.fields.begin(),
                        structure.fields.end(), [pending_field_id](const auto& field) {
                            return field.stable_id == pending_field_id;
                        });
                    if (found != structure.fields.end()) {
                        const auto distance = static_cast<std::size_t>(
                            std::distance(structure.fields.begin(), found));
                        if (struct_dissector::index_fits_int(distance))
                            target = static_cast<int>(distance);
                    }
                }
            }
        }
        if (target >= 0 && catalogEdit("Delete field", [&] {
            return struct_dissector::remove_field(structure_index, target);
        })) {
            if (selected_field_ == target) selected_field_ = -1;
            if (editing_field_ == target) editing_field_ = -1;
        }
        pending_remove_structure_id_ = 0;
        pending_remove_structure_revision_ = 0;
        pending_remove_field_id_ = 0;
    });
    remove_dialog_->open();
}

void QtStructDissectorView::commitInlineEdit() {
    auto* dialog = qobject_cast<QtDissectorInlineEditDialog*>(inline_edit_dialog_);
    if (!dialog) return;
    const std::string text = dialog->editText().toStdString();
    int mutation_struct_idx = -1;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        mutation_struct_idx = struct_dissector::g_state.active_struct;
    }
    int target_field = edit_target_field_;
    bool edit_identity_valid = true;
    if (edit_structure_id_ != 0 && edit_field_id_ != 0) {
        edit_identity_valid = false;
        const auto edit_field_id = edit_field_id_;
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        mutation_struct_idx = struct_dissector::structure_index_by_id_locked(
            edit_structure_id_);
        if (struct_dissector::valid_index(mutation_struct_idx,
                struct_dissector::g_state.structs.size())) {
            const auto& structure = struct_dissector::g_state.structs[
                static_cast<std::size_t>(mutation_struct_idx)];
            if (structure.stable_id == edit_structure_id_ &&
                structure.layout_revision == edit_structure_revision_) {
                const auto found = std::find_if(structure.fields.begin(),
                    structure.fields.end(), [edit_field_id](const auto& field) {
                        return field.stable_id == edit_field_id;
                    });
                if (found != structure.fields.end()) {
                    const auto distance = static_cast<std::size_t>(
                        std::distance(structure.fields.begin(), found));
                    if (struct_dissector::index_fits_int(distance)) {
                        target_field = static_cast<int>(distance);
                        edit_identity_valid = true;
                    }
                }
            }
        }
    }
    bool applied = false;
    if (edit_identity_valid) {
        switch (static_cast<dissector_edit_target_t>(edit_target_)) {
        case dissector_edit_target_t::field_name:
            if (target_field >= 0 && !text.empty())
                applied = catalogEdit("Rename field", [&] {
                    return struct_dissector::rename_field(mutation_struct_idx,
                        target_field, text);
                });
            break;
        case dissector_edit_target_t::field_size: {
            std::uint32_t new_size = 0;
            if (std::sscanf(text.c_str(), "%u", &new_size) == 1 && new_size > 0)
                applied = catalogEdit("Resize field", [&] {
                    return struct_dissector::set_field_size(mutation_struct_idx,
                        target_field, new_size);
                });
            else
                diag::log_tagged_fmt("dissector",
                    "set_field_size_input_invalid input='%s'", text.c_str());
            break;
        }
        case dissector_edit_target_t::field_comment:
            if (target_field >= 0)
                applied = catalogEdit("Edit field comment", [&] {
                    return struct_dissector::set_field_comment(mutation_struct_idx,
                        target_field, text);
                });
            break;
        case dissector_edit_target_t::struct_name:
            if (mutation_struct_idx >= 0 && !text.empty())
                applied = catalogEdit("Rename structure", [&] {
                    return struct_dissector::rename_struct(mutation_struct_idx, text);
                });
            break;
        case dissector_edit_target_t::array_count: {
            unsigned long value = 0;
            char* end = nullptr;
            value = std::strtoul(text.c_str(), &end, 0);
            if (end && *end == '\0' && value <= 1048576)
                applied = catalogEdit("Set field array count", [&] {
                    return struct_dissector::set_field_array_count(mutation_struct_idx,
                        target_field, static_cast<std::uint32_t>(value));
                });
            break;
        }
        case dissector_edit_target_t::nested_target:
            applied = catalogEdit("Set nested structure", [&] {
                return struct_dissector::set_field_nested_target_by_name(
                    mutation_struct_idx, target_field, text, false);
            });
            break;
        case dissector_edit_target_t::pointer_target:
            applied = catalogEdit("Set pointer target", [&] {
                return struct_dissector::set_field_nested_target_by_name(
                    mutation_struct_idx, target_field, text, true);
            });
            break;
        case dissector_edit_target_t::enum_reference:
            applied = catalogEdit("Set field enum", [&] {
                return struct_dissector::set_field_enum_reference(mutation_struct_idx,
                    target_field, text);
            });
            break;
        case dissector_edit_target_t::bitfield: {
            if (text == "none" || text == "0")
                applied = catalogEdit("Clear field bitfield", [&] {
                    return struct_dissector::set_field_bitfield(mutation_struct_idx,
                        target_field, 0, 0);
                });
            else {
                unsigned int offset = 0;
                unsigned int width = 0;
                if (std::sscanf(text.c_str(), "%u:%u", &offset, &width) == 2 &&
                    offset <= 65535 && width <= 65535)
                    applied = catalogEdit("Configure field bitfield", [&] {
                        return struct_dissector::set_field_bitfield(mutation_struct_idx,
                            target_field, static_cast<std::uint16_t>(offset),
                            static_cast<std::uint16_t>(width));
                    });
            }
            break;
        }
        case dissector_edit_target_t::field_alignment: {
            unsigned int alignment = 0;
            if (std::sscanf(text.c_str(), "%u", &alignment) == 1 && alignment <= 4096)
                applied = catalogEdit("Set field alignment", [&] {
                    return struct_dissector::set_field_alignment(mutation_struct_idx,
                        target_field, static_cast<std::uint16_t>(alignment));
                });
            break;
        }
        default: break;
        }
    }
    operation_error_ = !applied;
    operation_status_ = applied ? "Structure layout updated"
        : "Layout change rejected; check ranges, overlap, recursion, and alignment";
    operation_label_->setText(QString::fromStdString(operation_status_));
    edit_target_ = 0;
    edit_target_field_ = -1;
    edit_structure_id_ = 0;
    edit_structure_revision_ = 0;
    edit_field_id_ = 0;
}

}
