#include "qt/scanner/scan_hub_controller.hpp"

#include "helpers/diag_log.hpp"

#include "qt/analysis/qt_gui_post.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/docking/hub_dock.hpp"
#include "qt/scanner/scan_hub_widget.hpp"

namespace aida::qt::scanner {

ScanHubController& ScanHubController::instance()
{
	static ScanHubController* controller = new ScanHubController();
	return *controller;
}

ScanHubController::ScanHubController(QObject* parent) : QObject(parent) {}

void ScanHubController::install(docking::AidaDockHost* host)
{
	host_ = host;
	hub_wired_ = false;
	analysis::gui_post(this, [this] { bind_hub_widget(); });
}

const char* ScanHubController::tab_label(int tab) noexcept
{
	static constexpr const char* labels[kTabCount] = {
		"Value Scan", "Crypto", "AOB", "Decrypt", "Pointers", "Snapshots",
		"Integrity"
	};
	return tab >= 0 && tab < kTabCount ? labels[tab] : "";
}

const char* ScanHubController::tab_tooltip(int tab) noexcept
{
	static constexpr const char* tooltips[kTabCount] = {
		"scan values in memory", "crypto constant hunter",
		"array-of-bytes pattern", "decrypt oracle", "pointer scanner",
		"memory snapshot diff", "integrity hunter"
	};
	return tab >= 0 && tab < kTabCount ? tooltips[tab] : "";
}

void ScanHubController::set_current_tab(int tab)
{
	if (tab < 0 || tab >= kTabCount)
		return;
	current_tab_.store(tab, std::memory_order_release);
	analysis::gui_post(this, [this, tab] {
		bind_hub_widget();
		if (host_) {
			if (auto* hub = host_->hub_widget(registry::hub_kind_t::scan))
				if (hub->current_subview() != tab)
					hub->set_subview(tab);
		}
		Q_EMIT currentTabChanged(tab);
	});
}

void ScanHubController::note_current_tab_from_widget(int tab)
{
	if (tab < 0 || tab >= kTabCount)
		return;
	current_tab_.store(tab, std::memory_order_release);
}

void ScanHubController::note_page_shown()
{
	bind_hub_widget();
}

void ScanHubController::bind_hub_widget()
{
	if (!host_ || hub_wired_)
		return;
	auto* hub = host_->hub_widget(registry::hub_kind_t::scan);
	if (!hub)
		return;
	hub_wired_ = true;
	ScanHubWidget::wire_hub(hub, this);
	const int current = hub->current_subview();
	if (current >= 0 && current != current_tab())
		set_current_tab(current);
}

}
