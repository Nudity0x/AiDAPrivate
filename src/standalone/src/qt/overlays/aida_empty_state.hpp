#pragma once

#include <QString>
#include <QWidget>

#include <string>
#include <vector>

class QVariantAnimation;

namespace aida::qt::overlays {

enum class AidaGlyph {
    Dots,
    BinaryFile,
    Memory,
    Network,
    Shield,
    Key,
    Search,
    Flask,
    Layers,
    Cpu,
    Bug,
    Flow,
    Message,
    Spark
};

struct AidaEmptyStateAction {
    QString id;
    QString label;
    int kind = 1;
    bool disabled = false;
    QString tooltip;
};

struct AidaEmptyStateConfig {
    AidaGlyph glyph = AidaGlyph::Dots;
    QString title;
    QString body;
    QString footer;
    QStringList kbd_hints;
    std::vector<AidaEmptyStateAction> actions;
    int max_width = 0;
};

class AidaEmptyGlyph : public QWidget {
    Q_OBJECT
public:
    explicit AidaEmptyGlyph(QWidget* parent = nullptr);
    AidaEmptyGlyph(AidaGlyph glyph, QWidget* parent = nullptr);
    ~AidaEmptyGlyph() override;

    void setGlyph(AidaGlyph glyph);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    AidaGlyph glyph_ = AidaGlyph::Dots;
    qreal clock_ = 0.0;
    QVariantAnimation* ticker_ = nullptr;
};

class AidaEmptyState : public QWidget {
    Q_OBJECT
public:
    explicit AidaEmptyState(QWidget* parent = nullptr);
    explicit AidaEmptyState(const AidaEmptyStateConfig& config, QWidget* parent = nullptr);

    void setConfig(const AidaEmptyStateConfig& config);
    const AidaEmptyStateConfig& config() const noexcept { return config_; }

Q_SIGNALS:
    void actionTriggered(const QString& id);

private:
    void rebuild();

    AidaEmptyStateConfig config_;
};

void paintGlyph(QPainter& painter, AidaGlyph glyph, const QPointF& center, qreal size,
                const QColor& color, qreal alpha, qreal clock_seconds);

}
