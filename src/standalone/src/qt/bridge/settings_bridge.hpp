#pragma once

#include "core/settings/settings_persistence_service.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class QByteArray;
class QSettings;
class QTimer;

struct settings_sa_t;

namespace aida::qt::bridge {

class QtSettingsBridge : public QObject {
    Q_OBJECT
public:
    enum class section_t : std::uint8_t {
        providers,
        chat,
        editor,
        terminal,
        security_approvals,
        sandbox,
        mcp,
        symbols,
        ui_theme
    };

    explicit QtSettingsBridge(QObject* parent = nullptr);
    ~QtSettingsBridge() override;

    const settings_sa_t& settings() const noexcept;
    bool mutate(section_t section, const std::function<void(settings_sa_t&)>& fn,
                const char* label = nullptr);

    aida::settings_persistence::status_t status() const noexcept { return latest_; }

    static bool is_forced(const char* field_name) noexcept;
    static bool is_secret_field(const char* field_name) noexcept;
    static bool value_is_secret(const std::string& value) noexcept;

    QSettings& qt_settings();
    void save_qt_state(const char* key, const QByteArray& value);
    QByteArray qt_state(const char* key) const;
    QString qt_settings_path() const;

Q_SIGNALS:
    void statusChanged(const aida::settings_persistence::status_t& status);
    void saveRejected(const QString& section, const QString& detail);

private:
    QTimer* status_timer_ = nullptr;
    mutable std::unique_ptr<QSettings> qt_settings_;
    aida::settings_persistence::status_t latest_;
};

}
