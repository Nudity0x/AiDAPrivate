#pragma once

#include <QObject>

#include <cstdint>
#include <memory>

#include "core/scanner/snapshot_diff.hpp"

namespace aida::qt {
namespace docking {
class AidaDockHost;
}
}

namespace aida::qt::scanner {

class SnapshotDiffController : public QObject {
	Q_OBJECT
public:
	static SnapshotDiffController& instance();

	void install(docking::AidaDockHost* host);
	docking::AidaDockHost* host() const noexcept { return host_; }

	void take_snapshot();
	void compare(std::uint64_t id_a, std::uint64_t id_b);
	void clear();
	void load(const std::string& path);

	int selected_change() const noexcept { return selected_change_; }
	void set_selected_change(int row);

	std::shared_ptr<const snapshot_diff::diff_result_t> published_diff() const;
	std::size_t snapshot_count() const;

Q_SIGNALS:
	void stateChanged();

private:
	explicit SnapshotDiffController(QObject* parent = nullptr);

	docking::AidaDockHost* host_ = nullptr;
	int selected_change_ = -1;
};

}
