#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cfg_layout.hpp"
#include "function_index.hpp"
#include "standalone_driver.hpp"
#include "symbol_classifier.hpp"
#include "zydis_disasm.hpp"
#include "../debugger/debugger_engine.hpp"
#include "../analysis/pdb_events.hpp"
#include "../infra/executor.hpp"
#include "../infra/event_bus.hpp"
#include "disasm_view.hpp"
#include "../ui/application_action_registry.hpp"
#include "../settings/settings_persistence_service.hpp"
#include "../settings/standalone_settings.hpp"
#include "../../helpers/diag_log.hpp"
#include "../../qt/analysis_bridge/gui_post.hpp"
#include "../../qt/analysis_bridge/revision_combine.hpp"

namespace cfg_view {

struct instruction_line_t {
	uint64_t    addr = 0;
	std::string text;
};

struct basic_block_t {
	uint64_t                       start_addr = 0;
	uint64_t                       end_addr = 0;
	std::vector<instruction_line_t> instructions;
	std::vector<int>               successors;
	bool                           is_entry = false;
	bool                           has_breakpoint = false;
};

struct cfg_model_snapshot_t {
	std::vector<basic_block_t> blocks;
	cfg_layout::graph_t graph;
	uint64_t entry_addr = 0;
	uint64_t current_rip = 0;
	std::unordered_map<int, std::size_t> node_lookup;
	std::map<int, std::vector<function_index::injection_row_t>> entry_injections;
	std::uint64_t generation = 0;
};

struct cfg_vec2_t {
	float x = 0.f;
	float y = 0.f;
};

using model_publish_hook_t = std::function<void()>;

namespace detail {

struct cfg_model_store_t {
	std::mutex mutex;
	std::shared_ptr<const cfg_model_snapshot_t> model;
	std::atomic<std::uint64_t> next_model_generation{1};
	std::atomic<bool> building{false};
};

inline cfg_model_store_t& model_store()
{
	static cfg_model_store_t value;
	return value;
}

inline model_publish_hook_t& publish_hook()
{
	static model_publish_hook_t value;
	return value;
}

}

inline void set_model_publish_hook(model_publish_hook_t hook)
{
	detail::publish_hook() = std::move(hook);
}

inline std::shared_ptr<const cfg_model_snapshot_t> capture_model()
{
	auto& store = detail::model_store();
	std::lock_guard<std::mutex> lock(store.mutex);
	return store.model;
}

inline void publish_model(std::shared_ptr<const cfg_model_snapshot_t> model)
{
	auto& store = detail::model_store();
	{
		std::lock_guard<std::mutex> lock(store.mutex);
		store.model = std::move(model);
	}
	if (detail::publish_hook())
		detail::publish_hook()();
}

inline bool building()
{
	return detail::model_store().building.load(std::memory_order_acquire);
}

inline void build_cfg(uint64_t entry_address);
inline void build_cfg(const disasm_view::workspace_context_t& context,
                      uint64_t entry_address);

namespace detail {

inline float estimate_text_width_for_cfg(const char* text, float font_size)
{
	if (!text || font_size <= 0.f)
		return 0.f;
	float width = 0.f;
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
		const unsigned char c = *p;
		if (c == '\t') {
			width += font_size * 2.0f;
		} else if (c == ' ') {
			width += font_size * 0.35f;
		} else if (std::strchr("ilI.,:;!|'", c)) {
			width += font_size * 0.34f;
		} else if (std::strchr("mwMW@#%&", c)) {
			width += font_size * 0.86f;
		} else {
			width += font_size * 0.58f;
		}
	}
	return width;
}

inline float estimate_text_width_for_cfg(const std::string& text, float font_size)
{
	return estimate_text_width_for_cfg(text.c_str(), font_size);
}

inline bool safe_decode_for_cfg(const uint8_t* code, int avail, uint64_t va,
                               bool is_64bit, AsmInstr& out)
{
#if defined(_MSC_VER)
	__try {
		out = zydis_decode_one(code, avail, va, is_64bit);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		std::snprintf(out.mnem, sizeof(out.mnem), "db");
		std::snprintf(out.ops, sizeof(out.ops), "0x%02X", (code && avail > 0) ? code[0] : 0);
		out.addr = va;
		out.len = 1;
		if (code && avail > 0)
			out.raw[0] = code[0];
		return false;
	}
#else
	out = zydis_decode_one(code, avail, va, is_64bit);
	return true;
#endif
}

inline std::atomic<bool>&                    pdb_subscription_armed_flag()
{
	static std::atomic<bool> armed{false};
	return armed;
}

inline aida::events::subscription_handle_t& pdb_subscription_slot()
{
	static aida::events::subscription_handle_t slot;
	return slot;
}

inline void rebuild_on_pdb_load(const aida::events::event_pdb_loaded& ev)
{
	if (!ev.success) return;
	const auto model = capture_model();
	const uint64_t entry = model ? model->entry_addr : 0;
	if (entry == 0) return;
	aida::qt::gui_post_or_run([entry] {
		build_cfg(entry);
	});
}

inline void ensure_pdb_subscription()
{
	auto& armed = pdb_subscription_armed_flag();
	if (armed.load(std::memory_order_acquire)) return;
	bool expected = false;
	if (!armed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
	pdb_subscription_slot() = aida::events::subscribe(
		aida::events::event_pdb_loaded_def,
		[](const aida::events::event_pdb_loaded& ev) {
			rebuild_on_pdb_load(ev);
		});
	if (!pdb_subscription_slot().valid()) {
		armed.store(false, std::memory_order_release);
	}
}

}

inline void clear()
{
	publish_model({});
}

namespace detail {

inline int find_or_create_block(std::map<uint64_t, int>& addr_to_block,
								std::vector<basic_block_t>& blocks, uint64_t addr)
{
	auto it = addr_to_block.find(addr);
	if (it != addr_to_block.end())
		return it->second;
	int idx = static_cast<int>(blocks.size());
	blocks.emplace_back();
	blocks.back().start_addr = addr;
	addr_to_block[addr] = idx;
	return idx;
}

inline void compute_world_bounds(const cfg_layout::graph_t& g, float& min_x, float& min_y,
                                 float& max_x, float& max_y)
{
	min_x = min_y = 1e9f;
	max_x = max_y = -1e9f;
	for (const auto& n : g.nodes) {
		float lx = n.x - n.width * 0.5f;
		float rx = n.x + n.width * 0.5f;
		float ty = n.y;
		float by = n.y + n.height;
		if (lx < min_x) min_x = lx;
		if (rx > max_x) max_x = rx;
		if (ty < min_y) min_y = ty;
		if (by > max_y) max_y = by;
	}
	if (min_x > max_x) { min_x = -100.f; max_x = 100.f; }
	if (min_y > max_y) { min_y = -100.f; max_y = 100.f; }
}

inline std::string resolve_branch_symbol_for_cfg(
	const disasm_view::workspace_context_t& context, uint64_t target)
{
	if (target == 0) return std::string();
	const auto address = disasm_view::typed_address(context, target);
	std::string sym = address ? disasm_view::resolve_name(context, *address) : std::string();
	if (!sym.empty()) {
		auto bang = sym.find('!');
		if (bang != std::string::npos) sym = sym.substr(bang + 1);
		return sym;
	}
	return std::string();
}

inline std::string resolve_branch_symbol_for_cfg(uint64_t target)
{
	return resolve_branch_symbol_for_cfg(disasm_view::capture_selected_workspace(), target);
}

inline std::string substitute_branch_operand(
	const disasm_view::workspace_context_t& context,
	const std::string& ops, uint64_t target)
{
	if (target == 0) return ops;
	std::string sym = resolve_branch_symbol_for_cfg(context, target);
	if (sym.empty()) return ops;
	const auto address = disasm_view::typed_address(context, target);
	symbol_classifier::kind_t k = address
		? symbol_classifier::classify(context.workspace, *address)
		: symbol_classifier::kind_t::unknown;
	if (k == symbol_classifier::kind_t::external_import) {
		if (sym.compare(0, 6, "__imp_") != 0)
			sym = "__imp_" + sym;
	}
	char hex_buf[32];
	std::snprintf(hex_buf, sizeof(hex_buf), "0x%llX", static_cast<unsigned long long>(target));
	std::size_t pos = ops.find(hex_buf);
	if (pos == std::string::npos) {
		std::snprintf(hex_buf, sizeof(hex_buf), "0x%llx", static_cast<unsigned long long>(target));
		pos = ops.find(hex_buf);
	}
	if (pos == std::string::npos) {
		std::snprintf(hex_buf, sizeof(hex_buf), "%llXh", static_cast<unsigned long long>(target));
		pos = ops.find(hex_buf);
	}
	if (pos == std::string::npos) return ops;
	std::string out = ops.substr(0, pos) + sym + ops.substr(pos + std::strlen(hex_buf));
	return out;
}

}

inline void build_cfg(const disasm_view::workspace_context_t& workspace_context,
                      uint64_t entry_address)
{
	detail::ensure_pdb_subscription();

	if (detail::model_store().building.load(std::memory_order_acquire)) {
		diag::log_tagged_fmt("cfg", "build_cfg skipped already_building entry=0x%llX",
			static_cast<unsigned long long>(entry_address));
		return;
	}

	const uint32_t target_pid = workspace_context.workspace &&
		workspace_context.workspace->identity().process()
		? workspace_context.workspace->identity().process()->pid : 0;
	diag::log_tagged_fmt("cfg", "build_cfg START entry=0x%llX pid=%u",
		static_cast<unsigned long long>(entry_address), target_pid);

	detail::model_store().building.store(true, std::memory_order_release);

	try {
		aida::infra::executor::submission_t sub;
		sub.owner_subsystem = "disasm";
		sub.label = "disasm.cfg.build";
		sub.thread_class = "bounded_task";
		sub.domain = aida::infra::executor::domain_t::feature_worker;
		sub.priority = 2;
		sub.body = [entry_address, workspace_context]() {
		try {
		struct build_guard_t {
			~build_guard_t()
			{
				detail::model_store().building.store(false, std::memory_order_release);
			}
		} build_guard;

		auto t_start = std::chrono::steady_clock::now();
		const std::size_t max_bytes = 0x10000;
		const std::size_t max_insns = 4096;

		std::vector<uint8_t> mem;
		bool have_data = false;

		if (workspace_context) {
			const auto address = disasm_view::typed_address(workspace_context, entry_address);
			if (address) {
				auto bytes = disasm_view::read_bytes(workspace_context, *address, max_bytes);
				if (bytes) {
					mem = bytes.take_value();
					have_data = !mem.empty();
				}
			}
		} else if (driver_bridge::attached_pid() != 0) {
			have_data = driver_bridge::read_memory(entry_address, max_bytes, mem);
		}

		if (mem.empty()) {
			diag::log_tagged_fmt("cfg", "build_cfg FAILED no_memory_data entry=0x%llX have_data=%d",
				static_cast<unsigned long long>(entry_address), static_cast<int>(have_data));
			detail::model_store().building.store(false, std::memory_order_release);
			return;
		}
		diag::log_tagged_fmt("cfg", "build_cfg memory_read %zu bytes from 0x%llX",
			mem.size(), static_cast<unsigned long long>(entry_address));

		struct decoded_insn_t {
			AsmInstr    ins;
			uint64_t    branch_target = 0;
			bool        has_target = false;
		};

		std::vector<decoded_insn_t> all_insns;
		all_insns.reserve(max_insns);

		const uint8_t* data = mem.data();
		const std::size_t sz = mem.size();
		std::size_t pos = 0;

		while (pos < sz && all_insns.size() < max_insns) {
			int avail = static_cast<int>((std::min)(std::size_t{15}, sz - pos));
			uint64_t va = entry_address + static_cast<uint64_t>(pos);
			AsmInstr ins = {};
			const bool is_64bit = !workspace_context.image ||
				workspace_context.image->architecture() == aida::analysis::architecture_id_t::x86_64;
			if (!detail::safe_decode_for_cfg(data + pos, avail, va, is_64bit, ins)) {
				diag::log_tagged_fmt("cfg", "build_cfg decode_seh addr=0x%llX",
					static_cast<unsigned long long>(va));
			}
			if (ins.len <= 0) {
				ins.addr = va;
				ins.len = 1;
				std::snprintf(ins.mnem, sizeof(ins.mnem), "db");
				std::snprintf(ins.ops, sizeof(ins.ops), "0x%02X", data[pos]);
				ins.raw[0] = data[pos];
			}

			decoded_insn_t d;
			d.ins = ins;

			if ((ins.is_call || ins.is_branch) && ins.branch_target != 0) {
				d.branch_target = ins.branch_target;
				d.has_target = true;
			}

			all_insns.push_back(d);

			if (ins.is_ret)
				break;

			pos += static_cast<std::size_t>(ins.len);
		}

		if (all_insns.empty()) {
			detail::model_store().building.store(false, std::memory_order_release);
			return;
		}

		uint64_t decoded_lo = all_insns.front().ins.addr;
		uint64_t decoded_hi = all_insns.back().ins.addr + static_cast<uint64_t>(all_insns.back().ins.len);

		std::map<uint64_t, bool> leaders;
		leaders[entry_address] = true;

		for (auto& d : all_insns) {
			if (d.has_target && !d.ins.is_call) {
				if (d.branch_target >= decoded_lo && d.branch_target < decoded_hi)
					leaders[d.branch_target] = true;
				uint64_t fallthrough = d.ins.addr + d.ins.len;
				leaders[fallthrough] = true;
			}
			if (d.ins.is_ret) {
				uint64_t next = d.ins.addr + d.ins.len;
				leaders[next] = true;
			}
		}

		std::vector<basic_block_t> blocks;
		std::map<uint64_t, int> addr_to_block;

		int cur_block = -1;
		for (auto& d : all_insns) {
			if (leaders.count(d.ins.addr)) {
				cur_block = detail::find_or_create_block(addr_to_block, blocks, d.ins.addr);
				if (d.ins.addr == entry_address)
					blocks[static_cast<std::size_t>(cur_block)].is_entry = true;
			}
			if (cur_block < 0)
				cur_block = detail::find_or_create_block(addr_to_block, blocks, d.ins.addr);

			instruction_line_t line;
			line.addr = d.ins.addr;
			std::string ops_text = d.ins.ops;
			if ((d.ins.is_branch || d.ins.is_call) && d.ins.branch_target != 0) {
				ops_text = detail::substitute_branch_operand(
					workspace_context, ops_text, d.ins.branch_target);
			}
			line.text.reserve(std::strlen(d.ins.mnem) + 1 + ops_text.size());
			line.text.assign(d.ins.mnem);
			line.text.push_back(' ');
			line.text.append(ops_text);
			blocks[static_cast<std::size_t>(cur_block)].instructions.push_back(std::move(line));
			blocks[static_cast<std::size_t>(cur_block)].end_addr = d.ins.addr + d.ins.len;

			if (d.ins.is_ret)
				continue;

			if (d.has_target && !d.ins.is_call) {
				bool target_in_range = (d.branch_target >= decoded_lo && d.branch_target < decoded_hi);
				if (target_in_range) {
					auto it_target = addr_to_block.find(d.branch_target);
					if (it_target != addr_to_block.end())
						blocks[static_cast<std::size_t>(cur_block)].successors.push_back(it_target->second);
					else {
						int tidx = detail::find_or_create_block(addr_to_block, blocks, d.branch_target);
						blocks[static_cast<std::size_t>(cur_block)].successors.push_back(tidx);
					}
				}

				bool is_unconditional = (std::strcmp(d.ins.mnem, "jmp") == 0);
				if (!is_unconditional) {
					uint64_t fall = d.ins.addr + d.ins.len;
					auto it_fall = addr_to_block.find(fall);
					if (it_fall != addr_to_block.end())
						blocks[static_cast<std::size_t>(cur_block)].successors.push_back(it_fall->second);
					else {
						int fidx = detail::find_or_create_block(addr_to_block, blocks, fall);
						blocks[static_cast<std::size_t>(cur_block)].successors.push_back(fidx);
					}
				}

				uint64_t next_addr = d.ins.addr + d.ins.len;
				if (leaders.count(next_addr)) {
					cur_block = -1;
				}
			}
		}

		if (!workspace_context) {
			auto& bps = debugger_engine::g_state.breakpoints;
			std::lock_guard<std::mutex> bp_lk(debugger_engine::g_state.bp_mutex);
			for (auto& b : blocks) {
				for (auto& bp : bps) {
					if (bp.address >= b.start_addr && bp.address < b.end_addr) {
						b.has_breakpoint = true;
						break;
					}
				}
			}
		}

		const float code_base = 13.f;
		const float ui_base = 13.f;

		const float line_gap = 5.f;
		const float line_h = code_base + line_gap;
		const float padding = 14.f;
		const float header_h = ui_base + 12.f;
		const float addr_gap = 14.f;
		const float text_slack = 24.f;
		const float min_node_w = 380.f;
		const float max_node_w = 1200.f;

		std::map<int, std::vector<function_index::injection_row_t>> entry_injections;
		for (std::size_t bi = 0; bi < blocks.size(); ++bi) {
			const int block_id = static_cast<int>(bi);
			if (!blocks[bi].is_entry) continue;
			std::vector<function_index::injection_row_t> rows;
			if (workspace_context) {
				const auto address = disasm_view::typed_address(
					workspace_context, blocks[bi].start_addr);
				if (address) {
					rows = function_index::rows_before(
						workspace_context.workspace, *address);
				}
			}
			if (!rows.empty()) {
				char log_buf[160];
				std::snprintf(log_buf, sizeof(log_buf),
					"[cfg] entry block=%d addr=0x%llX inj_rows=%zu",
					block_id,
					static_cast<unsigned long long>(blocks[bi].start_addr),
					rows.size());
				diag::log_tagged("cfg_view", log_buf);
				entry_injections.emplace(block_id, std::move(rows));
			}
		}

		cfg_layout::graph_t graph;
		graph.nodes.reserve(blocks.size());
		for (std::size_t i = 0; i < blocks.size(); ++i) {
			const int block_id = static_cast<int>(i);
			cfg_layout::node_t n;
			n.id = block_id;
			n.is_entry = blocks[i].is_entry;

			float addr_w = 0.f;
			float text_w = 0.f;
			for (auto& ln : blocks[i].instructions) {
				char ab[24];
				std::snprintf(ab, sizeof(ab), "%llX",
					static_cast<unsigned long long>(ln.addr));
				float aw = detail::estimate_text_width_for_cfg(ab, code_base);
				if (aw > addr_w) addr_w = aw;
				if (!ln.text.empty()) {
					float tw = detail::estimate_text_width_for_cfg(ln.text, code_base);
					if (tw > text_w) text_w = tw;
				}
			}

			std::size_t inj_lines = 0;
			auto it_inj = entry_injections.find(block_id);
			if (it_inj != entry_injections.end()) {
				inj_lines = it_inj->second.size();
				for (const auto& r : it_inj->second) {
					if (r.text.empty()) continue;
					float tw = detail::estimate_text_width_for_cfg(r.text, code_base);
					if (tw > text_w) text_w = tw;
				}
			}

			char header_buf[160];
			const char* kind = blocks[i].is_entry
				? "ENTRY"
				: (blocks[i].successors.empty() && !blocks[i].is_entry ? "EXIT" : "BLOCK");
			if (blocks[i].is_entry) {
				std::string fname = detail::resolve_branch_symbol_for_cfg(
					workspace_context, entry_address);
				if (fname.empty() && entry_address != blocks[i].start_addr)
					fname = detail::resolve_branch_symbol_for_cfg(
						workspace_context, blocks[i].start_addr);
				if (!fname.empty()) {
					std::size_t avail = sizeof(header_buf) - 12;
					std::string fn_short = fname.size() > avail
						? fname.substr(0, avail - 2) + ".." : fname;
					std::snprintf(header_buf, sizeof(header_buf), "%s  %s",
						kind, fn_short.c_str());
				} else {
					std::snprintf(header_buf, sizeof(header_buf), "%s  %llX",
						kind, static_cast<unsigned long long>(blocks[i].start_addr));
				}
			} else {
				std::snprintf(header_buf, sizeof(header_buf), "%s  %llX",
					kind, static_cast<unsigned long long>(blocks[i].start_addr));
			}
			float header_w = detail::estimate_text_width_for_cfg(header_buf, ui_base);

			n.addr_col_w = addr_w + addr_gap;

			float body_w = n.addr_col_w + text_w + padding * 2.f + text_slack;
			float head_w = header_w + padding * 2.f + 20.f;
			float w = body_w > head_w ? body_w : head_w;
			if (w < min_node_w) w = min_node_w;
			if (w > max_node_w) w = max_node_w;
			n.width = w;

			std::size_t total_lines = blocks[i].instructions.size() + inj_lines;
			n.height = header_h + padding * 2.f + static_cast<float>(total_lines) * line_h;
			float min_h = header_h + line_h + padding * 2.f;
			if (n.height < min_h) n.height = min_h;
			graph.nodes.push_back(n);
		}

		for (std::size_t i = 0; i < blocks.size(); ++i) {
			const int block_id = static_cast<int>(i);
			auto& succs = blocks[i].successors;
			for (std::size_t j = 0; j < succs.size(); ++j) {
				cfg_layout::edge_t e;
				e.from = block_id;
				e.to = succs[j];
				e.is_true_branch = (j == 0 && succs.size() > 1);
				graph.edges.push_back(e);
			}
		}

		cfg_layout::layout(graph, 60.f, 60.f);

		const std::size_t block_count = blocks.size();
		const std::size_t node_count = graph.nodes.size();
		const std::size_t edge_count = graph.edges.size();
		if (workspace_context &&
			(workspace_context.workspace->closed() ||
			 workspace_context.workspace->generation() != workspace_context.publication->generation ||
			 workspace_context.workspace->analysis_revision() !=
				workspace_context.publication->analysis_revision))
			return;

		auto model = std::make_shared<cfg_model_snapshot_t>();
		model->blocks = std::move(blocks);
		model->graph = std::move(graph);
		model->entry_addr = entry_address;
		model->current_rip = debugger_engine::cached_registers().rip;
		model->entry_injections = std::move(entry_injections);
		model->node_lookup.reserve(model->graph.nodes.size());
		for (std::size_t node_index = 0; node_index < model->graph.nodes.size(); ++node_index)
			model->node_lookup.emplace(model->graph.nodes[node_index].id, node_index);
		model->generation = detail::model_store().next_model_generation.fetch_add(1,
			std::memory_order_acq_rel);
		publish_model(std::move(model));

		auto t_end = std::chrono::steady_clock::now();
		uint64_t dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
		diag::log_tagged_fmt("cfg", "build_cfg DONE entry=0x%llX blocks=%zu nodes=%zu edges=%zu duration_ms=%llu",
			static_cast<unsigned long long>(entry_address),
			block_count,
			node_count,
			edge_count,
			static_cast<unsigned long long>(dur_ms));
		} catch (const std::exception& ex) {
			diag::log_tagged_fmt("cfg", "build_cfg exception entry=0x%llX err='%s'",
				static_cast<unsigned long long>(entry_address), ex.what());
			detail::model_store().building.store(false, std::memory_order_release);
		} catch (...) {
			diag::log_tagged_fmt("cfg", "build_cfg exception entry=0x%llX err='<unknown>'",
				static_cast<unsigned long long>(entry_address));
			detail::model_store().building.store(false, std::memory_order_release);
		}
	};
		if (!aida::infra::executor::submit(std::move(sub)).submitted) {
			diag::log_tagged_fmt("cfg", "build_cfg worker_post_failed entry=0x%llX",
				static_cast<unsigned long long>(entry_address));
			detail::model_store().building.store(false, std::memory_order_release);
		}
	} catch (const std::exception& ex) {
		diag::log_tagged_fmt("cfg", "build_cfg worker_create_failed entry=0x%llX err='%s'",
			static_cast<unsigned long long>(entry_address), ex.what());
		detail::model_store().building.store(false, std::memory_order_release);
	} catch (...) {
		diag::log_tagged_fmt("cfg", "build_cfg worker_create_failed entry=0x%llX err='<unknown>'",
			static_cast<unsigned long long>(entry_address));
		detail::model_store().building.store(false, std::memory_order_release);
	}
}

inline void build_cfg(uint64_t entry_address)
{
	build_cfg(disasm_view::capture_selected_workspace(), entry_address);
}

struct workspace_graph_block_key_t {
	std::uint8_t function_space = 0;
	std::uint64_t function_value = 0;
	std::uint8_t block_space = 0;
	std::uint64_t block_value = 0;

	friend bool operator==(const workspace_graph_block_key_t& left,
		const workspace_graph_block_key_t& right) noexcept
	{
		return left.function_space == right.function_space &&
			left.function_value == right.function_value &&
			left.block_space == right.block_space &&
			left.block_value == right.block_value;
	}
};

struct workspace_graph_block_key_hash_t {
	std::size_t operator()(const workspace_graph_block_key_t& value) const noexcept
	{
		std::uint64_t hash = value.function_value;
		hash = aida::analysis_bridge::combine_extend(hash, value.block_value);
		hash ^= static_cast<std::uint64_t>(value.function_space) << 56u;
		hash ^= static_cast<std::uint64_t>(value.block_space) << 48u;
		return static_cast<std::size_t>(hash ^ (hash >> 32u));
	}
};

struct workspace_graph_layout_key_t {
	std::uint8_t function_space = 0;
	std::uint64_t function_value = 0;
	std::uint8_t page_space = 0;
	std::uint64_t page_value = 0;

	friend bool operator==(const workspace_graph_layout_key_t& left,
		const workspace_graph_layout_key_t& right) noexcept
	{
		return left.function_space == right.function_space &&
			left.function_value == right.function_value &&
			left.page_space == right.page_space &&
			left.page_value == right.page_value;
	}

	friend bool operator!=(const workspace_graph_layout_key_t& left,
		const workspace_graph_layout_key_t& right) noexcept
	{
		return !(left == right);
	}
};

struct workspace_graph_layout_key_hash_t {
	std::size_t operator()(const workspace_graph_layout_key_t& value) const noexcept
	{
		std::uint64_t hash = value.function_value;
		hash = aida::analysis_bridge::combine_extend(hash, value.page_value);
		hash ^= static_cast<std::uint64_t>(value.function_space) << 56u;
		hash ^= static_cast<std::uint64_t>(value.page_space) << 48u;
		return static_cast<std::size_t>(hash ^ (hash >> 32u));
	}
};

struct workspace_graph_layout_node_key_t {
	workspace_graph_layout_key_t layout;
	workspace_graph_block_key_t block;

	friend bool operator==(const workspace_graph_layout_node_key_t& left,
		const workspace_graph_layout_node_key_t& right) noexcept
	{
		return left.layout == right.layout && left.block == right.block;
	}
};

struct workspace_graph_layout_node_key_hash_t {
	std::size_t operator()(const workspace_graph_layout_node_key_t& value) const noexcept
	{
		const auto left = workspace_graph_layout_key_hash_t{}(value.layout);
		const auto right = workspace_graph_block_key_hash_t{}(value.block);
		return left ^ (right + static_cast<std::size_t>(
			aida::analysis_bridge::k_golden_gamma) + (left << 6u) + (left >> 2u));
	}
};

struct workspace_graph_edge_key_t {
	aida::analysis::entity_id_t source = 0;
	aida::analysis::entity_id_t target = 0;
	aida::analysis::edge_kind_t kind = aida::analysis::edge_kind_t::fallthrough;

	friend bool operator==(const workspace_graph_edge_key_t& left,
		const workspace_graph_edge_key_t& right) noexcept
	{
		return left.source == right.source && left.target == right.target &&
			left.kind == right.kind;
	}
};

struct workspace_graph_edge_key_hash_t {
	std::size_t operator()(const workspace_graph_edge_key_t& value) const noexcept
	{
		std::uint64_t hash = value.source;
		hash = aida::analysis_bridge::combine_extend(hash, value.target);
		hash ^= static_cast<std::uint64_t>(value.kind) << 56u;
		return static_cast<std::size_t>(hash ^ (hash >> 32u));
	}
};

inline workspace_graph_block_key_t workspace_graph_block_key(
	const aida::analysis::function_record_t& function,
	const aida::analysis::basic_block_record_t& block) noexcept
{
	return {static_cast<std::uint8_t>(function.start.space), function.start.value,
		static_cast<std::uint8_t>(block.start.space), block.start.value};
}

inline workspace_graph_layout_key_t workspace_graph_layout_key(
	const aida::analysis::function_record_t& function,
	const aida::analysis::basic_block_record_t& page_anchor) noexcept
{
	return {static_cast<std::uint8_t>(function.start.space), function.start.value,
		static_cast<std::uint8_t>(page_anchor.start.space), page_anchor.start.value};
}

struct workspace_graph_view_state_t {
	std::size_t block_page = 0;
	std::uint64_t layout_signature = 0;
	cfg_layout::graph_t layout;
	std::vector<std::size_t> block_indices;
	struct edge_t {
		int from = 0;
		int to = 0;
		aida::analysis::edge_kind_t kind = aida::analysis::edge_kind_t::fallthrough;
	};
	std::vector<edge_t> edges;
	bool edge_set_truncated = false;
	std::vector<int> outgoing;
	std::unordered_map<aida::analysis::entity_id_t, std::size_t> node_by_entity;
	std::unordered_map<aida::analysis::entity_id_t,
		std::vector<aida::analysis::entity_id_t>> successors;
	std::unordered_map<aida::analysis::entity_id_t,
		std::vector<aida::analysis::entity_id_t>> predecessors;
	std::unordered_map<aida::analysis::entity_id_t, std::size_t> page_block_by_entity;
	std::optional<aida::analysis::entity_id_t> selected_block;
	std::optional<aida::analysis::entity_id_t> selected_instruction;
	std::uint64_t selected_address = 0;
	std::unordered_set<workspace_graph_block_key_t, workspace_graph_block_key_hash_t>
		collapsed_reachable_roots;
	std::unordered_map<workspace_graph_block_key_t, cfg_vec2_t,
		workspace_graph_block_key_hash_t> pinned_node_positions;
	std::unordered_set<workspace_graph_layout_key_t, workspace_graph_layout_key_hash_t>
		pinned_layouts;
	std::unordered_map<workspace_graph_layout_node_key_t, cfg_vec2_t,
		workspace_graph_layout_node_key_hash_t> pinned_layout_positions;
	std::optional<workspace_graph_layout_key_t> current_layout;
	bool persisted_state_dirty = false;
	std::uint32_t persisted_state_version = 3;
};

inline constexpr std::size_t k_workspace_graph_persisted_entry_limit = 32;
inline constexpr std::size_t k_workspace_graph_persisted_item_limit = 256;
inline constexpr std::size_t k_workspace_graph_edge_candidate_limit = 65536;
inline constexpr std::size_t k_workspace_graph_edge_limit = 4096;

inline void workspace_graph_load_persisted(const disasm_view::workspace_context_t& context,
	workspace_graph_view_state_t& view)
{
	if (!context.workspace || g_sa_settings.workspace.graph_state_json.empty())
		return;
	const auto root = nlohmann::json::parse(g_sa_settings.workspace.graph_state_json,
		nullptr, false);
	const auto version = root.is_object() ? root.find("version") : root.end();
	const auto entries = root.is_object() ? root.find("entries") : root.end();
	const std::uint64_t root_version = version != root.end() && version->is_number_unsigned()
		? version->get<std::uint64_t>() : 0U;
	if (root.is_discarded() || !root.is_object() || version == root.end() ||
		!version->is_number_unsigned() || (root_version != 2U && root_version != 3U) ||
		entries == root.end() || !entries->is_array()) {
		view.persisted_state_version = 0;
		return;
	}
	const std::string binary_id = context.workspace->identity().binary_id().to_hex();
	std::size_t inspected_entries = 0;
	for (const auto& entry : *entries) {
		if (inspected_entries++ >= k_workspace_graph_persisted_entry_limit)
			break;
		const auto entry_binary = entry.is_object() ? entry.find("binary_id") : entry.end();
		if (!entry.is_object() || entry_binary == entry.end() ||
			!entry_binary->is_string() || entry_binary->get<std::string>() != binary_id)
			continue;
		const auto entry_version = entry.find("version");
		const std::uint64_t entry_version_value = entry_version != entry.end() &&
			entry_version->is_number_unsigned()
			? entry_version->get<std::uint64_t>() : 0U;
		if ((entry_version_value != 2U && entry_version_value != 3U) ||
			entry_version_value != root_version) {
			view.persisted_state_version = 0;
			return;
		}
		view.persisted_state_version = static_cast<std::uint32_t>(entry_version_value);
		view.persisted_state_dirty = view.persisted_state_version == 2U;
		if (entry.contains("collapsed") && entry["collapsed"].is_array()) {
			std::size_t inspected_collapsed = 0;
			for (const auto& value : entry["collapsed"]) {
				if (inspected_collapsed++ >= k_workspace_graph_persisted_item_limit)
					break;
				if (!value.is_object())
					continue;
				const auto function_space = value.find("function_space");
				const auto function_value = value.find("function_value");
				const auto block_space = value.find("block_space");
				const auto block_value = value.find("block_value");
				if (function_space == value.end() || function_value == value.end() ||
					block_space == value.end() || block_value == value.end() ||
					!function_space->is_number_unsigned() ||
					function_space->get<std::uint64_t>() > 3U ||
					!function_value->is_number_unsigned() ||
					!block_space->is_number_unsigned() ||
					block_space->get<std::uint64_t>() > 3U ||
					!block_value->is_number_unsigned())
					continue;
				view.collapsed_reachable_roots.insert({
					static_cast<std::uint8_t>(function_space->get<std::uint64_t>()),
					function_value->get<std::uint64_t>(),
					static_cast<std::uint8_t>(block_space->get<std::uint64_t>()),
					block_value->get<std::uint64_t>()});
			}
		}
		if (entry.contains("pins") && entry["pins"].is_array()) {
			std::size_t inspected_pins = 0;
			for (const auto& pin : entry["pins"]) {
				if (inspected_pins++ >= k_workspace_graph_persisted_item_limit)
					break;
				if (!pin.is_object())
					continue;
				const auto function_space = pin.find("function_space");
				const auto function_value = pin.find("function_value");
				const auto block_space = pin.find("block_space");
				const auto block_value = pin.find("block_value");
				const auto x_value = pin.find("x");
				const auto y_value = pin.find("y");
				if (function_space == pin.end() || function_value == pin.end() ||
					block_space == pin.end() || block_value == pin.end() ||
					x_value == pin.end() || y_value == pin.end() ||
					!function_space->is_number_unsigned() ||
					function_space->get<std::uint64_t>() > 3U ||
					!function_value->is_number_unsigned() ||
					!block_space->is_number_unsigned() ||
					block_space->get<std::uint64_t>() > 3U ||
					!block_value->is_number_unsigned() ||
					!x_value->is_number() || !y_value->is_number())
					continue;
				const float x = x_value->get<float>();
				const float y = y_value->get<float>();
				if (!std::isfinite(x) || !std::isfinite(y) ||
					std::abs(x) > 1000000.0f || std::abs(y) > 1000000.0f)
					continue;
				view.pinned_node_positions.emplace(workspace_graph_block_key_t{
					static_cast<std::uint8_t>(function_space->get<std::uint64_t>()),
					function_value->get<std::uint64_t>(),
					static_cast<std::uint8_t>(block_space->get<std::uint64_t>()),
					block_value->get<std::uint64_t>()}, cfg_vec2_t{x, y});
			}
		}
		if (view.persisted_state_version == 3U && entry.contains("layout_pins") &&
			entry["layout_pins"].is_array()) {
			std::size_t inspected_layout_pins = 0;
			for (const auto& pin : entry["layout_pins"]) {
				if (inspected_layout_pins++ >= k_workspace_graph_persisted_item_limit)
					break;
				if (!pin.is_object())
					continue;
				const auto fs = pin.find("function_space");
				const auto fv = pin.find("function_value");
				const auto ps = pin.find("page_space");
				const auto pv = pin.find("page_value");
				const auto bs = pin.find("block_space");
				const auto bv = pin.find("block_value");
				const auto x = pin.find("x");
				const auto y = pin.find("y");
				if (fs == pin.end() || fv == pin.end() || ps == pin.end() ||
					pv == pin.end() || bs == pin.end() || bv == pin.end() ||
					x == pin.end() || y == pin.end() || !fs->is_number_unsigned() ||
					fs->get<std::uint64_t>() > 3U || !fv->is_number_unsigned() ||
					!ps->is_number_unsigned() || ps->get<std::uint64_t>() > 3U ||
					!pv->is_number_unsigned() || !bs->is_number_unsigned() ||
					bs->get<std::uint64_t>() > 3U || !bv->is_number_unsigned() ||
					!x->is_number() || !y->is_number())
					continue;
				const float px = x->get<float>();
				const float py = y->get<float>();
				if (!std::isfinite(px) || !std::isfinite(py) ||
					std::abs(px) > 1000000.0f || std::abs(py) > 1000000.0f)
					continue;
				workspace_graph_layout_node_key_t key{{
					static_cast<std::uint8_t>(fs->get<std::uint64_t>()),
					fv->get<std::uint64_t>(),
					static_cast<std::uint8_t>(ps->get<std::uint64_t>()),
					pv->get<std::uint64_t>()}, {
					static_cast<std::uint8_t>(fs->get<std::uint64_t>()),
					fv->get<std::uint64_t>(),
					static_cast<std::uint8_t>(bs->get<std::uint64_t>()),
					bv->get<std::uint64_t>()}};
			view.pinned_layouts.insert(key.layout);
			view.pinned_layout_positions.emplace(key, cfg_vec2_t{px, py});
		}
	}
	return;
}
	if (root_version == 2U) {
		view.persisted_state_version = 2;
		view.persisted_state_dirty = true;
	}
}

inline bool workspace_graph_save_persisted(const disasm_view::workspace_context_t& context,
	const workspace_graph_view_state_t& view)
{
	if (!context.workspace ||
		view.collapsed_reachable_roots.size() > k_workspace_graph_persisted_item_limit ||
		view.pinned_node_positions.size() > k_workspace_graph_persisted_item_limit ||
		view.pinned_layout_positions.size() > k_workspace_graph_persisted_item_limit ||
		view.pinned_layouts.size() > k_workspace_graph_persisted_item_limit)
		return false;
	nlohmann::json root = nlohmann::json::parse(g_sa_settings.workspace.graph_state_json,
		nullptr, false);
	const auto version = root.is_object() ? root.find("version") : root.end();
	const std::uint64_t root_version = version != root.end() && version->is_number_unsigned()
		? version->get<std::uint64_t>() : 0U;
	if (root.is_discarded() || !root.is_object() || version == root.end() ||
		!version->is_number_unsigned() || (root_version != 2U && root_version != 3U))
		root = nlohmann::json{{"version", 3U}, {"entries", nlohmann::json::array()}};
	else if (root_version == 2U) {
		root["version"] = 3U;
		if (root.contains("entries") && root["entries"].is_array()) {
			for (auto& entry : root["entries"]) {
				if (!entry.is_object())
					continue;
				entry["version"] = 3U;
				entry.erase("layout_pinned");
				entry["layout_pins"] = nlohmann::json::array();
			}
		}
	}
	if (!root.contains("entries") || !root["entries"].is_array())
		root["entries"] = nlohmann::json::array();
	const std::string binary_id = context.workspace->identity().binary_id().to_hex();
	auto& entries = root["entries"];
	for (auto iterator = entries.begin(); iterator != entries.end();) {
		const auto entry_binary = iterator->is_object()
			? iterator->find("binary_id") : iterator->end();
		if (!iterator->is_object() || entry_binary == iterator->end() ||
			!entry_binary->is_string() || entry_binary->get<std::string>() == binary_id)
			iterator = entries.erase(iterator);
		else
			++iterator;
	}
	nlohmann::json collapsed = nlohmann::json::array();
	for (const auto& key : view.collapsed_reachable_roots)
		collapsed.push_back({{"function_space", key.function_space},
			{"function_value", key.function_value}, {"block_space", key.block_space},
			{"block_value", key.block_value}});
	nlohmann::json pins = nlohmann::json::array();
	for (const auto& item : view.pinned_node_positions)
		pins.push_back({{"function_space", item.first.function_space},
			{"function_value", item.first.function_value},
			{"block_space", item.first.block_space}, {"block_value", item.first.block_value},
			{"x", item.second.x}, {"y", item.second.y}});
	nlohmann::json layout_pins = nlohmann::json::array();
	for (const auto& item : view.pinned_layout_positions)
		layout_pins.push_back({{"function_space", item.first.layout.function_space},
			{"function_value", item.first.layout.function_value},
			{"page_space", item.first.layout.page_space},
			{"page_value", item.first.layout.page_value},
			{"block_space", item.first.block.block_space},
			{"block_value", item.first.block.block_value},
			{"x", item.second.x}, {"y", item.second.y}});
	entries.insert(entries.begin(), nlohmann::json{{"version", 3U},
		{"binary_id", binary_id},
		{"collapsed", std::move(collapsed)}, {"pins", std::move(pins)}});
	entries.front()["layout_pins"] = std::move(layout_pins);
	while (entries.size() > k_workspace_graph_persisted_entry_limit)
		entries.erase(entries.end() - 1);
	const std::string encoded = root.dump();
	if (encoded.size() > 256U * 1024U)
		return false;
	const std::string previous = g_sa_settings.workspace.graph_state_json;
	g_sa_settings.workspace.graph_state_json = encoded;
	if (aida::settings_persistence::accepted(
			aida::settings_persistence::request_save(g_sa_settings)))
		return true;
	g_sa_settings.workspace.graph_state_json = previous;
	return false;
}

inline std::mutex& workspace_graph_registry_mutex()
{
	static std::mutex value;
	return value;
}

struct workspace_graph_registry_entry_t {
	std::shared_ptr<workspace_graph_view_state_t> view;
	std::uint64_t access = 0;
};

inline std::unordered_map<aida::analysis::binary_id_t,
	workspace_graph_registry_entry_t,
	aida::analysis::binary_id_hash_t>& workspace_graph_registry()
{
	static std::unordered_map<aida::analysis::binary_id_t,
		workspace_graph_registry_entry_t,
		aida::analysis::binary_id_hash_t> value;
	return value;
}

inline std::uint64_t& workspace_graph_registry_clock()
{
	static std::uint64_t value = 0;
	return value;
}

inline std::shared_ptr<workspace_graph_view_state_t> workspace_graph_state(
	const disasm_view::workspace_context_t& context)
{
	if (!context.workspace)
		return {};
	std::lock_guard<std::mutex> lock(workspace_graph_registry_mutex());
	auto& registry = workspace_graph_registry();
	const auto id = context.workspace->identity().binary_id();
	auto found = registry.find(id);
	if (found != registry.end()) {
		found->second.access = ++workspace_graph_registry_clock();
		return found->second.view;
	}
	if (registry.size() >= k_workspace_graph_persisted_entry_limit) {
		const auto oldest = std::min_element(registry.begin(), registry.end(),
			[](const auto& left, const auto& right) {
				return left.second.access < right.second.access;
			});
		if (oldest != registry.end())
			registry.erase(oldest);
	}
	auto created = std::make_shared<workspace_graph_view_state_t>();
	workspace_graph_load_persisted(context, *created);
	registry.emplace(id, workspace_graph_registry_entry_t{
		created, ++workspace_graph_registry_clock()});
	return created;
}

inline const aida::analysis::function_record_t* workspace_graph_function(
	const disasm_view::workspace_context_t& context)
{
	if (!context.publication || !context.publication->snapshot ||
		context.publication->snapshot->functions.empty())
		return nullptr;
	const auto& functions = context.publication->snapshot->functions;
	const auto selection = context.workspace->view_state().selection;
	if (!selection)
		return &functions.front();
	auto found = std::upper_bound(functions.begin(), functions.end(), *selection,
		[](const aida::analysis::address_t& address,
		   const aida::analysis::function_record_t& function) {
			return address < function.start;
		});
	if (found == functions.begin())
		return &functions.front();
	--found;
	if (found->start.space == selection->space &&
		selection->value >= found->start.value && selection->value < found->end.value)
		return &*found;
	return &functions.front();
}

inline std::uint64_t workspace_graph_generation(
	const disasm_view::workspace_context_t& context)
{
	return aida::analysis_bridge::combine_generation_revision(
		context.workspace->generation(), context.workspace->analysis_revision());
}

inline std::uint64_t workspace_graph_layout_signature(
	const disasm_view::workspace_context_t& context,
	const aida::analysis::function_record_t& function,
	std::size_t page)
{
	std::uint64_t value = workspace_graph_generation(context);
	value = aida::analysis_bridge::combine_extend(value, function.id);
	value = aida::analysis_bridge::combine_extend(value,
		static_cast<std::uint64_t>(page));
	return value == 0 ? 1 : value;
}

inline bool workspace_graph_cfg_edge_kind(aida::analysis::edge_kind_t kind) noexcept
{
	switch (kind) {
	case aida::analysis::edge_kind_t::fallthrough:
	case aida::analysis::edge_kind_t::conditional_taken:
	case aida::analysis::edge_kind_t::unconditional:
	case aida::analysis::edge_kind_t::exception_edge:
	case aida::analysis::edge_kind_t::indirect:
		return true;
	case aida::analysis::edge_kind_t::call:
	case aida::analysis::edge_kind_t::tail_call:
	case aida::analysis::edge_kind_t::return_edge:
		return false;
	}
	return false;
}

inline const char* workspace_graph_edge_label(aida::analysis::edge_kind_t kind,
	bool branching)
{
	switch (kind) {
	case aida::analysis::edge_kind_t::fallthrough: return branching ? "F" : nullptr;
	case aida::analysis::edge_kind_t::conditional_taken: return "T";
	case aida::analysis::edge_kind_t::unconditional: return "J";
	case aida::analysis::edge_kind_t::call: return "CALL";
	case aida::analysis::edge_kind_t::tail_call: return "TAIL";
	case aida::analysis::edge_kind_t::return_edge: return "RET";
	case aida::analysis::edge_kind_t::exception_edge: return "EX";
	case aida::analysis::edge_kind_t::indirect: return "IND";
	}
	return nullptr;
}

inline bool workspace_graph_contains_block_key(
	const aida::analysis::analysis_snapshot_t& snapshot,
	const workspace_graph_block_key_t& key)
{
	const auto function = std::lower_bound(snapshot.functions.begin(), snapshot.functions.end(), key,
		[](const aida::analysis::function_record_t& candidate,
		   const workspace_graph_block_key_t& value) {
			const auto space = static_cast<std::uint8_t>(candidate.start.space);
			return space < value.function_space ||
				(space == value.function_space && candidate.start.value < value.function_value);
		});
	if (function == snapshot.functions.end() ||
		static_cast<std::uint8_t>(function->start.space) != key.function_space ||
		function->start.value != key.function_value)
		return false;
	const std::size_t begin = function->first_block;
	if (begin > snapshot.blocks.size())
		return false;
	const std::size_t available = begin <= snapshot.blocks.size()
		? snapshot.blocks.size() - begin : 0;
	const std::size_t count = (std::min)(
		static_cast<std::size_t>(function->block_count), available);
	const auto first = snapshot.blocks.begin() + static_cast<std::ptrdiff_t>(begin);
	const auto last = first + static_cast<std::ptrdiff_t>(count);
	const auto block = std::lower_bound(first, last, key,
		[](const aida::analysis::basic_block_record_t& candidate,
		   const workspace_graph_block_key_t& value) {
			const auto space = static_cast<std::uint8_t>(candidate.start.space);
			return space < value.block_space ||
				(space == value.block_space && candidate.start.value < value.block_value);
		});
	return block != last && block->function_id == function->id &&
		static_cast<std::uint8_t>(block->start.space) == key.block_space &&
		block->start.value == key.block_value;
}

inline bool workspace_graph_contains_layout_key(
	const aida::analysis::analysis_snapshot_t& snapshot,
	const workspace_graph_layout_key_t& key)
{
	return workspace_graph_contains_block_key(snapshot, {
		key.function_space, key.function_value, key.page_space, key.page_value});
}

inline void workspace_graph_rebuild_layout(
	workspace_graph_view_state_t& view,
	const disasm_view::workspace_context_t& context,
	const aida::analysis::function_record_t& function,
	std::size_t page_begin, std::size_t page_end)
{
	const auto& snapshot = *context.publication->snapshot;
	const auto old_collapsed = view.collapsed_reachable_roots;
	const auto old_positions = view.pinned_node_positions;
	const auto old_pinned_layouts = view.pinned_layouts;
	const auto old_layout_positions = view.pinned_layout_positions;
	const auto old_persisted_version = view.persisted_state_version;
	bool persisted_state_changed = view.persisted_state_dirty;
	if (view.persisted_state_version != 3) {
		if (view.persisted_state_version != 2) {
			view.collapsed_reachable_roots.clear();
			view.pinned_node_positions.clear();
		}
		view.pinned_layouts.clear();
		view.pinned_layout_positions.clear();
		view.persisted_state_version = 3;
		persisted_state_changed = true;
	}
	std::unordered_set<workspace_graph_block_key_t, workspace_graph_block_key_hash_t>
		persisted_keys;
	persisted_keys.reserve(view.collapsed_reachable_roots.size() +
		view.pinned_node_positions.size());
	for (const auto& key : view.collapsed_reachable_roots) persisted_keys.insert(key);
	for (const auto& item : view.pinned_node_positions) persisted_keys.insert(item.first);
	for (const auto& item : view.pinned_layout_positions) persisted_keys.insert(item.first.block);
	std::unordered_set<workspace_graph_layout_key_t, workspace_graph_layout_key_hash_t>
		valid_layout_keys;
	valid_layout_keys.reserve(view.pinned_layouts.size());
	std::unordered_set<workspace_graph_block_key_t, workspace_graph_block_key_hash_t>
		valid_persisted_keys;
	valid_persisted_keys.reserve(persisted_keys.size());
	for (const auto& key : persisted_keys)
		if (workspace_graph_contains_block_key(snapshot, key))
			valid_persisted_keys.insert(key);
	for (const auto& key : view.pinned_layouts)
		if (workspace_graph_contains_layout_key(snapshot, key))
			valid_layout_keys.insert(key);
	for (auto iterator = view.collapsed_reachable_roots.begin();
		iterator != view.collapsed_reachable_roots.end();) {
		if (valid_persisted_keys.count(*iterator) == 0) {
			iterator = view.collapsed_reachable_roots.erase(iterator);
			persisted_state_changed = true;
		}
		else ++iterator;
	}
	for (auto iterator = view.pinned_node_positions.begin();
		iterator != view.pinned_node_positions.end();) {
		if (valid_persisted_keys.count(iterator->first) == 0) {
			iterator = view.pinned_node_positions.erase(iterator);
			persisted_state_changed = true;
		}
		else ++iterator;
	}
	for (auto iterator = view.pinned_layouts.begin(); iterator != view.pinned_layouts.end();) {
		if (valid_layout_keys.count(*iterator) == 0) {
			iterator = view.pinned_layouts.erase(iterator);
			persisted_state_changed = true;
		} else ++iterator;
	}
	for (auto iterator = view.pinned_layout_positions.begin();
		iterator != view.pinned_layout_positions.end();) {
		if (valid_layout_keys.count(iterator->first.layout) == 0 ||
			valid_persisted_keys.count(iterator->first.block) == 0) {
			iterator = view.pinned_layout_positions.erase(iterator);
			persisted_state_changed = true;
		} else ++iterator;
	}
	if (persisted_state_changed && !workspace_graph_save_persisted(context, view)) {
		view.collapsed_reachable_roots = old_collapsed;
		view.pinned_node_positions = old_positions;
		view.pinned_layouts = old_pinned_layouts;
		view.pinned_layout_positions = old_layout_positions;
		view.persisted_state_version = old_persisted_version;
		view.persisted_state_dirty = true;
		view.layout_signature = 0;
	} else if (persisted_state_changed) {
		view.persisted_state_dirty = false;
	}
	view.layout = {};
	view.block_indices.clear();
	view.edges.clear();
	view.edge_set_truncated = false;
	view.outgoing.clear();
	view.node_by_entity.clear();
	view.successors.clear();
	view.predecessors.clear();
	view.page_block_by_entity.clear();
	view.block_indices.reserve(page_end - page_begin);
	view.layout.nodes.reserve(page_end - page_begin);
	std::vector<std::size_t> page_block_indices;
	page_block_indices.reserve(page_end - page_begin);
	std::unordered_set<aida::analysis::entity_id_t> page_block_ids;
	page_block_ids.reserve(page_end - page_begin);
	std::unordered_map<aida::analysis::entity_id_t, workspace_graph_block_key_t>
		page_block_keys;
	page_block_keys.reserve(page_end - page_begin);
	view.page_block_by_entity.reserve(page_end - page_begin);
	for (std::size_t local = page_begin; local < page_end; ++local) {
		const std::size_t index = static_cast<std::size_t>(function.first_block) + local;
		if (index >= snapshot.blocks.size())
			break;
		const auto& block = snapshot.blocks[index];
		page_block_indices.push_back(index);
		page_block_ids.insert(block.id);
		page_block_keys.emplace(block.id, workspace_graph_block_key(function, block));
		view.page_block_by_entity.emplace(block.id, index);
	}
	view.current_layout = page_block_indices.empty() ?
		std::optional<workspace_graph_layout_key_t>{} :
		std::optional<workspace_graph_layout_key_t>{workspace_graph_layout_key(
			function, snapshot.blocks[page_block_indices.front()])};
	std::vector<const aida::analysis::edge_record_t*> page_cfg_edges;
	page_cfg_edges.reserve((std::min)(k_workspace_graph_edge_limit,
		page_block_indices.size() * 4U));
	if (!page_block_indices.empty()) {
		auto range_begin = snapshot.blocks[page_block_indices.front()].start;
		auto range_end = snapshot.blocks[page_block_indices.front()].end;
		for (const auto index : page_block_indices) {
			range_begin = (std::min)(range_begin, snapshot.blocks[index].start);
			range_end = (std::max)(range_end, snapshot.blocks[index].end);
		}
		auto edge = std::lower_bound(snapshot.edges.begin(), snapshot.edges.end(), range_begin,
			[](const aida::analysis::edge_record_t& candidate,
			   const aida::analysis::address_t& address) {
				return candidate.source < address;
			});
		std::unordered_set<workspace_graph_edge_key_t, workspace_graph_edge_key_hash_t>
			deduplicated;
		deduplicated.reserve((std::min)(k_workspace_graph_edge_limit,
			page_block_indices.size() * 4U));
		std::size_t inspected = 0;
		for (; edge != snapshot.edges.end() && edge->source < range_end &&
			inspected < k_workspace_graph_edge_candidate_limit; ++edge, ++inspected) {
			if (!edge->target_entity || !workspace_graph_cfg_edge_kind(edge->kind) ||
				page_block_ids.count(edge->source_entity) == 0 ||
				page_block_ids.count(*edge->target_entity) == 0)
				continue;
			const workspace_graph_edge_key_t key{edge->source_entity,
				*edge->target_entity, edge->kind};
			if (!deduplicated.insert(key).second)
				continue;
			if (page_cfg_edges.size() >= k_workspace_graph_edge_limit) {
				view.edge_set_truncated = true;
				break;
			}
			page_cfg_edges.push_back(&*edge);
		}
		if (inspected >= k_workspace_graph_edge_candidate_limit &&
			edge != snapshot.edges.end() && edge->source < range_end)
			view.edge_set_truncated = true;
	}
	view.successors.reserve(page_block_ids.size());
	view.predecessors.reserve(page_block_ids.size());
	for (const auto* edge : page_cfg_edges) {
		view.successors[edge->source_entity].push_back(*edge->target_entity);
		view.predecessors[*edge->target_entity].push_back(edge->source_entity);
	}
	std::unordered_set<aida::analysis::entity_id_t> hidden;
	std::unordered_set<aida::analysis::entity_id_t> protected_roots;
	std::vector<aida::analysis::entity_id_t> frontier;
	for (const auto& item : page_block_keys) {
		if (view.collapsed_reachable_roots.count(item.second) != 0) {
			protected_roots.insert(item.first);
			frontier.push_back(item.first);
		}
	}
	std::unordered_set<aida::analysis::entity_id_t> visited = protected_roots;
	for (std::size_t cursor = 0; cursor < frontier.size(); ++cursor) {
		const auto adjacent = view.successors.find(frontier[cursor]);
		if (adjacent == view.successors.end())
			continue;
		for (const auto target : adjacent->second) {
			if (protected_roots.count(target) == 0)
				hidden.insert(target);
			if (visited.insert(target).second)
				frontier.push_back(target);
		}
	}
	for (const auto index : page_block_indices) {
		const auto& block = snapshot.blocks[index];
		if (hidden.count(block.id) != 0)
			continue;
		const int node_id = static_cast<int>(view.layout.nodes.size());
		const std::size_t shown = (std::min)(static_cast<std::size_t>(block.instruction_count),
			static_cast<std::size_t>(14));
		cfg_layout::node_t node;
		node.id = node_id;
		node.width = 410.0f;
		node.height = 43.0f + static_cast<float>(shown) * 19.0f +
			(block.instruction_count > shown ? 20.0f : 8.0f);
		node.addr_col_w = 112.0f;
		node.is_entry = index == static_cast<std::size_t>(function.first_block);
		view.node_by_entity.emplace(block.id, view.layout.nodes.size());
		view.block_indices.push_back(index);
		view.layout.nodes.push_back(node);
	}
	for (const auto* edge : page_cfg_edges) {
		const auto from = view.node_by_entity.find(edge->source_entity);
		const auto to = view.node_by_entity.find(*edge->target_entity);
		if (from == view.node_by_entity.end() || to == view.node_by_entity.end())
			continue;
		workspace_graph_view_state_t::edge_t visual;
		visual.from = static_cast<int>(from->second);
		visual.to = static_cast<int>(to->second);
		visual.kind = edge->kind;
		view.edges.push_back(visual);
		if (visual.to > visual.from) {
			cfg_layout::edge_t layout_edge;
			layout_edge.from = visual.from;
			layout_edge.to = visual.to;
			layout_edge.is_true_branch = edge->kind ==
				aida::analysis::edge_kind_t::conditional_taken;
			view.layout.edges.push_back(layout_edge);
		}
	}
	view.outgoing.assign(view.layout.nodes.size(), 0);
	for (const auto& edge : view.edges) {
		if (edge.from >= 0 && static_cast<std::size_t>(edge.from) < view.outgoing.size())
			++view.outgoing[static_cast<std::size_t>(edge.from)];
	}
	cfg_layout::layout(view.layout, 82.0f, 94.0f);
	for (std::size_t index = 0; index < view.block_indices.size(); ++index) {
		const auto& block = snapshot.blocks[view.block_indices[index]];
		const auto block_key = workspace_graph_block_key(function, block);
		const auto pinned = view.pinned_node_positions.find(block_key);
		if (pinned != view.pinned_node_positions.end()) {
			view.layout.nodes[index].x = pinned->second.x;
			view.layout.nodes[index].y = pinned->second.y;
		} else if (view.current_layout &&
			view.pinned_layouts.count(*view.current_layout) != 0) {
			const auto layout_pinned = view.pinned_layout_positions.find(
				workspace_graph_layout_node_key_t{*view.current_layout, block_key});
			if (layout_pinned != view.pinned_layout_positions.end()) {
				view.layout.nodes[index].x = layout_pinned->second.x;
				view.layout.nodes[index].y = layout_pinned->second.y;
			}
		}
	}
	if (view.selected_block &&
		view.node_by_entity.find(*view.selected_block) == view.node_by_entity.end()) {
		view.selected_block.reset();
		view.selected_instruction.reset();
		view.selected_address = 0;
	}
}

inline std::optional<std::uint64_t> workspace_graph_direct_target(
	const disasm_view::workspace_context_t& context,
	const aida::analysis::instruction_record_t& instruction)
{
	if (!context.publication || !context.publication->snapshot ||
		instruction.target_fact_count == 0)
		return std::nullopt;
	const auto& facts = context.publication->snapshot->target_facts;
	const std::size_t begin = instruction.target_fact_begin;
	const std::size_t end = (std::min)(facts.size(),
		begin + static_cast<std::size_t>(instruction.target_fact_count));
	for (std::size_t index = begin; index < end; ++index) {
		if (!facts[index].direct)
			continue;
		return disasm_view::runtime_address(context, facts[index].target).value_or(
			facts[index].target.value);
	}
	return std::nullopt;
}

inline const aida::analysis::instruction_record_t* workspace_graph_selected_instruction(
	const workspace_graph_view_state_t& view,
	const aida::analysis::analysis_snapshot_t& snapshot)
{
	if (!view.selected_instruction)
		return nullptr;
	const auto found = std::find_if(snapshot.instructions.begin(), snapshot.instructions.end(),
		[&](const aida::analysis::instruction_record_t& instruction) {
			return instruction.id == *view.selected_instruction;
		});
	return found == snapshot.instructions.end() ? nullptr : &*found;
}

}
