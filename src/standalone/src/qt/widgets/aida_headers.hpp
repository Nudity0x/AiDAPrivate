#pragma once

#include <QWidget>

#include "aida_paint_utils.hpp"

namespace aida::qt::widgets {

class AidaButton;

class AidaSectionHeader : public QWidget
{
    Q_OBJECT
public:
    explicit AidaSectionHeader(QWidget* parent = nullptr);
    explicit AidaSectionHeader(const QString& title, QWidget* parent = nullptr);

    void setTitle(const QString& title);
    QString title() const { return title_; }

    void setCountLabel(const QString& count);
    QString countLabel() const { return count_label_; }

    void setActionLabel(const QString& action);
    QString actionLabel() const { return action_label_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void actionTriggered();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    QRectF actionRect() const;
    qreal rowHeight() const;

    QString title_;
    QString count_label_;
    QString action_label_;
    bool action_hot_ = false;
    bool action_armed_ = false;
};

class AidaViewHeader : public QWidget
{
    Q_OBJECT
public:
    explicit AidaViewHeader(QWidget* parent = nullptr);
    explicit AidaViewHeader(const QString& title, const QString& subtitle = QString(),
        QWidget* parent = nullptr);

    void setTitle(const QString& title);
    QString title() const { return title_; }

    void setSubtitle(const QString& subtitle);
    QString subtitle() const { return subtitle_; }

    void setStatus(AidaSemantic status);
    AidaSemantic status() const { return status_; }

    void setPrimaryAction(const QString& label);
    void setSecondaryAction(const QString& label);
    AidaButton* primaryButton() const { return primary_button_; }
    AidaButton* secondaryButton() const { return secondary_button_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void primaryActionTriggered();
    void secondaryActionTriggered();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    qreal headerHeight() const;
    void layoutButtons();

    QString title_;
    QString subtitle_;
    AidaSemantic status_ = AidaSemantic::Neutral;
    AidaButton* primary_button_ = nullptr;
    AidaButton* secondary_button_ = nullptr;
};

}
