#include "qt/editor/aida_code_group_host.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStackedLayout>
#include <QTabBar>
#include <QVBoxLayout>
#include <QtAlgorithms>

#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "helpers/globals.h"
#include "qt/bridge/aida_dialog.hpp"
#include "qt/documents/aida_document_controller.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/explorer/exchange/open_dispatch.hpp"
#include "qt/editor/aida_code_document.hpp"
#include "qt/editor/aida_code_editor.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"

namespace aida::qt::editor {

AidaCodeGroupHost::AidaCodeGroupHost(std::uint32_t group_id,
    documents::AidaDocumentController* controller, AidaCodeDocumentRegistry* registry,
    QWidget* parent)
    : QWidget(parent), group_id_(group_id), controller_(controller), registry_(registry)
{
    model_ = controller_ ? controller_->model() : nullptr;
    setObjectName(QStringLiteral("aida.document.code.group.%1").arg(group_id_));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tab_bar_ = new QTabBar(this);
    tab_bar_->setObjectName(objectName() + QStringLiteral(".tabs"));
    tab_bar_->setMovable(true);
    tab_bar_->setTabsClosable(true);
    tab_bar_->setDocumentMode(true);
    tab_bar_->setExpanding(false);
    tab_bar_->setUsesScrollButtons(true);
    tab_bar_->setElideMode(Qt::ElideMiddle);
    layout->addWidget(tab_bar_);

    banner_host_ = new QWidget(this);
    banner_host_->setObjectName(objectName() + QStringLiteral(".banners"));
    auto* banner_layout = new QVBoxLayout(banner_host_);
    banner_layout->setContentsMargins(0, 0, 0, 0);
    banner_layout->setSpacing(0);
    banner_host_->setVisible(false);
    layout->addWidget(banner_host_);

    auto* stack_holder = new QWidget(this);
    stack_ = new QStackedLayout(stack_holder);
    stack_->setStackingMode(QStackedLayout::StackOne);
    layout->addWidget(stack_holder, 1);

    connect(tab_bar_, &QTabBar::currentChanged, this, &AidaCodeGroupHost::onCurrentChanged);
    connect(tab_bar_, &QTabBar::tabCloseRequested, this, &AidaCodeGroupHost::onTabCloseRequested);
    connect(tab_bar_, &QTabBar::tabMoved, this, &AidaCodeGroupHost::onTabMoved);
    tab_bar_->installEventFilter(this);

    if (model_) {
        connect(model_, &documents::AidaDocumentModel::documentAdded, this,
                &AidaCodeGroupHost::onDocumentAdded);
        connect(model_, &documents::AidaDocumentModel::documentRemoved, this,
                &AidaCodeGroupHost::onDocumentRemoved);
        connect(model_, &documents::AidaDocumentModel::documentChanged, this,
                &AidaCodeGroupHost::onDocumentChanged);
        connect(model_, &documents::AidaDocumentModel::structureChanged, this,
                &AidaCodeGroupHost::onStructureChanged);
    }
    if (controller_) {
        connect(controller_, &documents::AidaDocumentController::closeConfirmationRequested,
                this, &AidaCodeGroupHost::onCloseConfirmation);
        connect(controller_, &documents::AidaDocumentController::exitReviewStateChanged,
                this, &AidaCodeGroupHost::onExitReviewStateChanged);
    }
    rebuildTabs();
}

AidaCodeGroupHost::~AidaCodeGroupHost() = default;

quint64 AidaCodeGroupHost::tabDocumentId(int index) const
{
    if (index < 0 || index >= tab_bar_->count())
        return 0;
    return tab_bar_->tabData(index).toULongLong();
}

int AidaCodeGroupHost::tabIndexForDocument(quint64 document_id) const
{
    for (int index = 0; index < tab_bar_->count(); ++index)
        if (tab_bar_->tabData(index).toULongLong() == document_id)
            return index;
    return -1;
}

QString AidaCodeGroupHost::tabLabel(const OpenTab& tab) const
{
    std::string label = tab.filename.empty() ? "Untitled" : tab.filename;
    if (tab.dirty)
        label.append(" *");
    if (tab.pinned)
        label.append(" [Pinned]");
    if (tab.external_conflict)
        label.append(" [Disk conflict]");
    if (tab.proposal_pending)
        label.append(" [Review]");
    return QString::fromStdString(label);
}

void AidaCodeGroupHost::rebuildTabs()
{
    if (rebuilding_ || !model_)
        return;
    rebuilding_ = true;
    const quint64 active_id = model_->activeDocumentInGroup(group_id_);
    tab_bar_->blockSignals(true);
    while (tab_bar_->count() > 0)
        tab_bar_->removeTab(0);
    for (int index = 0; index < model_->recordCount(); ++index) {
        const OpenTab* tab = model_->recordAt(index);
        if (!tab || tab->group_id != group_id_)
            continue;
        const int tab_index = tab_bar_->addTab(tabLabel(*tab));
        tab_bar_->setTabData(tab_index, QVariant::fromValue(tab->document_id));
        tab_bar_->setTabToolTip(tab_index, QString::fromStdString(tab->filepath));
        ensureEditor(tab->document_id);
    }
    tab_bar_->blockSignals(false);
    const int active_index = tabIndexForDocument(active_id);
    if (active_index >= 0)
        tab_bar_->setCurrentIndex(active_index);
    else if (tab_bar_->count() > 0)
        tab_bar_->setCurrentIndex(0);
    rebuilding_ = false;
    onCurrentChanged(tab_bar_->currentIndex());
}

void AidaCodeGroupHost::ensureEditor(quint64 document_id)
{
    if (!registry_ || document_id == 0)
        return;
    for (int index = 0; index < stack_->count(); ++index) {
        auto* editor = qobject_cast<AidaCodeEditor*>(stack_->widget(index));
        if (editor && editor->documentId() == document_id)
            return;
    }
    auto* editor = new AidaCodeEditor(registry_, document_id, this);
    editor->setObjectName(objectName() + QStringLiteral(".editor.") +
        QString::number(document_id));
    connect(editor, &AidaCodeEditor::focusGained, this, [this](quint64 document_id) {
        if (!controller_)
            return;
        const int index = tabIndexForDocument(document_id);
        if (index >= 0 && index != tab_bar_->currentIndex())
            tab_bar_->setCurrentIndex(index);
        controller_->switchTo(document_id);
    });
    connect(editor, &AidaCodeEditor::fileDropRequested, this, [](const QString& path) {
        explorer::open_path(path.toStdString());
    });
    stack_->addWidget(editor);
}

AidaCodeEditor* AidaCodeGroupHost::editorFor(quint64 document_id) const
{
    for (int index = 0; index < stack_->count(); ++index) {
        auto* editor = qobject_cast<AidaCodeEditor*>(stack_->widget(index));
        if (editor && editor->documentId() == document_id)
            return editor;
    }
    return nullptr;
}

AidaCodeEditor* AidaCodeGroupHost::currentEditor() const
{
    return qobject_cast<AidaCodeEditor*>(stack_->currentWidget());
}

void AidaCodeGroupHost::onCurrentChanged(int index)
{
    if (rebuilding_ || !controller_)
        return;
    const quint64 document_id = tabDocumentId(index);
    if (document_id == 0)
        return;
    ensureEditor(document_id);
    AidaCodeEditor* editor = editorFor(document_id);
    if (editor)
        stack_->setCurrentWidget(editor);
    controller_->switchTo(document_id);
    refreshBanners();
}

void AidaCodeGroupHost::onTabCloseRequested(int index)
{
    const quint64 document_id = tabDocumentId(index);
    if (document_id == 0 || !controller_)
        return;
    controller_->requestTabClose(document_id);
}

void AidaCodeGroupHost::onTabMoved(int from, int to)
{
    if (!controller_)
        return;
    const quint64 source = tabDocumentId(from);
    const quint64 target = tabDocumentId(to);
    if (source == 0 || target == 0)
        return;
    controller_->reorderDocument(source, target, to > from);
}

void AidaCodeGroupHost::onDocumentAdded(quint64 document_id)
{
    if (!model_)
        return;
    const int index = model_->findDocument(document_id);
    if (!model_->isValidIndex(index))
        return;
    const OpenTab* tab = model_->recordAt(index);
    if (!tab || tab->group_id != group_id_)
        return;
    rebuildTabs();
}

void AidaCodeGroupHost::onDocumentRemoved(quint64 document_id)
{
    AidaCodeEditor* editor = editorFor(document_id);
    if (editor) {
        stack_->removeWidget(editor);
        editor->deleteLater();
    }
    const int index = tabIndexForDocument(document_id);
    if (index >= 0)
        tab_bar_->removeTab(index);
    refreshBanners();
}

void AidaCodeGroupHost::onDocumentChanged(quint64 document_id,
    documents::document_change_flags_t flags)
{
    const int index = tabIndexForDocument(document_id);
    if (index < 0 || !model_)
        return;
    const OpenTab* tab = model_->recordAt(model_->findDocument(document_id));
    if (!tab)
        return;
    const auto flags_int = static_cast<quint32>(flags);
    const auto label_mask = static_cast<quint32>(
        documents::document_change_t::dirty | documents::document_change_t::content |
        documents::document_change_t::proposal | documents::document_change_t::external_conflict |
        documents::document_change_t::pinned);
    if (flags_int & label_mask)
        tab_bar_->setTabText(index, tabLabel(*tab));
    refreshBanners();
}

void AidaCodeGroupHost::onStructureChanged()
{
    rebuildTabs();
}

void AidaCodeGroupHost::onExitReviewStateChanged()
{
    refreshBanners();
}

bool AidaCodeGroupHost::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == tab_bar_ && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::RightButton) {
            const int index = tab_bar_->tabAt(mouse->position().toPoint());
            if (index >= 0)
                openTabContextMenu(index, mapToGlobal(mouse->position().toPoint()));
            return false;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AidaCodeGroupHost::openTabContextMenu(int tab_index, const QPoint& global_pos)
{
    if (!model_)
        return;
    const quint64 document_id = tabDocumentId(tab_index);
    const int document_index = model_->findDocument(document_id);
    if (!model_->isValidIndex(document_index))
        return;
    aida::ui::application_ui::open_editor_tab_context_menu(document_index,
        aida::ui::context_menu_open_origin_t::pointer);
    documents::show_context_menu(aida::ui::stable_menu_id_t("menu.editor.tab"),
        documents::make_menu_snapshot(aida::ui::stable_view_id_t("document.code"),
            aida::ui::stable_context_type_id_t("context.editor.tab")),
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

namespace {

QWidget* make_banner_row(QWidget* parent, const QString& text, const QString& variant)
{
    auto* row = new QFrame(parent);
    row->setObjectName(parent->objectName() + QStringLiteral(".row"));
    row->setProperty("aidaRole", QStringLiteral("notice"));
    row->setProperty("aidaVariant", variant);
    const auto& spacing = aida::qt::theme::tokens().spacing;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(spacing.md, spacing.xs, spacing.md, spacing.xs);
    layout->setSpacing(spacing.xs);
    auto* label = new QLabel(text, row);
    label->setWordWrap(true);
    layout->addWidget(label, 1);
    return row;
}

widgets::AidaButton* add_banner_button(QWidget* row, const QString& label)
{
    auto* button = new widgets::AidaButton(label, row);
    button->setKind(widgets::AidaButton::Kind::Ghost);
    button->setControlSize(widgets::AidaButton::ControlSize::Small);
    row->layout()->addWidget(button);
    return button;
}

}

void AidaCodeGroupHost::refreshBanners()
{
    if (!model_ || !controller_)
        return;
    qDeleteAll(banner_host_->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly));
    const int active_index = model_->activeInGroup(group_id_);
    if (!model_->isValidIndex(active_index)) {
        banner_host_->setVisible(false);
        return;
    }
    const OpenTab* tab = model_->recordAt(active_index);
    if (!tab) {
        banner_host_->setVisible(false);
        return;
    }
    const quint64 document_id = tab->document_id;
    const int document_index = model_->findDocument(document_id);
    bool any_banner = false;

    if (!tab->buffer_loaded) {
        QWidget* row = nullptr;
        if (tab->load_in_progress) {
            row = make_banner_row(banner_host_,
                QStringLiteral("Loading %1 asynchronously... The editor remains responsive while this bounded file read completes.")
                    .arg(QString::fromStdString(tab->filename)),
                QStringLiteral("info"));
            auto* cancel = add_banner_button(row, QStringLiteral("Cancel Load"));
            connect(cancel, &QAbstractButton::clicked, this, [this, document_index] {
                aida::ui::application_ui::execute_editor_tab_action(document_index,
                    "tab.load.cancel", aida::ui::action_invocation_source_t::toolbar);
            });
        } else {
            row = make_banner_row(banner_host_,
                tab->load_error.empty()
                    ? QStringLiteral("Document could not be loaded. The file is unavailable or the load did not start.")
                    : QString::fromStdString(tab->load_error),
                QStringLiteral("error"));
            auto* retry = add_banner_button(row, QStringLiteral("Retry"));
            connect(retry, &QAbstractButton::clicked, this, [this, document_index] {
                aida::ui::application_ui::execute_editor_tab_action(document_index,
                    "tab.load.retry", aida::ui::action_invocation_source_t::toolbar);
            });
            auto* close = add_banner_button(row, QStringLiteral("Close"));
            connect(close, &QAbstractButton::clicked, this, [this, document_index] {
                aida::ui::application_ui::execute_editor_tab_action(document_index,
                    "tab.close", aida::ui::action_invocation_source_t::toolbar);
            });
        }
        banner_host_->layout()->addWidget(row);
        any_banner = true;
    }

    if (tab->save_in_progress || !tab->save_error.empty()) {
        QWidget* row = make_banner_row(banner_host_, tab->save_in_progress
            ? QStringLiteral("Saving %1 in Task Center...").arg(QString::fromStdString(tab->filename))
            : QStringLiteral("Save: %1").arg(QString::fromStdString(tab->save_error)),
            tab->save_in_progress ? QStringLiteral("info") : QStringLiteral("error"));
        banner_host_->layout()->addWidget(row);
        any_banner = true;
    }

    if (tab->recovery_operation_pending) {
        QWidget* row = make_banner_row(banner_host_,
            QStringLiteral("%1...").arg(tab->recovery_operation_label.empty()
                ? QStringLiteral("Working with recovery storage")
                : QString::fromStdString(tab->recovery_operation_label)),
            QStringLiteral("info"));
        banner_host_->layout()->addWidget(row);
        any_banner = true;
    }

    if (!tab->recovery.available && tab->recovery_probe_completed &&
        !tab->recovery_error.empty()) {
        QWidget* row = make_banner_row(banner_host_,
            QStringLiteral("Recovery storage: %1").arg(QString::fromStdString(tab->recovery_error)),
            QStringLiteral("warning"));
        auto* retry = add_banner_button(row, QStringLiteral("Retry Recovery Check"));
        connect(retry, &QAbstractButton::clicked, this, [this, document_index] {
            aida::ui::application_ui::execute_editor_tab_action(document_index,
                "tab.recovery.retry_probe", aida::ui::action_invocation_source_t::toolbar);
        });
        banner_host_->layout()->addWidget(row);
        any_banner = true;
    }

    if (tab->recovery.available) {
        QWidget* row = make_banner_row(banner_host_,
            QStringLiteral("A verified unsaved recovery journal is available.  rev %1 | %2 bytes | %3 / %4 / %5")
                .arg(static_cast<qulonglong>(tab->recovery.metadata.revision))
                .arg(static_cast<qulonglong>(tab->recovery.metadata.byte_length))
                .arg(QString::fromStdString(tab->recovery.metadata.text.encoding))
                .arg(QString::fromStdString(tab->recovery.metadata.text.bom))
                .arg(QString::fromStdString(tab->recovery.metadata.text.eol)),
            QStringLiteral("accent"));
        auto* recover = add_banner_button(row, QStringLiteral("Recover Unsaved Content"));
        connect(recover, &QAbstractButton::clicked, this, [this, document_index] {
            aida::ui::application_ui::execute_editor_tab_action(document_index,
                "tab.recovery.recover", aida::ui::action_invocation_source_t::toolbar);
        });
        auto* compare = add_banner_button(row, QStringLiteral("Compare"));
        connect(compare, &QAbstractButton::clicked, this, [this, document_index] {
            aida::ui::application_ui::execute_editor_tab_action(document_index,
                "tab.recovery.compare", aida::ui::action_invocation_source_t::toolbar);
        });
        auto* discard = add_banner_button(row, QStringLiteral("Discard Recovery"));
        connect(discard, &QAbstractButton::clicked, this, [this, document_index] {
            aida::ui::application_ui::execute_editor_tab_action(document_index,
                "tab.recovery.discard", aida::ui::action_invocation_source_t::toolbar);
        });
        banner_host_->layout()->addWidget(row);
        any_banner = true;
    }

    if (controller_->pendingRecoveryDiscardDocument() == document_id)
        showRecoveryDiscardReview(document_id);

    if (tab->external_conflict) {
        QWidget* row = make_banner_row(banner_host_,
            QStringLiteral("The file changed on disk. AiDA will not overwrite it without your decision."),
            QStringLiteral("warning"));
        auto* compare = add_banner_button(row, QStringLiteral("Compare"));
        connect(compare, &QAbstractButton::clicked, this, [this, document_index] {
            aida::ui::application_ui::execute_editor_tab_action(document_index,
                "tab.compare_disk", aida::ui::action_invocation_source_t::toolbar);
        });
        auto* reload = add_banner_button(row, QStringLiteral("Reload from Disk"));
        connect(reload, &QAbstractButton::clicked, this, [this, document_index] {
            aida::ui::application_ui::execute_editor_tab_action(document_index,
                "tab.external.reload", aida::ui::action_invocation_source_t::toolbar);
        });
        auto* keep = add_banner_button(row, QStringLiteral("Keep Editor Version"));
        connect(keep, &QAbstractButton::clicked, this, [this, document_index] {
            aida::ui::application_ui::execute_editor_tab_action(document_index,
                "tab.external.keep_editor", aida::ui::action_invocation_source_t::toolbar);
        });
        banner_host_->layout()->addWidget(row);
        any_banner = true;
    }

    banner_host_->setVisible(any_banner);
}

void AidaCodeGroupHost::showCloseReview(quint64 document_id)
{
    if (!model_ || !controller_)
        return;
    const int index = model_->findDocument(document_id);
    if (!model_->isValidIndex(index))
        return;
    const OpenTab* tab = model_->recordAt(index);
    if (!tab)
        return;
    if (auto* existing = findChild<bridge::AidaDialog*>(QStringLiteral("aida.editor.close_review"))) {
        if (existing->property("aida.document_id").toULongLong() == document_id) {
            existing->raise();
            existing->activateWindow();
            return;
        }
        existing->setProperty("aida.superseded", true);
        existing->close();
    }
    const QString filename = tab->filename.empty()
        ? QStringLiteral("Untitled") : QString::fromStdString(tab->filename);

    auto* dialog = new bridge::AidaDialog(this);
    dialog->setWindowTitle(QStringLiteral("Unsaved document"));
    dialog->setObjectName(QStringLiteral("aida.editor.close_review"));
    dialog->setProperty("aida.document_id", QVariant::fromValue(document_id));
    dialog->setModal(true);
    auto* layout = new QVBoxLayout(dialog);
    auto* label = new QLabel(
        QStringLiteral("%1 has unsaved changes. Save before closing?").arg(filename), dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* buttons = new QHBoxLayout();
    auto* save = new widgets::AidaButton(QStringLiteral("Save"), dialog);
    save->setKind(widgets::AidaButton::Kind::Primary);
    auto* discard = new widgets::AidaButton(QStringLiteral("Discard"), dialog);
    discard->setKind(widgets::AidaButton::Kind::Destructive);
    auto* cancel = new widgets::AidaButton(QStringLiteral("Cancel"), dialog);
    buttons->addStretch(1);
    buttons->addWidget(save);
    buttons->addWidget(discard);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    connect(save, &QAbstractButton::clicked, this, [this, document_id, dialog] {
        const auto result = controller_->saveAndClose(document_id);
        dialog->accept();
        if (!result.succeeded) {
            controller_->clearPendingClose();
        }
    });
    connect(discard, &QAbstractButton::clicked, this, [this, document_id, dialog] {
        dialog->accept();
        controller_->closeDocument(document_id, true);
    });
    connect(cancel, &QAbstractButton::clicked, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [this, dialog](int) {
        if (!dialog->property("aida.superseded").toBool())
            controller_->clearPendingClose();
    });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void AidaCodeGroupHost::onCloseConfirmation(quint64 document_id)
{
    if (!model_)
        return;
    const int index = model_->findDocument(document_id);
    if (!model_->isValidIndex(index))
        return;
    const OpenTab* tab = model_->recordAt(index);
    if (!tab || tab->group_id != group_id_)
        return;
    showCloseReview(document_id);
}

void AidaCodeGroupHost::showRecoveryDiscardReview(quint64 document_id)
{
    if (!model_ || !controller_)
        return;
    const int index = model_->findDocument(document_id);
    if (!model_->isValidIndex(index))
        return;
    const OpenTab* tab = model_->recordAt(index);
    if (!tab)
        return;
    const QString target = tab->filename.empty()
        ? QStringLiteral("Untitled document") : QString::fromStdString(tab->filename);

    auto* dialog = new bridge::AidaDialog(this);
    dialog->setWindowTitle(QStringLiteral("Discard Recovery"));
    dialog->setObjectName(QStringLiteral("aida.editor.recovery.discard"));
    dialog->setModal(true);
    auto* layout = new QVBoxLayout(dialog);
    auto* title = new QLabel(QStringLiteral("Discard recovery for %1?").arg(target), dialog);
    layout->addWidget(title);
    auto* detail = new QLabel(QStringLiteral(
        "Scope: The current and retained last-good recovery journals for this document.\n"
        "Effect: Permanently removes the verified unsaved recovery content; the open editor buffer and disk file are unchanged.\n"
        "Recovery: The discarded recovery journals cannot be reconstructed after confirmation."), dialog);
    detail->setWordWrap(true);
    layout->addWidget(detail);
    auto* buttons = new QHBoxLayout();
    auto* confirm = new widgets::AidaButton(QStringLiteral("Permanently Discard Recovery"), dialog);
    confirm->setKind(widgets::AidaButton::Kind::Destructive);
    auto* cancel = new widgets::AidaButton(QStringLiteral("Cancel"), dialog);
    buttons->addStretch(1);
    buttons->addWidget(confirm);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    connect(confirm, &QAbstractButton::clicked, this, [this, document_id, dialog] {
        const auto result = controller_->discardRecovery(document_id);
        if (result.succeeded)
            controller_->clearPendingRecoveryDiscard();
        dialog->accept();
    });
    connect(cancel, &QAbstractButton::clicked, this, [this, dialog] {
        controller_->clearPendingRecoveryDiscard();
        dialog->reject();
    });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

}
