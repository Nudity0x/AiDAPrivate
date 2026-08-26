#include "qt/disasm/dialogs/disasm_xref_popup.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

#include <QDialogButtonBox>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTableView>
#include <QVBoxLayout>

namespace aida::qt::disasm::dialogs {

XrefTableModel::XrefTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void XrefTableModel::set_entries(std::vector<disasm_view::xref_popup_entry_t> entries)
{
    beginResetModel();
    entries_ = std::move(entries);
    rebuild_visible();
    endResetModel();
}

void XrefTableModel::set_filter(const QString& filter)
{
    if (filter_ == filter)
        return;
    beginResetModel();
    filter_ = filter;
    rebuild_visible();
    endResetModel();
}

void XrefTableModel::rebuild_visible()
{
    visible_.clear();
    visible_.reserve(entries_.size());
    const std::string filter = filter_.toStdString();
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto& entry = entries_[index];
        if (!filter.empty()) {
            char address[32]{};
            std::snprintf(address, sizeof(address), "%016llX",
                static_cast<unsigned long long>(entry.addr));
            if (entry.function_name.find(filter) == std::string::npos &&
                std::string(address).find(filter) == std::string::npos)
                continue;
        }
        visible_.push_back(static_cast<int>(index));
    }
}

const disasm_view::xref_popup_entry_t* XrefTableModel::entry_at(int row) const
{
    if (row < 0 || static_cast<std::size_t>(row) >= visible_.size())
        return nullptr;
    return &entries_[static_cast<std::size_t>(visible_[static_cast<std::size_t>(row)])];
}

int XrefTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

int XrefTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 2;
}

QVariant XrefTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};
    if (role == Qt::FontRole && index.column() == 0)
        return theme::fonts::codeRegular();
    if (role != Qt::DisplayRole)
        return {};
    const auto* entry = entry_at(index.row());
    if (!entry)
        return {};
    if (index.column() == 0)
        return QStringLiteral("%016llX").arg(
            static_cast<unsigned long long>(entry->addr));
    return QString::fromStdString(entry->function_name);
}

QVariant XrefTableModel::headerData(int section, Qt::Orientation orientation,
                                    int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    return section == 0 ? QStringLiteral("Address") : QStringLiteral("Function");
}

AidaDisasmXrefPopup::AidaDisasmXrefPopup(disasm_view::workspace_context_t context,
                                         aida::analysis::address_t address,
                                         QWidget* parent)
    : bridge::AidaDialog(parent), context_(std::move(context))
{
    setObjectName(QStringLiteral("aida.disasm.dialog.xrefs"));
    setWindowTitle(QStringLiteral("Cross references"));
    setModal(false);
    const auto& t = theme::tokens();
    resize(static_cast<int>(t.shell.min_panel_w) * 8, t.control.height_md * 18);
    auto* layout = new QVBoxLayout(this);
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("aida.disasm.dialog.xrefs.filter"));
    filter_->setPlaceholderText(QStringLiteral("Filter by name or address"));
    filter_->setToolTip(QStringLiteral(
        "Show only cross references whose function name or hex address contains the text"));
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("aida.disasm.dialog.xrefs.status"));
    status_->setText(QStringLiteral("Searching cross references..."));
    layout->addWidget(status_);
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("aida.disasm.dialog.xrefs.table"));
    model_ = new XrefTableModel(table_);
    table_->verticalHeader()->setDefaultSectionSize(t.table.compact_row_h);
    table_->verticalHeader()->setVisible(false);
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setToolTip(QStringLiteral(
        "Double-click or press Enter to navigate to the referencing address"));
    const QFontMetricsF address_metrics(theme::fonts::codeRegular());
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    table_->horizontalHeader()->resizeSection(0,
        static_cast<int>(address_metrics.horizontalAdvance(
            QStringLiteral("0000000000000000"))) + t.table.cell_pad_x * 2);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(table_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        model_->set_filter(text);
    });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (const auto* entry = model_->entry_at(index.row())) {
            Q_EMIT navigateRequested(entry->addr);
            accept();
        }
    });
    connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        if (const auto* entry = model_->entry_at(index.row())) {
            Q_EMIT navigateRequested(entry->addr);
            accept();
        }
    });
    static_cast<void>(address);
}

void AidaDisasmXrefPopup::set_results(
    std::vector<disasm_view::xref_popup_entry_t> results)
{
    scanning_ = false;
    status_->hide();
    model_->set_entries(std::move(results));
    if (model_->rowCount() > 0)
        table_->selectRow(0);
}

void AidaDisasmXrefPopup::set_scanning(bool scanning)
{
    scanning_ = scanning;
    status_->setVisible(scanning_);
}

void AidaDisasmXrefPopup::set_error(const QString& error)
{
    if (error.isEmpty()) {
        status_->hide();
        return;
    }
    status_->setText(error);
    status_->show();
}

}
