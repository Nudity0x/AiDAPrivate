#pragma once

#include <QWidget>

namespace aida::qt::widgets {

class AidaButton;
class AidaProgressRing;

class AidaStateView : public QWidget
{
    Q_OBJECT
public:
    enum class State { Empty, Loading, Error, Success };

    explicit AidaStateView(QWidget* parent = nullptr);
    explicit AidaStateView(State state, const QString& title, const QString& message = QString(),
        QWidget* parent = nullptr);

    void setState(State state);
    State state() const { return state_; }

    void setTitle(const QString& title);
    QString title() const { return title_; }

    void setMessage(const QString& message);
    QString message() const { return message_; }

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
    qreal contentHeight() const;
    void layoutChildren();

    State state_ = State::Empty;
    QString title_;
    QString message_;
    AidaButton* action_button_ = nullptr;
    AidaProgressRing* ring_ = nullptr;
};

}
