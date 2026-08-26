#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qt/network/network_pane_base.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QScrollArea;
class QSpinBox;
class QTabWidget;
class QTimer;
class QVBoxLayout;

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::net {

class BoundedPlainTextEdit;
class BurpOperationRunner;

// JwtLabView ports jwt_lab_view.cpp (the TU has no external callers and is
// deleted by this wave). Secrets discipline is preserved verbatim: only
// alg/kid/valid/ok/counts reach the logs; the single intentional
// crack_secret_found line stays as the tool's product output. RSA/ECDSA
// verify, forge and the attack batch run off the GUI thread through the
// BurpOperationRunner (EVP work blocks); decode and HMAC verify stay
// GUI-synchronous (bounded <= 8 KiB inputs, unchanged from the legacy path).
class JwtLabView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit JwtLabView(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    struct op_output_t {
        enum class kind_t { none, verify, forge, attack } kind = kind_t::none;
        std::string text;
        std::vector<std::string> candidates;
    };

    void ensureInitialized();
    void decodeToken();
    void clearToken();
    void verifyHmac();
    void submitVerify(bool rsa);
    void submitForge();
    void submitAttack(bool packAll);
    void startCrack();
    void stopCrack();
    void pollCrack();
    void applyOperationOutput(const op_output_t& output);
    void rebuildAttackResults(const std::vector<std::string>& candidates);

    BoundedPlainTextEdit* token_edit_ = nullptr;
    QLabel* decoded_state_label_ = nullptr;
    QLabel* decoded_alg_label_ = nullptr;
    QPlainTextEdit* decoded_header_ = nullptr;
    QPlainTextEdit* decoded_payload_ = nullptr;
    QLineEdit* secret_edit_ = nullptr;
    BoundedPlainTextEdit* rsa_pub_edit_ = nullptr;
    QLabel* verify_result_label_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    BoundedPlainTextEdit* forge_header_ = nullptr;
    BoundedPlainTextEdit* forge_payload_ = nullptr;
    QComboBox* forge_alg_ = nullptr;
    QLineEdit* forge_secret_ = nullptr;
    BoundedPlainTextEdit* forge_rsa_priv_ = nullptr;
    BoundedPlainTextEdit* forge_ecdsa_priv_ = nullptr;
    widgets::AidaButton* forge_button_ = nullptr;
    QPlainTextEdit* forge_output_ = nullptr;
    QLineEdit* wordlist_edit_ = nullptr;
    QSpinBox* crack_concurrency_ = nullptr;
    QSpinBox* crack_max_attempts_ = nullptr;
    widgets::AidaButton* crack_start_button_ = nullptr;
    widgets::AidaButton* crack_stop_button_ = nullptr;
    QLabel* crack_status_label_ = nullptr;
    QLabel* crack_secret_label_ = nullptr;
    QComboBox* attack_choice_ = nullptr;
    QLineEdit* jku_edit_ = nullptr;
    widgets::AidaButton* attack_run_button_ = nullptr;
    widgets::AidaButton* attack_pack_button_ = nullptr;
    QLabel* attack_count_label_ = nullptr;
    QScrollArea* attack_scroll_ = nullptr;
    QWidget* attack_rows_ = nullptr;
    QVBoxLayout* attack_rows_layout_ = nullptr;

    BurpOperationRunner* runner_ = nullptr;
    QTimer* crack_timer_ = nullptr;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> initialization_requested_{false};
    std::uint64_t active_crack_id_ = 0;
    std::atomic<std::uint64_t> started_crack_id_{0};
    std::shared_ptr<const op_output_t> op_output_ =
        std::make_shared<const op_output_t>();
    std::string crack_last_found_;
    std::vector<std::string> attack_candidates_;
};

}
