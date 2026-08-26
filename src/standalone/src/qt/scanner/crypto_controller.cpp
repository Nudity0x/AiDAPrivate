#include "qt/scanner/crypto_controller.hpp"

#include "core/disasm/disasm_view.hpp"
#include "helpers/diag_log.hpp"

#include "qt/docking/dock_host.hpp"

namespace aida::qt::scanner {

CryptoController& CryptoController::instance()
{
	static CryptoController* controller = new CryptoController();
	return *controller;
}

CryptoController::CryptoController(QObject* parent) : QObject(parent) {}

void CryptoController::install(docking::AidaDockHost* host)
{
	host_ = host;
	crypto_workspace_scan::set_focus_view([host](const std::string& stable_view_id) {
		if (host)
			static_cast<void>(host->open_or_focus(
				registry::stable_view_id_t(stable_view_id)));
	});
}

std::shared_ptr<crypto_workspace_scan::state_t> CryptoController::state_for(
	const disasm_view::workspace_context_t& context)
{
	if (!context.workspace)
		return {};
	const std::string key = context.workspace->identity().binary_id().to_hex();
	std::lock_guard<std::mutex> lock(states_mutex_);
	auto& state = states_[key];
	if (!state)
		state = std::make_shared<crypto_workspace_scan::state_t>();
	return state;
}

void CryptoController::start_scan(const disasm_view::workspace_context_t& context,
	const std::shared_ptr<crypto_workspace_scan::state_t>& state, bool entropy_only)
{
	crypto_workspace_scan::detail::start_workspace_scan(context, state, entropy_only);
	Q_EMIT stateChanged();
}

void CryptoController::cancel_scan(
	const std::shared_ptr<crypto_workspace_scan::state_t>& state)
{
	if (!state)
		return;
	state->cancellation_requested.store(true, std::memory_order_release);
	diag::log_tagged("scan_audit", "[scan_audit] crypto_scanner cancel");
}

void CryptoController::export_results(const disasm_view::workspace_context_t& context,
	const std::shared_ptr<crypto_workspace_scan::state_t>& state,
	const std::shared_ptr<const crypto_scanner::workspace_scan_snapshot_t>& snapshot,
	std::string path, bool csv)
{
	crypto_workspace_scan::detail::export_results(context, state, snapshot,
		std::move(path), csv);
	Q_EMIT stateChanged();
}

void CryptoController::request_filter(
	const disasm_view::workspace_context_t& context,
	const std::shared_ptr<crypto_workspace_scan::state_t>& state,
	const std::shared_ptr<const crypto_scanner::workspace_scan_snapshot_t>& snapshot,
	std::string filter, int category, int sort_column, bool sort_ascending)
{
	crypto_workspace_scan::detail::request_filtered_results(context, state, snapshot,
		std::move(filter), category, sort_column, sort_ascending);
}

void CryptoController::reconcile(
	const std::shared_ptr<crypto_workspace_scan::state_t>& state)
{
	if (!state)
		return;
	if (state->export_dispatch_failed.exchange(false, std::memory_order_acq_rel)) {
		crypto_workspace_scan::detail::set_export_status(state,
			crypto_workspace_scan::export_terminal_t::failed,
			"UI dispatcher rejected crypto export completion");
		Q_EMIT stateChanged();
	}
}

}
