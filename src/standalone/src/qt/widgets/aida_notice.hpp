#pragma once

#include <QFrame>

#include "aida_paint_utils.hpp"

namespace aida::qt::widgets {

class AidaButton;

class AidaNotice : public QFrame
{
    Q_OBJECT
public:
    explicit AidaNotice(QWidget* parent = nullptr);
    explicit AidaNotice(const QString& title, const QString& message = QString(),
        AidaSemantic kind = AidaSemantic::Info, QWidget* parent = nullptr);

    void setTitle(const QString& title);
    QString title() const { return title_; }

    void setMessage(const QString& message);
    QString message() const { return message_; }

    void setKind(AidaSemantic kind);
    AidaSemantic kind() const { return kind_; }

    void setActionLabel(const QString& label);
    QString actionLabel() const;
    AidaButton* actionButton() const { return action_button_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void actionTriggered();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    qreal noticeHeight() const;
    void layoutAction();

    QString title_;
    QString message_;
    AidaSemantic kind_ = AidaSemantic::Info;
    AidaButton* action_button_ = nullptr;
};

}
