#pragma once

#include <QAbstractListModel>
#include <QDialog>
#include <QStyledItemDelegate>

#include <QString>

#include <string>
#include <vector>

class QLineEdit;
class QListView;
class QLabel;

namespace aida::agent {
struct agent_info_t;
}

namespace aida::qt::ai {

class AidaAgentPickerModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AidaAgentPickerModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void setFilter(const QString& query);
    void refresh();

    const aida::agent::agent_info_t* agentAt(int row) const;
    QString nameAt(int row) const;
    bool isNoticeAt(int row) const;

private:
    struct row_t {
        std::string name;
        std::string description;
        bool native = false;
        bool active = false;
        bool notice = false;
        int score = 0;
    };

    std::vector<row_t> rows_;
};

class AidaAgentPickerDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit AidaAgentPickerDelegate(AidaAgentPickerModel* model, QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    AidaAgentPickerModel* model_ = nullptr;
};

class AidaAgentPickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit AidaAgentPickerDialog(QWidget* parent = nullptr);

    static void toggleInteractive(QWidget* anchor);
    static bool pickerVisible();

    void openAndReset();

Q_SIGNALS:
    void manageAgentsRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void dispatchRow(int row);
    void moveSelection(int delta);

    AidaAgentPickerModel* model_ = nullptr;
    AidaAgentPickerDelegate* delegate_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QListView* list_ = nullptr;
    QLabel* active_label_ = nullptr;
};

}
