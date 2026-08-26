#pragma once

#include <QFrame>
#include <QObject>
#include <QWidget>

#include <QString>

#include <string>
#include <vector>

#include "core/mcp/mcp_marketplace.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSplitter;
class QStackedLayout;
class QTimer;
class QVBoxLayout;

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::widgets {
class AidaSearchField;
class AidaStateView;
class AidaToolButton;
}

namespace aida::qt::mcp {

class AidaMcpMarketplaceController : public QObject {
    Q_OBJECT
public:
    static AidaMcpMarketplaceController& instance();

    void install();
    void shutdown();

    void refreshViewState();

Q_SIGNALS:
    void searchResultsReady();
    void searchFailed(const QString& error);
    void installFinished(bool success, const QString& package, const QString& error);
    void installLogAppended(const QString& line);
    void installedChanged();

private:
    explicit AidaMcpMarketplaceController(QObject* parent = nullptr);

    ::mcp_marketplace::search_state_t last_search_state_ =
        ::mcp_marketplace::search_state_t::idle;
    ::mcp_marketplace::install_state_t last_install_state_ =
        ::mcp_marketplace::install_state_t::idle;
    std::string last_search_error_;
    std::string last_install_error_;
    QTimer* tick_timer_ = nullptr;
    bool installed_ = false;
};

class AidaMarketplaceCardWidget : public QFrame {
    Q_OBJECT
public:
    explicit AidaMarketplaceCardWidget(const ::mcp_marketplace::package_info_t& package,
                                       QWidget* parent = nullptr);

    const ::mcp_marketplace::package_info_t& package() const noexcept { return package_; }
    void refreshInstallButton();
    void setSelected(bool selected);

Q_SIGNALS:
    void openDetails(const QString& package_name);
    void reviewInstall(const QString& package_name);
    void contextMenuRequested(const QString& package_name, const QPoint& global_pos);

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void relayout_text();

    ::mcp_marketplace::package_info_t package_;
    QString full_name_;
    QString full_meta_;
    QLabel* glyph_label_ = nullptr;
    QLabel* name_label_ = nullptr;
    QLabel* meta_label_ = nullptr;
    QLabel* description_label_ = nullptr;
    QPushButton* install_button_ = nullptr;
};

class AidaMcpInstallReviewDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    AidaMcpInstallReviewDialog(const ::mcp_marketplace::package_info_t& package,
                               QWidget* parent = nullptr);

    static void review(const ::mcp_marketplace::package_info_t& package, QWidget* parent);

Q_SIGNALS:
    void installConfirmed(const ::mcp_marketplace::package_info_t& package);

private:
    ::mcp_marketplace::package_info_t package_;
    QPushButton* confirm_button_ = nullptr;
};

class AidaMcpMarketplaceView : public QWidget {
    Q_OBJECT
public:
    explicit AidaMcpMarketplaceView(QWidget* parent = nullptr);
    ~AidaMcpMarketplaceView() override = default;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void startSearch();
    void rebuildResults();
    void refreshDetail();
    void appendInstallLog(const QString& line);
    void openPackageMenu(const QString& package_name, const QPoint& global_pos);
    void beginInstall(const ::mcp_marketplace::package_info_t& package);
    void presentEmptyState();
    void mark_selected_cards();

    AidaMcpMarketplaceController* controller_ = nullptr;
    widgets::AidaSearchField* search_edit_ = nullptr;
    QPushButton* search_button_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QWidget* results_host_ = nullptr;
    QVBoxLayout* results_layout_ = nullptr;
    QScrollArea* results_scroll_ = nullptr;
    QStackedLayout* results_stack_ = nullptr;
    widgets::AidaStateView* empty_state_ = nullptr;
    QLabel* results_status_ = nullptr;
    QWidget* detail_panel_ = nullptr;
    QLabel* detail_title_ = nullptr;
    QLabel* detail_name_ = nullptr;
    QLabel* detail_description_ = nullptr;
    QLabel* detail_badges_ = nullptr;
    QVBoxLayout* detail_provenance_ = nullptr;
    QLabel* detail_risk_ = nullptr;
    QLabel* detail_tags_ = nullptr;
    QPushButton* detail_install_button_ = nullptr;
    QPlainTextEdit* install_log_ = nullptr;
    widgets::AidaToolButton* detail_close_ = nullptr;
    QTimer* state_poll_ = nullptr;

    std::string selected_pkg_;
    std::string installing_pkg_;
    std::vector<::mcp_marketplace::package_info_t> results_;
    bool first_search_done_ = false;
};

void install_mcp_marketplace_domain(docking::AidaDockHost* host);

}
