#pragma once

#include <QWidget>

#include <vector>

#include "core/scanner/aob_generator.hpp"

namespace aida::qt::scanner {

class AobByteGridWidget : public QWidget {
	Q_OBJECT
public:
	explicit AobByteGridWidget(QWidget* parent = nullptr);

	void set_bytes(const std::vector<aob_generator::aob_byte_t>& bytes);
	void clear();

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override;

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	int rows_for_width(int width) const;

	std::vector<aob_generator::aob_byte_t> bytes_;
	int cell_w_ = 0;
	int cell_h_ = 0;
	int row_stride_ = 0;
};

}
