#include "qt/network/comparer/comparer_view.hpp"

#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <utility>

#include "core/infra/executor.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/network/burp_review_dialog.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"

namespace aida::qt::net {

namespace {

constexpr std::size_t k_max_published_bytes = 32U * 1024U;

aida::burp::comparer::diff_mode_t mode_from_index(int idx) {
    switch (idx) {
        case 0: return aida::burp::comparer::diff_mode_t::bytes;
        case 1: return aida::burp::comparer::diff_mode_t::chars;
        case 2: return aida::burp::comparer::diff_mode_t::words;
        case 3: return aida::burp::comparer::diff_mode_t::lines;
    }
    return aida::burp::comparer::diff_mode_t::lines;
}

const char* mode_name_from_index(int idx) {
    switch (idx) {
        case 0: return "bytes";
        case 1: return "chars";
        case 2: return "words";
        case 3: return "lines";
    }
    return "lines";
}

}

ComparerSlotsModel::ComparerSlotsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int ComparerSlotsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(slots_.size());
}

int ComparerSlotsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ComparerSlotsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* slot = slotAt(index.row());
    if (!slot)
        return {};
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Label: return QString::fromStdString(slot->label);
        case Id: return QString::number(static_cast<unsigned long long>(slot->id));
        case Size: return QString::number(static_cast<qulonglong>(slot->data.size()));
        case Source: return QString::fromStdString(slot->source_hint);
        default: return {};
        }
    }
    if (role == Qt::UserRole)
        return static_cast<unsigned long long>(slot->id);
    return {};
}

void ComparerSlotsModel::multiData(const QModelIndex& index,
                                   QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

void ComparerSlotsModel::refreshFromEngine() {
    auto slots = aida::burp::comparer::list_slots();
    if (slots_.size() == slots.size()) {
        bool same = true;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const auto& a = slots_[i];
            const auto& b = slots[i];
            if (a.id != b.id || a.created_ms != b.created_ms ||
                a.data.size() != b.data.size() || a.label != b.label) {
                same = false;
                break;
            }
        }
        if (same)
            return;
    }
    beginResetModel();
    slots_ = std::move(slots);
    endResetModel();
}

const aida::burp::comparer::slot_t* ComparerSlotsModel::slotAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(slots_.size()))
        return nullptr;
    return &slots_[static_cast<std::size_t>(row)];
}

const aida::burp::comparer::slot_t* ComparerSlotsModel::findSlot(std::uint64_t id) const noexcept {
    for (const auto& slot : slots_) {
        if (slot.id == id)
            return &slot;
    }
    return nullptr;
}

std::vector<aida::burp::comparer::slot_t> ComparerSlotsModel::slots() const {
    return slots_;
}

ComparerView::ComparerView(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.comparer"));
    const auto& t = theme::tokens();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* barRow = new QHBoxLayout();
    barRow->setSpacing(t.spacing.sm);
    barRow->addWidget(new QLabel(QStringLiteral("Label"), content));
    label_edit_ = new QLineEdit(QStringLiteral("Slot"), content);
    label_edit_->setMaxLength(127);
    barRow->addWidget(label_edit_);
    paste_button_ = new widgets::AidaButton(QStringLiteral("Paste -> slot"), content);
    paste_button_->setKind(widgets::AidaButton::Kind::Secondary);
    paste_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    barRow->addWidget(paste_button_);
    barRow->addWidget(new QLabel(QStringLiteral("File"), content));
    file_edit_ = new QLineEdit(content);
    file_edit_->setMaxLength(1023);
    barRow->addWidget(file_edit_, 1);
    add_file_button_ = new widgets::AidaButton(QStringLiteral("Add file"), content);
    add_file_button_->setKind(widgets::AidaButton::Kind::Secondary);
    add_file_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    barRow->addWidget(add_file_button_);
    clear_button_ = new widgets::AidaButton(QStringLiteral("Clear slots"), content);
    clear_button_->setKind(widgets::AidaButton::Kind::Destructive);
    clear_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    barRow->addWidget(clear_button_);
    layout->addLayout(barRow);

    auto* pasteRow = new QHBoxLayout();
    pasteRow->setSpacing(t.spacing.sm);
    pasteRow->addWidget(new QLabel(QStringLiteral("Paste raw text"), content));
    paste_edit_ = new QLineEdit(content);
    paste_edit_->setMaxLength(65535);
    pasteRow->addWidget(paste_edit_, 1);
    add_text_button_ = new widgets::AidaButton(QStringLiteral("Add text"), content);
    add_text_button_->setKind(widgets::AidaButton::Kind::Secondary);
    add_text_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    pasteRow->addWidget(add_text_button_);
    layout->addLayout(pasteRow);

    auto* selectorRow = new QHBoxLayout();
    selectorRow->setSpacing(t.spacing.sm);
    slots_model_ = new ComparerSlotsModel(this);
    selectorRow->addWidget(new QLabel(QStringLiteral("A"), content));
    combo_a_ = new QComboBox(content);
    combo_a_->setModel(slots_model_);
    combo_a_->setModelColumn(ComparerSlotsModel::Label);
    combo_a_->setMinimumWidth(field_width_chars(combo_a_, 20));
    selectorRow->addWidget(combo_a_);
    selectorRow->addWidget(new QLabel(QStringLiteral("B"), content));
    combo_b_ = new QComboBox(content);
    combo_b_->setModel(slots_model_);
    combo_b_->setModelColumn(ComparerSlotsModel::Label);
    combo_b_->setMinimumWidth(field_width_chars(combo_b_, 20));
    selectorRow->addWidget(combo_b_);
    selectorRow->addWidget(new QLabel(QStringLiteral("Mode"), content));
    mode_combo_ = new QComboBox(content);
    mode_combo_->addItems({QStringLiteral("bytes"), QStringLiteral("chars"),
        QStringLiteral("words"), QStringLiteral("lines")});
    mode_combo_->setCurrentIndex(3);
    selectorRow->addWidget(mode_combo_);
    swap_button_ = new widgets::AidaButton(QStringLiteral("Swap"), content);
    swap_button_->setKind(widgets::AidaButton::Kind::Ghost);
    swap_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    selectorRow->addWidget(swap_button_);
    selectorRow->addStretch(1);
    layout->addLayout(selectorRow);

    stats_label_ = new QLabel(QStringLiteral("Select two slots above to compute the diff."), content);
    stats_label_->setObjectName(QStringLiteral("aida.view.network.comparer.stats"));
    stats_label_->setProperty("aidaTone", QStringLiteral("dim"));
    layout->addWidget(stats_label_);

    viewer_ = new ComparerDiffViewer(content);
    layout->addWidget(viewer_, 1);

    connect(paste_button_, &QAbstractButton::clicked, this, [this] { addSlotFromClipboard(); });
    connect(add_file_button_, &QAbstractButton::clicked, this, [this] { addSlotFromFile(); });
    connect(add_text_button_, &QAbstractButton::clicked, this, [this] { addSlotFromText(); });
    connect(clear_button_, &QAbstractButton::clicked, this, [this] { clearSlots(); });

    connect(combo_a_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (slots_refreshing_)
            return;
        const std::uint64_t id = selectedId(true);
        if (id != selected_a_) {
            selected_a_ = id;
            cached_valid_ = false;
            ensureDiff();
        }
    });
    connect(combo_b_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (slots_refreshing_)
            return;
        const std::uint64_t id = selectedId(false);
        if (id != selected_b_) {
            selected_b_ = id;
            cached_valid_ = false;
            ensureDiff();
        }
    });
    connect(mode_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index != mode_idx_) {
            diag::log_tagged_fmt("comparer_v", "mode_changed mode=%s",
                mode_name_from_index(index));
            mode_idx_ = index;
            cached_valid_ = false;
        }
    });
    connect(swap_button_, &QAbstractButton::clicked, this, [this] {
        if (selected_a_ == 0 || selected_b_ == 0)
            return;
        diag::log_tagged_fmt("comparer_v", "swap_slots a=%llu b=%llu",
            static_cast<unsigned long long>(selected_a_),
            static_cast<unsigned long long>(selected_b_));
        std::swap(selected_a_, selected_b_);
        cached_valid_ = false;
        syncComboSelections();
        ensureDiff();
    });
    connect(viewer_, &ComparerDiffViewer::contextMenuRequested, this,
        &ComparerView::openSlotContextMenu);

    slots_timer_ = new QTimer(this);
    slots_timer_->setInterval(500);
    connect(slots_timer_, &QTimer::timeout, this, [this] {
        refreshSlots();
        ensureDiff();
    });

    refreshSlots();
    setContent(content);
}

void ComparerView::onPaneShown() {
    refreshSlots();
    ensureDiff();
    slots_timer_->start();
}

void ComparerView::onPaneHidden() {
    slots_timer_->stop();
}

void ComparerView::refreshSlots() {
    slots_refreshing_ = true;
    slots_model_->refreshFromEngine();
    pruneSelection(slots_model_->slots());
    syncComboSelections();
    slots_refreshing_ = false;
}

void ComparerView::pruneSelection(const std::vector<aida::burp::comparer::slot_t>& slots) {
    const auto exists = [&slots](std::uint64_t id) {
        return id == 0 || std::any_of(slots.begin(), slots.end(),
            [id](const auto& slot) { return slot.id == id; });
    };
    if (!exists(selected_a_) || !exists(selected_b_)) {
        if (!exists(selected_a_)) selected_a_ = 0;
        if (!exists(selected_b_)) selected_b_ = 0;
        cached_valid_ = false;
        diff_error_ = "A selected comparer input was removed; select an available slot.";
        diff_generation_.fetch_add(1, std::memory_order_acq_rel);
        diff_pending_.store(false, std::memory_order_release);
        updateStatsStrip();
    }
}

std::uint64_t ComparerView::selectedId(bool sideA) const {
    const QComboBox* combo = sideA ? combo_a_ : combo_b_;
    const int index = combo->currentIndex();
    if (index < 0)
        return 0;
    const auto* slot = slots_model_->slotAt(index);
    return slot ? slot->id : 0;
}

void ComparerView::setSelectedId(bool sideA, std::uint64_t id) {
    QComboBox* combo = sideA ? combo_a_ : combo_b_;
    const QSignalBlocker blocker(combo);
    const auto slots = slots_model_->slots();
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].id == id) {
            combo->setCurrentIndex(static_cast<int>(i));
            break;
        }
    }
    if (id == 0)
        combo->setCurrentIndex(-1);
}

void ComparerView::ensureDiff() {
    if (selected_a_ == 0 || selected_b_ == 0) {
        cached_valid_ = false;
        updateStatsStrip();
        viewer_->clearDiff();
        return;
    }
    if (cached_valid_)
        return;
    const auto published = std::atomic_load_explicit(&publication_, std::memory_order_acquire);
    if (published && published->slot_a == selected_a_ && published->slot_b == selected_b_ &&
        published->mode_idx == mode_idx_) {
        adoptPublication(published);
        return;
    }
    if (diff_pending_.load(std::memory_order_acquire) &&
        requested_a_ == selected_a_ && requested_b_ == selected_b_ &&
        requested_mode_ == mode_idx_)
        return;
    const std::uint64_t slot_a = selected_a_;
    const std::uint64_t slot_b = selected_b_;
    const int mode_idx = mode_idx_;
    const std::uint64_t generation = diff_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    requested_a_ = slot_a;
    requested_b_ = slot_b;
    requested_mode_ = mode_idx;
    diff_pending_.store(true, std::memory_order_release);
    updateStatsStrip();
    QPointer<ComparerView> pane(this);

    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.comparer";
    submission.label = "comparer.compute_diff";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 3;
    submission.ui_access_policy = "immutable_snapshots_only";
    submission.failure_policy = "typed_diagnostic";
    submission.body = [pane, slot_a, slot_b, mode_idx, generation] {
        auto next = std::make_shared<diff_publication_t>();
        next->generation = generation;
        next->slot_a = slot_a;
        next->slot_b = slot_b;
        next->mode_idx = mode_idx;
        aida::burp::comparer::slot_t slotA;
        aida::burp::comparer::slot_t slotB;
        try {
            if (!aida::burp::comparer::get_slot(slot_a, slotA))
                next->error = "Comparer input A is no longer available.";
            else if (!aida::burp::comparer::get_slot(slot_b, slotB))
                next->error = "Comparer input B is no longer available.";
            else {
                auto content = std::make_shared<comparer_diff_content_t>();
                content->generation = generation;
                content->blocks = aida::burp::comparer::compute_diff_with_stats(
                    slot_a, slot_b, mode_from_index(mode_idx), next->stats);
                if (slotA.data.size() > k_max_published_bytes)
                    slotA.data.resize(k_max_published_bytes);
                if (slotB.data.size() > k_max_published_bytes)
                    slotB.data.resize(k_max_published_bytes);
                decode_utf8_with_cu_map(slotA.data, content->sideA);
                decode_utf8_with_cu_map(slotB.data, content->sideB);
                next->content = std::move(content);
                next->succeeded = true;
            }
        } catch (const std::exception& exception) {
            next->error = exception.what();
        } catch (...) {
            next->error = "Comparer diff failed with an unknown exception.";
        }
        if (!pane)
            return;
        std::shared_ptr<const diff_publication_t> immutable = next;
        QMetaObject::invokeMethod(pane.data(),
            [pane, immutable]() {
                if (pane->diff_generation_.load(std::memory_order_acquire) !=
                    immutable->generation)
                    return;
                std::atomic_store_explicit(&pane->publication_, immutable,
                    std::memory_order_release);
                pane->diff_pending_.store(false, std::memory_order_release);
                pane->ensureDiff();
            }, Qt::QueuedConnection);
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        diff_pending_.store(false, std::memory_order_release);
        updateStatsStrip();
    }
}

void ComparerView::adoptPublication(
    const std::shared_ptr<const diff_publication_t>& publication) {
    diff_error_ = publication->error;
    if (!publication->succeeded) {
        cached_valid_ = false;
        updateStatsStrip();
        viewer_->clearDiff();
        return;
    }
    cached_stats_ = publication->stats;
    cached_valid_ = true;
    applied_generation_ = publication->generation;
    diff_error_.clear();
    diag::log_tagged_fmt("comparer_v", "diff_computed a=%llu b=%llu mode=%d blocks_eq=%zu ins=%zu del=%zu rep=%zu",
        static_cast<unsigned long long>(publication->slot_a),
        static_cast<unsigned long long>(publication->slot_b),
        publication->mode_idx,
        publication->stats.equal_runs, publication->stats.insert_runs,
        publication->stats.delete_runs, publication->stats.replace_runs);
    viewer_->setDiff(publication->content);
    updateStatsStrip();
}

void ComparerView::updateStatsStrip() {
    if (diff_pending_.load(std::memory_order_acquire) &&
        selected_a_ != 0 && selected_b_ != 0) {
        stats_label_->setText(QStringLiteral("Computing diff..."));
        set_label_tone(stats_label_, "accent");
        return;
    }
    if (cached_valid_) {
        const auto& st = cached_stats_;
        stats_label_->setText(QStringLiteral(
            "A=%1B  B=%2B  equal_runs=%3 insert_runs=%4 delete_runs=%5 replace_runs=%6  "
            "bytes equal=%7 inserted=%8 deleted=%9 replaced=%10%11")
            .arg(st.a_size).arg(st.b_size)
            .arg(st.equal_runs).arg(st.insert_runs).arg(st.delete_runs).arg(st.replace_runs)
            .arg(st.bytes_equal).arg(st.bytes_inserted).arg(st.bytes_deleted).arg(st.bytes_replaced)
            .arg(st.truncated ? QStringLiteral("  (truncated)") : QString()));
        set_label_tone(stats_label_, "secondary");
    } else if (!diff_error_.empty()) {
        stats_label_->setText(QString::fromStdString(diff_error_));
        set_label_tone(stats_label_, "error");
    } else {
        stats_label_->setText(QStringLiteral("Select two slots above to compute the diff."));
        set_label_tone(stats_label_, "dim");
    }
}

void ComparerView::addSlotFromClipboard() {
    const QString text = clipboard::text();
    if (text.isEmpty())
        return;
    const QByteArray utf8 = text.toUtf8();
    std::vector<std::uint8_t> data(utf8.begin(), utf8.end());
    const std::string label = label_edit_->text().toStdString();
    diag::log_tagged_fmt("comparer_v", "paste_slot label='%s' bytes=%zu", label.c_str(), data.size());
    QPointer<ComparerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.comparer_view";
    submission.label = "comparer.add_clipboard_slot";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, label, data = std::move(data)]() {
        aida::burp::comparer::add_slot_from_bytes(label, data, "clipboard");
        if (pane)
            QMetaObject::invokeMethod(pane.data(), [pane] { pane->refreshSlots(); },
                Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void ComparerView::addSlotFromFile() {
    const QString path = file_edit_->text();
    if (path.isEmpty())
        return;
    diag::log_tagged_fmt("comparer_v", "add_file_slot label='%s' path='%s'",
        label_edit_->text().toStdString().c_str(), path.toStdString().c_str());
    const std::string label = label_edit_->text().toStdString();
    const std::string pathStd = path.toStdString();
    QPointer<ComparerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.comparer_view";
    submission.label = "comparer.add_file_slot";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, label, pathStd]() {
        aida::burp::comparer::add_slot_from_file(label, pathStd);
        if (pane)
            QMetaObject::invokeMethod(pane.data(), [pane] { pane->refreshSlots(); },
                Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
}

void ComparerView::addSlotFromText() {
    const QString text = paste_edit_->text();
    if (text.isEmpty())
        return;
    const QByteArray utf8 = text.toUtf8();
    std::vector<std::uint8_t> data(utf8.begin(), utf8.end());
    diag::log_tagged_fmt("comparer_v", "add_text_slot label='%s' bytes=%zu",
        label_edit_->text().toStdString().c_str(), data.size());
    const std::string label = label_edit_->text().toStdString();
    QPointer<ComparerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.comparer_view";
    submission.label = "comparer.add_text_slot";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane, label, data = std::move(data)]() {
        aida::burp::comparer::add_slot_from_bytes(label, data, "manual");
        if (pane)
            QMetaObject::invokeMethod(pane.data(), [pane] { pane->refreshSlots(); },
                Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
    paste_edit_->clear();
}

void ComparerView::clearSlots() {
    diag::log_tagged("comparer_v", "clear_slots");
    QPointer<ComparerView> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.comparer_view";
    submission.label = "comparer.clear_slots";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::external_tool;
    submission.priority = 3;
    submission.body = [pane]() {
        aida::burp::comparer::clear_slots();
        if (pane)
            QMetaObject::invokeMethod(pane.data(), [pane] { pane->refreshSlots(); },
                Qt::QueuedConnection);
    };
    static_cast<void>(aida::infra::executor::submit(std::move(submission)));
    selected_a_ = 0;
    selected_b_ = 0;
    cached_valid_ = false;
    viewer_->clearDiff();
    updateStatsStrip();
    syncComboSelections();
}

void ComparerView::openSlotContextMenu(bool sideA, const QPoint& globalPos) {
    const std::uint64_t slotId = selectedId(sideA);
    aida::burp::comparer::slot_t current;
    if (slotId == 0 || !aida::burp::comparer::get_slot(slotId, current))
        return;
    const std::uint64_t retained_id = current.id;
    const std::uint64_t retained_created = current.created_ms;
    const std::size_t retained_size = current.data.size();
    const auto validate_identity = [retained_id, retained_created, retained_size]() -> bool {
        aida::burp::comparer::slot_t live;
        return aida::burp::comparer::get_slot(retained_id, live) &&
            live.created_ms == retained_created && live.data.size() == retained_size;
    };
    const QString slotText = QString::fromUtf8(
        reinterpret_cast<const char*>(current.data.data()),
        current.data.empty() ? 0 : static_cast<qsizetype>(current.data.size()));

    auto* menuHolder = new QMenu(this);
    menuHolder->setAttribute(Qt::WA_DeleteOnClose);
    QMenu& menu = *menuHolder;
    const QString label = QString::fromStdString(current.label.empty()
        ? "Slot #" + std::to_string(current.id) : current.label);
    menu.setTitle(label);

    auto* copyAction = menu.addAction(QStringLiteral("Copy slot text"));
    connect(copyAction, &QAction::triggered, this, [this, validate_identity, slotText] {
        if (!validate_identity()) {
            diff_error_ = "The comparer slot was removed or replaced; select it again";
            updateStatsStrip();
            return;
        }
        clipboard::set_text(slotText);
    });

    auto* useA = menu.addAction(QStringLiteral("Use as comparer input A"));
    const bool canA = selected_a_ != current.id;
    useA->setEnabled(canA);
    if (!canA)
        useA->setToolTip(QStringLiteral("This slot is already selected as comparer input A"));
    connect(useA, &QAction::triggered, this, [this, retained_id, validate_identity] {
        if (!validate_identity())
            return;
        selected_a_ = retained_id;
        cached_valid_ = false;
        setSelectedId(true, retained_id);
        ensureDiff();
    });

    auto* useB = menu.addAction(QStringLiteral("Use as comparer input B"));
    const bool canB = selected_b_ != current.id;
    useB->setEnabled(canB);
    if (!canB)
        useB->setToolTip(QStringLiteral("This slot is already selected as comparer input B"));
    connect(useB, &QAction::triggered, this, [this, retained_id, validate_identity] {
        if (!validate_identity())
            return;
        selected_b_ = retained_id;
        cached_valid_ = false;
        setSelectedId(false, retained_id);
        ensureDiff();
    });

    auto* swapAction = menu.addAction(QStringLiteral("Swap A and B"));
    const bool canSwap = selected_a_ != 0 && selected_b_ != 0;
    swapAction->setEnabled(canSwap);
    if (!canSwap)
        swapAction->setToolTip(QStringLiteral("Select both comparer inputs before swapping them"));
    connect(swapAction, &QAction::triggered, this, [this] {
        std::swap(selected_a_, selected_b_);
        cached_valid_ = false;
        syncComboSelections();
        ensureDiff();
    });

    menu.addSeparator();
    auto* removeAction = menu.addAction(QStringLiteral("Remove slot"));
    connect(removeAction, &QAction::triggered, this, [this, retained_id] {
        openRemoveReview(retained_id);
    });

    menu.popup(globalPos);
}

void ComparerView::syncComboSelections() {
    setSelectedId(true, selected_a_);
    setSelectedId(false, selected_b_);
}

void ComparerView::openRemoveReview(std::uint64_t slotId) {
    aida::burp::comparer::slot_t current;
    if (!aida::burp::comparer::get_slot(slotId, current)) {
        diff_error_ = "The comparer slot is no longer available.";
        updateStatsStrip();
        return;
    }
    const std::uint64_t retained_id = current.id;
    const std::uint64_t retained_created = current.created_ms;
    const std::size_t retained_size = current.data.size();
    auto* dialog = new BurpReviewDialog(
        QStringLiteral("Remove comparer slot"),
        { QStringLiteral("Remove '%1' and its %2 bytes from the comparer? This cannot be undone.")
            .arg(QString::fromStdString(current.label))
            .arg(current.data.size()) },
        QStringLiteral("Remove"), true, this);
    dialog->setRevalidator([retained_id, retained_created, retained_size](QString& reasonOut) {
        aida::burp::comparer::slot_t live;
        if (!aida::burp::comparer::get_slot(retained_id, live) ||
            live.created_ms != retained_created || live.data.size() != retained_size) {
            reasonOut = QStringLiteral(
                "The comparer slot was removed or replaced; select it again");
            return false;
        }
        return true;
    });
    dialog->setSubmitCallback([this, retained_id] {
        QPointer<ComparerView> pane(this);
        aida::infra::executor::submission_t submission;
        submission.owner_subsystem = "burp.comparer_view";
        submission.label = "comparer.remove_slot";
        submission.thread_class = "bounded_task";
        submission.domain = aida::infra::executor::domain_t::external_tool;
        submission.priority = 3;
        submission.body = [pane, retained_id]() {
            aida::burp::comparer::remove_slot(retained_id);
            if (pane)
                QMetaObject::invokeMethod(pane.data(), [pane] { pane->refreshSlots(); },
                    Qt::QueuedConnection);
        };
        static_cast<void>(aida::infra::executor::submit(std::move(submission)));
        if (selected_a_ == retained_id) selected_a_ = 0;
        if (selected_b_ == retained_id) selected_b_ = 0;
        cached_valid_ = false;
        viewer_->clearDiff();
        syncComboSelections();
    });
    dialog->open();
}

}
