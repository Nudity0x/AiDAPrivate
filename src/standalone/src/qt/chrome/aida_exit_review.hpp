#pragma once

#include <QObject>
#include <QString>

#include <atomic>

#include "qt/bridge/aida_dialog.hpp"
#include "qt/chrome/aida_legacy_chrome_bridge.hpp"

class QHBoxLayout;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

namespace aida::qt {
class AidaMainWindow;
}

namespace aida::qt::chrome {

class AidaDirtyCloseDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    explicit AidaDirtyCloseDialog(QWidget* parent = nullptr);

    void showFor(const exit_review_snapshot_t& snapshot);

Q_SIGNALS:
    void saveRequested();
    void discardRequested();
    void cancelRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuildButtons();
    void updateFromSnapshot(const exit_review_snapshot_t& snapshot);

    QLabel* message_ = nullptr;
    QLabel* notice_ = nullptr;
    QLabel* queue_header_ = nullptr;
    QListWidget* queue_list_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* discard_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QHBoxLayout* buttons_ = nullptr;
    std::uint64_t document_id_ = 0;
};

class AidaExitReviewController : public QObject {
    Q_OBJECT
public:
    AidaExitReviewController(AidaMainWindow* window, QObject* parent = nullptr);
    ~AidaExitReviewController() override;

    bool gateHook();
    void onSessionAbort();

private:
    void tick();

    AidaMainWindow* window_ = nullptr;
    AidaDirtyCloseDialog* dialog_ = nullptr;
    QTimer* timer_ = nullptr;
    exit_review_snapshot_t last_snapshot_;
    std::atomic<bool> close_recommitted_{false};
};

}
