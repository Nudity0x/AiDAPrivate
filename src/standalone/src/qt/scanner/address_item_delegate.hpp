#pragma once

#include <QHash>
#include <QStyledItemDelegate>

#include <functional>

#include "qt/scanner/scanner_palette.hpp"

namespace aida::qt::scanner {

class AddressItemDelegate : public QStyledItemDelegate {
	Q_OBJECT
public:
	explicit AddressItemDelegate(QObject* parent = nullptr);

	void setPalette(const scanner_palette_t& palette);
	void set_freeze_toggle_handler(std::function<void(int row)> handler);

	QSize sizeHint(const QStyleOptionViewItem& option,
		const QModelIndex& index) const override;
	void paint(QPainter* painter, const QStyleOptionViewItem& option,
		const QModelIndex& index) const override;
	bool editorEvent(QEvent* event, QAbstractItemModel* model,
		const QStyleOptionViewItem& option, const QModelIndex& index) override;

	static QRect switch_rect(const QRect& cell);

private:
	QString elided(const QFontMetricsF& metrics, const QFont& font,
		const QString& text, int width) const;

	scanner_palette_t palette_;
	std::function<void(int row)> freeze_toggle_handler_;
	mutable QHash<QString, QString> elide_cache_;
};

}
