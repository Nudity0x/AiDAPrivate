#pragma once

#include <QModelIndex>
#include <QVariant>

#include <memory>
#include <string>

#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QLabel;
class QPushButton;
class QSplitter;
class QStackedLayout;
class QTableView;
class QTimer;
class QVariantAnimation;
class QWidget;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
class AidaToggleSwitch;
}

namespace aida::qt::net {

class QtHumanRequestEditor;

class InterceptModel : public SnapshotTableModel<mitm_proxy::http_exchange> {
public:
    enum Column { Id = 0, Method, Host, Path, Size, ColumnCount };

    explicit InterceptModel(QObject* parent = nullptr);

    int rowForExchangeId(std::uint64_t id) const noexcept;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const mitm_proxy::http_exchange& row, int column, int role) const override;
};

// InterceptDropReviewDialog ports the ImGui "Review Intercept Drop" popup:
// confirm stays enabled only while the retained publication is still current;
// accept re-enters the byte-verbatim reviewed drop path
// (network_view::confirm_intercept_drop_review re-fetches live state and
// compares identities exactly as execute_reviewed_intercept_command did).
class InterceptDropReviewDialog : public aida::qt::bridge::AidaDialog {
    Q_OBJECT
public:
    InterceptDropReviewDialog(network_view::intercept_drop_review_t review,
                              QWidget* parent = nullptr);

private:
    network_view::intercept_drop_review_t review_;
    QLabel* bodyLabel_ = nullptr;
    QPushButton* confirmButton_ = nullptr;
};

class InterceptPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit InterceptPane(QWidget* parent = nullptr);
    ~InterceptPane() override;

    static void installDropReviewDisplay();
    static void presentDropReview(const network_view::intercept_drop_review_t& review);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onSnapshot(std::shared_ptr<const network_view::intercept_runtime_snapshot_t> snapshot);
    void onSelectionChanged();
    void refreshCommandButtons();
    void retainDraftForSelection();
    void syncEditorsToSelection();
    void onModifiedAuthorityChanged();
    void syncEditorMirror();
    void updateEmptyState();
    void showContextForRow(int row, const QPoint& globalPos,
                           aida::ui::context_menu_open_origin_t origin);

    InterceptModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* tableStack_ = nullptr;
    widgets::AidaStateView* emptyView_ = nullptr;
    widgets::AidaToggleSwitch* enableToggle_ = nullptr;
    widgets::AidaButton* forwardAllButton_ = nullptr;
    widgets::AidaButton* dropAllButton_ = nullptr;
    widgets::AidaButton* forwardButton_ = nullptr;
    widgets::AidaButton* dropButton_ = nullptr;
    widgets::AidaButton* forwardModifiedButton_ = nullptr;
    widgets::AidaButton* sendRepeaterButton_ = nullptr;
    widgets::AidaButton* sendFuzzerButton_ = nullptr;
    QLabel* heldLabel_ = nullptr;
    QLabel* interceptingPill_ = nullptr;
    QSplitter* editorSplitter_ = nullptr;
    QtHumanRequestEditor* originalEditor_ = nullptr;
    QtHumanRequestEditor* modifiedEditor_ = nullptr;
    QLabel* originalBytesLabel_ = nullptr;
    QLabel* modifiedNoticeLabel_ = nullptr;
    QLabel* modifiedUnavailableLabel_ = nullptr;
    QTimer* snapshotTimer_ = nullptr;
    QVariantAnimation* flashAnimation_ = nullptr;
    std::uint64_t selectedExchangeId_ = 0;
    std::uint64_t currentGeneration_ = 0;
    bool running_ = false;
    bool enabled_ = false;
    int prevHeldCount_ = 0;
};

}
