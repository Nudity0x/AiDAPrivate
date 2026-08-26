#include "qt/analysis/qt_declaration_review_dialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/disasm/disasm_view.hpp"

#include "qt/widgets/aida_notice.hpp"

namespace aida::qt::analysis {

QtDeclarationReviewDialog::QtDeclarationReviewDialog(
    std::shared_ptr<aida::analysis::analysis_workspace_t> workspace,
    std::shared_ptr<const aida::analysis::analysis_publication_t> publication,
    std::uint64_t generation, std::uint64_t analysis_revision,
    std::uint64_t overlay_revision, QString title, QString heading,
    std::string declaration, QString confirm_label,
    std::function<bool(const std::string& declaration)> commit, QWidget* parent)
    : QDialog(parent), workspace_(std::move(workspace)),
      publication_(std::move(publication)), generation_(generation),
      analysis_revision_(analysis_revision), overlay_revision_(overlay_revision),
      declaration_(std::move(declaration)), commit_(std::move(commit)) {
    setObjectName(QStringLiteral("aida.dialog.types.declaration_review"));
    setWindowTitle(title);
    setModal(true);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 560);
    setMinimumSize(460, 320);
    auto* layout = new QVBoxLayout(this);
    auto* heading_label = new QLabel(heading, this);
    layout->addWidget(heading_label);
    auto* revisions = new QLabel(QStringLiteral(
        "Workspace generation %1  analysis revision %2  overlay revision %3")
        .arg(generation_).arg(analysis_revision_).arg(overlay_revision_), this);
    layout->addWidget(revisions);
    auto* text = new QPlainTextEdit(this);
    text->setReadOnly(true);
    text->setPlainText(QString::fromStdString(declaration_));
    layout->addWidget(text, 1);
    stale_notice_ = new widgets::AidaNotice(this);
    stale_notice_->setTitle(QStringLiteral("Review is stale"));
    stale_notice_->setMessage(QStringLiteral(
        "The workspace, analysis, or overlay revision changed. Cancel and select the type again."));
    stale_notice_->setVisible(false);
    layout->addWidget(stale_notice_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this);
    buttons_->button(QDialogButtonBox::Ok)->setText(confirm_label);
    layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        if (!isCurrent()) return;
        if (commit_ && commit_(declaration_)) accept();
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, [this] { reject(); });
    refreshStaleness();
}

bool QtDeclarationReviewDialog::isCurrent() const {
    const auto workspace = workspace_.lock();
    const auto context = disasm_view::capture_workspace(workspace);
    return !declaration_.empty() && workspace && !workspace->closing() &&
        !workspace->closed() && publication_ && context.publication == publication_ &&
        workspace->generation() == generation_ &&
        publication_->analysis_revision == analysis_revision_ &&
        workspace->overlay_revision() == overlay_revision_;
}

void QtDeclarationReviewDialog::refreshStaleness() {
    const bool current = isCurrent();
    stale_notice_->setVisible(!current);
    if (buttons_)
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(current);
}

}
