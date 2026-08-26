#include "qt/ai/qt_ai_evidence_view.hpp"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "qt/ai/qt_ai_chat_dialogs.hpp"
#include "qt/ai/qt_ai_domain.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::ai {

using aida::automation_ui::evidence_envelope_t;
using aida::automation_ui::message_action_t;

AidaEvidenceModel::AidaEvidenceModel(QObject* parent) : QAbstractListModel(parent) {
    reload();
}

int AidaEvidenceModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(items_ ? items_->size() : 0);
}

QVariant AidaEvidenceModel::data(const QModelIndex& index, int role) const {
    const auto* item = envelopeAt(index.row());
    if (!item)
        return {};
    if (role == Qt::DisplayRole) {
        const QString label = item->display_label.empty()
            ? QString::fromStdString(item->entity_id)
            : QString::fromStdString(item->display_label);
        return label + QStringLiteral("    ") + QString::fromStdString(item->source_kind) +
            (item->truncated ? QStringLiteral("  bounded") : QString());
    }
    return {};
}

void AidaEvidenceModel::reload() {
    beginResetModel();
    items_ = aida::automation_ui::evidence_snapshot();
    endResetModel();
}

const evidence_envelope_t* AidaEvidenceModel::envelopeAt(int row) const {
    if (!items_ || row < 0 || row >= static_cast<int>(items_->size()))
        return nullptr;
    return &(*items_)[static_cast<std::size_t>(row)];
}

AidaProposalReviewCard::AidaProposalReviewCard(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.ai.evidence.review_card"));
    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(t.panel.padding, t.panel.padding,
                               t.panel.padding, t.panel.padding);
    layout->setSpacing(t.spacing.xs + t.spacing.xxs);
    auto* top = new QHBoxLayout();
    top->setSpacing(t.spacing.sm);
    auto* header = new QLabel(QStringLiteral("AI CHANGE REVIEW"), this);
    header->setProperty("aidaVariant", QStringLiteral("secondary"));
    state_label_ = new QLabel(this);
    state_label_->setProperty("aidaVariant", QStringLiteral("info"));
    top->addWidget(header);
    top->addWidget(state_label_);
    top->addStretch(1);
    layout->addLayout(top);
    title_label_ = new QLabel(this);
    title_label_->setFont(theme::fonts::bodyEm());
    layout->addWidget(title_label_);
    provenance_label_ = new QLabel(this);
    provenance_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(provenance_label_);
    rationale_label_ = new QLabel(this);
    rationale_label_->setWordWrap(true);
    layout->addWidget(rationale_label_);
    auto* panes = new QHBoxLayout();
    panes->setSpacing(t.spacing.sm);
    before_ = new QPlainTextEdit(this);
    before_->setReadOnly(true);
    before_->setFont(theme::fonts::codeRegular());
    after_ = new QPlainTextEdit(this);
    after_->setReadOnly(true);
    after_->setFont(theme::fonts::codeRegular());
    auto* before_box = new QVBoxLayout();
    before_box->addWidget(new QLabel(QStringLiteral("BEFORE"), this));
    before_box->addWidget(before_);
    auto* after_box = new QVBoxLayout();
    after_box->addWidget(new QLabel(QStringLiteral("AFTER"), this));
    after_box->addWidget(after_);
    panes->addLayout(before_box, 1);
    panes->addLayout(after_box, 1);
    layout->addLayout(panes, 1);
    consequence_label_ = new QLabel(this);
    consequence_label_->setWordWrap(true);
    layout->addWidget(consequence_label_);
    detail_label_ = new QLabel(this);
    detail_label_->setWordWrap(true);
    layout->addWidget(detail_label_);
    auto* actions = new QHBoxLayout();
    apply_button_ = new QPushButton(QStringLiteral("Apply Reviewed Change"), this);
    apply_button_->setProperty("aidaVariant", QStringLiteral("primary"));
    reject_button_ = new QPushButton(QStringLiteral("Reject"), this);
    actions->addWidget(apply_button_);
    actions->addWidget(reject_button_);
    actions->addStretch(1);
    layout->addLayout(actions);
    connect(apply_button_, &QPushButton::clicked, this, [this] {
        Q_EMIT applyRequested();
    });
    connect(reject_button_, &QPushButton::clicked, this, [this] {
        const auto publication = aida::automation_ui::reverse_engineering_proposal_snapshot();
        const auto result = aida::automation_ui::execute_message_action(
            publication->source, message_action_t::reject_change);
        Q_EMIT feedback(QString::fromStdString(result.detail));
        if (result.succeeded && !result.target_view_id.empty())
            Q_EMIT openViewRequested(QString::fromStdString(result.target_view_id));
    });
    refresh();
}

void AidaProposalReviewCard::refresh() {
    const auto publication = aida::automation_ui::reverse_engineering_proposal_snapshot();
    const auto& proposal = *publication;
    const bool visible = proposal.kind !=
            aida::automation_ui::reverse_engineering_proposal_kind_t::none &&
        (proposal.pending || proposal.applying || proposal.review_staged ||
         proposal.applied || proposal.rejected || proposal.stale);
    setVisible(visible);
    if (!visible)
        return;

    using state_t = aida::automation_ui::reverse_engineering_proposal_state_t;
    const char* state_text = "PENDING";
    const char* state_variant = "info";
    if (proposal.state == state_t::queued) { state_text = "QUEUED"; state_variant = "info"; }
    else if (proposal.state == state_t::running) { state_text = "VALIDATING"; state_variant = "accent"; }
    else if (proposal.partial) { state_text = "PARTIAL / ROLLBACK AVAILABLE"; state_variant = "warning"; }
    else if (proposal.state == state_t::error) { state_text = "ERROR"; state_variant = "error"; }
    else if (proposal.applying) { state_text = "APPLYING"; state_variant = "accent"; }
    else if (proposal.review_staged) { state_text = "STAGED / REVIEW REQUIRED"; state_variant = "warning"; }
    else if (proposal.applied && proposal.terminal_readback) { state_text = "VERIFIED"; state_variant = "success"; }
    else if (proposal.rejected) { state_text = "REJECTED"; state_variant = "neutral"; }
    else if (proposal.stale) { state_text = "STALE"; state_variant = "warning"; }
    if (state_label_->property("aidaVariant") != QLatin1String(state_variant)) {
        state_label_->setProperty("aidaVariant", QString::fromLatin1(state_variant));
        state_label_->style()->unpolish(state_label_);
        state_label_->style()->polish(state_label_);
    }
    state_label_->setText(QString::fromLatin1(state_text));
    title_label_->setText(QStringLiteral("%1  |  %2")
        .arg(QString::fromStdString(proposal.kind_label),
             QString::fromStdString(proposal.target_label)));
    provenance_label_->setText(QStringLiteral("Provenance: %1")
        .arg(QString::fromStdString(proposal.provenance)));
    rationale_label_->setText(QString::fromStdString(proposal.rationale));
    before_->setPlainText(QString::fromStdString(proposal.before_value));
    after_->setPlainText(QString::fromStdString(proposal.after_value));
    consequence_label_->setText(QStringLiteral("Consequence: %1")
        .arg(QString::fromStdString(proposal.consequence)));
    detail_label_->setText(QString::fromStdString(proposal.detail));

    const bool show_actions = proposal.pending && !proposal.applying;
    apply_button_->setVisible(show_actions);
    reject_button_->setVisible(show_actions);
    if (show_actions) {
        const auto capability = aida::automation_ui::message_action_capability(
            proposal.source, message_action_t::apply_change);
        apply_button_->setEnabled(capability.enabled);
        if (!capability.enabled)
            apply_button_->setToolTip(QString::fromStdString(capability.disabled_reason));
        else
            apply_button_->setToolTip(QString());
    }
}

AidaEvidenceView::AidaEvidenceView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.ai.evidence"));
    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(t.spacing.sm - t.spacing.xxs, t.spacing.sm - t.spacing.xxs,
                               t.spacing.sm - t.spacing.xxs, t.spacing.sm - t.spacing.xxs);
    layout->setSpacing(t.spacing.xs);
    proposal_card_ = new AidaProposalReviewCard(this);
    layout->addWidget(proposal_card_, 1);
    feedback_label_ = new QLabel(this);
    feedback_label_->setObjectName(QStringLiteral("aida.ai.evidence.feedback"));
    feedback_label_->setWordWrap(true);
    feedback_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    feedback_label_->setVisible(false);
    layout->addWidget(feedback_label_);
    model_ = new AidaEvidenceModel(this);
    list_ = new QListView(this);
    list_->setObjectName(QStringLiteral("aida.ai.evidence.list"));
    list_->setModel(model_);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setAccessibleName(QStringLiteral("Collected evidence"));
    layout->addWidget(list_, 2);
    empty_state_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No evidence collected"),
        QStringLiteral("Use Add to Chat or Assign to Agent from a source context menu."),
        this);
    empty_state_->setObjectName(QStringLiteral("aida.ai.evidence.empty"));
    layout->addWidget(empty_state_);

    connect(proposal_card_, &AidaProposalReviewCard::feedback, this,
            [this](const QString& detail) {
        feedback_label_->setText(detail);
        feedback_label_->setVisible(!detail.isEmpty());
        refreshNow();
    });
    connect(proposal_card_, &AidaProposalReviewCard::openViewRequested, this,
            [](const QString& view_id) { open_ai_view(view_id.toStdString()); });
    connect(proposal_card_, &AidaProposalReviewCard::applyRequested, this, [this] {
        const auto publication = aida::automation_ui::reverse_engineering_proposal_snapshot();
        AidaApplyChangeDialog::request(publication->source, this,
            [this](const QString& detail) {
                feedback_label_->setText(detail);
                feedback_label_->setVisible(!detail.isEmpty());
                refreshNow();
            });
    });
    connect(list_, &QListView::clicked, this, [this](const QModelIndex& index) {
        const auto* item = model_->envelopeAt(index.row());
        if (item)
            selected_id_ = item->id;
    });
    connect(list_, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto* item = model_->envelopeAt(index.row());
        if (!item)
            return;
        std::string reason;
        if (!aida::automation_ui::navigate_to_evidence_source(item->id, reason)) {
            feedback_label_->setText(QString::fromStdString(reason));
            feedback_label_->setVisible(true);
        }
    });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const QModelIndex index = list_->indexAt(pos);
        if (index.isValid())
            showItemMenu(list_->viewport()->mapToGlobal(pos), index.row());
    });
    proposal_card_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(proposal_card_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        showProposalMenu(proposal_card_->mapToGlobal(pos));
    });

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(500);
    connect(refresh_timer_, &QTimer::timeout, this, &AidaEvidenceView::refreshNow);
    if (isVisible())
        refresh_timer_->start();
}

void AidaEvidenceView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshNow();
    if (!refresh_timer_->isActive())
        refresh_timer_->start();
}

void AidaEvidenceView::hideEvent(QHideEvent* event) {
    refresh_timer_->stop();
    QWidget::hideEvent(event);
}

void AidaEvidenceView::refreshNow() {
    model_->reload();
    proposal_card_->refresh();
    const bool has_items = model_->rowCount() > 0;
    list_->setVisible(has_items);
    empty_state_->setVisible(!has_items && !proposal_card_->isVisible());
    if (!selected_id_.empty()) {
        for (int row = 0; row < model_->rowCount(); ++row) {
            const auto* item = model_->envelopeAt(row);
            if (item != nullptr && item->id == selected_id_) {
                list_->setCurrentIndex(model_->index(row));
                break;
            }
        }
    }
}

void AidaEvidenceView::showItemMenu(const QPoint& global_pos, int row) {
    const auto* item = model_->envelopeAt(row);
    if (!item)
        return;
    const std::string retained_id = item->id;
    const auto retained_generation = item->generation;
    const auto retained_hash = item->snapshot_hash;

    auto* menu = new QMenu(this);
    auto* return_source = menu->addAction(QStringLiteral("Return to Source"));
    auto* add_chat = menu->addAction(QStringLiteral("Add to Chat"));
    auto* assign_agent = menu->addAction(QStringLiteral("Assign to Agent"));
    const char* stale_reason = item->stale_reason.empty()
        ? "This evidence is stale; return to its source and collect it again"
        : item->stale_reason.c_str();
    add_chat->setEnabled(!item->stale);
    assign_agent->setEnabled(!item->stale);
    if (item->stale) {
        add_chat->setToolTip(QString::fromLatin1(stale_reason));
        assign_agent->setToolTip(QString::fromLatin1(stale_reason));
    }
    auto* copy_id = menu->addAction(QStringLiteral("Copy ID"));

    auto valid = [retained_id, retained_generation, retained_hash]() -> bool {
        const auto live = aida::automation_ui::evidence_snapshot();
        const auto found = std::find_if(live->begin(), live->end(),
            [&](const evidence_envelope_t& candidate) { return candidate.id == retained_id; });
        return found != live->end() && found->generation == retained_generation &&
            found->snapshot_hash == retained_hash;
    };
    auto report = [this](bool ok, const std::string& reason) {
        if (!ok) {
            feedback_label_->setText(reason.empty()
                ? QStringLiteral("The evidence envelope changed; select it again")
                : QString::fromStdString(reason));
            feedback_label_->setVisible(true);
        }
    };
    connect(return_source, &QAction::triggered, this, [this, retained_id, valid, report] {
        if (!valid()) {
            report(false, "The evidence envelope changed; select it again");
            return;
        }
        std::string reason;
        report(aida::automation_ui::navigate_to_evidence_source(retained_id, reason), reason);
    });
    connect(add_chat, &QAction::triggered, this, [this, retained_id, valid, report] {
        if (!valid()) {
            report(false, "The evidence envelope changed; select it again");
            return;
        }
        std::string reason;
        report(aida::automation_ui::queue_evidence_for_chat(retained_id, reason), reason);
    });
    connect(assign_agent, &QAction::triggered, this, [this, retained_id, valid, report] {
        if (!valid()) {
            report(false, "The evidence envelope changed; select it again");
            return;
        }
        std::string reason;
        report(aida::automation_ui::queue_evidence_for_agent(retained_id, reason), reason);
    });
    connect(copy_id, &QAction::triggered, this, [retained_id] {
        clipboard::set_text(QString::fromStdString(retained_id));
    });
    menu->popup(global_pos);
}

void AidaEvidenceView::showProposalMenu(const QPoint& global_pos) {
    const auto publication = aida::automation_ui::reverse_engineering_proposal_snapshot();
    const auto& proposal = *publication;
    if (proposal.kind == aida::automation_ui::reverse_engineering_proposal_kind_t::none)
        return;
    const auto source = proposal.source;
    const auto operation_id = proposal.operation_id;

    auto* menu = new QMenu(this);
    auto* review = menu->addAction(QStringLiteral("Review Change"));
    auto* apply = menu->addAction(QStringLiteral("Apply Reviewed Change"));
    auto* reject = menu->addAction(QStringLiteral("Reject"));

    auto capability_for = [source](message_action_t action) {
        return aida::automation_ui::message_action_capability(source, action);
    };
    review->setEnabled(capability_for(message_action_t::review_change).enabled);
    apply->setEnabled(capability_for(message_action_t::apply_change).enabled);
    reject->setEnabled(capability_for(message_action_t::reject_change).enabled);

    auto valid = [source, operation_id]() -> bool {
        const auto current_publication =
            aida::automation_ui::reverse_engineering_proposal_snapshot();
        const auto& current = *current_publication;
        return current.source.session_id == source.session_id &&
            current.source.fingerprint == source.fingerprint &&
            current.operation_id == operation_id && current.pending;
    };
    auto dispatch = [this](const aida::automation_ui::message_identity_t& src,
                           message_action_t action) {
        const auto result = aida::automation_ui::execute_message_action(src, action);
        feedback_label_->setText(QString::fromStdString(result.detail));
        feedback_label_->setVisible(!result.detail.empty());
        if (result.succeeded && !result.target_view_id.empty())
            open_ai_view(result.target_view_id);
        refreshNow();
    };
    connect(review, &QAction::triggered, this, [source, valid, dispatch] {
        if (valid())
            dispatch(source, message_action_t::review_change);
    });
    connect(apply, &QAction::triggered, this, [this, source, valid] {
        if (valid())
            AidaApplyChangeDialog::request(source, this, {});
    });
    connect(reject, &QAction::triggered, this, [source, valid, dispatch] {
        if (valid())
            dispatch(source, message_action_t::reject_change);
    });
    menu->popup(global_pos);
}

}
