#include "qt/analysis/qt_write_review_dialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>

#include "helpers/diag_log.hpp"

#include "core/infra/executor.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/scanner/memory_scanner.hpp"
#include "core/ui/task_center.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_gui_post.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_notice.hpp"

namespace aida::qt::analysis {

QtWriteReviewDialog* QtWriteReviewDialog::active_instance_ = nullptr;

namespace {

std::string format_review_bytes(const std::vector<std::uint8_t>& bytes) {
    std::string output;
    const std::size_t shown = (std::min)(bytes.size(), std::size_t{64});
    output.reserve(shown * 3 + 32);
    char encoded[4]{};
    for (std::size_t index = 0; index < shown; ++index) {
        if (!output.empty()) output.push_back(' ');
        std::snprintf(encoded, sizeof(encoded), "%02X", bytes[index]);
        output.append(encoded);
    }
    if (shown != bytes.size())
        output.append(" ... (").append(std::to_string(bytes.size())).append(" bytes)");
    return output;
}

std::uint64_t structure_identity_locked(int structure_index) {
    const auto& state = struct_dissector::g_state;
    if (!struct_dissector::valid_index(structure_index, state.structs.size()))
        return 0;
    const auto& definition = state.structs[static_cast<std::size_t>(structure_index)];
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (const char character : definition.name)
        mix(static_cast<unsigned char>(character));
    mix(definition.stable_id);
    mix(definition.layout_revision);
    mix(definition.total_size);
    mix(static_cast<std::uint64_t>(definition.kind));
    mix(definition.packing);
    mix(definition.explicit_alignment);
    for (const auto& field : definition.fields) {
        for (const char character : field.name)
            mix(static_cast<unsigned char>(character));
        mix(static_cast<std::uint64_t>(field.type));
        mix(field.offset);
        mix(field.size);
        mix(field.array_count);
        mix(field.stable_id);
        mix(field.target_structure_id);
        mix(field.enum_id);
        mix(field.bit_offset);
        mix(field.bit_width);
        mix(field.explicit_alignment);
        for (const char character : field.referenced_type_name)
            mix(static_cast<unsigned char>(character));
    }
    return hash;
}

}

QtWriteReviewDialog::QtWriteReviewDialog(QWidget* parent) : QDialog(parent) {
    active_instance_ = this;
    setObjectName(QStringLiteral("aida.dialog.structure_write_review"));
    setWindowTitle(QStringLiteral("Review Structure Field Write"));
    setModal(true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(700, 620);
    setMinimumSize(440, 360);

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(QStringLiteral(
        "Review the exact process mutation before it is submitted."), this);
    layout->addWidget(intro);

    const auto add_row = [this, layout](const QString& label, QLabel*& value_out) {
        auto* row = new QHBoxLayout();
        auto* name = new QLabel(label, this);
        name->setFixedWidth(aida::qt::theme::tokens().row.property_label_w);
        auto* value = new QLabel(this);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        row->addWidget(name);
        row->addWidget(value, 1);
        layout->addLayout(row);
        value_out = value;
    };
    add_row(QStringLiteral("Target PID"), pid_value_);
    add_row(QStringLiteral("Structure / field"), field_value_);
    add_row(QStringLiteral("Address range"), range_value_);
    add_row(QStringLiteral("Old bytes"), old_bytes_);
    add_row(QStringLiteral("New bytes"), new_bytes_);

    auto* consequence = new widgets::AidaNotice(QStringLiteral(
        "Process memory will change"),
        QStringLiteral("Applying this write can change control flow, data "
            "interpretation, stability, or process behavior immediately. AiDA "
            "revalidates the reviewed old bytes before writing and requires an "
            "exact readback match."), widgets::AidaSemantic::Warning, this);
    layout->addWidget(consequence);
    auto* undo_notice = new widgets::AidaNotice(QStringLiteral("Undo is explicit"),
        QStringLiteral("AiDA does not silently roll back a live process mutation. "
            "After a verified write, use Stage undo to review the inverse write of "
            "these exact old bytes."), widgets::AidaSemantic::Neutral, this);
    layout->addWidget(undo_notice);

    status_label_ = new QLabel(this);
    layout->addWidget(status_label_);
    error_notice_ = new widgets::AidaNotice(this);
    error_notice_->setVisible(false);
    layout->addWidget(error_notice_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this);
    layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] { onConfirm(); });
    connect(buttons_, &QDialogButtonBox::rejected, this, [this] { onCancel(); });

    timer_ = new QTimer(this);
    timer_->setInterval(50);
    connect(timer_, &QTimer::timeout, this, [this] { pollWorker(); });
}

void QtWriteReviewDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    timer_->start();
}

bool QtWriteReviewDialog::stage(const disasm_view::workspace_context_t& context,
                                int structure_index, int field_index,
                                const struct_dissector::field_def_t& field,
                                const struct_dissector::live_value_t& value,
                                std::uint64_t base_address, const char* text,
                                std::string& error) {
    // Verbatim port of struct_dissector_view::stage_write_review.
    if (!context.workspace) {
        error = "The structure is not bound to a current process workspace.";
        return false;
    }
    if (!context.workspace->identity().process()) {
        error = "The structure is not bound to a current process workspace.";
        return false;
    }
    const auto parsed = [&]() -> std::optional<std::vector<std::uint8_t>> {
        memory_scanner::value_type_t type = memory_scanner::value_type_t::int32_val;
        bool hex_input = false;
        switch (field.type) {
        case struct_dissector::field_type_t::int8:
        case struct_dissector::field_type_t::uint8:
            type = memory_scanner::value_type_t::byte_val; break;
        case struct_dissector::field_type_t::int16:
        case struct_dissector::field_type_t::uint16:
            type = memory_scanner::value_type_t::int16_val; break;
        case struct_dissector::field_type_t::int32:
        case struct_dissector::field_type_t::uint32:
            type = memory_scanner::value_type_t::int32_val; break;
        case struct_dissector::field_type_t::int64:
        case struct_dissector::field_type_t::uint64:
        case struct_dissector::field_type_t::pointer:
            type = memory_scanner::value_type_t::int64_val;
            hex_input = field.type == struct_dissector::field_type_t::pointer;
            break;
        case struct_dissector::field_type_t::float32:
            type = memory_scanner::value_type_t::float_val; break;
        case struct_dissector::field_type_t::float64:
            type = memory_scanner::value_type_t::double_val; break;
        case struct_dissector::field_type_t::ascii_string:
            type = memory_scanner::value_type_t::string_ascii; break;
        case struct_dissector::field_type_t::utf16_string:
            type = memory_scanner::value_type_t::string_utf16; break;
        default:
            type = memory_scanner::value_type_t::byte_array;
            hex_input = true;
            break;
        }
        auto bytes = memory_scanner::parse_value(text ? text : "", type, hex_input);
        return bytes.empty() ? std::optional<std::vector<std::uint8_t>>{}
            : std::move(bytes);
    }();
    if (!parsed) {
        error = "The entered value is invalid for this field type.";
        return false;
    }
    const std::uint64_t span = static_cast<std::uint64_t>(field.size) * field.array_count;
    if (span == 0 || span > 4096 || parsed->size() > span || parsed->size() > 4096) {
        error = "The encoded value does not fit the bounded field range.";
        return false;
    }
    if (value.raw_bytes.size() < parsed->size()) {
        error = "The current field snapshot does not contain every byte that would be replaced.";
        return false;
    }
    if (base_address == 0 || base_address >
        (std::numeric_limits<std::uint64_t>::max)() - field.offset ||
        base_address + field.offset >
        (std::numeric_limits<std::uint64_t>::max)() - (parsed->size() - 1)) {
        error = "The reviewed write range overflows the target address space.";
        return false;
    }
    std::uint64_t identity = 0;
    std::string structure_name;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        if (!struct_dissector::valid_index(structure_index,
                struct_dissector::g_state.structs.size())) {
            error = "The selected structure no longer exists.";
            return false;
        }
        identity = structure_identity_locked(structure_index);
        structure_name = struct_dissector::g_state.structs[
            static_cast<std::size_t>(structure_index)].name;
    }
    if (review_.cancellation)
        review_.cancellation->store(true, std::memory_order_release);
    review_ = {};
    review_.visible = true;
    review_.open_requested = true;
    review_.status = qt_write_review_status_t::review;
    review_.serial = write_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
    review_.pid = context.workspace->identity().process()->pid;
    review_.address = base_address + field.offset;
    review_.workspace_generation = context.workspace->generation();
    review_.structure_identity = identity;
    review_.base_address = base_address;
    review_.structure_index = structure_index;
    review_.field_index = field_index;
    review_.structure_name = std::move(structure_name);
    review_.field_name = field.name;
    review_.old_bytes.assign(value.raw_bytes.begin(),
        value.raw_bytes.begin() + static_cast<std::ptrdiff_t>(parsed->size()));
    review_.new_bytes = std::move(*parsed);
    review_.workspace = context.workspace;
    error.clear();
    rebuildContent();
    show();
    raise();
    activateWindow();
    return true;
}

void QtWriteReviewDialog::rebuildContent() {
    pid_value_->setText(QString::number(review_.pid));
    field_value_->setText(QStringLiteral("%1.%2")
        .arg(QString::fromStdString(review_.structure_name))
        .arg(QString::fromStdString(review_.field_name)));
    const std::uint64_t end_address = review_.new_bytes.empty()
        ? review_.address
        : review_.address + review_.new_bytes.size() - 1;
    range_value_->setText(QStringLiteral("0x%1 - 0x%2 (%3 bytes)")
        .arg(review_.address, 16, 16, QLatin1Char('0'))
        .arg(end_address, 16, 16, QLatin1Char('0'))
        .arg(review_.new_bytes.size()));
    old_bytes_->setText(QString::fromStdString(format_review_bytes(review_.old_bytes)));
    new_bytes_->setText(QString::fromStdString(format_review_bytes(review_.new_bytes)));
    pollWorker();
}

void QtWriteReviewDialog::pollWorker() {
    const bool active = review_.status == qt_write_review_status_t::queued ||
        review_.status == qt_write_review_status_t::running;
    const bool can_stage_undo = !active &&
        ((review_.status == qt_write_review_status_t::succeeded &&
            review_.error.empty()) || review_.mutation_may_remain);
    auto* ok = buttons_->button(QDialogButtonBox::Ok);
    auto* cancel = buttons_->button(QDialogButtonBox::Cancel);
    const char* status_text =
        review_.status == qt_write_review_status_t::review ? "Awaiting confirmation" :
        review_.status == qt_write_review_status_t::queued ? "Queued" :
        review_.status == qt_write_review_status_t::running ? "Writing and verifying" :
        review_.status == qt_write_review_status_t::succeeded ? "Verified" :
        review_.status == qt_write_review_status_t::cancelled ? "Cancelled" :
        review_.status == qt_write_review_status_t::stale ? "Stale" : "Failed";
    status_label_->setText(QStringLiteral("Status: %1").arg(
        QString::fromLatin1(status_text)));
    if (review_.status == qt_write_review_status_t::review) {
        ok->setText(QStringLiteral("Apply and verify"));
        ok->setEnabled(true);
        cancel->setText(QStringLiteral("Cancel"));
        cancel->setVisible(true);
    } else if (active) {
        ok->setText(QStringLiteral("Cancel operation"));
        ok->setEnabled(true);
        cancel->setVisible(false);
    } else {
        ok->setText(can_stage_undo ? QStringLiteral("Stage undo")
            : QStringLiteral("Close"));
        ok->setEnabled(true);
        cancel->setText(QStringLiteral("Close"));
        cancel->setVisible(can_stage_undo);
    }
    error_notice_->setVisible(!review_.error.empty());
    if (!review_.error.empty()) {
        error_notice_->setTitle(review_.status == qt_write_review_status_t::succeeded
            ? QStringLiteral("Verified with publication warning")
            : review_.mutation_may_remain
                ? QStringLiteral("Mutation verification and rollback failed")
                : QStringLiteral("Mutation not applied"));
        error_notice_->setMessage(QString::fromStdString(review_.error));
        error_notice_->setKind(review_.status == qt_write_review_status_t::succeeded
            ? widgets::AidaSemantic::Warning : widgets::AidaSemantic::Error);
    }
    if (active)
        timer_->start();
    else
        timer_->stop();
}

void QtWriteReviewDialog::onConfirm() {
    switch (review_.status) {
    case qt_write_review_status_t::review:
        submitWrite();
        break;
    case qt_write_review_status_t::queued:
    case qt_write_review_status_t::running:
        if (review_.cancellation)
            review_.cancellation->store(true, std::memory_order_release);
        break;
    default:
        if ((review_.status == qt_write_review_status_t::succeeded &&
                review_.error.empty()) || review_.mutation_may_remain)
            stageUndo();
        else
            reject();
        break;
    }
}

void QtWriteReviewDialog::onCancel() {
    if (review_.status == qt_write_review_status_t::review ||
        review_.status == qt_write_review_status_t::queued ||
        review_.status == qt_write_review_status_t::running) {
        if (review_.cancellation)
            review_.cancellation->store(true, std::memory_order_release);
    }
    review_.visible = false;
    reject();
}

void QtWriteReviewDialog::stageUndo() {
    std::swap(review_.old_bytes, review_.new_bytes);
    review_.serial = write_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
    review_.status = qt_write_review_status_t::review;
    review_.error.clear();
    review_.cancellation.reset();
    rebuildContent();
}

bool QtWriteReviewDialog::submitWrite() {
    // Verbatim port of struct_dissector_view::submit_write_review's worker.
    if (!review_.visible || review_.status != qt_write_review_status_t::review)
        return false;
    auto workspace = review_.workspace.lock();
    if (!workspace)
        return false;
    auto cancellation = std::make_shared<std::atomic<bool>>(false);
    review_.cancellation = cancellation;
    review_.status = qt_write_review_status_t::queued;
    review_.mutation_may_remain = false;
    const auto serial = review_.serial;
    const auto pid = review_.pid;
    const auto address = review_.address;
    const auto workspace_generation = review_.workspace_generation;
    const auto structure_identity = review_.structure_identity;
    const auto base_address = review_.base_address;
    const auto structure_index = review_.structure_index;
    const auto field_index = review_.field_index;
    const auto structure_name = review_.structure_name;
    const auto field_name = review_.field_name;
    const auto old_bytes = review_.old_bytes;
    const auto new_bytes = review_.new_bytes;
    const std::string task_id = "structure.write." + std::to_string(pid) + "." +
        std::to_string(serial);
    aida::ui::task_center::task_registration_t registration;
    registration.id = task_id;
    registration.source = "structure_dissector";
    registration.owner = "Structure Dissector";
    registration.owner_view = "view.types.structures";
    registration.owner_action = "Apply reviewed field write";
    registration.target = "PID " + std::to_string(pid);
    registration.label = "Write and verify structure field";
    registration.stage = "Queued reviewed mutation";
    registration.affected_entity = structure_name + "." + field_name;
    registration.cancellation_is_safe = true;
    registration.callbacks.cancel = [cancellation] {
        bool expected = false;
        return cancellation->compare_exchange_strong(expected, true,
            std::memory_order_acq_rel);
    };
    if (!aida::ui::task_center::register_task(std::move(registration))) {
        review_.status = qt_write_review_status_t::failed;
        review_.error = "Task Center rejected ownership of the reviewed mutation.";
        pollWorker();
        return false;
    }
    auto result_error = std::make_shared<std::string>();
    auto observed = std::make_shared<std::vector<std::uint8_t>>();
    auto mutation_may_remain = std::make_shared<std::atomic<bool>>(false);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "structure_dissector";
    submission.label = "structure.write_reviewed_field";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 2;
    submission.target_pid = pid;
    submission.generation = workspace_generation;
    submission.cancel_hook = [cancellation] {
        cancellation->store(true, std::memory_order_release);
    };
    submission.body = [this, workspace, cancellation, result_error, observed,
        mutation_may_remain, task_id, pid, address, workspace_generation,
        structure_identity, base_address, structure_index, field_index,
        structure_name, field_name, old_bytes, new_bytes, serial]() {
        running_serial_ = serial;
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::running, 0.1f,
            "Revalidating reviewed target bytes"));
        bool wrote = false;
        if (cancellation->load(std::memory_order_acquire)) {
            *result_error = "The reviewed mutation was cancelled before it started.";
        } else {
            const auto process = workspace ? workspace->identity().process()
                : std::nullopt;
            bool definition_current = false;
            {
                std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
                definition_current =
                    struct_dissector::g_state.active_struct == structure_index &&
                    struct_dissector::g_state.base_address == base_address &&
                    structure_identity_locked(structure_index) == structure_identity &&
                    struct_dissector::valid_index(structure_index,
                        struct_dissector::g_state.structs.size()) &&
                    struct_dissector::valid_index(field_index,
                        struct_dissector::g_state.structs[
                            static_cast<std::size_t>(structure_index)].fields.size());
            }
            if (!workspace || workspace->closing() || workspace->closed() ||
                workspace->generation() != workspace_generation || !process ||
                process->pid != pid || !driver_bridge::is_loaded() ||
                driver_bridge::attached_pid() != pid) {
                *result_error = "The reviewed process or workspace generation is stale.";
            } else if (!definition_current) {
                *result_error = "The reviewed structure, field, or base address is stale.";
            } else {
                std::vector<std::uint8_t> current;
                if (!driver_bridge::read_memory_for(pid, address, old_bytes.size(),
                        current) || current.size() != old_bytes.size())
                    *result_error = "The pre-write exact-range read failed.";
                else if (current != old_bytes)
                    *result_error =
                        "The target bytes changed after review; no write was performed.";
                else if (cancellation->load(std::memory_order_acquire))
                    *result_error =
                        "The reviewed mutation was cancelled before the write.";
                else if (!driver_bridge::write_memory_for(pid, address, new_bytes))
                    *result_error = "The driver rejected the reviewed write.";
                else {
                    std::string verification_error;
                    if (!driver_bridge::read_memory_for(pid, address, new_bytes.size(),
                            *observed) || observed->size() != new_bytes.size())
                        verification_error =
                            "The post-write exact-range readback failed.";
                    else if (*observed != new_bytes)
                        verification_error =
                            "The post-write bytes do not match the reviewed value.";
                    else
                        wrote = true;
                    if (!wrote) {
                        std::vector<std::uint8_t> restored;
                        const bool rollback_verified =
                            driver_bridge::write_memory_for(pid, address, old_bytes) &&
                            driver_bridge::read_memory_for(pid, address,
                                old_bytes.size(), restored) &&
                            restored == old_bytes;
                        *result_error = verification_error + (rollback_verified
                            ? " The original bytes were restored and verified."
                            : " Automatic rollback did not pass exact verification; the target may be partially mutated.");
                        mutation_may_remain->store(!rollback_verified,
                            std::memory_order_release);
                    }
                }
            }
        }
        const bool posted = gui_post(this, [this, workspace, cancellation, result_error,
            mutation_may_remain, task_id, pid, workspace_generation,
            structure_identity, base_address, structure_index, field_index,
            new_bytes, serial, wrote] {
            publishResult(wrote, *result_error,
                mutation_may_remain->load(std::memory_order_acquire), serial,
                workspace_generation, structure_identity, base_address,
                structure_index, field_index, new_bytes, task_id, pid,
                cancellation->load(std::memory_order_acquire));
        });
        if (!posted) {
            // The dialog was destroyed before delivery; the task still gets its
            // terminal update (ports the dispatch-failure branch).
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                wrote ? aida::ui::task_center::task_state_t::partial
                      : aida::ui::task_center::task_state_t::failed, 1.0f,
                "UI publication rejected", wrote
                    ? "The write and readback succeeded, but UI publication was rejected"
                    : "The mutation result could not be published"));
        }
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        cancellation->store(true, std::memory_order_release);
        review_.status = qt_write_review_status_t::failed;
        review_.error = "Worker queue rejected the reviewed mutation: " +
            submitted.reject_reason;
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Worker queue rejected", submitted.reject_reason));
        pollWorker();
        return false;
    }
    pollWorker();
    return true;
}

void QtWriteReviewDialog::publishResult(bool wrote, const std::string& result_error,
                                        bool mutation_may_remain,
                                        std::uint64_t serial,
                                        std::uint64_t workspace_generation,
                                        std::uint64_t structure_identity,
                                        std::uint64_t base_address,
                                        int structure_index, int field_index,
                                        const std::vector<std::uint8_t>& new_bytes,
                                        const std::string& task_id,
                                        std::uint32_t pid, bool cancelled) {
    // Verbatim port of the publish() lambda in submit_write_review.
    const auto workspace = review_.workspace.lock();
    const auto process = workspace ? workspace->identity().process() : std::nullopt;
    bool structure_current = false;
    {
        std::lock_guard<std::mutex> lock(struct_dissector::g_state.mtx);
        structure_current = struct_dissector::g_state.active_struct == structure_index &&
            struct_dissector::g_state.base_address == base_address &&
            structure_identity_locked(structure_index) == structure_identity &&
            struct_dissector::valid_index(structure_index,
                struct_dissector::g_state.structs.size()) &&
            struct_dissector::valid_index(field_index,
                struct_dissector::g_state.structs[
                    static_cast<std::size_t>(structure_index)].fields.size());
    }
    const bool target_current = workspace && !workspace->closing() &&
        !workspace->closed() && workspace->generation() == workspace_generation &&
        process && process->pid == pid;
    if (review_.serial != serial)
        return;
    if (!target_current || !structure_current) {
        review_.status = qt_write_review_status_t::stale;
        review_.mutation_may_remain = wrote;
        review_.error = wrote
            ? "The write passed exact readback, but the structure, field, target, or workspace changed before publication. Review an inverse write before continuing."
            : "The structure, field, target, or workspace changed before publication.";
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            wrote ? aida::ui::task_center::task_state_t::partial
                  : aida::ui::task_center::task_state_t::cancelled, 1.0f,
            "Discarded stale mutation result", review_.error));
        pollWorker();
        return;
    }
    if (wrote) {
        review_.status = qt_write_review_status_t::succeeded;
        review_.error.clear();
        review_.mutation_may_remain = false;
        struct_dissector::refresh_values();
        QtAnalysisBridge::instance().toastInfo(QStringLiteral(
            "Reviewed field write completed and passed exact readback."), 3.0);
    } else if (cancelled) {
        review_.status = qt_write_review_status_t::cancelled;
        review_.error = "The reviewed mutation was cancelled.";
    } else {
        review_.status = qt_write_review_status_t::failed;
        review_.error = result_error;
        review_.mutation_may_remain = mutation_may_remain;
    }
    static_cast<void>(aida::ui::task_center::update_task(task_id,
        review_.status == qt_write_review_status_t::succeeded
            ? aida::ui::task_center::task_state_t::completed
            : review_.mutation_may_remain
                ? aida::ui::task_center::task_state_t::partial
                : review_.status == qt_write_review_status_t::cancelled
                    ? aida::ui::task_center::task_state_t::cancelled
                    : aida::ui::task_center::task_state_t::failed,
        1.0f, review_.status == qt_write_review_status_t::succeeded
            ? "Mutation verified" : review_.mutation_may_remain
                ? "Mutation outcome requires review" : "Mutation not applied",
        review_.status == qt_write_review_status_t::succeeded
            ? "Exact readback matched reviewed bytes" : review_.error));
    pollWorker();
}

}
