#pragma once

#include <QHash>
#include <QObject>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTextObjectInterface>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/ai/standalone_chat.hpp"
#include "core/ui/chat_markdown.hpp"

class QElapsedTimer;
class QPlainTextEdit;

namespace aida::qt::ai {

class AidaChatMessageModel;

constexpr int kChatToolCardObjectType = QTextFormat::UserObject + 7;
constexpr int kToolCardNameProperty = QTextFormat::UserProperty + 101;
constexpr int kToolCardKindProperty = QTextFormat::UserProperty + 102;
constexpr int kToolCardPayloadProperty = QTextFormat::UserProperty + 103;
constexpr int kToolCardOrdinalProperty = QTextFormat::UserProperty + 104;
constexpr int kToolCardExpandedProperty = QTextFormat::UserProperty + 105;

class AidaToolCardTextObject : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    explicit AidaToolCardTextObject(QObject* parent = nullptr);

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument,
                         const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                    int posInDocument, const QTextFormat& format) override;

    static constexpr int k_collapsed_line_cap = 10;
};

class AidaMarkdownDocumentBuilder {
public:
    static std::unique_ptr<QTextDocument> build(
        const std::vector<chat_render::span_t>& spans, std::size_t revealed_len,
        QObject* handler_parent, AidaToolCardTextObject* handler,
        const std::vector<bool>* expanded_cards);
};

class AidaChatDocumentCache {
public:
    struct entry_t {
        std::uint64_t text_fnv = 0;
        std::size_t text_size = 0;
        std::vector<chat_render::span_t> spans;
        std::unique_ptr<QTextDocument> document;
        std::size_t revealed_len = 0;
        quint64 palette_revision = 0;
        std::uint64_t use_tick = 0;
        std::vector<bool> expanded_cards;
        bool parsed = false;
    };

    const entry_t& entryFor(std::size_t index, const std::string& text);
    QTextDocument* documentFor(std::size_t index, std::size_t revealed_len,
                               qreal text_width, AidaToolCardTextObject* handler);
    QTextDocument* plainDocumentFor(std::size_t index, const std::string& text,
                                    qreal text_width);
    void setCardExpanded(std::size_t index, int ordinal, bool expanded);
    void invalidateDocument(std::size_t index);
    void clear();

    std::uint64_t hits() const noexcept { return hits_; }
    std::uint64_t misses() const noexcept { return misses_; }
    std::uint64_t rebuilds() const noexcept { return rebuilds_; }

    static constexpr std::size_t k_max_entries = 512;

private:
    entry_t& touch(std::size_t index);
    void evictIfNeeded();

    std::unordered_map<std::size_t, entry_t> entries_;
    std::uint64_t tick_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t rebuilds_ = 0;
};

class AidaChatMessageDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    AidaChatMessageDelegate(AidaChatMessageModel* model, AidaChatDocumentCache* cache,
                            QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const override;

    bool thinkingExpanded(std::uint64_t fingerprint) const;
    void setThinkingExpanded(std::uint64_t fingerprint, bool expanded);

    std::uint64_t revealedFor(std::size_t absolute_index) const;
    void setRevealed(std::size_t absolute_index, std::uint64_t revealed);
    bool advanceStreamingReveal();

    void invalidateHeights();

Q_SIGNALS:
    void messageActionRequested(const aida::automation_ui::message_identity_t& identity,
                                aida::automation_ui::message_action_t action);
    void linkActivated(const QString& url);
    void contextMenuRequested(const QModelIndex& index, const QPoint& globalPos);

protected:
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view,
                   const QStyleOptionViewItem& option, const QModelIndex& index) override;

private:
    struct row_metrics_t {
        int role_h = 0;
        int body_h = 0;
        int thinking_h = 0;
        int total_h = 0;
        int width = -1;
        std::uint64_t doc_key = 0;
    };

    QRect cardRect(const QRect& row) const;
    QRect rolePillRect(const QRect& card, const QFontMetricsF& fm,
                       const QString& label) const;
    QRect thinkingHeaderRect(const QRect& card, int role_h) const;
    QRect bodyRect(const QRect& card, int role_h, int thinking_h) const;
    QVector<QRect> actionRects(const QRect& card) const;
    row_metrics_t measure(const QStyleOptionViewItem& option,
                          const QModelIndex& index) const;
    void paintThinkingDots(QPainter* painter, const QRect& rect) const;

    AidaChatMessageModel* model_ = nullptr;
    AidaChatDocumentCache* cache_ = nullptr;
    AidaToolCardTextObject* card_handler_ = nullptr;
    mutable QHash<quint64, bool> thinking_expanded_;
    mutable QHash<quint64, std::uint64_t> revealed_;
    mutable QElapsedTimer* phase_timer_ = nullptr;
    mutable QHash<quint64, row_metrics_t> height_cache_;
};

}
