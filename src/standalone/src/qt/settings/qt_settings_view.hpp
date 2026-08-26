#pragma once

#include <QWidget>

#include <QString>

class QHBoxLayout;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTimer;

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::auth {
class AidaAuthView;
}

namespace aida::qt::settings {

class AidaSettingsMcpPage;
class AidaSettingsEditorPage;

class AidaSettingsView : public QWidget {
    Q_OBJECT
public:
    explicit AidaSettingsView(QWidget* parent = nullptr);
    ~AidaSettingsView() override;

    void openToProvider(const QString& provider_id);
    void setActiveTab(int tab_index);

    static AidaSettingsView* activeInstance();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void refreshPersistenceStatus();
    void applyCompactMode(bool compact);

    QLabel* status_label_ = nullptr;
    QListWidget* sidebar_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    auth::AidaAuthView* accounts_page_ = nullptr;
    AidaSettingsMcpPage* mcp_page_ = nullptr;
    AidaSettingsEditorPage* editor_page_ = nullptr;
    QTimer* status_timer_ = nullptr;
    bool compact_ = false;
};

void install_settings_domain(docking::AidaDockHost* host);

}
