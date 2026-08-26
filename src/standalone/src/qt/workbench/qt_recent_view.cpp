#include "qt/workbench/qt_recent_view.hpp"

#include <QAbstractTableModel>
#include <QHeaderView>
#include <QTableView>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "core/session/analysis_session.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/context_menu_contract.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/explorer/exchange/open_dispatch.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::workbench {

namespace {

class QtRecentModel : public QAbstractTableModel {
public:
    struct row_t {
        QString name;
        QString path;
        bool open = false;
    };

    explicit QtRecentModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setRows(std::vector<row_t> rows) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }
    const row_t* rowAt(int row) const noexcept {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
            return nullptr;
        return &rows_[static_cast<std::size_t>(row)];
    }

    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : 2;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) return {};
        const auto* row = rowAt(index.row());
        if (!row) return {};
        if (role == Qt::DisplayRole) {
            return index.column() == 0 ? QVariant(row->name) : QVariant(row->path);
        }
        if (role == Qt::ToolTipRole) {
            return index.column() == 0
                ? QVariant(row->name == row->path
                      ? row->name : row->name + QStringLiteral("\n") + row->path)
                : QVariant(row->path);
        }
        if (role == Qt::UserRole) return row->open;
        return {};
    }
    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override {
        for (QModelRoleData& roleData : span) {
            if (roleData.role() == Qt::DisplayRole ||
                roleData.role() == Qt::ToolTipRole || roleData.role() == Qt::UserRole)
                roleData.setData(data(index, roleData.role()));
            else
                roleData.clearData();
        }
    }
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        return section == 0 ? QVariant(QStringLiteral("Name"))
            : QVariant(QStringLiteral("Path"));
    }

private:
    std::vector<row_t> rows_;
};

QString path_leaf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return QString::fromStdString(
        pos == std::string::npos ? path : path.substr(pos + 1));
}

}

QtRecentView::QtRecentView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.recent"));
    const auto& tokens = aida::qt::theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    model_ = new QtRecentModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.recent.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.recent.state_view"));
    state_view_->setState(widgets::AidaStateView::State::Empty);
    state_view_->setTitle(QStringLiteral("No recent binaries"));
    state_view_->setMessage(QStringLiteral(
        "Open a binary from Project Explorer. Open and closed sessions will remain discoverable here."));
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);

    connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        auto* model = static_cast<QtRecentModel*>(model_);
        const auto* row = model ? model->rowAt(index.row()) : nullptr;
        if (!row) return;
        if (row->open) {
            std::size_t index_of = 0;
            if (analysis_session::find_session_by_path(
                    row->path.toStdString(), &index_of))
                analysis_session::switch_session(index_of);
            return;
        }
        // Closed recent path: open through the documents open dispatch (05).
        aida::qt::explorer::open_path(row->path.toStdString());
    });
    connect(table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = table_->indexAt(pos);
        if (index.isValid())
            showRowMenu(table_->viewport()->mapToGlobal(pos), index.row());
    });
    reload();
}

void QtRecentView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    reload();
}

void QtRecentView::reload() {
    // Parse the recent list on demand, cached by string identity (07 sec. 8.5).
    const std::string json_text = g_sa_settings.recent_workspaces_json;
    cached_json_identity_ = json_text;
    std::vector<std::string> recent_paths;
    bool recent_parse_failed = false;
    if (!json_text.empty()) {
        const auto json = nlohmann::json::parse(json_text, nullptr, false);
        if (!json.is_discarded() && json.is_array()) {
            for (const auto& value : json)
                if (value.is_string())
                    recent_paths.push_back(value.get<std::string>());
        } else {
            recent_parse_failed = true;
        }
    }
    std::vector<QtRecentModel::row_t> rows;
    const std::size_t open_count = analysis_session::session_count();
    const std::size_t active = analysis_session::active_session_idx();
    (void)active;
    std::vector<std::string> open_paths;
    for (std::size_t index = 0; index < open_count; ++index) {
        const auto session = analysis_session::session_handle_at(index);
        if (!session) continue;
        QtRecentModel::row_t row;
        row.name = session->filename.empty()
            ? path_leaf(session->path)
            : QString::fromStdString(session->filename);
        row.path = QString::fromStdString(session->path);
        row.open = true;
        rows.push_back(row);
        open_paths.push_back(session->path);
    }
    for (const auto& path : recent_paths) {
        bool is_open = false;
        for (const auto& open : open_paths)
            if (open == path) { is_open = true; break; }
        if (is_open) continue;
        QtRecentModel::row_t row;
        row.name = path_leaf(path);
        row.path = QString::fromStdString(path);
        row.open = false;
        rows.push_back(row);
        if (rows.size() >= 10 + open_count) break;
    }
    static_cast<QtRecentModel*>(model_)->setRows(std::move(rows));
    const bool empty = model_->rowCount() == 0;
    if (empty && recent_parse_failed) {
        state_view_->setState(widgets::AidaStateView::State::Error);
        state_view_->setTitle(QStringLiteral("Recent list unavailable"));
        state_view_->setMessage(QStringLiteral(
            "The stored recent-workspaces list could not be parsed."));
    } else {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No recent binaries"));
        state_view_->setMessage(QStringLiteral(
            "Open a binary from Project Explorer. Open and closed sessions will remain discoverable here."));
    }
    state_view_->setVisible(empty);
    table_->setVisible(!empty);
}

void QtRecentView::showRowMenu(const QPoint& global_pos, int view_row) {
    auto* model = static_cast<QtRecentModel*>(model_);
    const auto* row = model ? model->rowAt(view_row) : nullptr;
    if (!row) return;
    analysis::QtAnalysisBridge::instance().showRecentMenu(row->path.toStdString(),
        row->open, aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
