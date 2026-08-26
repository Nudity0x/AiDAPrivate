#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/ai/conversation_history.hpp"

class QLineEdit;
class QListView;
class QModelIndex;
class QShowEvent;

namespace aida::qt::ai {

class AidaConversationBridge : public QObject {
    Q_OBJECT
public:
    static AidaConversationBridge& instance();

    void installHook();

Q_SIGNALS:
    void catalogChanged();
    void conversationReplaced();

private:
    explicit AidaConversationBridge(QObject* parent = nullptr);
    std::uint64_t observed_revision_ = 0;
    std::uint64_t observed_generation_ = 0;
    std::string observed_current_id_;
};

class AidaConversationModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AidaConversationModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void setFilter(const QString& filter);
    void reload();
    const ConversationSummary* summaryAt(int row) const;
    std::size_t summaryIndexForRow(int row) const;

private:
    void rebuild();

    std::shared_ptr<const std::vector<ConversationSummary>> snapshot_;
    std::vector<std::size_t> filtered_;
    std::string filter_lower_;
};

class AidaConversationListView : public QWidget {
    Q_OBJECT
public:
    explicit AidaConversationListView(QWidget* parent = nullptr);

    void refreshOnce();
    void setBusy(bool busy);

Q_SIGNALS:
    void deleteReviewRequested(const QString& conversation_id, quint64 revision);
    void feedbackMessage(const QString& message, bool is_error);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void openContextMenu(const QPoint& global_pos, std::size_t summary_index);
    bool validateIdentity(const std::string& id, std::uint64_t revision,
                          std::string& reason) const;

    AidaConversationModel* model_ = nullptr;
    QListView* list_ = nullptr;
    QLineEdit* filter_ = nullptr;
    bool shown_once_ = false;
    bool busy_ = false;
};

}
