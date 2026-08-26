#pragma once

#include <QAbstractTableModel>
#include <QString>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qt/analysis/qt_binary_map_types.hpp"

namespace aida::qt::analysis {

// Grouped binary-map list model (07 sec. 6.5): GroupHeader rows (Regions / Modules
// / Sections / Functions / Globals / imports::<dll>) plus entity rows. The
// visible flattened vector is rebuilt on filter/collapse changes; group
// headers paint full-width via the delegate role.
class QtBinaryMapListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class row_kind_t : std::uint8_t {
        group_header,
        region,
        module,
        section,
        function,
        global,
        import_dll,
        import_function,
        export_entry
    };

    struct list_row_t {
        row_kind_t kind = row_kind_t::group_header;
        std::string group_key;
        std::string title;
        int count = 0;
        bool collapsed = false;
        qt_binary_map_live_region_t region;
        driver_bridge::module_info_t module;
        aida::binary_map::map_section_t section;
        aida::binary_map::map_function_t function;
        aida::binary_map::map_global_t global;
        std::string dll;
        std::string function_name;
        std::string export_name;
        std::uint64_t export_va = 0;
    };

    explicit QtBinaryMapListModel(QObject* parent = nullptr);

    void rebuild(QtBinaryMapViewState& state,
                 std::shared_ptr<const aida::binary_map::map_t> map,
                 std::shared_ptr<const qt_binary_map_live_snapshot_t> live,
                 qt_binary_map_active_mode_t mode);
    const list_row_t* rowAt(int row) const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

private:
    std::vector<list_row_t> rows_;
};

}
