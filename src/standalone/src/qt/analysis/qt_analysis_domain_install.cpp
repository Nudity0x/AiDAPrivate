#include "qt/analysis/qt_analysis_bridge.hpp"

#include "helpers/diag_log.hpp"

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/disasm/disasm_view.hpp"

#include "qt/analysis/qt_analysis_host_hooks.hpp"
#include "qt/analysis/qt_relationship_views.hpp"
#include "qt/analysis/qt_source_reconstruct_dialog.hpp"
#include "qt/analysis/qt_struct_dissector_view.hpp"
#include "qt/analysis/qt_struct_recon_view.hpp"
#include "qt/analysis/qt_types_catalog_model.hpp"
#include "qt/analysis/qt_workspace_context.hpp"
#include "qt/analysis/qt_xref_db_view.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/registry/qt_view_registry.hpp"

#include "core/analysis/struct_recon_engine.hpp"

namespace aida::qt::analysis {

void register_analysis_view_factories(docking::AidaDockHost* host);

// View factory implementations live with their widgets; the install table
// references them here (07 sec. 12 phase 1 + the domain composition root).
void QtAnalysisBridge::installViewFactories() {
    register_analysis_view_factories(host_);
}

void QtAnalysisBridge::installEngineHooks() {
    analysis_host_hooks_t hooks;

    hooks.stage_type_application =
        [this](const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
               std::uint64_t runtime_address, std::string& error) -> bool {
        // Ports types_hub_view::stage_type_application: stage into the types
        // hub state, focus the Structures page.
        auto* context = contextFor(workspace);
        if (!context) {
            error = "The selected address is not mapped in the active type workspace.";
            return false;
        }
        {
            auto& state = *context->typesHubState;
            std::lock_guard<std::mutex> lock(state.mutex);
            char formatted[32]{};
            std::snprintf(formatted, sizeof(formatted), "0x%llX",
                static_cast<unsigned long long>(runtime_address));
            state.apply_address = QString::fromLatin1(formatted);
            state.apply_type.clear();
            state.apply_status =
                "Enter a canonical type and review Apply before changing the overlay.";
            state.apply_error = false;
        }
        openView("view.types.structures");
        error.clear();
        return true;
    };

    hooks.activate_types_subview = [this](int subview) {
        static const char* k_ids[] = {
            "view.types.structures", "view.types.unions", "view.types.enums",
            "view.types.typedefs", "view.types.functions", "view.types.inferred",
            "view.types.dissector"
        };
        if (subview < 0 || subview > 6 || !host_) return;
        openView(k_ids[subview]);
        if (host_->hub_widget(registry::hub_kind_t::types))
            host_->activate_hub_subview(registry::hub_kind_t::types, subview);
    };

    hooks.open_source_reconstruction =
        [this](const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace) {
        if (!workspace || !host_) return;
        QtSourceReconstructDialog::openFor(workspace, nullptr);
    };

    hooks.submit_xref_query =
        [this](const std::shared_ptr<aida::analysis::analysis_workspace_t>& workspace,
               std::uint64_t runtime_address, bool query_to,
               std::string& error) -> bool {
        auto* context = contextFor(workspace);
        if (!context) {
            error = "The workspace is unavailable";
            return false;
        }
        // Stage the query; the References view submits it on show.
        {
            auto& state = *context->xrefState;
            std::lock_guard<std::mutex> lock(state.mutex);
            char formatted[32]{};
            std::snprintf(formatted, sizeof(formatted), "0x%llX",
                static_cast<unsigned long long>(runtime_address));
            state.address = QString::fromLatin1(formatted);
            state.query_to = query_to;
        }
        openView("view.analysis.references");
        if (auto* view = qobject_cast<QtXrefDbView*>(xref_view_.data()))
            return view->queryAddress(runtime_address, query_to);
        return true;
    };

    hooks.proximity_drill_selected = []() -> bool {
        auto* view = qobject_cast<QtProximityBrowserView*>(
            instance().proximity_view_.data());
        return view && view->drillSelected();
    };

    hooks.proximity_drill_capability = [](std::string& reason) -> bool {
        auto* view = qobject_cast<QtProximityBrowserView*>(
            instance().proximity_view_.data());
        if (!view) {
            reason = "Select a Proximity Browser node first";
            return false;
        }
        if (view->drillCapable()) return true;
        reason = "Select a Proximity Browser node first";
        return false;
    };

    hooks.recon_has_current_structure = []() -> bool {
        auto* view = qobject_cast<QtStructReconView*>(
            instance().recon_view_.data());
        if (view) return view->hasCurrentStructure();
        // Engine-level fallback (works without the view being created).
        const auto structure = struct_recon::capture_current_snapshot();
        return structure && !structure->fields.empty();
    };

    hooks.recon_copy_declaration = [](std::string& detail) -> bool {
        auto* view = qobject_cast<QtStructReconView*>(
            instance().recon_view_.data());
        if (view) return view->copyCurrentDeclaration(detail);
        const auto structure = struct_recon::capture_current_snapshot();
        if (!structure || structure->fields.empty()) {
            detail = "Reconstruct or load a structure first";
            return false;
        }
        const std::string declaration = struct_recon::export_as_cpp(*structure);
        if (declaration.empty() || declaration.size() > 64U * 1024U) {
            detail = "The reconstruction declaration is empty or exceeds 64 KiB";
            return false;
        }
        clipboard::set_text(QString::fromStdString(declaration));
        detail = "Generated C++ declaration copied";
        return true;
    };

    hooks.recon_declare_and_apply = [this](std::string& detail) -> bool {
        auto* view = qobject_cast<QtStructReconView*>(
            recon_view_.data());
        if (!view) {
            openView("view.types.struct_recon");
            view = qobject_cast<QtStructReconView*>(recon_view_.data());
        }
        if (!view) {
            detail = "The reconstruction view is unavailable";
            return false;
        }
        return view->declareAndApplyCurrent(detail);
    };

    hooks.stage_dissector_target =
        [this](staged_dissector_target_t context, std::string& error) -> bool {
        // Route to the live dissector view; the store survives view recreation.
        auto* view = QtStructDissectorView::activeInstance();
        if (view)
            return view->stageTarget(std::move(context), error);
        // No live dissector yet: stage into the domain store; the view adopts
        // it on construction.
        staged_dissector_target_store().context = std::move(context);
        staged_dissector_target_store().status =
            "Review the retained source identity before using this base address.";
        staged_dissector_target_store().stale = false;
        error.clear();
        return true;
    };

    install_analysis_host_hooks(std::move(hooks));
}

}
