#pragma once

#include <QAbstractListModel>
#include <QDialog>
#include <QStyledItemDelegate>

#include <QString>

#include <string>
#include <vector>

#include "core/ai/command_registry.hpp"

class QLabel;
class QLineEdit;
class QListView;
class QPlainTextEdit;
class QSplitter;

namespace aida::qt::ai {

class AidaCommandModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AidaCommandModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void setQuery(const QString& query);
    const QString& query() const noexcept { return query_; }
    const aida::commands::command_t* commandAt(int row) const;
    bool isHeaderAt(int row) const;
    bool isNoticeAt(int row) const;
    int firstCommandRow() const;
    int nextCommandRow(int row, int delta) const;

private:
    struct row_t {
        bool header = false;
        bool notice = false;
        QString category_label;
        aida::commands::command_t command;
    };

    void rebuild();

    QString query_;
    std::vector<row_t> rows_;
};

class AidaCommandDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit AidaCommandDelegate(AidaCommandModel* model, QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    AidaCommandModel* model_ = nullptr;
};

class AidaPalettePreviewPane : public QWidget {
    Q_OBJECT
public:
    explicit AidaPalettePreviewPane(QWidget* parent = nullptr);

    void showCommand(const aida::commands::command_t& command);
    void clearPreview();

private:
    QLabel* title_ = nullptr;
    QLabel* description_ = nullptr;
    QPlainTextEdit* body_ = nullptr;
    QLabel* hints_ = nullptr;
    QLabel* source_path_ = nullptr;
};

class AidaCommandPaletteDialog : public QDialog {
    Q_OBJECT
public:
    explicit AidaCommandPaletteDialog(QWidget* parent = nullptr);

    static void toggleInteractive(QWidget* anchor);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void executeSelection(const aida::commands::command_t& command);
    void moveSelection(int delta);
    void syncPreview();

    AidaCommandModel* model_ = nullptr;
    AidaCommandDelegate* delegate_ = nullptr;
    QLineEdit* input_ = nullptr;
    QListView* list_ = nullptr;
    AidaPalettePreviewPane* preview_ = nullptr;
    QSplitter* splitter_ = nullptr;
};

}
