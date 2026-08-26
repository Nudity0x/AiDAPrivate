#include "qt/chrome/aida_theme_catalog.hpp"

#include <QTimer>

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "core/settings/settings_persistence_service.hpp"
#include "core/settings/standalone_settings.hpp"
#include "helpers/diag_log.hpp"
#include "qt/theme/aida_theme_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::chrome {

namespace {

constexpr std::size_t k_catalog_payload_cap = 1024U * 1024U;

std::uint32_t abgr_to_argb(std::uint32_t abgr)
{
    return (abgr & 0xFF00FF00u) | ((abgr & 0x000000FFu) << 16) | ((abgr & 0x00FF0000u) >> 16);
}

bool parse_catalog(const std::string& json_text, std::vector<AidaCatalogTheme>& out)
{
    out.clear();
    if (json_text.empty())
        return true;
    const auto parsed = nlohmann::json::parse(json_text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array())
        return false;
    if (parsed.size() > aida::theme_transfer::maximum_theme_count)
        return false;
    for (const auto& item : parsed) {
        if (!item.is_object())
            return false;
        AidaCatalogTheme theme;
        theme.name = item.value("name", std::string());
        const auto accent = item.find("accent");
        if (accent != item.end() && accent->is_array() && accent->size() == 3) {
            for (int channel = 0; channel < 3; ++channel)
                theme.accent[channel] = (*accent)[static_cast<std::size_t>(channel)]
                    .get<float>();
        }
        theme.bg_base = item.value("bg_base", std::uint32_t{0});
        theme.panel_bg = item.value("panel_bg", std::uint32_t{0});
        theme.panel_header = item.value("panel_header", std::uint32_t{0});
        theme.title_bar = item.value("title_bar", std::uint32_t{0});
        theme.text_primary = item.value("text_primary", std::uint32_t{0});
        theme.text_secondary = item.value("text_secondary", std::uint32_t{0});
        theme.text_dim = item.value("text_dim", std::uint32_t{0});
        theme.icon_index = item.value("icon_index", -1);
        theme.icon_file_path = item.value("icon_file_path", std::string());
        if (theme.name.empty() ||
            theme.name.size() > aida::theme_transfer::maximum_theme_name_bytes ||
            theme.icon_file_path.size() > aida::theme_transfer::maximum_icon_path_bytes)
            continue;
        out.push_back(std::move(theme));
    }
    return true;
}

}

AidaThemeCatalogController& AidaThemeCatalogController::instance()
{
    static AidaThemeCatalogController* controller = [] {
        return new AidaThemeCatalogController();
    }();
    return *controller;
}

AidaThemeCatalogController::AidaThemeCatalogController(QObject* parent)
    : QObject(parent)
{
    transfer_poll_ = new QTimer(this);
    transfer_poll_->setInterval(300);
    transfer_poll_->setTimerType(Qt::CoarseTimer);
    connect(transfer_poll_, &QTimer::timeout, this, [this] {
        processTransferCompletion();
        refreshTransferPoll();
    });
}

void AidaThemeCatalogController::initializeFromSettings()
{
    if (initialized_)
        return;
    initialized_ = true;
    std::vector<AidaCatalogTheme> parsed;
    if (parse_catalog(g_sa_settings.custom_themes_json, parsed))
        custom_ = std::move(parsed);
    else
        diag::log_tagged_critical("qt_theme", "custom_theme_catalog_parse_failed");
    active_builtin_ = g_sa_settings.active_theme_idx;
    active_custom_ = g_sa_settings.active_custom_theme_idx;
    if (active_custom_ >= static_cast<int>(custom_.size()))
        active_custom_ = -1;

    auto& controller = theme::AidaThemeController::instance();
    if (active_custom_ >= 0) {
        controller.applyCustomTheme(toControllerTheme(
            custom_[static_cast<std::size_t>(active_custom_)]));
    } else {
        controller.applyForIndex(active_builtin_, false);
    }
    controller.applySettings(g_sa_settings.ui_density, g_sa_settings.ui_reduced_motion,
        g_sa_settings.ui_diagnostics_mode);
}

void AidaThemeCatalogController::applyBuiltIn(int index, bool animate)
{
    active_builtin_ = index;
    active_custom_ = -1;
    g_sa_settings.active_theme_idx = index;
    g_sa_settings.active_custom_theme_idx = -1;
    static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
    theme::AidaThemeController::instance().applyForIndex(index, animate);
    diag::log_tagged_fmt("ui", "theme_changed idx=%d", index);
    Q_EMIT activeThemeChanged();
}

void AidaThemeCatalogController::applyCustom(int index)
{
    if (index < 0 || index >= static_cast<int>(custom_.size()))
        return;
    active_custom_ = index;
    g_sa_settings.active_custom_theme_idx = index;
    static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
    theme::AidaThemeController::instance().applyCustomTheme(
        toControllerTheme(custom_[static_cast<std::size_t>(index)]));
    diag::log_tagged_fmt("ui", "theme_changed custom=%d name='%s'", index,
        custom_[static_cast<std::size_t>(index)].name.c_str());
    Q_EMIT activeThemeChanged();
}

void AidaThemeCatalogController::toggleDayNight()
{
    const bool currently_dark = widgets::relative_luminance(theme::tokens().bg_base) < 0.5;
    int new_index;
    switch (active_builtin_) {
    case 0: new_index = 1; break;
    case 1: new_index = 0; break;
    case 2: new_index = 3; break;
    case 3: new_index = 2; break;
    default: new_index = currently_dark ? 1 : 0; break;
    }
    applyBuiltIn(new_index, true);
}

bool AidaThemeCatalogController::validate(const AidaCatalogTheme& theme, std::string& error) const
{
    if (theme.name.empty() ||
        theme.name.size() > aida::theme_transfer::maximum_theme_name_bytes ||
        theme.name.find('\0') != std::string::npos) {
        error = "Theme names must contain between 1 and 96 bytes.";
        return false;
    }
    for (const float channel : theme.accent) {
        if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f) {
            error = "Theme accent channels must be finite values between 0 and 1.";
            return false;
        }
    }
    if (theme.icon_index < -1 || theme.icon_index > 4095) {
        error = "The theme icon index is outside the supported range.";
        return false;
    }
    if (theme.icon_file_path.size() > aida::theme_transfer::maximum_icon_path_bytes ||
        theme.icon_file_path.find('\0') != std::string::npos) {
        error = "The theme icon path exceeds its exact bound or contains an embedded null.";
        return false;
    }
    error.clear();
    return true;
}

bool AidaThemeCatalogController::persistCatalog(std::string& error)
{
    if (custom_.size() > aida::theme_transfer::maximum_theme_count) {
        error = "The custom theme catalog is limited to 128 themes.";
        return false;
    }
    nlohmann::json catalog = nlohmann::json::array();
    for (const auto& theme : custom_) {
        if (!validate(theme, error))
            return false;
        const auto captured = toTransfer(theme);
        catalog.push_back({
            {"schema_version", 1},
            {"name", captured.name},
            {"accent", {captured.accent[0], captured.accent[1], captured.accent[2]}},
            {"bg_base", captured.bg_base},
            {"panel_bg", captured.panel_bg},
            {"panel_header", captured.panel_header},
            {"title_bar", captured.title_bar},
            {"text_primary", captured.text_primary},
            {"text_secondary", captured.text_secondary},
            {"text_dim", captured.text_dim},
            {"icon_index", captured.icon_index},
            {"icon_file_path", captured.icon_file_path}
        });
    }
    std::string payload;
    try {
        payload = catalog.dump();
    } catch (...) {
        error = "The custom theme catalog could not be serialized.";
        return false;
    }
    if (payload.size() > k_catalog_payload_cap) {
        error = "The custom theme catalog exceeds its 1 MiB bound.";
        return false;
    }
    const std::string previous_payload = g_sa_settings.custom_themes_json;
    const int previous_active = g_sa_settings.active_custom_theme_idx;
    g_sa_settings.custom_themes_json = std::move(payload);
    g_sa_settings.active_custom_theme_idx = active_custom_;
    const auto requested = aida::settings_persistence::request_save(g_sa_settings);
    if (!aida::settings_persistence::accepted(requested)) {
        g_sa_settings.custom_themes_json = previous_payload;
        g_sa_settings.active_custom_theme_idx = previous_active;
        error = "The immutable settings snapshot for the custom theme catalog was rejected.";
        return false;
    }
    error.clear();
    Q_EMIT catalogChanged();
    return true;
}

bool AidaThemeCatalogController::saveCustomTheme(const AidaCatalogTheme& theme,
                                                 int editing_index, std::string& error)
{
    const bool replacing = editing_index >= 0 &&
        editing_index < static_cast<int>(custom_.size());
    if (!validate(theme, error))
        return false;
    if (!replacing && editing_index >= 0) {
        error = "The custom theme being edited no longer exists.";
        return false;
    }
    if (!replacing && custom_.size() >= aida::theme_transfer::maximum_theme_count) {
        error = "Delete a custom theme before creating another; the catalog limit is 128.";
        return false;
    }
    const int previous_active = active_custom_;
    AidaCatalogTheme previous_theme;
    if (replacing) {
        previous_theme = custom_[static_cast<std::size_t>(editing_index)];
        custom_[static_cast<std::size_t>(editing_index)] = theme;
    } else {
        custom_.push_back(theme);
    }
    active_custom_ = replacing ? editing_index : static_cast<int>(custom_.size()) - 1;
    if (!persistCatalog(error)) {
        if (replacing)
            custom_[static_cast<std::size_t>(editing_index)] = std::move(previous_theme);
        else
            custom_.pop_back();
        active_custom_ = previous_active;
        last_error_ = QString::fromStdString(error);
        return false;
    }
    theme::AidaThemeController::instance().applyCustomTheme(
        toControllerTheme(custom_[static_cast<std::size_t>(active_custom_)]));
    g_sa_settings.active_custom_theme_idx = active_custom_;
    last_error_.clear();
    Q_EMIT activeThemeChanged();
    return true;
}

bool AidaThemeCatalogController::deleteCustomTheme(int index, std::string& error)
{
    if (index < 0 || index >= static_cast<int>(custom_.size())) {
        error = "The reviewed theme changed or no longer exists. Cancel and select it again.";
        return false;
    }
    const int previous_active = active_custom_;
    AidaCatalogTheme removed = custom_[static_cast<std::size_t>(index)];
    custom_.erase(custom_.begin() + index);
    if (active_custom_ == index)
        active_custom_ = -1;
    else if (active_custom_ > index)
        --active_custom_;
    if (!persistCatalog(error)) {
        custom_.insert(custom_.begin() + index, std::move(removed));
        active_custom_ = previous_active;
        last_error_ = QString::fromStdString(error);
        return false;
    }
    if (active_custom_ < 0)
        theme::AidaThemeController::instance().applyForIndex(active_builtin_, false);
    last_error_.clear();
    Q_EMIT activeThemeChanged();
    return true;
}

theme::AidaCustomTheme AidaThemeCatalogController::toControllerTheme(
    const AidaCatalogTheme& theme)
{
    theme::AidaCustomTheme converted;
    converted.name = QString::fromStdString(theme.name);
    converted.accent[0] = theme.accent[0];
    converted.accent[1] = theme.accent[1];
    converted.accent[2] = theme.accent[2];
    converted.bg_base = abgr_to_argb(theme.bg_base);
    converted.panel_bg = abgr_to_argb(theme.panel_bg);
    converted.panel_header = abgr_to_argb(theme.panel_header);
    converted.title_bar = abgr_to_argb(theme.title_bar);
    converted.text_primary = abgr_to_argb(theme.text_primary);
    converted.text_secondary = abgr_to_argb(theme.text_secondary);
    converted.text_dim = abgr_to_argb(theme.text_dim);
    return converted;
}

AidaCatalogTheme AidaThemeCatalogController::fromTransfer(
    const aida::theme_transfer::theme_t& transfer)
{
    AidaCatalogTheme materialized;
    materialized.name = transfer.name;
    materialized.accent[0] = transfer.accent[0];
    materialized.accent[1] = transfer.accent[1];
    materialized.accent[2] = transfer.accent[2];
    materialized.bg_base = transfer.bg_base;
    materialized.panel_bg = transfer.panel_bg;
    materialized.panel_header = transfer.panel_header;
    materialized.title_bar = transfer.title_bar;
    materialized.text_primary = transfer.text_primary;
    materialized.text_secondary = transfer.text_secondary;
    materialized.text_dim = transfer.text_dim;
    materialized.icon_index = transfer.icon_index;
    materialized.icon_file_path = transfer.icon_file_path;
    return materialized;
}

aida::theme_transfer::theme_t AidaThemeCatalogController::toTransfer(
    const AidaCatalogTheme& theme)
{
    aida::theme_transfer::theme_t captured;
    captured.name = theme.name;
    captured.accent = { theme.accent[0], theme.accent[1], theme.accent[2] };
    captured.bg_base = theme.bg_base;
    captured.panel_bg = theme.panel_bg;
    captured.panel_header = theme.panel_header;
    captured.title_bar = theme.title_bar;
    captured.text_primary = theme.text_primary;
    captured.text_secondary = theme.text_secondary;
    captured.text_dim = theme.text_dim;
    captured.icon_index = theme.icon_index;
    captured.icon_file_path = theme.icon_file_path;
    return captured;
}

void AidaThemeCatalogController::processTransferCompletion()
{
    auto completion = aida::theme_transfer::take_completion();
    if (!completion || completion->operation !=
            aida::theme_transfer::operation_t::import_theme ||
        !completion->success)
        return;
    if (!completion->imported_theme) {
        last_error_ = QStringLiteral("The validated theme import did not contain a theme.");
        aida::theme_transfer::acknowledge_import(completion->serial, false,
            last_error_.toStdString());
        Q_EMIT transferStatusChanged();
        return;
    }
    if (custom_.size() >= aida::theme_transfer::maximum_theme_count) {
        last_error_ = QStringLiteral("Delete a custom theme before importing another; the catalog limit is 128.");
        aida::theme_transfer::acknowledge_import(completion->serial, false,
            last_error_.toStdString());
        Q_EMIT transferStatusChanged();
        return;
    }
    const int previous_active = active_custom_;
    custom_.push_back(fromTransfer(*completion->imported_theme));
    active_custom_ = static_cast<int>(custom_.size()) - 1;
    std::string persistence_error;
    if (!persistCatalog(persistence_error)) {
        custom_.pop_back();
        active_custom_ = previous_active;
        last_error_ = QString::fromStdString(persistence_error);
        aida::theme_transfer::acknowledge_import(completion->serial, false,
            persistence_error);
        Q_EMIT transferStatusChanged();
        return;
    }
    theme::AidaThemeController::instance().applyCustomTheme(
        toControllerTheme(custom_.back()));
    g_sa_settings.active_custom_theme_idx = active_custom_;
    last_error_.clear();
    aida::theme_transfer::acknowledge_import(completion->serial, true);
    Q_EMIT catalogChanged();
    Q_EMIT activeThemeChanged();
    Q_EMIT transferStatusChanged();
}

void AidaThemeCatalogController::refreshTransferPoll()
{
    const auto status = aida::theme_transfer::status();
    if (status.pending) {
        if (!transfer_poll_->isActive())
            transfer_poll_->start();
    } else if (transfer_poll_->isActive()) {
        transfer_poll_->stop();
    }
}

void AidaThemeCatalogController::noteTransferActivity()
{
    processTransferCompletion();
    refreshTransferPoll();
}

void AidaThemeCatalogController::reportError(const QString& error)
{
    last_error_ = error;
    Q_EMIT transferStatusChanged();
}

}
