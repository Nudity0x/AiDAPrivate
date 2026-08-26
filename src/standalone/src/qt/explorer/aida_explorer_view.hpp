#pragma once

#include <QWidget>

#include <cstdint>
#include <string>
#include <vector>

class QFrame;
class QLabel;
class QListView;
class QModelIndex;

namespace aida::ui {
enum class context_menu_open_origin_t : std::uint8_t;
}

namespace aida::qt::documents {
class AidaDocumentModel;
}

namespace aida::qt::widgets {
class AidaLineEdit;
class AidaStateView;
}

namespace aida::qt::explorer {

class AidaExplorerModel;
class AidaExplorerDelegate;

class AidaExplorerView : public QWidget {
    Q_OBJECT
public:
    explicit AidaExplorerView(QWidget* parent = nullptr);
    ~AidaExplorerView() override;

    void setDocumentModel(documents::AidaDocumentModel* model);
    AidaExplorerModel* model() const noexcept { return model_; }

Q_SIGNALS:
    void openFolderRequested();
    void searchRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
    void onActivated(const QModelIndex& index);
    void onFilterChanged(const QString& text);
    void onIndexingStateChanged();
    void onCustomContextMenu(const QPoint& pos);
    void showNameDialog(int operation, const std::string& source, bool directory,
                        const std::string& proposed);
    void showDeleteDialog(const std::vector<std::pair<std::string, bool>>& targets);

private:
    enum class EmptyAction : std::uint8_t { None, OpenFolder, ClearFilter };

    void rebuildHeader();
    void applyRootLabel();
    void refreshStateStrip();
    void applyStateLabel();
    void refreshEmptyState();
    void applyContentWidth();
    void syncBackendSelectionFromView();
    void applyBackendSelectionToView();
    void requestFileOperation(int operation, const std::string& path, bool directory);
    void openRowContextMenu(int row, const QPoint& global_pos,
                            aida::ui::context_menu_open_origin_t origin);
    void openEmptyContextMenu(const QPoint& global_pos,
                              aida::ui::context_menu_open_origin_t origin);
    int selectedSourceIndex() const;
    std::vector<std::pair<std::string, bool>> selectedTargets() const;

    AidaExplorerModel* model_ = nullptr;
    AidaExplorerDelegate* delegate_ = nullptr;
    QListView* list_ = nullptr;
    QLabel* root_label_ = nullptr;
    widgets::AidaLineEdit* filter_edit_ = nullptr;
    QFrame* state_strip_ = nullptr;
    QLabel* state_label_ = nullptr;
    widgets::AidaStateView* empty_state_ = nullptr;
    QString root_full_text_;
    QString state_full_text_;
    EmptyAction empty_action_ = EmptyAction::None;
    bool applying_backend_selection_ = false;
};

}
