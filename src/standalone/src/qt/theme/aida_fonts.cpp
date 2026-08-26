#include "aida_fonts.hpp"

#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>

#include <cmath>

#include "aida_tokens.hpp"
#include "helpers/diag_log.hpp"

namespace aida::qt::theme::fonts {

namespace {

struct font_state_t {
    bool attempted = false;
    bool ok = false;
    QString ui_family;
    QString code_family;
    QStringList ui_families;
    QStringList code_families;
    mono_grid_t grid;
    bool grid_valid = false;
};

font_state_t s_state;

struct font_resource_t {
    const char* path;
    const char* role;
};

constexpr font_resource_t kResources[] = {
    { ":/fonts/Inter-Regular.otf", "ui" },
    { ":/fonts/Inter-Medium.otf", "ui" },
    { ":/fonts/Inter-SemiBold.otf", "ui" },
    { ":/fonts/Inter-Bold.otf", "ui" },
    { ":/fonts/JetBrainsMono-Regular.ttf", "code" },
    { ":/fonts/JetBrainsMono-SemiBold.ttf", "code" },
    { ":/fonts/verdana.ttf", "fallback" },
};

QString register_font(const font_resource_t& res)
{
    const int id = QFontDatabase::addApplicationFont(QString::fromLatin1(res.path));
    if (id == -1) {
        diag::log_tagged_fmt("qt_fonts", "font_load_failed path=%s role=%s", res.path, res.role);
        return QString();
    }
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    const QString family = families.isEmpty() ? QString() : families.first();
    diag::log_tagged_fmt("qt_fonts", "font_loaded path=%s role=%s id=%d family=%s",
        res.path, res.role, id, family.toUtf8().constData());
    return family;
}

void compute_mono_grid(font_state_t& state)
{
    mono_grid_t grid;
    const QFont mono = codeRegular();
    const QFontInfo info(mono);
    grid.fixed_pitch = info.fixedPitch();
    const QFontMetricsF fm(mono);
    grid.cell_w = fm.horizontalAdvance(u'M');
    grid.cell_h = fm.height();
    grid.line_h = std::floor(grid.cell_h * tokens().typography.code_line_height + 0.5);

    static const char kProbe[] = "MWiIl10_";
    bool uniform = true;
    const qreal reference = grid.cell_w;
    for (const char* p = kProbe; *p != '\0'; ++p) {
        const qreal advance = fm.horizontalAdvance(QChar::fromLatin1(*p));
        if (std::fabs(advance - reference) > 0.01) {
            uniform = false;
            break;
        }
    }
    grid.uniform_advance = uniform;
    grid.valid = grid.fixed_pitch && grid.uniform_advance;
    state.grid = grid;
    state.grid_valid = true;

    diag::log_tagged_fmt("qt_fonts",
        "mono_grid family=%s resolved=%s cell_w=%.2f cell_h=%.2f line_h=%.1f fixed_pitch=%d uniform=%d",
        state.code_family.toUtf8().constData(),
        info.family().toUtf8().constData(),
        grid.cell_w, grid.cell_h, grid.line_h,
        grid.fixed_pitch ? 1 : 0, grid.uniform_advance ? 1 : 0);
    if (!grid.fixed_pitch || !grid.uniform_advance)
        diag::log_tagged_fmt("qt_fonts", "mono_grid_selfcheck_failed fixed_pitch=%d uniform=%d",
            grid.fixed_pitch ? 1 : 0, grid.uniform_advance ? 1 : 0);
}

QFont make_font(const QStringList& families, int weight, int pixelSize, bool no_hinting)
{
    QFont font;
    font.setFamilies(families);
    font.setWeight(static_cast<QFont::Weight>(weight));
    font.setPixelSize(pixelSize);
    if (no_hinting)
        font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

}

bool load()
{
    font_state_t& state = s_state;
    if (state.attempted)
        return state.ok;
    state.attempted = true;

    QString inter_family;
    QString jetbrains_family;
    QString verdana_family;
    bool all_ok = true;
    for (const font_resource_t& res : kResources) {
        const QString family = register_font(res);
        if (family.isEmpty()) {
            all_ok = false;
            continue;
        }
        if (res.role[0] == 'u' && inter_family.isEmpty())
            inter_family = family;
        else if (res.role[0] == 'c' && jetbrains_family.isEmpty())
            jetbrains_family = family;
        else if (res.role[0] == 'f' && verdana_family.isEmpty())
            verdana_family = family;
    }

    state.ui_family = !inter_family.isEmpty() ? inter_family
        : (!verdana_family.isEmpty() ? verdana_family : QStringLiteral("Verdana"));
    state.code_family = !jetbrains_family.isEmpty() ? jetbrains_family : QStringLiteral("Cascadia Mono");

    state.ui_families = { state.ui_family, QStringLiteral("Verdana"), QStringLiteral("Segoe UI"),
        QStringLiteral("sans-serif") };
    state.code_families = { state.code_family, QStringLiteral("Cascadia Mono"),
        QStringLiteral("Consolas"), QStringLiteral("monospace") };

    state.ok = all_ok && !inter_family.isEmpty() && !jetbrains_family.isEmpty();
    diag::log_tagged_fmt("qt_fonts", "fonts_load_complete ok=%d ui_family=%s code_family=%s",
        state.ok ? 1 : 0,
        state.ui_family.toUtf8().constData(),
        state.code_family.toUtf8().constData());

    compute_mono_grid(state);
    return state.ok;
}

bool loaded()
{
    return s_state.attempted && s_state.ok;
}

QString uiFamily()
{
    return s_state.ui_family.isEmpty() ? QStringLiteral("Verdana") : s_state.ui_family;
}

QString codeFamily()
{
    return s_state.code_family.isEmpty() ? QStringLiteral("Consolas") : s_state.code_family;
}

QStringList uiFamilies()
{
    if (s_state.ui_families.isEmpty())
        return { QStringLiteral("Verdana"), QStringLiteral("Segoe UI"), QStringLiteral("sans-serif") };
    return s_state.ui_families;
}

QStringList codeFamilies()
{
    if (s_state.code_families.isEmpty())
        return { QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), QStringLiteral("monospace") };
    return s_state.code_families;
}

QFont ui(int weight, int pixelSize)
{
    return make_font(uiFamilies(), weight, pixelSize, pixelSize <= 14);
}

QFont code(int weight, int pixelSize)
{
    return make_font(codeFamilies(), weight, pixelSize, true);
}

QFont body() { return ui(400, 14); }
QFont bodyEm() { return ui(500, 14); }
QFont strong() { return ui(600, 14); }
QFont h1() { return ui(700, 16); }
QFont h2() { return ui(600, 15); }
QFont large() { return ui(400, 16); }
QFont caption() { return ui(500, 12); }
QFont display() { return ui(700, 22); }
QFont codeRegular() { return code(400, 13); }
QFont codeEm() { return code(600, 13); }
QFont codeLarge() { return code(400, 15); }

const mono_grid_t& monoGrid()
{
    if (!s_state.grid_valid)
        compute_mono_grid(s_state);
    return s_state.grid;
}

void invalidateMonoGrid()
{
    s_state.grid_valid = false;
}

}
