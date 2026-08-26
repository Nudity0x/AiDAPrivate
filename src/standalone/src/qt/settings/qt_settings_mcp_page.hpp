#pragma once

#include <QAbstractListModel>
#include <QWidget>

#include <QString>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/mcp/mcp_client.hpp"

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSplitter;
class QTimer;
class QVBoxLayout;

struct mcp_client_server_t;

namespace aida::qt::settings {

class AidaMcpServerDraftModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AidaMcpServerDraftModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void loadFromSettings();
    void discardChanges();

    std::vector<mcp_client_server_t>& draft() noexcept { return draft_; }
    const std::vector<mcp_client_server_t>& draft() const noexcept { return draft_; }
    bool dirty() const noexcept { return dirty_; }
    void markClean();
    void touchRows();
    void updateRow(int row, const mcp_client_server_t& server);
    int appendNewServer();
    void removeRow(int row);

Q_SIGNALS:
    void draftEdited();

private:
    std::vector<mcp_client_server_t> draft_;
    bool dirty_ = false;
};

class AidaMcpInstalledSection : public QWidget {
    Q_OBJECT
public:
    explicit AidaMcpInstalledSection(QWidget* parent = nullptr);

    void refresh();

private:
    QVBoxLayout* rows_layout_ = nullptr;
    std::string last_signature_;
};

class AidaSettingsMcpPage : public QWidget {
    Q_OBJECT
public:
    explicit AidaSettingsMcpPage(QWidget* parent = nullptr);
    ~AidaSettingsMcpPage() override;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void refreshFormFromDraft();
    void commitFormToDraft();
    void refreshRowStatuses();
    void onApply();
    void onRemoveSelected();
    void onFieldEdited();
    void onSignIn(const std::string& server_name);
    void onCancelAuth(const std::string& server_name);
    void updateArgvPreview();
    void updateTransportVisibility();

    std::uint64_t beginOauthGeneration(const std::string& server_name);
    bool completeOauthGeneration(const std::string& server_name, std::uint64_t generation);
    void cancelOauthGeneration(const std::string& server_name);

    AidaMcpServerDraftModel* model_ = nullptr;
    QListView* list_ = nullptr;
    QPushButton* add_button_ = nullptr;
    QPushButton* apply_button_ = nullptr;
    QPushButton* discard_button_ = nullptr;
    QPushButton* remove_button_ = nullptr;
    QPushButton* marketplace_button_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QComboBox* transport_combo_ = nullptr;
    QLineEdit* url_edit_ = nullptr;
    QLineEdit* key_edit_ = nullptr;
    QLineEdit* command_edit_ = nullptr;
    QLineEdit* args_edit_ = nullptr;
    QLabel* argv_preview_ = nullptr;
    QCheckBox* enabled_check_ = nullptr;
    QCheckBox* auto_connect_check_ = nullptr;
    QPushButton* reconnect_button_ = nullptr;
    QPushButton* disconnect_button_ = nullptr;
    QLabel* oauth_pill_ = nullptr;
    QPushButton* oauth_button_ = nullptr;
    QLabel* dirty_label_ = nullptr;
    QLabel* form_error_ = nullptr;
    AidaMcpInstalledSection* installed_section_ = nullptr;
    QTimer* status_timer_ = nullptr;

    bool loading_form_ = false;
    int selected_row_ = -1;
    std::mutex oauth_mtx_;
    std::unordered_map<std::string, std::uint64_t> oauth_generations_;
    std::uint64_t oauth_generation_ = 0;
};

}
