#pragma once

#include <QPointer>
#include <QWidget>

class QKeyEvent;

namespace aida::qt::overlays {

class AidaOverlayHost : public QWidget {
    Q_OBJECT
public:
    explicit AidaOverlayHost(QWidget* parent = nullptr);
    ~AidaOverlayHost() override;

    void present(QWidget* content);
    void dismiss();
    bool active() const noexcept { return active_; }
    QWidget* content() const noexcept { return content_; }

    void setCancelable(bool cancelable);

Q_SIGNALS:
    void cancelRequested();
    void dismissed();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void syncGeometry();
    void installFilter();
    void removeFilter();

    QPointer<QWidget> content_;
    QPointer<QWidget> focus_before_;
    QWidget* filter_target_ = nullptr;
    bool active_ = false;
    bool cancelable_ = false;
};

}
