#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QVariant>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/network/burp/ws_editor.hpp"
#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::net {

class QtByteCappedPlainTextEdit;

// Byte-verbatim port of ws_editor_view::resolve_retained_artifact: the backend
// frame query is unchanged; only the view state dissolved.
bool wsResolveRetainedFrame(std::uint64_t connection_id, std::uint64_t frame_id,
                            std::vector<std::uint8_t>& bytes,
                            std::string& unavailable_reason);

network_view::artifact_identity_t wsFrameArtifactIdentity(
    std::uint64_t connection_id, const aida::burp::ws_editor::ws_frame_log_t& frame);

class QtWsConnectionModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Id = 0, Url, Sent, Recv, Error, ColumnCount };

    explicit QtWsConnectionModel(QObject* parent = nullptr);

    void adopt(std::vector<aida::burp::ws_editor::ws_status_t> rows);
    const aida::burp::ws_editor::ws_status_t* rowAt(int row) const noexcept;
    int rowForConnectionId(std::uint64_t id) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    std::vector<aida::burp::ws_editor::ws_status_t> rows_;
};

class QtWsFrameModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Dir = 0, Op, Len, Time, Preview, ColumnCount };

    explicit QtWsFrameModel(QObject* parent = nullptr);

    void refresh(std::uint64_t connectionId);
    void clearFrames();
    const aida::burp::ws_editor::ws_frame_log_t* rowAt(int row) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    std::uint64_t connection_id_ = 0;
    std::vector<aida::burp::ws_editor::ws_frame_log_t> rows_;
};

// QtWsEditorView ports burp/ws_editor_view.cpp. The frame log refreshes from a
// GUI-thread QTimer (500 ms) while the pane is visible; the ws registry reads
// are in-memory under the backend mutex (no I/O). Connect/disconnect/send run
// on executor workers; the connect result lands via queued invokeMethod.
class QtWsEditorView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit QtWsEditorView(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void connectNow();
    void refreshConnections();
    void refreshFrames();
    void onConnectionSelected();
    void showFrameContext(const QPoint& viewportPos);
    void frameContextForIndex(const QModelIndex& index, const QPoint& globalPos,
                              aida::ui::context_menu_open_origin_t origin);
    void sendNow();
    void disconnectSelected();
    void clearSelected();
    void pingSelected();
    void closeSelected();

    QComboBox* schemeCombo_ = nullptr;
    QLineEdit* hostEdit_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QCheckBox* verifyTlsCheck_ = nullptr;
    widgets::AidaButton* connectButton_ = nullptr;
    QLineEdit* originEdit_ = nullptr;
    QLineEdit* subprotocolEdit_ = nullptr;
    QtByteCappedPlainTextEdit* headersEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* connectionsHeader_ = nullptr;
    QTableView* connectionsView_ = nullptr;
    QtWsConnectionModel* connectionModel_ = nullptr;
    QLabel* selectedLabel_ = nullptr;
    widgets::AidaButton* disconnectButton_ = nullptr;
    widgets::AidaButton* clearButton_ = nullptr;
    widgets::AidaButton* pingButton_ = nullptr;
    widgets::AidaButton* closeButton_ = nullptr;
    QTableView* framesView_ = nullptr;
    QtWsFrameModel* frameModel_ = nullptr;
    QComboBox* composeModeCombo_ = nullptr;
    QSpinBox* composeOpcodeSpin_ = nullptr;
    QCheckBox* composeFinCheck_ = nullptr;
    QCheckBox* composeMaskedCheck_ = nullptr;
    QStackedLayout* composeStack_ = nullptr;
    QtByteCappedPlainTextEdit* composeTextEdit_ = nullptr;
    QtByteCappedPlainTextEdit* composeHexEdit_ = nullptr;
    widgets::AidaButton* sendButton_ = nullptr;
    QWidget* rightContent_ = nullptr;
    QWidget* emptyState_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    std::uint64_t selectedConnectionId_ = 0;
    std::uint64_t selectedFrameId_ = 0;
    QString last_action_;
    QString last_action_kind_;
};

}
