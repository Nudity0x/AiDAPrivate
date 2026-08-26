#pragma once

#include <QAbstractTableModel>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/analysis/pdb_parser.hpp"
#include "core/analysis/workspace/analysis_workspace.hpp"

namespace disasm_view {
struct workspace_context_t;
}

namespace aida::qt::analysis {

// Types hub sub-tabs (verbatim from types_hub_view_api.hpp sub_tab_t).
enum class qt_types_tab_t : int {
    structures = 0,
    unions_ = 1,
    enums = 2,
    typedefs = 3,
    functions = 4,
    inferred = 5,
    dissector = 6,
    count = 7
};

struct qt_type_reference_t {
    aida::analysis::address_t address;
    std::string label;
};

struct qt_struct_entry_t {
    std::string module;
    pdb_parser::struct_def_t definition;
};

struct qt_enum_entry_t {
    std::string module;
    pdb_parser::enum_def_t definition;
};

struct qt_function_type_entry_t {
    aida::analysis::address_t address;
    std::string name;
    std::string signature;
    std::uint64_t size = 0;
    std::string provenance;
};

struct qt_typedef_entry_t {
    aida::analysis::address_t address;
    std::string name;
    std::string canonical_type;
    bool explicitly_unknown = false;
    std::uint8_t confidence = 0;
};

struct qt_type_catalog_t {
    std::vector<qt_struct_entry_t> structs;
    std::vector<qt_struct_entry_t> unions;
    std::vector<qt_enum_entry_t> enums;
    std::vector<qt_function_type_entry_t> functions;
    std::vector<qt_typedef_entry_t> typedefs;
};

// Per-binary types-hub state (07 sec. 1.3/sec. 6.1); replaces types_hub_view::state_registry().
struct QtTypesHubState {
    std::mutex mutex;
    qt_types_tab_t active = qt_types_tab_t::structures;
    QString search;
    QString apply_address;
    QString apply_type;
    int selected = -1;
    std::string pdb_error;
    std::shared_ptr<const qt_type_catalog_t> catalog;
    std::atomic<bool> catalog_loading{false};
    std::uint64_t catalog_generation = 0;
    std::uint64_t catalog_analysis_revision = 0;
    std::shared_ptr<const std::vector<std::size_t>> visible_indices;
    const qt_type_catalog_t* visible_catalog = nullptr;
    qt_types_tab_t visible_tab = qt_types_tab_t::structures;
    std::string visible_filter;
    std::atomic<bool> visible_loading{false};
    bool list_focused = false;
    int context_row = -1;
    qt_types_tab_t context_tab = qt_types_tab_t::structures;
    std::uint64_t context_generation = 0;
    std::uint64_t context_analysis_revision = 0;
    std::string apply_status;
    bool apply_error = false;
    bool apply_pending = false;
    std::uint64_t apply_generation = 0;
    std::uint64_t apply_expected_overlay_revision = 0;
    bool declaration_review_requested = false;
    std::string declaration_review_name;
    std::shared_ptr<const std::string> declaration_review_text;
    std::weak_ptr<aida::analysis::analysis_workspace_t> declaration_review_workspace;
    std::shared_ptr<const aida::analysis::analysis_publication_t>
        declaration_review_publication;
    std::string declaration_review_workspace_id;
    std::uint64_t declaration_review_generation = 0;
    std::uint64_t declaration_review_analysis_revision = 0;
    std::uint64_t declaration_review_overlay_revision = 0;
    std::uint64_t enum_declaration_cache_generation = 0;
    std::uint64_t enum_declaration_cache_analysis_revision = 0;
    std::string enum_declaration_cache_identity;
    std::shared_ptr<const std::string> enum_declaration_cache;
    const qt_type_catalog_t* reference_catalog = nullptr;
    std::string reference_type;
    std::shared_ptr<const std::vector<qt_type_reference_t>> references;
    std::atomic<bool> reference_loading{false};
    const qt_type_catalog_t* reference_request_catalog = nullptr;
    std::string reference_request_type;
};

// Flat row projection for the five catalog tabs (07 sec. 6.1). One model class;
// the tab config selects columns.
class QtTypesCatalogModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit QtTypesCatalogModel(QObject* parent = nullptr);

    void setTab(qt_types_tab_t tab);
    qt_types_tab_t tab() const noexcept { return tab_; }
    void setContent(std::shared_ptr<const qt_type_catalog_t> catalog,
                    std::shared_ptr<const std::vector<std::size_t>> visible);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    // Stable identity of the entry behind a view row (for retained menus).
    std::string entityIdAt(int view_row) const;
    std::string nameAt(int view_row) const;
    bool addressAt(int view_row, aida::analysis::address_t& out) const;

private:
    qt_types_tab_t tab_ = qt_types_tab_t::structures;
    std::shared_ptr<const qt_type_catalog_t> catalog_;
    std::shared_ptr<const std::vector<std::size_t>> visible_;
};

// Engine-adjacent builders (worker stage; verbatim logic from types_hub_view).
qt_type_catalog_t qt_build_type_catalog(const disasm_view::workspace_context_t& context);
std::vector<std::size_t> qt_filter_type_catalog(const qt_type_catalog_t& catalog,
    qt_types_tab_t tab, const std::string& filter);
std::vector<qt_type_reference_t> qt_type_references_for(
    const qt_type_catalog_t& catalog, const std::string& type_name);
std::string qt_struct_to_ida_syntax(const pdb_parser::struct_def_t& definition);
std::string qt_struct_to_c_bounded(const pdb_parser::struct_def_t& definition);
std::string qt_enum_to_c(const pdb_parser::enum_def_t& definition);
std::string qt_canonical_record_name(std::string name);
const pdb_parser::struct_def_t* qt_catalog_record(const qt_type_catalog_t& catalog,
                                                  const std::string& name);
const pdb_parser::enum_def_t* qt_catalog_enum(const qt_type_catalog_t& catalog,
                                              const std::string& name);

}
