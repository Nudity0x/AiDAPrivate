#pragma once

#include <QStyledItemDelegate>

#include <QColor>
#include <QFontMetricsF>
#include <QHash>

namespace aida::qt::explorer {

class AidaExplorerDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit AidaExplorerDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

    bool setContentWidth(int width);
    int contentWidth() const noexcept { return content_width_; }

    static int rowBaseWidth();
    static int estimatedRowWidth(int max_depth, int max_name_units, const QFontMetricsF& fm);

private:
    void paintFolderIcon(QPainter& painter, const QPointF& center, qreal scale,
                         const QColor& color, bool expanded) const;
    void paintFileIcon(QPainter& painter, const QPointF& center, qreal scale,
                       const QColor& color) const;
    void paintDisclosure(QPainter& painter, const QPointF& center, qreal scale,
                         const QColor& color, bool expanded) const;
    QString elidedName(const QString& name, qreal width, const QFont& font) const;

    mutable QHash<QString, QString> elide_cache_;
    int content_width_ = 0;
};

}
