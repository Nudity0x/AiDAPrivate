#pragma once

#include <QWidget>

#include <QHash>
#include <QString>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace aida::provider {
struct provider_info_t;
}

namespace aida::qt::ai {

class AidaProviderCardWidget : public QWidget {
    Q_OBJECT
public:
    explicit AidaProviderCardWidget(const QString& provider_id, QWidget* parent = nullptr);

    QString providerId() const noexcept { return provider_id_; }
    void refreshCard();
    void setDetailOpen(bool open);

Q_SIGNALS:
    void detailsToggled(const QString& provider_id, bool open);
    void testRequested(const QString& provider_id, const QString& model_id);
    void setDefaultRequested(const QString& provider_id, const QString& model_id);
    void modelSelectionChanged(const QString& provider_id, const QString& model_id);
    void contextMenuRequested(const QString& provider_id, const QPoint& global_pos);

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void rebuildModelCombo();
    void updateTestPresentation();

    QString provider_id_;
    QLabel* glyph_ = nullptr;
    QLabel* name_label_ = nullptr;
    QLabel* status_pill_ = nullptr;
    QLabel* badges_label_ = nullptr;
    QComboBox* model_combo_ = nullptr;
    QLabel* cost_label_ = nullptr;
    QLabel* context_label_ = nullptr;
    QProgressBar* cost_bar_ = nullptr;
    QPushButton* test_button_ = nullptr;
    QPushButton* default_button_ = nullptr;
    QPushButton* details_button_ = nullptr;
    QLabel* test_result_ = nullptr;
    bool loading_combo_ = false;
    bool detail_open_ = false;
};

class AidaProviderView : public QWidget {
    Q_OBJECT
public:
    explicit AidaProviderView(QWidget* parent = nullptr);
    ~AidaProviderView() override;

    static void runTestConnection(const std::string& provider_id, const std::string& model_id);
    static void startCatalogRefresh();
    static void shutdownWorkers();

    struct test_result_t {
        bool completed = false;
        bool success = false;
        int latency_ms = 0;
        int http_status = 0;
        std::string message;
        std::string provider_id;
        std::string model_id;
    };

    void onRefreshCompleted();
    void onTestsUpdated();

    static std::mutex s_mtx;
    static std::map<std::string, test_result_t> s_pending_results;
    static std::map<std::string, std::shared_ptr<std::atomic<bool>>> s_in_flight_tests;
    static std::atomic<bool> s_refresh_in_flight;
    static std::atomic<bool> s_refresh_completed;
    static std::atomic<bool> s_refresh_success;
    static std::string s_refresh_message;
    static std::atomic<bool> s_shutdown;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void rebuildCards();
    void refreshDetailPane();
    void updateCacheAgeCallout();

    QLineEdit* search_edit_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QLabel* cache_age_label_ = nullptr;
    QScrollArea* cards_scroll_ = nullptr;
    QWidget* cards_host_ = nullptr;
    QVBoxLayout* cards_layout_ = nullptr;
    QWidget* detail_pane_ = nullptr;
    QLabel* detail_title_ = nullptr;
    QLineEdit* detail_base_url_ = nullptr;
    QPlainTextEdit* detail_headers_ = nullptr;
    QPlainTextEdit* detail_raw_json_ = nullptr;
    QCheckBox* raw_toggle_ = nullptr;
    QString selected_detail_provider_id_;

    static bool s_catalog_load_started;
};

}
