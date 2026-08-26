#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>
#include <string>

#include "qt/analysis/qt_types_catalog_model.hpp"

class QAbstractTableModel;
class QLabel;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaLineEdit;
class AidaSearchField;
class AidaStateView;
}

namespace aida::qt::analysis {

class QtWorkspaceContext;
class QtTypeDeclarationReviewDialog;

// One tab of the types catalog (07 sec. 6.1): structures/unions/enums/typedefs/
// functions. The five hub pages instantiate this with different tab configs.
class QtTypesCatalogView : public QWidget {
    Q_OBJECT
public:
    QtTypesCatalogView(qt_types_tab_t tab, QWidget* parent = nullptr);

    // stage_type_application engine hook body (07 sec. 6.1, verbatim semantics).
    bool stageTypeApplication(std::uint64_t runtime_address, std::string& error);

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebindContext(QtWorkspaceContext* context);
    void requestCatalog();
    void requestVisible();
    void refreshPresentation();
    void showRowMenu(const QPoint& global_pos, int view_row);
    void showDetailFor(int view_row);
    void publishSelection(int view_row);
    void openDeclarationReview(const std::string& name, std::string declaration);
    void applyType();

    qt_types_tab_t tab_;
    QtWorkspaceContext* context_ = nullptr;
    std::shared_ptr<QtTypesHubState> state_;
    QtTypesCatalogModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    widgets::AidaSearchField* search_ = nullptr;
    widgets::AidaStateView* state_view_ = nullptr;
    widgets::AidaLineEdit* apply_address_ = nullptr;
    widgets::AidaLineEdit* apply_type_ = nullptr;
    QLabel* apply_status_ = nullptr;
    QLabel* error_label_ = nullptr;
    QLabel* detail_title_ = nullptr;
    QLabel* detail_subtitle_ = nullptr;
    QWidget* detail_host_ = nullptr;
    QTableView* detail_members_ = nullptr;
    QAbstractTableModel* detail_members_model_ = nullptr;
    QTableView* references_table_ = nullptr;
    QAbstractTableModel* references_model_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QProgressBar* pdb_progress_ = nullptr;
    QPushButton* pdb_cancel_ = nullptr;
    QPushButton* load_pdb_ = nullptr;
    QLabel* workspace_label_ = nullptr;
    QTimer* filter_debounce_ = nullptr;
    QTimer* apply_watch_ = nullptr;
    QMetaObject::Connection poller_connection_;
    QMetaObject::Connection context_connection_;
};

}
