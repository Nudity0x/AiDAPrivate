#include "qt/network/jwt_lab/jwt_lab_view.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

#include "core/infra/executor.hpp"
#include "core/network/burp/jwt_lab.hpp"
#include "helpers/diag_log.hpp"
#include "qt/network/bounded_plain_text_edit.hpp"
#include "qt/network/burp_operation.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"

namespace aida::qt::net {

JwtLabView::JwtLabView(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.jwt_lab"));
    const auto& t = theme::tokens();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* header = new QLabel(QStringLiteral("JWT Lab"), content);
    header->setProperty("aidaTone", QStringLiteral("titleAccent"));
    layout->addWidget(header);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);

    auto* leftPanel = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(t.spacing.sm);
    auto* tokenTitle = new QLabel(QStringLiteral("Token"), leftPanel);
    tokenTitle->setProperty("aidaTone", QStringLiteral("secondary"));
    leftLayout->addWidget(tokenTitle);
    token_edit_ = new BoundedPlainTextEdit(8191, leftPanel);
    token_edit_->setFont(theme::fonts::codeRegular());
    token_edit_->setMinimumHeight(editor_min_height_lines(token_edit_, 4));
    leftLayout->addWidget(token_edit_);
    auto* tokenButtons = new QHBoxLayout();
    tokenButtons->setSpacing(t.spacing.sm);
    auto* decodeButton = new widgets::AidaButton(QStringLiteral("Decode"), leftPanel);
    decodeButton->setKind(widgets::AidaButton::Kind::Primary);
    decodeButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    tokenButtons->addWidget(decodeButton);
    auto* clearButton = new widgets::AidaButton(QStringLiteral("Clear"), leftPanel);
    clearButton->setKind(widgets::AidaButton::Kind::Ghost);
    clearButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    tokenButtons->addWidget(clearButton);
    tokenButtons->addStretch(1);
    leftLayout->addLayout(tokenButtons);

    decoded_state_label_ = new QLabel(QStringLiteral("Decoded"), leftPanel);
    decoded_state_label_->setProperty("aidaTone", QStringLiteral("secondary"));
    leftLayout->addWidget(decoded_state_label_);
    decoded_alg_label_ = new QLabel(leftPanel);
    decoded_alg_label_->setProperty("aidaTone", QStringLiteral("dim"));
    leftLayout->addWidget(decoded_alg_label_);
    auto* headerTitle = new QLabel(QStringLiteral("Header"), leftPanel);
    headerTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    leftLayout->addWidget(headerTitle);
    decoded_header_ = new QPlainTextEdit(leftPanel);
    decoded_header_->setReadOnly(true);
    decoded_header_->setFont(theme::fonts::codeRegular());
    decoded_header_->setMinimumHeight(editor_min_height_lines(decoded_header_, 4));
    leftLayout->addWidget(decoded_header_);
    auto* payloadTitle = new QLabel(QStringLiteral("Payload"), leftPanel);
    payloadTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    leftLayout->addWidget(payloadTitle);
    decoded_payload_ = new QPlainTextEdit(leftPanel);
    decoded_payload_->setReadOnly(true);
    decoded_payload_->setFont(theme::fonts::codeRegular());
    decoded_payload_->setMinimumHeight(editor_min_height_lines(decoded_payload_, 5));
    leftLayout->addWidget(decoded_payload_);

    auto* hmacTitle = new QLabel(QStringLiteral("HMAC verify"), leftPanel);
    hmacTitle->setProperty("aidaTone", QStringLiteral("secondary"));
    leftLayout->addWidget(hmacTitle);
    auto* hmacRow = new QHBoxLayout();
    hmacRow->setSpacing(t.spacing.sm);
    secret_edit_ = new QLineEdit(leftPanel);
    secret_edit_->setMaxLength(1023);
    hmacRow->addWidget(secret_edit_, 1);
    auto* hmacButton = new widgets::AidaButton(QStringLiteral("Verify HMAC"), leftPanel);
    hmacButton->setKind(widgets::AidaButton::Kind::Secondary);
    hmacButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    hmacRow->addWidget(hmacButton);
    leftLayout->addLayout(hmacRow);

    auto* rsaTitle = new QLabel(QStringLiteral("RSA public PEM (for verify or alg-confusion)"),
        leftPanel);
    rsaTitle->setProperty("aidaTone", QStringLiteral("secondary"));
    leftLayout->addWidget(rsaTitle);
    rsa_pub_edit_ = new BoundedPlainTextEdit(8191, leftPanel);
    rsa_pub_edit_->setFont(theme::fonts::codeRegular());
    rsa_pub_edit_->setMinimumHeight(editor_min_height_lines(rsa_pub_edit_, 4));
    leftLayout->addWidget(rsa_pub_edit_);
    auto* rsaRow = new QHBoxLayout();
    rsaRow->setSpacing(t.spacing.sm);
    auto* rsaButton = new widgets::AidaButton(QStringLiteral("Verify RSA"), leftPanel);
    rsaButton->setKind(widgets::AidaButton::Kind::Secondary);
    rsaButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    rsaRow->addWidget(rsaButton);
    auto* ecdsaButton = new widgets::AidaButton(QStringLiteral("Verify ECDSA"), leftPanel);
    ecdsaButton->setKind(widgets::AidaButton::Kind::Secondary);
    ecdsaButton->setControlSize(widgets::AidaButton::ControlSize::Small);
    rsaRow->addWidget(ecdsaButton);
    rsaRow->addStretch(1);
    leftLayout->addLayout(rsaRow);
    verify_result_label_ = new QLabel(leftPanel);
    verify_result_label_->setProperty("aidaTone", QStringLiteral("dim"));
    leftLayout->addWidget(verify_result_label_);
    leftLayout->addStretch(1);
    splitter->addWidget(leftPanel);

    tabs_ = new QTabWidget(splitter);

    auto* forgeTab = new QWidget(tabs_);
    auto* forgeLayout = new QVBoxLayout(forgeTab);
    forgeLayout->setSpacing(t.spacing.sm);
    forgeLayout->addWidget(new QLabel(QStringLiteral("Header JSON"), forgeTab));
    forge_header_ = new BoundedPlainTextEdit(2047, forgeTab);
    forge_header_->setFont(theme::fonts::codeRegular());
    forge_header_->setMinimumHeight(editor_min_height_lines(forge_header_, 4));
    forgeLayout->addWidget(forge_header_);
    forgeLayout->addWidget(new QLabel(QStringLiteral("Payload JSON"), forgeTab));
    forge_payload_ = new BoundedPlainTextEdit(4095, forgeTab);
    forge_payload_->setFont(theme::fonts::codeRegular());
    forge_payload_->setMinimumHeight(editor_min_height_lines(forge_payload_, 5));
    forgeLayout->addWidget(forge_payload_);
    auto* algRow = new QHBoxLayout();
    algRow->setSpacing(t.spacing.sm);
    algRow->addWidget(new QLabel(QStringLiteral("alg"), forgeTab));
    forge_alg_ = new QComboBox(forgeTab);
    forge_alg_->setEditable(true);
    forge_alg_->addItems({QStringLiteral("none"), QStringLiteral("HS256"),
        QStringLiteral("HS384"), QStringLiteral("HS512"), QStringLiteral("RS256"),
        QStringLiteral("RS384"), QStringLiteral("RS512"), QStringLiteral("ES256"),
        QStringLiteral("ES384"), QStringLiteral("ES512")});
    forge_alg_->setCurrentText(QStringLiteral("HS256"));
    if (auto* algEdit = forge_alg_->lineEdit())
        algEdit->setMaxLength(31);
    algRow->addWidget(forge_alg_);
    algRow->addStretch(1);
    forgeLayout->addLayout(algRow);
    auto* supportedHint = new QLabel(QStringLiteral(
        "Supported: none, HS256, HS384, HS512, RS256, RS384, RS512, ES256, ES384, ES512"),
        forgeTab);
    supportedHint->setProperty("aidaTone", QStringLiteral("dim"));
    forgeLayout->addWidget(supportedHint);
    forgeLayout->addWidget(new QLabel(QStringLiteral("HMAC secret"), forgeTab));
    forge_secret_ = new QLineEdit(forgeTab);
    forge_secret_->setMaxLength(1023);
    forgeLayout->addWidget(forge_secret_);
    forgeLayout->addWidget(new QLabel(QStringLiteral("RSA private PEM"), forgeTab));
    forge_rsa_priv_ = new BoundedPlainTextEdit(8191, forgeTab);
    forge_rsa_priv_->setFont(theme::fonts::codeRegular());
    forge_rsa_priv_->setMinimumHeight(editor_min_height_lines(forge_rsa_priv_, 3));
    forgeLayout->addWidget(forge_rsa_priv_);
    forgeLayout->addWidget(new QLabel(QStringLiteral("ECDSA private PEM"), forgeTab));
    forge_ecdsa_priv_ = new BoundedPlainTextEdit(8191, forgeTab);
    forge_ecdsa_priv_->setFont(theme::fonts::codeRegular());
    forge_ecdsa_priv_->setMinimumHeight(editor_min_height_lines(forge_ecdsa_priv_, 3));
    forgeLayout->addWidget(forge_ecdsa_priv_);
    auto* forgeButtons = new QHBoxLayout();
    forgeButtons->setSpacing(t.spacing.sm);
    forge_button_ = new widgets::AidaButton(QStringLiteral("Forge"), forgeTab);
    forge_button_->setKind(widgets::AidaButton::Kind::Primary);
    forge_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    forgeButtons->addWidget(forge_button_);
    auto* copyToToken = new widgets::AidaButton(QStringLiteral("Copy to token"), forgeTab);
    copyToToken->setKind(widgets::AidaButton::Kind::Ghost);
    copyToToken->setControlSize(widgets::AidaButton::ControlSize::Small);
    forgeButtons->addWidget(copyToToken);
    forgeButtons->addStretch(1);
    forgeLayout->addLayout(forgeButtons);
    forgeLayout->addWidget(new QLabel(QStringLiteral("Forged token"), forgeTab));
    forge_output_ = new QPlainTextEdit(forgeTab);
    forge_output_->setReadOnly(true);
    forge_output_->setFont(theme::fonts::codeRegular());
    forge_output_->setMinimumHeight(editor_min_height_lines(forge_output_, 4));
    forge_output_->setPlaceholderText(QStringLiteral("Forge a token to see the signed result"));
    forgeLayout->addWidget(forge_output_, 1);
    tabs_->addTab(forgeTab, QStringLiteral("Forge"));

    auto* crackTab = new QWidget(tabs_);
    auto* crackLayout = new QVBoxLayout(crackTab);
    crackLayout->setSpacing(t.spacing.sm);
    crackLayout->addWidget(new QLabel(QStringLiteral("Wordlist id"), crackTab));
    wordlist_edit_ = new QLineEdit(QStringLiteral("common_passwords"), crackTab);
    wordlist_edit_->setMaxLength(127);
    crackLayout->addWidget(wordlist_edit_);
    auto* crackRow = new QHBoxLayout();
    crackRow->setSpacing(t.spacing.sm);
    crackRow->addWidget(new QLabel(QStringLiteral("Concurrency"), crackTab));
    crack_concurrency_ = new QSpinBox(crackTab);
    crack_concurrency_->setRange(1, 32);
    crack_concurrency_->setValue(8);
    crackRow->addWidget(crack_concurrency_);
    crackRow->addWidget(new QLabel(QStringLiteral("Max attempts"), crackTab));
    crack_max_attempts_ = new QSpinBox(crackTab);
    crack_max_attempts_->setRange(1, 1000000000);
    crack_max_attempts_->setValue(1000000);
    crackRow->addWidget(crack_max_attempts_);
    crackRow->addStretch(1);
    crackLayout->addLayout(crackRow);
    auto* crackButtons = new QHBoxLayout();
    crackButtons->setSpacing(t.spacing.sm);
    crack_start_button_ = new widgets::AidaButton(QStringLiteral("Start crack"), crackTab);
    crack_start_button_->setKind(widgets::AidaButton::Kind::Primary);
    crack_start_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    crackButtons->addWidget(crack_start_button_);
    crack_stop_button_ = new widgets::AidaButton(QStringLiteral("Stop"), crackTab);
    crack_stop_button_->setKind(widgets::AidaButton::Kind::Destructive);
    crack_stop_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    crackButtons->addWidget(crack_stop_button_);
    crackButtons->addStretch(1);
    crackLayout->addLayout(crackButtons);
    crack_status_label_ = new QLabel(crackTab);
    crack_status_label_->setProperty("aidaTone", QStringLiteral("dim"));
    crackLayout->addWidget(crack_status_label_);
    crack_secret_label_ = new QLabel(crackTab);
    crack_secret_label_->setProperty("aidaTone", QStringLiteral("titleSuccess"));
    crack_secret_label_->setVisible(false);
    crackLayout->addWidget(crack_secret_label_);
    crackLayout->addStretch(1);
    tabs_->addTab(crackTab, QStringLiteral("Crack"));

    auto* attackTab = new QWidget(tabs_);
    auto* attackLayout = new QVBoxLayout(attackTab);
    attackLayout->setSpacing(t.spacing.sm);
    auto* attackRow = new QHBoxLayout();
    attackRow->setSpacing(t.spacing.sm);
    attack_choice_ = new QComboBox(attackTab);
    attack_choice_->addItems({QStringLiteral("alg=none"), QStringLiteral("alg confusion"),
        QStringLiteral("kid traversal"), QStringLiteral("jku injection"),
        QStringLiteral("signature strip")});
    attackRow->addWidget(attack_choice_);
    attackRow->addStretch(1);
    attackLayout->addLayout(attackRow);
    jku_edit_ = new QLineEdit(attackTab);
    jku_edit_->setMaxLength(511);
    jku_edit_->setPlaceholderText(QStringLiteral("Attacker URL (jku)"));
    attackLayout->addWidget(jku_edit_);
    auto* attackButtons = new QHBoxLayout();
    attackButtons->setSpacing(t.spacing.sm);
    attack_run_button_ = new widgets::AidaButton(QStringLiteral("Run attack"), attackTab);
    attack_run_button_->setKind(widgets::AidaButton::Kind::Primary);
    attack_run_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    attackButtons->addWidget(attack_run_button_);
    attack_pack_button_ = new widgets::AidaButton(QStringLiteral("Pack: All"), attackTab);
    attack_pack_button_->setKind(widgets::AidaButton::Kind::Secondary);
    attack_pack_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    attackButtons->addWidget(attack_pack_button_);
    attackButtons->addStretch(1);
    attackLayout->addLayout(attackButtons);
    attack_count_label_ = new QLabel(QStringLiteral("Candidates: 0"), attackTab);
    attack_count_label_->setProperty("aidaTone", QStringLiteral("secondary"));
    attackLayout->addWidget(attack_count_label_);
    attack_scroll_ = new QScrollArea(attackTab);
    attack_scroll_->setWidgetResizable(true);
    attack_rows_ = new QWidget(attack_scroll_);
    attack_rows_layout_ = new QVBoxLayout(attack_rows_);
    attack_rows_layout_->setContentsMargins(0, 0, 0, 0);
    attack_rows_layout_->setSpacing(t.spacing.xs);
    attack_rows_layout_->addStretch(1);
    attack_scroll_->setWidget(attack_rows_);
    attackLayout->addWidget(attack_scroll_, 1);
    tabs_->addTab(attackTab, QStringLiteral("Attacks"));

    splitter->addWidget(tabs_);
    splitter->setStretchFactor(0, 9);
    splitter->setStretchFactor(1, 11);
    layout->addWidget(splitter, 1);

    runner_ = new BurpOperationRunner(QStringLiteral("burp_ui"), this);
    crack_timer_ = new QTimer(this);
    crack_timer_->setInterval(250);
    connect(crack_timer_, &QTimer::timeout, this, [this] { pollCrack(); });

    connect(decodeButton, &QAbstractButton::clicked, this, [this] { decodeToken(); });
    connect(clearButton, &QAbstractButton::clicked, this, [this] { clearToken(); });
    connect(hmacButton, &QAbstractButton::clicked, this, [this] { verifyHmac(); });
    connect(rsaButton, &QAbstractButton::clicked, this, [this] { submitVerify(true); });
    connect(ecdsaButton, &QAbstractButton::clicked, this, [this] { submitVerify(false); });
    connect(forge_button_, &QAbstractButton::clicked, this, [this] { submitForge(); });
    connect(copyToToken, &QAbstractButton::clicked, this, [this] {
        const QString output = forge_output_->toPlainText();
        if (!output.isEmpty() && !output.startsWith(QLatin1Char('('))) {
            token_edit_->setPlainText(output);
            diag::log_tagged("jwt_v", "forged_token_copied_to_input");
        }
    });
    connect(crack_start_button_, &QAbstractButton::clicked, this, [this] { startCrack(); });
    connect(crack_stop_button_, &QAbstractButton::clicked, this, [this] { stopCrack(); });
    connect(attack_run_button_, &QAbstractButton::clicked, this, [this] { submitAttack(false); });
    connect(attack_pack_button_, &QAbstractButton::clicked, this, [this] { submitAttack(true); });

    connect(runner_, &BurpOperationRunner::completed, this,
        [this](quint64, bool, bool, const QString&) {
            const auto output = std::atomic_load_explicit(&op_output_, std::memory_order_acquire);
            if (output)
                applyOperationOutput(*output);
        });
    connect(runner_, &BurpOperationRunner::submitted, this, [this](quint64) {
        forge_button_->setEnabled(false);
        attack_run_button_->setEnabled(false);
        attack_pack_button_->setEnabled(false);
    });
    connect(runner_, &BurpOperationRunner::completed, this, [this] {
        forge_button_->setEnabled(true);
        attack_run_button_->setEnabled(true);
        attack_pack_button_->setEnabled(true);
    });

    setContent(content);
}

void JwtLabView::onPaneShown() {
    ensureInitialized();
    if (active_crack_id_ != 0)
        crack_timer_->start();
}

void JwtLabView::onPaneHidden() {
    crack_timer_->stop();
}

void JwtLabView::ensureInitialized() {
    if (initialized_.load(std::memory_order_acquire) ||
        initialization_requested_.exchange(true, std::memory_order_acq_rel))
        return;
    QPointer<JwtLabView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.jwt_lab_view";
    submission.label = "jwt_lab.initialize";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane]() {
        aida::burp::jwt_lab::initialize();
        if (!pane)
            return;
        pane->initialized_.store(true, std::memory_order_release);
        pane->initialization_requested_.store(false, std::memory_order_release);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted)
        initialization_requested_.store(false, std::memory_order_release);
}

void JwtLabView::decodeToken() {
    const std::string token = token_edit_->toPlainText().toStdString();
    const auto parsed = aida::burp::jwt_lab::decode(token);
    decoded_state_label_->setText(parsed.valid_structure
        ? QStringLiteral("Decoded (valid)") : QStringLiteral("Decoded (structure invalid)"));
    set_label_tone(decoded_state_label_, parsed.valid_structure ? "success" : "warning");
    decoded_alg_label_->setText(QStringLiteral("alg=%1  kid=%2")
        .arg(QString::fromStdString(parsed.alg))
        .arg(QString::fromStdString(parsed.kid)));
    decoded_header_->setPlainText(QString::fromStdString(
        parsed.header.is_object() ? parsed.header.dump(2) : std::string("(invalid)")));
    decoded_payload_->setPlainText(QString::fromStdString(
        parsed.payload.is_object() ? parsed.payload.dump(2) : std::string("(invalid)")));
    diag::log_tagged_fmt("burp", "jwt_decode alg=%s kid=%s valid=%d",
        parsed.alg.c_str(), parsed.kid.c_str(), parsed.valid_structure ? 1 : 0);
}

void JwtLabView::clearToken() {
    token_edit_->clear();
    decoded_header_->clear();
    decoded_payload_->clear();
    decoded_alg_label_->clear();
    decoded_state_label_->setText(QStringLiteral("Decoded"));
    set_label_tone(decoded_state_label_, "secondary");
    diag::log_tagged("jwt_v", "token_cleared");
}

void JwtLabView::verifyHmac() {
    const std::string token = token_edit_->toPlainText().toStdString();
    const std::string secret = secret_edit_->text().toStdString();
    const bool ok = aida::burp::jwt_lab::verify_hmac(token, secret);
    verify_result_label_->setText(ok ? QStringLiteral("HMAC verified")
                                     : QStringLiteral("HMAC failed"));
    set_label_tone(verify_result_label_, ok ? "success" : "error");
    diag::log_tagged_fmt("jwt_v", "verify_hmac ok=%d", ok ? 1 : 0);
}

void JwtLabView::submitVerify(bool rsa) {
    const std::string token = token_edit_->toPlainText().toStdString();
    const std::string pem = rsa_pub_edit_->toPlainText().toStdString();
    BurpRequest request;
    request.owner = QStringLiteral("burp.jwt_lab");
    request.ownerView = QStringLiteral("view.network.jwt_lab");
    request.ownerAction = rsa ? QStringLiteral("network.jwt.verify_rsa")
                              : QStringLiteral("network.jwt.verify_ecdsa");
    request.label = rsa ? QStringLiteral("Verify JWT RSA signature")
                        : QStringLiteral("Verify JWT ECDSA signature");
    request.target = QStringLiteral("JWT token");
    request.affectedEntity = request.target;
    QPointer<JwtLabView> pane(this);
    request.execute = [pane, rsa, token = std::move(token), pem = std::move(pem)]() {
        aida::burp::ui_operation::result_t result;
        const bool ok = rsa ? aida::burp::jwt_lab::verify_rsa(token, pem)
                            : aida::burp::jwt_lab::verify_ecdsa(token, pem);
        result.success = ok;
        result.message = ok ? (rsa ? "RSA verified" : "ECDSA verified")
                            : (rsa ? "RSA failed" : "ECDSA failed");
        diag::log_tagged_fmt("jwt_v", rsa ? "verify_rsa ok=%d" : "verify_ecdsa ok=%d",
            ok ? 1 : 0);
        if (pane) {
            auto output = std::make_shared<op_output_t>();
            output->kind = op_output_t::kind_t::verify;
            output->text = result.message;
            std::atomic_store_explicit(&pane->op_output_,
                std::shared_ptr<const op_output_t>(std::move(output)),
                std::memory_order_release);
        }
        return result;
    };
    static_cast<void>(runner_->submit(std::move(request)));
}

void JwtLabView::submitForge() {
    aida::burp::jwt_lab::jwt_forge_input_t in;
    const QString headerText = forge_header_->toPlainText();
    const QString payloadText = forge_payload_->toPlainText();
    try {
        in.header = nlohmann::json::parse(headerText.isEmpty() ? std::string("{}")
            : headerText.toStdString(), nullptr, false);
    } catch (...) { in.header = nlohmann::json::object(); }
    try {
        in.payload = nlohmann::json::parse(payloadText.isEmpty() ? std::string("{}")
            : payloadText.toStdString(), nullptr, false);
    } catch (...) { in.payload = nlohmann::json::object(); }
    if (!in.header.is_object()) in.header = nlohmann::json::object();
    if (!in.payload.is_object()) in.payload = nlohmann::json::object();
    in.alg = forge_alg_->currentText().toStdString();
    in.hmac_secret = forge_secret_->text().toStdString();
    in.rsa_private_pem = forge_rsa_priv_->toPlainText().toStdString();
    in.ecdsa_private_pem = forge_ecdsa_priv_->toPlainText().toStdString();

    BurpRequest request;
    request.owner = QStringLiteral("burp.jwt_lab");
    request.ownerView = QStringLiteral("view.network.jwt_lab");
    request.ownerAction = QStringLiteral("network.jwt.forge");
    request.label = QStringLiteral("Forge JWT");
    request.target = QString::fromStdString(in.alg);
    request.affectedEntity = QStringLiteral("JWT token");
    QPointer<JwtLabView> pane(this);
    request.execute = [pane, in = std::move(in)]() {
        aida::burp::ui_operation::result_t result;
        const std::string outToken = aida::burp::jwt_lab::forge(in);
        result.success = !outToken.empty();
        result.message = outToken.empty()
            ? std::string("(error: ") + aida::burp::jwt_lab::last_error() + ")"
            : outToken;
        diag::log_tagged_fmt("burp", "jwt_forge alg=%s success=%d",
            in.alg.c_str(), outToken.empty() ? 0 : 1);
        if (pane) {
            auto output = std::make_shared<op_output_t>();
            output->kind = op_output_t::kind_t::forge;
            output->text = result.message;
            std::atomic_store_explicit(&pane->op_output_,
                std::shared_ptr<const op_output_t>(std::move(output)),
                std::memory_order_release);
        }
        return result;
    };
    static_cast<void>(runner_->submit(std::move(request)));
}

void JwtLabView::submitAttack(bool packAll) {
    const std::string token = token_edit_->toPlainText().toStdString();
    const std::string pem = rsa_pub_edit_->toPlainText().toStdString();
    const std::string jku = jku_edit_->text().toStdString();
    const int choice = attack_choice_->currentIndex();

    BurpRequest request;
    request.owner = QStringLiteral("burp.jwt_lab");
    request.ownerView = QStringLiteral("view.network.jwt_lab");
    request.ownerAction = QStringLiteral("network.jwt.attack");
    request.label = packAll ? QStringLiteral("Run all JWT attacks")
                            : QStringLiteral("Run JWT attack");
    request.target = QStringLiteral("JWT token");
    request.affectedEntity = request.target;
    QPointer<JwtLabView> pane(this);
    request.execute = [pane, packAll, choice, token = std::move(token),
                       pem = std::move(pem), jku = std::move(jku)]() {
        aida::burp::ui_operation::result_t result;
        std::vector<std::string> candidates;
        if (packAll) {
            auto addSet = [](std::vector<std::string>& dst,
                             const std::vector<std::string>& src) {
                for (const auto& v : src) dst.push_back(v);
            };
            addSet(candidates, aida::burp::jwt_lab::attack_alg_none(token));
            addSet(candidates, aida::burp::jwt_lab::attack_alg_confusion(token, pem));
            addSet(candidates, aida::burp::jwt_lab::attack_kid_traversal(token));
            addSet(candidates, aida::burp::jwt_lab::attack_jku_injection(token, jku));
            addSet(candidates, aida::burp::jwt_lab::attack_signature_strip(token));
        } else {
            switch (choice) {
                case 0: candidates = aida::burp::jwt_lab::attack_alg_none(token); break;
                case 1: candidates = aida::burp::jwt_lab::attack_alg_confusion(token, pem); break;
                case 2: candidates = aida::burp::jwt_lab::attack_kid_traversal(token); break;
                case 3: candidates = aida::burp::jwt_lab::attack_jku_injection(token, jku); break;
                case 4: candidates = aida::burp::jwt_lab::attack_signature_strip(token); break;
                default: break;
            }
        }
        diag::log_tagged_fmt("burp", "jwt_attack choice=%d candidates=%zu",
            packAll ? -1 : choice, candidates.size());
        result.success = true;
        result.message = std::to_string(candidates.size()) + " candidates";
        if (pane) {
            auto output = std::make_shared<op_output_t>();
            output->kind = op_output_t::kind_t::attack;
            output->candidates = std::move(candidates);
            std::atomic_store_explicit(&pane->op_output_,
                std::shared_ptr<const op_output_t>(std::move(output)),
                std::memory_order_release);
        }
        return result;
    };
    static_cast<void>(runner_->submit(std::move(request)));
}

void JwtLabView::applyOperationOutput(const op_output_t& output) {
    switch (output.kind) {
    case op_output_t::kind_t::verify:
        verify_result_label_->setText(QString::fromStdString(output.text));
        break;
    case op_output_t::kind_t::forge:
        forge_output_->setPlainText(QString::fromStdString(output.text));
        break;
    case op_output_t::kind_t::attack:
        rebuildAttackResults(output.candidates);
        break;
    default:
        break;
    }
}

void JwtLabView::rebuildAttackResults(const std::vector<std::string>& candidates) {
    attack_candidates_ = candidates;
    attack_count_label_->setText(QStringLiteral("Candidates: %1").arg(candidates.size()));
    while (auto* item = attack_rows_layout_->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto* row = new QWidget(attack_rows_);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(theme::tokens().spacing.sm);
        auto* useButton = new widgets::AidaButton(QStringLiteral("Use"), row);
        useButton->setKind(widgets::AidaButton::Kind::Ghost);
        useButton->setControlSize(widgets::AidaButton::ControlSize::Small);
        useButton->setToolTip(QStringLiteral("Copy this candidate into the token input"));
        rowLayout->addWidget(useButton);
        std::string preview = candidates[i];
        if (preview.size() > 160)
            preview = preview.substr(0, 160) + "...";
        auto* previewLabel = new QLabel(QString::fromStdString(preview), row);
        previewLabel->setToolTip(QString::fromStdString(candidates[i]));
        rowLayout->addWidget(previewLabel, 1);
        attack_rows_layout_->addWidget(row);
        connect(useButton, &QAbstractButton::clicked, this, [this, i] {
            if (i >= attack_candidates_.size())
                return;
            token_edit_->setPlainText(QString::fromStdString(attack_candidates_[i]));
            diag::log_tagged_fmt("jwt_v", "attack_candidate_used idx=%zu", i);
        });
    }
    attack_rows_layout_->addStretch(1);
}

void JwtLabView::startCrack() {
    aida::burp::jwt_lab::crack_config_t cfg;
    cfg.token = token_edit_->toPlainText().toStdString();
    cfg.wordlist_id = wordlist_edit_->text().toStdString();
    cfg.concurrency = static_cast<size_t>(crack_concurrency_->value());
    cfg.max_attempts = static_cast<size_t>(crack_max_attempts_->value());
    QPointer<JwtLabView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.jwt_lab_view";
    submission.label = "jwt_lab.start_crack";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, cfg = std::move(cfg)]() {
        const std::uint64_t id = aida::burp::jwt_lab::start_crack(cfg);
        if (!pane || id == 0)
            return;
        pane->started_crack_id_.store(id, std::memory_order_release);
        QMetaObject::invokeMethod(pane.data(), [pane]() {
            const std::uint64_t started = pane->started_crack_id_.exchange(
                0, std::memory_order_acq_rel);
            if (started != 0) {
                pane->active_crack_id_ = started;
                pane->crack_timer_->start();
            }
        }, Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void JwtLabView::stopCrack() {
    if (active_crack_id_ == 0)
        return;
    diag::log_tagged_fmt("jwt_v", "crack_stop id=%llu",
        static_cast<unsigned long long>(active_crack_id_));
    const std::uint64_t id = active_crack_id_;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.jwt_lab_view";
    submission.label = "jwt_lab.stop_crack";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [id]() { aida::burp::jwt_lab::crack_stop(id); };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void JwtLabView::pollCrack() {
    if (active_crack_id_ == 0) {
        crack_timer_->stop();
        return;
    }
    const auto status = aida::burp::jwt_lab::crack_status(active_crack_id_);
    crack_status_label_->setText(QStringLiteral("id=%1  attempts=%2  running=%3")
        .arg(static_cast<unsigned long long>(status.id))
        .arg(status.attempts)
        .arg(status.running ? QStringLiteral("yes") : QStringLiteral("no")));
    if (!status.secret_found.empty()) {
        if (status.secret_found != crack_last_found_) {
            crack_last_found_ = status.secret_found;
            diag::log_tagged_fmt("jwt_v", "crack_secret_found secret='%s' id=%llu",
                status.secret_found.c_str(),
                static_cast<unsigned long long>(active_crack_id_));
        }
        crack_secret_label_->setText(QStringLiteral("Secret: %1")
            .arg(QString::fromStdString(status.secret_found)));
        crack_secret_label_->setVisible(true);
    }
    if (!status.running)
        crack_timer_->stop();
}

}
