#include "qt/ai/qt_ai_chat_delegate.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QTextOption>
#include <QTextTable>
#include <QToolTip>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "helpers/diag_log.hpp"
#include "qt/ai/qt_ai_chat_model.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_theme_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace aida::qt::ai {

namespace {

std::uint64_t fnv1a64(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash != 0 ? hash : 1;
}

QColor token_color(syntax::token_type type) {
    const auto& t = theme::tokens();
    switch (type) {
    case syntax::token_type::keyword:        return t.syn_keyword;
    case syntax::token_type::type_name:      return t.syn_type;
    case syntax::token_type::string_lit:     return t.syn_string;
    case syntax::token_type::number:         return t.syn_number;
    case syntax::token_type::comment_line:   return t.syn_comment;
    case syntax::token_type::comment_block:  return t.syn_comment;
    case syntax::token_type::preprocessor:   return t.syn_preprocessor;
    case syntax::token_type::operator_sym:   return t.syn_operator;
    case syntax::token_type::function_call:  return t.syn_function;
    case syntax::token_type::identifier:     return t.syn_identifier;
    case syntax::token_type::punctuation:
        return widgets::with_alpha(t.syn_operator, 0.85);
    case syntax::token_type::decorator:      return t.syn_keyword;
    case syntax::token_type::boolean_lit:    return t.syn_number;
    case syntax::token_type::register_name:  return t.syn_register;
    case syntax::token_type::directive:      return t.syn_keyword;
    default:                                 return t.syn_identifier;
    }
}

struct span_payload_t {
    QString title;
    QString body;
};

span_payload_t decode_tool_payload(const std::string& raw, bool is_call) {
    span_payload_t out;
    const auto nl = raw.find('\n');
    const std::string first = nl == std::string::npos ? raw : raw.substr(0, nl);
    const std::string rest = nl == std::string::npos ? std::string() : raw.substr(nl + 1);
    if (is_call) {
        out.title = QString::fromStdString(first);
        out.body = QString::fromStdString(rest);
    } else {
        out.title = QStringLiteral("Tool result");
        out.body = QString::fromStdString(raw);
    }
    return out;
}

QStringList payload_lines(const QString& body) {
    return body.split(QLatin1Char('\n'));
}

struct hover_action_t {
    const char* label;
    automation_ui::message_action_t action;
};

constexpr hover_action_t k_hover_actions[] = {
    { "Copy", automation_ui::message_action_t::copy_text },
    { "Retry", automation_ui::message_action_t::retry_from_here },
    { "Edit", automation_ui::message_action_t::edit_message },
    { "Delete", automation_ui::message_action_t::delete_message },
};

QString compact_count(int value) {
    if (value >= 1000)
        return QStringLiteral("%1.%2k").arg(value / 1000)
            .arg((value % 1000) / 100);
    return QString::number(value);
}

}

AidaChatMessageDelegate::AidaChatMessageDelegate(AidaChatMessageModel* model,
                                                 AidaChatDocumentCache* cache,
                                                 QObject* parent)
    : QStyledItemDelegate(parent), model_(model), cache_(cache) {
    card_handler_ = new AidaToolCardTextObject(this);
}

bool AidaChatMessageDelegate::thinkingExpanded(std::uint64_t fingerprint) const {
    return thinking_expanded_.value(fingerprint, false);
}

void AidaChatMessageDelegate::setThinkingExpanded(std::uint64_t fingerprint, bool expanded) {
    thinking_expanded_.insert(fingerprint, expanded);
}

std::uint64_t AidaChatMessageDelegate::revealedFor(std::size_t absolute_index) const {
    return revealed_.value(absolute_index, 0);
}

void AidaChatMessageDelegate::setRevealed(std::size_t absolute_index, std::uint64_t revealed) {
    revealed_.insert(absolute_index, revealed);
}

void AidaChatMessageDelegate::invalidateHeights() {
    height_cache_.clear();
}

bool AidaChatMessageDelegate::advanceStreamingReveal() {
    const int rows = model_->rowCount();
    if (rows <= 0)
        return false;
    const int last_row = rows - 1;
    automation_ui::chat_message_snapshot_t snapshot;
    if (!model_->messageAt(last_row, snapshot))
        return false;
    if (!snapshot.streaming)
        return false;
    const auto absolute = model_->absoluteIndexForRow(last_row);
    const std::uint64_t target = snapshot.text.size();
    if (target == 0)
        return true;
    std::uint64_t current = revealed_.value(absolute, 0);
    if (current == 0)
        current = (std::min)(target, static_cast<std::uint64_t>(16));
    const std::uint64_t next = (std::min)(target,
        current + (std::max)(static_cast<std::uint64_t>(1), (target - current) / 4));
    if (next == current)
        return true;
    revealed_.insert(absolute, next);
    invalidateHeights();
    const QModelIndex index = model_->index(last_row);
    Q_EMIT sizeHintChanged(index);
    return true;
}

QRect AidaChatMessageDelegate::cardRect(const QRect& row) const {
    return row.adjusted(0, 0, 0, -theme::tokens().spacing.sm);
}

QRect AidaChatMessageDelegate::rolePillRect(const QRect& card, const QFontMetricsF& fm,
                                            const QString& label) const {
    const auto& t = theme::tokens();
    const int h = (std::max)(t.spacing.xl, qRound(fm.height()) + t.spacing.xs + t.spacing.xxs);
    const int w = qRound(fm.horizontalAdvance(label)) + t.spacing.md;
    return QRect(card.left() + t.spacing.sm, card.top() + t.spacing.sm, w, h);
}

QRect AidaChatMessageDelegate::thinkingHeaderRect(const QRect& card, int role_h) const {
    const auto& t = theme::tokens();
    return QRect(card.left() + t.spacing.sm,
                 card.top() + t.spacing.sm + role_h + t.spacing.sm,
                 card.width() - t.spacing.sm * 2, t.spacing.lg + t.spacing.xxs);
}

QRect AidaChatMessageDelegate::bodyRect(const QRect& card, int role_h, int thinking_h) const {
    const auto& t = theme::tokens();
    return QRect(card.left() + t.spacing.sm,
                 card.top() + t.spacing.sm + role_h + t.spacing.sm + thinking_h,
                 card.width() - t.spacing.sm * 2,
                 card.height() - t.spacing.sm * 2 - role_h - t.spacing.sm - thinking_h);
}

QVector<QRect> AidaChatMessageDelegate::actionRects(const QRect& card) const {
    const auto& t = theme::tokens();
    QVector<QRect> out;
    const QFontMetricsF fm(theme::fonts::caption());
    const int chip_h = t.spacing.lg + t.spacing.xxs;
    int right = card.right() - t.spacing.sm;
    for (int i = 3; i >= 0; --i) {
        const int w = qRound(fm.horizontalAdvance(
            QLatin1String(k_hover_actions[i].label))) + t.spacing.md + t.spacing.xxs;
        out.prepend(QRect(right - w, card.top() + t.spacing.xs, w, chip_h));
        right -= w + t.spacing.xs;
    }
    return out;
}

AidaChatMessageDelegate::row_metrics_t AidaChatMessageDelegate::measure(
    const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const auto absolute = model_->absoluteIndexForRow(index.row());
    const auto identity = model_->identityAt(index.row());
    automation_ui::chat_message_snapshot_t snapshot;
    if (!model_->messageAt(index.row(), snapshot))
        return {};

    const int card_width = (std::max)(option.rect.width() - 2, 40);
    const int body_width = (std::max)(card_width - theme::tokens().spacing.sm * 2, 32);
    std::uint64_t revealed = revealed_.value(absolute, 0);
    if (!snapshot.streaming || revealed == 0 || revealed > snapshot.text.size())
        revealed = snapshot.text.size();
    const quint64 palette = theme::AidaThemeController::instance().generation();
    const quint64 height_key = identity.fingerprint ^
        (static_cast<quint64>(body_width) << 1) ^
        (static_cast<quint64>(revealed) << 17) ^ (palette << 33) ^
        (thinkingExpanded(identity.fingerprint) ? 0x9E3779B97F4A7C15ULL : 0ULL);
    const auto cached = height_cache_.constFind(height_key);
    if (cached != height_cache_.constEnd() && cached->width == option.rect.width())
        return *cached;

    row_metrics_t metrics;
    const QString role_label = snapshot.is_user ? QStringLiteral("YOU")
        : !snapshot.tool_name.empty() ? QStringLiteral("TOOL") : QStringLiteral("AIDA");
    const auto& t_spacing = theme::tokens().spacing;
    const QFontMetricsF fm(theme::fonts::caption());
    metrics.role_h = (std::max)(t_spacing.xl,
        qRound(fm.height()) + t_spacing.xs + t_spacing.xxs);

    const bool show_thinking = !snapshot.is_user && snapshot.has_thinking &&
        !snapshot.thinking_text.empty();
    if (show_thinking) {
        const int thinking_header_h = t_spacing.lg + t_spacing.xxs;
        metrics.thinking_h = thinking_header_h;
        if (thinkingExpanded(identity.fingerprint)) {
            QTextDocument thinking_doc;
            thinking_doc.setUndoRedoEnabled(false);
            thinking_doc.setDocumentMargin(0);
            thinking_doc.setDefaultFont(theme::fonts::caption());
            thinking_doc.setPlainText(QString::fromStdString(snapshot.thinking_text));
            thinking_doc.setTextWidth(body_width - thinking_header_h);
            metrics.thinking_h += t_spacing.xs + qRound(thinking_doc.size().height());
        }
    }

    QTextDocument* doc = snapshot.is_user
        ? cache_->plainDocumentFor(absolute, snapshot.text, body_width)
        : cache_->documentFor(absolute, static_cast<std::size_t>(revealed), body_width,
                              card_handler_);
    if (doc) {
        metrics.body_h = qRound(doc->size().height());
        if (metrics.body_h <= 0)
            metrics.body_h = qRound(QFontMetricsF(theme::fonts::body()).lineSpacing());
    }

    const auto& t = theme::tokens();
    metrics.width = option.rect.width();
    metrics.total_h = t.spacing.sm + metrics.role_h + t.spacing.sm + metrics.thinking_h +
        metrics.body_h + t.spacing.sm;
    if (height_cache_.size() > 512)
        height_cache_.clear();
    height_cache_.insert(height_key, metrics);
    return metrics;
}

QSize AidaChatMessageDelegate::sizeHint(const QStyleOptionViewItem& option,
                                        const QModelIndex& index) const {
    const auto metrics = measure(option, index);
    const int width = option.rect.width() > 0
        ? option.rect.width() : theme::scale_logical(theme::tokens().shell.min_panel_w, 2.0);
    return QSize(width, metrics.total_h + theme::tokens().spacing.sm);
}

void AidaChatMessageDelegate::paintThinkingDots(QPainter* painter, const QRect& rect) const {
    if (!phase_timer_) {
        phase_timer_ = new QElapsedTimer();
        phase_timer_->start();
    }
    const auto& t = theme::tokens();
    const qreal period = t.motion.hero / 1000.0;
    const qreal phase = std::fmod(phase_timer_->elapsed() / 1000.0, period) / period;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < 3; ++i) {
        const qreal local = std::fmod(phase + i * 0.22, 1.0);
        const qreal alpha = 0.25 + 0.75 * (local < 0.5 ? local * 2.0 : (1.0 - local) * 2.0);
        painter->setPen(Qt::NoPen);
        painter->setBrush(widgets::with_alpha(t.text_dim, alpha));
        painter->drawEllipse(QPointF(rect.left() + t.spacing.sm + i * t.spacing.md,
                                     rect.center().y()),
                             t.radius.xs, t.radius.xs);
    }
    painter->restore();
}

void AidaChatMessageDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const {
    const auto absolute = model_->absoluteIndexForRow(index.row());
    const auto identity = model_->identityAt(index.row());
    automation_ui::chat_message_snapshot_t snapshot;
    if (!model_->messageAt(index.row(), snapshot))
        return;

    const auto& t = theme::tokens();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRect card = cardRect(option.rect);
    const bool user = snapshot.is_user;
    const bool tool = !snapshot.tool_name.empty();
    QColor fill = t.bg_elevated;
    if (user)
        fill = widgets::mix_colors(t.bg_elevated, t.accent_dim, 0.14);
    else if (tool)
        fill = widgets::mix_colors(t.bg_elevated, t.warning, 0.07);
    fill = widgets::with_alpha(fill, 0.94);
    painter->setPen(Qt::NoPen);
    painter->setBrush(fill);
    painter->drawRoundedRect(card, t.radius.md, t.radius.md);
    painter->setPen(QPen(user ? widgets::with_alpha(t.accent_dim, 0.56) : t.border_subtle, 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(card, t.radius.md, t.radius.md);

    const QColor role_color = user ? t.accent_hover : tool ? t.warning : t.accent;
    painter->setPen(Qt::NoPen);
    painter->setBrush(widgets::with_alpha(role_color, 0.78));
    painter->drawRoundedRect(QRectF(card.left(), card.top(),
                                    t.spacing.xxs, card.height()),
                             t.spacing.xxs * 0.5, t.spacing.xxs * 0.5);

    const QString role_label = user ? QStringLiteral("YOU")
        : tool ? QStringLiteral("TOOL") : QStringLiteral("AIDA");
    const QString role_detail = tool ? QString::fromStdString(snapshot.tool_name)
        : !snapshot.model_id.empty() ? QString::fromStdString(snapshot.model_id)
        : user ? QStringLiteral("Workspace request") : QStringLiteral("Workspace assistant");
    const QFont pill_font = theme::fonts::caption();
    const QFontMetricsF pill_fm(pill_font);
    const QRect pill = rolePillRect(card, pill_fm, role_label);
    painter->setPen(QPen(widgets::with_alpha(role_color, 0.58), 1.0));
    painter->setBrush(widgets::with_alpha(role_color, 0.18));
    painter->drawRoundedRect(pill, pill.height() * 0.5, pill.height() * 0.5);
    painter->setFont(pill_font);
    painter->setPen(role_color);
    painter->drawText(pill, Qt::AlignCenter, role_label);
    painter->setFont(theme::fonts::caption());

    const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
    QString badge_text;
    if (!hovered && (snapshot.input_tokens > 0 || snapshot.output_tokens > 0)) {
        badge_text = QStringLiteral("in %1  ·  out %2")
            .arg(compact_count(snapshot.input_tokens))
            .arg(compact_count(snapshot.output_tokens));
        if (snapshot.cost > 0.0)
            badge_text += QStringLiteral("  ·  $%1").arg(snapshot.cost, 0, 'f', 4);
    } else if (!hovered && snapshot.timestamp > 0) {
        badge_text = QDateTime::fromMSecsSinceEpoch(snapshot.timestamp)
            .time().toString(QStringLiteral("HH:mm:ss"));
    }
    int badge_w = 0;
    if (!badge_text.isEmpty())
        badge_w = qRound(pill_fm.horizontalAdvance(badge_text)) + t.spacing.sm;
    const int detail_w = (std::max)(
        card.right() - pill.right() - t.spacing.sm * 2 - badge_w, 0);
    const QRect detail_rect(pill.right() + t.spacing.sm, pill.top(), detail_w,
                            pill.height());
    painter->setPen(t.text_dim);
    if (detail_w > 0)
        painter->drawText(detail_rect, Qt::AlignVCenter | Qt::AlignLeft,
                          pill_fm.elidedText(role_detail, Qt::ElideRight, detail_rect.width()));
    if (!badge_text.isEmpty()) {
        const QRect badge_rect(detail_rect.right() + t.spacing.sm, pill.top(),
                               badge_w - t.spacing.sm, pill.height());
        painter->setPen(t.text_dim);
        painter->drawText(badge_rect, Qt::AlignVCenter | Qt::AlignRight,
                          pill_fm.elidedText(badge_text, Qt::ElideRight, badge_rect.width()));
    }

    const auto metrics = measure(option, index);
    int content_y = card.top() + t.spacing.sm + metrics.role_h + t.spacing.sm;

    const int thinking_header_h = t.spacing.lg + t.spacing.xxs;
    const bool show_thinking = !user && snapshot.has_thinking &&
        !snapshot.thinking_text.empty();
    if (show_thinking) {
        const QRect header = thinkingHeaderRect(card, metrics.role_h);
        const bool expanded = thinkingExpanded(identity.fingerprint);
        painter->setPen(t.text_dim);
        painter->setFont(theme::fonts::caption());
        painter->drawText(header, Qt::AlignVCenter | Qt::AlignLeft,
                          expanded ? QStringLiteral("▾ Reasoning")
                                   : QStringLiteral("▸ Reasoning"));
        content_y += thinking_header_h;
        if (expanded) {
            painter->setFont(theme::fonts::caption());
            painter->setPen(t.text_dim);
            QTextDocument doc;
            doc.setUndoRedoEnabled(false);
            doc.setDocumentMargin(0);
            doc.setDefaultFont(theme::fonts::caption());
            doc.setPlainText(QString::fromStdString(snapshot.thinking_text));
            doc.setTextWidth(header.width() - thinking_header_h);
            painter->save();
            painter->translate(header.left() + thinking_header_h,
                               header.bottom() + t.spacing.xs);
            doc.drawContents(painter, painter->clipBoundingRect());
            painter->restore();
            content_y += t.spacing.xs + qRound(doc.size().height());
        }
    }

    const QRect body = bodyRect(card, metrics.role_h, metrics.thinking_h);
    if (snapshot.streaming && snapshot.text.empty() && !user) {
        paintThinkingDots(painter, QRect(body.left(), content_y, body.width(),
                                         t.spacing.xl + t.spacing.xxs));
    } else {
        std::uint64_t revealed = revealed_.value(absolute, 0);
        if (!snapshot.streaming || revealed == 0 || revealed > snapshot.text.size())
            revealed = snapshot.text.size();
        QTextDocument* doc = user
            ? cache_->plainDocumentFor(absolute, snapshot.text, body.width())
            : cache_->documentFor(absolute, static_cast<std::size_t>(revealed),
                                  body.width(), card_handler_);
        if (doc) {
            painter->save();
            painter->translate(body.left(), content_y);
            doc->drawContents(painter, painter->clipBoundingRect());
            painter->restore();
        }
    }

    if (option.state & QStyle::State_MouseOver) {
        const auto rects = actionRects(card);
        painter->setFont(theme::fonts::caption());
        const auto identity_now = identity;
        const auto caps = [&](int which) {
            return automation_ui::message_action_capability(identity_now,
                k_hover_actions[which].action);
        };
        for (int i = 0; i < rects.size(); ++i) {
            const auto cap = caps(i);
            if (!cap.visible)
                continue;
            painter->setPen(QPen(t.border_subtle, 1.0));
            painter->setBrush(widgets::with_alpha(t.panel_header, 0.95));
            painter->drawRoundedRect(rects[i], t.radius.sm, t.radius.sm);
            painter->setPen(cap.enabled ? t.text_secondary : widgets::with_alpha(t.text_dim, 0.5));
            painter->drawText(rects[i], Qt::AlignCenter,
                              QLatin1String(k_hover_actions[i].label));
        }
    }
    painter->restore();
}

bool AidaChatMessageDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                          const QStyleOptionViewItem& option,
                                          const QModelIndex& index) {
    Q_UNUSED(model);
    const auto* mouse = event->type() == QEvent::MouseButtonRelease
        ? static_cast<QMouseEvent*>(event) : nullptr;
    if (event->type() == QEvent::MouseButtonPress) {
        const auto* press = static_cast<QMouseEvent*>(event);
        if (press->button() == Qt::RightButton) {
            Q_EMIT contextMenuRequested(index, press->globalPosition().toPoint());
            return true;
        }
    }
    if (!mouse)
        return false;

    const auto absolute = model_->absoluteIndexForRow(index.row());
    const auto identity = model_->identityAt(index.row());
    automation_ui::chat_message_snapshot_t snapshot;
    if (!model_->messageAt(index.row(), snapshot))
        return false;
    const QRect card = cardRect(option.rect);
    const QPoint pos = mouse->position().toPoint();

    if (option.state & QStyle::State_MouseOver) {
        const auto rects = actionRects(card);
        for (int i = 0; i < rects.size(); ++i) {
            if (!rects[i].contains(pos))
                continue;
            const auto cap = automation_ui::message_action_capability(identity,
                k_hover_actions[i].action);
            if (cap.visible && cap.enabled) {
                Q_EMIT messageActionRequested(identity, k_hover_actions[i].action);
                return true;
            }
            return true;
        }
    }

    if (!snapshot.is_user && snapshot.has_thinking && !snapshot.thinking_text.empty()) {
        const auto metrics = measure(option, index);
        if (thinkingHeaderRect(card, metrics.role_h).contains(pos)) {
            setThinkingExpanded(identity.fingerprint,
                                !thinkingExpanded(identity.fingerprint));
            invalidateHeights();
            Q_EMIT sizeHintChanged(index);
            return true;
        }
    }

    QTextDocument* doc = nullptr;
    if (!snapshot.text.empty()) {
        const auto metrics = measure(option, index);
        const QRect body = bodyRect(card, metrics.role_h, metrics.thinking_h);
        const QPointF local = pos - body.topLeft();
        std::uint64_t revealed = revealed_.value(absolute, 0);
        if (!snapshot.streaming || revealed == 0 || revealed > snapshot.text.size())
            revealed = snapshot.text.size();
        doc = snapshot.is_user
            ? cache_->plainDocumentFor(absolute, snapshot.text, body.width())
            : cache_->documentFor(absolute, static_cast<std::size_t>(revealed),
                                  body.width(), card_handler_);
        if (doc) {
            const QString anchor = doc->documentLayout()->anchorAt(local);
            if (!anchor.isEmpty()) {
                const QUrl url(anchor);
                if (url.scheme() == QLatin1String("http") ||
                    url.scheme() == QLatin1String("https")) {
                    Q_EMIT linkActivated(anchor);
                    return true;
                }
            }
            const int hit = doc->documentLayout()->hitTest(local, Qt::ExactHit);
            if (hit >= 0) {
                QTextCursor cursor(doc);
                cursor.setPosition(hit);
                const QTextCharFormat fmt = cursor.charFormat();
                if (fmt.objectType() == kChatToolCardObjectType) {
                    const int ordinal = fmt.property(kToolCardOrdinalProperty).toInt();
                    const QString body_text =
                        fmt.property(kToolCardPayloadProperty).toString();
                    const bool expanded = fmt.property(kToolCardExpandedProperty).toBool();
                    const auto lines = payload_lines(body_text);
                    if (!expanded && lines.size() <= AidaToolCardTextObject::k_collapsed_line_cap)
                        return false;
                    cache_->setCardExpanded(absolute, ordinal, !expanded);
                    invalidateHeights();
                    Q_EMIT sizeHintChanged(index);
                    return true;
                }
            }
        }
    }
    return false;
}

QWidget* AidaChatMessageDelegate::createEditor(QWidget* parent,
                                               const QStyleOptionViewItem& option,
                                               const QModelIndex& index) const {
    Q_UNUSED(option);
    Q_UNUSED(index);
    auto* container = new QWidget(parent);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(theme::tokens().spacing.xs, theme::tokens().spacing.xs,
                               theme::tokens().spacing.xs, theme::tokens().spacing.xs);
    layout->setSpacing(theme::tokens().spacing.xs);
    auto* edit = new QPlainTextEdit(container);
    edit->setObjectName(QStringLiteral("editor"));
    layout->addWidget(edit, 1);
    auto* buttons = new QHBoxLayout();
    auto* save = new QPushButton(QStringLiteral("Save and Resend"), container);
    save->setObjectName(QStringLiteral("save"));
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), container);
    cancel->setObjectName(QStringLiteral("cancel"));
    buttons->addStretch(1);
    buttons->addWidget(save);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    QObject::connect(save, &QPushButton::clicked, container, [this, container] {
        const_cast<AidaChatMessageDelegate*>(this)->commitData(container);
        const_cast<AidaChatMessageDelegate*>(this)->closeEditor(container,
            QAbstractItemDelegate::NoHint);
    });
    QObject::connect(cancel, &QPushButton::clicked, container, [this, container] {
        const_cast<AidaChatMessageDelegate*>(this)->closeEditor(container,
            QAbstractItemDelegate::RevertModelCache);
    });
    return container;
}

void AidaChatMessageDelegate::setEditorData(QWidget* editor,
                                            const QModelIndex& index) const {
    automation_ui::chat_message_snapshot_t snapshot;
    if (!model_->messageAt(index.row(), snapshot))
        return;
    if (auto* edit = editor->findChild<QPlainTextEdit*>(QStringLiteral("editor")))
        edit->setPlainText(QString::fromStdString(snapshot.text));
}

void AidaChatMessageDelegate::setModelData(QWidget* editor, QAbstractItemModel* model,
                                           const QModelIndex& index) const {
    Q_UNUSED(model);
    auto* edit = editor->findChild<QPlainTextEdit*>(QStringLiteral("editor"));
    if (!edit)
        return;
    const QString text = edit->toPlainText();
    if (text.trimmed().isEmpty())
        return;
    const auto identity = model_->identityAt(index.row());
    const std::string replacement = text.toStdString();
    if (replacement.empty())
        return;
    if (!automation_ui::truncate_messages_from(identity).succeeded)
        return;
    automation_ui::append_user_message(replacement);
}

void AidaChatMessageDelegate::updateEditorGeometry(QWidget* editor,
                                                    const QStyleOptionViewItem& option,
                                                    const QModelIndex& index) const {
    Q_UNUSED(index);
    const auto& s = theme::tokens().spacing;
    editor->setGeometry(option.rect.adjusted(s.sm - s.xxs, s.xs, -(s.sm - s.xxs), -s.xs));
}

bool AidaChatMessageDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view,
                                        const QStyleOptionViewItem& option,
                                        const QModelIndex& index) {
    if (event->type() != QEvent::ToolTip)
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    automation_ui::chat_message_snapshot_t snapshot;
    if (!model_->messageAt(index.row(), snapshot))
        return false;
    QString tip;
    if (snapshot.timestamp > 0) {
        tip = QDateTime::fromMSecsSinceEpoch(snapshot.timestamp)
            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }
    if (snapshot.input_tokens > 0 || snapshot.output_tokens > 0) {
        if (!tip.isEmpty())
            tip += QLatin1Char('\n');
        tip += QStringLiteral("in %1  ·  out %2").arg(snapshot.input_tokens)
                  .arg(snapshot.output_tokens);
        if (snapshot.cost > 0.0)
            tip += QStringLiteral("  ·  $%1").arg(snapshot.cost, 0, 'f', 4);
    }
    if (tip.isEmpty())
        return false;
    QToolTip::showText(event->globalPos(), tip, view);
    return true;
}

AidaToolCardTextObject::AidaToolCardTextObject(QObject* parent) : QObject(parent) {}

QSizeF AidaToolCardTextObject::intrinsicSize(QTextDocument* doc, int posInDocument,
                                              const QTextFormat& format) {
    Q_UNUSED(posInDocument);
    const auto& t = theme::tokens();
    const QString body = format.property(kToolCardPayloadProperty).toString();
    const bool expanded = format.property(kToolCardExpandedProperty).toBool();
    const QFontMetricsF fm(theme::fonts::codeRegular());
    const int line_h = qRound(fm.lineSpacing());
    const auto all_lines = payload_lines(body);
    const int lines = expanded ? static_cast<int>(all_lines.size())
        : (std::min)(k_collapsed_line_cap, static_cast<int>(all_lines.size()));
    const qreal w = doc ? doc->textWidth() : 0.0;
    const qreal footer_h = expanded ? (t.spacing.lg + t.spacing.xxs)
        : (all_lines.size() > k_collapsed_line_cap ? (t.spacing.lg + t.spacing.xxs)
                                                   : (t.spacing.xs + t.spacing.xxs));
    return QSizeF((std::max)(w, qreal(t.shell.min_panel_w)),
                  t.spacing.xxl + t.spacing.xs + t.spacing.xxs +
                  (std::max)(1, lines) * line_h + footer_h);
}

void AidaToolCardTextObject::drawObject(QPainter* painter, const QRectF& rect,
                                        QTextDocument* doc, int posInDocument,
                                        const QTextFormat& format) {
    Q_UNUSED(doc);
    Q_UNUSED(posInDocument);
    const auto& t = theme::tokens();
    const QString name = format.property(kToolCardNameProperty).toString();
    const QString kind = format.property(kToolCardKindProperty).toString();
    const QString body = format.property(kToolCardPayloadProperty).toString();
    const bool expanded = format.property(kToolCardExpandedProperty).toBool();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const QRectF card = rect.adjusted(0, t.panel.border, 0, -t.panel.border);
    painter->setPen(QPen(t.border_subtle, 1.0));
    painter->setBrush(widgets::with_alpha(t.panel_header, 0.85));
    painter->drawRoundedRect(card, t.radius.md, t.radius.md);

    const bool is_call = kind == QLatin1String("call");
    const QColor accent = is_call ? t.info : t.warning;
    painter->setPen(Qt::NoPen);
    painter->setBrush(accent);
    painter->drawRoundedRect(QRectF(card.left(), card.top(), t.spacing.xxs, card.height()),
                             t.spacing.xxs * 0.5, t.spacing.xxs * 0.5);

    const qreal inset_x = t.spacing.sm + t.spacing.xxs;
    const qreal head_h = t.spacing.lg + t.spacing.xxs;
    const QFont head_font = theme::fonts::bodyEm();
    const QFontMetricsF head_fm(head_font);
    painter->setFont(head_font);
    painter->setPen(t.text_secondary);
    const QRectF head_rect(card.left() + inset_x, card.top() + t.spacing.xs,
                           card.width() - inset_x * 2, head_h);
    painter->drawText(head_rect, Qt::AlignVCenter | Qt::AlignLeft,
                      head_fm.elidedText(QStringLiteral("%1  ·  %2")
                          .arg(is_call ? QStringLiteral("Tool call")
                                       : QStringLiteral("Tool result"), name),
                          Qt::ElideRight, head_rect.width()));

    const QFont code_font = theme::fonts::codeRegular();
    painter->setFont(code_font);
    painter->setPen(t.text_dim);
    const QFontMetricsF fm(code_font);
    const qreal line_h = fm.lineSpacing();
    const auto lines = payload_lines(body);
    const int shown = expanded ? static_cast<int>(lines.size())
        : (std::min)(k_collapsed_line_cap, static_cast<int>(lines.size()));
    qreal y = card.top() + t.spacing.xxl;
    const QRectF clip(card.left() + inset_x, y, card.width() - inset_x * 2,
                      card.bottom() - y - t.spacing.xs);
    painter->setClipRect(clip);
    for (int i = 0; i < shown && y < clip.bottom(); ++i) {
        painter->drawText(QPointF(clip.left(), y + fm.ascent()), lines[i]);
        y += line_h;
    }
    painter->setClipping(false);
    const QRectF footer_rect(card.left() + inset_x, card.bottom() - t.spacing.lg,
                             card.width() - inset_x * 2, t.spacing.md + t.spacing.xxs);
    if (!expanded && lines.size() > k_collapsed_line_cap) {
        painter->setPen(t.accent);
        painter->drawText(footer_rect, Qt::AlignVCenter | Qt::AlignLeft,
                          QStringLiteral("Expand (%1 more lines)  ▸")
                              .arg(lines.size() - k_collapsed_line_cap));
    } else if (expanded) {
        painter->setPen(t.accent);
        painter->drawText(footer_rect, Qt::AlignVCenter | Qt::AlignLeft,
                          QStringLiteral("Collapse  ▾"));
    }
    painter->restore();
}

namespace {

void insert_inline_runs(QTextCursor& cursor, const chat_render::span_t& span,
                        qreal& budget) {
    const auto& t = theme::tokens();
    QTextCharFormat fmt;
    switch (span.type) {
    case chat_render::span_type::bold:
        fmt.setFontWeight(QFont::Bold);
        break;
    case chat_render::span_type::italic:
        fmt.setFontItalic(true);
        break;
    case chat_render::span_type::bold_italic:
        fmt.setFontWeight(QFont::Bold);
        fmt.setFontItalic(true);
        break;
    case chat_render::span_type::inline_code:
        fmt.setFont(theme::fonts::codeRegular());
        fmt.setBackground(t.panel_header);
        break;
    case chat_render::span_type::link:
        fmt.setAnchor(true);
        fmt.setAnchorHref(QString::fromStdString(span.url));
        fmt.setForeground(t.accent_hover);
        fmt.setFontUnderline(true);
        break;
    case chat_render::span_type::strikethrough:
        fmt.setFontStrikeOut(true);
        break;
    default:
        break;
    }
    QString text = QString::fromStdString(span.text);
    if (static_cast<qreal>(span.text.size()) > budget)
        text.truncate(static_cast<QString::size_type>(budget));
    budget -= (std::min)(budget, static_cast<qreal>(span.text.size()));
    const auto segments = text.split(QLatin1Char('\n'));
    for (int i = 0; i < segments.size(); ++i) {
        if (i > 0)
            cursor.insertBlock();
        if (!segments[i].isEmpty())
            cursor.insertText(segments[i], fmt);
    }
}

void insert_code_block(QTextCursor& cursor, const chat_render::span_t& span,
                       qreal& budget, QObject* handler_parent) {
    Q_UNUSED(handler_parent);
    const auto& t = theme::tokens();
    QTextTableFormat table_fmt;
    table_fmt.setBorder(0);
    table_fmt.setCellPadding(t.spacing.sm);
    table_fmt.setCellSpacing(0);
    table_fmt.setTopMargin(t.spacing.xs);
    table_fmt.setBottomMargin(t.spacing.xs);
    QTextTable* table = cursor.insertTable(1, 1, table_fmt);
    QTextTableCell cell = table->cellAt(0, 0);
    QTextTableCellFormat cell_fmt;
    cell_fmt.setBackground(widgets::with_alpha(t.panel_header, 0.85));
    cell.setFormat(cell_fmt);
    QTextCursor cell_cursor = cell.firstCursorPosition();

    std::string code = span.text;
    if (static_cast<qreal>(code.size()) > budget)
        code.resize(static_cast<std::size_t>((std::max)(budget, qreal{0})));
    budget -= (std::min)(budget, static_cast<qreal>(span.text.size()));

    const auto lang = chat_render::chat_language_def(span.language);
    const QFont code_font = theme::fonts::codeRegular();
    QTextCharFormat base;
    base.setFont(code_font);
    base.setForeground(theme::tokens().syn_identifier);
    std::istringstream lines(code);
    std::string line;
    bool first = true;
    while (std::getline(lines, line)) {
        if (!first)
            cell_cursor.insertBlock();
        first = false;
        std::vector<syntax::token_t> tokens;
        syntax::tokenize(line, lang, tokens);
        for (const auto& token : tokens) {
            QTextCharFormat tf = base;
            tf.setForeground(token_color(token.type));
            cell_cursor.insertText(
                QString::fromStdString(line.substr(token.start, token.length)), tf);
        }
    }
    if (first)
        cell_cursor.insertText(QString(), base);
    QTextCursor after = cursor;
    after.movePosition(QTextCursor::End);
    cursor = after;
}

void insert_tool_card(QTextCursor& cursor, const chat_render::span_t& span,
                      bool is_call, int ordinal, bool expanded, qreal& budget) {
    const auto payload = decode_tool_payload(span.text, is_call);
    QTextCharFormat fmt;
    fmt.setObjectType(kChatToolCardObjectType);
    fmt.setProperty(kToolCardNameProperty, payload.title);
    fmt.setProperty(kToolCardKindProperty,
                    is_call ? QStringLiteral("call") : QStringLiteral("result"));
    fmt.setProperty(kToolCardPayloadProperty, payload.body);
    fmt.setProperty(kToolCardOrdinalProperty, ordinal);
    fmt.setProperty(kToolCardExpandedProperty, expanded);
    cursor.insertText(QString(QChar::ObjectReplacementCharacter), fmt);
    budget -= (std::min)(budget, static_cast<qreal>(span.text.size()));
}

void insert_table(QTextCursor& cursor, const chat_render::span_t& span, qreal& budget) {
    if (span.table_data.empty())
        return;
    std::size_t columns = 0;
    for (const auto& row : span.table_data)
        columns = (std::max)(columns, row.size());
    if (columns == 0)
        return;
    const auto& t = theme::tokens();
    QTextTableFormat table_fmt;
    table_fmt.setBorder(0);
    table_fmt.setCellPadding(t.spacing.xs);
    table_fmt.setCellSpacing(0);
    table_fmt.setTopMargin(t.spacing.xxs);
    table_fmt.setBottomMargin(t.spacing.xxs);
    QTextTable* table = cursor.insertTable(static_cast<int>(span.table_data.size()),
                                           static_cast<int>(columns), table_fmt);
    for (std::size_t r = 0; r < span.table_data.size(); ++r) {
        for (std::size_t c = 0; c < columns; ++c) {
            QTextTableCell cell = table->cellAt(static_cast<int>(r), static_cast<int>(c));
            QTextCursor cell_cursor = cell.firstCursorPosition();
            QTextCharFormat fmt;
            if (r == 0)
                fmt.setFontWeight(QFont::Bold);
            const std::string text =
                c < span.table_data[r].size() ? span.table_data[r][c] : std::string();
            cell_cursor.insertText(QString::fromStdString(text), fmt);
        }
    }
    QTextCursor after = cursor;
    after.movePosition(QTextCursor::End);
    cursor = after;
    for (const auto& row : span.table_data)
        for (const auto& cell_text : row)
            budget -= (std::min)(budget, static_cast<qreal>(cell_text.size()));
}

} // namespace

std::unique_ptr<QTextDocument> AidaMarkdownDocumentBuilder::build(
    const std::vector<chat_render::span_t>& spans, std::size_t revealed_len,
    QObject* handler_parent, AidaToolCardTextObject* handler,
    const std::vector<bool>* expanded_cards) {
    const auto& t = theme::tokens();
    auto doc = std::make_unique<QTextDocument>();
    doc->setUndoRedoEnabled(false);
    doc->setDocumentMargin(0);
    doc->setDefaultFont(theme::fonts::body());
    QTextOption wrap_option;
    wrap_option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc->setDefaultTextOption(wrap_option);
    if (handler)
        doc->documentLayout()->registerHandler(kChatToolCardObjectType, handler);

    QTextCursor cursor(doc.get());
    const qreal budget_start = revealed_len == std::string::npos
        ? static_cast<qreal>(1e15) : static_cast<qreal>(revealed_len);
    qreal budget = budget_start;
    Q_UNUSED(handler_parent);

    int tool_ordinal = 0;
    bool block_dirty = false;
    auto refresh_dirty = [&] {
        block_dirty = !cursor.block().text().isEmpty();
    };
    auto open_block = [&] {
        if (block_dirty) {
            cursor.insertBlock();
            block_dirty = false;
        }
    };

    for (const auto& span : spans) {
        if (budget <= 0)
            break;
        switch (span.type) {
        case chat_render::span_type::text:
        case chat_render::span_type::bold:
        case chat_render::span_type::italic:
        case chat_render::span_type::bold_italic:
        case chat_render::span_type::inline_code:
        case chat_render::span_type::link:
        case chat_render::span_type::strikethrough:
            insert_inline_runs(cursor, span, budget);
            refresh_dirty();
            break;
        case chat_render::span_type::paragraph_break:
            cursor.insertBlock();
            block_dirty = false;
            break;
        case chat_render::span_type::heading1:
        case chat_render::span_type::heading2:
        case chat_render::span_type::heading3: {
            open_block();
            QFont heading_font = span.type == chat_render::span_type::heading1
                ? theme::fonts::large()
                : span.type == chat_render::span_type::heading2
                    ? theme::fonts::h1() : theme::fonts::h2();
            if (span.type == chat_render::span_type::heading1)
                heading_font.setWeight(QFont::Bold);
            QTextCharFormat fmt;
            fmt.setFont(heading_font);
            fmt.setForeground(t.text_primary);
            cursor.insertText(QString::fromStdString(span.text), fmt);
            QTextBlockFormat block_fmt;
            block_fmt.setTopMargin(t.spacing.xs + t.spacing.xxs);
            block_fmt.setBottomMargin(t.spacing.xxs);
            cursor.setBlockFormat(block_fmt);
            block_dirty = true;
            budget -= (std::min)(budget, static_cast<qreal>(span.text.size()));
            break;
        }
        case chat_render::span_type::list_bullet:
        case chat_render::span_type::list_numbered:
        case chat_render::span_type::task_unchecked:
        case chat_render::span_type::task_checked: {
            open_block();
            QTextBlockFormat block_fmt;
            block_fmt.setLeftMargin(t.spacing.lg + t.spacing.xxs +
                span.list_indent * (t.spacing.md + t.spacing.xxs));
            cursor.setBlockFormat(block_fmt);
            QString prefix;
            if (span.type == chat_render::span_type::list_bullet)
                prefix = QStringLiteral("•  ");
            else if (span.type == chat_render::span_type::list_numbered)
                prefix = QStringLiteral("%1.  ").arg(span.list_index);
            else
                prefix = span.type == chat_render::span_type::task_checked
                    ? QStringLiteral("[x] ") : QStringLiteral("[ ] ");
            QTextCharFormat dim;
            dim.setForeground(t.text_dim);
            cursor.insertText(prefix, dim);
            QTextCharFormat plain;
            cursor.insertText(QString::fromStdString(span.text), plain);
            block_dirty = true;
            budget -= (std::min)(budget,
                static_cast<qreal>(span.text.size() + prefix.size()));
            break;
        }
        case chat_render::span_type::blockquote: {
            open_block();
            QTextCharFormat fmt;
            fmt.setForeground(t.text_dim);
            fmt.setFontItalic(true);
            cursor.insertText(QString::fromStdString(span.text), fmt);
            QTextBlockFormat block_fmt;
            block_fmt.setLeftMargin(t.spacing.lg);
            block_fmt.setBackground(widgets::with_alpha(t.info_soft, 0.10));
            cursor.setBlockFormat(block_fmt);
            block_dirty = true;
            budget -= (std::min)(budget, static_cast<qreal>(span.text.size()));
            break;
        }
        case chat_render::span_type::hrule: {
            open_block();
            QTextCharFormat fmt;
            fmt.setForeground(t.border_subtle);
            cursor.insertText(QStringLiteral("────────────────"), fmt);
            QTextBlockFormat block_fmt;
            block_fmt.setTopMargin(t.spacing.xs);
            block_fmt.setBottomMargin(t.spacing.xs);
            cursor.setBlockFormat(block_fmt);
            block_dirty = true;
            break;
        }
        case chat_render::span_type::code_block:
            open_block();
            insert_code_block(cursor, span, budget, handler_parent);
            refresh_dirty();
            break;
        case chat_render::span_type::table:
            open_block();
            insert_table(cursor, span, budget);
            refresh_dirty();
            break;
        case chat_render::span_type::tool_call:
        case chat_render::span_type::tool_result: {
            open_block();
            if (handler == nullptr) {
                chat_render::span_t fallback;
                fallback.type = chat_render::span_type::code_block;
                fallback.text = span.text;
                fallback.language = span.language;
                insert_code_block(cursor, fallback, budget, handler_parent);
                refresh_dirty();
                break;
            }
            const bool expanded = expanded_cards &&
                tool_ordinal < static_cast<int>(expanded_cards->size())
                ? (*expanded_cards)[static_cast<std::size_t>(tool_ordinal)] : false;
            insert_tool_card(cursor, span,
                             span.type == chat_render::span_type::tool_call,
                             tool_ordinal, expanded, budget);
            ++tool_ordinal;
            block_dirty = true;
            break;
        }
        }
    }
    return doc;
}

const AidaChatDocumentCache::entry_t& AidaChatDocumentCache::entryFor(
    std::size_t index, const std::string& text) /*lock-free: GUI thread only*/ {
    const std::uint64_t fnv = fnv1a64(text);
    auto found = entries_.find(index);
    if (found != entries_.end() && found->second.parsed &&
        found->second.text_fnv == fnv && found->second.text_size == text.size()) {
        found->second.use_tick = ++tick_;
        ++hits_;
        if (((hits_ + misses_) & 0xFF) == 0)
            diag::log_tagged_fmt("qt_ai_chat",
                "chatdoc cache_hit=%llu cache_miss=%llu rebuild=%llu entries=%zu",
                static_cast<unsigned long long>(hits_),
                static_cast<unsigned long long>(misses_),
                static_cast<unsigned long long>(rebuilds_), entries_.size());
        return found->second;
    }
    ++misses_;
    entry_t fresh;
    fresh.text_fnv = fnv;
    fresh.text_size = text.size();
    fresh.spans = chat_render::parse_markdown(text);
    fresh.parsed = true;
    fresh.use_tick = ++tick_;
    fresh.expanded_cards.assign(
        std::count_if(fresh.spans.begin(), fresh.spans.end(), [](const auto& s) {
            return s.type == chat_render::span_type::tool_call ||
                s.type == chat_render::span_type::tool_result;
        }), false);
    if (found != entries_.end()) {
        fresh.expanded_cards.resize((std::max)(fresh.expanded_cards.size(),
                                               found->second.expanded_cards.size()));
        for (std::size_t i = 0; i < found->second.expanded_cards.size() &&
                i < fresh.expanded_cards.size(); ++i)
            fresh.expanded_cards[i] = found->second.expanded_cards[i];
        found->second = std::move(fresh);
        return found->second;
    }
    auto inserted = entries_.emplace(index, std::move(fresh));
    evictIfNeeded();
    return inserted.first->second;
}

QTextDocument* AidaChatDocumentCache::documentFor(
    std::size_t index, std::size_t revealed_len, qreal text_width,
    AidaToolCardTextObject* handler) {
    auto& entry = touch(index);
    const quint64 palette = theme::AidaThemeController::instance().generation();
    if (!entry.document || entry.revealed_len != revealed_len ||
        entry.palette_revision != palette) {
        entry.document = AidaMarkdownDocumentBuilder::build(
            entry.spans, revealed_len, nullptr, handler, &entry.expanded_cards);
        entry.revealed_len = revealed_len;
        entry.palette_revision = palette;
        ++rebuilds_;
    }
    if (text_width > 0 && entry.document->textWidth() != text_width)
        entry.document->setTextWidth(text_width);
    return entry.document.get();
}

QTextDocument* AidaChatDocumentCache::plainDocumentFor(std::size_t index,
                                                       const std::string& text,
                                                       qreal text_width) {
    entryFor(index, text);
    auto& entry = touch(index);
    const quint64 palette = theme::AidaThemeController::instance().generation();
    if (!entry.document || entry.palette_revision != palette ||
        entry.revealed_len != text.size()) {
        auto plain = std::make_unique<QTextDocument>();
        plain->setUndoRedoEnabled(false);
        plain->setDocumentMargin(0);
        plain->setDefaultFont(theme::fonts::body());
        plain->setPlainText(QString::fromStdString(text));
        entry.document = std::move(plain);
        entry.revealed_len = text.size();
        entry.palette_revision = palette;
        ++rebuilds_;
    }
    if (text_width > 0 && entry.document->textWidth() != text_width)
        entry.document->setTextWidth(text_width);
    return entry.document.get();
}

AidaChatDocumentCache::entry_t& AidaChatDocumentCache::touch(std::size_t index) {
    auto& entry = entries_[index];
    entry.use_tick = ++tick_;
    return entry;
}

void AidaChatDocumentCache::setCardExpanded(std::size_t index, int ordinal, bool expanded) {
    auto found = entries_.find(index);
    if (found == entries_.end())
        return;
    auto& cards = found->second.expanded_cards;
    if (ordinal < 0)
        return;
    if (static_cast<std::size_t>(ordinal) >= cards.size())
        cards.resize(static_cast<std::size_t>(ordinal) + 1, false);
    if (cards[static_cast<std::size_t>(ordinal)] == expanded)
        return;
    cards[static_cast<std::size_t>(ordinal)] = expanded;
    found->second.document.reset();
}

void AidaChatDocumentCache::invalidateDocument(std::size_t index) {
    auto found = entries_.find(index);
    if (found != entries_.end())
        found->second.document.reset();
}

void AidaChatDocumentCache::clear() {
    entries_.clear();
}

void AidaChatDocumentCache::evictIfNeeded() {
    while (entries_.size() > k_max_entries) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.use_tick < oldest->second.use_tick)
                oldest = it;
        }
        entries_.erase(oldest);
    }
}

}

