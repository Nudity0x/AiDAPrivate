#pragma once

#include <QHash>
#include <QObject>
#include <QSet>

#include <cstdint>
#include <string>
#include <vector>

class QTimer;
struct OpenTab;

namespace aida::qt::documents {

enum class document_change_t : quint32 {
    none = 0,
    dirty = 1u << 0,
    caret = 1u << 1,
    scroll = 1u << 2,
    folds = 1u << 3,
    recovery = 1u << 4,
    load = 1u << 5,
    save = 1u << 6,
    external_conflict = 1u << 7,
    proposal = 1u << 8,
    content = 1u << 9,
    language = 1u << 10,
    selection = 1u << 11,
    pinned = 1u << 12
};
Q_DECLARE_FLAGS(document_change_flags_t, document_change_t)
Q_DECLARE_OPERATORS_FOR_FLAGS(document_change_flags_t)

class AidaDocumentModel : public QObject {
    Q_OBJECT
public:
    explicit AidaDocumentModel(QObject* parent = nullptr);
    ~AidaDocumentModel() override;

    int recordCount() const;
    const OpenTab* recordAt(int index) const;
    OpenTab* recordAt(int index);
    int findDocument(quint64 document_id) const;
    int findPathDocument(const std::string& path) const;
    bool isValidIndex(int index) const;
    int activeDocument() const;
    quint64 activeDocumentId() const;
    std::vector<std::uint32_t> groups() const;
    int activeInGroup(std::uint32_t group_id);
    quint64 activeDocumentInGroup(std::uint32_t group_id);
    std::size_t groupDocumentCount(std::uint32_t group_id) const;
    bool closeReviewInProgress() const;

    void syncFromBackend();
    void updateFromEditor(quint64 document_id);
    void notifyChanged(quint64 document_id, document_change_flags_t flags);
    void notifyStructureChanged();
    void notifyGroupMembershipChanged(quint64 document_id, std::uint32_t old_group,
                                      std::uint32_t new_group);

Q_SIGNALS:
    void documentAdded(quint64 document_id);
    void documentAboutToBeRemoved(quint64 document_id);
    void documentRemoved(quint64 document_id);
    void documentChanged(quint64 document_id,
                         aida::qt::documents::document_change_flags_t flags);
    void activeDocumentChanged(quint64 document_id, quint32 group_id);
    void groupMembershipChanged(quint64 document_id, quint32 old_group, quint32 new_group);
    void structureChanged();

private:
    struct record_snapshot_t {
        quint64 revision = 0;
        quint64 content_hash = 0;
        bool dirty = false;
        int caret_line = 0;
        int caret_column = 0;
        int anchor_line = 0;
        int anchor_column = 0;
        bool selection_active = false;
        float scroll_x = 0.f;
        float scroll_y = 0.f;
        std::size_t folds_signature = 0;
        bool proposal_pending = false;
        bool load_in_progress = false;
        bool load_failed = false;
        bool save_in_progress = false;
        bool buffer_loaded = false;
        bool external_conflict = false;
        bool recovery_available = false;
        bool recovery_operation_pending = false;
        bool recovery_checkpoint_pending = false;
        bool pinned = false;
        std::string language_override;
        std::string save_error;
        std::string load_error;
        std::string recovery_error;
    };

    static record_snapshot_t snapshotOf(const OpenTab& tab);
    document_change_flags_t diffOf(quint64 document_id, const record_snapshot_t& before,
                                   const record_snapshot_t& after) const;

    QHash<quint64, record_snapshot_t> snapshots_;
    QSet<quint64> known_ids_;
    QHash<quint64, std::uint32_t> group_of_;
    quint64 last_active_document_ = 0;
    std::uint32_t last_active_group_ = 0;
    QSet<quint64> pending_caret_emit_;
    QTimer* caret_timer_ = nullptr;
};

}
