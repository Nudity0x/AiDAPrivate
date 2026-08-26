#include "qt/ai/qt_ai_chat_history.hpp"

#include <QAction>
#include <QDateTime>
#include <QFileDialog>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>

#include "core/ai/conversation_evidence_store.hpp"
#include "core/ai/standalone_chat.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::ai {

AidaConversationBridge::AidaConversationBridge(QObject* parent) : QObject(parent) {}

AidaConversationBridge& AidaConversationBridge::instance() {
    static AidaConversationBridge* bridge = [] {
        auto* created = new AidaConversationBridge();
        created->installHook();
        return created;
    }();
    return *bridge;
}

void AidaConversationBridge::installHook() {
    conversations::set_catalog_change_hook([this] {
        const auto revision = aida::conversation_store::status();
        Q_UNUSED(revision);
        const bool replaced = observed_current_id_ != conversations::current_id;
        observed_current_id_ = conversations::current_id;
        const bool catalog_moved = observed_generation_ != conversations::catalog_generation;
        observed_generation_ = conversations::catalog_generation;
        if (catalog_moved || replaced) {
            QMetaObject::invokeMethod(this, [this, replaced] {
                Q_EMIT catalogChanged();
                if (replaced)
                    Q_EMIT conversationReplaced();
            }, Qt::QueuedConnection);
        }
    });
}

AidaConversationModel::AidaConversationModel(QObject* parent) : QAbstractListModel(parent) {
    rebuild();
}

void AidaConversationModel::rebuild() {
    snapshot_ = conversations::catalog_snapshot();
    filtered_.clear();
    filtered_.reserve(snapshot_->size());
    auto matches = [this](std::string_view candidate) {
        if (filter_lower_.empty())
            return true;
        if (candidate.size() < filter_lower_.size())
            return false;
        for (std::size_t start = 0; start + filter_lower_.size() <= candidate.size(); ++start) {
            bool ok = true;
            for (std::size_t offset = 0; offset < filter_lower_.size(); ++offset) {
                const auto value = static_cast<unsigned char>(candidate[start + offset]);
                if (static_cast<char>(std::tolower(value)) != filter_lower_[offset]) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                return true;
        }
        return false;
    };
    for (std::size_t index = 0; index < snapshot_->size(); ++index) {
        const auto& conversation = (*snapshot_)[index];
        const std::string_view title = conversation.title.empty()
            ? std::string_view("Untitled") : std::string_view(conversation.title);
        if (matches(title))
            filtered_.push_back(index);
    }
}

int AidaConversationModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(filtered_.size());
}

QVariant AidaConversationModel::data(const QModelIndex& index, int role) const {
    const auto* summary = summaryAt(index.row());
    if (!summary)
        return {};
    if (role == Qt::DisplayRole) {
        const QString title = summary->title.empty()
            ? QStringLiteral("Untitled") : QString::fromStdString(summary->title);
        return summary->pinned ? QStringLiteral("[Pinned] ") + title : title;
    }
    if (role == Qt::ToolTipRole) {
        QString tip = QStringLiteral("%1 messages%2\n%3")
            .arg(summary->msg_count)
            .arg(summary->pinned ? QStringLiteral("\nPinned") : QString())
            .arg(QString::fromStdString(summary->id));
        if (summary->created > 0) {
            tip += QStringLiteral("\nCreated: %1")
                .arg(QDateTime::fromMSecsSinceEpoch(summary->created)
                    .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        }
        return tip;
    }
    return {};
}

void AidaConversationModel::setFilter(const QString& filter) {
    const std::string next = filter.toLower().toStdString();
    if (next == filter_lower_)
        return;
    filter_lower_ = next;
    reload();
}

void AidaConversationModel::reload() {
    beginResetModel();
    rebuild();
    endResetModel();
}

const ConversationSummary* AidaConversationModel::summaryAt(int row) const {
    if (row < 0 || row >= static_cast<int>(filtered_.size()))
        return nullptr;
    return &(*snapshot_)[filtered_[static_cast<std::size_t>(row)]];
}

std::size_t AidaConversationModel::summaryIndexForRow(int row) const {
    return filtered_[static_cast<std::size_t>(row)];
}

AidaConversationListView::AidaConversationListView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.ai.chat.history_panel"));
    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(t.spacing.xs, t.spacing.xs, t.spacing.xs, t.spacing.xs);
    layout->setSpacing(t.spacing.xs);
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("aida.ai.chat.history_filter"));
    filter_->setPlaceholderText(QStringLiteral("Filter conversations..."));
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);
    list_ = new QListView(this);
    list_->setObjectName(QStringLiteral("aida.ai.chat.history_list"));
    list_->setUniformItemSizes(true);
    model_ = new AidaConversationModel(this);
    list_->setModel(model_);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setAccessibleName(QStringLiteral("Saved conversations"));
    layout->addWidget(list_, 1);

    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        model_->setFilter(text);
    });
    connect(&AidaConversationBridge::instance(), &AidaConversationBridge::catalogChanged,
            model_, &AidaConversationModel::reload);
    connect(list_, &QListView::clicked, this, [this](const QModelIndex& index) {
        const auto* summary = model_->summaryAt(index.row());
        if (!summary)
            return;
        const auto store = aida::conversation_store::status();
        if (store.pending || store.failed)
            return;
        if (summary->id != conversations::current_id)
            conversations::load_conversation(summary->id);
    });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const QModelIndex index = list_->indexAt(pos);
        if (!index.isValid())
            return;
        openContextMenu(list_->viewport()->mapToGlobal(pos),
                        model_->summaryIndexForRow(index.row()));
    });
}

void AidaConversationListView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!shown_once_) {
        shown_once_ = true;
        const auto store = aida::conversation_store::status();
        if (!store.pending && !store.failed)
            conversations::refresh_history();
    }
}

void AidaConversationListView::refreshOnce() {
    const auto store = aida::conversation_store::status();
    if (!store.pending && !store.failed)
        conversations::refresh_history();
}

void AidaConversationListView::setBusy(bool busy) {
    busy_ = busy;
}

bool AidaConversationListView::validateIdentity(const std::string& id,
                                                std::uint64_t revision,
                                                std::string& reason) const {
    const auto live = conversations::catalog_snapshot();
    const auto found = std::find_if(live->begin(), live->end(),
        [&](const ConversationSummary& item) { return item.id == id; });
    if (found != live->end() && found->revision == revision) {
        reason.clear();
        return true;
    }
    reason = "The conversation changed after the menu opened; select it again";
    return false;
}

void AidaConversationListView::openContextMenu(const QPoint& global_pos,
                                               std::size_t summary_index) {
    const auto snapshot = conversations::catalog_snapshot();
    if (summary_index >= snapshot->size())
        return;
    const auto& conversation = (*snapshot)[summary_index];
    const std::string retained_id = conversation.id;
    const std::uint64_t retained_revision = conversation.revision;
    const bool selected = conversation.id == conversations::current_id;
    const auto store = aida::conversation_store::status();
    const bool store_blocked = store.pending || store.failed;
    const bool mutation_blocked = store_blocked || (selected && busy_);

    auto* menu = new QMenu(this);
    auto* open_action = menu->addAction(QStringLiteral("Open"));
    open_action->setEnabled(!selected && !store_blocked);
    if (selected)
        open_action->setToolTip(QStringLiteral("This conversation is already open"));
    else if (store_blocked)
        open_action->setToolTip(
            QStringLiteral("Wait for conversation persistence to recover"));
    auto* fork_action = menu->addAction(QStringLiteral("Fork"));
    fork_action->setEnabled(!mutation_blocked);
    auto* pin_action = menu->addAction(conversation.pinned ? QStringLiteral("Unpin")
                                                           : QStringLiteral("Pin"));
    pin_action->setEnabled(!store_blocked);
    auto* export_action = menu->addAction(QStringLiteral("Export Markdown..."));
    export_action->setEnabled(!store_blocked);
    auto* copy_action = menu->addAction(QStringLiteral("Copy ID"));
    menu->addSeparator();
    auto* delete_action = menu->addAction(QStringLiteral("Delete..."));
    delete_action->setEnabled(!mutation_blocked);

    auto revalidate = [this, retained_id, retained_revision]() -> bool {
        std::string stale;
        if (validateIdentity(retained_id, retained_revision, stale))
            return true;
        Q_EMIT feedbackMessage(QString::fromStdString(stale), true);
        return false;
    };
    connect(open_action, &QAction::triggered, this, [this, retained_id, revalidate] {
        if (revalidate())
            conversations::load_conversation(retained_id);
    });
    connect(fork_action, &QAction::triggered, this, [this, retained_id, revalidate] {
        if (!revalidate())
            return;
        std::string forked_id;
        if (conversations::fork_conversation(retained_id, forked_id))
            chrome::toast_info(QStringLiteral("Conversation fork queued"), 3.0);
        else
            Q_EMIT feedbackMessage(QStringLiteral("Conversation could not be forked"), true);
    });
    connect(pin_action, &QAction::triggered, this,
            [this, retained_id, pinned = conversation.pinned, revalidate] {
        if (!revalidate())
            return;
        if (conversations::set_pinned(retained_id, !pinned))
            chrome::toast_info(pinned ? QStringLiteral("Conversation unpin queued")
                                      : QStringLiteral("Conversation pin queued"), 3.0);
        else
            Q_EMIT feedbackMessage(
                QStringLiteral("Conversation pin state could not be saved"), true);
    });
    connect(export_action, &QAction::triggered, this, [this, retained_id, revalidate] {
        if (!revalidate())
            return;
        const QString path = QFileDialog::getSaveFileName(this,
            QStringLiteral("Export Conversation"), QStringLiteral("AiDA Conversation.md"),
            QStringLiteral("Markdown files (*.md);;Text files (*.txt);;All files (*.*)"));
        if (path.isEmpty())
            return;
        std::string error;
        if (conversations::export_markdown(retained_id, path.toStdString(), error))
            chrome::toast_info(QStringLiteral("Conversation export queued"), 3.0);
        else
            Q_EMIT feedbackMessage(error.empty()
                ? QStringLiteral("Conversation export failed")
                : QString::fromStdString(error), true);
    });
    connect(copy_action, &QAction::triggered, this, [retained_id] {
        clipboard::set_text(QString::fromStdString(retained_id));
    });
    connect(delete_action, &QAction::triggered, this,
            [this, retained_id, retained_revision, revalidate] {
        if (revalidate())
            Q_EMIT deleteReviewRequested(QString::fromStdString(retained_id),
                                         retained_revision);
    });
    menu->popup(global_pos);
}

}
