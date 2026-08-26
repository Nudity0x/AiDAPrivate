#include "aida_theme_controller.hpp"

#include <QApplication>
#include <QFile>
#include <QFileSystemWatcher>
#include <QStyleFactory>

#include "aida_fonts.hpp"
#include "aida_icons.hpp"
#include "aida_motion.hpp"
#include "aida_palette.hpp"
#include "aida_proxy_style.hpp"
#include "aida_stylesheet.hpp"
#include "aida_tokens.hpp"
#include "helpers/diag_log.hpp"

namespace aida::qt::theme {

namespace {

int channel(float v)
{
    const int c = static_cast<int>(v * 255.f);
    return c < 0 ? 0 : (c > 255 ? 255 : c);
}

}

AidaThemeController& AidaThemeController::instance()
{
    static AidaThemeController* controller = [] {
        AidaThemeController* created = new AidaThemeController(qApp);
        return created;
    }();
    return *controller;
}

AidaThemeController::AidaThemeController(QObject* parent)
    : QObject(parent)
{
}

bool AidaThemeController::installIntoApplication(QApplication& app)
{
    diag::log_tagged_fmt("qt_theme", "install_begin installed=%d", installed_ ? 1 : 0);

    AidaProxyStyle* style = new AidaProxyStyle(QStyleFactory::create(QStringLiteral("fusion")));
    app.setStyle(style);

    const bool fonts_ok = fonts::load();
    if (!fonts_ok)
        diag::log_tagged_fmt("qt_theme", "install_fonts_degraded=1");

    rebuildTokens();

    app.setPalette(build_dark_palette(tokens()));

    QApplication::setFont(fonts::body());

    stylesheet::applyToApplication(tokens());

    installLiveReload();

    installed_ = true;
    ++generation_;
    diag::log_tagged_fmt("qt_theme", "install_complete generation=%llu fonts_ok=%d live_dir=%d",
        static_cast<unsigned long long>(generation_), fonts_ok ? 1 : 0,
        stylesheet::liveDirectory().isEmpty() ? 0 : 1);
    Q_EMIT themeChanged();
    Q_EMIT themeGenerationChanged(generation_);
    return fonts_ok;
}

Density AidaThemeController::density() const
{
    return density_;
}

void AidaThemeController::setDensity(Density density)
{
    if (density_ == density)
        return;
    density_ = density;
    rebuildTokens();
    applySheetOnly();
    diag::log_tagged_fmt("qt_theme", "density_changed comfortable=%d generation=%llu",
        density_ == Density::Comfortable ? 1 : 0,
        static_cast<unsigned long long>(generation_));
}

bool AidaThemeController::reducedMotion() const
{
    return AidaMotion::reducedMotion();
}

void AidaThemeController::setReducedMotion(bool reduced)
{
    if (AidaMotion::reducedMotion() == reduced)
        return;
    AidaMotion::setReducedMotion(reduced);
    ++generation_;
    Q_EMIT themeChanged();
    Q_EMIT themeGenerationChanged(generation_);
}

bool AidaThemeController::diagnosticsMode() const
{
    return diagnostics_;
}

void AidaThemeController::setDiagnosticsMode(bool enabled)
{
    if (diagnostics_ == enabled)
        return;
    diagnostics_ = enabled;
    Q_EMIT diagnosticsModeChanged(enabled);
}

void AidaThemeController::applySettings(int ui_density, bool ui_reduced_motion, bool ui_diagnostics_mode)
{
    setDensity(ui_density == 1 ? Density::Comfortable : Density::Compact);
    setReducedMotion(ui_reduced_motion);
    setDiagnosticsMode(ui_diagnostics_mode);
}

void AidaThemeController::applyCustomTheme(const AidaCustomTheme& theme)
{
    custom_ = theme;
    custom_active_ = true;
    rebuildTokens();
    applyColorsAndSheet();
    diag::log_tagged_fmt("qt_theme", "custom_theme_applied name=%s generation=%llu",
        theme.name.toUtf8().constData(), static_cast<unsigned long long>(generation_));
}

void AidaThemeController::applyDefaultTheme()
{
    custom_active_ = false;
    rebuildTokens();
    applyColorsAndSheet();
    diag::log_tagged_fmt("qt_theme", "default_theme_applied generation=%llu",
        static_cast<unsigned long long>(generation_));
}

void AidaThemeController::applyForIndex(int theme_index, bool animate)
{
    Q_UNUSED(theme_index);
    Q_UNUSED(animate);
    applyDefaultTheme();
}

quint64 AidaThemeController::generation() const
{
    return generation_;
}

void AidaThemeController::rebuildTokens()
{
    tokens_t next = default_tokens();

    if (custom_active_) {
        const int ar = channel(custom_.accent[0]);
        const int ag = channel(custom_.accent[1]);
        const int ab = channel(custom_.accent[2]);

        next.accent = QColor(ar, ag, ab);
        next.accent_u32 = qRgba(ar, ag, ab, 255);
        next.accent_hover = QColor((std::min)(ar + 24, 255), (std::min)(ag + 24, 255),
            (std::min)(ab + 24, 255));
        next.accent_dim = QColor(ar, ag, ab, 130);
        next.accent_glow = QColor(ar, ag, ab, 50);
        next.accent_grad_top = QColor((std::min)(ar + 18, 255), (std::min)(ag + 14, 255),
            (std::min)(ab + 14, 255));
        next.accent_grad_bot = QColor((std::max)(ar - 22, 0), (std::max)(ag - 22, 0),
            (std::max)(ab - 22, 0));
        next.border_focus = QColor(ar, ag, ab, 210);
        next.selection = QColor(ar, ag, ab, 70);
        next.selection_strong = QColor(ar, ag, ab, 130);
        next.hover_wash = QColor(ar, ag, ab, 204);
        next.accent_fill = QColor(ar, ag, ab, 56);
        next.accent_edge = QColor(ar, ag, ab, 140);
        next.accent_line = QColor(ar, ag, ab, 52);

        next.bg_base = QColor::fromRgba(custom_.bg_base);
        next.panel_bg = QColor::fromRgba(custom_.panel_bg);
        next.panel_header = QColor::fromRgba(custom_.panel_header);
        next.title_bar = QColor::fromRgba(custom_.title_bar);
        next.text_primary = QColor::fromRgba(custom_.text_primary);
        next.text_secondary = QColor::fromRgba(custom_.text_secondary);
        next.text_dim = QColor::fromRgba(custom_.text_dim);
    }

    apply_density_geometry(next, density_ == Density::Comfortable);
    tokens() = next;
}

void AidaThemeController::applyColorsAndSheet()
{
    if (!qApp)
        return;
    qApp->setPalette(build_dark_palette(tokens()));
    icons::clearCache();
    stylesheet::applyToApplication(tokens());
    ++generation_;
    Q_EMIT themeChanged();
    Q_EMIT themeGenerationChanged(generation_);
}

void AidaThemeController::applySheetOnly()
{
    if (!qApp)
        return;
    stylesheet::applyToApplication(tokens());
    ++generation_;
    Q_EMIT themeChanged();
    Q_EMIT themeGenerationChanged(generation_);
}

void AidaThemeController::installLiveReload()
{
    const QString live = stylesheet::liveDirectory();
    if (live.isEmpty() || watcher_)
        return;

    const QString templatePath = live + QStringLiteral("/aida_dark.qss");
    if (!QFile::exists(templatePath)) {
        diag::log_tagged_fmt("qt_theme", "qss_live_watch_skipped missing=%s",
            templatePath.toUtf8().constData());
        return;
    }

    watcher_ = new QFileSystemWatcher(this);
    watcher_->addPath(live);
    watcher_->addPath(templatePath);

    connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        if (watcher_ && !watcher_->files().contains(path) && QFile::exists(path))
            watcher_->addPath(path);
        stylesheet::clearCaches();
        applySheetOnly();
        diag::log_tagged_fmt("qt_theme", "qss_live_reloaded path=%s generation=%llu",
            path.toUtf8().constData(), static_cast<unsigned long long>(generation_));
    });
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this, templatePath](const QString&) {
        if (watcher_ && !watcher_->files().contains(templatePath) && QFile::exists(templatePath))
            watcher_->addPath(templatePath);
    });

    diag::log_tagged_fmt("qt_theme", "qss_live_watch_installed dir=%s", live.toUtf8().constData());
}

}
