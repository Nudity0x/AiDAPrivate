#pragma once

#include <QObject>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QTimer;

namespace aida::qt::documents {

class AidaDocumentModel;

struct document_op_result_t {
    bool succeeded = false;
    std::string detail;
};

class AidaDocumentController : public QObject {
    Q_OBJECT
public:
    explicit AidaDocumentController(AidaDocumentModel* model, QObject* parent = nullptr);
    ~AidaDocumentController() override;

    AidaDocumentModel* model() const noexcept { return model_; }

    bool openDocument(const std::string& path, const std::string& name = {},
                    int caret_line = -1, int caret_column = -1);
    void openOrFocusContent(const std::string& path, const std::string& name,
                            const std::string& content);
    bool switchTo(quint64 document_id, bool record_history = true);
    void closeDocument(quint64 document_id, bool confirmed_discard = false);
    void requestTabClose(quint64 document_id);
    std::uint32_t createGroupForTab(quint64 document_id);
    bool moveToGroup(quint64 document_id, std::uint32_t group_id);
    bool navigateGroupHistory(std::uint32_t group_id, bool forward);
    bool reopenClosedDocument();
    bool canReopenClosedDocument() const;
    void reorderDocument(quint64 source_document, quint64 target_document, bool insert_after);

    document_op_result_t saveDocument(quint64 document_id);
    document_op_result_t saveDocumentAs(quint64 document_id, const std::string& destination);
    document_op_result_t saveAndClose(quint64 document_id);
    document_op_result_t saveAll();
    document_op_result_t verifySaveGate(quint64 document_id, bool require_destination);

    bool reloadExternal(quint64 document_id);
    bool keepEditorVersion(quint64 document_id);
    bool stageExternalProposal(const std::string& path, const std::string& proposed_content,
                               const std::string& origin, std::string& detail);
    void acceptExternalWrite(const std::string& path, const std::string& content);

    void requestRecoveryProbe(quint64 document_id);
    document_op_result_t recoverFromJournal(quint64 document_id);
    document_op_result_t compareWithJournal(quint64 document_id);
    document_op_result_t compareWithDisk(quint64 document_id);
    document_op_result_t discardRecovery(quint64 document_id);
    document_op_result_t requestRecoveryDiscard(quint64 document_id);
    quint64 pendingRecoveryDiscardDocument() const;
    void clearPendingRecoveryDiscard();

    bool cancelDocumentLoad(quint64 document_id);

    std::size_t requestCloseAll();
    void advanceCloseAll();
    void resolvePendingCloseAfterSave();
    void cancelCloseAll();

    document_op_result_t requestExitReview();
    bool exitReviewRequested() const;
    bool exitReviewReady() const;
    bool exitReviewCommitted() const;
    void pollExitReview();
    bool consumeExitReviewReady();
    void resolveExitReviewDocument(quint64 document_id, quint64 revision,
                                   bool confirmed_discard);
    document_op_result_t beginExitDiscardCleanup(quint64 document_id, quint64 revision);
    void failExitDiscardCleanup(quint64 document_id, std::string detail);

    document_op_result_t restoreSession(const std::string& serialized,
                                        int legacy_active_index = -1);
    std::string serializeSession();

    qint64 pendingCloseDocumentId() const;
    bool showCloseConfirm() const;
    void clearPendingClose();
    std::string closeConfirmError() const;
    void finishCloseAllDocument(quint64 document_id);

    std::string labelForGroup(std::uint32_t group_id) const;
    std::vector<std::uint32_t> currentGroups() const;
    std::uint32_t activeGroup() const;
    bool prepareGroupClose(std::uint32_t group_id, std::string& reason);
    void closeGroup(std::uint32_t group_id);

    void writeHotExitSnapshots();

Q_SIGNALS:
    void closeConfirmationRequested(quint64 document_id);
    void closeConfirmStateChanged();
    void exitReviewStateChanged();
    void documentSaved(quint64 document_id);
    void documentLoadFailed(quint64 document_id);

private Q_SLOTS:
    void onExternalPollTimer();
    void onDispatchSweepTimer();
    void onRecoveryCheckpointTimer();
    void onExitReviewTimer();

private:
    void hookHotExit();

    AidaDocumentModel* model_ = nullptr;
    QTimer* external_poll_timer_ = nullptr;
    QTimer* dispatch_sweep_timer_ = nullptr;
    QTimer* recovery_checkpoint_timer_ = nullptr;
    QTimer* exit_review_timer_ = nullptr;
    bool hot_exit_hooked_ = false;
};

}
