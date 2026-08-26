#include "qt/disasm/dialogs/static_patch_review_dialog.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_notice.hpp"

#include "core/analysis/workspace/overlay_journal.hpp"

#include <QDialogButtonBox>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableView>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace aida::qt::disasm::dialogs {

namespace {

constexpr int k_max_hex_chars = 196608;

}

class PatchBytesEdit : public QPlainTextEdit {
public:
    explicit PatchBytesEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent) {}

    void insertFromMimeData(const QMimeData* source) override
    {
        if (!source)
            return;
        QString text = source->text().toUpper();
        const int room = k_max_hex_chars - toPlainText().size();
        if (room <= 0)
            return;
        if (text.size() > room)
            text = text.left(room);
        textCursor().insertText(text);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        QPlainTextEdit::keyPressEvent(event);
        if (toPlainText().size() > k_max_hex_chars) {
            const QString text = toPlainText().left(k_max_hex_chars);
            const int pos = textCursor().position();
            QSignalBlocker blocker(this);
            setPlainText(text);
            QTextCursor cursor = textCursor();
            cursor.setPosition((std::min)(pos, k_max_hex_chars));
            setTextCursor(cursor);
        }
    }
};

PatchDiffModel::PatchDiffModel(QObject* parent) : QAbstractTableModel(parent) {}

void PatchDiffModel::set_bytes(const std::vector<std::uint8_t>& original,
                               const std::vector<std::uint8_t>& proposed)
{
    beginResetModel();
    original_copy_ = original;
    proposed_copy_ = proposed;
    original_ = &original_copy_;
    proposed_ = &proposed_copy_;
    endResetModel();
}

int PatchDiffModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid() || !original_ || !proposed_)
        return 0;
    return static_cast<int>((std::max)(original_->size(), proposed_->size()));
}

int PatchDiffModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 4;
}

QVariant PatchDiffModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || !original_ || !proposed_)
        return {};
    if (role == Qt::FontRole)
        return index.column() <= 2
            ? QVariant(theme::fonts::codeRegular()) : QVariant();
    if (role != Qt::DisplayRole)
        return {};
    const auto row = static_cast<std::size_t>(index.row());
    const bool has_original = row < original_->size();
    const bool has_proposed = row < proposed_->size();
    switch (index.column()) {
    case 0:
        return QStringLiteral("+0x%1").arg(row, 4, 16, QLatin1Char('0')).toUpper();
    case 1:
        return has_original
            ? QStringLiteral("%1").arg((*original_)[row], 2, 16, QLatin1Char('0')).toUpper()
            : QStringLiteral("--");
    case 2:
        return has_proposed
            ? QStringLiteral("%1").arg((*proposed_)[row], 2, 16, QLatin1Char('0')).toUpper()
            : QStringLiteral("--");
    case 3:
        if (!has_original)
            return QStringLiteral("extends overlay range");
        if (!has_proposed)
            return QStringLiteral("outside replacement range");
        if ((*original_)[row] == (*proposed_)[row])
            return QStringLiteral("unchanged");
        return QStringLiteral("replace");
    }
    return {};
}

QVariant PatchDiffModel::headerData(int section, Qt::Orientation orientation,
                                    int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case 0: return QStringLiteral("Offset");
    case 1: return QStringLiteral("Original");
    case 2: return QStringLiteral("Proposed");
    case 3: return QStringLiteral("Change");
    }
    return {};
}

AidaStaticPatchReviewDialog::AidaStaticPatchReviewDialog(
    disasm_view::workspace_context_t context, disasm_view::static_patch_init_t init,
    QWidget* parent)
    : bridge::AidaDialog(parent), context_(std::move(context)), init_(std::move(init))
{
    setObjectName(QStringLiteral("aida.disasm.dialog.static_patch_review"));
    setWindowTitle(QStringLiteral("Static Patch Review"));
    setModal(false);
    setSizeGripEnabled(true);
    const auto& t = theme::tokens();
    resize(static_cast<int>(t.shell.min_panel_w) * 8, t.control.height_md * 20);
    proposed_ = init_.proposed;

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        QStringLiteral("Review immutable-source bytes and commit a reversible workspace overlay"),
        this));
    const auto display = disasm_view::runtime_address(context_, init_.address)
        .value_or(init_.address.value);
    layout->addWidget(new QLabel(QStringLiteral("Workspace: %1")
        .arg(QString::fromStdString(context_.workspace->identity().bin_name())), this));
    layout->addWidget(new QLabel(QStringLiteral("Address:   0x%1")
        .arg(display, 0, 16), this));
    layout->addWidget(new QLabel(QStringLiteral("Selection: %1 byte%2")
        .arg(init_.extent)
        .arg(init_.extent == 1 ? QString() : QStringLiteral("s")), this));
    layout->addWidget(new QLabel(QStringLiteral("Fence: generation %1 / analysis %2 / overlay %3")
        .arg(init_.generation).arg(init_.analysis_revision).arg(init_.overlay_revision), this));

    stale_notice_ = new widgets::AidaNotice(QStringLiteral("Review is stale"),
        QStringLiteral("The workspace publication or overlay changed. Close and reopen the patch action so the original-byte diff cannot target stale state."),
        widgets::AidaSemantic::Warning, this);
    stale_notice_->setObjectName(QStringLiteral("aida.static_patch.stale"));
    stale_notice_->hide();
    layout->addWidget(stale_notice_);
    pending_notice_ = new widgets::AidaNotice(QStringLiteral("Overlay mutation pending"),
        QStringLiteral("Wait for the current workspace mutation to publish before committing another operation."),
        widgets::AidaSemantic::Info, this);
    pending_notice_->setObjectName(QStringLiteral("aida.static_patch.pending"));
    pending_notice_->hide();
    layout->addWidget(pending_notice_);

    layout->addWidget(new QLabel(init_.mode == disasm_view::static_patch_mode_t::nop_fill
        ? QStringLiteral("Proposed NOP bytes") : QStringLiteral("Replacement bytes"), this));
    bytes_ = new PatchBytesEdit(this);
    bytes_->setObjectName(QStringLiteral("aida.static_patch.bytes"));
    bytes_->setPlainText(QString::fromStdString(init_.encoded));
    bytes_->setMinimumHeight(t.control.input_h * 2 + t.spacing.lg);
    if (init_.mode == disasm_view::static_patch_mode_t::nop_fill)
        bytes_->setReadOnly(true);
    layout->addWidget(bytes_);
    connect(bytes_, &QPlainTextEdit::textChanged, this,
        &AidaStaticPatchReviewDialog::reparse);

    layout->addWidget(new QLabel(QStringLiteral("Description"), this));
    description_ = new QLineEdit(this);
    description_->setObjectName(QStringLiteral("aida.static_patch.description"));
    description_->setMaxLength(255);
    description_->setText(QString::fromStdString(init_.description));
    layout->addWidget(description_);

    parse_notice_ = new widgets::AidaNotice(QStringLiteral("Patch bytes"), QString(),
        widgets::AidaSemantic::Error, this);
    parse_notice_->setObjectName(QStringLiteral("aida.static_patch.parse"));
    parse_notice_->hide();
    layout->addWidget(parse_notice_);
    status_notice_ = new widgets::AidaNotice(QStringLiteral("Existing overlay detail"),
        QString(), widgets::AidaSemantic::Info, this);
    status_notice_->setObjectName(QStringLiteral("aida.static_patch.status"));
    status_notice_->hide();
    layout->addWidget(status_notice_);
    if (!init_.status.empty()) {
        status_notice_->setMessage(QString::fromStdString(init_.status));
        status_notice_->show();
    }
    existing_badge_ = new QLabel(this);
    existing_badge_->setObjectName(QStringLiteral("aida.static_patch.existing"));
    if (init_.existing) {
        existing_badge_->setText(QStringLiteral("Existing overlay at address — %1 byte%2")
            .arg(init_.existing_size)
            .arg(init_.existing_size == 1 ? QString() : QStringLiteral("s")));
    } else {
        existing_badge_->hide();
    }
    layout->addWidget(existing_badge_);

    diff_model_ = new PatchDiffModel(this);
    diff_model_->set_bytes(init_.original, proposed_);
    diff_ = new QTableView(this);
    diff_->setObjectName(QStringLiteral("aida.static_patch.diff"));
    diff_->verticalHeader()->setDefaultSectionSize(t.table.compact_row_h);
    diff_->verticalHeader()->setVisible(false);
    diff_->setModel(diff_model_);
    diff_->setSelectionMode(QAbstractItemView::NoSelection);
    diff_->setAlternatingRowColors(true);
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const int offset_column_w = static_cast<int>(code_metrics.horizontalAdvance(
        QStringLiteral("+0x0000"))) + t.table.cell_pad_x * 2;
    const int hex_column_w = static_cast<int>(code_metrics.horizontalAdvance(
        QStringLiteral("00"))) + t.table.cell_pad_x * 2;
    diff_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    diff_->horizontalHeader()->resizeSection(0, offset_column_w);
    diff_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    diff_->horizontalHeader()->resizeSection(1, hex_column_w);
    diff_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    diff_->horizontalHeader()->resizeSection(2, hex_column_w);
    diff_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(diff_, 1);

    error_notice_ = new widgets::AidaNotice(QStringLiteral("Patch workflow error"),
        QString(), widgets::AidaSemantic::Error, this);
    error_notice_->setObjectName(QStringLiteral("aida.static_patch.error"));
    error_notice_->hide();
    layout->addWidget(error_notice_);

    auto* buttons = new QHBoxLayout();
    undo_ = new QPushButton(QStringLiteral("Undo Last Overlay"), this);
    undo_->setObjectName(QStringLiteral("aida.static_patch.undo"));
    redo_ = new QPushButton(QStringLiteral("Redo Last Overlay"), this);
    redo_->setObjectName(QStringLiteral("aida.static_patch.redo"));
    revert_ = new QPushButton(QStringLiteral("Revert This Overlay"), this);
    revert_->setObjectName(QStringLiteral("aida.static_patch.revert"));
    buttons->addWidget(undo_);
    buttons->addWidget(redo_);
    buttons->addWidget(revert_);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    connect(undo_, &QPushButton::clicked, this, &AidaStaticPatchReviewDialog::undo_overlay);
    connect(redo_, &QPushButton::clicked, this, &AidaStaticPatchReviewDialog::redo_overlay);
    connect(revert_, &QPushButton::clicked, this, &AidaStaticPatchReviewDialog::revert_overlay);

    auto* footer = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    commit_ = footer->button(QDialogButtonBox::Apply);
    commit_->setObjectName(QStringLiteral("aida.static_patch.commit"));
    commit_->setText(QStringLiteral("Commit Workspace Overlay"));
    layout->addWidget(footer);
    connect(footer, &QDialogButtonBox::clicked, this, [this](QAbstractButton* button) {
        auto* box = qobject_cast<QDialogButtonBox*>(sender());
        if (box && box->buttonRole(button) == QDialogButtonBox::ApplyRole)
            commit();
        else
            reject();
    });

    revalidate_timer_ = new QTimer(this);
    revalidate_timer_->setInterval(100);
    connect(revalidate_timer_, &QTimer::timeout, this,
        &AidaStaticPatchReviewDialog::revalidate);
    revalidate_timer_->start();
    revalidate();
    if (init_.focus_input)
        bytes_->setFocus();
}

AidaStaticPatchReviewDialog::~AidaStaticPatchReviewDialog() = default;

void AidaStaticPatchReviewDialog::reconfigure(
    disasm_view::workspace_context_t context, disasm_view::static_patch_init_t init)
{
    context_ = std::move(context);
    init_ = std::move(init);
    proposed_ = init_.proposed;
    parse_error_.clear();
    bytes_->setReadOnly(init_.mode == disasm_view::static_patch_mode_t::nop_fill);
    {
        QSignalBlocker blocker(bytes_);
        bytes_->setPlainText(QString::fromStdString(init_.encoded));
    }
    description_->setText(QString::fromStdString(init_.description));
    diff_model_->set_bytes(init_.original, proposed_);
    if (!init_.status.empty()) {
        status_notice_->setMessage(QString::fromStdString(init_.status));
        status_notice_->show();
    } else {
        status_notice_->hide();
    }
    existing_badge_->setVisible(init_.existing);
    if (init_.existing) {
        existing_badge_->setText(QStringLiteral("Existing overlay at address — %1 byte%2")
            .arg(init_.existing_size)
            .arg(init_.existing_size == 1 ? QString() : QStringLiteral("s")));
    }
    parse_notice_->hide();
    error_notice_->hide();
    if (init_.focus_input)
        bytes_->setFocus();
    revalidate();
}

void AidaStaticPatchReviewDialog::revalidate()
{
    if (!context_.workspace) {
        reject();
        return;
    }
    const bool generation_current = context_.workspace->generation() == init_.generation;
    const bool analysis_current =
        context_.workspace->analysis_revision() == init_.analysis_revision;
    const bool overlay_current =
        context_.workspace->overlay_revision() == init_.overlay_revision;
    std::uint32_t pending = 0;
    if (context_.view)
        pending = context_.view->pending_mutations.load(std::memory_order_acquire);
    const bool identity_current = generation_current && analysis_current && overlay_current &&
        !context_.workspace->closing() && !context_.workspace->closed();
    stale_notice_->setVisible(!identity_current);
    pending_notice_->setVisible(pending != 0 && identity_current);
    const bool can_commit = identity_current && pending == 0 && parse_error_.isEmpty() &&
        !proposed_.empty();
    commit_->setEnabled(can_commit);
    undo_->setEnabled(identity_current && pending == 0);
    redo_->setEnabled(identity_current && pending == 0);
    revert_->setEnabled(init_.existing && identity_current && pending == 0);
}

void AidaStaticPatchReviewDialog::reparse()
{
    if (init_.mode != disasm_view::static_patch_mode_t::bytes)
        return;
    std::string error;
    const auto decoded = disasm_view::decode_patch_bytes(
        bytes_->toPlainText().toStdString(), error);
    if (decoded) {
        proposed_ = *decoded;
        parse_error_.clear();
    } else {
        proposed_.clear();
        parse_error_ = QString::fromStdString(error);
    }
    if (parse_error_.isEmpty()) {
        parse_notice_->hide();
    } else {
        parse_notice_->setMessage(parse_error_);
        parse_notice_->show();
    }
    diff_model_->set_bytes(init_.original, proposed_);
    revalidate();
}

void AidaStaticPatchReviewDialog::commit()
{
    aida::analysis::overlay_operation_t operation;
    operation.kind = aida::analysis::overlay_operation_kind_t::byte_patch;
    operation.address = init_.address;
    operation.bytes = proposed_;
    operation.text = description_->text().toStdString();
    std::vector<aida::analysis::overlay_operation_t> operations;
    operations.push_back(std::move(operation));
    if (disasm_view::queue_overlay_transaction(context_, std::move(operations),
            init_.generation, init_.analysis_revision, init_.overlay_revision)) {
        accept();
        return;
    }
    error_notice_->setMessage(QStringLiteral(
        "The generation-fenced overlay queue rejected the patch; reopen the review against current state."));
    error_notice_->show();
}

void AidaStaticPatchReviewDialog::undo_overlay()
{
    if (disasm_view::queue_overlay_history(context_, false, init_.generation,
            init_.analysis_revision, init_.overlay_revision)) {
        accept();
        return;
    }
    error_notice_->setMessage(QStringLiteral(
        "No generation-fenced overlay transaction was available to undo."));
    error_notice_->show();
}

void AidaStaticPatchReviewDialog::redo_overlay()
{
    if (disasm_view::queue_overlay_history(context_, true, init_.generation,
            init_.analysis_revision, init_.overlay_revision)) {
        accept();
        return;
    }
    error_notice_->setMessage(QStringLiteral(
        "No generation-fenced overlay transaction was available to redo."));
    error_notice_->show();
}

void AidaStaticPatchReviewDialog::revert_overlay()
{
    std::optional<aida::analysis::overlay_operation_t> exact_patch;
    if (const auto overlay = context_.workspace->overlay()) {
        const auto patches = overlay->patch_operations();
        const auto found = std::find_if(patches.begin(), patches.end(),
            [this](const aida::analysis::overlay_operation_t& operation) {
                return operation.address == init_.address && !operation.remove;
            });
        if (found != patches.end())
            exact_patch = *found;
    }
    if (!exact_patch) {
        error_notice_->setMessage(QStringLiteral(
            "The exact overlay no longer exists; reopen the review against current state."));
        error_notice_->show();
        return;
    }
    exact_patch->remove = true;
    std::vector<aida::analysis::overlay_operation_t> operations;
    operations.push_back(std::move(*exact_patch));
    if (disasm_view::queue_overlay_transaction(context_, std::move(operations),
            init_.generation, init_.analysis_revision, init_.overlay_revision)) {
        accept();
        return;
    }
    error_notice_->setMessage(QStringLiteral(
        "The exact overlay changed before it could be reverted."));
    error_notice_->show();
}

}
