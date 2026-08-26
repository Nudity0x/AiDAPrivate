#pragma once

#include <QDialog>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace aida::analysis {
class analysis_workspace_t;
struct analysis_publication_t;
}

namespace aida::qt::widgets {
class AidaNotice;
}

class QDialogButtonBox;
class QPlainTextEdit;
class QLabel;

namespace aida::qt::analysis {

// Shared "Review Type Declaration" modal (07 sec. 6.1/sec. 6.2/sec. 6.4): presents the
// bounded declaration + revision stamps, blocks Commit when the workspace/
// analysis/overlay revision no longer matches, and invokes the commit callback
// only while current. open() + WA_DeleteOnClose, never exec() (S7).
class QtDeclarationReviewDialog : public QDialog {
    Q_OBJECT
public:
    QtDeclarationReviewDialog(
        std::shared_ptr<aida::analysis::analysis_workspace_t> workspace,
        std::shared_ptr<const aida::analysis::analysis_publication_t> publication,
        std::uint64_t generation, std::uint64_t analysis_revision,
        std::uint64_t overlay_revision, QString title, QString heading,
        std::string declaration, QString confirm_label,
        std::function<bool(const std::string& declaration)> commit,
        QWidget* parent = nullptr);

private:
    bool isCurrent() const;
    void refreshStaleness();

    std::weak_ptr<aida::analysis::analysis_workspace_t> workspace_;
    std::shared_ptr<const aida::analysis::analysis_publication_t> publication_;
    std::uint64_t generation_ = 0;
    std::uint64_t analysis_revision_ = 0;
    std::uint64_t overlay_revision_ = 0;
    std::string declaration_;
    std::function<bool(const std::string&)> commit_;
    widgets::AidaNotice* stale_notice_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

}
