#pragma once

#include <QHash>
#include <QStyledItemDelegate>

#include "qt/scanner/scanner_palette.hpp"

namespace aida::qt::scanner {

class ScanResultDelegate : public QStyledItemDelegate {
	Q_OBJECT
public:
	explicit ScanResultDelegate(QObject* parent = nullptr);

	void setPalette(const scanner_palette_t& palette);

	QSize sizeHint(const QStyleOptionViewItem& option,
		const QModelIndex& index) const override;
	void paint(QPainter* painter, const QStyleOptionViewItem& option,
		const QModelIndex& index) const override;

private:
	QString elided(const QFontMetricsF& metrics, const QFont& font,
		const QString& text, int width) const;

	scanner_palette_t palette_;
	mutable QHash<QString, QString> elide_cache_;
};

}
