#pragma once

#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

#include "qt/bridge/aida_dialog.hpp"

class QLabel;
class QPushButton;
class QTimer;

namespace aida::qt::net {

class BurpOperationRunner;

// BurpReviewDialog replaces the design::begin_dialog_exact review popups
// (intruder clear, sequencer delete, collaborator stop/clear, match_replace
// delete/clear, comparer remove). It is shown with open() (async,
// window-modal; qdialog.cpp:458-526), never exec() (qdialog.cpp:539-575), and
// closes itself with WA_DeleteOnClose. A 250ms QTimer (Coarse default,
// qtimer.cpp:674-677) re-evaluates the revalidator: the confirm button is
// enabled iff the revalidator passes and the bound runner is not pending;
// the stale reason is shown inline (mirrors "changed after review; cancel and
// select again", sequencer_view.cpp:576-577). Esc takes the default reject
// path (qdialog.cpp:696-741).
class BurpReviewDialog : public aida::qt::bridge::AidaDialog {
    Q_OBJECT
public:
    using revalidator_t = std::function<bool(QString& reasonOut)>;

    BurpReviewDialog(const QString& title, const QStringList& bodyLines,
                     const QString& confirmLabel, bool destructive,
                     QWidget* parent = nullptr);

    void setRevalidator(revalidator_t fn);
    void setRunner(BurpOperationRunner* runner);
    void setSubmitCallback(std::function<void()> fn);
    void reevaluate();

    void accept() override;

private:
    void buildUi(const QStringList& bodyLines, const QString& confirmLabel,
                 bool destructive);

    revalidator_t revalidator_;
    std::function<void()> submit_;
    QPointer<BurpOperationRunner> runner_;
    QTimer* revalidate_timer_ = nullptr;
    QLabel* stale_label_ = nullptr;
    QPushButton* confirm_button_ = nullptr;
    QString stale_reason_;
};

}
