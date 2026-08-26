#pragma once

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/analysis/workspace/byte_provider.hpp"
#include "core/disasm/disasm_view.hpp"
#include "core/infra/taskflow_runtime.hpp"

#include <QObject>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class QTimer;

namespace aida::qt::editor {

struct hex_ui_state_t {
    std::uint64_t base_addr = 0;
    std::string source_name;
    bool active = false;
    std::int64_t sel_start = -1;
    std::int64_t sel_end = -1;
    bool selecting = false;
    float scroll_y = 0.0f;
    float target_scroll_y = 0.0f;
    bool goto_visible = false;
    char goto_buf[32] = {};
    bool search_visible = false;
    char search_buf[256] = {};
    bool search_hex = true;
    std::int64_t search_match = -1;
    std::uint64_t search_match_len = 0;
    std::int64_t search_match_idx = -1;
    std::vector<std::uint64_t> search_matches;
    std::string search_last_query;
    bool search_last_hex = true;
    std::uint64_t context_offset = 0;
    std::uint64_t context_generation = 0;
    std::uint64_t context_overlay_revision = 0;
    std::uint8_t context_value = 0;
    bool context_live = false;
    bool context_valid = false;
};

struct hex_patch_span_t {
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> bytes;
};

enum class hex_source_kind_t : std::uint8_t {
    workspace_provider,
    live_memory
};

std::optional<std::vector<std::uint8_t>> hex_parse_bytes(std::string text);
std::optional<std::uint64_t> hex_parse_u64(std::string text);
std::vector<std::uint8_t> hex_search_pattern(const hex_ui_state_t& state);

class AidaHexDocument final : public QObject,
    public aida::analysis::workspace_lifecycle_participant_t,
    public std::enable_shared_from_this<AidaHexDocument> {
    Q_OBJECT
public:
    explicit AidaHexDocument(QObject* parent = nullptr);
    ~AidaHexDocument() override;

    AidaHexDocument(const AidaHexDocument&) = delete;
    AidaHexDocument& operator=(const AidaHexDocument&) = delete;

    void request_cancel() noexcept override;
    aida::analysis::workspace_result_t<void> drain(
        std::chrono::steady_clock::time_point deadline) override;

    std::mutex mutex;
    hex_ui_state_t ui;
    std::weak_ptr<aida::analysis::analysis_workspace_t> owner;
    aida::analysis::binary_id_t owner_id;
    hex_source_kind_t source_kind = hex_source_kind_t::workspace_provider;
    std::vector<std::uint8_t> live_bytes;
    std::uint64_t live_base = 0;
    aida::analysis::byte_view_t window;
    std::uint64_t window_offset = 0;
    std::uint64_t window_size = 0;
    std::uint64_t patch_revision = (std::numeric_limits<std::uint64_t>::max)();
    std::vector<hex_patch_span_t> patches;
    std::atomic<bool> patch_refreshing{false};
    std::shared_ptr<aida::analysis::cancellation_source_t> search_cancellation;
    std::atomic<std::uint64_t> search_serial{1};
    std::atomic<bool> searching{false};
    std::atomic<bool> live_loading{false};
    std::atomic<std::uint64_t> live_request_serial{0};
    std::atomic<std::uint64_t> live_dispatch_failure_serial{0};
    std::shared_ptr<std::atomic<bool>> live_cancellation;
    std::atomic<bool> cancelled{false};
    std::atomic<std::uint32_t> pending_jobs{0};
    std::mutex drain_mutex;
    std::condition_variable drain_cv;
    aida::infra::taskflow_runtime::job_handle_t patch_job;
    aida::infra::taskflow_runtime::job_handle_t search_job;
    std::uint64_t scroll_to_offset = (std::numeric_limits<std::uint64_t>::max)();
    std::string error;

    void consumeDispatchFailure();
    void requestPatchRefresh(const disasm_view::workspace_context_t& context);
    bool ensureWindow(const disasm_view::workspace_context_t& context, std::uint64_t begin,
                      std::uint64_t end);
    std::uint8_t patchedByte(std::uint64_t offset, std::uint8_t original, bool* patched) const;
    void startSearch(const disasm_view::workspace_context_t& context);
    void stepSearchResult(int direction);
    bool requestLiveMemory(const disasm_view::workspace_context_t& context,
                           std::uint64_t address, std::size_t size);
    void activate(const disasm_view::workspace_context_t& context);
    bool focusAddress(const disasm_view::workspace_context_t& context,
                      const aida::analysis::address_t& address, std::string* error);
    bool stateMatches(const disasm_view::workspace_context_t& context) const;
    bool isActive(const disasm_view::workspace_context_t& context);
    std::string sourceName(const disasm_view::workspace_context_t& context);
    std::string lastError();
    std::uint64_t byteCount(const disasm_view::workspace_context_t& context);

Q_SIGNALS:
    void stateChanged();
    void searchStateChanged();
    void liveStateChanged();
    void errorChanged();

private:
    struct pending_job_guard_t {
        explicit pending_job_guard_t(std::shared_ptr<AidaHexDocument> state_value)
            : state(std::move(state_value)) {
            state->pending_jobs.fetch_add(1, std::memory_order_acq_rel);
        }
        ~pending_job_guard_t() {
            state->pending_jobs.fetch_sub(1, std::memory_order_acq_rel);
            state->drain_cv.notify_all();
        }
        std::shared_ptr<AidaHexDocument> state;
    };
};

class AidaHexDocumentRegistry : public QObject {
    Q_OBJECT
public:
    explicit AidaHexDocumentRegistry(QObject* parent = nullptr);
    ~AidaHexDocumentRegistry() override;

    static AidaHexDocumentRegistry& instance();

    std::shared_ptr<AidaHexDocument> stateFor(
        const disasm_view::workspace_context_t& context);
    std::shared_ptr<AidaHexDocument> find(const aida::analysis::binary_id_t& id);
    void unregisterState(const aida::analysis::binary_id_t& id, const AidaHexDocument* state);
    void close(const disasm_view::workspace_context_t& context);

private:
    void sweepDispatchFailures();

    std::mutex registry_mutex;
    std::unordered_map<aida::analysis::binary_id_t, std::shared_ptr<AidaHexDocument>,
        aida::analysis::binary_id_hash_t> documents_;
    QTimer* sweep_timer_ = nullptr;
};

}
