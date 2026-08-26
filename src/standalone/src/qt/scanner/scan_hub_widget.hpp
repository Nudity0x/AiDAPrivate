#pragma once

#include "qt/docking/hub_dock.hpp"

#include <QTabBar>

namespace aida::qt::scanner {

class ScanHubController;

class ScanHubWidget : public docking::AidaHubWidget {
	Q_OBJECT
public:
	explicit ScanHubWidget(registry::qt_view_registry_t* registry,
		QWidget* parent = nullptr);

	static void wire_hub(docking::AidaHubWidget* hub, ScanHubController* controller);
};

}
