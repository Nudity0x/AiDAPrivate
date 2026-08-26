#pragma once

#include "core/disasm/disasm_view.hpp"
#include "qt/bridge/aida_dialog.hpp"

#include <QAbstractTableModel>
#include <QString>

#include <memory>
#include <vector>

class QLabel;
class QLineEdit;
class QTableView;

namespace aida::qt::disasm::dialogs {

class XrefTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit XrefTableModel(QObject* parent = nullptr);

    void set_entries(std::vector<disasm_view::xref_popup_entry_t> entries);
    void set_filter(const QString& filter);
    const disasm_view::xref_popup_entry_t* entry_at(int row) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    void rebuild_visible();

    std::vector<disasm_view::xref_popup_entry_t> entries_;
    std::vector<int> visible_;
    QString filter_;
};

class AidaDisasmXrefPopup : public bridge::AidaDialog {
    Q_OBJECT
public:
    AidaDisasmXrefPopup(disasm_view::workspace_context_t context,
                        aida::analysis::address_t address,
                        QWidget* parent = nullptr);

    void set_results(std::vector<disasm_view::xref_popup_entry_t> results);
    void set_scanning(bool scanning);
    void set_error(const QString& error);

Q_SIGNALS:
    void navigateRequested(quint64 address);

private:
    disasm_view::workspace_context_t context_;
    QLineEdit* filter_ = nullptr;
    QTableView* table_ = nullptr;
    XrefTableModel* model_ = nullptr;
    QLabel* status_ = nullptr;
    bool scanning_ = false;
};

}
