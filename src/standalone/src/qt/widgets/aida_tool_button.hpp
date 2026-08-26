#pragma once

#include <QToolButton>

namespace aida::qt::widgets {

class AidaToolButton : public QToolButton
{
    Q_OBJECT
public:
    explicit AidaToolButton(QWidget* parent = nullptr);
    explicit AidaToolButton(const QIcon& icon, const QString& toolTip, QWidget* parent = nullptr);

    void setActive(bool active);
    bool isActive() const;

    QSize sizeHint() const override;

protected:
    bool event(QEvent* event) override;

private:
    void syncAccessibleName();
};

}
