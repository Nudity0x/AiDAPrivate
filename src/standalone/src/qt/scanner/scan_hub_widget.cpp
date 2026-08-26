#include "qt/scanner/scan_hub_widget.hpp"

#include "qt/scanner/scan_hub_controller.hpp"

namespace aida::qt::scanner {

ScanHubWidget::ScanHubWidget(registry::qt_view_registry_t* registry,
	QWidget* parent)
	: docking::AidaHubWidget(registry::hub_kind_t::scan, registry, parent)
{
	setObjectName(QStringLiteral("aida.hub.scan.widget"));
	wire_hub(this, &ScanHubController::instance());
}

void ScanHubWidget::wire_hub(docking::AidaHubWidget* hub,
	ScanHubController* controller)
{
	if (!hub || !controller)
		return;
	if (auto* tab_bar = hub->findChild<QTabBar*>()) {
		for (int index = 0; index < tab_bar->count() &&
			index < ScanHubController::kTabCount; ++index)
			tab_bar->setTabToolTip(index, QString::fromLatin1(
				ScanHubController::tab_tooltip(index)));
	}
	QObject::connect(hub, &docking::AidaHubWidget::subviewActivated, controller,
		[](int subview) {
			ScanHubController::instance().note_current_tab_from_widget(subview);
		});
}

}
