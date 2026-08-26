#include "qt/disasm/dialogs/disasm_comment_dialog.hpp"

#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_notice.hpp"

#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QVBoxLayout>

namespace aida::qt::disasm::dialogs {

class CommentTextEdit : public QPlainTextEdit {
public:
    explicit CommentTextEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent) {}

    static constexpr int k_max_chars = 4095;

    void insertFromMimeData(const QMimeData* source) override
    {
        if (!source)
            return;
        QString text = source->text();
        const int room = k_max_chars - toPlainText().size() -
            (textCursor().hasSelection()
                ? textCursor().selection().toPlainText().size() : 0);
        if (room <= 0)
            return;
        if (text.size() > room)
            text = text.left(room);
        textCursor().insertText(text);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        const bool paste = event->matches(QKeySequence::Paste);
        QPlainTextEdit::keyPressEvent(event);
        if (!paste && toPlainText().size() > k_max_chars) {
            const QString text = toPlainText().left(k_max_chars);
            const int pos = textCursor().position();
            QSignalBlocker blocker(this);
            setPlainText(text);
            QTextCursor cursor = textCursor();
            cursor.setPosition((std::min)(pos, k_max_chars));
            setTextCursor(cursor);
        }
    }
};

AidaDisasmCommentDialog::AidaDisasmCommentDialog(
    disasm_view::workspace_context_t context, aida::analysis::address_t address,
    QWidget* parent)
    : bridge::AidaDialog(parent), context_(std::move(context)), address_(address)
{
    setObjectName(QStringLiteral("aida.disasm.dialog.comment"));
    setWindowTitle(QStringLiteral("Edit comment"));
    setModal(true);
    auto* layout = new QVBoxLayout(this);
    const auto runtime = disasm_view::runtime_address(context_, address_);
    auto* address_label = new QLabel(QStringLiteral("Address: 0x%1")
        .arg(runtime.value_or(address_.value), 0, 16), this);
    layout->addWidget(address_label);
    editor_ = new CommentTextEdit(this);
    editor_->setObjectName(QStringLiteral("aida.disasm.dialog.comment.input"));
    editor_->setPlainText(QString::fromStdString(disasm_view::comment(context_, address_)));
    const auto& t = theme::tokens();
    editor_->setMinimumSize(static_cast<int>(t.shell.min_panel_w) * 4,
        t.control.input_h * 6);
    layout->addWidget(editor_, 1);
    error_ = new widgets::AidaNotice(QStringLiteral("Comment rejected"), QString(),
        widgets::AidaSemantic::Error, this);
    error_->setObjectName(QStringLiteral("aida.disasm.dialog.comment.error"));
    error_->hide();
    layout->addWidget(error_);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Apply)->setText(QStringLiteral("Apply"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::clicked, this,
        [this](QAbstractButton* button) {
            auto* box = qobject_cast<QDialogButtonBox*>(sender());
            if (box && box->buttonRole(button) == QDialogButtonBox::ApplyRole)
                apply();
            else
                reject();
        });
    editor_->setFocus();
}

void AidaDisasmCommentDialog::keyPressEvent(QKeyEvent* event)
{
    if ((event->modifiers() & Qt::ControlModifier) &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        apply();
        return;
    }
    bridge::AidaDialog::keyPressEvent(event);
}

void AidaDisasmCommentDialog::apply()
{
    const auto text = editor_->toPlainText();
    if (disasm_view::queue_comment(context_, address_, text.toStdString())) {
        accept();
        return;
    }
    error_->setMessage(QStringLiteral("The workspace is unavailable or closing."));
    error_->show();
}

}
