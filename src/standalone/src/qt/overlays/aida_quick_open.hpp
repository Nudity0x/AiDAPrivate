#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/analysis/code_index.hpp"
#include "core/editor/programming_language_service.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QElapsedTimer;
class QLabel;
class QLineEdit;
class QListView;
class QStackedLayout;
class QTimer;

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::widgets {
class AidaProgressBar;
}

namespace aida::qt::overlays {

class AidaEmptyState;

namespace quick_open_detail {

constexpr std::size_t k_maximum_results = 128;
constexpr std::size_t k_candidates_per_frame = 4096;
constexpr std::size_t k_maximum_mru_workspaces = 16;
constexpr std::size_t k_maximum_mru_files = 32;
constexpr std::size_t k_maximum_path_bytes = 4096;

enum class result_kind_t : std::uint8_t {
    file,
    symbol,
    view,
    command
};

struct result_t {
    result_kind_t kind = result_kind_t::file;
    std::string identity;
    std::string label;
    std::string detail;
    std::string relative_path;
    std::string target_id;
    int line = 1;
    int column = 1;
    int score = 0;
    std::uint64_t index_generation = 0;
    std::string root_path;
    bool enabled = true;
    std::string disabled_reason;
};

struct mru_workspace_t {
    std::string root;
    std::vector<std::string> files;
};

struct search_state_t {
    std::shared_ptr<const code_index::published_index_t> index;
    std::vector<result_t> results;
    std::vector<mru_workspace_t> mru;
    std::string query;
    std::string selected_identity;
    std::string status;
    std::string error;
    std::size_t file_cursor = 0;
    std::size_t symbol_cursor = 0;
    std::size_t scanned_candidates = 0;
    std::size_t total_candidates = 0;
    bool auxiliary_complete = false;
    bool result_limit_reached = false;
};

std::string lower_ascii(std::string value);
bool normalized_relative_path(std::string_view value, std::string& output);
bool normalized_root_path(std::string_view value, std::string& output);
std::vector<mru_workspace_t> load_mru();
bool persist_mru(search_state_t& state, std::string_view root, std::string_view relative);
int mru_bonus(const search_state_t& state, std::string_view root, std::string_view relative);
int fuzzy_score(std::string_view query, std::string_view label, std::string_view detail);
int kind_bonus(result_kind_t kind) noexcept;
void consider(search_state_t& state, result_t result);
std::vector<result_t> ordered_results(const search_state_t& state);
const char* kind_label(result_kind_t kind) noexcept;
void reset_search(search_state_t& state, std::string query,
                  std::shared_ptr<const code_index::published_index_t> index);
bool scan_complete(const search_state_t& state) noexcept;

}

class AidaQuickOpenModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        IdentityRole = Qt::UserRole,
        DetailRole,
        KindRole,
        EnabledRole,
        DisabledReasonRole,
        ScoreRole
    };

    explicit AidaQuickOpenModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void setResults(std::vector<quick_open_detail::result_t> results);
    const quick_open_detail::result_t* resultAt(int row) const;
    int rowForIdentity(const std::string& identity) const;

private:
    std::vector<quick_open_detail::result_t> results_;
};

class AidaQuickOpenDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit AidaQuickOpenDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

class AidaQuickOpenDialog : public bridge::AidaDialog {
    Q_OBJECT
public:
    AidaQuickOpenDialog(docking::AidaDockHost* host, QWidget* parent = nullptr);
    ~AidaQuickOpenDialog() override;

    void openFresh();
    void setQueryText(const QString& text);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void onQueryChanged(const QString& text);
    void onScanChunk();
    void refreshModel();
    void activate(const quick_open_detail::result_t& result, bool open_to_side);
    void activateCurrent(bool open_to_side);
    void moveSelection(int delta);
    void updateStatusLine();
    void updateEmptyState();
    bool sourceChanged() const;
    void centerOnHost();

    docking::AidaDockHost* host_ = nullptr;
    QLineEdit* query_edit_ = nullptr;
    QListView* results_view_ = nullptr;
    AidaQuickOpenModel* model_ = nullptr;
    AidaQuickOpenDelegate* delegate_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* error_label_ = nullptr;
    widgets::AidaProgressBar* progress_ = nullptr;
    QLabel* footer_label_ = nullptr;
    QStackedLayout* results_stack_ = nullptr;
    AidaEmptyState* empty_state_ = nullptr;
    QTimer* scan_timer_ = nullptr;
    quick_open_detail::search_state_t state_;
    std::string selected_identity_;
};

class AidaQuickOpenController : public QObject {
    Q_OBJECT
public:
    AidaQuickOpenController(docking::AidaDockHost* host, QWidget* dialog_parent,
                            QObject* parent = nullptr);
    ~AidaQuickOpenController() override;

    void open();
    bool isOpen() const;

private:
    void pollLegacyRequest();

    docking::AidaDockHost* host_ = nullptr;
    QWidget* dialog_parent_ = nullptr;
    QPointer<AidaQuickOpenDialog> dialog_;
    QTimer* legacy_poll_ = nullptr;
};

}
