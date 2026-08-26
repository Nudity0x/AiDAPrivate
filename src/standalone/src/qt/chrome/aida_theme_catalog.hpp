#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <string>
#include <vector>

#include "core/settings/theme_transfer_service.hpp"

class QTimer;

namespace aida::qt::theme {
struct AidaCustomTheme;
}

namespace aida::qt::chrome {

struct AidaCatalogTheme {
    std::string name = "Custom Theme";
    float accent[3] = { 0.53f, 0.53f, 1.0f };
    std::uint32_t bg_base = 0;
    std::uint32_t panel_bg = 0;
    std::uint32_t panel_header = 0;
    std::uint32_t title_bar = 0;
    std::uint32_t text_primary = 0;
    std::uint32_t text_secondary = 0;
    std::uint32_t text_dim = 0;
    int icon_index = -1;
    std::string icon_file_path;
};

class AidaThemeCatalogController : public QObject {
    Q_OBJECT
public:
    static AidaThemeCatalogController& instance();

    void initializeFromSettings();

    int builtInActive() const noexcept { return active_builtin_; }
    int customActive() const noexcept { return active_custom_; }
    const std::vector<AidaCatalogTheme>& customThemes() const noexcept { return custom_; }

    void applyBuiltIn(int index, bool animate);
    void applyCustom(int index);
    void toggleDayNight();

    bool saveCustomTheme(const AidaCatalogTheme& theme, int editing_index,
                         std::string& error);
    bool deleteCustomTheme(int index, std::string& error);

    bool validate(const AidaCatalogTheme& theme, std::string& error) const;
    bool persistCatalog(std::string& error);

    QString lastError() const { return last_error_; }
    void clearLastError() { last_error_.clear(); }
    void reportError(const QString& error);

    void noteTransferActivity();

    static theme::AidaCustomTheme toControllerTheme(const AidaCatalogTheme& theme);
    static AidaCatalogTheme fromTransfer(const aida::theme_transfer::theme_t& transfer);
    static aida::theme_transfer::theme_t toTransfer(const AidaCatalogTheme& theme);

Q_SIGNALS:
    void catalogChanged();
    void activeThemeChanged();
    void transferStatusChanged();

private:
    explicit AidaThemeCatalogController(QObject* parent = nullptr);
    void processTransferCompletion();
    void refreshTransferPoll();

    std::vector<AidaCatalogTheme> custom_;
    int active_builtin_ = 0;
    int active_custom_ = -1;
    QString last_error_;
    QTimer* transfer_poll_ = nullptr;
    bool initialized_ = false;
};

}
