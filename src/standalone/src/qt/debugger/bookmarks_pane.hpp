#pragma once

#include "qt/debugger/debugger_pane_base.hpp"

#include "core/debugger/debugger_engine.hpp"

class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaLineEdit;
}

namespace aida::qt::debugger {

class BookmarksModel;

// Bookmarks pane: add-bar (address + optional label) over the engine's
// anno_mutex bookmarks/labels. Remove runs the review confirm.
class BookmarksPane : public DebuggerPaneBase {
    Q_OBJECT
public:
    explicit BookmarksPane(QWidget* parent = nullptr);

protected:
    void onShown() override;
    void onHidden() override;
    bool hasContentRows() const override;

private:
    void poll();
    void addBookmark();
    void jumpToSelected();

    BookmarksModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    widgets::AidaLineEdit* address_edit_ = nullptr;
    widgets::AidaLineEdit* label_edit_ = nullptr;
    widgets::AidaButton* add_button_ = nullptr;
    QTimer* timer_ = nullptr;
    std::uint64_t last_signature_ = 0;
};

}
