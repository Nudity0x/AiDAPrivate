#include "qt/dialogs/aida_chat_select_dialog.hpp"

#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "qt/bridge/clipboard.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::dialogs {

AidaChatSelectDialog::AidaChatSelectDialog(QWidget* parent)
    : bridge::AidaDialog(parent)
{
    setObjectName(QStringLiteral("aida.chat_select"));
    setWindowTitle(QStringLiteral("Select & Copy Text"));
    setModal(false);

    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
                             t.panel.padding);
    root->setSpacing(t.spacing.sm);

    edit_ = new QPlainTextEdit(this);
    edit_->setObjectName(QStringLiteral("aida.chat_select.text"));
    edit_->setReadOnly(true);
    edit_->setFont(theme::fonts::codeRegular());
    edit_->setPlaceholderText(QStringLiteral("No text to display"));
    root->addWidget(edit_, 1);

    auto* footer = new QHBoxLayout();
    copy_button_ = new QPushButton(QStringLiteral("Copy All"), this);
    copy_button_->setObjectName(QStringLiteral("aida.chat_select.copy_all"));
    copy_button_->setEnabled(false);
    connect(copy_button_, &QPushButton::clicked, this, [this] {
        const QString text = edit_->toPlainText();
        if (text.isEmpty())
            return;
        clipboard::set_text(text);
        chrome::toast_info(QStringLiteral("Message copied to clipboard"), 2.5);
    });
    footer->addWidget(copy_button_);
    footer->addStretch(1);
    close_button_ = new QPushButton(QStringLiteral("Close"), this);
    close_button_->setObjectName(QStringLiteral("aida.chat_select.close"));
    close_button_->setDefault(true);
    connect(close_button_, &QPushButton::clicked, this, [this] { reject(); });
    footer->addWidget(close_button_);
    root->addLayout(footer);

    setMinimumSize(320, 240);
    resize(560, 420);
}

void AidaChatSelectDialog::showText(const QString& text)
{
    edit_->setPlainText(text);
    copy_button_->setEnabled(!text.isEmpty());
    if (!isVisible())
        open();
    else {
        raise();
        activateWindow();
    }
}

AidaChatSelectController::AidaChatSelectController(QObject* parent)
    : QObject(parent)
{
}

void AidaChatSelectController::install(QWidget* dialog_parent)
{
    dialog_parent_ = dialog_parent;
}

void AidaChatSelectController::showText(const QString& text)
{
    if (!dialog_) {
        if (!dialog_parent_)
            return;
        dialog_ = new AidaChatSelectDialog(dialog_parent_);
    }
    dialog_->showText(text);
}

AidaChatSelectController& chatSelectController()
{
    static AidaChatSelectController* controller = [] {
        return new AidaChatSelectController();
    }();
    return *controller;
}

}
