#include "qt/scanner/qt_scanner_domain.hpp"

#include "helpers/diag_log.hpp"

#include "qt/docking/dock_host.hpp"
#include "qt/registry/qt_view_registry.hpp"
#include "qt/scanner/aob_controller.hpp"
#include "qt/scanner/aob_widget.hpp"
#include "qt/scanner/crypto_controller.hpp"
#include "qt/scanner/crypto_widget.hpp"
#include "qt/scanner/pointer_controller.hpp"
#include "qt/scanner/pointer_widget.hpp"
#include "qt/scanner/scan_hub_controller.hpp"
#include "qt/scanner/scanner_controller.hpp"
#include "qt/scanner/snapshot_controller.hpp"
#include "qt/scanner/snapshot_widget.hpp"
#include "qt/scanner/value_scan_widget.hpp"

namespace aida::qt::scanner {

void install_scanner_domain(docking::AidaDockHost* host, bridge::MenuBridge* menus,
	bridge::ActionBridge* actions)
{
	if (!host)
		return;
	static_cast<void>(menus);
	ScannerController::instance().install(host, actions);
	AobController::instance().install(host);
	CryptoController::instance().install(host);
	PointerScanController::instance().install(host);
	SnapshotDiffController::instance().install(host);
	ScanHubController::instance().install(host);

	using registry::stable_view_id_t;
	const auto install = [host](const char* id, registry::qt_view_factory_t factory) {
		const auto result = host->install_view_factory(stable_view_id_t(id),
			std::move(factory));
		if (!result.ok())
			diag::log_tagged_fmt("qt_scanner",
				"view_factory_install_failed view=%s status=%d detail=%s",
				id, static_cast<int>(result.status), result.detail.c_str());
	};
	install("view.memory.value_scan", [](QWidget* parent,
			const registry::view_instance_id_t&) -> QWidget* {
		return new ValueScanWidget(parent);
	});
	install("view.memory.value_scan_results", [](QWidget* parent,
			const registry::view_instance_id_t&) -> QWidget* {
		return new ValueScanResultsDockWidget(parent);
	});
	install("view.memory.address_list", [](QWidget* parent,
			const registry::view_instance_id_t&) -> QWidget* {
		return new AddressListDockWidget(parent);
	});
	install("view.memory.crypto", [](QWidget* parent,
			const registry::view_instance_id_t&) -> QWidget* {
		return new CryptoScannerWidget(parent);
	});
	install("view.memory.aob", [](QWidget* parent,
			const registry::view_instance_id_t&) -> QWidget* {
		return new AobGeneratorWidget(parent);
	});
	install("view.memory.pointers", [](QWidget* parent,
			const registry::view_instance_id_t&) -> QWidget* {
		return new PointerScannerWidget(parent);
	});
	install("view.memory.snapshots", [](QWidget* parent,
			const registry::view_instance_id_t&) -> QWidget* {
		return new SnapshotDiffWidget(parent);
	});
	diag::log_tagged("qt_scanner", "scanner_domain_installed");
}

}
