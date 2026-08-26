#include "qt/graph/cfg_scene_controller.hpp"

#include "qt/analysis/qt_analysis_host_hooks.hpp"
#include "qt/analysis_bridge/disasm_workspace_model.hpp"
#include "qt/analysis_bridge/revision_combine.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/theme/disasm_theme_tokens.hpp"

#include "core/analysis/workspace/overlay_journal.hpp"
#include "core/debugger/debugger_interaction_context.hpp"
#include "core/debugger/debugger_view.hpp"
#include "core/disasm/pseudocode_view.hpp"
#include "core/ai/standalone_chat.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/ui/analysis_context_menu.hpp"
#include "helpers/diag_log.hpp"

#include <QGraphicsScene>

#include <algorithm>
#include <cmath>

namespace aida::qt::graph {

namespace {

std::uint64_t workspace_generation_of(const disasm_view::workspace_context_t& context)
{
    if (!context.workspace)
        return 0;
    return aida::analysis_bridge::combine_generation_revision(context.workspace->generation(),
        context.workspace->analysis_revision());
}

}

CfgSceneController::CfgSceneController(QObject* parent) : QObject(parent)
{
    scene_ = new QGraphicsScene(this);
    scene_->setItemIndexMethod(QGraphicsScene::BspTreeIndex);
    edge_layer_ = new CfgEdgeLayerItem(cfg_edge_routing_t::live_cubic);
    scene_->addItem(edge_layer_);
}

CfgSceneController::~CfgSceneController() = default;

void CfgSceneController::setLiveContext(const disasm_view::workspace_context_t& context)
{
    live_ = true;
    context_ = context;
    edge_layer_->setZValue(0.0);
}

void CfgSceneController::setWorkspaceContext(
    const disasm_view::workspace_context_t& context)
{
    live_ = false;
    context_ = context;
    workspace_state_ = context ? cfg_view::workspace_graph_state(context) : nullptr;
    edge_layer_->setZValue(0.0);
}

std::shared_ptr<cfg_view::workspace_graph_view_state_t>
CfgSceneController::workspaceState() const
{
    return workspace_state_;
}

void CfgSceneController::clearLive()
{
    live_model_.reset();
    displayed_model_generation_ = 0;
    selected_node_.reset();
    text_sel_item_ = nullptr;
    for (auto& item : items_)
        delete item.second;
    items_.clear();
    edge_layer_->clear_edges();
    Q_EMIT contentChanged();
}

void CfgSceneController::applyLiveModel(
    const std::shared_ptr<const cfg_view::cfg_model_snapshot_t>& model)
{
    if (!model) {
        clearLive();
        return;
    }
    if (model->generation == displayed_model_generation_ && live_model_ == model)
        return;
    live_model_ = model;
    displayed_model_generation_ = model->generation;
    selected_node_.reset();
    text_sel_item_ = nullptr;
    last_cursor_addr_ = model->entry_addr;
    assembleLive(*model);
    Q_EMIT contentChanged();
    Q_EMIT fitRequested();
}

void CfgSceneController::assembleLive(const cfg_view::cfg_model_snapshot_t& model)
{
    for (auto& item : items_)
        delete item.second;
    items_.clear();
    edge_layer_->clear_edges();
    const auto& nodes = model.graph.nodes;
    const auto& edges = model.graph.edges;
    const auto& blocks = model.blocks;
    for (std::size_t node_index = 0; node_index < nodes.size(); ++node_index) {
        const auto& node = nodes[node_index];
        if (node.id < 0 || static_cast<std::size_t>(node.id) >= blocks.size())
            continue;
        auto* item = new CfgBlockItem(makeLiveBlockData(model, node_index), this);
        item->startEntrance();
        scene_->addItem(item);
        items_[node.id] = item;
    }
    std::vector<cfg_edge_data_t> edge_data;
    edge_data.reserve(edges.size());
    for (const auto& edge : edges) {
        const auto from_found = model.node_lookup.find(edge.from);
        const auto to_found = model.node_lookup.find(edge.to);
        if (from_found == model.node_lookup.end() || to_found == model.node_lookup.end())
            continue;
        const auto& from = nodes[from_found->second];
        const auto& to = nodes[to_found->second];
        cfg_edge_data_t data;
        data.from_rect = QRectF(from.x - from.width * 0.5, from.y, from.width,
            from.height);
        data.to_rect = QRectF(to.x - to.width * 0.5, to.y, to.width, to.height);
        data.true_branch = edge.is_true_branch;
        data.branching = edge.from >= 0 &&
            static_cast<std::size_t>(edge.from) < blocks.size() &&
            blocks[static_cast<std::size_t>(edge.from)].successors.size() > 1;
        edge_data.push_back(data);
    }
    edge_layer_->set_edges(std::move(edge_data), worldBounds());
}

cfg_block_data_t CfgSceneController::makeLiveBlockData(
    const cfg_view::cfg_model_snapshot_t& model, std::size_t node_index)
{
    const auto& node = model.graph.nodes[node_index];
    const auto& block = model.blocks[static_cast<std::size_t>(node.id)];
    cfg_block_data_t data;
    data.node_id = node.id;
    data.rect = QRectF(node.x - node.width * 0.5, node.y, node.width, node.height);
    data.is_entry = block.is_entry;
    data.is_exit = block.successors.empty() && !block.is_entry;
    data.has_breakpoint = block.has_breakpoint;
    data.current_rip = model.current_rip >= block.start_addr &&
        model.current_rip < block.end_addr;
    data.current_rip_address = model.current_rip;
    data.header_height = 25.0;
    data.row_height = 18.0;
    data.addr_column_width = (std::max)(80.0, static_cast<qreal>(node.addr_col_w));
    data.body_padding = 14.0;
    data.font_size = 13.0;
    const char* kind = block.is_entry ? "ENTRY" : (data.is_exit ? "EXIT" : "BLOCK");
    if (block.is_entry) {
        std::string fname = cfg_view::detail::resolve_branch_symbol_for_cfg(
            context_, model.entry_addr);
        if (fname.empty() && model.entry_addr != block.start_addr)
            fname = cfg_view::detail::resolve_branch_symbol_for_cfg(context_,
                block.start_addr);
        if (!fname.empty()) {
            const std::size_t avail = 148;
            const std::string fn_short = fname.size() > avail
                ? fname.substr(0, avail - 1) + "\xE2\x80\xA6" : fname;
            data.header = QString::fromStdString(std::string(kind) + "  " + fn_short);
        } else {
            data.header = QStringLiteral("%1  %2").arg(QString::fromLatin1(kind))
                .arg(QString::number(block.start_addr, 16).toUpper());
        }
    } else {
        data.header = QStringLiteral("%1  %2").arg(QString::fromLatin1(kind))
            .arg(QString::number(block.start_addr, 16).toUpper());
    }
    const auto injections = model.entry_injections.find(node.id);
    if (block.is_entry && injections != model.entry_injections.end()) {
        for (const auto& injection : injections->second)
            data.injections.push_back({static_cast<int>(injection.kind),
                QString::fromStdString(injection.text)});
    }
    data.rows.reserve(block.instructions.size());
    for (const auto& line : block.instructions)
        data.rows.push_back({line.addr, QString::fromStdString(line.text)});
    data.total_instructions = block.instructions.size();
    return data;
}

void CfgSceneController::rebuildWorkspaceLayout(
    const aida::analysis::function_record_t& function, std::size_t page_begin,
    std::size_t page_end)
{
    if (!workspace_state_ || !context_ || !context_.publication ||
        !context_.publication->snapshot)
        return;
    cfg_view::workspace_graph_rebuild_layout(*workspace_state_, context_, function,
        page_begin, page_end);
    assembleWorkspace();
    Q_EMIT contentChanged();
}

void CfgSceneController::assembleWorkspace()
{
    for (auto& item : items_)
        delete item.second;
    items_.clear();
    edge_layer_->clear_edges();
    const auto& view = *workspace_state_;
    const auto& snapshot = *context_.publication->snapshot;
    for (std::size_t node_index = 0; node_index < view.layout.nodes.size(); ++node_index) {
        if (node_index >= view.block_indices.size())
            break;
        auto* item = new CfgBlockItem(makeWorkspaceBlockData(node_index), this);
        item->startEntrance();
        scene_->addItem(item);
        items_[node_index] = item;
    }
    std::vector<cfg_edge_data_t> edge_data;
    edge_data.reserve(view.edges.size());
    for (const auto& edge : view.edges) {
        if (edge.from < 0 || edge.to < 0 ||
            static_cast<std::size_t>(edge.from) >= view.layout.nodes.size() ||
            static_cast<std::size_t>(edge.to) >= view.layout.nodes.size())
            continue;
        const auto& from = view.layout.nodes[static_cast<std::size_t>(edge.from)];
        const auto& to = view.layout.nodes[static_cast<std::size_t>(edge.to)];
        cfg_edge_data_t data;
        data.from_rect = QRectF(from.x - from.width * 0.5, from.y, from.width,
            from.height);
        data.to_rect = QRectF(to.x - to.width * 0.5, to.y, to.width, to.height);
        data.kind = static_cast<int>(edge.kind);
        data.branching = static_cast<std::size_t>(edge.from) < view.outgoing.size() &&
            view.outgoing[static_cast<std::size_t>(edge.from)] > 1;
        data.true_branch = edge.kind == aida::analysis::edge_kind_t::conditional_taken;
        edge_data.push_back(data);
    }
    edge_layer_->set_edges(std::move(edge_data), worldBounds());
    mirrorWorkspaceSelection();
}

cfg_block_data_t CfgSceneController::makeWorkspaceBlockData(std::size_t node_index)
{
    const auto& view = *workspace_state_;
    const auto& snapshot = *context_.publication->snapshot;
    const auto& node = view.layout.nodes[node_index];
    const auto& block = snapshot.blocks[view.block_indices[node_index]];
    cfg_block_data_t data;
    data.node_id = static_cast<int>(node_index);
    data.rect = QRectF(node.x - node.width * 0.5, node.y, node.width, node.height);
    data.is_entry = node.is_entry;
    data.is_exit = static_cast<std::size_t>(node_index) < view.outgoing.size() &&
        view.outgoing[node_index] == 0;
    data.confidence = static_cast<unsigned>(block.confidence);
    data.header_height = 34.0;
    data.row_height = 19.0;
    data.addr_column_width = 112.0;
    data.body_padding = 9.0;
    data.font_size = 13.0;
    const auto block_address = disasm_view::runtime_address(context_, block.start)
        .value_or(block.start.value);
    const std::string block_name = disasm_view::resolve_name(context_, block.start);
    if (!block_name.empty()) {
        data.header = QString::fromStdString(block_name);
    } else if (data.is_entry) {
        data.header = QStringLiteral("entry_%1")
            .arg(QString::number(block_address, 16).toUpper());
    } else {
        data.header = QStringLiteral("loc_%1")
            .arg(QString::number(block_address, 16).toUpper());
    }
    const std::size_t instruction_begin = block.first_instruction;
    const std::size_t available = instruction_begin <= snapshot.instructions.size()
        ? snapshot.instructions.size() - instruction_begin : 0;
    const std::size_t instruction_total = (std::min)(
        static_cast<std::size_t>(block.instruction_count), available);
    const std::size_t shown = (std::min)(instruction_total, std::size_t{14});
    disasm_view::request_format_range(context_, instruction_begin,
        instruction_begin + instruction_total);
    data.rows.reserve(shown);
    for (std::size_t row = 0; row < shown; ++row) {
        const auto& instruction = snapshot.instructions[instruction_begin + row];
        const auto formatted = disasm_view::formatted_instruction(context_, instruction.id);
        const auto address = disasm_view::runtime_address(context_, instruction.address)
            .value_or(instruction.address.value);
        data.rows.push_back({address, formatted
            ? QString::fromStdString(formatted->text) : QString()});
    }
    data.total_instructions = instruction_total;
    return data;
}

void CfgSceneController::mirrorWorkspaceSelection()
{
    if (!workspace_state_ || !context_ || !context_.workspace ||
        !context_.publication || !context_.publication->snapshot)
        return;
    const auto& view = *workspace_state_;
    const auto& snapshot = *context_.publication->snapshot;
    const auto workspace_selection = context_.workspace->view_state().selection;
    if (!workspace_selection)
        return;
    const auto selection_runtime = disasm_view::runtime_address(context_,
        *workspace_selection).value_or(workspace_selection->value);
    if (view.selected_address == selection_runtime)
        return;
    for (std::size_t node_index = 0; node_index < view.block_indices.size();
         ++node_index) {
        const auto& block = snapshot.blocks[view.block_indices[node_index]];
        if (workspace_selection->space != block.start.space ||
            workspace_selection->value < block.start.value ||
            workspace_selection->value >= block.end.value)
            continue;
        workspace_state_->selected_block = block.id;
        workspace_state_->selected_address = selection_runtime;
        workspace_state_->selected_instruction.reset();
        const std::size_t begin = block.first_instruction;
        const std::size_t count = (std::min)(
            static_cast<std::size_t>(block.instruction_count),
            begin <= snapshot.instructions.size()
                ? snapshot.instructions.size() - begin : 0);
        for (std::size_t row = 0; row < count; ++row) {
            if (snapshot.instructions[begin + row].address == *workspace_selection) {
                workspace_state_->selected_instruction =
                    snapshot.instructions[begin + row].id;
                break;
            }
        }
        const auto node = view.node_by_entity.find(block.id);
        if (node != view.node_by_entity.end()) {
            const auto item = items_.find(static_cast<int>(node->second));
            if (item != items_.end()) {
                for (auto& entry : items_)
                    entry.second->setSelected(false);
                item->second->setSelected(true);
            }
        }
        Q_EMIT selectionChanged();
        break;
    }
}

void CfgSceneController::blockClicked(CfgBlockItem* item, int line, bool shift)
{
    if (!item)
        return;
    for (auto& entry : items_)
        entry.second->setSelected(false);
    item->setSelected(true);
    if (text_sel_item_ && text_sel_item_ != item)
        text_sel_item_->clearTextSelection();
    text_sel_item_ = item;
    const auto& data = item->data();
    std::uint64_t address = 0;
    if (line >= 0 && static_cast<std::size_t>(line) < data.rows.size())
        address = data.rows[static_cast<std::size_t>(line)].address;
    if (live_) {
        last_cursor_addr_ = address != 0 ? address : 0;
        if (address == 0) {
            const auto found = live_model_ ? live_model_->node_lookup.find(data.node_id)
                : std::unordered_map<int, std::size_t>::const_iterator();
            if (live_model_ && found != live_model_->node_lookup.end())
                address = live_model_->blocks[found->second].start_addr;
        }
        last_cursor_addr_ = address;
        selected_node_ = data.node_id;
        if (address != 0)
            disasm_view::select_address(address,
                disasm_view::capture_selected_workspace());
    } else if (workspace_state_ && context_ && context_.publication &&
               context_.publication->snapshot) {
        const auto& view = *workspace_state_;
        const auto& snapshot = *context_.publication->snapshot;
        const auto node_index = static_cast<std::size_t>(data.node_id);
        if (node_index < view.block_indices.size()) {
            const auto& block = snapshot.blocks[view.block_indices[node_index]];
            workspace_state_->selected_block = block.id;
            if (line >= 0 && static_cast<std::size_t>(line) < data.rows.size() &&
                data.rows[static_cast<std::size_t>(line)].address != 0) {
                const std::size_t instruction_begin = block.first_instruction;
                if (instruction_begin + static_cast<std::size_t>(line) <
                    snapshot.instructions.size()) {
                    const auto& instruction =
                        snapshot.instructions[instruction_begin +
                            static_cast<std::size_t>(line)];
                    workspace_state_->selected_instruction = instruction.id;
                    workspace_state_->selected_address = disasm_view::runtime_address(
                        context_, instruction.address).value_or(instruction.address.value);
                    disasm_view::select_address(instruction.address, context_);
                }
            } else {
                workspace_state_->selected_instruction.reset();
                workspace_state_->selected_address = disasm_view::runtime_address(context_,
                    block.start).value_or(block.start.value);
                disasm_view::select_address(block.start, context_);
            }
        }
    }
    static_cast<void>(shift);
    Q_EMIT selectionChanged();
}

void CfgSceneController::blockDoubleClicked(CfgBlockItem* item, int line)
{
    if (!item)
        return;
    const auto& data = item->data();
    std::uint64_t address = 0;
    if (line >= 0 && static_cast<std::size_t>(line) < data.rows.size())
        address = data.rows[static_cast<std::size_t>(line)].address;
    if (address == 0 && live_ && live_model_) {
        const auto found = live_model_->node_lookup.find(data.node_id);
        if (found != live_model_->node_lookup.end())
            address = live_model_->blocks[found->second].start_addr;
    }
    if (address == 0 && !live_ && workspace_state_)
        address = workspace_state_->selected_address;
    if (address == 0)
        return;
    if (!live_ && workspace_state_ && context_ && context_.publication &&
        context_.publication->snapshot) {
        const auto* selected = cfg_view::workspace_graph_selected_instruction(
            *workspace_state_, *context_.publication->snapshot);
        const auto target = selected
            ? cfg_view::workspace_graph_direct_target(context_, *selected) : std::nullopt;
        if (target) {
            disasm_view::goto_address(*target, context_);
            workspace_state_->selected_address = *target;
            return;
        }
    }
    last_cursor_addr_ = address;
    disasm_view::goto_address(address, live_
        ? disasm_view::capture_selected_workspace() : context_);
    Q_EMIT navigateToDisassembly(address);
}

void CfgSceneController::blockContextMenu(CfgBlockItem* item, int line,
                                          const QPoint& screen_pos)
{
    static_cast<void>(line);
    if (!item)
        return;
    if (live_) {
        aida::ui::analysis_context_menu::open(buildLiveMenu(item),
            aida::ui::context_menu_open_origin_t::pointer, screen_pos, nullptr);
    } else {
        aida::ui::analysis_context_menu::open(buildWorkspaceMenu(item),
            aida::ui::context_menu_open_origin_t::pointer, screen_pos, nullptr);
    }
}

CfgBlockItem* CfgSceneController::selectedItem() const
{
    if (live_) {
        if (!selected_node_)
            return nullptr;
        const auto found = items_.find(*selected_node_);
        return found == items_.end() ? nullptr : found->second;
    }
    if (workspace_state_ && workspace_state_->selected_block) {
        const auto node = workspace_state_->node_by_entity.find(
            *workspace_state_->selected_block);
        if (node != workspace_state_->node_by_entity.end()) {
            const auto found = items_.find(static_cast<int>(node->second));
            if (found != items_.end())
                return found->second;
        }
    }
    return nullptr;
}

void CfgSceneController::clearTextSelection()
{
    if (text_sel_item_) {
        text_sel_item_->clearTextSelection();
        text_sel_item_ = nullptr;
    }
}

bool CfgSceneController::hasTextSelection() const
{
    return text_sel_item_ && text_sel_item_->textSelectionActive();
}

QString CfgSceneController::selectedText(bool include_address) const
{
    return QString::fromStdString(buildSelectionText(include_address));
}

std::string CfgSceneController::buildSelectionText(bool include_address) const
{
    if (!text_sel_item_)
        return {};
    const auto selection = text_sel_item_->textSelection();
    if (selection.first < 0 || selection.second < 0)
        return {};
    const auto& rows = text_sel_item_->data().rows;
    int lo = (std::min)(selection.first, selection.second);
    int hi = (std::max)(selection.first, selection.second);
    if (lo < 0)
        lo = 0;
    if (static_cast<std::size_t>(hi) >= rows.size())
        hi = static_cast<int>(rows.size()) - 1;
    if (hi < lo)
        return {};
    std::string out;
    out.reserve(static_cast<std::size_t>(hi - lo + 1) * 64);
    char buffer[256];
    for (int index = lo; index <= hi; ++index) {
        const auto& row = rows[static_cast<std::size_t>(index)];
        if (include_address) {
            std::snprintf(buffer, sizeof(buffer), ".text:%016llX  %s\n",
                static_cast<unsigned long long>(row.address),
                row.text.toStdString().c_str());
        } else {
            std::snprintf(buffer, sizeof(buffer), "%s\n",
                row.text.toStdString().c_str());
        }
        out += buffer;
    }
    if (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

QRectF CfgSceneController::worldBounds() const
{
    float min_x, min_y, max_x, max_y;
    if (live_ && live_model_) {
        cfg_view::detail::compute_world_bounds(live_model_->graph, min_x, min_y, max_x,
            max_y);
        return QRectF(min_x, min_y, max_x - min_x, max_y - min_y);
    }
    if (!live_ && workspace_state_) {
        cfg_view::detail::compute_world_bounds(workspace_state_->layout, min_x, min_y,
            max_x, max_y);
        return QRectF(min_x, min_y, max_x - min_x, max_y - min_y);
    }
    return {};
}

QRectF CfgSceneController::blockSceneRect(int node_id) const
{
    const auto found = items_.find(node_id);
    if (found == items_.end())
        return {};
    return found->second->sceneBoundingRect();
}

std::optional<int> CfgSceneController::nodeForAddress(std::uint64_t address) const
{
    if (live_ && live_model_) {
        for (std::size_t index = 0; index < live_model_->blocks.size(); ++index) {
            const auto& block = live_model_->blocks[index];
            if (address >= block.start_addr && address < block.end_addr) {
                const auto found = live_model_->node_lookup.find(static_cast<int>(index));
                if (found != live_model_->node_lookup.end())
                    return found->first;
            }
        }
        return std::nullopt;
    }
    if (!live_ && workspace_state_ && context_ && context_.publication &&
        context_.publication->snapshot) {
        const auto& snapshot = *context_.publication->snapshot;
        for (std::size_t node_index = 0; node_index < workspace_state_->block_indices.size();
             ++node_index) {
            const auto& block =
                snapshot.blocks[workspace_state_->block_indices[node_index]];
            const auto start = disasm_view::runtime_address(context_, block.start)
                .value_or(block.start.value);
            const auto end = disasm_view::runtime_address(context_, block.end)
                .value_or(block.end.value);
            if (address >= start && address < end) {
                const auto node = workspace_state_->node_by_entity.find(block.id);
                if (node != workspace_state_->node_by_entity.end())
                    return static_cast<int>(node->second);
            }
        }
    }
    return std::nullopt;
}

void CfgSceneController::persistMutation(const std::function<void()>& mutator)
{
    if (live_ || !workspace_state_ || !context_)
        return;
    const auto old_collapsed = workspace_state_->collapsed_reachable_roots;
    const auto old_positions = workspace_state_->pinned_node_positions;
    const auto old_pinned_layouts = workspace_state_->pinned_layouts;
    const auto old_layout_positions = workspace_state_->pinned_layout_positions;
    const auto old_signature = workspace_state_->layout_signature;
    mutator();
    if (cfg_view::workspace_graph_save_persisted(context_, *workspace_state_)) {
        assembleWorkspace();
        Q_EMIT contentChanged();
        return;
    }
    workspace_state_->collapsed_reachable_roots = old_collapsed;
    workspace_state_->pinned_node_positions = old_positions;
    workspace_state_->pinned_layouts = old_pinned_layouts;
    workspace_state_->pinned_layout_positions = old_layout_positions;
    workspace_state_->layout_signature = old_signature;
}

aida::ui::analysis_context_menu::context_t CfgSceneController::buildLiveMenu(
    CfgBlockItem* item)
{
    static_cast<void>(item);
    using namespace aida::ui::analysis_context_menu;
    using aida::ui::action_handler_result_t;
    context_t menu;
    if (!live_model_)
        return menu;
    const auto& model = *live_model_;
    const auto address = last_cursor_addr_;
    const auto selected_block = selected_node_.value_or(-1);
    const auto selection_item = text_sel_item_;
    const auto selection = selection_item
        ? selection_item->textSelection() : std::pair<int, int>{-1, -1};
    menu.kind = menu_kind_t::graph;
    menu.entity_id = "legacy-graph:" + std::to_string(selected_block) + ":" +
        std::to_string(address) + ":" +
        std::to_string(selection_item ? selection_item->data().node_id : -1) + ":" +
        std::to_string(selection.first) + ":" + std::to_string(selection.second);
    menu.generation = model.generation;
    const auto store_generation = model.generation;
    menu.live_generation = []() {
        const auto current = cfg_view::capture_model();
        return current ? current->generation : 0;
    };
    menu.validate_identity = [this, address, selected_block, selection_item, selection,
                             store_generation]() {
        const auto current = cfg_view::capture_model();
        const auto current_selection = selection_item
            ? selection_item->textSelection() : std::pair<int, int>{-1, -1};
        return current && current->generation == store_generation &&
            last_cursor_addr_ == address &&
            selected_node_.value_or(-1) == selected_block &&
            (!selection_item ||
                (text_sel_item_ == selection_item &&
                 current_selection.first == selection.first &&
                 current_selection.second == selection.second))
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The selected graph entity changed");
    };
    menu.actions["analysis.navigate.disassembly"].invoke = [this, address]() {
        disasm_view::goto_address(address, disasm_view::capture_selected_workspace());
        Q_EMIT navigateToDisassembly(address);
        return action_handler_result_t::completed();
    };
    char address_text[32]{};
    std::snprintf(address_text, sizeof(address_text), "%016llX",
        static_cast<unsigned long long>(address));
    menu.actions["analysis.copy.address"].invoke = [value = std::string(address_text)]() {
        aida::qt::clipboard::set_text(QString::fromStdString(value));
        return action_handler_result_t::completed();
    };
    const auto text = buildSelectionText(false);
    const auto addressed = buildSelectionText(true);
    if (!text.empty()) {
        menu.actions["analysis.copy.block"].invoke = [text]() {
            aida::qt::clipboard::set_text(QString::fromStdString(text));
            return action_handler_result_t::completed();
        };
        menu.actions["analysis.copy.block_addressed"].invoke = [addressed]() {
            aida::qt::clipboard::set_text(QString::fromStdString(addressed));
            return action_handler_result_t::completed();
        };
    }
    menu.actions["analysis.graph.fit"].invoke = [this]() {
        Q_EMIT fitRequested();
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.graph.zoom_in"].invoke = [this]() {
        Q_EMIT zoomStepRequested(1.18);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.graph.zoom_out"].invoke = [this]() {
        Q_EMIT zoomStepRequested(0.85);
        return action_handler_result_t::completed();
    };
    if (selected_block >= 0 &&
        static_cast<std::size_t>(selected_block) < model.blocks.size()) {
        const int context_block = selected_block;
        menu.actions["analysis.graph.select_block"].invoke = [this, context_block]() {
            const auto found = items_.find(context_block);
            if (found != items_.end()) {
                clearTextSelection();
                text_sel_item_ = found->second;
                found->second->setTextSelection(0,
                    static_cast<int>(found->second->data().rows.size()) - 1);
            }
            return action_handler_result_t::completed();
        };
    }
    if (!text.empty()) {
        menu.actions["analysis.graph.clear_selection"].invoke = [this]() {
            clearTextSelection();
            return action_handler_result_t::completed();
        };
    }
    return menu;
}

aida::ui::analysis_context_menu::context_t CfgSceneController::buildWorkspaceMenu(
    CfgBlockItem* item)
{
    using namespace aida::ui::analysis_context_menu;
    using aida::ui::action_handler_result_t;
    using aida::ui::capability_state_t;
    context_t menu;
    menu.kind = menu_kind_t::graph;
    if (!workspace_state_ || !context_ || !context_.publication ||
        !context_.publication->snapshot || !context_.workspace)
        return menu;
    const auto& view = *workspace_state_;
    const auto& snapshot = *context_.publication->snapshot;
    const auto& context = context_;
    const auto node_index = static_cast<std::size_t>(item->data().node_id);
    if (node_index >= view.block_indices.size())
        return menu;
    const auto& context_block = snapshot.blocks[view.block_indices[node_index]];
    const auto function = cfg_view::workspace_graph_function(context);
    if (!function)
        return menu;
    const auto retained_block = context_block.id;
    const auto retained_block_key = cfg_view::workspace_graph_block_key(*function,
        context_block);
    const auto retained_layout = view.current_layout;
    const auto retained_instruction = view.selected_instruction;
    const auto retained_address = view.selected_address;
    const auto retained_generation = context.publication->generation;
    const auto retained_analysis_revision = context.publication->analysis_revision;
    const auto retained_overlay_revision = context.workspace->overlay_revision();
    const auto state = workspace_state_;
    menu.entity_id = "graph-block:" + std::to_string(retained_block) + ":" +
        std::to_string(retained_instruction ? *retained_instruction : 0) + ":" +
        std::to_string(retained_address);
    menu.generation = workspace_generation_of(context);
    menu.live_generation = [context]() { return workspace_generation_of(context); };
    menu.validate_identity = [state, retained_block, retained_instruction,
                              retained_address, retained_layout]() {
        return state->selected_block && *state->selected_block == retained_block &&
            state->selected_instruction == retained_instruction &&
            state->selected_address == retained_address &&
            state->current_layout == retained_layout
            ? capability_state_t::available()
            : capability_state_t::unavailable(
                "The selected graph block or instruction changed");
    };
    auto unavailable = [&menu](const char* id, std::string reason) {
        action_slot_t slot;
        slot.capability = capability_state_t::unavailable(reason);
        slot.invoke = [reason = std::move(reason)]() {
            return action_handler_result_t::failed(reason);
        };
        menu.actions.emplace(id, std::move(slot));
    };
    const auto address = view.selected_address != 0 ? view.selected_address :
        disasm_view::runtime_address(context, context_block.start).value_or(
            context_block.start.value);
    const auto typed = disasm_view::typed_address(context, address);
    const auto validate_retained_action = [context, state, retained_block,
        retained_instruction, retained_address, retained_layout, retained_generation,
        retained_analysis_revision, retained_overlay_revision]() -> std::string {
        if (!context.workspace || context.workspace->closed() || !context.publication ||
            !context.publication->snapshot ||
            context.workspace->analysis_publication() != context.publication ||
            context.workspace->generation() != retained_generation ||
            context.workspace->analysis_revision() != retained_analysis_revision ||
            context.workspace->overlay_revision() != retained_overlay_revision ||
            context.publication->generation != retained_generation ||
            context.publication->analysis_revision != retained_analysis_revision)
            return "The graph workspace publication or overlay changed; reopen the context action";
        if (cfg_view::workspace_graph_state(context) != state || !state->selected_block ||
            *state->selected_block != retained_block ||
            state->selected_instruction != retained_instruction ||
            state->selected_address != retained_address ||
            state->current_layout != retained_layout)
            return "The selected graph block or instruction changed; reopen the context action";
        return {};
    };
    menu.actions["analysis.navigate.back"].invoke = [context]() {
        disasm_view::navigate_back(context);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.forward"].invoke = [context]() {
        disasm_view::navigate_forward(context);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.disassembly"].invoke = [this, address]() {
        disasm_view::goto_address(address, context_);
        Q_EMIT navigateToDisassembly(address);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.graph"].invoke = []() {
        const auto hook = analysis_bridge::view_focus_hook();
        if (hook)
            hook("document.graph");
        return action_handler_result_t::completed();
    };
    const auto function_start = disasm_view::enclosing_function_start(address, context);
    if (function_start != 0) {
        auto decompile = [context, function_start]() {
            pseudocode_view::request_decompile(context, function_start, false);
            const auto hook = analysis_bridge::view_focus_hook();
            if (hook)
                hook("document.pseudocode");
            return action_handler_result_t::completed();
        };
        menu.actions["analysis.navigate.pseudocode"].invoke = decompile;
        menu.actions["analysis.function.decompile"].invoke = std::move(decompile);
    } else {
        unavailable("analysis.navigate.pseudocode", "No recovered function contains this graph selection");
        unavailable("analysis.function.decompile", "No recovered function contains this graph selection");
    }
    menu.actions["analysis.navigate.functions"].invoke = [context, address]() {
        disasm_view::select_address(address, context, false);
        const auto hook = analysis_bridge::view_focus_hook();
        if (hook)
            hook("view.analysis.functions");
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.structures"].invoke = [context, address]() {
        disasm_view::select_address(address, context, false);
        const auto hook = analysis_bridge::view_focus_hook();
        if (hook)
            hook("view.types.structures");
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.types"].invoke = [context, address]() {
        disasm_view::select_address(address, context, false);
        const auto hook = analysis_bridge::view_focus_hook();
        if (hook)
            hook("view.types.inferred");
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.xrefs"].invoke = [context, address]() {
        disasm_view::open_xrefs(address, context);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.callers"].invoke = [context, function_start, address]() {
        disasm_view::open_xrefs(function_start != 0 ? function_start : address, context);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.xrefs_from"].invoke = [context, address]() {
        disasm_view::select_address(address, context, false);
        const auto hook = analysis_bridge::view_focus_hook();
        if (hook)
            hook("view.analysis.references");
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.navigate.hex"].invoke = [context, address]() {
        disasm_view::select_address(address, context, false);
        const auto hook = analysis_bridge::view_focus_hook();
        if (hook)
            hook("document.hex");
        return action_handler_result_t::completed();
    };
    const auto* selected_instruction = cfg_view::workspace_graph_selected_instruction(view,
        snapshot);
    const auto direct_target = selected_instruction
        ? cfg_view::workspace_graph_direct_target(context, *selected_instruction)
        : std::nullopt;
    if (direct_target) {
        menu.actions["analysis.navigate.follow"].invoke = [context, target = *direct_target]() {
            disasm_view::goto_address(target, context);
            return action_handler_result_t::completed();
        };
    } else {
        unavailable("analysis.navigate.follow", "The selected graph instruction has no direct resolved target");
    }
    std::optional<std::uint64_t> source_address;
    const auto successor = view.successors.find(retained_block);
    const bool has_reachable_target = successor != view.successors.end() &&
        std::any_of(successor->second.begin(), successor->second.end(),
            [&view, retained_block](const auto target) {
                return target != retained_block &&
                    view.node_by_entity.count(target) != 0;
            });
    const auto predecessor = view.predecessors.find(retained_block);
    if (predecessor != view.predecessors.end()) {
        const auto visible_source = std::find_if(predecessor->second.begin(),
            predecessor->second.end(), [&view](const auto source) {
                return view.node_by_entity.count(source) != 0;
            });
        const auto source = visible_source == predecessor->second.end()
            ? view.page_block_by_entity.end()
            : view.page_block_by_entity.find(*visible_source);
        if (source != view.page_block_by_entity.end() &&
            source->second < snapshot.blocks.size()) {
            const auto& source_block = snapshot.blocks[source->second];
            source_address = disasm_view::runtime_address(context, source_block.start)
                .value_or(source_block.start.value);
        }
    }
    if (source_address) {
        menu.actions["analysis.graph.navigate_source"].invoke =
            [context, value = *source_address]() {
                disasm_view::goto_address(value, context);
                return action_handler_result_t::completed();
            };
    } else {
        unavailable("analysis.graph.navigate_source",
            "No incoming source block is present in the visible page edge set; it may be on another page or hidden");
    }
    std::optional<std::uint64_t> visible_direct_target;
    if (direct_target) {
        const auto typed_target = disasm_view::typed_address(context, *direct_target);
        if (typed_target && successor != view.successors.end()) {
            for (const auto target_entity : successor->second) {
                if (view.node_by_entity.count(target_entity) == 0)
                    continue;
                const auto target = view.page_block_by_entity.find(target_entity);
                if (target == view.page_block_by_entity.end() ||
                    target->second >= snapshot.blocks.size())
                    continue;
                const auto& block = snapshot.blocks[target->second];
                if (typed_target->space == block.start.space &&
                    typed_target->value >= block.start.value &&
                    typed_target->value < block.end.value) {
                    visible_direct_target = *direct_target;
                    break;
                }
            }
        }
    }
    if (visible_direct_target) {
        menu.actions["analysis.graph.navigate_target"].invoke =
            [context, value = *visible_direct_target]() {
                disasm_view::goto_address(value, context);
                return action_handler_result_t::completed();
            };
    } else {
        unavailable("analysis.graph.navigate_target",
            "The selected instruction's direct target is not present in the visible page edge set; it may be on another page or hidden");
    }
    const bool collapsed = view.collapsed_reachable_roots.count(retained_block_key) != 0;
    if (view.edge_set_truncated) {
        const std::string reason =
            "The visible page edge set is bounded; complete reachability is unavailable";
        unavailable("analysis.graph.collapse_reachable", reason);
        unavailable("analysis.graph.expand_reachable", reason);
    } else if (has_reachable_target && !collapsed) {
        unavailable("analysis.graph.expand_reachable",
            "The selected visible-page reachable scope is already expanded");
        menu.actions["analysis.graph.collapse_reachable"].invoke =
            [this, retained_block_key]() {
                if (workspace_state_->collapsed_reachable_roots.size() >=
                    cfg_view::k_workspace_graph_persisted_item_limit)
                    return action_handler_result_t::failed(
                        "The bounded graph collapse-state limit was reached");
                persistMutation([this, retained_block_key] {
                    workspace_state_->collapsed_reachable_roots.insert(retained_block_key);
                    workspace_state_->layout_signature = 0;
                });
                return action_handler_result_t::completed();
            };
    } else {
        unavailable("analysis.graph.collapse_reachable", collapsed
            ? "The selected reachable scope is already collapsed"
            : "The selected block has no reachable target blocks on the visible page");
        if (collapsed) {
            menu.actions["analysis.graph.expand_reachable"].invoke =
                [this, retained_block_key]() {
                    persistMutation([this, retained_block_key] {
                        workspace_state_->collapsed_reachable_roots.erase(retained_block_key);
                        workspace_state_->layout_signature = 0;
                    });
                    return action_handler_result_t::completed();
                };
        } else {
            unavailable("analysis.graph.expand_reachable",
                "The selected visible-page reachable scope is already expanded");
        }
    }
    const bool node_pinned = view.pinned_node_positions.count(retained_block_key) != 0;
    if (node_pinned) {
        unavailable("analysis.graph.pin_node", "The selected node is already pinned");
        menu.actions["analysis.graph.pin_node"].check_state =
            aida::ui::action_check_state_t::checked;
        menu.actions["analysis.graph.unpin_node"].invoke =
            [this, retained_block_key]() {
                persistMutation([this, retained_block_key] {
                    workspace_state_->pinned_node_positions.erase(retained_block_key);
                    workspace_state_->layout_signature = 0;
                });
                return action_handler_result_t::completed();
            };
    } else {
        unavailable("analysis.graph.unpin_node", "The selected node is not pinned");
        if (view.pinned_node_positions.size() >=
            cfg_view::k_workspace_graph_persisted_item_limit) {
            unavailable("analysis.graph.pin_node", "The bounded pinned-node limit was reached");
        } else {
            menu.actions["analysis.graph.pin_node"].invoke =
                [this, retained_block, retained_block_key]() {
                    const auto found = workspace_state_->node_by_entity.find(retained_block);
                    if (found == workspace_state_->node_by_entity.end() ||
                        found->second >= workspace_state_->layout.nodes.size())
                        return action_handler_result_t::failed("The graph node layout changed");
                    const auto& node = workspace_state_->layout.nodes[found->second];
                    persistMutation([this, retained_block_key, x = node.x, y = node.y] {
                        workspace_state_->pinned_node_positions[retained_block_key] =
                            cfg_view::cfg_vec2_t{x, y};
                    });
                    return action_handler_result_t::completed();
                };
        }
    }
    const bool layout_pinned = retained_layout &&
        view.pinned_layouts.count(*retained_layout) != 0;
    if (!retained_layout) {
        unavailable("analysis.graph.pin_layout", "The current graph page has no layout identity");
        unavailable("analysis.graph.unpin_layout", "The current graph page has no layout identity");
    } else if (layout_pinned) {
        unavailable("analysis.graph.pin_layout", "The current function page layout is already pinned");
        menu.actions["analysis.graph.pin_layout"].check_state =
            aida::ui::action_check_state_t::checked;
        menu.actions["analysis.graph.unpin_layout"].invoke =
            [this, retained_layout]() {
                persistMutation([this, retained_layout] {
                    workspace_state_->pinned_layouts.erase(*retained_layout);
                    for (auto iterator = workspace_state_->pinned_layout_positions.begin();
                        iterator != workspace_state_->pinned_layout_positions.end();) {
                        if (iterator->first.layout == *retained_layout)
                            iterator = workspace_state_->pinned_layout_positions.erase(iterator);
                        else ++iterator;
                    }
                    workspace_state_->layout_signature = 0;
                });
                return action_handler_result_t::completed();
            };
    } else if (view.node_by_entity.size() > cfg_view::k_workspace_graph_persisted_item_limit ||
        view.pinned_layout_positions.size() + view.node_by_entity.size() >
            cfg_view::k_workspace_graph_persisted_item_limit) {
        const std::string reason =
            "The bounded 256-node pinned-layout position limit was reached";
        unavailable("analysis.graph.pin_layout", reason);
        unavailable("analysis.graph.unpin_layout", "The current function page layout is not pinned");
    } else {
        unavailable("analysis.graph.unpin_layout", "The current function page layout is not pinned");
        menu.actions["analysis.graph.pin_layout"].invoke = [this, context, retained_layout]() {
            const auto* current_function = cfg_view::workspace_graph_function(context);
            if (!current_function || !context.publication ||
                !context.publication->snapshot ||
                workspace_state_->current_layout != retained_layout)
                return action_handler_result_t::failed(
                    "The graph publication changed before the layout could be pinned");
            persistMutation([this, context, retained_layout, current_function] {
                workspace_state_->pinned_layouts.insert(*retained_layout);
                const auto& current_snapshot = *context.publication->snapshot;
                for (const auto& item_entry : workspace_state_->node_by_entity) {
                    if (item_entry.second >= workspace_state_->layout.nodes.size() ||
                        item_entry.second >= workspace_state_->block_indices.size() ||
                        workspace_state_->block_indices[item_entry.second] >=
                            current_snapshot.blocks.size())
                        continue;
                    const auto& node = workspace_state_->layout.nodes[item_entry.second];
                    const auto& block =
                        current_snapshot.blocks[workspace_state_->block_indices[item_entry.second]];
                    workspace_state_->pinned_layout_positions[{*retained_layout,
                        cfg_view::workspace_graph_block_key(*current_function, block)}] =
                        cfg_view::cfg_vec2_t{node.x, node.y};
                }
            });
            return action_handler_result_t::completed();
        };
    }
    char address_text[32]{};
    std::snprintf(address_text, sizeof(address_text), "%016llX",
        static_cast<unsigned long long>(address));
    menu.actions["analysis.copy.address"].invoke = [value = std::string(address_text)]() {
        aida::qt::clipboard::set_text(QString::fromStdString(value));
        return action_handler_result_t::completed();
    };
    const auto block_begin = static_cast<std::size_t>(context_block.first_instruction);
    const auto block_count = (std::min)(static_cast<std::size_t>(context_block.instruction_count),
        block_begin <= snapshot.instructions.size() ? snapshot.instructions.size() - block_begin : 0);
    std::string block_text;
    std::string addressed_text;
    bool complete_text = block_count != 0;
    for (std::size_t row = 0; row < block_count; ++row) {
        const auto& instruction = snapshot.instructions[block_begin + row];
        const auto formatted = disasm_view::formatted_instruction(context, instruction.id);
        if (!formatted) {
            complete_text = false;
            continue;
        }
        if (!block_text.empty()) {
            block_text.push_back('\n');
            addressed_text.push_back('\n');
        }
        block_text += formatted->text;
        const auto runtime = disasm_view::runtime_address(context, instruction.address).value_or(
            instruction.address.value);
        char prefix[32]{};
        std::snprintf(prefix, sizeof(prefix), "%016llX  ",
            static_cast<unsigned long long>(runtime));
        addressed_text += prefix;
        addressed_text += formatted->text;
    }
    if (complete_text) {
        const std::string evidence_text = addressed_text;
        const std::string graph_name = disasm_view::resolve_name(context, context_block.start);
        const std::string evidence_label = graph_name.empty()
            ? std::string(address_text) : graph_name;
        const std::string evidence_return = std::string(address_text);
        const auto queue_evidence = [context, address, block_id = context_block.id,
            evidence_text, evidence_label, evidence_return](bool agent) {
            aida::automation_ui::evidence_envelope_t envelope;
            envelope.workspace_id = context.workspace->identity().binary_id().to_hex();
            envelope.source_view_id = "document.graph";
            envelope.source_kind = "basic_block";
            envelope.entity_id = "block:" + std::to_string(block_id);
            envelope.display_label = evidence_label;
            envelope.return_target = "address:" + evidence_return;
            envelope.excerpt = evidence_text;
            envelope.address = address;
            envelope.revision = context.publication->analysis_revision;
            envelope.generation = context.publication->generation;
            envelope.snapshot_hash = workspace_generation_of(context);
            envelope.content_hash = analysis_bridge::disasm_evidence_hash(evidence_text);
            const auto evidence_id = aida::automation_ui::register_evidence(std::move(envelope));
            if (evidence_id.empty())
                return action_handler_result_t::failed(
                    "The bounded evidence registry rejected this graph block");
            std::string error;
            const bool queued = agent
                ? aida::automation_ui::queue_evidence_for_agent(evidence_id, error)
                : aida::automation_ui::queue_evidence_for_chat(evidence_id, error);
            return queued ? action_handler_result_t::completed()
                : action_handler_result_t::failed(error);
        };
        menu.actions["analysis.evidence.chat"].invoke = [queue_evidence]() {
            return queue_evidence(false);
        };
        menu.actions["analysis.evidence.agent"].invoke = [queue_evidence]() {
            return queue_evidence(true);
        };
        menu.actions["analysis.copy.block"].invoke = [value = std::move(block_text)]() {
            aida::qt::clipboard::set_text(QString::fromStdString(value));
            return action_handler_result_t::completed();
        };
        menu.actions["analysis.copy.block_addressed"].invoke =
            [value = std::move(addressed_text)]() {
                aida::qt::clipboard::set_text(QString::fromStdString(value));
                return action_handler_result_t::completed();
            };
    } else {
        unavailable("analysis.copy.block", "Block formatting is still in progress");
        unavailable("analysis.copy.block_addressed", "Block formatting is still in progress");
        unavailable("analysis.evidence.chat", "Block formatting is still in progress");
        unavailable("analysis.evidence.agent", "Block formatting is still in progress");
    }
    if (typed) {
        const bool bookmarked = disasm_view::bookmarked(context, *typed);
        menu.actions["analysis.modify.rename"].invoke = [context, value = *typed]() {
            disasm_view::request_rename_dialog(context, value);
            return action_handler_result_t::completed();
        };
        menu.actions["analysis.modify.comment"].invoke = [context, value = *typed]() {
            disasm_view::request_comment_dialog(context, value);
            return action_handler_result_t::completed();
        };
        if (bookmarked) {
            unavailable("analysis.modify.bookmark", "The selected address is already bookmarked");
            menu.actions["analysis.modify.remove_bookmark"].invoke =
                [context, value = *typed]() {
                    return disasm_view::queue_bookmark(context, value, {})
                        ? action_handler_result_t::completed()
                        : action_handler_result_t::failed("The bookmark update was rejected");
                };
        } else {
            menu.actions["analysis.modify.bookmark"].invoke = [context, value = *typed,
                label = std::string(address_text)]() {
                return disasm_view::queue_bookmark(context, value, label)
                    ? action_handler_result_t::completed()
                    : action_handler_result_t::failed("The bookmark update was rejected");
            };
            unavailable("analysis.modify.remove_bookmark", "The selected address is not bookmarked");
        }
    } else {
        unavailable("analysis.modify.rename", "The graph selection has no mapped workspace address");
        unavailable("analysis.modify.comment", "The graph selection has no mapped workspace address");
        unavailable("analysis.modify.bookmark", "The graph selection has no mapped workspace address");
        unavailable("analysis.modify.remove_bookmark", "The graph selection has no mapped workspace address");
    }
    menu.actions["analysis.graph.fit"].invoke = [this]() {
        Q_EMIT fitRequested();
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.graph.zoom_in"].invoke = [this]() {
        Q_EMIT zoomStepRequested(1.18);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.graph.zoom_out"].invoke = [this]() {
        Q_EMIT zoomStepRequested(0.85);
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.graph.reset"].invoke = [this]() {
        Q_EMIT resetRequested();
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.graph.select_block"].invoke = [this, id = context_block.id,
        address]() {
        workspace_state_->selected_block = id;
        workspace_state_->selected_address = address;
        Q_EMIT selectionChanged();
        return action_handler_result_t::completed();
    };
    menu.actions["analysis.graph.clear_selection"].invoke = [this]() {
        workspace_state_->selected_block.reset();
        workspace_state_->selected_instruction.reset();
        workspace_state_->selected_address = 0;
        Q_EMIT selectionChanged();
        return action_handler_result_t::completed();
    };
    unavailable("analysis.navigate.disassembly_side",
        "Independent side documents require per-instance disassembly presentation state");
    unavailable("analysis.modify.assemble",
        "No assembler provider is registered; use reviewed Patch Bytes with explicitly assembled bytes");
    if (typed) {
        const auto selected_typed = *typed;
        const auto selected_runtime = address;
        auto& host_hooks = analysis::analysis_host_hooks();
        if (host_hooks.submit_xref_query) {
            menu.actions["analysis.navigate.callees"].invoke =
                [context, selected_runtime, validate_retained_action]() {
                    if (const auto reason = validate_retained_action(); !reason.empty())
                        return action_handler_result_t::failed(reason);
                    auto& hooks = analysis::analysis_host_hooks();
                    if (!hooks.submit_xref_query)
                        return action_handler_result_t::failed(
                            "The canonical bounded References owner is unavailable for this workspace");
                    std::string error;
                    if (!hooks.submit_xref_query(context.workspace, selected_runtime, false, error))
                        return action_handler_result_t::failed(error);
                    return action_handler_result_t::completed();
                };
        } else {
            unavailable("analysis.navigate.callees",
                "The canonical bounded References owner is unavailable for this workspace");
        }
        menu.actions["analysis.modify.retype"].invoke =
            [context, selected_runtime, validate_retained_action]() {
                if (const auto reason = validate_retained_action(); !reason.empty())
                    return action_handler_result_t::failed(reason);
                const auto hook = analysis_bridge::view_focus_hook();
                if (hook)
                    hook("view.types.structures");
                auto& hooks = analysis::analysis_host_hooks();
                if (!hooks.stage_type_application)
                    return action_handler_result_t::failed("The analysis UI is not available");
                std::string error;
                if (!hooks.stage_type_application(context.workspace, selected_runtime, error))
                    return action_handler_result_t::failed(error);
                return action_handler_result_t::completed();
            };
        const auto extent = selected_instruction
            ? static_cast<std::uint64_t>(selected_instruction->length) : 0;
        const bool provider_backed = extent != 0 &&
            disasm_view::provider_offset(context, selected_typed).has_value();
        if (provider_backed) {
            menu.actions["analysis.modify.patch"].invoke =
                [context, selected_typed, extent, validate_retained_action]() {
                    if (const auto reason = validate_retained_action(); !reason.empty())
                        return action_handler_result_t::failed(reason);
                    std::string error;
                    if (!disasm_view::open_static_patch_review(context, selected_typed, extent,
                            disasm_view::static_patch_mode_t::bytes, &error))
                        return action_handler_result_t::failed(error);
                    return action_handler_result_t::completed();
                };
            menu.actions["analysis.modify.nop"].invoke =
                [context, selected_typed, extent, validate_retained_action]() {
                    if (const auto reason = validate_retained_action(); !reason.empty())
                        return action_handler_result_t::failed(reason);
                    std::string error;
                    if (!disasm_view::open_static_patch_review(context, selected_typed, extent,
                            disasm_view::static_patch_mode_t::nop_fill, &error))
                        return action_handler_result_t::failed(error);
                    return action_handler_result_t::completed();
                };
        } else {
            unavailable("analysis.modify.patch",
                "The selected graph instruction has no fully provider-backed byte range");
            unavailable("analysis.modify.nop",
                "The selected graph instruction has no fully provider-backed byte range");
        }
        const auto process = context.workspace->identity().process();
        const auto debugger_mutation_context = debugger_interaction::capture(
            debugger_interaction::kind_t::instruction, address, 0, -1, 0, extent);
        const auto breakpoint_definition_context = debugger_interaction::capture(
            debugger_interaction::kind_t::breakpoint, address);
        const bool debugger_matches_workspace_process = process &&
            process->creation_time_100ns != 0 &&
            driver_bridge::attached_pid() == process->pid &&
            debugger_mutation_context.target_pid == process->pid &&
            debugger_mutation_context.process_creation_time_100ns ==
                process->creation_time_100ns &&
            debugger_interaction::is_current(debugger_mutation_context) &&
            breakpoint_definition_context.target_pid == process->pid &&
            breakpoint_definition_context.process_creation_time_100ns ==
                process->creation_time_100ns &&
            debugger_interaction::is_current(breakpoint_definition_context);
        if (debugger_matches_workspace_process) {
            if (extent != 0) {
                menu.actions["analysis.modify.patch"].invoke =
                    [extent, debugger_mutation_context, validate_retained_action]() {
                        if (const auto reason = validate_retained_action(); !reason.empty())
                            return action_handler_result_t::failed(reason);
                        if (!debugger_interaction::is_current(debugger_mutation_context))
                            return action_handler_result_t::failed(
                                "The graph workspace process identity or debugger stop changed before patch review");
                        const auto hook = analysis_bridge::view_focus_hook();
                        if (hook)
                            hook("view.debug.patches");
                        std::string error;
                        if (!debugger_view::stage_patch_review(debugger_mutation_context, extent,
                                "Reviewed patch from Graph", &error))
                            return action_handler_result_t::failed(error);
                        return action_handler_result_t::completed();
                    };
                menu.actions["analysis.modify.patch"].capability =
                    capability_state_t::available();
                menu.actions["analysis.modify.nop"].invoke =
                    [extent, debugger_mutation_context, validate_retained_action]() {
                        if (const auto reason = validate_retained_action(); !reason.empty())
                            return action_handler_result_t::failed(reason);
                        if (!debugger_interaction::is_current(debugger_mutation_context))
                            return action_handler_result_t::failed(
                                "The graph workspace process identity or debugger stop changed before NOP review");
                        const auto hook = analysis_bridge::view_focus_hook();
                        if (hook)
                            hook("view.debug.patches");
                        std::string error;
                        if (!debugger_view::stage_nop_review(
                                debugger_mutation_context, extent, &error))
                            return action_handler_result_t::failed(error);
                        return action_handler_result_t::completed();
                    };
                menu.actions["analysis.modify.nop"].capability =
                    capability_state_t::available();
            } else {
                unavailable("analysis.modify.patch",
                    "The selected graph block has no exact instruction byte range");
                unavailable("analysis.modify.nop",
                    "The selected graph block has no exact instruction byte range");
            }
            const auto breakpoint_capability = debugger_view::address_mutation_capability(
                debugger_mutation_context, true);
            if (breakpoint_capability.enabled) {
                menu.actions["analysis.debug.breakpoint"].invoke =
                    [breakpoint_definition_context, validate_retained_action]() {
                        if (const auto reason = validate_retained_action(); !reason.empty())
                            return action_handler_result_t::failed(reason);
                        std::string error;
                        if (!debugger_view::queue_toggle_breakpoint(
                                breakpoint_definition_context, &error))
                            return action_handler_result_t::failed(error);
                        return action_handler_result_t::completed();
                    };
                menu.actions["analysis.debug.hardware_breakpoint"].invoke =
                    [breakpoint_definition_context, validate_retained_action]() {
                        if (const auto reason = validate_retained_action(); !reason.empty())
                            return action_handler_result_t::failed(reason);
                        const auto capability = debugger_view::address_mutation_capability(
                            breakpoint_definition_context, true);
                        if (!capability.enabled)
                            return action_handler_result_t::failed(capability.disabled_reason
                                ? capability.disabled_reason : "Breakpoint staging is unavailable");
                        const auto hook = analysis_bridge::view_focus_hook();
                        if (hook)
                            hook("view.debug.breakpoints");
                        std::string error;
                        if (!debugger_view::stage_breakpoint_definition(
                                breakpoint_definition_context,
                                debugger_view::breakpoint_definition_mode_t::hardware_execute,
                                &error))
                            return action_handler_result_t::failed(error);
                        return action_handler_result_t::completed();
                    };
            } else {
                const std::string reason = breakpoint_capability.disabled_reason
                    ? breakpoint_capability.disabled_reason : "Breakpoint staging is unavailable";
                unavailable("analysis.debug.breakpoint", reason);
                unavailable("analysis.debug.hardware_breakpoint", reason);
            }
        } else {
            const std::string reason = !process
                ? "Breakpoint definitions require a process-backed debugger workspace"
                : process->creation_time_100ns == 0
                ? "The graph workspace lacks a verified process creation identity"
                : driver_bridge::attached_pid() == process->pid
                ? "The attached process reused the graph workspace PID with a different creation identity"
                : "Attach the debugger to PID " + std::to_string(process->pid) +
                    " before staging a graph breakpoint";
            unavailable("analysis.debug.breakpoint", reason);
            unavailable("analysis.debug.hardware_breakpoint", reason);
        }
    } else {
        unavailable("analysis.navigate.callees",
            "The graph selection has no mapped workspace address");
        unavailable("analysis.modify.retype",
            "The graph selection has no mapped workspace address");
        unavailable("analysis.modify.patch",
            "The graph selection has no mapped workspace address");
        unavailable("analysis.modify.nop",
            "The graph selection has no mapped workspace address");
        unavailable("analysis.debug.breakpoint",
            "The graph selection has no mapped workspace address");
        unavailable("analysis.debug.hardware_breakpoint",
            "The graph selection has no mapped workspace address");
    }
    return menu;
}

aida::ui::analysis_context_menu::context_t
CfgSceneController::buildWorkspaceCanvasMenu()
{
    using namespace aida::ui::analysis_context_menu;
    using aida::ui::action_handler_result_t;
    context_t menu;
    menu.kind = menu_kind_t::graph;
    const auto context = context_;
    const auto state = workspace_state_;
    menu.entity_id = "graph-canvas:" + (context_ && context_.workspace
        ? context_.workspace->identity().binary_id().to_hex() : std::string());
    menu.generation = workspace_generation_of(context);
    menu.live_generation = [context]() { return workspace_generation_of(context); };
    menu.validate_identity = [context, state]() {
        return cfg_view::workspace_graph_state(context) == state
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The active graph canvas changed");
    };
    menu.actions["analysis.graph.fit"].invoke = [this]() {
        Q_EMIT fitRequested();
        return action_handler_result_t::completed();
    };
    return menu;
}

}
