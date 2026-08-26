#include "qt/debugger/dialogs/stage_patch_dialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "core/runtime/standalone_driver.hpp"

#include "qt/theme/aida_fonts.hpp"

namespace aida::qt::debugger {

void StagePatchDialog::present(
    const debugger_view::patch_stage_review_t& review, QWidget* parent) {
    auto* dialog = new StagePatchDialog(review, parent);
    dialog->open();
}

StagePatchDialog::StagePatchDialog(
    debugger_view::patch_stage_review_t review, QWidget* parent)
    : AidaDialog(parent), review_(std::move(review)) {
    setObjectName(QStringLiteral("aida.debugger.patch_stage"));
    setWindowTitle(QStringLiteral("Stage Patch Review"));
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(420, 300);

    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(QStringLiteral("Review a patch definition"), this);
    title->setFont(theme::fonts::strong());
    layout->addWidget(title);
    auto* subtitle = new QLabel(QStringLiteral(
        "This stages an inactive definition. It does not write target memory."),
        this);
    subtitle->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(subtitle);

    auto* form = new QFormLayout();
    auto* address_label = new QLabel(QString::asprintf("0x%016llX",
        static_cast<unsigned long long>(review_.address)), this);
    address_label->setFont(theme::fonts::codeRegular());
    form->addRow(QStringLiteral("Address:"), address_label);
    if (review_.extent != 0)
        form->addRow(QStringLiteral("Selected range:"),
            new QLabel(QString::asprintf("%llu bytes",
                static_cast<unsigned long long>(review_.extent)), this));
    layout->addLayout(form);

    layout->addWidget(new QLabel(
        QStringLiteral("Replacement bytes (hex)"), this));
    bytes_edit_ = new QPlainTextEdit(this);
    bytes_edit_->setObjectName(
        QStringLiteral("aida.debugger.patch_stage.bytes"));
    bytes_edit_->setFont(theme::fonts::codeRegular());
    bytes_edit_->setPlaceholderText(QStringLiteral("90 90 90"));
    if (review_.exact && !review_.expected_before.empty()) {
        QString prefill;
        for (std::size_t i = 0; i < review_.expected_before.size(); ++i) {
            if (i != 0)
                prefill += u' ';
            prefill += QString::asprintf("%02X",
                static_cast<unsigned>(review_.expected_before[i]));
        }
        bytes_edit_->setPlainText(prefill);
    }
    connect(bytes_edit_, &QPlainTextEdit::textChanged, this,
        &StagePatchDialog::reparse);
    layout->addWidget(bytes_edit_, 1);

    layout->addWidget(new QLabel(QStringLiteral("Description"), this));
    description_edit_ = new QLineEdit(
        QString::fromStdString(review_.description), this);
    description_edit_->setObjectName(
        QStringLiteral("aida.debugger.patch_stage.description"));
    description_edit_->setMaxLength(255);
    layout->addWidget(description_edit_);

    validation_label_ = new QLabel(this);
    validation_label_->setWordWrap(true);
    validation_label_->setVisible(false);
    layout->addWidget(validation_label_);

    target_label_ = new QLabel(QStringLiteral(
        "The reviewed live target or debugger stop is unavailable; cancel and "
        "capture a new patch review."), this);
    target_label_->setWordWrap(true);
    target_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    layout->addWidget(target_label_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    stage_button_ = buttons->button(QDialogButtonBox::Apply);
    stage_button_->setText(QStringLiteral("Stage Inactive"));
    connect(buttons, &QDialogButtonBox::clicked, this,
        [this, buttons](QAbstractButton* button) {
            if (button == buttons->button(QDialogButtonBox::Apply))
                stage();
            else
                reject();
        });
    layout->addWidget(buttons);

    auto* target_timer = new QTimer(this);
    target_timer->setInterval(250);
    target_timer->setTimerType(Qt::CoarseTimer);
    connect(target_timer, &QTimer::timeout, this, [this] {
        const bool target_ready = driver_bridge::is_loaded() &&
            debugger_interaction::is_current(review_.context) &&
            review_.context.address == review_.address;
        target_label_->setVisible(!target_ready);
        stage_button_->setEnabled(parse_valid_ && target_ready);
    });
    target_timer->start();

    reparse();

    bridge::AidaDialog::RevalidateScope::hooks_t hooks;
    const auto context = review_.context;
    hooks.identity_fn = [context]() {
        return QString::number(context.target_pid) + QStringLiteral(":") +
            QString::number(context.process_creation_time_100ns) +
            QStringLiteral(":") + QString::number(context.address);
    };
    hooks.generation_fn = []() {
        return static_cast<quint64>(
            debugger_interaction::current_stop_generation());
    };
    add_revalidate_scope(hooks, QStringLiteral(
        "The reviewed live target or debugger stop changed while the patch "
        "review was open."));
}

void StagePatchDialog::reparse() {
    parsed_.clear();
    parse_valid_ = debugger_view::parse_patch_bytes(
        bytes_edit_->toPlainText().toStdString(), parsed_);
    const bool empty = bytes_edit_->toPlainText().trimmed().isEmpty();

    QString message;
    bool hard_error = false;
    if (!parse_valid_ && !empty) {
        message = QStringLiteral(
            "Enter complete two-digit hex bytes separated by whitespace "
            "(maximum 4096 bytes).");
        hard_error = true;
    } else if (review_.extent != 0 && parse_valid_ &&
               parsed_.size() > review_.extent) {
        parse_valid_ = false;
        message = QStringLiteral(
            "Replacement bytes exceed the retained selected range.");
        hard_error = true;
    } else if (review_.extent != 0 && parse_valid_ &&
               parsed_.size() < review_.extent) {
        message = QStringLiteral(
            "Replacement bytes cover only part of the retained selected "
            "range; review before staging.");
    }
    if (review_.exact && parse_valid_ &&
        parsed_.size() != review_.expected_before.size()) {
        parse_valid_ = false;
        message = QStringLiteral(
            "The reviewed replacement must preserve the exact proposal byte "
            "range.");
        hard_error = true;
    }
    validation_label_->setText(message);
    validation_label_->setVisible(!message.isEmpty());
    const char* severity = hard_error ? "error" : "warning";
    if (validation_label_->property("aidaVariant") != severity) {
        validation_label_->setProperty("aidaVariant",
            QString::fromLatin1(severity));
        validation_label_->style()->unpolish(validation_label_);
        validation_label_->style()->polish(validation_label_);
    }
    const bool target_ready = driver_bridge::is_loaded() &&
        debugger_interaction::is_current(review_.context) &&
        review_.context.address == review_.address;
    stage_button_->setEnabled(parse_valid_ && target_ready);
}

void StagePatchDialog::stage() {
    std::string error;
    if (debugger_view::commit_patch_stage_review(review_, parsed_,
            description_edit_->text().toStdString(), &error)) {
        accept();
        return;
    }
    if (!error.empty()) {
        validation_label_->setText(QString::fromStdString(error));
        validation_label_->setVisible(true);
    }
}

}
