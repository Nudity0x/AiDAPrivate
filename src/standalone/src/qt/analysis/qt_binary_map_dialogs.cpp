#include "qt/analysis/qt_binary_map_dialogs.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/disasm/disasm_view.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_binary_map_shared.hpp"
#include "qt/widgets/aida_notice.hpp"

namespace aida::qt::analysis {

QtChangeProtectionDialog::QtChangeProtectionDialog(
    std::shared_ptr<QtBinaryMapViewState> state,
    qt_binary_map_live_target_binding_t binding, std::uint64_t address,
    std::uint64_t size, std::uint32_t current_protect, QWidget* parent)
    : QDialog(parent), state_(std::move(state)), binding_(binding),
      address_(address), size_(size), current_protect_(current_protect) {
    setObjectName(QStringLiteral("aida.dialog.binary_map.change_protection"));
    setWindowTitle(QStringLiteral("Change Protection"));
    setModal(true);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(600, 460);
    setMinimumSize(400, 320);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("Address: %1")
        .arg(address_, 16, 16, QLatin1Char('0')), this));
    layout->addWidget(new QLabel(QStringLiteral("Size:    %1 bytes")
        .arg(size_), this));
    layout->addWidget(new QLabel(QStringLiteral("Current: 0x%1")
        .arg(current_protect_, 0, 16), this));
    auto* warning = new widgets::AidaNotice(QStringLiteral("Live process mutation"),
        QStringLiteral("This mutates the attached process over the exact range above. "
            "AiDA will read back the resulting region protection, but this operation "
            "has no automatic undo."), widgets::AidaSemantic::Warning, this);
    layout->addWidget(warning);

    combo_ = new QComboBox(this);
    combo_->setObjectName(QStringLiteral("aida.binary_map.change_protection.combo"));
    combo_->setToolTip(QStringLiteral(
        "New page protection applied to the reviewed range"));
    static const char* k_labels[] = {
        "PAGE_NOACCESS (0x01)", "PAGE_READONLY (0x02)", "PAGE_READWRITE (0x04)",
        "PAGE_WRITECOPY (0x08)", "PAGE_EXECUTE (0x10)", "PAGE_EXECUTE_READ (0x20)",
        "PAGE_EXECUTE_READWRITE (0x40)", "PAGE_EXECUTE_WRITECOPY (0x80)"
    };
    for (const char* label : k_labels)
        combo_->addItem(QString::fromLatin1(label));
    layout->addWidget(combo_);
    status_ = new QLabel(this);
    status_->setObjectName(
        QStringLiteral("aida.binary_map.change_protection.status"));
    status_->setProperty("aidaVariant", QStringLiteral("secondary"));
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this);
    apply_button_ = buttons->button(QDialogButtonBox::Ok);
    apply_button_->setText(QStringLiteral("Apply"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { apply(); });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { reject(); });

    timer_ = new QTimer(this);
    timer_->setInterval(250);
    connect(timer_, &QTimer::timeout, this, [this] { revalidate(); });
    timer_->start();
}

void QtChangeProtectionDialog::revalidate() {
    const bool pending = state_->change_protect_pending.load(std::memory_order_acquire);
    const auto workspace = state_->workspace.lock();
    std::string identity_error;
    const bool current = !pending && bm_validate_live_binding(binding_, workspace,
        identity_error);
    apply_button_->setEnabled(current);
    if (pending) {
        status_->setText(QStringLiteral("A reviewed protection change is already running"));
    } else if (!current) {
        status_->setText(QStringLiteral(
            "The reviewed region is stale: %1")
            .arg(QString::fromStdString(identity_error)));
    } else {
        status_->clear();
    }
}

void QtChangeProtectionDialog::apply() {
    static const std::uint32_t k_values[] = {
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
    };
    const int choice = (std::max)(0, (std::min)(combo_->currentIndex(), 7));
    const std::uint32_t new_protect = k_values[choice];
    diag::log_tagged_critical_fmt("binary_map",
        "change_protect_request addr=0x%llx size=%llu new=0x%X",
        static_cast<unsigned long long>(address_),
        static_cast<unsigned long long>(size_),
        static_cast<unsigned>(new_protect));
    const auto context =
        disasm_view::capture_workspace(state_->workspace.lock());
    const bool queued = context &&
        bm_queue_protection_change(state_, context, binding_, address_, size_,
            current_protect_, new_protect);
    if (!queued)
        QtAnalysisBridge::instance().toastError(QStringLiteral(
            "The reviewed protection change could not be queued; see Task Center"), 3.0);
    accept();
}

}
