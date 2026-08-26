#pragma once

#include "core/disasm/cfg_view.hpp"
#include "core/ui/analysis_context_menu.hpp"
#include "qt/graph/cfg_block_item.hpp"
#include "qt/graph/cfg_edge_layer_item.hpp"

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRectF>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

class QGraphicsScene;

namespace aida::qt::graph {

class CfgSceneController : public QObject {
    Q_OBJECT
public:
    explicit CfgSceneController(QObject* parent = nullptr);
    ~CfgSceneController() override;

    QGraphicsScene* scene() const noexcept { return scene_; }
    CfgEdgeLayerItem* edge_layer() const noexcept { return edge_layer_; }

    bool live() const noexcept { return live_; }

    void setLiveContext(const disasm_view::workspace_context_t& context);
    void applyLiveModel(const std::shared_ptr<const cfg_view::cfg_model_snapshot_t>& model);
    void clearLive();

    void setWorkspaceContext(const disasm_view::workspace_context_t& context);
    std::shared_ptr<cfg_view::workspace_graph_view_state_t> workspaceState() const;
    void rebuildWorkspaceLayout(const aida::analysis::function_record_t& function,
                                std::size_t page_begin, std::size_t page_end);
    void mirrorWorkspaceSelection();

    void blockClicked(CfgBlockItem* item, int line, bool shift);
    void blockDoubleClicked(CfgBlockItem* item, int line);
    void blockContextMenu(CfgBlockItem* item, int line, const QPoint& screen_pos);

    aida::ui::analysis_context_menu::context_t buildLiveMenu(CfgBlockItem* item);
    aida::ui::analysis_context_menu::context_t buildWorkspaceMenu(CfgBlockItem* item);
    aida::ui::analysis_context_menu::context_t buildWorkspaceCanvasMenu();
    CfgBlockItem* selectedItem() const;

    void clearTextSelection();
    QString selectedText(bool include_address) const;
    bool hasTextSelection() const;

    QRectF worldBounds() const;
    QRectF blockSceneRect(int node_id) const;
    std::optional<int> nodeForAddress(std::uint64_t address) const;

    std::uint64_t contentGeneration() const noexcept { return content_generation_; }
    std::uint64_t lastCursorAddress() const noexcept { return last_cursor_addr_; }
    void setLastCursorAddress(std::uint64_t address) noexcept { last_cursor_addr_ = address; }

    void persistMutation(const std::function<void()>& mutator);

Q_SIGNALS:
    void contentChanged();
    void selectionChanged();
    void navigateToDisassembly(quint64 address);
    void fitRequested();
    void zoomStepRequested(qreal factor);
    void resetRequested();

private:
    void assembleLive(const cfg_view::cfg_model_snapshot_t& model);
    void assembleWorkspace();
    cfg_block_data_t makeLiveBlockData(const cfg_view::cfg_model_snapshot_t& model,
                                       std::size_t node_index);
    cfg_block_data_t makeWorkspaceBlockData(std::size_t node_index);
    std::string buildSelectionText(bool include_address) const;

    QGraphicsScene* scene_ = nullptr;
    CfgEdgeLayerItem* edge_layer_ = nullptr;
    std::unordered_map<int, CfgBlockItem*> items_;
    bool live_ = true;
    disasm_view::workspace_context_t context_;
    std::shared_ptr<const cfg_view::cfg_model_snapshot_t> live_model_;
    std::uint64_t displayed_model_generation_ = 0;
    std::shared_ptr<cfg_view::workspace_graph_view_state_t> workspace_state_;
    std::optional<int> selected_node_;
    std::optional<aida::analysis::entity_id_t> selected_block_entity_;
    std::optional<aida::analysis::entity_id_t> selected_instruction_entity_;
    std::uint64_t selected_address_ = 0;
    CfgBlockItem* text_sel_item_ = nullptr;
    std::uint64_t last_cursor_addr_ = 0;
    std::uint64_t content_generation_ = 0;
};

}
