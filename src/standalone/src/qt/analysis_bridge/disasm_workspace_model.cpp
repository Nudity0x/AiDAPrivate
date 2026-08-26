#include "qt/analysis_bridge/disasm_workspace_model.hpp"

#include "qt/analysis_bridge/gui_post.hpp"
#include "qt/analysis_bridge/revision_combine.hpp"

#include "core/analysis/auto_comment_store.hpp"
#include "core/analysis/workspace/overlay_journal.hpp"
#include "core/analysis/workspace/publication_indexes.hpp"
#include "core/analysis/workspace/workspace_registry.hpp"
#include "core/analysis/workspace/x86_decoder.hpp"
#include "core/workbench/workbench_shell_integration.hpp"
#include "core/infra/taskflow_runtime.hpp"
#include "core/ui/task_center.hpp"
#include "core/debugger/debugger_view.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "helpers/diag_log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <windows.h>

namespace disasm_view {

struct workspace_model_t {
    workspace_model_t(const aida::analysis::binary_id_t& id,
                      const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace)
        : automatic_comments(id), view(std::make_shared<state_t>()), owner(workspace) {}

    auto_comment_store::workspace_store_t automatic_comments;
    std::shared_ptr<state_t> view;
    std::unordered_map<std::string, std::shared_ptr<state_t>> presentations;
    std::unordered_map<const state_t*, int> section_filter_mirror;
    std::weak_ptr<aida::analysis::analysis_workspace_t> owner;
    std::mutex mutation_mutex;
    std::mutex initialization_mutex;
    std::atomic<bool> initialized{false};
    std::atomic<std::uint32_t> format_generation{1};
    std::atomic<std::uint64_t> presentation_selection_revision{0};
};

namespace {

using namespace aida::analysis;
using aida::qt::gui_post_or_run;

std::mutex& model_registry_mutex() {
    static std::mutex value;
    return value;
}

std::unordered_map<binary_id_t, std::shared_ptr<workspace_model_t>, binary_id_hash_t>&
model_registry() {
    static std::unordered_map<binary_id_t, std::shared_ptr<workspace_model_t>, binary_id_hash_t> value;
    return value;
}

std::shared_ptr<workspace_model_t> model_for(const std::shared_ptr<analysis_workspace_t>& workspace) {
    if (!workspace)
        return {};
    const auto id = workspace->identity().binary_id();
    std::lock_guard<std::mutex> lock(model_registry_mutex());
    auto& registry = model_registry();
    for (auto it = registry.begin(); it != registry.end();) {
        const auto owner = it->second ? it->second->owner.lock() : nullptr;
        if (!owner || owner->closed())
            it = registry.erase(it);
        else
            ++it;
    }
    auto found = registry.find(id);
    if (found != registry.end())
        return found->second;
    auto created = std::make_shared<workspace_model_t>(id, workspace);
    registry[id] = created;
    return created;
}

void initialize_model(const std::shared_ptr<analysis_workspace_t>& workspace,
                      const std::shared_ptr<workspace_model_t>& model) {
    if (!workspace || !model || model->initialized.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(model->initialization_mutex);
    if (model->initialized.load(std::memory_order_acquire))
        return;
    model->initialized.store(true, std::memory_order_release);
}

std::string normalize_presentation_key(std::string_view key) {
    if (key == "primary")
        return {};
    return std::string(key);
}

std::shared_ptr<state_t> presentation_for(
    const std::shared_ptr<workspace_model_t>& model, std::string_view key) {
    const std::string identity = normalize_presentation_key(key);
    if (!model || identity.empty())
        return model ? model->view : nullptr;
    std::lock_guard<std::mutex> lock(model->initialization_mutex);
    const auto found = model->presentations.find(identity);
    if (found != model->presentations.end())
        return found->second;
    auto created = std::make_shared<state_t>();
    model->presentations.emplace(identity, created);
    return created;
}

std::shared_ptr<state_t> authoritative_state(const workspace_context_t& context) {
    return context.model ? context.model->view : context.view;
}

workspace_context_t authoritative_context(const workspace_context_t& context) {
    workspace_context_t result = context;
    result.view = authoritative_state(context);
    return result;
}

struct delivery_targets_t {
    std::weak_ptr<aida::qt::analysis_bridge::format_delivery_fn> format;
    std::weak_ptr<aida::qt::analysis_bridge::xref_delivery_fn> xref;
    std::weak_ptr<aida::qt::analysis_bridge::export_delivery_fn> export_status;
    std::weak_ptr<aida::qt::analysis_bridge::view_hooks_t> hooks;
};

std::mutex& delivery_registry_mutex() {
    static std::mutex value;
    return value;
}

std::unordered_map<state_t*, delivery_targets_t>& delivery_registry() {
    static std::unordered_map<state_t*, delivery_targets_t> value;
    return value;
}

void note_ui_state_changed(const std::shared_ptr<state_t>& view) {
    if (view)
        view->ui_serial.fetch_add(1, std::memory_order_acq_rel);
}

void deliver_format_page(const workspace_context_t& context,
                         aida::qt::analysis_bridge::format_page_delivery_t payload) {
    std::weak_ptr<aida::qt::analysis_bridge::format_delivery_fn> target;
    {
        std::lock_guard<std::mutex> lock(delivery_registry_mutex());
        const auto found = delivery_registry().find(context.view.get());
        if (found != delivery_registry().end())
            target = found->second.format;
    }
    const auto fn = target.lock();
    if (!fn)
        return;
    gui_post_or_run([fn, payload = std::move(payload)]() mutable {
        (*fn)(std::move(payload));
    });
}

void deliver_format_reset(const workspace_context_t& context) {
    aida::qt::analysis_bridge::format_page_delivery_t payload;
    payload.generation = context.publication ? context.publication->generation : 0;
    payload.analysis_revision =
        context.publication ? context.publication->analysis_revision : 0;
    payload.overlay_revision =
        context.publication ? context.publication->overlay_revision : 0;
    payload.reset = true;
    deliver_format_page(context, std::move(payload));
}

void deliver_xref_results(const workspace_context_t& context,
                          aida::qt::analysis_bridge::xref_delivery_t payload) {
    std::weak_ptr<aida::qt::analysis_bridge::xref_delivery_fn> target;
    {
        std::lock_guard<std::mutex> lock(delivery_registry_mutex());
        const auto found = delivery_registry().find(context.view.get());
        if (found != delivery_registry().end())
            target = found->second.xref;
    }
    const auto fn = target.lock();
    if (!fn)
        return;
    gui_post_or_run([fn, payload = std::move(payload)]() mutable {
        (*fn)(std::move(payload));
    });
}

void deliver_export_status(const workspace_context_t& context,
                           aida::qt::analysis_bridge::export_delivery_t payload) {
    std::weak_ptr<aida::qt::analysis_bridge::export_delivery_fn> target;
    {
        std::lock_guard<std::mutex> lock(delivery_registry_mutex());
        const auto found = delivery_registry().find(context.view.get());
        if (found != delivery_registry().end())
            target = found->second.export_status;
    }
    const auto fn = target.lock();
    if (!fn)
        return;
    gui_post_or_run([fn, payload = std::move(payload)]() mutable {
        (*fn)(std::move(payload));
    });
}

std::shared_ptr<aida::qt::analysis_bridge::view_hooks_t> view_hooks_for(
    const std::shared_ptr<state_t>& view) {
    std::lock_guard<std::mutex> lock(delivery_registry_mutex());
    const auto found = delivery_registry().find(view.get());
    if (found == delivery_registry().end())
        return {};
    return found->second.hooks.lock();
}

std::optional<std::uint64_t> checked_add(std::uint64_t left, std::uint64_t right) {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left)
        return {};
    return left + right;
}

std::optional<std::uint64_t> optional_value(
    aida::analysis::workspace_result_t<std::uint64_t> result) {
    if (!result)
        return {};
    return result.take_value();
}

std::string byte_text(const byte_view_t& view, std::uint64_t view_offset,
                      std::uint64_t instruction_offset, std::uint8_t length) {
    if (instruction_offset < view_offset)
        return {};
    const std::uint64_t relative = instruction_offset - view_offset;
    if (relative > view.size() || length > view.size() - static_cast<std::size_t>(relative))
        return {};
    std::string output;
    output.reserve(static_cast<std::size_t>(length) * 3);
    char buffer[4]{};
    for (std::uint8_t index = 0; index < length; ++index) {
        std::snprintf(buffer, sizeof(buffer), "%02X",
            view[static_cast<std::size_t>(relative) + index]);
        if (!output.empty())
            output.push_back(' ');
        output.append(buffer);
    }
    return output;
}

void publish_format_failure(const workspace_context_t& context,
                            std::uint64_t page_key, std::string error) {
    if (!context.view)
        return;
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        context.view->pending_format_pages.erase(page_key);
        context.view->format_error = std::move(error);
    }
    note_ui_state_changed(context.view);
}

bool operand_equals_ci(std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(),
        right.begin(), [](char l, char r) {
            return std::tolower(static_cast<unsigned char>(l)) ==
                std::tolower(static_cast<unsigned char>(r));
        });
}

bool operand_word_char(char value) {
    const auto current = static_cast<unsigned char>(value);
    return std::isalnum(current) != 0 || value == '_' || value == '.' ||
        value == '$' || value == '?' || value == '@' || value == '+' || value == '-';
}

operand_color_role_t classify_word_token(std::string_view token,
                                         bool& has_name_candidate) {
    static constexpr std::array<std::string_view, 33> registers{
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
        "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
        "ax", "bx", "cx", "dx", "al", "ah", "bl", "bh", "cl",
        "ch", "dl", "dh", "rip", "eip", "cs", "ds", "ss"};
    if (std::any_of(registers.begin(), registers.end(),
            [&](std::string_view value) { return operand_equals_ci(token, value); }) ||
        (token.size() >= 2 && (token[0] == 'r' || token[0] == 'R') &&
         std::isdigit(static_cast<unsigned char>(token[1])) != 0))
        return operand_color_role_t::reg;
    if (!token.empty() &&
        (std::isdigit(static_cast<unsigned char>(token.front())) != 0 ||
         token.front() == '-' || token.front() == '+'))
        return operand_color_role_t::imm;
    static constexpr std::array<std::string_view, 10> type_tokens{
        "byte", "word", "dword", "qword", "tbyte", "xmmword", "ymmword",
        "zmmword", "ptr", "offset"};
    if (std::any_of(type_tokens.begin(), type_tokens.end(),
            [&](std::string_view value) { return operand_equals_ci(token, value); }))
        return operand_color_role_t::keyword;
    has_name_candidate = true;
    return operand_color_role_t::name_candidate;
}

void tokenize_operands(std::string_view operands, std::uint32_t base_offset,
                       std::vector<operand_token_t>& output,
                       bool& has_name_sensitive_token) {
    has_name_sensitive_token = false;
    std::size_t position = 0;
    const std::size_t limit = (std::min)(operands.size(),
        static_cast<std::size_t>(512));
    while (position < limit) {
        const auto begin = position;
        if (operands[position] == '"' || operands[position] == '\'') {
            const char quote = operands[position++];
            while (position < limit && operands[position] != quote)
                ++position;
            if (position < limit)
                ++position;
            output.push_back(operand_token_t{
                base_offset + static_cast<std::uint32_t>(begin),
                static_cast<std::uint32_t>(position - begin),
                static_cast<std::uint8_t>(operand_color_role_t::string_ref)});
            continue;
        }
        const bool word = operand_word_char(operands[position]);
        ++position;
        while (position < limit && operand_word_char(operands[position]) == word)
            ++position;
        const auto token = operands.substr(begin, position - begin);
        operand_color_role_t role;
        if (word) {
            role = classify_word_token(token, has_name_sensitive_token);
        } else {
            role = (token.find('[') != std::string_view::npos ||
                    token.find(']') != std::string_view::npos)
                ? operand_color_role_t::reg_ptr : operand_color_role_t::plain;
        }
        output.push_back(operand_token_t{
            base_offset + static_cast<std::uint32_t>(begin),
            static_cast<std::uint32_t>(position - begin),
            static_cast<std::uint8_t>(role)});
    }
}

void tokenize_formatted_row(formatted_instruction_t& row) {
    const std::string_view line(row.text);
    const auto mnemonic_end = line.find_first_of(" \t");
    row.tokens.clear();
    if (mnemonic_end == std::string_view::npos) {
        row.mnemonic_end = line.size();
        return;
    }
    row.mnemonic_end = mnemonic_end;
    bool has_name_sensitive_token = false;
    tokenize_operands(line.substr(mnemonic_end),
        static_cast<std::uint32_t>(mnemonic_end), row.tokens,
        has_name_sensitive_token);
}

std::shared_ptr<const std::vector<bookmark_t>> cached_bookmark_snapshot(
    const workspace_context_t& context) {
    if (!context.view)
        return {};
    const void* publication_key = static_cast<const void*>(context.publication.get());
    const std::uint64_t overlay_revision = context.publication
        ? context.publication->overlay_revision : 0;
    auto& cache = context.view->bookmark_cache;
    if (cache.rows && cache.publication == publication_key &&
        cache.overlay_revision == overlay_revision)
        return cache.rows;
    auto rows = std::make_shared<std::vector<bookmark_t>>(bookmark_snapshot(context));
    cache.rows = rows;
    cache.publication = publication_key;
    cache.overlay_revision = overlay_revision;
    return rows;
}

void request_format_page(const workspace_context_t& context,
                         std::size_t begin, std::size_t end) {
    if (!context || !context.publication->snapshot || !context.image || begin >= end ||
        end > context.publication->snapshot->instructions.size())
        return;
    const auto page_key = aida::analysis_bridge::format_page_key(
        context.publication->generation, context.publication->analysis_revision,
        context.publication->overlay_revision,
        static_cast<std::uint64_t>(begin), static_cast<std::uint64_t>(end));
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        if (!context.view->pending_format_pages.insert(page_key).second)
            return;
    }
    const auto format_body = [context, begin, end, page_key](const std::atomic<bool>& cancelled) {
        if (cancelled.load(std::memory_order_acquire) ||
            context.workspace->cancellation_token().stop_requested()) {
            publish_format_failure(context, page_key, "Formatting cancelled.");
            return;
        }
        auto decoder_result = worker_owned_x86_decoder_t::create(
            context.image->architecture_mode());
        if (!decoder_result) {
            publish_format_failure(context, page_key, decoder_result.error().message);
            return;
        }
        auto decoder = decoder_result.take_value();
        const auto& instructions = context.publication->snapshot->instructions;
        std::vector<std::uint64_t> offsets(end - begin, 0);
        std::uint64_t minimum = (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t maximum = 0;
        bool contiguous_lease = true;
        for (std::size_t index = begin; index < end; ++index) {
            const auto offset = provider_offset(context, instructions[index].address);
            if (!offset) {
                contiguous_lease = false;
                continue;
            }
            offsets[index - begin] = *offset;
            minimum = (std::min)(minimum, *offset);
            const auto finish = checked_add(*offset, instructions[index].length);
            if (!finish) {
                contiguous_lease = false;
                continue;
            }
            maximum = (std::max)(maximum, *finish);
        }
        constexpr std::uint64_t maximum_page_lease = 8ULL << 20;
        if (minimum == (std::numeric_limits<std::uint64_t>::max)() || maximum < minimum ||
            maximum - minimum > maximum_page_lease)
            contiguous_lease = false;
        byte_view_t page_view;
        if (contiguous_lease) {
            auto lease = context.workspace->provider().lease(minimum, maximum - minimum,
                context.workspace->cancellation_token());
            if (lease)
                page_view = lease.take_value();
            else
                contiguous_lease = false;
        }
        std::unordered_map<entity_id_t, formatted_instruction_t> completed;
        completed.reserve(end - begin);
        instruction_format_options_t options;
        options.maximum_text_bytes = 2048;
        for (std::size_t index = begin; index < end; ++index) {
            if (cancelled.load(std::memory_order_acquire) ||
                context.workspace->cancellation_token().stop_requested())
                break;
            const auto& instruction = instructions[index];
            formatted_instruction_t row;
            row.instruction_id = instruction.id;
            row.generation = context.publication->generation;
            row.analysis_revision = context.publication->analysis_revision;
            row.overlay_revision = context.publication->overlay_revision;
            row.runtime_address = runtime_address(context, instruction.address).value_or(
                instruction.address.value);
            workspace_result_t<std::string> formatted = contiguous_lease
                ? decoder->format_one(page_view, minimum, context.workspace->provider(),
                    *context.image, instruction, options,
                    context.workspace->cancellation_token())
                : decoder->format_one(context.workspace->provider(), *context.image,
                    instruction, options, context.workspace->cancellation_token());
            if (formatted) {
                row.text = formatted.take_value();
                tokenize_formatted_row(row);
            } else {
                row.error = formatted.error().stable_code() + ": " + formatted.error().message;
            }
            const auto offset = offsets[index - begin];
            if (contiguous_lease) {
                row.bytes = byte_text(page_view, minimum, offset, instruction.length);
            } else {
                auto lease = context.workspace->provider().lease(offset, instruction.length,
                    context.workspace->cancellation_token());
                if (lease)
                    row.bytes = byte_text(lease.value(), offset, offset, instruction.length);
            }
            completed.emplace(instruction.id, std::move(row));
        }
        if (!context.workspace || context.workspace->closed() ||
            context.workspace->generation() != context.publication->generation) {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            context.view->pending_format_pages.erase(page_key);
            return;
        }
        aida::qt::analysis_bridge::format_page_delivery_t delivery;
        delivery.page_key = page_key;
        delivery.generation = context.publication->generation;
        delivery.analysis_revision = context.publication->analysis_revision;
        delivery.overlay_revision = context.publication->overlay_revision;
        {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            context.view->pending_format_pages.erase(page_key);
            for (auto& item : completed)
                context.view->formatted.insert_or_assign(item.first, item.second);
            context.view->format_error.clear();
            context.view->formatted_revision.fetch_add(1, std::memory_order_acq_rel);
        }
        note_ui_state_changed(context.view);
        delivery.rows = std::move(completed);
        deliver_format_page(context, std::move(delivery));
    };
    const std::string target_id = context.workspace->identity().binary_id().to_hex();
    aida::infra::taskflow_runtime::task_descriptor_t descriptor;
    descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
    descriptor.owner_subsystem = "analysis_ui";
    descriptor.label = "format_visible_disassembly";
    descriptor.target_id = target_id.c_str();
    descriptor.generation = context.publication->generation;
    descriptor.cancellable_body = [format_body](
        const aida::infra::taskflow_runtime::cancellation_token_t& runtime_cancel) {
        format_body(runtime_cancel.requested);
    };
    const auto submitted = aida::infra::taskflow_runtime::submit(std::move(descriptor));
    if (!submitted.submitted)
        publish_format_failure(context, page_key,
            submitted.reject_reason.empty() ? "Formatting queue rejected the page." :
                                               submitted.reject_reason);
}

bool reconcile_committed_overlay_state(const workspace_context_t& context,
                                       std::string& error) {
    try {
        if (!context.workspace) {
            error = "The workspace presentation state is unavailable.";
            return false;
        }
        const auto existing = context.workspace->overlay_presentation();
        if (existing && existing->overlay_revision ==
                context.workspace->overlay_revision()) {
            error.clear();
            return true;
        }
        const auto overlay = context.workspace->overlay();
        if (!overlay) {
            error = "The committed workspace overlay is unavailable.";
            return false;
        }
        const auto snapshot = overlay->snapshot();
        if (snapshot.revision != context.workspace->overlay_revision()) {
            error = "The authoritative overlay revision changed before derived publication.";
            return false;
        }
        auto next = std::make_shared<workspace_overlay_presentation_t>();
        next->overlay_revision = snapshot.revision;
        next->comments.reserve(snapshot.items.size());
        next->renames.reserve(snapshot.items.size());
        next->bookmarks.reserve(snapshot.items.size());
        next->workspace_bookmarks.reserve(snapshot.items.size());
        for (const auto& item : snapshot.items) {
            const auto& operation = item.second;
            if (operation.target_discriminator !=
                overlay_target_discriminator_v9_t::native_address)
                continue;
            switch (operation.kind) {
            case overlay_operation_kind_t::comment:
            case overlay_operation_kind_t::comment_update:
                next->comments.push_back({operation.address, operation.text});
                break;
            case overlay_operation_kind_t::name:
                next->renames.push_back({operation.address, operation.name});
                break;
            case overlay_operation_kind_t::bookmark:
                next->bookmarks.push_back({operation.address, operation.name});
                next->workspace_bookmarks.push_back(operation.address);
                break;
            default:
                break;
            }
        }
        const auto address_less = [](const auto& left, const auto& right) {
            return left.address < right.address;
        };
        std::sort(next->comments.begin(), next->comments.end(), address_less);
        std::sort(next->renames.begin(), next->renames.end(), address_less);
        std::sort(next->bookmarks.begin(), next->bookmarks.end(), address_less);
        std::sort(next->workspace_bookmarks.begin(), next->workspace_bookmarks.end());
        auto published = context.workspace->publish_overlay_presentation(
            snapshot.revision,
            std::static_pointer_cast<const workspace_overlay_presentation_t>(
                std::move(next)));
        if (!published) {
            error = published.error().stable_code() + ": " +
                published.error().message;
            return false;
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Derived overlay publication preparation failed: ") +
            exception.what();
        return false;
    } catch (...) {
        error = "Derived overlay publication preparation failed.";
        return false;
    }
}

class pending_mutation_guard_t final {
public:
    explicit pending_mutation_guard_t(std::shared_ptr<state_t> state) noexcept
        : state_(std::move(state)) {}

    ~pending_mutation_guard_t() {
        if (state_)
            state_->pending_mutations.fetch_sub(1, std::memory_order_acq_rel);
    }

    void release() noexcept {
        state_.reset();
    }

private:
    std::shared_ptr<state_t> state_;
};

class derived_publication_retry_guard_t final {
public:
    explicit derived_publication_retry_guard_t(
        std::shared_ptr<state_t> state) noexcept
        : state_(std::move(state)) {}

    ~derived_publication_retry_guard_t() {
        if (state_)
            state_->derived_publication_retry_pending.store(
                false, std::memory_order_release);
    }

private:
    std::shared_ptr<state_t> state_;
};

void record_overlay_presentation_result(const workspace_context_t& source_context,
                                        bool published,
                                        std::string detail) {
    const auto context = authoritative_context(source_context);
    if (!context.view || !context.workspace)
        return;
    const auto publication = context.workspace->analysis_publication();
    const auto presentation = publication
        ? publication->overlay_presentation : nullptr;
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        context.view->derived_publication_target_revision =
            publication ? publication->overlay_revision : 0;
        context.view->derived_publication_revision = presentation
            ? presentation->overlay_revision : 0;
        context.view->derived_publication_error = published
            ? std::string() : (detail.empty()
                ? "The committed overlay is awaiting derived presentation publication."
                : std::move(detail));
    }
    note_ui_state_changed(context.view);
}

}

bool queue_overlay_presentation_retry(const workspace_context_t& source_context) {
    const auto context = authoritative_context(source_context);
    if (!context || !context.workspace->overlay())
        return false;
    bool expected = false;
    if (!context.view->derived_publication_retry_pending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return false;
    auto body = [context]() {
        derived_publication_retry_guard_t pending(context.view);
        std::string detail;
        bool published = false;
        try {
            std::lock_guard<std::mutex> mutation_lock(context.model->mutation_mutex);
            published = reconcile_committed_overlay_state(context, detail);
        } catch (const std::exception& exception) {
            detail = std::string("Derived overlay publication retry failed: ") +
                exception.what();
        } catch (...) {
            detail = "Derived overlay publication retry failed.";
        }
        try {
            record_overlay_presentation_result(
                context, published, std::move(detail));
        } catch (...) {
        }
    };
    try {
        aida::infra::taskflow_runtime::task_descriptor_t descriptor;
        descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
        descriptor.owner_subsystem = "analysis_ui";
        descriptor.label = "workspace_overlay_presentation_retry";
        const std::string target_id = context.workspace->identity().binary_id().to_hex();
        descriptor.target_id = target_id.c_str();
        descriptor.generation = context.workspace->generation();
        descriptor.cancellable_body = [body = std::move(body)](
            const aida::infra::taskflow_runtime::cancellation_token_t&) mutable {
            body();
        };
        const auto submitted = aida::infra::taskflow_runtime::submit(
            std::move(descriptor));
        if (submitted.submitted)
            return true;
        context.view->derived_publication_retry_pending.store(
            false, std::memory_order_release);
        record_overlay_presentation_result(context, false,
            submitted.reject_reason.empty()
                ? "The derived overlay publication retry queue rejected the request."
                : submitted.reject_reason);
        return false;
    } catch (const std::exception& exception) {
        context.view->derived_publication_retry_pending.store(
            false, std::memory_order_release);
        try {
            record_overlay_presentation_result(context, false,
                std::string("The derived overlay publication retry could not be queued: ") +
                    exception.what());
        } catch (...) {
        }
        return false;
    } catch (...) {
        context.view->derived_publication_retry_pending.store(
            false, std::memory_order_release);
        try {
            record_overlay_presentation_result(context, false,
                "The derived overlay publication retry could not be queued.");
        } catch (...) {
        }
        return false;
    }
}

bool queue_overlay_transaction(const workspace_context_t& source_context,
                               std::vector<overlay_operation_t> operations,
                               std::optional<std::uint64_t> required_generation,
                               std::optional<std::uint64_t> required_analysis_revision,
                               std::optional<std::uint64_t> required_overlay_revision,
                               overlay_completion_t completion){
    const auto context = authoritative_context(source_context);
    if (!context || context.workspace->closing() || context.workspace->closed() ||
        !context.workspace->overlay() || operations.empty())
        return false;
    const auto expected_generation = required_generation.value_or(context.workspace->generation());
    const auto expected_analysis_revision = required_analysis_revision.value_or(
        context.workspace->analysis_revision());
    const auto expected_overlay_revision = required_overlay_revision.value_or(
        context.workspace->overlay_revision());
    if (context.workspace->generation() != expected_generation ||
        context.workspace->analysis_revision() != expected_analysis_revision ||
        context.workspace->overlay_revision() != expected_overlay_revision)
        return false;
    const std::size_t operation_count = operations.size();
    context.view->pending_mutations.fetch_add(1, std::memory_order_acq_rel);
    pending_mutation_guard_t setup_pending(context.view);
    auto mutation_body = [context, operations = std::move(operations),
                          expected_generation, expected_analysis_revision,
                          expected_overlay_revision, completion = std::move(completion)](
        bool cancelled) mutable {
        pending_mutation_guard_t pending(context.view);
        std::string error;
        bool authoritative_succeeded = false;
        try {
            if (cancelled) {
                error = "Mutation cancelled before execution.";
            } else if (context.workspace->generation() != expected_generation ||
                       context.workspace->analysis_revision() != expected_analysis_revision) {
                error = "The analysis publication changed before the mutation ran; select the item and retry.";
            } else {
                std::lock_guard<std::mutex> mutation_lock(
                    context.model->mutation_mutex);
                auto overlay = context.workspace->overlay();
                if (!overlay) {
                    error = "Workspace overlay is unavailable.";
                } else {
                    overlay_transaction_request_t request;
                    request.expected_revision = expected_overlay_revision;
                    request.operations = operations;
                    auto result = overlay->transact(
                        request, context.workspace->cancellation_token());
                    if (result) {
                        authoritative_succeeded = true;
                        try {
                            std::string publication_detail;
                            const bool published =
                                reconcile_committed_overlay_state(
                                    context, publication_detail);
                            record_overlay_presentation_result(
                                context, published,
                                std::move(publication_detail));
                        } catch (const std::exception& exception) {
                            try {
                                record_overlay_presentation_result(context, false,
                                    std::string("Derived overlay publication failed: ") +
                                        exception.what());
                            } catch (...) {
                            }
                        } catch (...) {
                            try {
                                record_overlay_presentation_result(context, false,
                                    "Derived overlay publication failed.");
                            } catch (...) {
                            }
                        }
                    } else {
                        error = result.error().stable_code() + ": " +
                            result.error().message;
                    }
                }
            }
        } catch (const std::exception& exception) {
            if (!authoritative_succeeded)
                error = std::string("Overlay mutation execution failed: ") +
                    exception.what();
        } catch (...) {
            if (!authoritative_succeeded)
                error = "Overlay mutation execution failed.";
        }
        const bool succeeded = authoritative_succeeded && error.empty();
        std::string completion_error = error;
        gui_post_or_run([context, succeeded, error = std::move(error),
                         completion_error = std::move(completion_error),
                         completion = std::move(completion)]() mutable {
            try {
                {
                    std::lock_guard<std::mutex> lock(context.view->mutex);
                    context.view->mutation_error = std::move(error);
                    context.view->formatted.clear();
                    context.view->formatted_revision.fetch_add(1, std::memory_order_acq_rel);
                    context.view->cached_overlay_revision = context.workspace->overlay_revision();
                }
                note_ui_state_changed(context.view);
            } catch (...) {
            }
            deliver_format_reset(context);
            if (completion) {
                try {
                    completion(succeeded, std::move(completion_error));
                } catch (...) {
                }
            }
        });
    };
    bool accepted = false;
    try {
        const std::string target_id =
            context.workspace->identity().binary_id().to_hex();
        aida::infra::taskflow_runtime::task_descriptor_t descriptor;
        descriptor.domain =
            aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
        descriptor.owner_subsystem = "analysis_ui";
        descriptor.label = "workspace_overlay_mutation";
        descriptor.target_id = target_id.c_str();
        descriptor.generation = context.publication->generation;
        descriptor.cancellable_body = [mutation_body = std::move(mutation_body)](
            const aida::infra::taskflow_runtime::cancellation_token_t&
                runtime_cancel) mutable {
            mutation_body(runtime_cancel.requested.load(
                std::memory_order_acquire));
        };
        const auto submitted = aida::infra::taskflow_runtime::submit(
            std::move(descriptor));
        if (!submitted.submitted) {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            context.view->mutation_error = submitted.reject_reason.empty()
                ? "Mutation queue rejected the request."
                : submitted.reject_reason;
            note_ui_state_changed(context.view);
            return false;
        }
        setup_pending.release();
        accepted = true;
        aida::ui::task_center::task_registration_t registration;
        registration.owner = "analysis";
        registration.owner_view = "document.disassembly";
        registration.owner_action = "analysis.overlay.mutate";
        registration.target = target_id;
        registration.label = "Apply reviewed analysis overlay mutation";
        registration.stage = "Queued";
        registration.affected_entity = std::to_string(operation_count) +
            " overlay operation(s)";
        registration.progress = -1.f;
        registration.cancellation_is_safe = true;
        registration.callbacks.focus = [] {
            const auto hook = aida::qt::analysis_bridge::view_focus_hook();
            if (hook)
                hook("document.disassembly");
        };
        static_cast<void>(aida::ui::task_center::register_taskflow_job(
            submitted.handle, std::move(registration)));
        return true;
    } catch (const std::exception& exception) {
        if (accepted)
            return true;
        try {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            context.view->mutation_error =
                std::string("Mutation queue setup failed: ") + exception.what();
            note_ui_state_changed(context.view);
        } catch (...) {
        }
        return false;
    } catch (...) {
        if (accepted)
            return true;
        try {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            context.view->mutation_error = "Mutation queue setup failed.";
            note_ui_state_changed(context.view);
        } catch (...) {
        }
        return false;
    }
}

namespace {

constexpr std::size_t k_static_patch_maximum_bytes = 64U * 1024U;

std::string encode_patch_bytes(const std::vector<std::uint8_t>& bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string encoded;
    if (!bytes.empty())
        encoded.reserve(bytes.size() * 3U - 1U);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0)
            encoded.push_back(' ');
        encoded.push_back(digits[(bytes[index] >> 4U) & 0x0fU]);
        encoded.push_back(digits[bytes[index] & 0x0fU]);
    }
    return encoded;
}

}

std::optional<std::vector<std::uint8_t>> decode_patch_bytes(
    std::string_view encoded, std::string& error) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve((std::min)(k_static_patch_maximum_bytes,
        (encoded.size() + 1U) / 2U));
    int high = -1;
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const unsigned char value = static_cast<unsigned char>(encoded[index]);
        if (std::isspace(value) || value == ',' || value == ':' || value == '_' || value == '-') {
            if (high != -1) {
                error = "Each byte must contain exactly two hexadecimal digits.";
                return {};
            }
            continue;
        }
        if (value == '0' && index + 1U < encoded.size() &&
            (encoded[index + 1U] == 'x' || encoded[index + 1U] == 'X') && high == -1) {
            ++index;
            continue;
        }
        int nibble = -1;
        if (value >= '0' && value <= '9') nibble = value - '0';
        else if (value >= 'a' && value <= 'f') nibble = value - 'a' + 10;
        else if (value >= 'A' && value <= 'F') nibble = value - 'A' + 10;
        if (nibble < 0) {
            error = "Patch bytes may contain only hexadecimal digits and separators.";
            return {};
        }
        if (high < 0) {
            high = nibble;
        } else {
            if (bytes.size() >= k_static_patch_maximum_bytes) {
                error = "Interactive patch review is limited to 64 KiB per operation.";
                return {};
            }
            bytes.push_back(static_cast<std::uint8_t>((high << 4) | nibble));
            high = -1;
        }
    }
    if (high != -1) {
        error = "The final byte is missing its second hexadecimal digit.";
        return {};
    }
    if (bytes.empty()) {
        error = "Enter at least one replacement byte.";
        return {};
    }
    error.clear();
    return bytes;
}

namespace {

bool queue_overlay_operation(const workspace_context_t& context,
                             overlay_operation_t operation,
                             std::optional<std::uint64_t> required_generation = {},
                             std::optional<std::uint64_t> required_analysis_revision = {},
                             std::optional<std::uint64_t> required_overlay_revision = {},
                             overlay_completion_t completion = {}) {
    std::vector<overlay_operation_t> operations;
    operations.push_back(std::move(operation));
    return queue_overlay_transaction(context, std::move(operations),
        required_generation, required_analysis_revision, required_overlay_revision,
        std::move(completion));
}

}

bool queue_overlay_history(const workspace_context_t& source_context, bool redo,
                           std::uint64_t expected_generation,
                           std::uint64_t expected_analysis_revision,
                           std::uint64_t expected_overlay_revision) {
    const auto context = authoritative_context(source_context);
    if (!context || !context.workspace->overlay() ||
        context.workspace->generation() != expected_generation ||
        context.workspace->analysis_revision() != expected_analysis_revision ||
        context.workspace->overlay_revision() != expected_overlay_revision)
        return false;
    context.view->pending_mutations.fetch_add(1, std::memory_order_acq_rel);
    pending_mutation_guard_t setup_pending(context.view);
    auto body = [context, redo, expected_generation, expected_analysis_revision,
                 expected_overlay_revision](bool cancelled) {
        pending_mutation_guard_t pending(context.view);
        std::string error;
        bool authoritative_succeeded = false;
        try {
            if (cancelled) {
                error = "Overlay history request was cancelled before execution.";
            } else if (context.workspace->generation() != expected_generation ||
                       context.workspace->analysis_revision() != expected_analysis_revision) {
                error = "The analysis publication changed before the overlay history request ran.";
            } else {
                std::lock_guard<std::mutex> mutation_lock(
                    context.model->mutation_mutex);
                auto overlay = context.workspace->overlay();
                if (!overlay) {
                    error = "Workspace overlay history is unavailable.";
                } else {
                    const auto result = redo
                        ? overlay->redo(expected_overlay_revision,
                            context.workspace->cancellation_token())
                        : overlay->undo(expected_overlay_revision,
                            context.workspace->cancellation_token());
                    if (!result) {
                        error = result.error().stable_code() + ": " +
                            result.error().message;
                    } else {
                        authoritative_succeeded = true;
                        try {
                            std::string publication_detail;
                            const bool published =
                                reconcile_committed_overlay_state(
                                    context, publication_detail);
                            record_overlay_presentation_result(
                                context, published,
                                std::move(publication_detail));
                        } catch (const std::exception& exception) {
                            try {
                                record_overlay_presentation_result(context, false,
                                    std::string("Derived overlay publication failed: ") +
                                        exception.what());
                            } catch (...) {
                            }
                        } catch (...) {
                            try {
                                record_overlay_presentation_result(context, false,
                                    "Derived overlay publication failed.");
                            } catch (...) {
                            }
                        }
                    }
                }
            }
        } catch (const std::exception& exception) {
            if (!authoritative_succeeded)
                error = std::string("Overlay history execution failed: ") +
                    exception.what();
        } catch (...) {
            if (!authoritative_succeeded)
                error = "Overlay history execution failed.";
        }
        gui_post_or_run([context, error = std::move(error)]() mutable {
            try {
                {
                    std::lock_guard<std::mutex> lock(context.view->mutex);
                    context.view->mutation_error = std::move(error);
                    context.view->formatted.clear();
                    context.view->formatted_revision.fetch_add(1, std::memory_order_acq_rel);
                    context.view->cached_overlay_revision = context.workspace->overlay_revision();
                }
                note_ui_state_changed(context.view);
            } catch (...) {
            }
            deliver_format_reset(context);
        });
    };
    try {
        aida::infra::taskflow_runtime::task_descriptor_t descriptor;
        descriptor.domain =
            aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
        descriptor.owner_subsystem = "analysis_ui";
        descriptor.label = redo
            ? "workspace_overlay_redo" : "workspace_overlay_undo";
        const std::string target_id =
            context.workspace->identity().binary_id().to_hex();
        descriptor.target_id = target_id.c_str();
        descriptor.generation = expected_generation;
        descriptor.cancellable_body = [body = std::move(body)](
            const aida::infra::taskflow_runtime::cancellation_token_t& cancel) mutable {
            body(cancel.requested.load(std::memory_order_acquire));
        };
        const auto submitted = aida::infra::taskflow_runtime::submit(
            std::move(descriptor));
        if (!submitted.submitted) {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            context.view->mutation_error = submitted.reject_reason.empty()
                ? "Overlay history queue rejected the request."
                : submitted.reject_reason;
            note_ui_state_changed(context.view);
            return false;
        }
        setup_pending.release();
        return true;
    } catch (const std::exception& exception) {
        try {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            context.view->mutation_error =
                std::string("Overlay history queue setup failed: ") +
                    exception.what();
            note_ui_state_changed(context.view);
        } catch (...) {
        }
        return false;
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            context.view->mutation_error =
                "Overlay history queue setup failed.";
            note_ui_state_changed(context.view);
        } catch (...) {
        }
        return false;
    }
}

namespace {

std::weak_ptr<analysis_workspace_t>& static_patch_owner() {
    static std::weak_ptr<analysis_workspace_t> value;
    return value;
}

bool initialize_static_patch_review(const workspace_context_t& context,
                                    const aida::analysis::address_t& address,
                                    std::uint64_t extent,
                                    static_patch_mode_t mode,
                                    std::vector<std::uint8_t> original,
                                    std::vector<std::uint8_t> proposed,
                                    bool prefer_existing_patch,
                                    bool focus_input,
                                    std::string description,
                                    std::string status,
                                    std::optional<std::uint64_t> required_generation,
                                    std::optional<std::uint64_t> required_analysis_revision,
                                    std::optional<std::uint64_t> required_overlay_revision,
                                    std::string* error) {
    const auto fail = [error](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    const std::uint64_t generation = required_generation.value_or(
        context.workspace->generation());
    const std::uint64_t analysis_revision = required_analysis_revision.value_or(
        context.workspace->analysis_revision());
    const std::uint64_t overlay_revision = required_overlay_revision.value_or(
        context.workspace->overlay_revision());
    const auto fence_current = [&] {
        return context.workspace->generation() == generation &&
            context.workspace->analysis_revision() == analysis_revision &&
            context.workspace->overlay_revision() == overlay_revision;
    };
    if (!fence_current())
        return fail("The patch review publication fence changed before the modal could be initialized.");
    bool existing_patch = false;
    std::uint64_t existing_patch_size = 0;
    if (const auto overlay = context.workspace->overlay()) {
        const auto patches = overlay->patch_operations();
        const auto existing = std::find_if(patches.begin(), patches.end(),
            [&address](const overlay_operation_t& operation) {
                return operation.address == address && !operation.remove;
            });
        if (existing != patches.end()) {
            existing_patch = true;
            existing_patch_size = existing->bytes.size();
            if (prefer_existing_patch && existing->bytes.size() <= k_static_patch_maximum_bytes)
                proposed = existing->bytes;
        }
    }
    if (mode == static_patch_mode_t::nop_fill)
        proposed.assign(static_cast<std::size_t>(extent), 0x90U);
    const std::string encoded = encode_patch_bytes(proposed);
    if (encoded.size() > 196608U)
        return fail("The selected patch cannot be represented within the bounded review editor.");
    if (!fence_current())
        return fail("The patch review publication fence changed before the modal could be initialized.");
    static_patch_init_t init;
    init.mode = mode;
    init.address = address;
    init.extent = extent;
    init.generation = generation;
    init.analysis_revision = analysis_revision;
    init.overlay_revision = overlay_revision;
    init.existing = existing_patch;
    init.existing_size = existing_patch_size;
    init.original = std::move(original);
    init.proposed = std::move(proposed);
    init.encoded = encoded;
    init.description = description.size() > 255U ? description.substr(0, 255U) : description;
    init.status = existing_patch_size > k_static_patch_maximum_bytes
        ? "The existing overlay exceeds the 64 KiB interactive editor bound. The editor shows the selected immutable baseline; Revert This Overlay remains available."
        : std::move(status);
    init.focus_input = focus_input;
    static_patch_owner() = context.workspace;
    const auto hook = aida::qt::analysis_bridge::static_patch_review_hook();
    if (!hook)
        return fail("The static patch review presenter is unavailable.");
    workspace_context_t retained = context;
    gui_post_or_run([retained = std::move(retained), init = std::move(init), hook]() mutable {
        hook(retained, std::move(init));
    });
    if (error) error->clear();
    return true;
}

}

bool open_static_patch_review(const workspace_context_t& context,
                              const aida::analysis::address_t& address,
                              std::uint64_t extent,
                              static_patch_mode_t mode,
                              std::string* error) {
    const auto fail = [error](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (!context || context.workspace->closing() || context.workspace->closed() ||
        !context.workspace->overlay())
        return fail("The selected workspace has no writable overlay journal.");
    if (mode == static_patch_mode_t::assembly)
        return fail("No reusable standalone assembler provider is registered. Zydis encoding is a build dependency, but the UI has no validated assembly parser/provider; use reviewed Patch Bytes or NOP Fill.");
    if (extent == 0 || extent > k_static_patch_maximum_bytes)
        return fail("Interactive static patch review requires a mapped selection from 1 byte through 64 KiB.");
    if (extent > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
        return fail("The selected patch extent exceeds this host's addressable size.");
    const auto offset = provider_offset(context, address);
    if (!offset || *offset > context.workspace->provider().size() ||
        extent > context.workspace->provider().size() - *offset)
        return fail("The selected range is not fully backed by the immutable workspace provider.");
    auto original = read_bytes(context, address, static_cast<std::size_t>(extent));
    if (!original)
        return fail(original.error().stable_code() + ": " + original.error().message);
    auto proposed = original.value();
    return initialize_static_patch_review(context, address, extent, mode,
        original.take_value(), std::move(proposed), true,
        mode == static_patch_mode_t::bytes,
        mode == static_patch_mode_t::nop_fill
            ? "Static NOP overlay" : "Static byte overlay",
        {}, {}, {}, {}, error);
}

bool open_selected_patch_review(static_patch_mode_t mode, std::string* error) {
    const auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    const auto context = capture_selected_workspace();
    if (!context || !context.publication || !context.publication->snapshot)
        return fail("No analyzed workspace is selected.");
    std::optional<address_t> selected;
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        selected = context.view->selection;
    }
    if (!selected)
        return fail("Select an instruction before opening the patch workflow.");
    const auto found = std::find_if(context.publication->snapshot->instructions.begin(),
        context.publication->snapshot->instructions.end(), [&selected](const instruction_record_t& instruction) {
            return instruction.address == *selected;
        });
    if (found == context.publication->snapshot->instructions.end() || found->length == 0)
        return fail("The selected analysis entity has no current instruction byte range.");
    const std::uint64_t runtime = runtime_address(context, *selected).value_or(selected->value);
    const auto process = context.workspace->identity().process();
    if (process && driver_bridge::attached_pid() == process->pid) {
        if (mode == static_patch_mode_t::assembly)
            return fail("No reusable standalone assembler provider is registered; use reviewed Patch Bytes or NOP Fill.");
        const auto debugger_context = debugger_interaction::capture(
            debugger_interaction::kind_t::instruction, runtime, 0, -1, 0,
            static_cast<std::uint64_t>(found->length));
        if (process->creation_time_100ns == 0 ||
            debugger_context.target_pid != process->pid ||
            debugger_context.process_creation_time_100ns !=
                process->creation_time_100ns ||
            !debugger_interaction::is_current(debugger_context))
            return fail("The disassembly workspace process identity or debugger stop changed before patch review.");
        const bool opened = mode == static_patch_mode_t::nop_fill
            ? debugger_view::stage_nop_review(debugger_context, found->length, error)
            : debugger_view::stage_patch_review(debugger_context, found->length,
                "Reviewed patch from Disassembly shortcut", error);
        if (opened) {
            const auto focus = aida::qt::analysis_bridge::view_focus_hook();
            if (focus)
                focus("view.debug.patches");
        }
        return opened;
    }
    return open_static_patch_review(context, *selected, found->length, mode, error);
}

bool open_exact_static_patch_review(const workspace_context_t& context,
                                    const address_t& address,
                                    const std::vector<std::uint8_t>& expected_before,
                                    const std::vector<std::uint8_t>& reviewed_after,
                                    const std::string& provenance,
                                    std::uint64_t expected_generation,
                                    std::uint64_t expected_analysis_revision,
                                    std::uint64_t expected_overlay_revision,
                                    std::string* error) {
    if (expected_before.empty() || reviewed_after.empty() ||
        expected_before.size() != reviewed_after.size() ||
        reviewed_after.size() > k_static_patch_maximum_bytes) {
        if (error) *error = "Exact static patch review requires matching before/after ranges from 1 byte through 64 KiB.";
        return false;
    }
    if (provenance.empty() || provenance.size() > 512U) {
        if (error) *error = "Exact static patch review requires bounded proposal provenance.";
        return false;
    }
    if (!context || context.workspace->closing() || context.workspace->closed() ||
        context.workspace->generation() != expected_generation ||
        context.workspace->analysis_revision() != expected_analysis_revision ||
        context.workspace->overlay_revision() != expected_overlay_revision) {
        if (error) *error = "The exact static patch publication fence changed before review opened.";
        return false;
    }
    const auto offset = provider_offset(context, address);
    if (!offset || *offset > context.workspace->provider().size() ||
        reviewed_after.size() > context.workspace->provider().size() - *offset) {
        if (error) *error = "The exact static patch range is not fully backed by the immutable workspace provider.";
        return false;
    }
    return initialize_static_patch_review(context, address, reviewed_after.size(),
        static_patch_mode_t::bytes, expected_before, reviewed_after, false, false,
        "AI reviewed proposal: " + provenance,
        "AI before/after bytes were identity-checked; confirm Apply Patch here to commit the reversible overlay transaction.",
        expected_generation, expected_analysis_revision, expected_overlay_revision,
        error);
}

workspace_context_t capture_workspace(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
    std::string_view presentation_key) {
    workspace_context_t context;
    if (!workspace || workspace->closed())
        return context;
    context.workspace = workspace;
    context.publication = workspace->analysis_publication();
    context.image = context.publication && context.publication->snapshot
        ? context.publication->snapshot->image : workspace->image();
    context.model = model_for(workspace);
    context.view = presentation_for(context.model, presentation_key);
    context.progress = workspace->progress();
    initialize_model(workspace, context.model);
    if (context.model && context.view && context.publication &&
        context.publication->overlay_revision != 0) {
        const auto authority = authoritative_context(context);
        const auto presentation = context.publication->overlay_presentation;
        bool attempted = false;
        {
            std::lock_guard<std::mutex> lock(authority.view->mutex);
            attempted = authority.view->derived_publication_target_revision ==
                context.publication->overlay_revision;
        }
        if ((!presentation || presentation->overlay_revision !=
                context.publication->overlay_revision) && !attempted &&
            authority.view->pending_mutations.load(std::memory_order_acquire) == 0) {
            try {
                std::unique_lock<std::mutex> mutation_lock(
                    context.model->mutation_mutex, std::try_to_lock);
                if (mutation_lock.owns_lock()) {
                    std::string detail;
                    const bool published = reconcile_committed_overlay_state(
                        authority, detail);
                    record_overlay_presentation_result(
                        authority, published, std::move(detail));
                    if (published) {
                        const auto refreshed = workspace->analysis_publication();
                        if (refreshed &&
                            refreshed->generation ==
                                context.publication->generation &&
                            refreshed->analysis_revision ==
                                context.publication->analysis_revision &&
                            refreshed->overlay_revision ==
                                context.publication->overlay_revision) {
                            context.publication = refreshed;
                            context.image = refreshed->snapshot
                                ? refreshed->snapshot->image : workspace->image();
                        }
                    }
                }
            } catch (...) {
                try {
                    record_overlay_presentation_result(authority, false,
                        "Derived overlay publication recovery failed.");
                } catch (...) {
                }
            }
        }
    }
    if (context.view && context.publication) {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        if (context.view->cached_generation != context.publication->generation ||
            context.view->cached_analysis_revision != context.publication->analysis_revision ||
            context.view->cached_overlay_revision != context.publication->overlay_revision) {
            context.view->formatted.clear();
            context.view->formatted_revision.fetch_add(1, std::memory_order_acq_rel);
            context.view->pending_format_pages.clear();
            context.view->cached_generation = context.publication->generation;
            context.view->cached_analysis_revision = context.publication->analysis_revision;
            context.view->cached_overlay_revision = context.publication->overlay_revision;
            context.model->format_generation.fetch_add(1, std::memory_order_acq_rel);
        }
        const auto workspace_view = workspace->view_state();
        if (!context.view->selection_initialized ||
            (normalize_presentation_key(presentation_key).empty() && workspace_view.revision != 0 &&
             workspace_view.revision != context.model->presentation_selection_revision.load(
                std::memory_order_acquire))) {
            context.view->selection = workspace_view.selection;
            context.view->selection_initialized = true;
            if (normalize_presentation_key(presentation_key).empty())
                context.model->presentation_selection_revision.store(
                    workspace_view.revision, std::memory_order_release);
        }
    }
    return context;
}

workspace_context_t capture_workspace(
    const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace) {
    return capture_workspace(workspace, {});
}

workspace_context_t capture_selected_workspace() {
    return capture_workspace(
        aida::analysis::workspace_registry().selected_for_ui(),
        aida::qt::analysis_bridge::focused_presentation_key());
}

workspace_context_t capture_selected_workspace(std::string_view presentation_key) {
    return capture_workspace(
        aida::analysis::workspace_registry().selected_for_ui(), presentation_key);
}

void reset_presentation(std::string_view presentation_key) {
    const std::string key = normalize_presentation_key(presentation_key);
    if (key.empty())
        return;
    std::lock_guard<std::mutex> registry_lock(model_registry_mutex());
    for (auto& [_, model] : model_registry()) {
        if (model) {
            std::lock_guard<std::mutex> model_lock(model->initialization_mutex);
            auto& slot = model->presentations[key];
            if (slot)
                model->section_filter_mirror.erase(slot.get());
            slot = std::make_shared<state_t>();
        }
    }
}

void release_presentation(std::string_view presentation_key) {
    const std::string key = normalize_presentation_key(presentation_key);
    if (key.empty())
        return;
    std::lock_guard<std::mutex> registry_lock(model_registry_mutex());
    for (auto& [_, model] : model_registry()) {
        if (model) {
            std::lock_guard<std::mutex> model_lock(model->initialization_mutex);
            const auto existing = model->presentations.find(key);
            if (existing != model->presentations.end())
                model->section_filter_mirror.erase(existing->second.get());
            model->presentations.erase(key);
        }
    }
    {
        std::lock_guard<std::mutex> lock(delivery_registry_mutex());
        for (auto it = delivery_registry().begin(); it != delivery_registry().end();) {
            const auto& targets = it->second;
            if (targets.format.expired() && targets.xref.expired() &&
                targets.export_status.expired() && targets.hooks.expired())
                it = delivery_registry().erase(it);
            else
                ++it;
        }
    }
}

void clone_presentation(std::string_view source_key, std::string_view target_key) {
    const std::string source = normalize_presentation_key(source_key);
    const std::string target = normalize_presentation_key(target_key);
    if (target.empty() || source == target)
        return;
    std::lock_guard<std::mutex> registry_lock(model_registry_mutex());
    for (auto& [_, model] : model_registry()) {
        if (model) {
            const auto source_state = presentation_for(model, source);
            const auto target_state = presentation_for(model, target);
            if (!source_state || !target_state)
                continue;
            std::scoped_lock state_lock(source_state->mutex, target_state->mutex);
            target_state->addr_format = source_state->addr_format;
            target_state->show_bytes = source_state->show_bytes;
            target_state->display_image_base = source_state->display_image_base;
            target_state->active_section = source_state->active_section;
            model->section_filter_mirror[target_state.get()] = source_state->active_section;
            target_state->selection = source_state->selection;
            target_state->target_scroll_y = source_state->target_scroll_y;
            target_state->scroll_restore_pending = true;
            target_state->scroll_to_selection = false;
            target_state->selection_initialized = source_state->selection_initialized;
        }
    }
}

bool capture_selected_presentation(std::string_view presentation_key,
                                   presentation_snapshot_t& snapshot) {
    const auto context = capture_selected_workspace(presentation_key);
    if (!context.view)
        return false;
    std::lock_guard<std::mutex> lock(context.view->mutex);
    snapshot.addr_format = context.view->addr_format;
    snapshot.show_bytes = context.view->show_bytes;
    snapshot.display_image_base = context.view->display_image_base;
    snapshot.active_section = context.view->active_section;
    snapshot.selection = context.view->selection;
    snapshot.scroll_y = context.view->target_scroll_y;
    return true;
}

bool restore_selected_presentation(std::string_view presentation_key,
                                   const presentation_snapshot_t& snapshot) {
    const std::string key = normalize_presentation_key(presentation_key);
    if (key.empty())
        return false;
    const auto context = capture_selected_workspace(key);
    if (!context.view)
        return false;
    if (snapshot.active_section >= 0 && (!context.image ||
        static_cast<std::size_t>(snapshot.active_section) >=
            context.image->sections().size()))
        return false;
    if (snapshot.selection) {
        if (snapshot.selection->architecture !=
                context.workspace->identity().architecture() ||
            (context.image && snapshot.selection->mode !=
                context.image->architecture_mode()) ||
            !runtime_address(context, *snapshot.selection))
            return false;
    }
    std::lock_guard<std::mutex> lock(context.view->mutex);
    context.view->addr_format = snapshot.addr_format;
    context.view->show_bytes = snapshot.show_bytes;
    context.view->display_image_base = snapshot.display_image_base;
    context.view->active_section = snapshot.active_section;
    context.model->section_filter_mirror[context.view.get()] = snapshot.active_section;
    context.view->selection = snapshot.selection;
    context.view->selection_initialized = true;
    context.view->target_scroll_y = (std::max)(snapshot.scroll_y, 0.0f);
    context.view->scroll_restore_pending = true;
    context.view->scroll_to_selection = false;
    context.view->ui_serial.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

std::optional<aida::analysis::address_t> typed_address(
    const workspace_context_t& context, std::uint64_t value) {
    if (!context.workspace)
        return {};
    const auto& identity = context.workspace->identity();
    aida::analysis::address_t address;
    address.architecture = identity.architecture();
    address.mode = context.image ? context.image->architecture_mode() :
        (identity.architecture() == aida::analysis::architecture_id_t::x86_64
            ? aida::analysis::architecture_mode_t::x86_64
            : aida::analysis::architecture_mode_t::x86_32);
    if (identity.target_kind() == aida::analysis::target_kind_t::live_snapshot) {
        address.space = aida::analysis::address_space_id_t::live_virtual;
        address.value = value;
        return address;
    }
    if (!context.image)
        return {};
    const auto display_base = display_base_override(context);
    const std::uint64_t base = display_base.value_or(context.image->image_base());
    if (value >= base) {
        std::optional<std::uint64_t> rva;
        if (display_base) {
            const auto candidate = value - base;
            if (candidate < context.image->image_size())
                rva = candidate;
        } else {
            rva = optional_value(context.image->va_to_rva(value));
        }
        if (!rva) return {};
        address.space = aida::analysis::address_space_id_t::relative_virtual;
        address.value = *rva;
        return address;
    }
    if (value < context.image->image_size()) {
        address.space = aida::analysis::address_space_id_t::relative_virtual;
        address.value = value;
        return address;
    }
    return {};
}

std::optional<std::uint64_t> display_base_override(const workspace_context_t& context) {
    if (!context.view)
        return {};
    std::lock_guard<std::mutex> lock(context.view->mutex);
    return context.view->display_image_base;
}

std::uint64_t display_image_base(const workspace_context_t& context) {
    if (const auto value = display_base_override(context))
        return *value;
    return context.image ? context.image->image_base() : 0;
}

std::optional<std::uint64_t> runtime_address_with_base(
    const workspace_context_t& context, const aida::analysis::address_t& address,
    const std::optional<std::uint64_t>& base_override) {
    using aida::analysis::address_space_id_t;
    if (address.space == address_space_id_t::virtual_address ||
        address.space == address_space_id_t::live_virtual)
        return address.value;
    if (!context.image)
        return {};
    if (address.space == address_space_id_t::relative_virtual) {
        if (base_override)
            return checked_add(*base_override, address.value);
        return optional_value(context.image->rva_to_va(address.value));
    }
    if (address.space == address_space_id_t::file_offset) {
        auto rva = optional_value(context.image->file_offset_to_rva(address.value));
        if (!rva)
            return {};
        if (base_override)
            return checked_add(*base_override, *rva);
        return optional_value(context.image->rva_to_va(*rva));
    }
    return {};
}

std::optional<std::uint64_t> runtime_address(
    const workspace_context_t& context, const aida::analysis::address_t& address) {
    return runtime_address_with_base(context, address, display_base_override(context));
}

std::optional<std::uint64_t> provider_offset(
    const workspace_context_t& context, const aida::analysis::address_t& address) {
    using aida::analysis::address_space_id_t;
    if (!context.workspace)
        return {};
    if (address.space == address_space_id_t::file_offset)
        return address.value < context.workspace->provider().size()
            ? std::optional<std::uint64_t>(address.value) : std::nullopt;
    if (address.space == address_space_id_t::live_virtual) {
        const auto& module = context.workspace->identity().module();
        if (!module || address.value < module->base || address.value - module->base >= module->size)
            return {};
        return address.value - module->base;
    }
    if (!context.image)
        return {};
    std::optional<std::uint64_t> rva;
    if (address.space == address_space_id_t::relative_virtual)
        rva = address.value;
    else if (address.space == address_space_id_t::virtual_address)
        rva = optional_value(context.image->va_to_rva(address.value));
    return rva ? optional_value(context.image->rva_to_file_offset(*rva)) : std::nullopt;
}

aida::analysis::workspace_result_t<std::vector<std::uint8_t>> read_bytes(
    const workspace_context_t& context, const aida::analysis::address_t& address,
    std::size_t size) {
    using namespace aida::analysis;
    if (!context.workspace)
        return workspace_result_t<std::vector<std::uint8_t>>::failure(
            make_workspace_error(workspace_error_code_t::target_not_found,
                "workspace is unavailable", "ui_read"));
    const auto offset = provider_offset(context, address);
    if (!offset)
        return workspace_result_t<std::vector<std::uint8_t>>::failure(
            make_workspace_error(workspace_error_code_t::out_of_range,
                "address is not backed by the workspace provider", "ui_read"));
    return context.workspace->provider().read_vector(*offset, size, 64ULL << 20,
        context.workspace->cancellation_token());
}

std::string resolve_symbol(const workspace_context_t& context,
                           const aida::analysis::address_t& address) {
    if (!context.publication || !context.publication->snapshot)
        return {};
    const auto& symbols = context.publication->snapshot->symbols;
    auto found = std::lower_bound(symbols.begin(), symbols.end(), address,
        [](const aida::analysis::symbol_record_t& symbol,
           const aida::analysis::address_t& value) {
            return symbol.address < value;
        });
    if (found != symbols.end() && found->address == address)
        return found->name;
    if (found != symbols.begin()) {
        --found;
        if (found->address.space == address.space && found->address.value <= address.value) {
            char suffix[32]{};
            std::snprintf(suffix, sizeof(suffix), "+0x%llX",
                static_cast<unsigned long long>(address.value - found->address.value));
            return found->name + suffix;
        }
    }
    return {};
}

std::string resolve_name(const workspace_context_t& context,
                         const aida::analysis::address_t& address) {
    const auto presentation = context.publication
        ? context.publication->overlay_presentation : nullptr;
    if (presentation &&
        presentation->overlay_revision == context.publication->overlay_revision) {
        const auto found = std::lower_bound(
            presentation->renames.begin(), presentation->renames.end(), address,
            [](const auto& entry, const auto& value) {
                return entry.address < value;
            });
        if (found != presentation->renames.end() && found->address == address)
            return found->text;
    }
    return resolve_symbol(context, address);
}

std::string comment(const workspace_context_t& context,
                    const aida::analysis::address_t& address) {
    const auto presentation = context.publication
        ? context.publication->overlay_presentation : nullptr;
    if (!presentation ||
        presentation->overlay_revision != context.publication->overlay_revision)
        return {};
    const auto found = std::lower_bound(
        presentation->comments.begin(), presentation->comments.end(), address,
        [](const auto& entry, const auto& value) {
            return entry.address < value;
        });
    return found != presentation->comments.end() && found->address == address
        ? found->text : std::string();
}

std::string auto_comment(const workspace_context_t& context,
                         const aida::analysis::address_t& address) {
    return context.model ? context.model->automatic_comments.get(address) : std::string();
}

void request_format_range(const workspace_context_t& context,
                          std::size_t begin, std::size_t end) {
    request_format_page(context, begin, end);
}

std::optional<formatted_instruction_t> formatted_instruction(
    const workspace_context_t& context, aida::analysis::entity_id_t instruction_id) {
    if (!context.view)
        return {};
    std::lock_guard<std::mutex> lock(context.view->mutex);
    auto found = context.view->formatted.find(instruction_id);
    return found == context.view->formatted.end()
        ? std::nullopt : std::optional<formatted_instruction_t>(found->second);
}

bool queue_comment(const workspace_context_t& context,
                   const aida::analysis::address_t& address, std::string text,
                   std::optional<std::uint64_t> required_generation,
                   std::optional<std::uint64_t> required_analysis_revision,
                   std::optional<std::uint64_t> required_overlay_revision,
                   overlay_completion_t completion) {
    aida::analysis::overlay_operation_t operation;
    operation.kind = aida::analysis::overlay_operation_kind_t::comment;
    operation.address = address;
    operation.text = std::move(text);
    return queue_overlay_operation(context, std::move(operation), required_generation,
        required_analysis_revision, required_overlay_revision, std::move(completion));
}

bool queue_rename(const workspace_context_t& context,
                  const aida::analysis::address_t& address, std::string name,
                  std::optional<std::uint64_t> required_generation,
                  std::optional<std::uint64_t> required_analysis_revision,
                  std::optional<std::uint64_t> required_overlay_revision,
                  overlay_completion_t completion) {
    aida::analysis::overlay_operation_t operation;
    operation.kind = aida::analysis::overlay_operation_kind_t::name;
    operation.address = address;
    operation.name = std::move(name);
    return queue_overlay_operation(context, std::move(operation), required_generation,
        required_analysis_revision, required_overlay_revision, std::move(completion));
}

std::vector<bookmark_t> bookmark_snapshot(const workspace_context_t& context) {
    std::vector<bookmark_t> result;
    const auto presentation = context.publication
        ? context.publication->overlay_presentation : nullptr;
    if (!presentation ||
        presentation->overlay_revision != context.publication->overlay_revision)
        return result;
    result.reserve(presentation->bookmarks.size());
    for (const auto& entry : presentation->bookmarks)
        result.push_back({entry.address.value, entry.text});
    return result;
}

bool bookmarked(const workspace_context_t& context,
                const aida::analysis::address_t& address) {
    const auto presentation = context.publication
        ? context.publication->overlay_presentation : nullptr;
    if (!presentation ||
        presentation->overlay_revision != context.publication->overlay_revision)
        return false;
    const auto found = std::lower_bound(
        presentation->bookmarks.begin(), presentation->bookmarks.end(), address,
        [](const auto& entry, const auto& value) {
            return entry.address < value;
        });
    return found != presentation->bookmarks.end() && found->address == address;
}

bool queue_bookmark(const workspace_context_t& context,
                    const aida::analysis::address_t& address, std::string label) {
    aida::analysis::overlay_operation_t operation;
    operation.kind = aida::analysis::overlay_operation_kind_t::bookmark;
    operation.address = address;
    operation.name = std::move(label);
    return queue_overlay_operation(context, std::move(operation));
}

bool queue_patch(const workspace_context_t& context,
                 const aida::analysis::address_t& address,
                 std::vector<std::uint8_t> bytes) {
    aida::analysis::overlay_operation_t operation;
    operation.kind = aida::analysis::overlay_operation_kind_t::byte_patch;
    operation.address = address;
    operation.bytes = std::move(bytes);
    return queue_overlay_operation(context, std::move(operation));
}

bool queue_type_application(const workspace_context_t& context,
                            const aida::analysis::address_t& address,
                            std::string type,
                            std::optional<std::uint64_t> required_generation,
                            std::optional<std::uint64_t> required_analysis_revision,
                            std::optional<std::uint64_t> required_overlay_revision,
                            overlay_completion_t completion) {
    aida::analysis::overlay_operation_t operation;
    operation.kind = aida::analysis::overlay_operation_kind_t::type_application;
    operation.address = address;
    operation.type = std::move(type);
    return queue_overlay_operation(context, std::move(operation), required_generation,
        required_analysis_revision, required_overlay_revision, std::move(completion));
}

bool queue_type_declaration(const workspace_context_t& context,
                            std::string declaration) {
    if (!context.workspace || !context.image)
        return false;
    aida::analysis::overlay_operation_t operation;
    operation.kind = aida::analysis::overlay_operation_kind_t::type_declaration;
    operation.address.space = aida::analysis::address_space_id_t::relative_virtual;
    operation.address.value = 0;
    operation.address.architecture = context.workspace->identity().architecture();
    operation.address.mode = context.image->architecture_mode();
    operation.text = std::move(declaration);
    return queue_overlay_operation(context, std::move(operation));
}

bool queue_type_declaration_and_application(
    const workspace_context_t& context,
    const aida::analysis::address_t& address,
    std::string declaration,
    std::string canonical_type) {
    if (declaration.empty() || canonical_type.empty())
        return false;
    aida::analysis::overlay_operation_t declaration_operation;
    declaration_operation.kind = aida::analysis::overlay_operation_kind_t::type_declaration;
    declaration_operation.address.space = aida::analysis::address_space_id_t::relative_virtual;
    declaration_operation.address.value = 0;
    declaration_operation.address.architecture = context.workspace
        ? context.workspace->identity().architecture()
        : address.architecture;
    declaration_operation.address.mode = context.image
        ? context.image->architecture_mode() : address.mode;
    declaration_operation.text = std::move(declaration);
    aida::analysis::overlay_operation_t application_operation;
    application_operation.kind = aida::analysis::overlay_operation_kind_t::type_application;
    application_operation.address = address;
    application_operation.type = std::move(canonical_type);
    std::vector<aida::analysis::overlay_operation_t> operations;
    operations.reserve(2);
    operations.push_back(std::move(declaration_operation));
    operations.push_back(std::move(application_operation));
    return queue_overlay_transaction(context, std::move(operations));
}

mutation_state_t mutation_state(const workspace_context_t& context) {
    mutation_state_t result;
    const auto authority = authoritative_state(context);
    if (!context.workspace || !authority)
        return result;
    result.pending = authority->pending_mutations.load(std::memory_order_acquire);
    result.overlay_revision = context.workspace->overlay_revision();
    std::lock_guard<std::mutex> lock(authority->mutex);
    result.error = authority->mutation_error;
    result.derived_publication_pending =
        authority->derived_publication_retry_pending.load(
            std::memory_order_acquire);
    result.derived_publication_revision =
        authority->derived_publication_revision;
    result.derived_publication_error =
        authority->derived_publication_error;
    return result;
}

bool queue_overlay_undo(const workspace_context_t& context) {
    if (!context.workspace)
        return false;
    return queue_overlay_history(context, false, context.workspace->generation(),
        context.workspace->analysis_revision(), context.workspace->overlay_revision());
}

bool queue_overlay_redo(const workspace_context_t& context) {
    if (!context.workspace)
        return false;
    return queue_overlay_history(context, true, context.workspace->generation(),
        context.workspace->analysis_revision(), context.workspace->overlay_revision());
}

namespace {

constexpr std::size_t kWorkspaceNavigationHistoryLimit = 4096;

void trim_workspace_navigation(std::vector<aida::analysis::address_t>& entries) {
    if (entries.size() > kWorkspaceNavigationHistoryLimit)
        entries.erase(entries.begin(),
            entries.begin() + static_cast<std::ptrdiff_t>(
                entries.size() - kWorkspaceNavigationHistoryLimit));
}

bool publish_workbench_selection(const workspace_context_t& context,
                                 const aida::analysis::address_t& destination,
                                 bool record_history) {
    const auto runtime = runtime_address(context, destination).value_or(destination.value);
    aida::workbench::selection_context_t selection;
    selection.kind = aida::workbench::selection_kind_t::address;
    selection.has_address = true;
    selection.address = runtime;
    selection.extent = 1;
    selection.entity_key = "analysis.address." + std::to_string(destination.value);
    aida::workbench::document_local_cursor_t cursor;
    cursor.has_position = true;
    cursor.position = runtime;
    aida::workbench::workbench_shell_workspace_context_t workbench;
    if (record_history) {
        return aida::workbench::workbench_shell_runtime_t::instance()
            .publish_selection(context.workspace, selection, cursor,
                aida::workbench::navigation_origin_t::user, workbench).ok();
    }
    return true;
}

void synchronize_workspace_selection(const workspace_context_t& context,
                                     const aida::analysis::address_t& destination,
                                     bool reveal) {
    if (!context.workspace || !context.view)
        return;
    const auto updated = context.workspace->update_view_state(
        [&](aida::analysis::workspace_view_state_t& state) {
            state.selection = destination;
        });
    if (!updated)
        return;
    if (context.model) {
        context.model->presentation_selection_revision.store(
            context.workspace->view_state().revision, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        context.view->selection = destination;
        context.view->scroll_to_selection = reveal;
    }
    note_ui_state_changed(context.view);
}

void synchronize_workspace_navigation(const workspace_context_t& context,
                                      const aida::analysis::address_t& destination,
                                      bool reveal,
                                      bool record_navigation,
                                      bool clear_forward) {
    if (!context.workspace || !context.view)
        return;
    std::optional<aida::analysis::address_t> current;
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        current = context.view->selection;
    }
    const auto updated = context.workspace->update_view_state(
        [&](aida::analysis::workspace_view_state_t& state) {
            const auto previous = state.selection ? state.selection : current;
            if (record_navigation && previous && *previous != destination) {
                state.navigation_back.push_back(*previous);
                trim_workspace_navigation(state.navigation_back);
            }
            if (clear_forward)
                state.navigation_forward.clear();
            state.selection = destination;
        });
    if (!updated)
        return;
    if (context.model) {
        context.model->presentation_selection_revision.store(
            context.workspace->view_state().revision, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        context.view->selection = destination;
        context.view->scroll_to_selection = reveal;
    }
    note_ui_state_changed(context.view);
}

std::optional<aida::analysis::address_t> navigate_workspace_history(
    const workspace_context_t& context,
    bool forward) {
    if (!context.workspace || !context.view)
        return {};
    std::optional<aida::analysis::address_t> destination;
    const auto updated = context.workspace->update_view_state(
        [&](aida::analysis::workspace_view_state_t& state) {
            auto& source = forward ? state.navigation_forward : state.navigation_back;
            auto& target = forward ? state.navigation_back : state.navigation_forward;
            if (source.empty())
                return;
            destination = source.back();
            source.pop_back();
            if (state.selection && *state.selection != *destination) {
                target.push_back(*state.selection);
                trim_workspace_navigation(target);
            }
            state.selection = *destination;
        });
    if (!updated || !destination)
        return {};
    if (context.model) {
        context.model->presentation_selection_revision.store(
            context.workspace->view_state().revision, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        context.view->selection = *destination;
        context.view->scroll_to_selection = true;
    }
    note_ui_state_changed(context.view);
    return destination;
}

std::optional<aida::analysis::address_t> history_selection(
    const workspace_context_t& context,
    const aida::workbench::workbench_shell_workspace_context_t& workbench) {
    const auto active = std::find_if(
        workbench.persistence.documents.begin(), workbench.persistence.documents.end(),
        [&workbench](const aida::workbench::document_persistence_dto_t& document) {
            return document.id == workbench.persistence.active_document;
        });
    if (active == workbench.persistence.documents.end() ||
        !active->local_state.selection.has_address)
        return {};
    return typed_address(context, active->local_state.selection.address);
}

}

void select_address(const aida::analysis::address_t& destination,
                    const workspace_context_t& context,
                    bool record_history) {
    if (!context.workspace || !context.view || !context.model)
        return;
    std::optional<aida::analysis::address_t> current;
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        current = context.view->selection;
    }
    if (current && *current == destination) {
        synchronize_workspace_selection(context, destination, false);
        return;
    }
    if (record_history && !publish_workbench_selection(context, destination, true))
        return;
    synchronize_workspace_selection(context, destination, false);
}

void select_address(std::uint64_t value, const workspace_context_t& context,
                    bool record_history) {
    const auto destination = typed_address(context, value);
    if (destination)
        select_address(*destination, context, record_history);
}

void goto_address(const aida::analysis::address_t& destination,
                  const workspace_context_t& context) {
    if (!context.workspace || !context.view || !context.model)
        return;
    std::optional<aida::analysis::address_t> current;
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        current = context.view->selection;
    }
    if (current && *current == destination) {
        synchronize_workspace_selection(context, destination, true);
        return;
    }
    if (!publish_workbench_selection(context, destination, true))
        return;
    synchronize_workspace_navigation(context, destination, true, true, true);
}

void goto_address(std::uint64_t value, const workspace_context_t& context) {
    const auto destination = typed_address(context, value);
    if (!destination)
        return;
    goto_address(*destination, context);
}

bool request_goto(const workspace_context_t& context) {
    if (!context.view)
        return false;
    const auto hooks = view_hooks_for(context.view);
    if (!hooks || !hooks->show_goto)
        return false;
    gui_post_or_run([hooks]() {
        hooks->show_goto();
    });
    return true;
}

void navigate_back(const workspace_context_t& context) {
    if (!context.workspace)
        return;
    const auto local_destination = navigate_workspace_history(context, false);
    aida::workbench::workbench_shell_workspace_context_t workbench;
    const auto result = aida::workbench::workbench_shell_runtime_t::instance()
        .navigate_history(context.workspace, false, workbench);
    if (!result || local_destination)
        return;
    if (const auto destination = history_selection(context, workbench))
        synchronize_workspace_selection(context, *destination, true);
}

void navigate_forward(const workspace_context_t& context) {
    if (!context.workspace)
        return;
    const auto local_destination = navigate_workspace_history(context, true);
    aida::workbench::workbench_shell_workspace_context_t workbench;
    const auto result = aida::workbench::workbench_shell_runtime_t::instance()
        .navigate_history(context.workspace, true, workbench);
    if (!result || local_destination)
        return;
    if (const auto destination = history_selection(context, workbench))
        synchronize_workspace_selection(context, *destination, true);
}

void open_xrefs(std::uint64_t value, const workspace_context_t& context) {
    const auto address = typed_address(context, value);
    if (!address || !context.publication || !context.publication->snapshot ||
        context.view->xref_scanning.exchange(true, std::memory_order_acq_rel))
        return;
    const auto hooks = view_hooks_for(context.view);
    if (hooks && hooks->show_xref_popup) {
        gui_post_or_run([hooks, address = *address]() {
            hooks->show_xref_popup(address);
        });
    }
    const std::string target_id = context.workspace->identity().binary_id().to_hex();
    aida::infra::taskflow_runtime::task_descriptor_t descriptor;
    descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::feature_worker;
    descriptor.owner_subsystem = "analysis_ui";
    descriptor.label = "workspace_xref_query";
    descriptor.target_id = target_id.c_str();
    descriptor.generation = context.publication->generation;
    descriptor.cancellable_body = [context, address = *address](
        const aida::infra::taskflow_runtime::cancellation_token_t& cancel) {
        std::vector<xref_popup_entry_t> results;
        results.reserve(256);
        constexpr std::size_t maximum_results = 10000;
        const auto indexes = aida::analysis::publication_indexes::for_publication(
            context.publication, context.workspace->cancellation_token());
        if (indexes) {
            const auto& xrefs = context.publication->snapshot->xrefs;
            const auto range = indexes->xrefs_to(address);
            for (std::uint32_t ordinal = range.begin; ordinal < range.end; ++ordinal) {
                if (cancel.requested.load(std::memory_order_acquire) ||
                    context.workspace->cancellation_token().stop_requested())
                    break;
                const auto& xref = xrefs[indexes->xref_to_entry(ordinal)];
                if (xref.target != address)
                    continue;
                xref_popup_entry_t entry;
                entry.addr = runtime_address(context, xref.source).value_or(xref.source.value);
                entry.type = static_cast<int>(xref.kind);
                entry.module_name = context.workspace->identity().bin_name();
                entry.function_name = resolve_name(context, xref.source);
                results.push_back(std::move(entry));
                if (results.size() >= maximum_results)
                    break;
            }
        }
        context.view->xref_scanning.store(false, std::memory_order_release);
        aida::qt::analysis_bridge::xref_delivery_t delivery;
        delivery.address = address;
        delivery.results = std::move(results);
        deliver_xref_results(context, std::move(delivery));
    };
    const auto submitted = aida::infra::taskflow_runtime::submit(std::move(descriptor));
    if (!submitted.submitted) {
        context.view->xref_scanning.store(false, std::memory_order_release);
        aida::qt::analysis_bridge::xref_delivery_t delivery;
        delivery.address = *address;
        delivery.error = submitted.reject_reason;
        deliver_xref_results(context, std::move(delivery));
        {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            context.view->format_error = submitted.reject_reason;
        }
        note_ui_state_changed(context.view);
    }
}

void bump_format_generation(const workspace_context_t& context) {
    if (!context.model || !context.view)
        return;
    context.model->format_generation.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        context.view->formatted.clear();
        context.view->formatted_revision.fetch_add(1, std::memory_order_acq_rel);
        context.view->pending_format_pages.clear();
    }
    note_ui_state_changed(context.view);
    deliver_format_reset(context);
}

void bump_format_generation() {
    bump_format_generation(capture_selected_workspace());
}

std::uint32_t format_generation(const workspace_context_t& context) {
    return context.model ? context.model->format_generation.load(std::memory_order_acquire) : 0;
}

std::uint64_t enclosing_function_start(std::uint64_t value,
                                       const workspace_context_t& context) {
    const auto address = typed_address(context, value);
    if (!address || !context.publication || !context.publication->snapshot)
        return 0;
    const auto& functions = context.publication->snapshot->functions;
    auto found = std::upper_bound(functions.begin(), functions.end(), *address,
        [](const aida::analysis::address_t& target,
           const aida::analysis::function_record_t& function) {
            return target < function.start;
        });
    if (found == functions.begin())
        return 0;
    --found;
    if (found->start.space != address->space || address->value < found->start.value ||
        address->value >= found->end.value)
        return 0;
    return runtime_address(context, found->start).value_or(found->start.value);
}

std::optional<std::pair<std::size_t, std::size_t>> instruction_range(
    const workspace_context_t& context) {
    if (!context || !context.publication->snapshot)
        return {};
    const auto& instructions = context.publication->snapshot->instructions;
    std::size_t begin = 0;
    std::size_t end = instructions.size();
    int section_index = -1;
    const auto mirrored = context.model->section_filter_mirror.find(context.view.get());
    if (mirrored != context.model->section_filter_mirror.end())
        section_index = mirrored->second;
    if (section_index < 0 || !context.image ||
        static_cast<std::size_t>(section_index) >= context.image->sections().size())
        return std::make_pair(begin, end);
    const auto& section = context.image->sections()[static_cast<std::size_t>(section_index)];
    const auto section_end = checked_add(section.virtual_address,
        (std::max)(section.virtual_size, section.raw_size));
    if (!section_end)
        return {};
    const auto lower = std::lower_bound(instructions.begin(), instructions.end(),
        section.virtual_address, [](const instruction_record_t& instruction, std::uint64_t rva) {
            return instruction.address.value < rva;
        });
    const auto upper = std::lower_bound(lower, instructions.end(), *section_end,
        [](const instruction_record_t& instruction, std::uint64_t rva) {
            return instruction.address.value < rva;
        });
    begin = static_cast<std::size_t>(std::distance(instructions.begin(), lower));
    end = static_cast<std::size_t>(std::distance(instructions.begin(), upper));
    return std::make_pair(begin, end);
}

std::string address_label(const workspace_context_t& context,
                          const address_t& address, addr_format_t format) {
    char buffer[48]{};
    if (format == addr_format_t::rva) {
        std::uint64_t rva = address.value;
        if (address.space != address_space_id_t::relative_virtual && context.image) {
            const auto runtime = runtime_address(context, address);
            if (runtime) {
                const auto base = display_base_override(context);
                const auto translated = base && *runtime >= *base
                    ? std::optional<std::uint64_t>(*runtime - *base)
                    : optional_value(context.image->va_to_rva(*runtime));
                if (translated)
                    rva = *translated;
            }
        }
        std::snprintf(buffer, sizeof(buffer), "+%08llX",
            static_cast<unsigned long long>(rva));
    } else if (format == addr_format_t::file_offset) {
        const auto offset = provider_offset(context, address);
        if (offset)
            std::snprintf(buffer, sizeof(buffer), "%08llX",
                static_cast<unsigned long long>(*offset));
        else
            std::snprintf(buffer, sizeof(buffer), "--------");
    } else {
        std::snprintf(buffer, sizeof(buffer), "%016llX",
            static_cast<unsigned long long>(runtime_address(context, address).value_or(address.value)));
    }
    return buffer;
}

std::optional<std::uint64_t> parse_address_text(const workspace_context_t& context,
                                                std::string text) {
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](char value) {
        return value != ' ' && value != '\t';
    }));
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    if (text.empty())
        return {};
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text.erase(0, 2);
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (parsed.ec == std::errc() && parsed.ptr == text.data() + text.size())
        return value;
    if (context.publication && context.publication->snapshot) {
        const auto& symbols = context.publication->snapshot->symbols;
        auto found = std::find_if(symbols.begin(), symbols.end(), [&](const symbol_record_t& symbol) {
            return symbol.name == text;
        });
        if (found != symbols.end())
            return runtime_address(context, found->address);
    }
    return {};
}

const aida::analysis::pe_section_t* section_for(
    const workspace_context_t& context, const aida::analysis::address_t& address) {
    if (!context.image)
        return nullptr;
    const auto runtime = runtime_address(context, address).value_or(address.value);
    const auto rva = optional_value(context.image->va_to_rva(runtime));
    if (!rva)
        return nullptr;
    const auto found = std::find_if(context.image->sections().begin(),
        context.image->sections().end(), [&](const auto& section) {
            const std::uint64_t extent = (std::max)(section.virtual_size, section.raw_size);
            return *rva >= section.virtual_address &&
                *rva - section.virtual_address < extent;
        });
    return found == context.image->sections().end() ? nullptr : &*found;
}

bool apply_rebase(const workspace_context_t& context, std::uint64_t new_base,
                  std::string* error) {
    if (!context.workspace || !context.image || !context.view) {
        if (error) *error = "Open a static file-backed analysis workspace before rebasing";
        return false;
    }
    if (new_base == 0 ||
        context.image->image_size() > (std::numeric_limits<std::uint64_t>::max)() - new_base) {
        if (error) *error = "Invalid image base.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        context.view->display_image_base = new_base == context.image->image_base()
            ? std::nullopt : std::optional<std::uint64_t>(new_base);
        context.view->formatted.clear();
        context.view->formatted_revision.fetch_add(1, std::memory_order_acq_rel);
        context.view->pending_format_pages.clear();
    }
    context.model->format_generation.fetch_add(1, std::memory_order_acq_rel);
    note_ui_state_changed(context.view);
    deliver_format_reset(context);
    if (error) error->clear();
    return true;
}

bool request_rebase(const workspace_context_t& context, std::string* error) {
    if (!context.workspace || !context.image || !context.view) {
        if (error) *error = "Open a static file-backed analysis workspace before rebasing";
        return false;
    }
    if (context.workspace->target_kind() != aida::analysis::target_kind_t::static_file) {
        if (error) *error = "Rebase is available only for static file-backed workspaces";
        return false;
    }
    const auto hook = aida::qt::analysis_bridge::rebase_dialog_hook();
    if (!hook) {
        if (error) *error = "The rebase presenter is unavailable.";
        return false;
    }
    gui_post_or_run([context, hook]() {
        hook(context);
    });
    if (error) error->clear();
    return true;
}

namespace {

std::optional<std::string>& export_path_override() {
    static std::optional<std::string> value;
    return value;
}

std::mutex& export_path_mutex() {
    static std::mutex value;
    return value;
}

void queue_listing_export(const workspace_context_t& context) {
    if (!context || !context.image || !context.publication ||
        !context.publication->snapshot ||
        context.view->export_pending.exchange(true, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard<std::mutex> lock(context.view->mutex);
        context.view->export_error.clear();
        context.view->export_status = "Exporting full listing...";
    }
    note_ui_state_changed(context.view);
    const std::string target_id = context.workspace->identity().binary_id().to_hex();
    std::string export_path = "aida_disasm_dump.txt";
    {
        std::lock_guard<std::mutex> lock(export_path_mutex());
        if (export_path_override())
            export_path = *export_path_override();
    }
    const std::string temporary_path = export_path + "." + target_id + "." +
        std::to_string(context.publication->generation) + ".tmp";
    const auto presentation_base = display_base_override(context);
    aida::infra::taskflow_runtime::task_descriptor_t descriptor;
    descriptor.domain = aida::infra::taskflow_runtime::executor_domain_t::long_running;
    descriptor.owner_subsystem = "analysis_ui";
    descriptor.label = "export_workspace_disassembly";
    descriptor.target_id = target_id.c_str();
    descriptor.generation = context.publication->generation;
    descriptor.cancellable_body = [context, export_path, temporary_path, presentation_base](
        const aida::infra::taskflow_runtime::cancellation_token_t& runtime_cancel) {
        std::string error;
        std::size_t written = 0;
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to open " + export_path + " for writing.";
        } else {
            auto decoder_result = worker_owned_x86_decoder_t::create(
                context.image->architecture_mode());
            if (!decoder_result) {
                error = decoder_result.error().stable_code() + ": " +
                    decoder_result.error().message;
            } else {
                auto decoder = decoder_result.take_value();
                instruction_format_options_t options;
                options.maximum_text_bytes = 2048;
                for (const auto& instruction : context.publication->snapshot->instructions) {
                    if (runtime_cancel.requested.load(std::memory_order_acquire) ||
                        context.workspace->cancellation_token().stop_requested()) {
                        error = "Listing export cancelled.";
                        break;
                    }
                    const auto formatted = decoder->format_one(context.workspace->provider(),
                        *context.image, instruction, options,
                        context.workspace->cancellation_token());
                    if (!formatted) {
                        error = formatted.error().stable_code() + ": " + formatted.error().message;
                        break;
                    }
                    std::string bytes;
                    if (const auto offset = provider_offset(context, instruction.address)) {
                        auto lease = context.workspace->provider().lease(*offset,
                            instruction.length, context.workspace->cancellation_token());
                        if (lease)
                            bytes = byte_text(lease.value(), *offset, *offset, instruction.length);
                    }
                    std::uint64_t display_address = instruction.address.value;
                    if (presentation_base && instruction.address.space ==
                        aida::analysis::address_space_id_t::relative_virtual) {
                        display_address = checked_add(*presentation_base,
                            instruction.address.value).value_or(instruction.address.value);
                    } else {
                        display_address = runtime_address(context,
                            instruction.address).value_or(instruction.address.value);
                    }
                    char address[32]{};
                    std::snprintf(address, sizeof(address), "%016llX",
                        static_cast<unsigned long long>(display_address));
                    output << address << "  " << bytes << "  " << formatted.value() << "\r\n";
                    if (!output) {
                        error = "Writing " + export_path + " failed.";
                        break;
                    }
                    ++written;
                }
            }
        }
        output.close();
        if (error.empty() && !MoveFileExA(temporary_path.c_str(), export_path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "Replacing " + export_path + " failed with Win32 error " +
                std::to_string(GetLastError()) + ".";
        }
        if (!error.empty())
            DeleteFileA(temporary_path.c_str());
        {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            context.view->export_error = std::move(error);
            context.view->export_status = context.view->export_error.empty()
                ? "Exported " + std::to_string(written) +
                    " instructions to " + export_path + "."
                : std::string();
        }
        context.view->export_pending.store(false, std::memory_order_release);
        note_ui_state_changed(context.view);
        aida::qt::analysis_bridge::export_delivery_t delivery;
        {
            std::lock_guard<std::mutex> lock(context.view->mutex);
            delivery.status = context.view->export_status;
            delivery.error = context.view->export_error;
        }
        deliver_export_status(context, std::move(delivery));
    };
    const auto submitted = aida::infra::taskflow_runtime::submit(std::move(descriptor));
    if (!submitted.submitted) {
        context.view->export_pending.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(context.view->mutex);
        context.view->export_status.clear();
        context.view->export_error = submitted.reject_reason.empty()
            ? "Listing export queue rejected the request." : submitted.reject_reason;
        note_ui_state_changed(context.view);
    }
}

}

bool request_listing_export(const workspace_context_t& context, std::string* error) {
    if (!context || !context.image || !context.publication ||
        !context.publication->snapshot) {
        if (error) *error = "A published file-backed analysis listing is required for export";
        return false;
    }
    if (context.view->export_pending.load(std::memory_order_acquire)) {
        if (error) *error = "A disassembly listing export is already running";
        return false;
    }
    const auto chooser = aida::qt::analysis_bridge::export_path_hook();
    if (chooser) {
        const auto chosen = chooser(context);
        if (!chosen) {
            if (error) error->clear();
            return true;
        }
        std::lock_guard<std::mutex> lock(export_path_mutex());
        export_path_override() = *chosen;
    }
    queue_listing_export(context);
    if (context.view->export_pending.load(std::memory_order_acquire)) {
        if (error) error->clear();
        return true;
    }
    std::lock_guard<std::mutex> lock(context.view->mutex);
    if (error) *error = context.view->export_error.empty()
        ? "The disassembly listing export request was rejected"
        : context.view->export_error;
    return false;
}

bool request_rename_dialog(const workspace_context_t& context,
                           const aida::analysis::address_t& address) {
    if (!context)
        return false;
    const auto hook = aida::qt::analysis_bridge::rename_dialog_hook();
    if (!hook)
        return false;
    gui_post_or_run([context, address, hook]() {
        hook(context, address);
    });
    return true;
}

bool request_comment_dialog(const workspace_context_t& context,
                            const aida::analysis::address_t& address) {
    if (!context)
        return false;
    const auto hook = aida::qt::analysis_bridge::comment_dialog_hook();
    if (!hook)
        return false;
    gui_post_or_run([context, address, hook]() {
        hook(context, address);
    });
    return true;
}

}

namespace aida::qt::analysis_bridge {

namespace {

std::mutex& hook_mutex() {
    static std::mutex value;
    return value;
}

disasm_view::dialog_open_hook_t& rename_dialog_slot() {
    static disasm_view::dialog_open_hook_t value;
    return value;
}

disasm_view::dialog_open_hook_t& comment_dialog_slot() {
    static disasm_view::dialog_open_hook_t value;
    return value;
}

disasm_view::rebase_dialog_hook_t& rebase_dialog_slot() {
    static disasm_view::rebase_dialog_hook_t value;
    return value;
}

disasm_view::static_patch_review_hook_t& static_patch_review_slot() {
    static disasm_view::static_patch_review_hook_t value;
    return value;
}

view_focus_hook_t& view_focus_slot() {
    static view_focus_hook_t value;
    return value;
}

export_path_hook_t& export_path_slot() {
    static export_path_hook_t value;
    return value;
}

focused_presentation_key_fn_t& focused_presentation_key_slot() {
    static focused_presentation_key_fn_t value;
    return value;
}

}

disasm_view::dialog_open_hook_t rename_dialog_hook() {
    std::lock_guard<std::mutex> lock(hook_mutex());
    return rename_dialog_slot();
}

disasm_view::dialog_open_hook_t comment_dialog_hook() {
    std::lock_guard<std::mutex> lock(hook_mutex());
    return comment_dialog_slot();
}

disasm_view::rebase_dialog_hook_t rebase_dialog_hook() {
    std::lock_guard<std::mutex> lock(hook_mutex());
    return rebase_dialog_slot();
}

disasm_view::static_patch_review_hook_t static_patch_review_hook() {
    std::lock_guard<std::mutex> lock(hook_mutex());
    return static_patch_review_slot();
}

void set_view_focus_hook(view_focus_hook_t hook) {
    std::lock_guard<std::mutex> lock(hook_mutex());
    view_focus_slot() = std::move(hook);
}

view_focus_hook_t view_focus_hook() {
    std::lock_guard<std::mutex> lock(hook_mutex());
    return view_focus_slot();
}

void set_export_path_hook(export_path_hook_t hook) {
    std::lock_guard<std::mutex> lock(hook_mutex());
    export_path_slot() = std::move(hook);
}

export_path_hook_t export_path_hook() {
    std::lock_guard<std::mutex> lock(hook_mutex());
    return export_path_slot();
}

void set_focused_presentation_key_hook(focused_presentation_key_fn_t hook) {
    std::lock_guard<std::mutex> lock(hook_mutex());
    focused_presentation_key_slot() = std::move(hook);
}

std::string focused_presentation_key() {
    std::lock_guard<std::mutex> lock(hook_mutex());
    return focused_presentation_key_slot() ? focused_presentation_key_slot()()
                                           : std::string();
}

std::uint64_t disasm_evidence_hash(const std::string& value) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

void set_format_delivery_target(const std::shared_ptr<disasm_view::state_t>& view,
                                std::weak_ptr<format_delivery_fn> target) {
    if (!view)
        return;
    std::lock_guard<std::mutex> lock(disasm_view::delivery_registry_mutex());
    disasm_view::delivery_registry()[view.get()].format = std::move(target);
}

void set_xref_delivery_target(const std::shared_ptr<disasm_view::state_t>& view,
                              std::weak_ptr<xref_delivery_fn> target) {
    if (!view)
        return;
    std::lock_guard<std::mutex> lock(disasm_view::delivery_registry_mutex());
    disasm_view::delivery_registry()[view.get()].xref = std::move(target);
}

void set_export_delivery_target(const std::shared_ptr<disasm_view::state_t>& view,
                                std::weak_ptr<export_delivery_fn> target) {
    if (!view)
        return;
    std::lock_guard<std::mutex> lock(disasm_view::delivery_registry_mutex());
    disasm_view::delivery_registry()[view.get()].export_status = std::move(target);
}

void set_view_hooks(const std::shared_ptr<disasm_view::state_t>& view,
                    std::weak_ptr<view_hooks_t> hooks) {
    if (!view)
        return;
    std::lock_guard<std::mutex> lock(disasm_view::delivery_registry_mutex());
    disasm_view::delivery_registry()[view.get()].hooks = std::move(hooks);
}

void clear_delivery_targets(const std::shared_ptr<disasm_view::state_t>& view) {
    if (!view)
        return;
    std::lock_guard<std::mutex> lock(disasm_view::delivery_registry_mutex());
    disasm_view::delivery_registry().erase(view.get());
}

void update_section_filter_mirror(const disasm_view::workspace_context_t& context,
                                  int section_index) {
    if (!context.model || !context.view)
        return;
    context.model->section_filter_mirror[context.view.get()] = section_index;
}

std::string normalize_presentation_key(std::string_view key) {
    return disasm_view::normalize_presentation_key(key);
}

}

namespace disasm_view {

void set_rename_dialog_hook(dialog_open_hook_t hook) {
    std::lock_guard<std::mutex> lock(aida::qt::analysis_bridge::hook_mutex());
    aida::qt::analysis_bridge::rename_dialog_slot() = std::move(hook);
}

void set_comment_dialog_hook(dialog_open_hook_t hook) {
    std::lock_guard<std::mutex> lock(aida::qt::analysis_bridge::hook_mutex());
    aida::qt::analysis_bridge::comment_dialog_slot() = std::move(hook);
}

void set_rebase_dialog_hook(rebase_dialog_hook_t hook) {
    std::lock_guard<std::mutex> lock(aida::qt::analysis_bridge::hook_mutex());
    aida::qt::analysis_bridge::rebase_dialog_slot() = std::move(hook);
}

void set_static_patch_review_hook(static_patch_review_hook_t hook) {
    std::lock_guard<std::mutex> lock(aida::qt::analysis_bridge::hook_mutex());
    aida::qt::analysis_bridge::static_patch_review_slot() = std::move(hook);
}

}
