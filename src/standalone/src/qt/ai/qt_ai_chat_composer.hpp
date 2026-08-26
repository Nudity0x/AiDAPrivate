#pragma once

#include <QPlainTextEdit>

namespace aida::qt::ai {

class AidaChatComposer : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit AidaChatComposer(QWidget* parent = nullptr);

    static constexpr int k_max_chars = 4096;

    void appendInjected(const QString& text);
    void clearComposer();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void submitRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void enforceCap();
};

}
