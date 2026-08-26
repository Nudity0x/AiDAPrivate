#pragma once

#include <QWidget>

#include <QRectF>

#include <cstdint>

namespace aida::qt::debugger {

// 12-chip RFLAGS grid (CF/PF/AF/ZF/SF/OF/TF/IF/DF/NT/RF/AC, masks verbatim).
// Click toggles the bit by staging RFLAGS ^ mask into the register-edit dialog
// (the dialog applies through the mutation queue).
class RflagsChipsWidget : public QWidget {
    Q_OBJECT
public:
    explicit RflagsChipsWidget(QWidget* parent = nullptr);

    void setRflags(std::uint64_t rflags);
    std::uint64_t rflags() const noexcept { return rflags_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void flagToggleRequested(const QString& flagName, std::uint64_t currentRflags,
                             std::uint64_t toggledRflags);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    struct chip_t {
        const char* short_name;
        const char* full_name;
        std::uint64_t mask;
    };
    static const chip_t k_chips[12];

    int chipAt(const QPoint& pos) const;
    QRectF chipRect(int index) const;
    void toggleChip(int index);

    std::uint64_t rflags_ = 0;
    int hovered_chip_ = -1;
    int current_chip_ = 0;
};

}
