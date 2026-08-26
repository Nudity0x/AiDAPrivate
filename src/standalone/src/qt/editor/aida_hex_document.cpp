#include "qt/editor/aida_hex_document.hpp"

#include <QTimer>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>

#include "core/analysis/workspace/overlay_journal.hpp"
#include "core/infra/executor.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/ui/task_center.hpp"
#include "helpers/diag_log.hpp"

namespace aida::qt::editor {

AidaHexDocument::AidaHexDocument(QObject* parent) : QObject(parent)
{
}

AidaHexDocument::~AidaHexDocument()
{
    request_cancel();
}

void AidaHexDocument::request_cancel() noexcept
{
    cancelled.store(true, std::memory_order_release);
    std::shared_ptr<aida::analysis::cancellation_source_t> search;
    aida::infra::taskflow_runtime::job_handle_t patch;
    aida::infra::taskflow_runtime::job_handle_t search_task;
    {
        std::lock_guard<std::mutex> lock(mutex);
        search = search_cancellation;
        search_cancellation.reset();
        if (live_cancellation)
            live_cancellation->store(true, std::memory_order_release);
        live_cancellation.reset();
        patch = patch_job;
        search_task = search_job;
        patch_job = {};
        search_job = {};
        window = {};
        window_size = 0;
        live_bytes.clear();
    }
    live_loading.store(false, std::memory_order_release);
    live_request_serial.fetch_add(1, std::memory_order_acq_rel);
    if (search)
        search->request_cancel();
    if (patch.valid())
        aida::infra::taskflow_runtime::cancel(patch);
    if (search_task.valid())
        aida::infra::taskflow_runtime::cancel(search_task);
    AidaHexDocumentRegistry::instance().unregisterState(owner_id, this);
}

aida::analysis::workspace_result_t<void> AidaHexDocument::drain(
    std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock<std::mutex> lock(drain_mutex);
    if (!drain_cv.wait_until(lock, deadline, [this] {
        return pending_jobs.load(std::memory_order_acquire) == 0;
    }))
        return aida::analysis::workspace_result_t<void>::failure(
            aida::analysis::make_workspace_error(
                aida::analysis::workspace_error_code_t::deadline_exceeded,
                "hex view cancellation reached its workspace close deadline", "hex_view"));
    return aida::analysis::workspace_result_t<void>::success();
}

bool AidaHexDocument::stateMatches(const disasm_view::workspace_context_t& context) const
{
    if (!context.workspace || cancelled.load(std::memory_order_acquire))
        return false;
    const auto owner_workspace = owner.lock();
    return owner_workspace && owner_workspace == context.workspace &&
        !owner_workspace->closing() && !owner_workspace->closed();
}

void AidaHexDocument::consumeDispatchFailure()
{
    const std::uint64_t dispatch_failure = live_dispatch_failure_serial.exchange(
        0, std::memory_order_acq_rel);
    if (dispatch_failure != 0 &&
        dispatch_failure == live_request_serial.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = "PUBLICATION_REJECTED: the UI dispatcher rejected the completed live-memory read";
        }
        Q_EMIT errorChanged();
    }
}

void AidaHexDocument::activate(const disasm_view::workspace_context_t& context)
{
    if (live_cancellation)
        live_cancellation->store(true, std::memory_order_release);
    live_cancellation.reset();
    live_request_serial.fetch_add(1, std::memory_order_acq_rel);
    live_loading.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex);
        source_kind = hex_source_kind_t::workspace_provider;
        live_bytes.clear();
        live_base = 0;
        ui.base_addr = context.workspace->identity().image_base();
        ui.source_name = context.workspace->identity().bin_name();
        if (const auto& member = context.workspace->identity().normalized_member_path())
            ui.source_name.append("::").append(*member);
        ui.active = true;
        window = {};
        window_size = 0;
        patch_revision = (std::numeric_limits<std::uint64_t>::max)();
        patches.clear();
        error.clear();
    }
    Q_EMIT stateChanged();
}

bool AidaHexDocument::focusAddress(const disasm_view::workspace_context_t& context,
    const aida::analysis::address_t& address, std::string* out_error)
{
    if (!context.workspace) {
        if (out_error) *out_error = "The selected workspace has no hex provider.";
        return false;
    }
    const auto offset = disasm_view::provider_offset(context, address);
    if (!offset || *offset >= context.workspace->provider().size() ||
        *offset > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        if (out_error) *out_error = "The selected address has no mapped file or provider offset.";
        return false;
    }
    activate(context);
    {
        std::lock_guard<std::mutex> lock(mutex);
        scroll_to_offset = *offset;
        ui.sel_start = static_cast<std::int64_t>(*offset);
        ui.sel_end = static_cast<std::int64_t>(*offset);
        error.clear();
        if (out_error) out_error->clear();
    }
    Q_EMIT stateChanged();
    return true;
}

bool AidaHexDocument::isActive(const disasm_view::workspace_context_t& context)
{
    std::lock_guard<std::mutex> lock(mutex);
    return ui.active && (source_kind == hex_source_kind_t::live_memory
        ? !live_bytes.empty() : context.workspace->provider().size() != 0);
}

std::string AidaHexDocument::sourceName(const disasm_view::workspace_context_t& context)
{
    std::lock_guard<std::mutex> lock(mutex);
    return ui.source_name.empty() ? context.workspace->identity().bin_name() : ui.source_name;
}

std::string AidaHexDocument::lastError()
{
    std::lock_guard<std::mutex> lock(mutex);
    return error;
}

std::uint64_t AidaHexDocument::byteCount(const disasm_view::workspace_context_t& context)
{
    std::lock_guard<std::mutex> lock(mutex);
    return source_kind == hex_source_kind_t::live_memory
        ? static_cast<std::uint64_t>(live_bytes.size())
        : context.workspace->provider().size();
}

AidaHexDocumentRegistry& AidaHexDocumentRegistry::instance()
{
    static AidaHexDocumentRegistry* registry = new AidaHexDocumentRegistry();
    return *registry;
}

AidaHexDocumentRegistry::AidaHexDocumentRegistry(QObject* parent) : QObject(parent)
{
    sweep_timer_ = new QTimer(this);
    sweep_timer_->setInterval(250);
    connect(sweep_timer_, &QTimer::timeout, this,
            &AidaHexDocumentRegistry::sweepDispatchFailures);
    sweep_timer_->start();
}

AidaHexDocumentRegistry::~AidaHexDocumentRegistry() = default;

void AidaHexDocumentRegistry::sweepDispatchFailures()
{
    std::vector<std::shared_ptr<AidaHexDocument>> snapshot;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        snapshot.reserve(documents_.size());
        for (const auto& entry : documents_)
            snapshot.push_back(entry.second);
    }
    for (const auto& document : snapshot)
        document->consumeDispatchFailure();
}

void AidaHexDocumentRegistry::unregisterState(const aida::analysis::binary_id_t& id,
    const AidaHexDocument* state)
{
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found = documents_.find(id);
    if (found != documents_.end() && found->second.get() == state)
        documents_.erase(found);
}

std::shared_ptr<AidaHexDocument> AidaHexDocumentRegistry::find(
    const aida::analysis::binary_id_t& id)
{
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found = documents_.find(id);
    return found == documents_.end() ? nullptr : found->second;
}

std::shared_ptr<AidaHexDocument> AidaHexDocumentRegistry::stateFor(
    const disasm_view::workspace_context_t& context)
{
    if (!context.workspace || context.workspace->closing() || context.workspace->closed())
        return {};
    const auto id = context.workspace->identity().binary_id();
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto found = documents_.find(id);
        if (found != documents_.end() && found->second->stateMatches(context))
            return found->second;
        if (found != documents_.end())
            documents_.erase(found);
    }
    auto created = std::shared_ptr<AidaHexDocument>(new AidaHexDocument(),
        [](AidaHexDocument* document) { document->deleteLater(); });
    created->owner = context.workspace;
    created->owner_id = id;
    created->ui.active = true;
    created->ui.base_addr = context.workspace->identity().image_base();
    created->ui.source_name = context.workspace->identity().bin_name();
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        documents_.emplace(id, created);
    }
    auto registered = context.workspace->register_lifecycle_participant(created);
    if (!registered) {
        created->request_cancel();
        return {};
    }
    return created;
}

void AidaHexDocumentRegistry::close(const disasm_view::workspace_context_t& context)
{
    auto state = stateFor(context);
    if (!state)
        return;
    state->request_cancel();
}

std::optional<std::vector<std::uint8_t>> hex_parse_bytes(std::string text)
{
    std::vector<std::uint8_t> output;
    int high = -1;
    for (char character : text) {
        if (std::isspace(static_cast<unsigned char>(character)))
            continue;
        int value = -1;
        if (character >= '0' && character <= '9')
            value = character - '0';
        else if (character >= 'a' && character <= 'f')
            value = character - 'a' + 10;
        else if (character >= 'A' && character <= 'F')
            value = character - 'A' + 10;
        else
            return {};
        if (high < 0)
            high = value;
        else {
            output.push_back(static_cast<std::uint8_t>((high << 4) | value));
            high = -1;
        }
    }
    if (high >= 0 || output.empty())
        return {};
    return output;
}

std::vector<std::uint8_t> hex_search_pattern(const hex_ui_state_t& state)
{
    if (state.search_hex) {
        auto parsed = hex_parse_bytes(state.search_buf);
        return parsed ? std::move(*parsed) : std::vector<std::uint8_t>();
    }
    const auto* begin = reinterpret_cast<const std::uint8_t*>(state.search_buf);
    return std::vector<std::uint8_t>(begin, begin + std::strlen(state.search_buf));
}

std::optional<std::uint64_t> hex_parse_u64(std::string text)
{
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text.erase(0, 2);
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return result.ec == std::errc() && result.ptr == text.data() + text.size()
        ? std::optional<std::uint64_t>(value) : std::nullopt;
}

void AidaHexDocument::requestPatchRefresh(const disasm_view::workspace_context_t& context)
{
    if (!stateMatches(context))
        return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (source_kind != hex_source_kind_t::workspace_provider ||
            patch_revision == context.workspace->overlay_revision())
            return;
    }
    if (patch_refreshing.exchange(true, std::memory_order_acq_rel))
        return;
    const std::string target_id = context.workspace->identity().binary_id().to_hex();
    auto pending = std::make_shared<pending_job_guard_t>(
        std::static_pointer_cast<AidaHexDocument>(shared_from_this()));
    aida::infra::taskflow_runtime::task_descriptor_t descriptor;
    descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
    descriptor.owner_subsystem = "hex_view";
    descriptor.label = "refresh_workspace_patches";
    descriptor.target_id = target_id.c_str();
    descriptor.generation = context.publication->generation;
    descriptor.cancellable_body = [context, pending](
        const aida::infra::taskflow_runtime::cancellation_token_t& cancel) {
        auto state = pending->state;
        std::vector<hex_patch_span_t> patches;
        if (!cancel.requested.load(std::memory_order_acquire) && state->stateMatches(context)) {
            if (auto overlay = context.workspace->overlay()) {
                const auto operations = overlay->patch_operations();
                patches.reserve(operations.size());
                for (const auto& operation : operations) {
                    if (cancel.requested.load(std::memory_order_acquire))
                        break;
                    const auto offset = disasm_view::provider_offset(context, operation.address);
                    if (!offset || operation.bytes.empty())
                        continue;
                    patches.push_back({*offset, operation.bytes});
                }
                std::sort(patches.begin(), patches.end(), [](const auto& left, const auto& right) {
                    return left.offset < right.offset;
                });
            }
        }
        if (state->stateMatches(context)) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->patches = std::move(patches);
            state->patch_revision = context.workspace->overlay_revision();
        }
        state->patch_refreshing.store(false, std::memory_order_release);
        const bool posted = QMetaObject::invokeMethod(state.get(), [state] {
            Q_EMIT state->stateChanged();
        }, Qt::QueuedConnection);
        if (!posted)
            diag::log_tagged("hex_view",
                "patch-refresh publication rejected by the UI dispatcher");
    };
    const auto submitted = aida::infra::taskflow_runtime::submit(std::move(descriptor));
    if (!submitted.submitted) {
        patch_refreshing.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = submitted.reject_reason;
        }
        Q_EMIT errorChanged();
    } else {
        bool cancel_submitted = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cancel_submitted = cancelled.load(std::memory_order_acquire);
            if (!cancel_submitted)
                patch_job = submitted.handle;
        }
        if (cancel_submitted)
            aida::infra::taskflow_runtime::cancel(submitted.handle);
    }
}

bool AidaHexDocument::ensureWindow(const disasm_view::workspace_context_t& context,
    std::uint64_t begin, std::uint64_t end)
{
    if (!stateMatches(context) || begin >= end)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (source_kind == hex_source_kind_t::live_memory)
            return end <= live_bytes.size();
        if (end > context.workspace->provider().size())
            return false;
        if (!window.empty() && begin >= window_offset &&
            end <= window_offset + window_size)
            return true;
    }
    constexpr std::uint64_t window_capacity = 4ULL << 20;
    const std::uint64_t aligned = begin & ~static_cast<std::uint64_t>(0xFFFF);
    const std::uint64_t required = end - aligned;
    const std::uint64_t available = context.workspace->provider().size() - aligned;
    const std::uint64_t length = (std::min)(available,
        (std::max)(required, window_capacity));
    auto lease = context.workspace->provider().lease(aligned, length,
        context.workspace->cancellation_token());
    if (!lease) {
        const std::string new_error = lease.error().stable_code() + ": " + lease.error().message;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            changed = error != new_error;
            error = new_error;
            window = {};
            window_size = 0;
        }
        if (changed)
            Q_EMIT errorChanged();
        return false;
    }
    bool had_error = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        window = lease.take_value();
        window_offset = aligned;
        window_size = length;
        had_error = !error.empty();
        error.clear();
    }
    if (had_error)
        Q_EMIT errorChanged();
    return true;
}

std::uint8_t AidaHexDocument::patchedByte(std::uint64_t offset, std::uint8_t original,
    bool* patched) const
{
    if (patched)
        *patched = false;
    auto found = std::upper_bound(patches.begin(), patches.end(), offset,
        [](std::uint64_t value, const hex_patch_span_t& span) {
            return value < span.offset;
        });
    if (found == patches.begin())
        return original;
    --found;
    if (offset < found->offset || offset - found->offset >= found->bytes.size())
        return original;
    if (patched)
        *patched = true;
    return found->bytes[static_cast<std::size_t>(offset - found->offset)];
}

void AidaHexDocument::startSearch(const disasm_view::workspace_context_t& context)
{
    std::vector<std::uint8_t> pattern;
    std::vector<std::uint8_t> live_source;
    std::uint64_t serial = 0;
    std::shared_ptr<aida::analysis::cancellation_source_t> cancellation;
    bool pattern_valid = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pattern = hex_search_pattern(ui);
        pattern_valid = !pattern.empty();
        if (pattern_valid) {
            if (search_cancellation)
                search_cancellation->request_cancel();
            cancellation = std::make_shared<aida::analysis::cancellation_source_t>();
            search_cancellation = cancellation;
            serial = search_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
            ui.search_matches.clear();
            ui.search_match = -1;
            ui.search_match_idx = -1;
            ui.search_match_len = pattern.size();
            error.clear();
            if (source_kind == hex_source_kind_t::live_memory)
                live_source = live_bytes;
        } else {
            error = "Search pattern is empty or malformed.";
        }
    }
    if (!pattern_valid) {
        Q_EMIT errorChanged();
        return;
    }
    searching.store(true, std::memory_order_release);
    Q_EMIT searchStateChanged();
    const std::string target_id = context.workspace->identity().binary_id().to_hex();
    auto pending = std::make_shared<pending_job_guard_t>(
        std::static_pointer_cast<AidaHexDocument>(shared_from_this()));
    aida::infra::taskflow_runtime::task_descriptor_t descriptor;
    descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
    descriptor.owner_subsystem = "hex_view";
    descriptor.label = "search_workspace_bytes";
    descriptor.target_id = target_id.c_str();
    descriptor.generation = context.publication->generation;
    descriptor.cancellable_body = [context, pending, pattern = std::move(pattern),
                                   live_source = std::move(live_source), cancellation, serial](
        const aida::infra::taskflow_runtime::cancellation_token_t& runtime_cancel) {
        auto state = pending->state;
        constexpr std::uint64_t chunk_size = 4ULL << 20;
        constexpr std::size_t maximum_matches = 100000;
        std::vector<std::uint64_t> matches;
        std::uint64_t cursor = 0;
        if (!live_source.empty() && state->stateMatches(context) &&
            !runtime_cancel.requested.load(std::memory_order_acquire) &&
            !cancellation->token().stop_requested()) {
            auto found = std::search(live_source.begin(), live_source.end(),
                pattern.begin(), pattern.end());
            while (found != live_source.end() && matches.size() < maximum_matches &&
                   !runtime_cancel.requested.load(std::memory_order_acquire) &&
                   !cancellation->token().stop_requested()) {
                matches.push_back(static_cast<std::uint64_t>(
                    std::distance(live_source.begin(), found)));
                found = std::search(found + 1, live_source.end(), pattern.begin(), pattern.end());
            }
        }
        while (live_source.empty() && cursor < context.workspace->provider().size() &&
               matches.size() < maximum_matches &&
               state->stateMatches(context) &&
               !runtime_cancel.requested.load(std::memory_order_acquire) &&
               !cancellation->token().stop_requested()) {
            const std::uint64_t remaining = context.workspace->provider().size() - cursor;
            const std::uint64_t length = (std::min)(remaining, chunk_size);
            auto lease = context.workspace->provider().lease(cursor, length,
                cancellation->token());
            if (!lease)
                break;
            const auto& view = lease.value();
            if (view.size() >= pattern.size()) {
                auto found = std::search(view.begin(), view.end(), pattern.begin(), pattern.end());
                while (found != view.end() && matches.size() < maximum_matches) {
                    matches.push_back(cursor + static_cast<std::uint64_t>(
                        std::distance(view.begin(), found)));
                    found = std::search(found + 1, view.end(), pattern.begin(), pattern.end());
                }
            }
            if (length == remaining)
                break;
            const std::uint64_t overlap = pattern.size() > 1 ? pattern.size() - 1 : 0;
            cursor += length - (std::min)(length - 1, overlap);
        }
        if (state->stateMatches(context)) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->search_serial.load(std::memory_order_acquire) == serial) {
                state->ui.search_matches = std::move(matches);
                state->ui.search_match_idx = state->ui.search_matches.empty() ? -1 : 0;
                state->ui.search_match = state->ui.search_matches.empty() ? -1 :
                    static_cast<std::int64_t>(state->ui.search_matches.front());
                if (!state->ui.search_matches.empty())
                    state->scroll_to_offset = state->ui.search_matches.front();
            }
        }
        state->searching.store(false, std::memory_order_release);
        const bool posted = QMetaObject::invokeMethod(state.get(), [state] {
            Q_EMIT state->searchStateChanged();
            Q_EMIT state->stateChanged();
        }, Qt::QueuedConnection);
        if (!posted)
            diag::log_tagged("hex_view",
                "search publication rejected by the UI dispatcher");
    };
    const auto submitted = aida::infra::taskflow_runtime::submit(std::move(descriptor));
    if (!submitted.submitted) {
        searching.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = submitted.reject_reason;
        }
        Q_EMIT errorChanged();
    } else {
        bool cancel_submitted = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cancel_submitted = cancelled.load(std::memory_order_acquire);
            if (!cancel_submitted)
                search_job = submitted.handle;
        }
        if (cancel_submitted)
            aida::infra::taskflow_runtime::cancel(submitted.handle);
    }
}

void AidaHexDocument::stepSearchResult(int direction)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (ui.search_matches.empty())
            return;
        const std::int64_t count = static_cast<std::int64_t>((std::min)(
            ui.search_matches.size(),
            static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())));
        std::int64_t index = ui.search_match_idx;
        if (index < 0 || index >= count)
            index = direction < 0 ? count - 1 : 0;
        else
            index = (index + (direction < 0 ? -1 : 1) + count) % count;
        ui.search_match_idx = index;
        ui.search_match = static_cast<std::int64_t>(
            ui.search_matches[static_cast<std::size_t>(index)]);
        scroll_to_offset = ui.search_matches[static_cast<std::size_t>(index)];
    }
    Q_EMIT searchStateChanged();
}

bool AidaHexDocument::requestLiveMemory(const disasm_view::workspace_context_t& context,
    std::uint64_t address, std::size_t size)
{
    if (!context.workspace)
        return false;
    if (!context.workspace->identity().process() || address == 0 || size == 0 ||
        size > (64ULL << 20) || address > (std::numeric_limits<std::uint64_t>::max)() -
            static_cast<std::uint64_t>(size - 1)) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = "INVALID_ARGUMENT: live-memory preview requires a bound process, address, and bounded size";
        }
        Q_EMIT errorChanged();
        return false;
    }
    const std::uint32_t pid = context.workspace->identity().process()->pid;
    const std::uint64_t generation = context.workspace->generation();
    const std::uint64_t serial = live_request_serial.fetch_add(1,
        std::memory_order_acq_rel) + 1;
    auto cancellation = std::make_shared<std::atomic<bool>>(false);
    const std::string task_id = "hex.live." + context.workspace->identity().binary_id().to_hex() +
        "." + std::to_string(serial);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (live_cancellation)
            live_cancellation->store(true, std::memory_order_release);
        live_cancellation = cancellation;
        source_kind = hex_source_kind_t::live_memory;
        live_bytes.clear();
        live_base = address;
        ui.base_addr = address;
        ui.source_name = context.workspace->identity().bin_name() + " memory";
        ui.active = true;
        ui.sel_start = -1;
        ui.sel_end = -1;
        window = {};
        window_size = 0;
        patches.clear();
        error.clear();
    }
    live_loading.store(true, std::memory_order_release);
    Q_EMIT liveStateChanged();

    aida::ui::task_center::task_registration_t registration;
    registration.id = task_id;
    registration.source = "hex_view";
    registration.owner = "Hex Editor";
    registration.owner_view = "document.hex";
    registration.owner_action = "Open live memory";
    registration.target = "PID " + std::to_string(pid);
    registration.label = "Read live memory";
    registration.stage = "Queued bounded read";
    char range_label[80]{};
    std::snprintf(range_label, sizeof(range_label), "0x%016llX (%zu bytes)",
        static_cast<unsigned long long>(address), size);
    registration.affected_entity = range_label;
    registration.cancellation_is_safe = true;
    registration.callbacks.cancel = [cancellation] {
        bool expected = false;
        return cancellation->compare_exchange_strong(expected, true,
            std::memory_order_acq_rel);
    };
    if (!aida::ui::task_center::register_task(std::move(registration))) {
        cancellation->store(true, std::memory_order_release);
        live_loading.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = "TASK_OWNERSHIP_FAILURE: Task Center rejected the live-memory read";
        }
        Q_EMIT errorChanged();
        return false;
    }

    auto result = std::make_shared<std::vector<std::uint8_t>>();
    auto failure = std::make_shared<std::string>();
    auto workspace = context.workspace;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "hex_view";
    submission.label = "hex.live_memory.read";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 4;
    submission.target_pid = pid;
    submission.generation = generation;
    submission.cancel_hook = [cancellation] {
        cancellation->store(true, std::memory_order_release);
    };
    submission.body = [state = shared_from_this(), workspace, cancellation, result, failure,
        task_id, pid, address, size, generation, serial]() {
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::running, 0.1f,
            "Reading exact bounded range"));
        pending_job_guard_t pending(state);
        if (cancellation->load(std::memory_order_acquire)) {
            *failure = "CANCELLED: live-memory read was cancelled";
        } else {
            if (!driver_bridge::read_memory_for(pid, address, size, *result) ||
                result->size() != size)
                *failure = "IO_FAILURE: bounded live-memory read failed or returned a partial range";
        }
        if (cancellation->load(std::memory_order_acquire) && failure->empty())
            *failure = "CANCELLED: live-memory read was cancelled";
        auto publish = [state, workspace, cancellation, result, failure, task_id,
            pid, address, generation, serial]() {
            const auto process = workspace ? workspace->identity().process() : std::nullopt;
            const bool current = workspace && !workspace->closing() && !workspace->closed() &&
                workspace->generation() == generation && process && process->pid == pid &&
                state->live_request_serial.load(std::memory_order_acquire) == serial &&
                !state->cancelled.load(std::memory_order_acquire);
            if (!current) {
                if (state->live_request_serial.load(std::memory_order_acquire) == serial)
                    state->live_loading.store(false, std::memory_order_release);
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    aida::ui::task_center::task_state_t::cancelled, 1.0f,
                    "Discarded stale publication", "Workspace, target, or request changed"));
                return;
            }
            state->live_loading.store(false, std::memory_order_release);
            std::string publish_error;
            std::size_t published_size = 0;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (cancellation->load(std::memory_order_acquire) || !failure->empty()) {
                    state->error = failure->empty() ? "CANCELLED: live-memory read was cancelled" : *failure;
                    publish_error = state->error;
                } else {
                    state->live_bytes = std::move(*result);
                    state->live_base = address;
                    state->error.clear();
                    published_size = state->live_bytes.size();
                }
            }
            if (!publish_error.empty()) {
                static_cast<void>(aida::ui::task_center::update_task(task_id,
                    cancellation->load(std::memory_order_acquire)
                        ? aida::ui::task_center::task_state_t::cancelled
                        : aida::ui::task_center::task_state_t::failed,
                    1.0f, "Live-memory read did not publish", publish_error));
                Q_EMIT state->errorChanged();
                return;
            }
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::completed, 1.0f,
                "Exact range published", std::to_string(published_size) + " bytes"));
            Q_EMIT state->liveStateChanged();
            Q_EMIT state->stateChanged();
        };
        const bool posted = QMetaObject::invokeMethod(state.get(), std::move(publish),
            Qt::QueuedConnection);
        if (!posted) {
            state->live_dispatch_failure_serial.store(serial, std::memory_order_release);
            state->live_loading.store(false, std::memory_order_release);
            static_cast<void>(aida::ui::task_center::update_task(task_id,
                aida::ui::task_center::task_state_t::failed, 1.0f,
                "UI publication rejected", "The UI dispatcher rejected the completed read"));
        }
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        cancellation->store(true, std::memory_order_release);
        live_loading.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = "QUEUE_REJECTED: " + submitted.reject_reason;
        }
        static_cast<void>(aida::ui::task_center::update_task(task_id,
            aida::ui::task_center::task_state_t::failed, 1.0f,
            "Worker queue rejected", submitted.reject_reason));
        Q_EMIT errorChanged();
        return false;
    }
    return true;
}

}
