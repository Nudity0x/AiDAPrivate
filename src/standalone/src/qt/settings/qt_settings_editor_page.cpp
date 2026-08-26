#include "qt/settings/qt_settings_editor_page.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

#include "core/settings/settings_persistence_service.hpp"
#include "core/settings/standalone_settings.hpp"
#include "helpers/globals.h"
#include "qt/chrome/aida_theme_catalog.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_theme_controller.hpp"
#include "qt/theme/aida_tokens.hpp"

extern settings_sa_t g_sa_settings;

namespace aida::qt::settings {

void AidaSettingsEditorPage::buildUi() {
    const auto& t = theme::tokens();
    setObjectName(QStringLiteral("aida.view.settings.editorPage"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.spacing.md, t.spacing.sm, t.spacing.md, t.spacing.sm);
    root->setSpacing(t.spacing.sm);

    auto* title = new QLabel(QStringLiteral("Editor & Appearance"), this);
    title->setObjectName(QStringLiteral("aida.view.settings.editor.title"));
    title->setFont(theme::fonts::h1());
    auto* subtitle = new QLabel(QStringLiteral(
        "Tune readability and visual behavior across code and disassembly views."), this);
    subtitle->setObjectName(QStringLiteral("aida.view.settings.editor.subtitle"));
    subtitle->setFont(theme::fonts::caption());
    subtitle->setProperty("aidaVariant", "secondary");
    subtitle->setWordWrap(true);
    root->addWidget(title);
    root->addWidget(subtitle);

    auto* code_group = new QGroupBox(QStringLiteral("Code editor"), this);
    auto* code_form = new QFormLayout(code_group);
    code_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    code_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    code_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    tab_size_spin_ = new QSpinBox(code_group);
    tab_size_spin_->setObjectName(QStringLiteral("aida.view.settings.editor.tabSize"));
    tab_size_spin_->setRange(1, 16);
    tab_size_spin_->setToolTip(QStringLiteral("Spaces inserted per indentation level"));
    code_form->addRow(QStringLiteral("Tab size"), tab_size_spin_);
    font_size_spin_ = new QDoubleSpinBox(code_group);
    font_size_spin_->setObjectName(QStringLiteral("aida.view.settings.editor.fontSize"));
    font_size_spin_->setRange(9.0, 32.0);
    font_size_spin_->setSingleStep(0.5);
    font_size_spin_->setSuffix(QStringLiteral(" px"));
    font_size_spin_->setToolTip(QStringLiteral("Editor glyph size in points"));
    code_form->addRow(QStringLiteral("Font size"), font_size_spin_);
    root->addWidget(code_group);

    auto* disasm_group = new QGroupBox(QStringLiteral("Disassembly"), this);
    auto* disasm_layout = new QVBoxLayout(disasm_group);
    disasm_layout->setSpacing(t.spacing.sm);
    auto* disasm_hint = new QLabel(QStringLiteral(
        "Control how instruction selection is emphasized while navigating a binary."),
        disasm_group);
    disasm_hint->setObjectName(QStringLiteral("aida.view.settings.editor.disasmHint"));
    disasm_hint->setWordWrap(true);
    disasm_hint->setFont(theme::fonts::caption());
    disasm_hint->setProperty("aidaVariant", "secondary");
    disasm_layout->addWidget(disasm_hint);
    full_line_select_check_ = new QCheckBox(
        QStringLiteral("Highlight the full instruction row"), disasm_group);
    full_line_select_check_->setObjectName(
        QStringLiteral("aida.view.settings.editor.fullLineSelect"));
    full_line_select_check_->setToolTip(QStringLiteral(
        "In-memory only for this session; not persisted to settings"));
    disasm_layout->addWidget(full_line_select_check_);
    root->addWidget(disasm_group);

    auto* appearance_group = new QGroupBox(QStringLiteral("Appearance"), this);
    auto* appearance_form = new QFormLayout(appearance_group);
    appearance_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    appearance_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    appearance_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    theme_combo_ = new QComboBox(appearance_group);
    theme_combo_->setObjectName(QStringLiteral("aida.view.settings.editor.theme"));
    theme_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    theme_combo_->setMinimumContentsLength(12);
    theme_combo_->addItems({ QStringLiteral("AiDA Dark"), QStringLiteral("AiDA Light"),
        QStringLiteral("Claude Dark"), QStringLiteral("Claude Light") });
    theme_combo_->setToolTip(QStringLiteral("Color theme applied across the IDE"));
    appearance_form->addRow(QStringLiteral("Active theme"), theme_combo_);
    density_combo_ = new QComboBox(appearance_group);
    density_combo_->setObjectName(QStringLiteral("aida.view.settings.editor.density"));
    density_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    density_combo_->setMinimumContentsLength(12);
    density_combo_->addItems({ QStringLiteral("Compact"), QStringLiteral("Comfortable") });
    density_combo_->setToolTip(QStringLiteral("Control spacing and row heights"));
    appearance_form->addRow(QStringLiteral("Interface density"), density_combo_);
    reduced_motion_check_ = new QCheckBox(QStringLiteral("Reduce interface motion"),
        appearance_group);
    reduced_motion_check_->setObjectName(
        QStringLiteral("aida.view.settings.editor.reducedMotion"));
    reduced_motion_check_->setToolTip(QStringLiteral(
        "Disable decorative transitions and animated progress where a static equivalent "
        "is available"));
    appearance_form->addRow(QString(), reduced_motion_check_);
    diagnostics_check_ = new QCheckBox(
        QStringLiteral("Show frame diagnostics in status bar"), appearance_group);
    diagnostics_check_->setObjectName(
        QStringLiteral("aida.view.settings.editor.diagnostics"));
    diagnostics_check_->setToolTip(QStringLiteral(
        "Overlay render timing and frame counters in the status bar"));
    appearance_form->addRow(QString(), diagnostics_check_);
    root->addWidget(appearance_group);

    auto* managed_group = new QGroupBox(
        QStringLiteral("Managed editor features (always on)"), this);
    auto* managed_layout = new QVBoxLayout(managed_group);
    managed_layout->setSpacing(t.spacing.xs);
    const char* managed[] = {
        "Line numbers",
        "Word wrap",
        "Minimap",
        "Bracket matching",
        "Current-line highlight",
        "Auto-complete",
        "Ghost text",
        "Auto-save",
    };
    for (const char* label : managed) {
        auto* check = new QCheckBox(QString::fromLatin1(label), managed_group);
        check->setChecked(true);
        check->setEnabled(false);
        check->setToolTip(QStringLiteral(
            "Managed by AiDA - forced on at startup"));
        managed_layout->addWidget(check);
    }
    root->addWidget(managed_group);
    root->addStretch(1);

    connect(tab_size_spin_, &QSpinBox::valueChanged, this, [this](int) { persist(); });
    connect(font_size_spin_, &QDoubleSpinBox::valueChanged, this, [this](double) { persist(); });
    connect(full_line_select_check_, &QCheckBox::toggled, this, [](bool checked) {
        editor_config::disasm_full_line_select = checked;
    });
    connect(theme_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (loading_)
            return;
        if (index != g_sa_settings.active_theme_idx)
            chrome::AidaThemeCatalogController::instance().applyBuiltIn(index, true);
    });
    connect(density_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (loading_)
            return;
        g_sa_settings.ui_density = index == 1 ? 1 : 0;
        theme::AidaThemeController::instance().applySettings(g_sa_settings.ui_density,
            g_sa_settings.ui_reduced_motion, g_sa_settings.ui_diagnostics_mode);
        static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
    });
    connect(reduced_motion_check_, &QCheckBox::toggled, this, [this](bool checked) {
        if (loading_)
            return;
        g_sa_settings.ui_reduced_motion = checked;
        theme::AidaThemeController::instance().applySettings(g_sa_settings.ui_density,
            g_sa_settings.ui_reduced_motion, g_sa_settings.ui_diagnostics_mode);
        static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
    });
    connect(diagnostics_check_, &QCheckBox::toggled, this, [this](bool checked) {
        if (loading_)
            return;
        g_sa_settings.ui_diagnostics_mode = checked;
        theme::AidaThemeController::instance().applySettings(g_sa_settings.ui_density,
            g_sa_settings.ui_reduced_motion, g_sa_settings.ui_diagnostics_mode);
        static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
    });
}

AidaSettingsEditorPage::AidaSettingsEditorPage(QWidget* parent) : QWidget(parent) {
    buildUi();
}

void AidaSettingsEditorPage::loadOnce() {
    if (loaded_)
        return;
    loaded_ = true;
    loading_ = true;
    tab_size_spin_->setValue((std::max)(g_sa_settings.editor_tab_size, 1));
    font_size_spin_->setValue(g_sa_settings.editor_font_size);
    full_line_select_check_->setChecked(editor_config::disasm_full_line_select);
    theme_combo_->setCurrentIndex(std::clamp(g_sa_settings.active_theme_idx, 0,
        theme_combo_->count() - 1));
    density_combo_->setCurrentIndex(g_sa_settings.ui_density == 1 ? 1 : 0);
    reduced_motion_check_->setChecked(g_sa_settings.ui_reduced_motion);
    diagnostics_check_->setChecked(g_sa_settings.ui_diagnostics_mode);
    loading_ = false;
}

void AidaSettingsEditorPage::persist() {
    if (loading_ || !loaded_)
        return;
    editor_config::tab_size = (std::max)(tab_size_spin_->value(), 1);
    editor_config::font_size = static_cast<float>(font_size_spin_->value());
    editor_config::show_line_numbers = true;
    editor_config::word_wrap = true;
    editor_config::minimap = true;
    editor_config::bracket_match = true;
    editor_config::highlight_current_line = true;
    editor_config::auto_complete = true;

    g_sa_settings.editor_tab_size = (std::max)(tab_size_spin_->value(), 1);
    g_sa_settings.editor_font_size = static_cast<float>(font_size_spin_->value());
    g_sa_settings.editor_line_numbers = true;
    g_sa_settings.editor_word_wrap = true;
    g_sa_settings.editor_minimap = true;
    g_sa_settings.editor_bracket_match = true;
    g_sa_settings.editor_highlight_line = true;
    g_sa_settings.editor_auto_complete = true;
    g_sa_settings.ghost_text_enabled = true;
    g_sa_settings.auto_save_enabled = true;
    static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
}

void AidaSettingsEditorPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    loadOnce();
}

}
