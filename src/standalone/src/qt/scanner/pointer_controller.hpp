#pragma once

#include <QObject>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/scanner/pointer_scanner.hpp"

namespace aida::qt {
namespace docking {
class AidaDockHost;
}
}

namespace disasm_view {
struct workspace_context_t;
}

namespace aida::qt::scanner {

enum class resolution_status_t : std::uint8_t {
	idle,
	queued,
	running,
	ready,
	failed,
	cancelled,
	stale
};

struct resolution_entry_t {
	resolution_status_t status = resolution_status_t::idle;
	std::vector<std::uint64_t> addresses;
	std::string error;
	std::uint32_t pid = 0;
	std::uint64_t workspace_generation = 0;
	std::uint64_t map_generation = 0;
	std::uint64_t result_generation = 0;
	std::uint64_t serial = 0;
	std::uint64_t touch = 0;
	std::shared_ptr<std::atomic<bool>> cancellation;
};

class PointerScanController : public QObject {
	Q_OBJECT
public:
	static PointerScanController& instance();

	void install(docking::AidaDockHost* host);
	docking::AidaDockHost* host() const noexcept { return host_; }

	void observe_generations();
	std::uint64_t map_generation() const noexcept { return map_generation_; }
	std::uint64_t result_generation() const noexcept { return result_generation_; }

	bool request_chain_resolution(const disasm_view::workspace_context_t& context,
		const pointer_scanner::pointer_chain_t& chain);
	std::optional<std::uint64_t> resolved_step_address(
		const pointer_scanner::pointer_chain_t& chain, int step,
		resolution_status_t* status = nullptr, std::string* error = nullptr);

	static std::optional<std::uint64_t> chain_base_address(
		const pointer_scanner::pointer_chain_t& chain);
	static std::string chain_identity_key(
		const pointer_scanner::pointer_chain_t& chain);
	static std::string cached_module_name(int module_index);
	static std::uint32_t attached_target_pid();
	static std::string format_offset(std::int64_t offset);

Q_SIGNALS:
	void stateChanged();

private:
	explicit PointerScanController(QObject* parent = nullptr);

	struct resolution_store_t {
		std::mutex mutex;
		std::unordered_map<std::string, resolution_entry_t> entries;
		std::atomic<std::uint64_t> serial{0};
		std::atomic<std::uint64_t> touch{0};
		std::atomic<std::uint64_t> current_map_generation{1};
		std::atomic<std::uint64_t> current_result_generation{1};
	};

	void prune_resolution_cache_locked();

	docking::AidaDockHost* host_ = nullptr;
	resolution_store_t resolutions_;
	std::uint64_t map_fingerprint_ = 0;
	std::uint64_t result_fingerprint_ = 0;
	std::uint64_t map_generation_ = 1;
	std::uint64_t result_generation_ = 1;
	bool observed_map_building_ = false;
	bool observed_scanning_ = false;
};

}
