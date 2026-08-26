#pragma once

#include <QString>
#include <QWidget>

#include "qt/overlays/aida_empty_state.hpp"

namespace aida::qt::overlays {

struct AidaNoTargetConfig {
    AidaGlyph glyph = AidaGlyph::BinaryFile;
    QString title;
    QString subtitle;
    QString hint;
    QString stable_id;
};

class AidaNoTargetOverlay : public QWidget {
    Q_OBJECT
public:
    explicit AidaNoTargetOverlay(QWidget* parent = nullptr);
    AidaNoTargetOverlay(const AidaNoTargetConfig& config, QWidget* parent = nullptr);

    void setConfig(const AidaNoTargetConfig& config);
    void refreshCapabilities();

    static QString stateIdForGlyph(AidaGlyph glyph);

private:
    void rebuild();

    AidaNoTargetConfig config_;
    AidaEmptyState* state_ = nullptr;
};

}
