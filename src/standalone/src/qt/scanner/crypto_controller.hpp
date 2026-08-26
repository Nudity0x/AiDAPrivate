#pragma once

#include <QObject>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/scanner/crypto_workspace_scan.hpp"

namespace aida::qt {
namespace docking {
class AidaDockHost;
}
}

namespace disasm_view {
struct workspace_context_t;
}

namespace aida::qt::scanner {

class CryptoController : public QObject {
	Q_OBJECT
public:
	static CryptoController& instance();

	void install(docking::AidaDockHost* host);
	docking::AidaDockHost* host() const noexcept { return host_; }

	std::shared_ptr<crypto_workspace_scan::state_t> state_for(
		const disasm_view::workspace_context_t& context);

	void start_scan(const disasm_view::workspace_context_t& context,
		const std::shared_ptr<crypto_workspace_scan::state_t>& state,
		bool entropy_only);
	void cancel_scan(const std::shared_ptr<crypto_workspace_scan::state_t>& state);
	void export_results(const disasm_view::workspace_context_t& context,
		const std::shared_ptr<crypto_workspace_scan::state_t>& state,
		const std::shared_ptr<const crypto_scanner::workspace_scan_snapshot_t>& snapshot,
		std::string path, bool csv);
	void request_filter(const disasm_view::workspace_context_t& context,
		const std::shared_ptr<crypto_workspace_scan::state_t>& state,
		const std::shared_ptr<const crypto_scanner::workspace_scan_snapshot_t>& snapshot,
		std::string filter, int category, int sort_column, bool sort_ascending);

	void reconcile(const std::shared_ptr<crypto_workspace_scan::state_t>& state);

Q_SIGNALS:
	void stateChanged();

private:
	explicit CryptoController(QObject* parent = nullptr);

	docking::AidaDockHost* host_ = nullptr;
	std::mutex states_mutex_;
	std::unordered_map<std::string,
		std::shared_ptr<crypto_workspace_scan::state_t>> states_;
};

}
