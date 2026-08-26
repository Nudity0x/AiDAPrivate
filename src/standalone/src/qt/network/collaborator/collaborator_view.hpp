#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QModelRoleDataSpan>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/network/burp/collaborator.hpp"
#include "qt/network/network_pane_base.hpp"

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QStackedLayout;
class QTableView;
class QTimer;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class BurpOperationRunner;

// CollaboratorInteractionsModel backs the interactions table (ID/Time/Kind/
// Client/Token). The model owns the latest immutable publication plus the
// filtered index vector (reverse order, newest first); the filter signature
// (kind\ntoken\nip) + cache generation gate recomputation, exactly as the
// legacy render path did (collaborator_view.cpp:522-535 pre-migration).
class CollaboratorInteractionsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Id = 0, Time, Kind, Client, Token, ColumnCount };

    explicit CollaboratorInteractionsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    void setPublication(
        std::shared_ptr<const std::vector<aida::burp::collaborator::interaction_t>> publication,
        std::uint64_t generation, std::uint64_t started_ms);
    void setFilter(const std::string& kind, const std::string& token, const std::string& ip);
    const aida::burp::collaborator::interaction_t* interactionAt(int row) const noexcept;
    const aida::burp::collaborator::interaction_t* findById(std::uint64_t id) const noexcept;

private:
    void refilter();

    std::shared_ptr<const std::vector<aida::burp::collaborator::interaction_t>> publication_;
    std::vector<std::size_t> filtered_indices_;
    std::uint64_t generation_ = 0;
    std::uint64_t started_ms_ = 0;
    std::string filter_kind_ = "all";
    std::string filter_token_;
    std::string filter_ip_;
};

class CollaboratorView : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit CollaboratorView(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    struct cache_publication_t {
        std::vector<aida::burp::collaborator::interaction_t> interactions;
        aida::burp::collaborator::status_t status;
    };

    void requestCacheRefresh();
    void applyCachePublication(const std::shared_ptr<const cache_publication_t>& publication);
    void refreshStatusCard(const aida::burp::collaborator::status_t& status);
    void refreshButtons(const aida::burp::collaborator::status_t& status);
    void refreshDetail();
    void submitStart();
    void submitReviewedOperation(int operation, aida::burp::collaborator::status_t reviewed);
    void submitGenerateToken();
    void openReviewDialog(int operation);
    void updateEmptyState();

    CollaboratorInteractionsModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* table_stack_ = nullptr;
    widgets::AidaStateView* empty_view_ = nullptr;
    QFrame* status_card_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* listeners_label_ = nullptr;
    QLabel* public_label_ = nullptr;
    widgets::AidaButton* start_stop_button_ = nullptr;
    widgets::AidaButton* generate_button_ = nullptr;
    widgets::AidaButton* copy_domain_button_ = nullptr;
    widgets::AidaButton* clear_button_ = nullptr;
    QLabel* op_status_label_ = nullptr;
    widgets::AidaButton* retry_button_ = nullptr;
    QWidget* config_form_ = nullptr;
    QLineEdit* bind_ip_ = nullptr;
    QLineEdit* public_host_ = nullptr;
    QLineEdit* public_ip_ = nullptr;
    QCheckBox* enable_http_ = nullptr;
    QCheckBox* enable_dns_ = nullptr;
    QCheckBox* enable_smtp_ = nullptr;
    QSpinBox* http_port_ = nullptr;
    QSpinBox* dns_port_ = nullptr;
    QSpinBox* smtp_port_ = nullptr;
    QLineEdit* canned_body_ = nullptr;
    QLineEdit* canned_ct_ = nullptr;
    QComboBox* filter_kind_ = nullptr;
    QLineEdit* filter_token_ = nullptr;
    QLineEdit* filter_ip_ = nullptr;
    QLabel* token_label_ = nullptr;
    QLabel* detail_header_ = nullptr;
    QTableView* detail_table_ = nullptr;
    QPlainTextEdit* detail_raw_ = nullptr;
    class DetailModel;
    DetailModel* detail_model_ = nullptr;

    BurpOperationRunner* runner_ = nullptr;
    QTimer* refresh_timer_ = nullptr;

    std::atomic<bool> cache_refresh_pending_{false};
    std::atomic<std::uint64_t> cache_generation_{0};
    std::shared_ptr<const cache_publication_t> cache_publication_;
    std::shared_ptr<const std::pair<std::string, std::string>> generated_token_{
        std::make_shared<const std::pair<std::string, std::string>>()};
    std::string last_generated_token_;
    std::string last_generated_domain_;
    std::int64_t selected_id_ = -1;
    bool last_running_ = false;
};

}
