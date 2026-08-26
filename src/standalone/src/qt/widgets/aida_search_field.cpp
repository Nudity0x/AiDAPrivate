#include "aida_search_field.hpp"

#include <QAction>
#include <QFocusEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>

#include "../theme/aida_fonts.hpp"
#include "../theme/aida_icons.hpp"
#include "aida_paint_utils.hpp"

namespace aida::qt::widgets {

AidaSearchField::AidaSearchField(QWidget* parent)
    : AidaLineEdit(parent)
{
    setObjectName(QStringLiteral("aida.search_field"));
    setPlaceholderText(QStringLiteral("Search"));
    setAccessibleName(placeholderText());

    const auto& t = aida::qt::theme::tokens();
    leading_action_ = addAction(aida::qt::theme::icons::tinted(
        QStringLiteral("search"), t.text_dim.rgb(), 16, devicePixelRatioF()), LeadingPosition);

    clear_action_ = new QAction(QIcon(QStringLiteral(":/icons/clear-x.svg")),
        QStringLiteral("Clear"), this);
    clear_action_->setToolTip(QStringLiteral("Clear search"));
    addAction(clear_action_, TrailingPosition);
    clear_action_->setVisible(false);
    connect(clear_action_, &QAction::triggered, this, [this] {
        clear();
        setFocus(Qt::OtherFocusReason);
        Q_EMIT cleared();
    });
    connect(this, &QLineEdit::textChanged, this, [this](const QString&) {
        updateClearVisibility();
    });

    match_label_ = new QLabel();
    match_label_->setFocusPolicy(Qt::NoFocus);
    match_label_->setFont(aida::qt::theme::fonts::caption());
    match_label_->setProperty("aidaTone", QStringLiteral("dim"));
    match_label_->hide();
}

AidaSearchField::AidaSearchField(const QString& placeholder, QWidget* parent)
    : AidaSearchField(parent)
{
    setPlaceholderText(placeholder);
    if (!placeholder.isEmpty())
        setAccessibleName(placeholder);
}

QLabel* AidaSearchField::matchCountLabel() const
{
    return match_label_;
}

void AidaSearchField::setMatchCount(qint64 total, qint64 active)
{
    match_total_ = total;
    match_active_ = active;
    if (total < 0) {
        match_label_->hide();
        match_label_->clear();
        return;
    }
    QString text;
    if (active >= 0)
        text = QStringLiteral("%1/%2").arg(active).arg(total);
    else
        text = QString::number(total);
    match_label_->setText(text);
    match_label_->show();
}

void AidaSearchField::clearMatchCount()
{
    setMatchCount(-1, -1);
}

void AidaSearchField::focusInEvent(QFocusEvent* event)
{
    refreshLeadingIcon(true);
    AidaLineEdit::focusInEvent(event);
}

void AidaSearchField::focusOutEvent(QFocusEvent* event)
{
    refreshLeadingIcon(false);
    AidaLineEdit::focusOutEvent(event);
}

void AidaSearchField::refreshLeadingIcon(bool focused)
{
    if (!leading_action_)
        return;
    const auto& t = aida::qt::theme::tokens();
    const QColor col = focused ? t.accent : t.text_dim;
    leading_action_->setIcon(aida::qt::theme::icons::tinted(
        QStringLiteral("search"), col.rgb(), 16, devicePixelRatioF()));
}

void AidaSearchField::updateClearVisibility()
{
    clear_action_->setVisible(!text().isEmpty());
}

void AidaSearchField::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::DevicePixelRatioChange)
        refreshLeadingIcon(hasFocus());
    AidaLineEdit::changeEvent(event);
}

void AidaSearchField::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && !text().isEmpty() && !event->isAutoRepeat()) {
        clear();
        Q_EMIT cleared();
        event->accept();
        return;
    }
    AidaLineEdit::keyPressEvent(event);
}

}
