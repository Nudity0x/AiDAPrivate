#pragma once

#include <QAbstractTableModel>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aida::qt::analysis {

// Ported from functions_panel.hpp (view-model types).
struct qt_function_entry_t {
    std::uint64_t address = 0;
    std::uint32_t size = 0;
    std::string name;
    std::string section;
    std::uint32_t calls_in = 0;
    std::uint32_t calls_out = 0;
    bool synthetic_name = true;
};

struct qt_functions_presentation_t {
    std::shared_ptr<const std::vector<qt_function_entry_t>> entries;
    std::vector<int> sorted_indices;
    std::unordered_map<std::uint64_t, std::size_t> row_by_address;
};

class QtFunctionsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Column : int {
        name = 0,
        size = 1,
        section = 2,
        calls = 3,
        column_count = 4
    };

    explicit QtFunctionsModel(QObject* parent = nullptr);

    void setPresentation(std::shared_ptr<const qt_functions_presentation_t> presentation);
    const qt_functions_presentation_t* presentation() const noexcept {
        return presentation_.get();
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const qt_function_entry_t* entryAt(int view_row) const noexcept;
    int viewRowForAddress(std::uint64_t address) const noexcept;

private:
    std::shared_ptr<const qt_functions_presentation_t> presentation_ =
        std::make_shared<const qt_functions_presentation_t>();
};

// Per-binary functions-panel state (07 sec. 4.3); replaces the
// workspace_states() map and the thread_local render_workspace() slot.
struct QtFunctionsPanelState {
    std::mutex mtx;
    std::shared_ptr<const std::vector<qt_function_entry_t>> entries =
        std::make_shared<const std::vector<qt_function_entry_t>>();
    std::shared_ptr<const qt_functions_presentation_t> presentation =
        std::make_shared<const qt_functions_presentation_t>();
    std::atomic<bool> ready{false};
    std::atomic<bool> building{false};
    std::uint64_t cached_module_base = 0;
    std::uint32_t cached_module_size = 0;
    std::string cached_module_name;
    std::uint64_t cached_generation = 0;
    std::uint64_t cached_analysis_revision = 0;
    std::uint64_t cached_overlay_revision = 0;
    std::uint64_t cached_symbol_revision = 0;
    QString filter;
    std::string last_filter_lower;
    bool filter_dirty = true;
    int selected_row = -1;
    std::uint64_t selected_addr = 0;
    int ctx_row = -1;
    std::uint64_t ctx_addr = 0;
    int sort_column = 0;
    bool sort_ascending = true;
    bool sort_dirty = false;
    std::atomic<std::uint64_t> filter_sort_serial{0};
    std::atomic<bool> filter_sort_building{false};
};

}
