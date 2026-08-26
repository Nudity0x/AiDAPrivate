#include "qt/analysis/qt_struct_recon_view.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>

#include "helpers/diag_log.hpp"

#include "core/analysis/struct_dissector.hpp"
#include "core/analysis/struct_monitor.hpp"
#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/disasm/function_index.hpp"
#include "core/infra/executor.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/ui/task_center.hpp"
#include "core/ai/entity_evidence_handoff.hpp"
#include "core/workbench/workbench_shell_integration.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_declaration_review_dialog.hpp"
#include "qt/analysis/qt_struct_dissector_view.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::analysis {

using widgets::with_alpha;

namespace {

bool checked_field_address(std::uint64_t base, std::uint64_t offset,
                           std::uint64_t& result) {
    if (offset > (std::numeric_limits<std::uint64_t>::max)() - base)
        return false;
    result = base + offset;
    return true;
}

std::uint64_t field_identity_hash(const struct_recon::struct_field_t& field) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    const auto mix_string = [&mix](const std::string& value) {
        mix(value.size());
        for (const char character : value)
            mix(static_cast<std::uint64_t>(static_cast<unsigned char>(character)));
    };
    const auto mix_bytes = [&mix](const std::vector<std::uint8_t>& value) {
        mix(value.size());
        for (const auto byte : value) mix(byte);
    };
    mix_string(field.name);
    mix(static_cast<std::uint64_t>(field.type));
    mix(field.offset);
    mix(static_cast<std::uint64_t>(field.size));
    mix_string(field.comment);
    std::uint32_t confidence_bits = 0;
    static_assert(sizeof(confidence_bits) == sizeof(field.type_confidence));
    std::memcpy(&confidence_bits, &field.type_confidence, sizeof(confidence_bits));
    mix(confidence_bits);
    mix(static_cast<std::uint64_t>(field.array_count));
    mix(static_cast<std::uint64_t>(field.value_history.count));
    mix(static_cast<std::uint64_t>(field.value_history.write_idx));
    for (const auto value : field.value_history.values) mix(value);
    mix(field.vtable_entries.size());
    for (const auto& entry : field.vtable_entries) {
        mix(entry.func_addr);
        mix(static_cast<std::uint64_t>(entry.index));
        mix_string(entry.name);
    }
    mix(field.accesses.size());
    for (const auto& access : field.accesses) {
        mix(access.instruction_addr);
        mix(access.access_offset);
        mix(static_cast<std::uint64_t>(access.access_size));
        mix(access.is_write ? 1U : 0U);
        mix_string(access.disasm_text);
        mix(static_cast<std::uint64_t>(access.hit_count));
        mix_string(access.source);
        mix(access.thread_id);
        mix(access.sample_index);
        mix(access.capture_session_id);
        mix(access.initial_value_captured ? 1U : 0U);
        mix(access.initial_value);
        mix_bytes(access.initial_bytes);
        mix(access.value_captured ? 1U : 0U);
        mix(access.value_after_access ? 1U : 0U);
        mix(access.observed_value);
        mix_bytes(access.observed_bytes);
    }
    return hash == 0 ? 1 : hash;
}

// Verbatim port of struct_recon_view's editable_field_binding_t.
struct editable_field_binding_t {
    int structure_index = -1;
    int field_index = -1;
    std::uint64_t structure_id = 0;
    std::uint64_t structure_revision = 0;
    std::uint64_t field_id = 0;
    std::uint64_t schema_revision = 0;
    std::uint64_t base_address = 0;
    std::uint64_t refresh_sequence = 0;
    bool live_snapshot_current = false;
    struct_dissector::field_def_t field;
    struct_dissector::live_value_t value;
};

// Verbatim port of find_editable_field_binding.
std::optional<editable_field_binding_t> findEditableBinding(
    const struct_recon::reconstructed_struct_t& reconstruction,
    const struct_recon::struct_field_t& reconstructed_field) {
    auto& state = struct_dissector::g_state;
    std::lock_guard<std::mutex> lock(state.mtx);
    for (std::size_t structure_index = 0; structure_index < state.structs.size();
         ++structure_index) {
        const auto& structure = state.structs[structure_index];
        if (structure.name != reconstruction.name || structure.stable_id == 0 ||
            structure.layout_revision == 0)
            continue;
        for (std::size_t field_index = 0; field_index < structure.fields.size();
             ++field_index) {
            const auto& field = structure.fields[field_index];
            if (field.stable_id == 0 || field.name != reconstructed_field.name ||
                field.offset != reconstructed_field.offset ||
                field.size != static_cast<std::uint32_t>(reconstructed_field.size))
                continue;
            if (!struct_dissector::index_fits_int(structure_index) ||
                !struct_dissector::index_fits_int(field_index))
                return std::nullopt;
            editable_field_binding_t result;
            result.structure_index = static_cast<int>(structure_index);
            result.field_index = static_cast<int>(field_index);
            result.structure_id = structure.stable_id;
            result.structure_revision = structure.layout_revision;
            result.field_id = field.stable_id;
            result.schema_revision = state.schema_revision;
            result.base_address = state.base_address;
            result.refresh_sequence =
                state.last_completed_seq.load(std::memory_order_acquire);
            result.field = field;
            if (field_index < state.cached_values.size())
                result.value = state.cached_values[field_index];
            result.live_snapshot_current =
                state.active_struct == static_cast<int>(structure_index) &&
                state.base_address == reconstruction.base_address &&
                state.base_address != 0 && result.refresh_sequence != 0 &&
                !result.value.raw_bytes.empty();
            return result;
        }
    }
    return std::nullopt;
}

// The reconstructed-field table model.
class QtReconFieldModel : public QAbstractTableModel {
public:
    enum class Column : int {
        offset = 0,
        type = 1,
        name = 2,
        size = 3,
        conf = 4,
        heat = 5,
        comment = 6,
        column_count = 7
    };

    explicit QtReconFieldModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setStructure(
        std::shared_ptr<const struct_recon::reconstructed_struct_t> structure) {
        beginResetModel();
        structure_ = std::move(structure);
        endResetModel();
    }

    const struct_recon::reconstructed_struct_t* structure() const noexcept {
        return structure_.get();
    }
    const struct_recon::struct_field_t* fieldAt(int row) const noexcept {
        if (!structure_ || row < 0 ||
            static_cast<std::size_t>(row) >= structure_->fields.size())
            return nullptr;
        return &structure_->fields[static_cast<std::size_t>(row)];
    }

    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() || !structure_
            ? 0 : static_cast<int>(structure_->fields.size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.parent().isValid()) return {};
        const auto* field = fieldAt(index.row());
        if (!field) return {};
        const auto& tokens = theme::tokens();
        const auto column = static_cast<Column>(index.column());
        if (role == Qt::DisplayRole) {
            char buf[64]{};
            switch (column) {
            case Column::offset:
                std::snprintf(buf, sizeof(buf), "0x%04llX",
                    static_cast<unsigned long long>(field->offset));
                return QString::fromLatin1(buf);
            case Column::type:
                if (field->array_count > 1)
                    return QStringLiteral("%1[%2]")
                        .arg(QString::fromLatin1(
                            struct_recon::field_type_name(field->type)))
                        .arg(field->array_count);
                return QString::fromLatin1(
                    struct_recon::field_type_name(field->type));
            case Column::name: return QString::fromStdString(field->name);
            case Column::size: return QString::number(field->size);
            case Column::conf: {
                if (field->type_confidence >= 75.f) return QStringLiteral("Strong");
                if (field->type_confidence >= 50.f) return QStringLiteral("Med");
                if (field->type_confidence >= 25.f) return QStringLiteral("Weak");
                return QStringLiteral("-");
            }
            case Column::heat: return field->value_history.heat_level();
            case Column::comment:
                if (!field->comment.empty())
                    return QString::fromStdString(field->comment);
                if (!field->accesses.empty())
                    return QStringLiteral("%1 accesses").arg(field->accesses.size());
                return {};
            default: return {};
            }
        }
        if (role == Qt::ForegroundRole) {
            switch (column) {
            case Column::offset: return tokens.text_address;
            case Column::conf: {
                if (field->type_confidence >= 75.f) return tokens.success;
                if (field->type_confidence >= 50.f) return tokens.warning;
                if (field->type_confidence >= 25.f) return tokens.error;
                return tokens.text_dim;
            }
            case Column::size:
            case Column::comment: return tokens.text_dim;
            default: return {};
            }
        }
        if (role == Qt::UserRole && column == Column::heat)
            return field->value_history.heat_level();
        if (role == Qt::ToolTipRole) {
            switch (column) {
            case Column::name:
                return QString::fromStdString(field->name);
            case Column::comment:
                return field->comment.empty()
                    ? QVariant{} : QString::fromStdString(field->comment);
            default:
                return data(index, Qt::DisplayRole);
            }
        }
        return {};
    }
    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override {
        for (QModelRoleData& roleData : span) {
            switch (roleData.role()) {
            case Qt::DisplayRole:
            case Qt::ForegroundRole:
            case Qt::UserRole:
            case Qt::ToolTipRole:
                roleData.setData(data(index, roleData.role()));
                break;
            default:
                roleData.clearData();
                break;
            }
        }
    }
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (static_cast<Column>(section)) {
        case Column::offset: return QStringLiteral("Offset");
        case Column::type: return QStringLiteral("Type");
        case Column::name: return QStringLiteral("Name");
        case Column::size: return QStringLiteral("Size");
        case Column::conf: return QStringLiteral("Conf");
        case Column::heat: return QStringLiteral("Heat");
        case Column::comment: return QStringLiteral("Comment");
        default: return {};
        }
    }

private:
    std::shared_ptr<const struct_recon::reconstructed_struct_t> structure_;
};

// Heat cell delegate: 0-10 heat bar (0-3 levels from the 10-entry ring are
// preserved by the model value).
class QtReconHeatDelegate : public QStyledItemDelegate {
public:
    explicit QtReconHeatDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        const int heat = index.data(Qt::UserRole).toInt();
        const auto& tokens = theme::tokens();
        const int inset = tokens.spacing.xs + tokens.spacing.xxs;
        const QRect cell = option.rect.adjusted(inset, 0, -inset, 0);
        const int bar_h = tokens.spacing.xs + tokens.spacing.xxs;
        const qreal bar_radius = bar_h * 0.5;
        const QRect bar(cell.left(), cell.center().y() - bar_h / 2, cell.width(),
            bar_h);
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(with_alpha(tokens.panel_header, 0.6));
        painter->drawRoundedRect(bar, bar_radius, bar_radius);
        const QColor color = heat <= 3 ? tokens.success
            : heat <= 6 ? tokens.warning : tokens.error;
        const int fill = static_cast<int>(bar.width() *
            (static_cast<double>(heat) / 10.0));
        if (fill > 0) {
            painter->setBrush(with_alpha(color, 0.85));
            painter->drawRoundedRect(QRectF(bar.left(), bar.top(), fill, bar.height()),
                bar_radius, bar_radius);
        }
        painter->restore();
    }
};

class QtReconListModel : public QAbstractTableModel {
public:
    explicit QtReconListModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}
    void setRows(QStringList rows) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }
    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : 1;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || role != Qt::DisplayRole || index.row() < 0 ||
            index.row() >= rows_.size())
            return {};
        return rows_.at(index.row());
    }
    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override {
        for (QModelRoleData& roleData : span) {
            if (roleData.role() == Qt::DisplayRole)
                roleData.setData(data(index, Qt::DisplayRole));
            else
                roleData.clearData();
        }
    }
private:
    QStringList rows_;
};

}

QtStructReconView::QtStructReconView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.types.struct_recon"));
    const auto& t = theme::tokens();
    setMinimumWidth(4 * static_cast<int>(t.shell.min_panel_w) +
        t.control.height_lg + t.toolbar.height);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto& sr = struct_recon::g_state;

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.struct_recon.toolbar"));
    auto* toolbar_layout = new QVBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(t.toolbar.padding_x, t.toolbar.padding_y,
        t.toolbar.padding_x, t.toolbar.padding_y);
    toolbar_layout->setSpacing(t.spacing.xs);
    const QFontMetricsF ui_metrics(font());
    auto* input_row = new QHBoxLayout();
    addr_edit_ = new QLineEdit(toolbar);
    addr_edit_->setObjectName(QStringLiteral("aida.struct_recon.address"));
    addr_edit_->setPlaceholderText(QStringLiteral("Base address (hex)"));
    addr_edit_->setToolTip(QStringLiteral(
        "Base address of the memory region to reconstruct"));
    addr_edit_->setText(QString::fromLatin1(sr.address_input));
    input_row->addWidget(addr_edit_);
    name_edit_ = new QLineEdit(toolbar);
    name_edit_->setObjectName(QStringLiteral("aida.struct_recon.name"));
    name_edit_->setPlaceholderText(QStringLiteral("Struct name"));
    name_edit_->setToolTip(QStringLiteral(
        "Name given to the reconstructed structure"));
    name_edit_->setText(QString::fromLatin1(sr.name_input));
    input_row->addWidget(name_edit_);
    size_edit_ = new QLineEdit(toolbar);
    size_edit_->setObjectName(QStringLiteral("aida.struct_recon.size"));
    size_edit_->setPlaceholderText(QStringLiteral("Size"));
    size_edit_->setToolTip(QStringLiteral(
        "Number of bytes to reconstruct, decimal or 0x-prefixed hexadecimal"));
    size_edit_->setMinimumWidth(static_cast<int>(ui_metrics.horizontalAdvance(
        QStringLiteral("1048576"))) + 2 * t.table.cell_pad_x + t.spacing.lg);
    size_edit_->setText(QString::fromLatin1(sr.size_input));
    input_row->addWidget(size_edit_);
    toolbar_layout->addLayout(input_row);

    auto* actions_row = new QHBoxLayout();
    snapshot_button_ = new QPushButton(QStringLiteral("Snapshot"), toolbar);
    snapshot_button_->setObjectName(QStringLiteral("aida.struct_recon.snapshot"));
    snapshot_button_->setToolTip(QStringLiteral(
        "Reconstruct a structure from a one-shot memory snapshot"));
    hw_button_ = new QPushButton(QStringLiteral("HW Monitor"), toolbar);
    hw_button_->setObjectName(QStringLiteral("aida.struct_recon.hw_monitor"));
    hw_button_->setToolTip(QStringLiteral(
        "Reconstruct using hardware-breakpoint access monitoring"));
    live_button_ = new QPushButton(QStringLiteral("Live Monitor"), toolbar);
    live_button_->setObjectName(QStringLiteral("aida.struct_recon.live_monitor"));
    live_button_->setToolTip(QStringLiteral(
        "Reconstruct using live memory-change monitoring"));
    export_button_ = new QPushButton(QStringLiteral("Export C++"), toolbar);
    export_button_->setObjectName(QStringLiteral("aida.struct_recon.export"));
    export_button_->setToolTip(QStringLiteral(
        "Export the reconstructed structure as a C++ declaration"));
    apply_button_ = new QPushButton(QStringLiteral("Apply Type"), toolbar);
    apply_button_->setObjectName(QStringLiteral("aida.struct_recon.apply"));
    apply_button_->setToolTip(QStringLiteral(
        "Apply the reconstructed type at the base address"));
    ai_button_ = new QPushButton(QStringLiteral("AI Name"), toolbar);
    ai_button_->setObjectName(QStringLiteral("aida.struct_recon.ai_name"));
    ai_button_->setToolTip(QStringLiteral(
        "Ask the AI provider to name the reconstructed fields"));
    save_button_ = new QPushButton(QStringLiteral("Save"), toolbar);
    save_button_->setObjectName(QStringLiteral("aida.struct_recon.save"));
    save_button_->setToolTip(QStringLiteral(
        "Save the reconstruction into the structure catalog"));
    load_button_ = new QPushButton(QStringLiteral("Load All"), toolbar);
    load_button_->setObjectName(QStringLiteral("aida.struct_recon.load"));
    load_button_->setToolTip(QStringLiteral(
        "Reload every retained reconstruction result"));
    refresh_button_ = new QPushButton(QStringLiteral("Refresh"), toolbar);
    refresh_button_->setObjectName(QStringLiteral("aida.struct_recon.refresh"));
    refresh_button_->setToolTip(QStringLiteral(
        "Re-read field values from the target"));
    actions_row->addWidget(snapshot_button_);
    actions_row->addWidget(hw_button_);
    actions_row->addWidget(live_button_);
    actions_row->addWidget(export_button_);
    actions_row->addWidget(apply_button_);
    actions_row->addWidget(ai_button_);
    actions_row->addWidget(save_button_);
    actions_row->addWidget(load_button_);
    actions_row->addWidget(refresh_button_);
    actions_row->addStretch(1);
    toolbar_layout->addLayout(actions_row);
    layout->addWidget(toolbar);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("aida.struct_recon.progress"));
    progress_->setVisible(false);
    layout->addWidget(progress_);
    live_label_ = new QLabel(this);
    live_label_->setObjectName(QStringLiteral("aida.struct_recon.live"));
    live_label_->setProperty("aidaVariant", QStringLiteral("info"));
    live_label_->setVisible(false);
    layout->addWidget(live_label_);
    operation_label_ = new QLabel(this);
    operation_label_->setObjectName(QStringLiteral("aida.struct_recon.operation"));
    operation_label_->setWordWrap(true);
    operation_label_->setVisible(false);
    layout->addWidget(operation_label_);
    info_label_ = new QLabel(this);
    info_label_->setObjectName(QStringLiteral("aida.struct_recon.info"));
    info_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(info_label_);

    splitter_ = new QSplitter(this);
    splitter_->setObjectName(QStringLiteral("aida.struct_recon.splitter"));
    model_ = new QtReconFieldModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.struct_recon.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = table_->horizontalHeader();
    using Column = QtReconFieldModel::Column;
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const auto with_cell_pad = [&t](int content) {
        return content + 2 * t.table.cell_pad_x + t.spacing.xs;
    };
    horizontal->setSectionResizeMode(static_cast<int>(Column::offset),
        QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::offset),
        with_cell_pad(static_cast<int>(code_metrics.horizontalAdvance(
            QStringLiteral("0xFFFF")))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::type), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::type),
        with_cell_pad(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("UTF-16 String[9]")))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::name),
        QHeaderView::Stretch);
    horizontal->setSectionResizeMode(static_cast<int>(Column::size), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::size),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Size"))),
            static_cast<int>(code_metrics.horizontalAdvance(
                QStringLiteral("65536"))))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::conf), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::conf),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Conf"))),
            static_cast<int>(ui_metrics.horizontalAdvance(
                QStringLiteral("Strong"))))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::heat), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::heat),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Heat"))), t.control.height_lg)));
    table_->setItemDelegateForColumn(static_cast<int>(Column::heat),
        new QtReconHeatDelegate(this));
    horizontal->setSectionResizeMode(static_cast<int>(Column::comment),
        QHeaderView::Stretch);
    horizontal->setStretchLastSection(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    splitter_->addWidget(table_);

    detail_panel_ = new QWidget(this);
    detail_panel_->setObjectName(QStringLiteral("aida.struct_recon.detail"));
    auto* detail_layout = new QVBoxLayout(detail_panel_);
    detail_layout->setContentsMargins(t.toolbar.padding_x, t.toolbar.padding_y,
        t.toolbar.padding_x, t.toolbar.padding_y);
    detail_title_ = new QLabel(QStringLiteral("Field Details"), detail_panel_);
    detail_title_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detail_layout->addWidget(detail_title_);
    detail_offset_ = new QLabel(detail_panel_);
    detail_size_ = new QLabel(detail_panel_);
    detail_type_ = new QLabel(detail_panel_);
    detail_array_ = new QLabel(detail_panel_);
    detail_conf_ = new QLabel(detail_panel_);
    detail_heat_ = new QLabel(detail_panel_);
    detail_name_ = new QLabel(detail_panel_);
    detail_layout->addWidget(detail_offset_);
    detail_layout->addWidget(detail_size_);
    detail_layout->addWidget(detail_type_);
    detail_layout->addWidget(detail_array_);
    detail_layout->addWidget(detail_conf_);
    detail_layout->addWidget(detail_heat_);
    detail_layout->addWidget(detail_name_);
    detail_name_->setWordWrap(true);
    vtable_header_ = new QLabel(QStringLiteral("VTable Entries"), detail_panel_);
    vtable_header_->setProperty("aidaVariant", QStringLiteral("secondary"));
    vtable_header_->setVisible(false);
    detail_layout->addWidget(vtable_header_);
    vtable_model_ = new QtReconListModel(this);
    vtable_table_ = new QTableView(detail_panel_);
    vtable_table_->setObjectName(QStringLiteral("aida.struct_recon.vtable"));
    vtable_table_->verticalHeader()->setVisible(false);
    vtable_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    vtable_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    vtable_table_->horizontalHeader()->setVisible(false);
    vtable_table_->horizontalHeader()->setStretchLastSection(true);
    vtable_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vtable_table_->setShowGrid(false);
    vtable_table_->setModel(vtable_model_);
    vtable_table_->setVisible(false);
    detail_layout->addWidget(vtable_table_, 1);
    access_header_ = new QLabel(QStringLiteral("Access Log"), detail_panel_);
    access_header_->setProperty("aidaVariant", QStringLiteral("secondary"));
    access_header_->setVisible(false);
    detail_layout->addWidget(access_header_);
    access_model_ = new QtReconListModel(this);
    access_table_ = new QTableView(detail_panel_);
    access_table_->setObjectName(QStringLiteral("aida.struct_recon.access_log"));
    access_table_->verticalHeader()->setVisible(false);
    access_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    access_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    access_table_->horizontalHeader()->setVisible(false);
    access_table_->horizontalHeader()->setStretchLastSection(true);
    access_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    access_table_->setShowGrid(false);
    access_table_->setModel(access_model_);
    access_table_->setVisible(false);
    detail_layout->addWidget(access_table_, 1);
    splitter_->addWidget(detail_panel_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    layout->addWidget(splitter_, 1);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.struct_recon.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(66);
    connect(timer_, &QTimer::timeout, this, [this] { pollEngine(); });

    connect(snapshot_button_, &QPushButton::clicked, this, [this] {
        snapshotReconstruct();
    });
    connect(hw_button_, &QPushButton::clicked, this, [this] { hwMonitor(); });
    connect(live_button_, &QPushButton::clicked, this, [this] {
        startMonitors(struct_monitor::g_state.active.load() ? 1 : 0);
    });
    connect(export_button_, &QPushButton::clicked, this, [this] {
        exportDeclaration();
    });
    connect(apply_button_, &QPushButton::clicked, this, [this] {
        std::string detail;
        const bool completed = declareAndApplyCurrent(detail);
        operation_status_ = detail;
        operation_error_ = !completed;
        operation_label_->setText(QString::fromStdString(operation_status_));
        operation_label_->setVisible(true);
    });
    connect(ai_button_, &QPushButton::clicked, this, [this] {
        if (struct_recon::g_state.ai_naming.load()) return;
        diag::log_tagged_fmt("struct_recon", "ai_name_clicked");
        struct_recon::ai_name_fields();
    });
    connect(save_button_, &QPushButton::clicked, this, [this] { saveStruct(); });
    connect(load_button_, &QPushButton::clicked, this, [this] { loadAll(); });
    connect(refresh_button_, &QPushButton::clicked, this, [this] { refreshValues(); });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        if (!current.isValid()) return;
        selected_field_ = current.row();
        updateDetailPanel();
        const auto* field =
            static_cast<QtReconFieldModel*>(model_)->fieldAt(current.row());
        if (field) {
            const auto* structure =
                static_cast<QtReconFieldModel*>(model_)->structure();
            if (structure) {
                std::uint64_t address = 0;
                const auto context = disasm_view::capture_selected_workspace();
                if (checked_field_address(structure->base_address, field->offset,
                        address) &&
                    disasm_view::typed_address(context, address)) {
                    disasm_view::select_address(address, context);
                }
            }
        }
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        if (index.isValid())
            showFieldMenu(table_->viewport()->mapToGlobal(pos), index.row());
    });

    refreshPresentation();
}

void QtStructReconView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    timer_->start();
}

void QtStructReconView::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

void QtStructReconView::pollEngine() {
    auto& sr = struct_recon::g_state;
    const bool monitoring = sr.monitoring.load();
    const auto frame_structure = struct_recon::capture_current_snapshot();
    const bool has_structure = frame_structure && !frame_structure->fields.empty();

    if (frame_structure != last_snapshot_) {
        last_snapshot_ = frame_structure;
        static_cast<QtReconFieldModel*>(model_)->setStructure(frame_structure);
        if (selected_field_ >= 0 && (!frame_structure ||
                selected_field_ >= static_cast<int>(frame_structure->fields.size())))
            selected_field_ = -1;
        updateDetailPanel();
    }

    if (monitoring) {
        progress_->setVisible(true);
        progress_->setRange(0, 100);
        progress_->setValue(static_cast<int>(sr.progress.load() * 100.f));
    } else {
        progress_->setVisible(false);
    }

    const bool live_active = struct_monitor::g_state.active.load();
    live_label_->setVisible(live_active);
    if (live_active) {
        const std::uint64_t cps =
            struct_monitor::g_state.captures_per_second.load();
        const std::uint64_t total = struct_monitor::g_state.total_captures.load();
        live_label_->setText(QStringLiteral("%1 cap/s   %2 total")
            .arg(cps).arg(total));
    }
    snapshot_button_->setVisible(!monitoring);
    hw_button_->setVisible(!monitoring);
    live_button_->setText(live_active ? QStringLiteral("Stop Live")
        : QStringLiteral("Live Monitor"));

    if (operation_pending_) {
        auto workspace = disasm_view::capture_selected_workspace();
        const auto mutation = disasm_view::mutation_state(workspace);
        if (!workspace.workspace ||
            workspace.workspace->generation() != operation_generation_) {
            operation_pending_ = false;
            operation_error_ = true;
            operation_status_ =
                "Analysis generation changed before the structure application committed.";
        } else if (mutation.overlay_revision > operation_overlay_revision_) {
            operation_pending_ = false;
            operation_error_ = false;
            operation_status_ =
                "Structure declaration and base application committed to the reversible overlay.";
        } else if (mutation.pending == 0 && !mutation.error.empty()) {
            operation_pending_ = false;
            operation_error_ = true;
            operation_status_ = mutation.error;
        }
        if (!operation_pending_) {
            operation_label_->setText(QString::fromStdString(operation_status_));
            operation_label_->setVisible(true);
        }
    }

    if (has_structure) {
        info_label_->setText(QStringLiteral("%1   0x%2   %3 bytes   %4 fields")
            .arg(QString::fromStdString(frame_structure->name))
            .arg(frame_structure->base_address, 0, 16)
            .arg(frame_structure->total_size)
            .arg(frame_structure->fields.size()));
        info_label_->setVisible(true);
    } else {
        info_label_->setVisible(false);
    }
    export_button_->setEnabled(has_structure);
    apply_button_->setEnabled(has_structure);
    ai_button_->setEnabled(has_structure &&
        !struct_recon::g_state.ai_naming.load());
    save_button_->setEnabled(has_structure);
    refresh_button_->setEnabled(has_structure &&
        frame_structure->base_address != 0);
    refreshPresentation();
}

void QtStructReconView::refreshPresentation() {
    const auto structure = struct_recon::capture_current_snapshot();
    const bool empty = (!structure || structure->fields.empty()) &&
        !struct_recon::g_state.monitoring.load();
    const bool driver_loaded = driver_bridge::is_loaded();
    if (empty) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(driver_loaded ? QStringLiteral("No struct reconstructed")
            : QStringLiteral("Attach a process first"));
        state_view_->setMessage(driver_loaded
            ? QStringLiteral("Enter a base address and click Snapshot to reconstruct struct layout.")
            : QStringLiteral("Inferred-struct reconstruction reads live memory and needs an attached target. Open the debugger or scanner view to attach."));
        state_view_->setVisible(true);
        splitter_->setVisible(false);
        return;
    }
    state_view_->setVisible(false);
    splitter_->setVisible(true);
}

void QtStructReconView::snapshotReconstruct() {
    auto& sr = struct_recon::g_state;
    const auto write_input = [](char* target, std::size_t capacity,
                                QLineEdit* edit) {
        std::strncpy(target, edit->text().toStdString().c_str(), capacity - 1);
        target[capacity - 1] = '\0';
    };
    write_input(sr.address_input, sizeof(sr.address_input), addr_edit_);
    write_input(sr.name_input, sizeof(sr.name_input), name_edit_);
    write_input(sr.size_input, sizeof(sr.size_input), size_edit_);
    std::uint64_t addr = 0;
    int size = 256;
    if (sr.address_input[0]) addr = std::strtoull(sr.address_input, nullptr, 16);
    if (sr.size_input[0])
        size = static_cast<int>(std::strtol(sr.size_input, nullptr, 0));
    if (size <= 0) size = 256;
    if (size > 4096) size = 4096;
    if (addr != 0) {
        diag::log_tagged_fmt("struct_recon",
            "snapshot_clicked addr=0x%llX size=%d name='%s'",
            static_cast<unsigned long long>(addr), size, sr.name_input);
        struct_recon::reconstruct_from_snapshot(addr, size, sr.name_input);
    } else {
        diag::log_tagged_fmt("struct_recon",
            "snapshot_skipped reason='addr_zero' input='%s'", sr.address_input);
    }
}

void QtStructReconView::hwMonitor() {
    auto& sr = struct_recon::g_state;
    std::uint64_t addr = 0;
    int size = 256;
    if (sr.address_input[0]) addr = std::strtoull(sr.address_input, nullptr, 16);
    if (size_edit_->text().toInt() > 0)
        size = size_edit_->text().toInt();
    if (size <= 0) size = 256;
    if (size > 4096) size = 4096;
    if (addr != 0) {
        diag::log_tagged_fmt("struct_recon",
            "hwmon_clicked addr=0x%llX size=%d name='%s'",
            static_cast<unsigned long long>(addr), size, sr.name_input);
        struct_recon::monitor_with_hwbp(addr, size, sr.name_input);
    } else {
        diag::log_tagged_fmt("struct_recon",
            "hwmon_skipped reason='addr_zero' input='%s'", sr.address_input);
    }
}

void QtStructReconView::startMonitors(int stop_live) {
    auto& sr = struct_recon::g_state;
    if (stop_live) {
        diag::log_tagged_fmt("struct_recon", "live_monitor_stop_clicked");
        struct_monitor::stop();
        return;
    }
    std::uint64_t addr = 0;
    int size = 256;
    if (sr.address_input[0]) addr = std::strtoull(sr.address_input, nullptr, 16);
    if (size_edit_->text().toInt() > 0) size = size_edit_->text().toInt();
    if (size <= 0) size = 256;
    if (size > 4096) size = 4096;
    if (addr != 0) {
        diag::log_tagged_fmt("struct_recon",
            "live_monitor_start_clicked addr=0x%llX size=%d name='%s'",
            static_cast<unsigned long long>(addr), size, sr.name_input);
        std::string name = sr.name_input;
        aida::infra::executor::submission_t sub;
        sub.owner_subsystem = "analysis";
        sub.label = "analysis.struct_recon.live_monitor_start";
        sub.thread_class = "long_running";
        sub.domain = aida::infra::executor::domain_t::long_running;
        sub.priority = 2;
        sub.body = [addr, size, name]() {
            struct_monitor::start(addr, size, name);
        };
        if (!aida::infra::executor::submit(std::move(sub)).submitted) {
            diag::log_tagged_fmt("struct_recon",
                "live_monitor_start_post_failed addr=0x%llX size=%d name='%s'",
                static_cast<unsigned long long>(addr), size, name.c_str());
        }
    } else {
        diag::log_tagged_fmt("struct_recon",
            "live_monitor_skipped reason='addr_zero' input='%s'", sr.address_input);
    }
}

void QtStructReconView::exportDeclaration() {
    const auto structure = struct_recon::capture_current_snapshot();
    const std::string cpp = structure
        ? struct_recon::export_as_cpp(*structure) : std::string{};
    const std::string name = structure ? structure->name : std::string{};
    const std::size_t field_count = structure ? structure->fields.size() : 0;
    if (!cpp.empty() && cpp.size() <= 64U * 1024U)
        showDeclarationPreview();
    diag::log_tagged_fmt("struct_recon",
        "export_cpp_review name='%s' fields=%zu bytes=%zu",
        name.c_str(), field_count, cpp.size());
}

void QtStructReconView::saveStruct() {
    const auto structure = struct_recon::capture_current_snapshot();
    const struct_recon::reconstructed_struct_t snap = structure
        ? *structure : struct_recon::reconstructed_struct_t{};
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "analysis";
    sub.label = "analysis.struct_recon.save_struct";
    sub.thread_class = "bounded_task";
    sub.domain = aida::infra::executor::domain_t::diagnostics;
    sub.priority = 4;
    sub.body = [snap]() {
        std::string error;
        if (!struct_recon::save_struct_to_disk(snap, error))
            throw std::runtime_error(error.empty()
                ? "The structure could not be saved" : error);
        diag::log_tagged_fmt("struct_recon",
            "save_disk_done name='%s' fields=%zu", snap.name.c_str(),
            snap.fields.size());
    };
    const auto save_submission = aida::infra::executor::submit(std::move(sub));
    if (!save_submission.submitted) {
        diag::log_tagged_fmt("struct_recon",
            "save_disk_post_failed name='%s' fields=%zu", snap.name.c_str(),
            snap.fields.size());
        operation_error_ = true;
        operation_status_ = save_submission.reject_reason.empty()
            ? "The structure persistence queue rejected Save."
            : save_submission.reject_reason;
    } else {
        operation_error_ = false;
        operation_status_ =
            "Save queued; the persisted structure remains available after restart.";
        aida::ui::task_center::task_registration_t registration;
        registration.owner = "analysis";
        registration.owner_view = "view.types.struct_recon";
        registration.owner_action = "types.structure.save";
        registration.label = "Save reconstructed structure";
        registration.stage = "Queued";
        registration.target = snap.name;
        static_cast<void>(aida::ui::task_center::register_executor_job(
            save_submission.task_id, std::move(registration)));
    }
    diag::log_tagged_fmt("struct_recon", "save_clicked name='%s' fields=%zu",
        snap.name.c_str(), snap.fields.size());
    operation_label_->setText(QString::fromStdString(operation_status_));
    operation_label_->setVisible(true);
}

void QtStructReconView::loadAll() {
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "analysis";
    sub.label = "analysis.struct_recon.load_all";
    sub.thread_class = "bounded_task";
    sub.domain = aida::infra::executor::domain_t::diagnostics;
    sub.priority = 4;
    sub.body = []() {
        std::string error;
        if (!struct_recon::load_structs_from_disk(error))
            throw std::runtime_error(error.empty()
                ? "The structure catalog could not be loaded" : error);
        std::size_t loaded = 0;
        {
            std::lock_guard<std::mutex> lock(struct_recon::g_state.mutex);
            loaded = struct_recon::g_state.saved_structs.size();
        }
        diag::log_tagged_fmt("struct_recon", "load_all_done count=%zu", loaded);
    };
    const auto load_submission = aida::infra::executor::submit(std::move(sub));
    if (!load_submission.submitted) {
        diag::log_tagged_fmt("struct_recon", "load_all_post_failed");
        operation_error_ = true;
        operation_status_ = load_submission.reject_reason.empty()
            ? "The structure persistence queue rejected Load All."
            : load_submission.reject_reason;
    } else {
        operation_error_ = false;
        operation_status_ =
            "Load All queued; saved structures will appear when disk loading completes.";
        aida::ui::task_center::task_registration_t registration;
        registration.owner = "analysis";
        registration.owner_view = "view.types.struct_recon";
        registration.owner_action = "types.structure.load_all";
        registration.label = "Load reconstructed structures";
        registration.stage = "Queued";
        static_cast<void>(aida::ui::task_center::register_executor_job(
            load_submission.task_id, std::move(registration)));
    }
    diag::log_tagged_fmt("struct_recon", "load_all_clicked");
    operation_label_->setText(QString::fromStdString(operation_status_));
    operation_label_->setVisible(true);
}

void QtStructReconView::refreshValues() {
    const auto structure = struct_recon::capture_current_snapshot();
    const std::uint64_t base = structure ? structure->base_address : 0;
    const bool active = structure && !structure->fields.empty();
    if (active && base != 0) {
        aida::infra::executor::submission_t sub;
        sub.owner_subsystem = "analysis";
        sub.label = "analysis.struct_recon.refresh_values";
        sub.thread_class = "bounded_task";
        sub.domain = aida::infra::executor::domain_t::diagnostics;
        sub.priority = 4;
        sub.body = []() {
            std::string error;
            if (!struct_recon::refresh_value_history(error))
                throw std::runtime_error(error.empty()
                    ? "The reconstructed live values could not be refreshed" : error);
            diag::log_tagged_fmt("struct_recon", "refresh_value_history_done");
        };
        const auto refresh_submission = aida::infra::executor::submit(std::move(sub));
        if (!refresh_submission.submitted) {
            diag::log_tagged_fmt("struct_recon", "refresh_value_history_post_failed");
            operation_error_ = true;
            operation_status_ = refresh_submission.reject_reason.empty()
                ? "The value refresh queue rejected the request."
                : refresh_submission.reject_reason;
        } else {
            operation_error_ = false;
            operation_status_ =
                "Value refresh queued; displayed values remain the last completed snapshot.";
            aida::ui::task_center::task_registration_t registration;
            registration.owner = "analysis";
            registration.owner_view = "view.types.struct_recon";
            registration.owner_action = "types.structure.refresh_values";
            registration.label = "Refresh reconstructed live values";
            registration.stage = "Queued";
            registration.target = "Address " + std::to_string(base);
            static_cast<void>(aida::ui::task_center::register_executor_job(
                refresh_submission.task_id, std::move(registration)));
        }
        diag::log_tagged_fmt("struct_recon", "refresh_clicked base=0x%llX",
            static_cast<unsigned long long>(base));
    } else {
        operation_error_ = true;
        operation_status_ =
            "Refresh requires an active reconstructed structure with a nonzero base address.";
        diag::log_tagged_fmt("struct_recon",
            "refresh_skipped active=%d base=0x%llX has_fields=%d",
            active ? 1 : 0, static_cast<unsigned long long>(base), active ? 1 : 0);
    }
    operation_label_->setText(QString::fromStdString(operation_status_));
    operation_label_->setVisible(true);
}

bool QtStructReconView::hasCurrentStructure() const {
    const auto structure = struct_recon::capture_current_snapshot();
    return structure && !structure->fields.empty();
}

bool QtStructReconView::copyCurrentDeclaration(std::string& detail) {
    const auto structure = struct_recon::capture_current_snapshot();
    if (!structure || structure->fields.empty()) {
        detail = "Reconstruct or load a structure first";
        return false;
    }
    const std::string declaration = struct_recon::export_as_cpp(*structure);
    if (declaration.empty() || declaration.size() > 64U * 1024U) {
        detail = "The reconstruction declaration is empty or exceeds 64 KiB";
        return false;
    }
    clipboard::set_text(QString::fromStdString(declaration));
    detail = "Generated C++ declaration copied";
    return true;
}

bool QtStructReconView::declareAndApplyCurrent(std::string& detail) {
    const auto structure = struct_recon::capture_current_snapshot();
    if (!structure || structure->fields.empty()) {
        detail = "Reconstruct or load a structure first";
        return false;
    }
    auto context = disasm_view::capture_selected_workspace();
    if (!context.workspace || !context.publication) {
        detail = "Open and analyze the static or live target that owns this structure first";
        return false;
    }
    if (context.workspace->closing() || context.workspace->closed()) {
        detail = "The selected analysis workspace is closing";
        return false;
    }
    const auto address = disasm_view::typed_address(context, structure->base_address);
    if (!address) {
        detail =
            "The reconstructed base address is outside the selected workspace mapping";
        return false;
    }
    const std::string declaration = struct_recon::export_as_cpp(*structure);
    if (declaration.empty() || declaration.size() > 64U * 1024U ||
        structure->name.empty()) {
        detail = "The reconstruction has no valid generated declaration or type name";
        return false;
    }
    retained_overlay_structure_ = structure;
    retained_overlay_workspace_id_ =
        context.workspace->identity().binary_id().to_hex();
    retained_overlay_generation_ = context.workspace->generation();
    retained_overlay_analysis_revision_ = context.publication->analysis_revision;
    retained_overlay_revision_ = context.workspace->overlay_revision();
    retained_overlay_base_ = structure->base_address;
    retained_overlay_workspace_ = context.workspace;
    retained_overlay_publication_ = context.publication;
    detail =
        "Review the atomic declaration and application before committing it to the overlay";
    QtAnalysisBridge::instance().openView("view.types.struct_recon");
    showDeclareApplyReview();
    return true;
}

void QtStructReconView::showDeclarationPreview() {
    const auto structure = struct_recon::capture_current_snapshot();
    if (!structure) return;
    const std::string declaration = struct_recon::export_as_cpp(*structure);
    if (declaration.empty() || declaration.size() > 64U * 1024U) return;
    if (preview_dialog_) {
        preview_dialog_->deleteLater();
        preview_dialog_ = nullptr;
    }
    auto* dialog = new QDialog(this);
    preview_dialog_ = dialog;
    dialog->setObjectName(QStringLiteral("aida.dialog.recon.declaration_preview"));
    dialog->setWindowTitle(QStringLiteral("Generated Reconstruction Declaration"));
    dialog->setModal(true);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(760, 600);
    dialog->setMinimumSize(440, 320);
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(QStringLiteral("Generated declaration: %1")
        .arg(QString::fromStdString(structure->name)), dialog));
    auto* size_label = new QLabel(QStringLiteral("%1 bytes  bounded maximum 64 KiB")
        .arg(declaration.size()), dialog);
    layout->addWidget(size_label);
    auto* text = new QPlainTextEdit(dialog);
    text->setReadOnly(true);
    text->setPlainText(QString::fromStdString(declaration));
    layout->addWidget(text, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Copy Declaration"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Close"));
    auto* export_button = buttons->addButton(QStringLiteral("Export File..."),
        QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, declaration] {
        clipboard::set_text(QString::fromStdString(declaration));
        operation_error_ = false;
        operation_status_ = "Generated C++ declaration copied.";
        operation_label_->setText(QString::fromStdString(operation_status_));
        operation_label_->setVisible(true);
        preview_dialog_->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(export_button, &QPushButton::clicked, dialog, [this, declaration, structure] {
        const std::string initial = structure->name.empty()
            ? "reconstructed_type.hpp" : structure->name + ".hpp";
        const auto destination = dialogs::save_file(this,
            QStringLiteral("Export Generated Structure Declaration"),
            "C/C++ Header (*.h;*.hpp)\0*.h;*.hpp\0C/C++ Source (*.c;*.cpp)\0*.c;*.cpp\0All Files (*.*)\0*.*\0\0",
            QStringLiteral("hpp"),
            QString::fromStdString(initial));
        if (!destination || destination->empty()) return;
        aida::infra::executor::submission_t sub;
        sub.owner_subsystem = "analysis";
        sub.label = "analysis.struct_recon.export_declaration";
        sub.thread_class = "bounded_file_io";
        sub.domain = aida::infra::executor::domain_t::external_tool;
        sub.priority = 3;
        const std::string path = *destination;
        sub.body = [path, declaration]() {
            std::string error;
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
                throw std::runtime_error("The export destination could not be opened");
            output.write(declaration.data(),
                static_cast<std::streamsize>(declaration.size()));
            output.flush();
            if (!output.good())
                throw std::runtime_error("The declaration export write failed");
        };
        if (!aida::infra::executor::submit(std::move(sub)).submitted) {
            operation_error_ = true;
            operation_status_ = "The declaration export queue rejected the request.";
        } else {
            operation_error_ = false;
            operation_status_ =
                "Generated declaration exported through an exact atomic file replacement.";
        }
        operation_label_->setText(QString::fromStdString(operation_status_));
        operation_label_->setVisible(true);
    });
    dialog->open();
}

void QtStructReconView::showDeclareApplyReview() {
    if (!retained_overlay_structure_) return;
    if (overlay_review_dialog_) {
        overlay_review_dialog_->deleteLater();
        overlay_review_dialog_ = nullptr;
    }
    const auto structure = retained_overlay_structure_;
    const std::string declaration = struct_recon::export_as_cpp(*structure);
    const auto workspace = retained_overlay_workspace_.lock();
    const auto publication = retained_overlay_publication_;
    const std::string type_name = structure->name;
    const std::uint64_t base = retained_overlay_base_;
    auto* dialog = new QtDeclarationReviewDialog(workspace, publication,
        retained_overlay_generation_, retained_overlay_analysis_revision_,
        retained_overlay_revision_,
        QStringLiteral("Review Reconstructed Type Application"),
        QStringLiteral("Declare %1 and apply at 0x%2")
            .arg(QString::fromStdString(type_name))
            .arg(base, 16, 16, QLatin1Char('0')),
        declaration, QStringLiteral("Commit Declaration and Application"),
        [this, workspace, base, type_name](const std::string& committed) {
            const auto context = disasm_view::capture_workspace(workspace);
            const auto address = disasm_view::typed_address(context, base);
            const bool queued = address &&
                disasm_view::queue_type_declaration_and_application(context, *address,
                    committed, type_name);
            operation_error_ = !queued;
            operation_status_ = queued
                ? "Atomic declaration and application queued through the reversible overlay authority"
                : "The overlay authority rejected the transaction; no change was claimed";
            operation_label_->setText(QString::fromStdString(operation_status_));
            operation_label_->setVisible(true);
            if (queued) {
                operation_pending_ = true;
                operation_generation_ = retained_overlay_generation_;
                operation_overlay_revision_ = retained_overlay_revision_;
            }
            return queued;
        }, this);
    overlay_review_dialog_ = dialog;
    dialog->open();
}

void QtStructReconView::updateDetailPanel() {
    auto* model = static_cast<QtReconFieldModel*>(model_);
    const auto* field = model ? model->fieldAt(selected_field_) : nullptr;
    const bool show = field != nullptr;
    detail_panel_->setVisible(show);
    if (!field) return;
    detail_offset_->setText(QStringLiteral("Offset   0x%1")
        .arg(field->offset, 4, 16, QLatin1Char('0')));
    detail_size_->setText(QStringLiteral("Size     %1 bytes").arg(field->size));
    detail_type_->setText(QStringLiteral("Type     %1")
        .arg(QString::fromLatin1(struct_recon::field_type_name(field->type))));
    detail_array_->setText(field->array_count > 1
        ? QStringLiteral("Array    [%1]").arg(field->array_count) : QString());
    const char* conf_name = "Unknown";
    if (field->type_confidence >= 75.f) conf_name = "Strong";
    else if (field->type_confidence >= 50.f) conf_name = "Moderate";
    else if (field->type_confidence >= 25.f) conf_name = "Weak";
    detail_conf_->setText(QStringLiteral("Confidence   %1").arg(
        QString::fromLatin1(conf_name)));
    const int heat = field->value_history.heat_level();
    detail_heat_->setText(QStringLiteral("Heat     %1/10  (%2 unique)")
        .arg(heat).arg(field->value_history.unique_count()));
    detail_name_->setText(QString::fromStdString(field->name));

    const bool has_vtable = field->type == struct_recon::field_type_t::vtable_ptr &&
        !field->vtable_entries.empty();
    vtable_header_->setVisible(has_vtable);
    vtable_table_->setVisible(has_vtable && vtable_expanded_);
    if (has_vtable) {
        QStringList rows;
        const std::size_t shown = (std::min)(field->vtable_entries.size(),
            std::size_t{32});
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& entry = field->vtable_entries[index];
            char buf[256]{};
            std::snprintf(buf, sizeof(buf), "[%2d]  0x%llX  %s", entry.index,
                static_cast<unsigned long long>(entry.func_addr), entry.name.c_str());
            rows.push_back(QString::fromLatin1(buf));
        }
        static_cast<QtReconListModel*>(vtable_model_)->setRows(std::move(rows));
    }
    const bool has_accesses = !field->accesses.empty();
    access_header_->setVisible(has_accesses);
    access_table_->setVisible(has_accesses);
    if (has_accesses) {
        QStringList rows;
        const std::size_t shown = (std::min)(field->accesses.size(), std::size_t{20});
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& access = field->accesses[index];
            char buf[160]{};
            std::snprintf(buf, sizeof(buf), "%s 0x%llX  +0x%llX  %dB  x%d",
                access.is_write ? "W" : "R",
                static_cast<unsigned long long>(access.instruction_addr),
                static_cast<unsigned long long>(access.access_offset),
                access.access_size, access.hit_count);
            rows.push_back(QString::fromLatin1(buf));
        }
        static_cast<QtReconListModel*>(access_model_)->setRows(std::move(rows));
    }
}

std::optional<std::pair<int, int>> QtStructReconView::resolveRetainedEditBinding() const {
    // Verbatim port of resolve_retained_edit_binding.
    auto& catalog = struct_dissector::g_state;
    std::lock_guard<std::mutex> lock(catalog.mtx);
    if (catalog.schema_revision != retained_edit_schema_revision_)
        return std::nullopt;
    const int structure_index = struct_dissector::structure_index_by_id_locked(
        retained_edit_structure_id_);
    if (!struct_dissector::valid_index(structure_index, catalog.structs.size()))
        return std::nullopt;
    const auto& structure = catalog.structs[static_cast<std::size_t>(structure_index)];
    if (structure.layout_revision != retained_edit_structure_revision_)
        return std::nullopt;
    const auto found = std::find_if(structure.fields.begin(), structure.fields.end(),
        [this](const auto& field) {
            return field.stable_id == retained_edit_field_id_;
        });
    if (found == structure.fields.end())
        return std::nullopt;
    const auto field_index = static_cast<std::size_t>(
        std::distance(structure.fields.begin(), found));
    if (!struct_dissector::index_fits_int(field_index))
        return std::nullopt;
    return std::pair<int, int>{structure_index, static_cast<int>(field_index)};
}

void QtStructReconView::showRetainedFieldEdit(int kind, int field_index) {
    const auto reconstruction = struct_recon::capture_current_snapshot();
    const auto context = disasm_view::capture_selected_workspace();
    if (!reconstruction || !context.workspace || !context.publication ||
        field_index < 0 || field_index >= static_cast<int>(reconstruction->fields.size()))
        return;
    const auto binding = findEditableBinding(*reconstruction,
        reconstruction->fields[static_cast<std::size_t>(field_index)]);
    if (!binding) return;
    retained_edit_kind_ = static_cast<retained_edit_kind_t>(kind);
    retained_edit_snapshot_ = reconstruction;
    retained_edit_workspace_ = context.workspace;
    retained_edit_publication_ = context.publication;
    retained_edit_workspace_id_ = context.workspace->identity().binary_id().to_hex();
    retained_edit_workspace_generation_ = context.workspace->generation();
    retained_edit_analysis_revision_ = context.publication->analysis_revision;
    retained_edit_target_pid_ = context.workspace->identity().process()
        ? context.workspace->identity().process()->pid : 0;
    retained_edit_field_hash_ = field_identity_hash(
        reconstruction->fields[static_cast<std::size_t>(field_index)]);
    retained_edit_structure_id_ = binding->structure_id;
    retained_edit_structure_revision_ = binding->structure_revision;
    retained_edit_field_id_ = binding->field_id;
    retained_edit_schema_revision_ = binding->schema_revision;
    retained_edit_base_ = reconstruction->base_address;
    retained_edit_refresh_sequence_ = binding->refresh_sequence;
    retained_edit_field_index_ = field_index;
    retained_edit_type_ = static_cast<int>(binding->field.type);
    retained_edit_text_ = retained_edit_kind_ == retained_edit_kind_t::rename
        ? binding->field.name
        : retained_edit_kind_ == retained_edit_kind_t::live_value
        ? binding->value.display_text : std::string{};
    if (field_edit_dialog_) {
        field_edit_dialog_->deleteLater();
        field_edit_dialog_ = nullptr;
    }
    auto* dialog = new QDialog(this);
    field_edit_dialog_ = dialog;
    dialog->setObjectName(QStringLiteral("aida.dialog.recon.field_edit_review"));
    dialog->setWindowTitle(QStringLiteral("Review Reconstructed Field Edit"));
    dialog->setModal(true);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(620, 420);
    dialog->setMinimumSize(440, 320);
    auto* layout = new QVBoxLayout(dialog);
    const char* operation = retained_edit_kind_ == retained_edit_kind_t::rename
        ? "Rename editable field" : retained_edit_kind_ == retained_edit_kind_t::retype
        ? "Retype editable field" : "Edit live field value";
    layout->addWidget(new QLabel(QString::fromLatin1(operation), dialog));
    const auto& field =
        reconstruction->fields[static_cast<std::size_t>(field_index)];
    layout->addWidget(new QLabel(QStringLiteral("%1.%2  +0x%3  %4 bytes")
        .arg(QString::fromStdString(reconstruction->name))
        .arg(QString::fromStdString(field.name))
        .arg(field.offset, 0, 16)
        .arg((std::max)(field.size, 0)), dialog));
    layout->addWidget(new QLabel(QStringLiteral(
        "Workspace generation %1  analysis revision %2  catalog revision %3")
        .arg(retained_edit_workspace_generation_)
        .arg(retained_edit_analysis_revision_)
        .arg(retained_edit_schema_revision_), dialog));
    auto* input = new QLineEdit(QString::fromStdString(retained_edit_text_), dialog);
    layout->addWidget(input);
    auto* type_combo = new QComboBox(dialog);
    for (int i = 0; i < static_cast<int>(struct_dissector::field_type_t::COUNT); ++i)
        type_combo->addItem(QString::fromLatin1(struct_dissector::field_type_name(
            static_cast<struct_dissector::field_type_t>(i))));
    type_combo->setCurrentIndex(retained_edit_type_);
    type_combo->setVisible(retained_edit_kind_ == retained_edit_kind_t::retype);
    layout->addWidget(type_combo);
    input->setVisible(retained_edit_kind_ != retained_edit_kind_t::retype);
    if (retained_edit_kind_ == retained_edit_kind_t::live_value) {
        auto* note = new QLabel(QStringLiteral(
            "The Structure Dissector will revalidate the exact target, old bytes, field identity, and address before writing, then require an exact readback match."), dialog);
        note->setWordWrap(true);
        layout->addWidget(note);
    }
    auto* stale_note = new QLabel(dialog);
    stale_note->setWordWrap(true);
    stale_note->setVisible(false);
    layout->addWidget(stale_note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(
        retained_edit_kind_ == retained_edit_kind_t::live_value
            ? QStringLiteral("Stage verified write")
            : QStringLiteral("Commit catalog edit"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, input, type_combo,
            dialog] {
        retained_edit_text_ = input->text().toStdString();
        retained_edit_type_ = type_combo->currentIndex();
        const auto resolved = resolveRetainedEditBinding();
        bool applied = false;
        std::string error;
        if (!resolved) {
            error = "The editable field revision changed before confirmation.";
        } else if (retained_edit_kind_ == retained_edit_kind_t::live_value) {
            struct_dissector::field_def_t editable_field;
            struct_dissector::live_value_t value;
            std::uint64_t base = 0;
            {
                auto& catalog = struct_dissector::g_state;
                std::lock_guard<std::mutex> lock(catalog.mtx);
                if (catalog.schema_revision == retained_edit_schema_revision_ &&
                    catalog.active_struct == resolved->first &&
                    catalog.base_address == retained_edit_base_ &&
                    catalog.last_completed_seq.load(std::memory_order_acquire) ==
                        retained_edit_refresh_sequence_ &&
                    struct_dissector::valid_index(resolved->second,
                        catalog.structs[static_cast<std::size_t>(resolved->first)]
                            .fields.size()) &&
                    static_cast<std::size_t>(resolved->second) <
                        catalog.cached_values.size()) {
                    editable_field = catalog.structs[
                        static_cast<std::size_t>(resolved->first)]
                        .fields[static_cast<std::size_t>(resolved->second)];
                    value = catalog.cached_values[
                        static_cast<std::size_t>(resolved->second)];
                    base = catalog.base_address;
                }
            }
            if (base == 0 || value.raw_bytes.empty()) {
                error = "The live value snapshot changed; refresh and select the field again.";
            } else {
                auto* dissector = QtStructDissectorView::activeInstance();
                if (dissector)
                    applied = dissector->stageWriteReview(resolved->first,
                        resolved->second, editable_field, value, base,
                        retained_edit_text_.c_str(), error);
            }
        } else {
            const auto kind = retained_edit_kind_;
            const std::string text = retained_edit_text_;
            const int type = retained_edit_type_;
            applied = struct_dissector::perform_user_catalog_edit(
                kind == retained_edit_kind_t::rename
                    ? "Rename reconstructed field" : "Retype reconstructed field",
                [resolved, kind, text, type] {
                    return kind == retained_edit_kind_t::rename
                        ? struct_dissector::rename_field(resolved->first,
                            resolved->second, text)
                        : struct_dissector::retype_field(resolved->first,
                            resolved->second,
                            static_cast<struct_dissector::field_type_t>(type));
                }, error);
        }
        operation_error_ = !applied;
        operation_status_ = applied
            ? retained_edit_kind_ == retained_edit_kind_t::live_value
                ? "Live mutation staged for explicit confirmation and exact readback verification."
                : "Editable structure catalog updated and durable persistence queued."
            : error.empty() ? "The revision-bound edit was rejected; no change was claimed."
                : error;
        operation_label_->setText(QString::fromStdString(operation_status_));
        operation_label_->setVisible(true);
        if (applied) {
            {
                auto& catalog = struct_dissector::g_state;
                std::lock_guard<std::mutex> lock(catalog.mtx);
                const int structure_index =
                    struct_dissector::structure_index_by_id_locked(
                        retained_edit_structure_id_);
                if (struct_dissector::valid_index(structure_index,
                        catalog.structs.size()))
                    catalog.active_struct = structure_index;
            }
            QtAnalysisBridge::instance().openView("view.types.dissector");
            dialog->accept();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    dialog->open();
}

void QtStructReconView::showFieldMenu(const QPoint& global_pos, int view_row) {
    auto* model = static_cast<QtReconFieldModel*>(model_);
    const auto* structure = model ? model->structure() : nullptr;
    const auto* field = model ? model->fieldAt(view_row) : nullptr;
    if (!structure || !field) return;
    selected_field_ = view_row;
    context_field_ = view_row;
    context_base_ = structure->base_address;
    context_offset_ = field->offset;
    context_size_ = field->size;
    context_name_ = field->name;
    context_struct_name_ = structure->name;
    const auto workspace = disasm_view::capture_selected_workspace();
    std::uint64_t absolute = 0;
    const bool address_valid = checked_field_address(structure->base_address,
        field->offset, absolute);
    const bool mapped = address_valid && workspace.workspace &&
        disasm_view::typed_address(workspace, absolute).has_value();
    aida::ui::application_ui::retained_entity_context_t retained;
    retained.owner_id = "types.reconstruction.field";
    retained.entity_id = context_struct_name_ + ":" + context_name_ + ":" +
        std::to_string(context_offset_);
    retained.entity_generation = workspace.publication
        ? workspace.publication->generation : 0;
    retained.active_view = aida::ui::stable_view_id_t("view.types.struct_recon");
    const int retained_field = context_field_;
    const std::uint64_t retained_base = context_base_;
    const std::uint64_t retained_offset = context_offset_;
    const int retained_size = context_size_;
    const std::string retained_name = context_name_;
    const std::string retained_struct_name = context_struct_name_;
    const std::uint64_t retained_field_hash = field_identity_hash(*field);
    const std::uint64_t retained_workspace_generation = workspace.workspace
        ? workspace.workspace->generation() : 0;
    const std::uint64_t retained_analysis_revision = workspace.publication
        ? workspace.publication->analysis_revision : 0;
    const std::string retained_workspace_id = workspace.workspace
        ? workspace.workspace->identity().binary_id().to_hex() : std::string{};
    retained.validate_identity = [retained_field, retained_base, retained_offset,
        retained_size, retained_name, retained_struct_name, workspace,
        retained_workspace_generation, retained_analysis_revision,
        retained_workspace_id, retained_field_hash] {
        const auto selected = disasm_view::capture_selected_workspace();
        if (!workspace.workspace || !workspace.publication ||
            workspace.workspace->closing() || workspace.workspace->closed() ||
            workspace.workspace->generation() != retained_workspace_generation ||
            workspace.publication->analysis_revision != retained_analysis_revision ||
            !selected.workspace || !selected.publication ||
            selected.workspace != workspace.workspace ||
            selected.publication != workspace.publication ||
            selected.workspace->identity().binary_id().to_hex() != retained_workspace_id ||
            selected.workspace->generation() != retained_workspace_generation ||
            selected.publication->analysis_revision != retained_analysis_revision)
            return aida::ui::capability_state_t::unavailable(
                "The type workspace changed; select the field again");
        const auto snapshot = struct_recon::capture_current_snapshot();
        if (!snapshot || retained_field < 0 ||
            retained_field >= static_cast<int>(snapshot->fields.size()) ||
            snapshot->base_address != retained_base ||
            snapshot->name != retained_struct_name)
            return aida::ui::capability_state_t::unavailable(
                "The reconstruction snapshot changed; select the field again");
        const auto& candidate =
            snapshot->fields[static_cast<std::size_t>(retained_field)];
        return candidate.offset == retained_offset && candidate.size == retained_size &&
            candidate.name == retained_name &&
            field_identity_hash(candidate) == retained_field_hash
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The retained field identity no longer matches the live reconstruction");
    };
    const auto add_action = [&retained](std::string id, bool enabled,
        const char* reason, auto invoke) {
        aida::ui::application_ui::retained_entity_action_t action;
        action.action_id = std::move(id);
        action.capability = enabled ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(reason);
        action.invoke = std::move(invoke);
        retained.actions.push_back(std::move(action));
    };
    add_action("types.reconstruction.field.follow_disassembly", mapped,
        "The field address is not mapped by the selected analysis workspace",
        [absolute, workspace] {
            disasm_view::goto_address(absolute, workspace);
            QtAnalysisBridge::instance().openView("document.disassembly");
            return aida::ui::action_handler_result_t::completed();
        });
    const std::size_t shown = (std::min)(field->accesses.size(), std::size_t{64});
    for (std::size_t index = 0; index < shown; ++index) {
        const auto access = field->accesses[index];
        add_action("types.reconstruction.field.follow_access." +
            std::to_string(index + 1),
            static_cast<bool>(workspace.workspace), "No analysis workspace is selected",
            [access, workspace] {
                disasm_view::goto_address(access.instruction_addr, workspace);
                QtAnalysisBridge::instance().openView("document.disassembly");
                return aida::ui::action_handler_result_t::completed();
            });
    }
    const std::string field_name = field->name;
    const std::string field_type = struct_recon::field_type_name(field->type);
    add_action("types.reconstruction.field.copy_name", true, "", [field_name] {
        clipboard::set_text(QString::fromStdString(field_name));
        return aida::ui::action_handler_result_t::completed();
    });
    add_action("types.reconstruction.field.copy_type", true, "", [field_type] {
        clipboard::set_text(QString::fromStdString(field_type));
        return aida::ui::action_handler_result_t::completed();
    });
    add_action("types.reconstruction.field.copy_offset", true, "", [field] {
        char text[32]{};
        std::snprintf(text, sizeof(text), "0x%llX",
            static_cast<unsigned long long>(field->offset));
        clipboard::set_text(QString::fromLatin1(text));
        return aida::ui::action_handler_result_t::completed();
    });
    add_action("types.reconstruction.field.copy_absolute_address", address_valid,
        "The field address overflowed the target address range", [absolute] {
            char text[32]{};
            std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(absolute));
            clipboard::set_text(QString::fromLatin1(text));
            return aida::ui::action_handler_result_t::completed();
        });
    add_action("types.reconstruction.field.copy_access_evidence",
        !field->accesses.empty() && field->accesses.size() <= 4096,
        field->accesses.size() > 4096
            ? "The access evidence exceeds the bounded 4,096-entry export limit"
            : "No monitored instruction access references were captured for this field",
        [field] {
            if (field->accesses.size() > 4096)
                return aida::ui::action_handler_result_t::failed(
                    "The access evidence exceeds the bounded 4,096-entry export limit");
            std::string evidence;
            evidence.reserve(64U * 1024U);
            for (const auto& access : field->accesses) {
                char prefix[96]{};
                std::snprintf(prefix, sizeof(prefix), "0x%llX %s ",
                    static_cast<unsigned long long>(access.instruction_addr),
                    access.is_write ? "write" : "read");
                const std::size_t required = std::strlen(prefix) +
                    access.disasm_text.size() + 1;
                if (required > 64U * 1024U - evidence.size())
                    return aida::ui::action_handler_result_t::failed(
                        "The access evidence exceeds the bounded 64 KiB export limit");
                evidence += prefix;
                evidence += access.disasm_text;
                evidence.push_back('\n');
            }
            clipboard::set_text(QString::fromStdString(evidence));
            return aida::ui::action_handler_result_t::completed();
        });
    add_action("types.reconstruction.field.declare_apply", true,
        "The retained field is stale", [this] {
            std::string detail;
            const bool completed = declareAndApplyCurrent(detail);
            operation_status_ = detail;
            operation_error_ = !completed;
            operation_label_->setText(QString::fromStdString(operation_status_));
            operation_label_->setVisible(true);
            return completed ? aida::ui::action_handler_result_t::completed()
                : aida::ui::action_handler_result_t::failed(detail);
        });
    const auto editable_binding = findEditableBinding(*structure, *field);
    const bool workspace_available = workspace.workspace && workspace.publication &&
        !workspace.workspace->closing() && !workspace.workspace->closed();
    const bool editable_available = workspace_available &&
        editable_binding.has_value() && struct_dissector::catalog_mutation_available();
    const char* editable_reason = !workspace_available
        ? "Select the analysis workspace that owns this reconstruction first"
        : !editable_binding
        ? "Create or select the exact editable structure and field in Structure Dissector first"
        : !struct_dissector::catalog_mutation_available()
        ? "Another Structure Dissector persistence transaction is running"
        : "The retained reconstructed field is stale";
    add_action("types.reconstruction.field.rename", editable_available,
        editable_reason, [this, retained_field] {
            showRetainedFieldEdit(static_cast<int>(retained_edit_kind_t::rename),
                retained_field);
            return aida::ui::action_handler_result_t::completed();
        });
    add_action("types.reconstruction.field.set_type", editable_available,
        editable_reason, [this, retained_field] {
            showRetainedFieldEdit(static_cast<int>(retained_edit_kind_t::retype),
                retained_field);
            return aida::ui::action_handler_result_t::completed();
        });
    const auto process = workspace.workspace
        ? workspace.workspace->identity().process()
        : std::optional<aida::analysis::process_identity_t>{};
    const bool process_current = process && driver_bridge::is_loaded() &&
        driver_bridge::attached_pid() != 0 &&
        driver_bridge::attached_pid() == process->pid;
    const bool live_edit_available = editable_available && process_current &&
        editable_binding && editable_binding->live_snapshot_current;
    const char* live_edit_reason = !editable_binding
        ? "Create or select the exact editable structure and field in Structure Dissector first"
        : !process_current
        ? "Attach the original process workspace before editing live memory"
        : !editable_binding->live_snapshot_current
        ? "Refresh the exact Structure Dissector field at this reconstruction base first"
        : editable_reason;
    add_action("types.reconstruction.field.edit_live", live_edit_available,
        live_edit_reason, [this, retained_field] {
            showRetainedFieldEdit(static_cast<int>(retained_edit_kind_t::live_value),
                retained_field);
            return aida::ui::action_handler_result_t::completed();
        });
    aida::automation_ui::entity_evidence::snapshot_t evidence;
    evidence.workspace_id = retained_workspace_id;
    evidence.source_view_id = "view.types.struct_recon";
    evidence.source_kind = "reconstructed_field";
    evidence.entity_id = retained.entity_id;
    evidence.display_label = retained_struct_name + "." + field_name;
    constexpr std::size_t maximum_evidence_bytes = 64U * 1024U;
    const auto append_evidence = [&](const std::string& value) {
        if (value.size() > maximum_evidence_bytes - evidence.excerpt.size())
            return false;
        evidence.excerpt.append(value);
        return true;
    };
    if (!append_evidence("Structure: ") || !append_evidence(retained_struct_name) ||
        !append_evidence("\nField: ") || !append_evidence(field_name) ||
        !append_evidence("\nType: ") || !append_evidence(field_type) ||
        !append_evidence("\nOffset: ") ||
        !append_evidence(std::to_string(retained_offset)) ||
        !append_evidence("\nSize: ") ||
        !append_evidence(std::to_string(retained_size)) ||
        !append_evidence("\nCaptured accesses: ") ||
        !append_evidence(std::to_string(field->accesses.size())))
        evidence.excerpt.clear();
    const std::size_t evidence_accesses = (std::min)(field->accesses.size(),
        std::size_t{32});
    for (std::size_t index = 0; index < evidence_accesses &&
        !evidence.excerpt.empty(); ++index) {
        const auto& access = field->accesses[index];
        if (!append_evidence("\n") ||
            !append_evidence(std::to_string(access.instruction_addr)) ||
            !append_evidence(access.is_write ? " write " : " read ") ||
            !append_evidence(access.disasm_text)) {
            evidence.excerpt.clear();
            break;
        }
    }
    evidence.address = address_valid ? absolute : 0;
    evidence.revision = retained_analysis_revision;
    evidence.generation = retained_workspace_generation;
    evidence.sensitive = true;
    const bool evidence_available = !evidence.excerpt.empty() && workspace.workspace;
    aida::automation_ui::entity_evidence::append_actions(retained,
        std::move(evidence), evidence_available
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The retained reconstruction field or workspace is stale"));
    QtAnalysisBridge::instance().showRetainedMenu(retained,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}

