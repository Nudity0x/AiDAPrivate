#pragma once

#include <QAbstractListModel>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/ai/standalone_chat.hpp"

class QLabel;
class QListView;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace aida::qt::widgets { class AidaStateView; }

namespace aida::qt::ai {

class AidaEvidenceModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AidaEvidenceModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void reload();
    const aida::automation_ui::evidence_envelope_t* envelopeAt(int row) const;

private:
    std::shared_ptr<const std::vector<aida::automation_ui::evidence_envelope_t>> items_;
};

class AidaProposalReviewCard : public QWidget {
    Q_OBJECT
public:
    explicit AidaProposalReviewCard(QWidget* parent = nullptr);

    void refresh();

Q_SIGNALS:
    void feedback(const QString& detail);
    void openViewRequested(const QString& view_id);
    void applyRequested();

private:
    QLabel* state_label_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* provenance_label_ = nullptr;
    QLabel* rationale_label_ = nullptr;
    QPlainTextEdit* before_ = nullptr;
    QPlainTextEdit* after_ = nullptr;
    QLabel* consequence_label_ = nullptr;
    QLabel* detail_label_ = nullptr;
    QPushButton* apply_button_ = nullptr;
    QPushButton* reject_button_ = nullptr;
};

class AidaEvidenceView : public QWidget {
    Q_OBJECT
public:
    explicit AidaEvidenceView(QWidget* parent = nullptr);

    void refreshNow();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void showItemMenu(const QPoint& global_pos, int row);
    void showProposalMenu(const QPoint& global_pos);

    AidaProposalReviewCard* proposal_card_ = nullptr;
    AidaEvidenceModel* model_ = nullptr;
    QListView* list_ = nullptr;
    widgets::AidaStateView* empty_state_ = nullptr;
    QLabel* feedback_label_ = nullptr;
    std::string selected_id_;
    QTimer* refresh_timer_ = nullptr;
};

}
