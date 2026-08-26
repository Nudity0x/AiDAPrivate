#pragma once

#include <QAbstractListModel>
#include <QAbstractTableModel>
#include <QWidget>

#include <QString>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ai/agent_manager_service.hpp"
#include "core/infra/event_bus.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListView;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QSplitter;
class QTableView;
class QTimer;
class QVBoxLayout;

namespace aida::qt::widgets {
class AidaSectionHeader;
}

namespace aida::qt::ai {

class AidaAgentListModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AidaAgentListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void setFilter(const QString& filter);
    void reloadFrom(const aida::agent_manager_service::snapshot_ptr& publication);

    QString nameAt(int row) const;
    const aida::agent::agent_info_t* agentAt(int row) const;

Q_SIGNALS:
    void modelReplaced();

private:
    struct row_t {
        std::string name;
        std::string description;
        bool native = true;
        bool hidden = false;
    };

    std::vector<row_t> rows_;
    QString filter_;
    aida::agent_manager_service::snapshot_ptr publication_;
};

class AidaAgentRulesModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit AidaAgentRulesModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void setRules(const aida::agent::ruleset_t& rules);
    aida::agent::ruleset_t rules() const;
    void appendRule(const std::string& key, const std::string& pattern,
                    aida::agent::permission_rule_t::action_t action);
    void removeRule(int row);

Q_SIGNALS:
    void rulesEdited();

private:
    struct rule_row_t {
        QString key;
        QString pattern;
        int action = 2;
    };

    std::vector<rule_row_t> rows_;
};

class AidaAgentManagerView : public QWidget {
    Q_OBJECT
public:
    explicit AidaAgentManagerView(QWidget* parent = nullptr);
    ~AidaAgentManagerView() override;

    void selectAgent(const QString& name);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void wireService();
    void pollService();
    void applyPublication(const aida::agent_manager_service::snapshot_ptr& publication);
    void loadEditorsForSelection();
    aida::agent::agent_info_t buildInfoFromEditors(bool keep_native) const;
    void markDirty();
    void setEditorsReadOnly(bool read_only);
    void openContextMenu(const QModelIndex& index, const QPoint& global_pos);
    void onSave();
    void onReset();
    void onDelete();
    void onDuplicateAsCustom();
    void rebuildToolChips();
    void updateNativeBanner();
    void refreshDetailHeader();

    AidaAgentListModel* list_model_ = nullptr;
    AidaAgentRulesModel* rules_model_ = nullptr;
    QListView* list_ = nullptr;
    QLineEdit* filter_edit_ = nullptr;
    QPushButton* reload_button_ = nullptr;
    QLabel* status_label_ = nullptr;
    QSplitter* splitter_ = nullptr;

    QLabel* detail_title_ = nullptr;
    QLabel* detail_badges_ = nullptr;
    QLabel* native_banner_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QPlainTextEdit* description_edit_ = nullptr;
    QLineEdit* color_edit_ = nullptr;
    QPushButton* color_swatch_ = nullptr;
    QComboBox* mode_combo_ = nullptr;
    QPlainTextEdit* system_prompt_edit_ = nullptr;
    QComboBox* provider_combo_ = nullptr;
    QComboBox* model_combo_ = nullptr;
    QDoubleSpinBox* temperature_spin_ = nullptr;
    QDoubleSpinBox* top_p_spin_ = nullptr;
    QSpinBox* max_steps_spin_ = nullptr;
    QTableView* rules_table_ = nullptr;
    QLineEdit* rule_key_edit_ = nullptr;
    QLineEdit* rule_pattern_edit_ = nullptr;
    QComboBox* rule_action_combo_ = nullptr;
    QWidget* tools_allowed_host_ = nullptr;
    QWidget* tools_denied_host_ = nullptr;
    QVBoxLayout* tools_allowed_layout_ = nullptr;
    QVBoxLayout* tools_denied_layout_ = nullptr;
    QLineEdit* new_allowed_edit_ = nullptr;
    QLineEdit* new_denied_edit_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* reset_button_ = nullptr;
    QPushButton* delete_button_ = nullptr;
    QPushButton* duplicate_button_ = nullptr;
    QScrollArea* editor_scroll_ = nullptr;
    QWidget* editor_root_ = nullptr;
    QTimer* poll_timer_ = nullptr;

    std::string selected_name_;
    std::string err_;
    std::vector<std::string> tools_allowed_;
    std::vector<std::string> tools_denied_;
    nlohmann::json preserved_options_ = nlohmann::json::object();
    bool loading_editors_ = false;
    bool dirty_ = false;
    std::uint64_t observed_service_generation_ = 0;
    std::string pending_selection_;
    aida::events::subscription_handle_t sub_changed_;
};

}
