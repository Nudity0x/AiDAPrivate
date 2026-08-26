#pragma once

#include <QWidget>

#include <optional>

#include "core/scanner/snapshot_diff.hpp"

namespace aida::qt::scanner {

class DiffDetailWidget : public QWidget {
	Q_OBJECT
public:
	explicit DiffDetailWidget(QWidget* parent = nullptr);

	void set_change(const snapshot_diff::changed_region_t& change);
	void clear_change();
	bool has_change() const noexcept { return change_.has_value(); }

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	std::optional<snapshot_diff::changed_region_t> change_;
	int content_height_ = 0;
};

}
