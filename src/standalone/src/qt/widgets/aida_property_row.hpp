#pragma once

#include <QWidget>

#include "aida_paint_utils.hpp"

namespace aida::qt::widgets {

class AidaPropertyRow : public QWidget
{
    Q_OBJECT
public:
    explicit AidaPropertyRow(QWidget* parent = nullptr);
    explicit AidaPropertyRow(const QString& label, const QString& value, QWidget* parent = nullptr);

    void setLabel(const QString& label);
    QString label() const { return label_; }

    void setValue(const QString& value);
    QString value() const { return value_; }

    void setValueKind(AidaSemantic kind);
    AidaSemantic valueKind() const { return value_kind_; }

    void setSelectable(bool selectable);
    bool isSelectable() const { return selectable_; }

    void setSelected(bool selected);
    bool isSelected() const { return selected_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void syncAccessibleName();

    QString label_;
    QString value_;
    AidaSemantic value_kind_ = AidaSemantic::Neutral;
    bool selectable_ = false;
    bool selected_ = false;
    bool hovered_ = false;
};

}
