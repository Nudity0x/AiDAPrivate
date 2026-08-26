#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/scanner/aob_generator.hpp"

namespace aida::qt {
namespace docking {
class AidaDockHost;
}
}

namespace disasm_view {
struct workspace_context_t;
}

namespace aida::qt::scanner {

enum class aob_terminal_t : std::uint8_t {
	idle,
	queued,
	running,
	succeeded,
	failed,
	cancelled,
	stale
};

struct aob_operation_status_t {
	aob_terminal_t terminal = aob_terminal_t::idle;
	std::string message;
	std::string path;
};

struct aob_view_state_t {
	int selected_saved = -1;
	std::uint64_t context_address = 0;
	std::string context_name;
	int active_format = 0;
	std::mutex operation_mutex;
	aob_operation_status_t export_status;
	aob_operation_status_t catalog_status;
	aob_operation_status_t comparison_status;
	std::atomic<bool> export_pending{false};
	std::atomic<bool> catalog_pending{false};
	std::atomic<bool> comparison_pending{false};
	std::atomic<bool> export_dispatch_failed{false};
	std::atomic<bool> catalog_dispatch_failed{false};
	std::atomic<bool> comparison_dispatch_failed{false};
	std::atomic<std::uint64_t> export_serial{0};
	std::atomic<std::uint64_t> catalog_serial{0};
	std::atomic<std::uint64_t> comparison_serial{0};
	aob_generator::export_format_t last_export_format = aob_generator::export_format_t::json;
	std::string last_export_path;
	bool last_catalog_save = true;
	std::uint64_t render_catalog_generation = 0;
	std::vector<aob_generator::signature_t> render_catalog;
};

class AobController : public QObject {
	Q_OBJECT
public:
	static AobController& instance();

	void install(docking::AidaDockHost* host);
	docking::AidaDockHost* host() const noexcept { return host_; }

	std::shared_ptr<aob_view_state_t> view_state_for(
		const disasm_view::workspace_context_t& context);
	std::shared_ptr<aob_view_state_t> view_state_for_key(const std::string& binary_id_hex);

	void poll(const std::shared_ptr<aob_generator::state_t>& generator,
		const std::shared_ptr<aob_view_state_t>& state);

	void request_generate(const disasm_view::workspace_context_t& context);
	void request_regenerate(const disasm_view::workspace_context_t& context);
	void request_save_current(const disasm_view::workspace_context_t& context);
	void request_optimize(const disasm_view::workspace_context_t& context);

	void request_export(const disasm_view::workspace_context_t& context,
		aob_generator::export_format_t format, std::string requested_path);
	void request_catalog(const disasm_view::workspace_context_t& context, bool save);
	void request_comparison(const disasm_view::workspace_context_t& context);

	void select_saved(const std::shared_ptr<aob_view_state_t>& state, int index,
		std::uint64_t address, const std::string& name);

Q_SIGNALS:
	void stateChanged();

private:
	explicit AobController(QObject* parent = nullptr);

	void reconcile_dispatch_failures(const std::shared_ptr<aob_view_state_t>& state);

	docking::AidaDockHost* host_ = nullptr;
	std::mutex states_mutex_;
	std::unordered_map<std::string, std::shared_ptr<aob_view_state_t>> states_;
};

}
