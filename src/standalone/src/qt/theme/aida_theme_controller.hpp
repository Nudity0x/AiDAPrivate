#pragma once

#include <QObject>
#include <QRgb>
#include <QString>

class QApplication;
class QFileSystemWatcher;

namespace aida::qt::theme {

enum class Density {
    Compact,
    Comfortable
};

struct AidaCustomTheme {
    QString name = QStringLiteral("Custom Theme");
    float accent[3] = { 0.53f, 0.53f, 1.0f };
    QRgb bg_base = 0xEB04081E;
    QRgb panel_bg = 0xD216161C;
    QRgb panel_header = 0xE622222C;
    QRgb title_bar = 0xE6101016;
    QRgb text_primary = 0xF0E6E4FF;
    QRgb text_secondary = 0xC8AAAFBE;
    QRgb text_dim = 0x8C6E6991;
};

class AidaThemeController : public QObject {
    Q_OBJECT
public:
    static AidaThemeController& instance();

    bool installIntoApplication(QApplication& app);

    Density density() const;
    void setDensity(Density density);

    bool reducedMotion() const;
    void setReducedMotion(bool reduced);

    bool diagnosticsMode() const;
    void setDiagnosticsMode(bool enabled);

    void applySettings(int ui_density, bool ui_reduced_motion, bool ui_diagnostics_mode);

    void applyCustomTheme(const AidaCustomTheme& theme);
    void applyDefaultTheme();
    void applyForIndex(int theme_index, bool animate);

    quint64 generation() const;

Q_SIGNALS:
    void themeChanged();
    void themeGenerationChanged(quint64 generation);
    void diagnosticsModeChanged(bool enabled);

private:
    explicit AidaThemeController(QObject* parent = nullptr);

    void rebuildTokens();
    void applyColorsAndSheet();
    void applySheetOnly();
    void installLiveReload();

    Density density_ = Density::Compact;
    bool diagnostics_ = false;
    bool installed_ = false;
    bool custom_active_ = false;
    AidaCustomTheme custom_;
    quint64 generation_ = 0;
    QFileSystemWatcher* watcher_ = nullptr;
};

}
