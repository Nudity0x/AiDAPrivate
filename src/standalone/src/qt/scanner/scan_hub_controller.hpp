#pragma once

#include <QObject>

#include <atomic>

namespace aida::qt {
namespace docking {
class AidaDockHost;
class AidaHubWidget;
}
}

namespace aida::qt::scanner {

class ScanHubController : public QObject {
	Q_OBJECT
public:
	static ScanHubController& instance();

	void install(docking::AidaDockHost* host);
	docking::AidaDockHost* host() const noexcept { return host_; }

	int current_tab() const noexcept
		{ return current_tab_.load(std::memory_order_acquire); }
	void set_current_tab(int tab);
	void note_current_tab_from_widget(int tab);
	void note_page_shown();

	static constexpr int kTabCount = 7;
	static const char* tab_label(int tab) noexcept;
	static const char* tab_tooltip(int tab) noexcept;

Q_SIGNALS:
	void currentTabChanged(int tab);

private:
	explicit ScanHubController(QObject* parent = nullptr);

	void bind_hub_widget();

	docking::AidaDockHost* host_ = nullptr;
	std::atomic<int> current_tab_{0};
	bool hub_wired_ = false;
};

}
