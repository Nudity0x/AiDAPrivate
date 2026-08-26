#pragma once

#include <QAbstractButton>

class QVariantAnimation;

namespace aida::qt::widgets {

class AidaButton : public QAbstractButton
{
    Q_OBJECT
public:
    enum class Kind { Primary, Secondary, Ghost, Destructive, AccentGradient };
    enum class ControlSize { Small, Medium, Large };

    explicit AidaButton(QWidget* parent = nullptr);
    explicit AidaButton(const QString& text, QWidget* parent = nullptr);
    ~AidaButton() override;

    void setKind(Kind kind);
    Kind kind() const { return kind_; }

    void setControlSize(ControlSize size);
    ControlSize controlSize() const { return control_size_; }

    void setLoading(bool loading);
    bool isLoading() const { return loading_; }

    void triggerFlash();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

    QRectF faceRect() const;
    qreal hoverValue() const { return hover_; }
    qreal pressValue() const { return press_; }
    qreal flashValue() const { return flash_; }
    void animateHoverTo(qreal target);
    void animatePressTo(qreal target);
    void setContentIconHidden(bool hidden) { icon_hidden_ = hidden; }

private:
    void applySizeFont();
    void syncAccessibleName();
    qreal controlHeight() const;
    qreal paddingX() const;
    void paintShell(QPainter& p, const QRectF& face);
    void paintContent(QPainter& p, const QRectF& face);
    void startOrbit();
    void stopOrbit();

    Kind kind_ = Kind::Secondary;
    ControlSize control_size_ = ControlSize::Medium;
    bool loading_ = false;
    bool icon_hidden_ = false;
    QVariantAnimation* hover_anim_ = nullptr;
    QVariantAnimation* press_anim_ = nullptr;
    QVariantAnimation* flash_anim_ = nullptr;
    QVariantAnimation* orbit_anim_ = nullptr;
    qreal hover_ = 0.0;
    qreal press_ = 0.0;
    qreal flash_ = 0.0;
    qreal orbit_phase_ = 0.0;
};

}
