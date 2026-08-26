#pragma once

#include <QWidget>

class QVBoxLayout;

namespace aida::qt::workbench {

// Start Center (07 sec. 8.6), view.start_center. Quick actions (action-bridge
// dispatched), workspace presets, the Continue section (open sessions + recent
// paths), recovery actions.
class QtStartCenterView : public QWidget {
    Q_OBJECT
public:
    explicit QtStartCenterView(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void rebuild();
    QWidget* buildActionsPanel();
    QWidget* buildPresetsPanel();
    QWidget* buildContinuePanel();
    QWidget* buildRecoveryPanel();

    QVBoxLayout* columns_layout_ = nullptr;
};

}
