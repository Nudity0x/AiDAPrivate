#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/network/burp/h2_editor.hpp"
#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QStackedLayout;
class QVBoxLayout;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class QtByteCappedPlainTextEdit;

// Retained HTTP/2 exchange snapshot (process-lifetime store, atomically
// published; the ImGui view kept the same data in a TU static). Mutated only
// on the GUI thread; resolution is lock-free from any thread.
struct QtH2RetainedSnapshot {
    std::uint64_t exchange_id = 0;
    std::uint64_t generation = 0;
    std::vector<std::uint8_t> request;
    std::vector<std::uint8_t> response;
    std::string host;
    std::uint16_t port = 0;
    bool tls = true;
    bool raw_protocol = false;
    std::uint64_t request_hash = 0;
    std::uint64_t response_hash = 0;
    std::size_t request_size = 0;
    std::size_t response_size = 0;
    bool has_response = false;
    aida::burp::h2_editor::response_t last_response;
};

class QtH2EditorController : public QObject {
    Q_OBJECT
public:
    explicit QtH2EditorController(QObject* parent = nullptr);

    void initialize();
    void shutdown();

    bool inFlight() const;
    std::shared_ptr<const QtH2RetainedSnapshot> retained() const;
    network_view::artifact_identity_t artifactIdentity(bool response) const;

    void sendRequest(const aida::burp::h2_editor::request_t& request);

    static bool resolveRetainedArtifact(std::uint64_t exchangeId, std::uint64_t generation,
                                        bool response, std::vector<std::uint8_t>& bytes,
                                        std::string& unavailable_reason);

Q_SIGNALS:
    void responseChanged();
    void inFlightChanged(bool inFlight);

private:
    void applySendResult(std::uint64_t exchangeId, std::uint64_t lifetimeGeneration,
                         const aida::burp::h2_editor::response_t& response);
};

// QtH2EditorView ports burp/h2_editor_view.cpp: two-pane splitter (edit |
// response), structured or raw-frames composition, one in-flight send gated by
// the controller. The send worker runs h2_editor::send off-GUI; the result is
// applied in a queued slot fenced by lifetime_generation + accepting.
class QtH2EditorView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit QtH2EditorView(QWidget* parent = nullptr);
    ~QtH2EditorView() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void addHeaderRow(const QString& name, const QString& value);
    void sendNow();
    void refreshResponse();
    void refreshBusy();
    void showResponseContext(const QPoint& globalPos,
                             aida::ui::context_menu_open_origin_t origin);

    QtH2EditorController* controller_ = nullptr;
    QLineEdit* hostEdit_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QSpinBox* timeoutSpin_ = nullptr;
    QCheckBox* rawModeCheck_ = nullptr;
    QStackedLayout* editStack_ = nullptr;
    QLineEdit* methodEdit_ = nullptr;
    QLineEdit* schemeEdit_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QLineEdit* authorityEdit_ = nullptr;
    QCheckBox* endStreamCheck_ = nullptr;
    QCheckBox* endHeadersCheck_ = nullptr;
    QCheckBox* paddedCheck_ = nullptr;
    QCheckBox* priorityCheck_ = nullptr;
    QLineEdit* headerNameEdit_ = nullptr;
    QLineEdit* headerValueEdit_ = nullptr;
    widgets::AidaButton* headerAddButton_ = nullptr;
    QWidget* headerRowsHost_ = nullptr;
    QVBoxLayout* headerRowsLayout_ = nullptr;
    QtByteCappedPlainTextEdit* bodyEdit_ = nullptr;
    QtByteCappedPlainTextEdit* rawHexEdit_ = nullptr;
    QLabel* hexErrorLabel_ = nullptr;
    widgets::AidaButton* sendButton_ = nullptr;
    QLabel* sendingLabel_ = nullptr;
    widgets::AidaButton* exchangeActionsButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QPlainTextEdit* responseHeadersView_ = nullptr;
    QLabel* responseBodyHeader_ = nullptr;
    QPlainTextEdit* responseBodyView_ = nullptr;
    QPlainTextEdit* rawWireView_ = nullptr;
    widgets::AidaStateView* noResponseState_ = nullptr;
    QWidget* responseContent_ = nullptr;
    QWidget* responsePane_ = nullptr;
    std::vector<std::pair<std::string, std::string>> headers_;
};

}
