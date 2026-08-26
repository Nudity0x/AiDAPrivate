#pragma once

#include "core/disasm/disasm_view.hpp"
#include "qt/bridge/aida_dialog.hpp"

#include <QAbstractTableModel>
#include <QString>

#include <vector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaNotice;
}

namespace aida::qt::disasm::dialogs {

class PatchDiffModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit PatchDiffModel(QObject* parent = nullptr);

    void set_bytes(const std::vector<std::uint8_t>& original,
                   const std::vector<std::uint8_t>& proposed);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    const std::vector<std::uint8_t>* original_ = nullptr;
    const std::vector<std::uint8_t>* proposed_ = nullptr;
    std::vector<std::uint8_t> original_copy_;
    std::vector<std::uint8_t> proposed_copy_;
};

class PatchBytesEdit;

class AidaStaticPatchReviewDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    AidaStaticPatchReviewDialog(disasm_view::workspace_context_t context,
                                disasm_view::static_patch_init_t init,
                                QWidget* parent = nullptr);
    ~AidaStaticPatchReviewDialog() override;

    void reconfigure(disasm_view::workspace_context_t context,
                     disasm_view::static_patch_init_t init);

private:
    void revalidate();
    void reparse();
    void commit();
    void undo_overlay();
    void redo_overlay();
    void revert_overlay();

    disasm_view::workspace_context_t context_;
    disasm_view::static_patch_init_t init_;
    std::vector<std::uint8_t> proposed_;
    QString parse_error_;
    PatchBytesEdit* bytes_ = nullptr;
    QLineEdit* description_ = nullptr;
    PatchDiffModel* diff_model_ = nullptr;
    QTableView* diff_ = nullptr;
    widgets::AidaNotice* stale_notice_ = nullptr;
    widgets::AidaNotice* pending_notice_ = nullptr;
    widgets::AidaNotice* status_notice_ = nullptr;
    widgets::AidaNotice* error_notice_ = nullptr;
    widgets::AidaNotice* parse_notice_ = nullptr;
    QLabel* existing_badge_ = nullptr;
    QPushButton* commit_ = nullptr;
    QPushButton* undo_ = nullptr;
    QPushButton* redo_ = nullptr;
    QPushButton* revert_ = nullptr;
    QTimer* revalidate_timer_ = nullptr;
};

}
