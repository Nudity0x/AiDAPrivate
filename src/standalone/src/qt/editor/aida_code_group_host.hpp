#pragma once

#include <QWidget>

#include <cstdint>
#include <functional>
#include <string>

#include "qt/documents/aida_document_model.hpp"

class QStackedLayout;
class QTabBar;

struct OpenTab;

namespace aida::qt::editor {

class AidaCodeDocumentRegistry;
class AidaCodeEditor;

}

namespace aida::qt::documents {

class AidaDocumentController;

}

namespace aida::qt::editor {

class AidaCodeGroupHost : public QWidget {
    Q_OBJECT
public:
    AidaCodeGroupHost(std::uint32_t group_id,
                      documents::AidaDocumentController* controller,
                      AidaCodeDocumentRegistry* registry, QWidget* parent = nullptr);
    ~AidaCodeGroupHost() override;

    std::uint32_t groupId() const noexcept { return group_id_; }
    AidaCodeEditor* editorFor(quint64 document_id) const;
    AidaCodeEditor* currentEditor() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
    void onCurrentChanged(int index);
    void onTabCloseRequested(int index);
    void onTabMoved(int from, int to);
    void onDocumentAdded(quint64 document_id);
    void onDocumentRemoved(quint64 document_id);
    void onDocumentChanged(quint64 document_id,
        aida::qt::documents::document_change_flags_t flags);
    void onStructureChanged();
    void onCloseConfirmation(quint64 document_id);
    void onExitReviewStateChanged();

private:
    void rebuildTabs();
    void refreshBanners();
    void ensureEditor(quint64 document_id);
    void showCloseReview(quint64 document_id);
    void showRecoveryDiscardReview(quint64 document_id);
    void openTabContextMenu(int tab_index, const QPoint& global_pos);
    quint64 tabDocumentId(int index) const;
    int tabIndexForDocument(quint64 document_id) const;
    QString tabLabel(const OpenTab& tab) const;

    std::uint32_t group_id_ = 0;
    documents::AidaDocumentController* controller_ = nullptr;
    documents::AidaDocumentModel* model_ = nullptr;
    AidaCodeDocumentRegistry* registry_ = nullptr;
    QTabBar* tab_bar_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    QWidget* banner_host_ = nullptr;
    bool rebuilding_ = false;
};

}
