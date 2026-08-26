#pragma once

#include <QDialog>

#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/struct_dissector.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace aida::qt::analysis {

class QtStructDissectorView;

// Inline field edit dialog (07 sec. 6.2; replaces the ##sd_inline_edit popup).
class QtDissectorInlineEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtDissectorInlineEditDialog(QtStructDissectorView* parent);
    void setEditTarget(int target, const std::string& seed);
    QString editText() const;

private:
    int target_ = 0;
    QLabel* hint_label_ = nullptr;
    QLineEdit* edit_ = nullptr;
    QPushButton* commit_ = nullptr;
};

// Remove-field confirmation (##sd_confirm_remove_field).
class QtDissectorRemoveFieldDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtDissectorRemoveFieldDialog(QWidget* parent);
};

// Structure layout configuration popup (##sd_layout_config): convert kind,
// packing/alignment, enum manager entry, catalog save/load.
class QtDissectorLayoutDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtDissectorLayoutDialog(QtStructDissectorView* parent);

private:
    QtStructDissectorView* view_ = nullptr;
    QLineEdit* pack_edit_ = nullptr;
    QLineEdit* align_edit_ = nullptr;
    QLabel* status_ = nullptr;
};

// Enum Manager (types.enum.manager). Full form validation pipeline preserved.
class QtEnumManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtEnumManagerDialog(QtStructDissectorView* parent);
    void setSelectedEnum(std::uint64_t stable_id);
    void open();

private:
    void rebuildCatalog();
    void refreshForm();
    void saveEnum();
    void deleteEnum();

    QtStructDissectorView* view_ = nullptr;
    QComboBox* enum_list_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QComboBox* underlying_combo_ = nullptr;
    QPlainTextEdit* values_edit_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* delete_button_ = nullptr;
    std::uint64_t selected_enum_id_ = 0;
    std::uint64_t draft_schema_revision_ = 0;
    std::uint64_t catalog_revision_ = 0;
    std::vector<struct_dissector::enum_def_t> catalog_snapshot_;
};

}
