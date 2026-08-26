#include "qt/analysis/qt_dissector_dialogs.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unordered_set>

#include "qt/analysis/qt_struct_dissector_view.hpp"

namespace aida::qt::analysis {

namespace {

std::string trim_enum_token(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string export_enum_to_c(const struct_dissector::enum_def_t& definition) {
    constexpr std::size_t maximum_output = 64U * 1024U;
    if (definition.name.size() > maximum_output || definition.values.size() > 65536)
        return {};
    std::string output;
    const auto append = [&output](const std::string& value) {
        if (value.size() > maximum_output - output.size()) return false;
        output.append(value);
        return true;
    };
    if (!append("enum ") || !append(definition.name) || !append(" {\n")) return {};
    for (const auto& value : definition.values) {
        if (!append("    ") || !append(value.name) || !append(" = ") ||
            !append(std::to_string(value.value)) || !append(",\n"))
            return {};
    }
    return append("};\n") ? output : std::string{};
}

}

// ---------------------------------------------------------------- inline edit

QtDissectorInlineEditDialog::QtDissectorInlineEditDialog(
    QtStructDissectorView* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("aida.dialog.dissector.inline_edit"));
    setModal(true);
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    hint_label_ = new QLabel(this);
    layout->addWidget(hint_label_);
    edit_ = new QLineEdit(this);
    layout->addWidget(edit_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this);
    commit_ = buttons->button(QDialogButtonBox::Ok);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(edit_, &QLineEdit::returnPressed, this, &QDialog::accept);
}

void QtDissectorInlineEditDialog::setEditTarget(int target,
                                                const std::string& seed) {
    target_ = target;
    const auto kind = static_cast<dissector_edit_target_t>(target);
    const char* hint = "rename";
    const char* commit = "Rename";
    switch (kind) {
    case dissector_edit_target_t::field_name: hint = "field name"; commit = "Rename"; break;
    case dissector_edit_target_t::field_size: hint = "new size"; commit = "Set Size"; break;
    case dissector_edit_target_t::field_comment: hint = "comment"; commit = "Set Comment"; break;
    case dissector_edit_target_t::struct_name: hint = "struct name"; commit = "Rename"; break;
    case dissector_edit_target_t::array_count:
        hint = "array count (1-1048576)"; commit = "Set Count"; break;
    case dissector_edit_target_t::nested_target:
        hint = "exact structure name"; commit = "Set Type"; break;
    case dissector_edit_target_t::pointer_target:
        hint = "exact pointee structure name"; commit = "Set Pointer"; break;
    case dissector_edit_target_t::enum_reference:
        hint = "exact enum name"; commit = "Set Enum"; break;
    case dissector_edit_target_t::bitfield:
        hint = "bit offset:width or none"; commit = "Set Bits"; break;
    case dissector_edit_target_t::field_alignment:
        hint = "alignment (0 or power of two)"; commit = "Align"; break;
    default: break;
    }
    hint_label_->setText(QString::fromLatin1(hint));
    hint_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    commit_->setText(QString::fromLatin1(commit));
    setWindowTitle(QStringLiteral("Structure Dissector - %1")
        .arg(QString::fromLatin1(commit)));
    edit_->setText(QString::fromStdString(seed));
    edit_->selectAll();
}

QString QtDissectorInlineEditDialog::editText() const { return edit_->text(); }

// ------------------------------------------------------------- remove confirm

QtDissectorRemoveFieldDialog::QtDissectorRemoveFieldDialog(QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("aida.dialog.dissector.confirm_remove_field"));
    setModal(true);
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(360, 240);
    resize(500, 300);
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(QStringLiteral(
        "Remove this field from the structure definition?"), this);
    title->setWordWrap(true);
    layout->addWidget(title);
    auto* note = new QLabel(QStringLiteral(
        "The catalog edit can be restored with Undo after removal."), this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Remove"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// ------------------------------------------------------------- layout config

QtDissectorLayoutDialog::QtDissectorLayoutDialog(QtStructDissectorView* parent)
    : QDialog(parent), view_(parent) {
    setObjectName(QStringLiteral("aida.dialog.dissector.layout_config"));
    setWindowTitle(QStringLiteral("Structure layout"));
    setModal(true);
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    int structure_index = -1;
    struct_dissector::structure_kind_t kind =
        struct_dissector::structure_kind_t::structure;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        structure_index = struct_dissector::g_state.active_struct;
        if (struct_dissector::valid_index(structure_index,
                struct_dissector::g_state.structs.size()))
            kind = struct_dissector::g_state.structs[
                static_cast<std::size_t>(structure_index)].kind;
    }
    auto* convert = new QPushButton(kind == struct_dissector::structure_kind_t::union_type
        ? QStringLiteral("Convert to Struct") : QStringLiteral("Convert to Union"), this);
    layout->addWidget(convert);
    auto* form = new QFormLayout();
    pack_edit_ = new QLineEdit(this);
    pack_edit_->setPlaceholderText(QStringLiteral("packing: 0,1,2,4,8,16"));
    form->addRow(QStringLiteral("Packing"), pack_edit_);
    align_edit_ = new QLineEdit(this);
    align_edit_->setPlaceholderText(QStringLiteral("alignment: 0,1,2,4,8,16"));
    form->addRow(QStringLiteral("Alignment"), align_edit_);
    layout->addLayout(form);
    auto* set_pack = new QPushButton(QStringLiteral("Set Pack"), this);
    auto* set_align = new QPushButton(QStringLiteral("Set Align"), this);
    auto* buttons_row = new QHBoxLayout();
    buttons_row->addWidget(set_pack);
    buttons_row->addWidget(set_align);
    layout->addLayout(buttons_row);
    auto* manage_enums = new QPushButton(QStringLiteral("Manage Enums..."), this);
    layout->addWidget(manage_enums);
    const bool persistence_running =
        struct_dissector::g_state.persistence_in_flight.load(std::memory_order_acquire);
    auto* save_catalog = new QPushButton(QStringLiteral("Save Catalog"), this);
    auto* load_catalog = new QPushButton(QStringLiteral("Load Catalog"), this);
    save_catalog->setEnabled(!persistence_running);
    load_catalog->setEnabled(!persistence_running);
    auto* persistence_row = new QHBoxLayout();
    persistence_row->addWidget(save_catalog);
    persistence_row->addWidget(load_catalog);
    layout->addLayout(persistence_row);
    auto* cancel_persistence = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_persistence->setVisible(persistence_running);
    layout->addWidget(cancel_persistence);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        status_->setText(QString::fromStdString(
            struct_dissector::g_state.persistence_status));
    }
    layout->addWidget(status_);

    connect(convert, &QPushButton::clicked, this, [this, structure_index, kind] {
        const bool applied = view_ && view_->catalogEdit("Convert structure kind", [&] {
            return struct_dissector::set_structure_kind(structure_index,
                kind == struct_dissector::structure_kind_t::union_type
                    ? struct_dissector::structure_kind_t::structure
                    : struct_dissector::structure_kind_t::union_type);
        });
        status_->setText(applied ? QStringLiteral("Structure kind updated")
            : QStringLiteral("Structure kind change rejected"));
    });
    connect(set_pack, &QPushButton::clicked, this, [this, structure_index] {
        unsigned int value = 0;
        const bool parsed = std::sscanf(pack_edit_->text().toStdString().c_str(), "%u",
            &value) == 1 && value <= 4096;
        const bool applied = parsed && view_ &&
            view_->catalogEdit("Set structure packing", [&] {
                return struct_dissector::set_structure_packing(structure_index,
                    static_cast<std::uint16_t>(value));
            });
        status_->setText(applied ? QStringLiteral("Packing updated")
            : QStringLiteral("Packing must be 0 or a power of two up to 4096"));
    });
    connect(set_align, &QPushButton::clicked, this, [this, structure_index] {
        unsigned int value = 0;
        const bool parsed = std::sscanf(align_edit_->text().toStdString().c_str(), "%u",
            &value) == 1 && value <= 4096;
        const bool applied = parsed && view_ &&
            view_->catalogEdit("Set structure alignment", [&] {
                return struct_dissector::set_structure_alignment(structure_index,
                    static_cast<std::uint16_t>(value));
            });
        status_->setText(applied ? QStringLiteral("Structure alignment updated")
            : QStringLiteral("Alignment must be 0 or a power of two up to 4096"));
    });
    connect(manage_enums, &QPushButton::clicked, this, [this] {
        if (view_) view_->openEnumManager();
        close();
    });
    connect(save_catalog, &QPushButton::clicked, this, [] {
        struct_dissector::request_save_schema();
    });
    connect(load_catalog, &QPushButton::clicked, this, [] {
        struct_dissector::request_load_schema();
    });
    connect(cancel_persistence, &QPushButton::clicked, this, [] {
        struct_dissector::cancel_persistence();
    });
}

// ---------------------------------------------------------------- enum manager

QtEnumManagerDialog::QtEnumManagerDialog(QtStructDissectorView* parent)
    : QDialog(parent), view_(parent) {
    setObjectName(QStringLiteral("aida.dialog.types.enum_manager"));
    setWindowTitle(QStringLiteral("Enum Manager"));
    setModal(true);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 560);
    setMinimumSize(420, 320);
    auto* layout = new QHBoxLayout(this);
    auto* left = new QVBoxLayout();
    enum_list_ = new QComboBox(this);
    left->addWidget(enum_list_);
    delete_button_ = new QPushButton(QStringLiteral("Delete Enum..."), this);
    left->addWidget(delete_button_);
    layout->addLayout(left, 1);
    auto* right = new QVBoxLayout();
    auto* form = new QFormLayout();
    name_edit_ = new QLineEdit(this);
    form->addRow(QStringLiteral("Name"), name_edit_);
    underlying_combo_ = new QComboBox(this);
    static const char* integer_types[] = {
        "Int8", "UInt8", "Int16", "UInt16", "Int32", "UInt32", "Int64", "UInt64"
    };
    for (const char* type : integer_types)
        underlying_combo_->addItem(QString::fromLatin1(type));
    underlying_combo_->setCurrentIndex(4);
    form->addRow(QStringLiteral("Underlying type"), underlying_combo_);
    right->addLayout(form);
    right->addWidget(new QLabel(QStringLiteral(
        "Values (one NAME=VALUE entry per line)"), this));
    values_edit_ = new QPlainTextEdit(this);
    right->addWidget(values_edit_, 1);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    right->addWidget(status_);
    save_button_ = new QPushButton(QStringLiteral("Save Enum"), this);
    right->addWidget(save_button_);
    layout->addLayout(right, 2);

    connect(enum_list_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= catalog_snapshot_.size())
            return;
        const auto& definition = catalog_snapshot_[static_cast<std::size_t>(index)];
        selected_enum_id_ = definition.stable_id;
        name_edit_->setText(QString::fromStdString(definition.name));
        underlying_combo_->setCurrentIndex(static_cast<int>(definition.underlying_type) -
            static_cast<int>(struct_dissector::field_type_t::int8));
        std::string values;
        for (const auto& value : definition.values) {
            if (!values.empty()) values.push_back('\n');
            values += value.name + "=" + std::to_string(value.value);
        }
        values_edit_->setPlainText(QString::fromStdString(values));
        refreshForm();
    });
    connect(name_edit_, &QLineEdit::textChanged, this, [this](const QString&) {
        refreshForm();
    });
    connect(values_edit_, &QPlainTextEdit::textChanged, this, [this] {
        refreshForm();
    });
    connect(underlying_combo_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshForm(); });
    connect(save_button_, &QPushButton::clicked, this, [this] { saveEnum(); });
    connect(delete_button_, &QPushButton::clicked, this, [this] { deleteEnum(); });
}

void QtEnumManagerDialog::setSelectedEnum(std::uint64_t stable_id) {
    selected_enum_id_ = stable_id;
}

void QtEnumManagerDialog::open() {
    rebuildCatalog();
    refreshForm();
    QDialog::open();
}

void QtEnumManagerDialog::rebuildCatalog() {
    auto& state = struct_dissector::g_state;
    std::lock_guard<std::mutex> lock(state.mtx);
    if (catalog_revision_ != state.schema_revision) {
        catalog_snapshot_ = state.enums;
        catalog_revision_ = state.schema_revision;
    }
    enum_list_->blockSignals(true);
    enum_list_->clear();
    int selected_index = -1;
    for (std::size_t index = 0; index < catalog_snapshot_.size(); ++index) {
        enum_list_->addItem(QString::fromStdString(catalog_snapshot_[index].name));
        if (catalog_snapshot_[index].stable_id == selected_enum_id_)
            selected_index = static_cast<int>(index);
    }
    if (selected_index >= 0)
        enum_list_->setCurrentIndex(selected_index);
    enum_list_->blockSignals(false);
}

void QtEnumManagerDialog::refreshForm() {
    struct_dissector::enum_def_t draft;
    draft.stable_id = selected_enum_id_;
    draft.name = trim_enum_token(name_edit_->text().toStdString());
    draft.underlying_type = static_cast<struct_dissector::field_type_t>(
        static_cast<int>(struct_dissector::field_type_t::int8) +
        underlying_combo_->currentIndex());
    std::string error;
    if (draft.name.empty())
        error = "Enter an enum name.";
    else if (draft.name.size() > 256)
        error = "Enum names are limited to 256 bytes.";
    const int underlying = static_cast<int>(draft.underlying_type);
    if (underlying < static_cast<int>(struct_dissector::field_type_t::int8) ||
        underlying > static_cast<int>(struct_dissector::field_type_t::uint64))
        error = "Choose an integer underlying type.";
    std::unordered_set<std::string> names;
    const std::string source = values_edit_->toPlainText().toStdString();
    std::size_t cursor = 0;
    std::size_t line_number = 1;
    while (cursor <= source.size()) {
        const std::size_t end = source.find('\n', cursor);
        std::string line = trim_enum_token(source.substr(cursor,
            end == std::string::npos ? std::string::npos : end - cursor));
        if (!line.empty()) {
            const std::size_t separator = line.find('=');
            if (separator == std::string::npos) {
                error = "Line " + std::to_string(line_number) + " must use NAME=VALUE.";
                break;
            }
            std::string name = trim_enum_token(line.substr(0, separator));
            const std::string number_text = trim_enum_token(line.substr(separator + 1));
            char* number_end = nullptr;
            errno = 0;
            const long long value = std::strtoll(number_text.c_str(), &number_end, 0);
            if (name.empty() || name.size() > 256) {
                error = "Line " + std::to_string(line_number) +
                    " has an invalid value name.";
                break;
            }
            if (!names.insert(name).second) {
                error = "Line " + std::to_string(line_number) + " repeats value name " +
                    name + ".";
                break;
            }
            if (number_text.empty() || errno == ERANGE || !number_end ||
                *number_end != '\0') {
                error = "Line " + std::to_string(line_number) +
                    " has an invalid integer value.";
                break;
            }
            draft.values.push_back({std::move(name),
                static_cast<std::int64_t>(value)});
            if (draft.values.size() > 65536) {
                error = "An enum is limited to 65,536 values.";
                break;
            }
        }
        if (end == std::string::npos) break;
        cursor = end + 1;
        ++line_number;
    }
    auto& state = struct_dissector::g_state;
    {
        std::lock_guard<std::mutex> lock(state.mtx);
        draft_schema_revision_ = state.schema_revision;
        const auto selected = selected_enum_id_ == 0 ? state.enums.end()
            : std::find_if(state.enums.begin(), state.enums.end(), [&](const auto& item) {
                return item.stable_id == selected_enum_id_;
            });
        if (selected_enum_id_ != 0 && selected == state.enums.end())
            error = "The selected enum no longer exists. Select it again.";
        if (std::any_of(state.enums.begin(), state.enums.end(), [&](const auto& item) {
            return item.name == draft.name && item.stable_id != selected_enum_id_;
        }))
            error = "Another enum already uses this name.";
        if (selected != state.enums.end() &&
            selected->underlying_type != draft.underlying_type &&
            std::any_of(state.structs.begin(), state.structs.end(),
                [&](const auto& structure) {
                    return std::any_of(structure.fields.begin(), structure.fields.end(),
                        [&](const auto& field) {
                            return field.enum_id == selected->stable_id;
                        });
                }))
            error = "Referenced enums cannot change their underlying storage type.";
    }
    status_->setText(QString::fromStdString(error));
    save_button_->setEnabled(error.empty());
}

void QtEnumManagerDialog::saveEnum() {
    struct_dissector::enum_def_t draft;
    draft.stable_id = selected_enum_id_;
    draft.name = trim_enum_token(name_edit_->text().toStdString());
    draft.underlying_type = static_cast<struct_dissector::field_type_t>(
        static_cast<int>(struct_dissector::field_type_t::int8) +
        underlying_combo_->currentIndex());
    const std::string source = values_edit_->toPlainText().toStdString();
    std::size_t cursor = 0;
    while (cursor <= source.size()) {
        const std::size_t end = source.find('\n', cursor);
        std::string line = trim_enum_token(source.substr(cursor,
            end == std::string::npos ? std::string::npos : end - cursor));
        if (!line.empty()) {
            const std::size_t separator = line.find('=');
            if (separator != std::string::npos) {
                std::string name = trim_enum_token(line.substr(0, separator));
                const std::string number_text = trim_enum_token(line.substr(separator + 1));
                char* number_end = nullptr;
                errno = 0;
                const long long value = std::strtoll(number_text.c_str(), &number_end, 0);
                if (!name.empty() && errno != ERANGE && number_end &&
                    *number_end == '\0' && !number_text.empty())
                    draft.values.push_back({std::move(name),
                        static_cast<std::int64_t>(value)});
            }
        }
        if (end == std::string::npos) break;
        cursor = end + 1;
    }
    const bool creating = draft.stable_id == 0;
    const bool saved = view_ && view_->catalogEdit("Save enum", [&] {
        return struct_dissector::upsert_enum_exact(draft, draft_schema_revision_);
    });
    status_->setText(saved ? QStringLiteral("Enum catalog updated")
        : QStringLiteral("Enum rejected because its catalog identity or references changed"));
    if (saved && creating) {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        const auto found = std::find_if(struct_dissector::g_state.enums.begin(),
            struct_dissector::g_state.enums.end(), [&](const auto& item) {
                return item.name == draft.name;
            });
        if (found != struct_dissector::g_state.enums.end())
            selected_enum_id_ = found->stable_id;
    }
    rebuildCatalog();
}

void QtEnumManagerDialog::deleteEnum() {
    if (selected_enum_id_ == 0) return;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        const auto found = std::find_if(struct_dissector::g_state.enums.begin(),
            struct_dissector::g_state.enums.end(), [&](const auto& item) {
                return item.stable_id == selected_enum_id_;
            });
        if (found == struct_dissector::g_state.enums.end()) {
            status_->setText(QStringLiteral(
                "The selected enum changed; select it again"));
            return;
        }
        name = found->name;
    }
    // The delete review is the confirmation dialog content (identity + zero
    // references revalidated at delete time).
    std::size_t reference_count = 0;
    bool exact_identity = false;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        const auto found = std::find_if(struct_dissector::g_state.enums.begin(),
            struct_dissector::g_state.enums.end(), [&](const auto& item) {
                return item.stable_id == selected_enum_id_ && item.name == name;
            });
        exact_identity = found != struct_dissector::g_state.enums.end() &&
            struct_dissector::g_state.schema_revision == catalog_revision_;
        if (exact_identity) {
            for (const auto& structure : struct_dissector::g_state.structs)
                reference_count += static_cast<std::size_t>(std::count_if(
                    structure.fields.begin(), structure.fields.end(),
                    [&](const auto& field) {
                        return field.enum_id == selected_enum_id_;
                    }));
        }
    }
    if (!exact_identity) {
        status_->setText(QStringLiteral(
            "The enum catalog changed after review began. Cancel and select the current entry again."));
        return;
    }
    if (reference_count != 0) {
        status_->setText(QStringLiteral(
            "%1 structure field reference(s) must be removed first.")
            .arg(reference_count));
        return;
    }
    const bool deleted = view_ && view_->catalogEdit("Delete enum", [&] {
        return struct_dissector::delete_enum_exact(selected_enum_id_, name,
            catalog_revision_);
    });
    status_->setText(deleted ? QStringLiteral("Enum deleted")
        : QStringLiteral(
            "Enum deletion rejected because its identity, revision, or references changed"));
    if (deleted) {
        selected_enum_id_ = 0;
        name_edit_->clear();
        values_edit_->clear();
    }
    rebuildCatalog();
}

}
