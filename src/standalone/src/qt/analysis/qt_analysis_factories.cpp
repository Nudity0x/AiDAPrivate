#include "helpers/diag_log.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/analysis/qt_analysis_list_view.hpp"
#include "qt/analysis/qt_binary_map_view.hpp"
#include "qt/analysis/qt_decrypt_oracle_view.hpp"
#include "qt/analysis/qt_functions_panel_widget.hpp"
#include "qt/analysis/qt_fuzzer_view.hpp"
#include "qt/analysis/qt_integrity_hunter_view.hpp"
#include "qt/analysis/qt_relationship_views.hpp"
#include "qt/analysis/qt_stealth_view.hpp"
#include "qt/analysis/qt_struct_dissector_view.hpp"
#include "qt/analysis/qt_struct_recon_view.hpp"
#include "qt/analysis/qt_types_catalog_view.hpp"
#include "qt/analysis/qt_xref_db_view.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/registry/qt_view_registry.hpp"
#include "qt/workbench/qt_recent_view.hpp"
#include "qt/workbench/qt_sessions_view.hpp"
#include "qt/workbench/qt_start_center_view.hpp"
#include "qt/workbench/qt_workbench_diff_view.hpp"
#include "qt/workbench/qt_workbench_inspector_view.hpp"
#include "qt/workbench/qt_workbench_navigator_view.hpp"

namespace aida::qt::analysis {

void register_analysis_view_factories(docking::AidaDockHost* host) {
    if (!host) return;
    using registry::stable_view_id_t;
    const auto install = [host](const char* id, registry::qt_view_factory_t factory) {
        const auto result = host->install_view_factory(stable_view_id_t(id),
            std::move(factory));
        if (!result.ok())
            diag::log_tagged_fmt("qt_analysis",
                "view_factory_install_failed view=%s status=%d detail=%s",
                id, static_cast<int>(result.status), result.detail.c_str());
    };

    // Analysis list views (one factory per domain).
    static const struct { const char* id; analysis_list_domain_t domain; }
        k_list_views[] = {
        {"view.analysis.imports", analysis_list_domain_t::imports},
        {"view.analysis.exports", analysis_list_domain_t::exports},
        {"view.analysis.names", analysis_list_domain_t::names},
        {"view.analysis.strings", analysis_list_domain_t::strings},
        {"view.analysis.segments", analysis_list_domain_t::segments},
        {"view.analysis.local_types", analysis_list_domain_t::local_types},
    };
    for (const auto& entry : k_list_views) {
        install(entry.id, [domain = entry.domain](QWidget* parent,
                const registry::view_instance_id_t&) -> QWidget* {
            return new QtAnalysisListView(domain, parent);
        });
    }
    install("view.analysis.functions", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new QtFunctionsPanelWidget(parent);
    });
    install("view.analysis.segment_registers", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new QtSegmentRegistersView(parent);
    });
    install("view.analysis.proximity", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        auto* view = new QtProximityBrowserView(parent);
        QtAnalysisBridge::instance().registerProximityView(view);
        return view;
    });
    install("view.analysis.references", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        auto* view = new QtXrefDbView(parent);
        QtAnalysisBridge::instance().registerXrefView(view);
        return view;
    });
    install("view.analysis.binary_map", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new QtBinaryMapView(parent);
    });

    // Analysis hub members (symbolic/taint/deobfuscation pages 0-2 stay with
    // the emulation planner; pages 3-4 are this domain's fuzzer + stealth).
    install("view.analysis.fuzzer", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new QtFuzzerView(parent);
    });
    install("view.analysis.protection", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new QtStealthView(parent);
    });

    // Types hub members.
    static const struct { const char* id; qt_types_tab_t tab; } k_type_tabs[] = {
        {"view.types.structures", qt_types_tab_t::structures},
        {"view.types.unions", qt_types_tab_t::unions_},
        {"view.types.enums", qt_types_tab_t::enums},
        {"view.types.typedefs", qt_types_tab_t::typedefs},
        {"view.types.functions", qt_types_tab_t::functions},
    };
    for (const auto& entry : k_type_tabs) {
        install(entry.id, [tab = entry.tab](QWidget* parent,
                const registry::view_instance_id_t&) -> QWidget* {
            return new QtTypesCatalogView(tab, parent);
        });
    }
    const auto recon_factory = [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        auto* view = new QtStructReconView(parent);
        QtAnalysisBridge::instance().registerReconView(view);
        return view;
    };
    install("view.types.inferred", recon_factory);
    install("view.types.struct_recon", recon_factory);
    install("view.types.dissector", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new QtStructDissectorView(parent);
    });

    // Scan hub members owned by this domain (07 sec. 0.1: decrypt oracle +
    // integrity hunter). The scan hub container belongs to 09.
    install("view.memory.decrypt", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new QtDecryptOracleView(parent);
    });
    install("view.memory.integrity", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new QtIntegrityHunterView(parent);
    });

    // Workbench domain.
    install("view.navigator", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new workbench::QtWorkbenchNavigatorView(parent);
    });
    install("view.inspector", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new workbench::QtWorkbenchInspectorView(parent);
    });
    install("document.diff", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new workbench::QtWorkbenchDiffView(parent);
    });
    install("view.sessions", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new workbench::QtSessionsView(parent);
    });
    install("view.recent", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new workbench::QtRecentView(parent);
    });
    install("view.start_center", [](QWidget* parent,
            const registry::view_instance_id_t&) -> QWidget* {
        return new workbench::QtStartCenterView(parent);
    });

    diag::log_tagged("qt_analysis", "analysis_view_factories_registered");
}

}
