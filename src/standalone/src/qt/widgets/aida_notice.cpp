#include "aida_notice.hpp"

#include <QEvent>
#include <QPainter>

#include "../theme/aida_fonts.hpp"
#include "../theme/aida_stylesheet.hpp"
#include "aida_button.hpp"

namespace aida::qt::widgets {

AidaNotice::AidaNotice(QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("aida.notice"));
    setFrameShape(QFrame::StyledPanel);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setProperty("aidaRole", QStringLiteral("notice"));
    setProperty("aidaVariant", QStringLiteral("info"));
}

AidaNotice::AidaNotice(const QString& title, const QString& message, AidaSemantic kind,
    QWidget* parent)
    : AidaNotice(parent)
{
    title_ = title;
    message_ = message;
    setKind(kind);
}

void AidaNotice::setTitle(const QString& title)
{
    if (title_ == title)
        return;
    title_ = title;
    setAccessibleName(title_);
    updateGeometry();
    update();
}

void AidaNotice::setMessage(const QString& message)
{
    if (message_ == message)
        return;
    message_ = message;
    setAccessibleDescription(message_);
    updateGeometry();
    update();
}

void AidaNotice::setKind(AidaSemantic kind)
{
    if (kind_ == kind)
        return;
    kind_ = kind;
    setProperty("aidaVariant", QString::fromLatin1(semantic_variant_name(kind_)));
    aida::qt::theme::stylesheet::repolish(this);
    update();
}

void AidaNotice::setActionLabel(const QString& label)
{
    if (label.isEmpty()) {
        if (action_button_) {
            action_button_->hide();
            action_button_->deleteLater();
            action_button_ = nullptr;
        }
    } else {
        if (!action_button_) {
            action_button_ = new AidaButton(this);
            action_button_->setKind(AidaButton::Kind::Ghost);
            action_button_->setControlSize(AidaButton::ControlSize::Small);
            connect(action_button_, &QAbstractButton::clicked, this,
                [this] { Q_EMIT actionTriggered(); });
        }
        action_button_->setText(label);
        action_button_->show();
    }
    layoutAction();
    updateGeometry();
    update();
}

QString AidaNotice::actionLabel() const
{
    return action_button_ ? action_button_->text() : QString();
}

qreal AidaNotice::noticeHeight() const
{
    const auto& t = aida::qt::theme::tokens();
    return qreal(message_.isEmpty() ? t.panel.header_h : t.panel.header_h + t.spacing.md);
}

QSize AidaNotice::sizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(t.shell.min_panel_w * 3 + t.spacing.section, qRound(noticeHeight()));
}

QSize AidaNotice::minimumSizeHint() const
{
    const auto& t = aida::qt::theme::tokens();
    return QSize(t.shell.min_panel_w * 2 - t.spacing.section, qRound(noticeHeight()));
}

void AidaNotice::layoutAction()
{
    if (!action_button_ || !action_button_->isVisible())
        return;
    const auto& t = aida::qt::theme::tokens();
    const QSize hint = action_button_->sizeHint();
    const qreal h = rect().height() > 0 ? qreal(rect().height()) : noticeHeight();
    const qreal y = (h - qreal(t.control.height_sm)) * 0.5;
    action_button_->setGeometry(qRound(rect().right() - hint.width() - qreal(t.spacing.sm)),
        qRound(y), hint.width(), t.control.height_sm);
}

void AidaNotice::paintEvent(QPaintEvent* event)
{
    QFrame::paintEvent(event);

    const auto& t = aida::qt::theme::tokens();
    const QColor col = semantic_color(kind_);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect());
    const bool has_msg = !message_.isEmpty();

    const QFont title_font = aida::qt::theme::fonts::bodyEm();
    const QFontMetricsF title_fm(title_font);
    const qreal pad_top = qreal(t.spacing.sm + t.panel.border);
    const qreal title_y = has_msg
        ? r.top() + pad_top + title_fm.ascent()
        : text_baseline_centered(r, title_fm);

    const qreal disc_r = qreal(t.spacing.sm);
    const qreal dot_r = qreal(t.status_bar.dot + t.panel.border) * 0.5;
    const qreal dot_cy = has_msg
        ? r.top() + pad_top + title_fm.height() * 0.5
        : r.top() + r.height() * 0.5;
    const QPointF disc_c(r.left() + qreal(t.spacing.lg), dot_cy);
    p.setPen(Qt::NoPen);
    p.setBrush(semantic_fill_color(kind_));
    p.drawEllipse(disc_c, disc_r, disc_r);
    QPen edge_pen(semantic_edge_color(kind_), qreal(t.panel.border));
    p.setPen(edge_pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(disc_c, disc_r - 0.5, disc_r - 0.5);
    p.setPen(Qt::NoPen);
    p.setBrush(col);
    p.drawEllipse(disc_c, dot_r, dot_r);

    const qreal text_x = r.left() + qreal(t.spacing.lg + t.spacing.md);
    qreal text_right = r.right() - qreal(t.spacing.sm);
    if (action_button_ && action_button_->isVisible())
        text_right = action_button_->geometry().left() - qreal(t.spacing.sm);

    p.setFont(title_font);
    p.setPen(t.text_primary);
    const qreal text_avail = text_right - text_x;
    if (text_avail > 0.0)
        p.drawText(QPointF(text_x, title_y),
            title_fm.elidedText(title_, Qt::ElideRight, text_avail));

    if (has_msg && text_avail > 0.0) {
        const QFont msg_font = aida::qt::theme::fonts::caption();
        const QFontMetricsF msg_fm(msg_font);
        const qreal msg_y = r.top() + pad_top + title_fm.height()
            + qreal(t.spacing.xxs) + msg_fm.ascent();
        p.setFont(msg_font);
        p.setPen(t.text_secondary);
        p.drawText(QPointF(text_x, msg_y),
            msg_fm.elidedText(message_, Qt::ElideRight, text_avail));
    }
}

void AidaNotice::resizeEvent(QResizeEvent* event)
{
    layoutAction();
    QFrame::resizeEvent(event);
}

void AidaNotice::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange) {
        updateGeometry();
        update();
    }
    QFrame::changeEvent(event);
}

}
