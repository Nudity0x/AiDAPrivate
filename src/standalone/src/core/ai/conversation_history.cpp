#include "conversation_history.hpp"

#include "../helpers/globals.h"
#include "standalone_chat.hpp"
#include "conversation_evidence_store.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


std::vector<ConversationSummary> conversations::history;
std::shared_ptr<const std::vector<ConversationSummary>> conversations::published_history =
	std::make_shared<const std::vector<ConversationSummary>>();
std::string conversations::current_id;
bool conversations::browser_open = false;
std::uint64_t conversations::current_revision = 0;
bool conversations::current_identity_uncommitted = false;
std::uint64_t conversations::catalog_generation = 0;
std::string conversations::persistence_error;

namespace {

std::function<void()>& catalog_change_hook_slot()
{
	static std::function<void()> hook;
	return hook;
}

void notify_catalog_changed()
{
	auto hook = catalog_change_hook_slot();
	if (hook) hook();
}

bool valid_conversation_id(std::string_view id)
{
	if (id.empty() || id.size() > 128U)
		return false;
	return std::all_of(id.begin(), id.end(), [](unsigned char value) {
		return std::isalnum(value) != 0 || value == '-' || value == '_';
	});
}

struct conversation_ui_transaction_t {
	std::uint64_t source_fingerprint = 0;
	std::uint64_t source_revision = 0;
	aida::conversation_store::operation_t operation =
		aida::conversation_store::operation_t::save;
	std::optional<aida::conversation_store::request_t> deferred_save;
};

conversation_ui_transaction_t& conversation_ui_transaction()
{
	static conversation_ui_transaction_t value;
	return value;
}

std::uint64_t conversation_fingerprint()
{
	std::uint64_t hash = 14695981039346656037ULL;
	auto append = [&](std::string_view value) {
		for (const char character : value) {
			hash ^= static_cast<unsigned char>(character);
			hash *= 1099511628211ULL;
		}
	};
	append(conversations::current_id);
	for (const auto& message : g_chat_messages) {
		append(message.text);
		append(message.thinking_text);
		append(message.model_id);
		hash ^= static_cast<std::uint64_t>(message.timestamp);
		hash *= 1099511628211ULL;
		hash ^= message.is_user ? 1ULL : 0ULL;
		hash *= 1099511628211ULL;
	}
	return hash;
}

aida::conversation_store::snapshot_t capture_conversation_snapshot(bool advance_revision)
{
	aida::conversation_store::snapshot_t snapshot;
	snapshot.id = conversations::current_id;
	const bool assigning_identity = snapshot.id.empty() && !g_chat_messages.empty();
	if (assigning_identity) {
		static std::uint64_t identity_sequence = 0;
		const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		snapshot.id = std::to_string(now);
		snapshot.id += "-" + std::to_string(::GetCurrentProcessId());
		snapshot.id += "-" + std::to_string(++identity_sequence);
		conversations::current_id = snapshot.id;
		conversations::current_identity_uncommitted = true;
	}
	snapshot.require_absent = conversations::current_identity_uncommitted;
	if (advance_revision && !snapshot.id.empty())
		++conversations::current_revision;
	snapshot.revision = conversations::current_revision;
	for (const auto& summary : conversations::history) {
		if (summary.id == snapshot.id) {
			snapshot.pinned = summary.pinned;
			snapshot.created = summary.created;
			snapshot.title = summary.title;
			break;
		}
	}
	if (snapshot.created == 0 && !g_chat_messages.empty())
		snapshot.created = g_chat_messages.front().timestamp;
	for (const auto& message : g_chat_messages) {
		if (snapshot.title.empty() && message.is_user && !message.text.empty())
			snapshot.title = message.text.substr(0, 80);
		aida::conversation_store::message_t persisted;
		persisted.text = message.text;
		persisted.thinking_text = message.thinking_text;
		persisted.is_user = message.is_user;
		persisted.has_thinking = message.has_thinking;
		persisted.timestamp = message.timestamp;
		persisted.input_tokens = message.input_tokens;
		persisted.output_tokens = message.output_tokens;
		persisted.cache_read_tokens = message.cache_read_tokens;
		persisted.cache_write_tokens = message.cache_write_tokens;
		persisted.model_id = message.model_id;
		snapshot.messages.push_back(std::move(persisted));
	}
	if (!snapshot.id.empty())
		snapshot.evidence = aida::automation_ui::persisted_evidence_snapshot(snapshot.id);
	snapshot.evidence_authoritative = snapshot.id.empty() ||
		aida::automation_ui::persisted_evidence_session_loaded(snapshot.id);
	return snapshot;
}

void publish_conversation_catalog()
{
	conversations::published_history =
		std::make_shared<const std::vector<ConversationSummary>>(conversations::history);
}

std::uint64_t summary_revision(std::string_view id)
{
	const auto found = std::find_if(conversations::history.begin(),
		conversations::history.end(), [&](const ConversationSummary& summary) {
			return summary.id == id;
		});
	return found == conversations::history.end() ? 0 : found->revision;
}

bool submit_conversation_request(aida::conversation_store::request_t request)
{
	conversation_ui_transaction_t& transaction = conversation_ui_transaction();
	const std::uint64_t fingerprint = conversation_fingerprint();
	const auto submitted = aida::conversation_store::submit(request);
	if (submitted == aida::conversation_store::request_result_t::busy &&
		request.operation == aida::conversation_store::operation_t::save) {
		transaction.deferred_save = std::move(request);
		return true;
	}
	const bool accepted = submitted == aida::conversation_store::request_result_t::queued ||
		submitted == aida::conversation_store::request_result_t::preview_recorded;
	if (accepted) {
		transaction.source_fingerprint = fingerprint;
		transaction.source_revision = request.current.revision;
		transaction.operation = request.operation;
	}
	return accepted;
}

}

void conversations::save_current()
{
	if (g_chat_messages.empty() && current_id.empty()) return;
	aida::conversation_store::request_t request;
	request.operation = aida::conversation_store::operation_t::save;
	request.current = capture_conversation_snapshot(true);
	request.catalog_generation = catalog_generation;
	static_cast<void>(submit_conversation_request(std::move(request)));
}

void conversations::load_conversation(const std::string& id)
{
	if (!valid_conversation_id(id)) return;
	aida::conversation_store::request_t request;
	request.operation = aida::conversation_store::operation_t::switch_conversation;
	request.current = capture_conversation_snapshot(true);
	request.target_id = id;
	request.target_revision = summary_revision(id);
	request.catalog_generation = catalog_generation;
	static_cast<void>(submit_conversation_request(std::move(request)));
}

void conversations::new_chat()
{
	const auto persistence = aida::conversation_store::status();
	if (persistence.pending || persistence.failed || is_ai_busy()) return;
	aida::conversation_store::request_t request;
	request.operation = aida::conversation_store::operation_t::new_conversation;
	request.current = capture_conversation_snapshot(true);
	request.catalog_generation = catalog_generation;
	static_cast<void>(submit_conversation_request(std::move(request)));
}

void conversations::refresh_history()
{
	aida::conversation_store::request_t request;
	request.operation = aida::conversation_store::operation_t::refresh_catalog;
	request.catalog_generation = catalog_generation;
	static_cast<void>(submit_conversation_request(std::move(request)));
	notify_catalog_changed();
}

void conversations::delete_conversation(const std::string& id,
	std::uint64_t reviewed_revision)
{
	if (!valid_conversation_id(id)) return;
	aida::conversation_store::request_t request;
	request.operation = aida::conversation_store::operation_t::delete_conversation;
	request.target_id = id;
	request.target_revision = reviewed_revision;
	request.catalog_generation = catalog_generation;
	static_cast<void>(submit_conversation_request(std::move(request)));
}

bool conversations::set_pinned(const std::string& id, bool pinned)
{
	if (!valid_conversation_id(id)) return false;
	aida::conversation_store::request_t request;
	request.operation = aida::conversation_store::operation_t::set_pinned;
	request.target_id = id;
	request.target_revision = summary_revision(id);
	request.catalog_generation = catalog_generation;
	request.pinned = pinned;
	return submit_conversation_request(std::move(request));
}

bool conversations::fork_conversation(const std::string& id, std::string& forked_id)
{
	forked_id.clear();
	if (!valid_conversation_id(id)) return false;
	aida::conversation_store::request_t request;
	request.operation = aida::conversation_store::operation_t::fork_conversation;
	request.current = capture_conversation_snapshot(true);
	request.target_id = id;
	request.target_revision = id == request.current.id
		? request.current.revision : summary_revision(id);
	request.catalog_generation = catalog_generation;
	return submit_conversation_request(std::move(request));
}

bool conversations::export_markdown(const std::string& id, const std::string& output_path,
	std::string& error)
{
	error.clear();
	if (!valid_conversation_id(id) || output_path.empty()) {
		error = "The conversation identity or export path is invalid.";
		return false;
	}
	aida::conversation_store::request_t request;
	request.operation = aida::conversation_store::operation_t::export_markdown;
	request.current = capture_conversation_snapshot(true);
	request.target_id = id;
	request.target_revision = id == request.current.id
		? request.current.revision : summary_revision(id);
	request.catalog_generation = catalog_generation;
	request.output_path = output_path;
	const bool queued = submit_conversation_request(std::move(request));
	if (!queued) error = "The conversation export could not be queued.";
	return queued;
}

void conversations::process_store_completion(bool allow_deferred)
{
	auto submit_deferred_save = [] {
		auto& transaction = conversation_ui_transaction();
		if (!transaction.deferred_save || aida::conversation_store::status().pending)
			return;
		auto request = std::move(*transaction.deferred_save);
		transaction.deferred_save.reset();
		request.catalog_generation = conversations::catalog_generation;
		if (!submit_conversation_request(std::move(request)))
			conversations::persistence_error = "The deferred conversation snapshot could not be queued.";
	};
	auto completion = aida::conversation_store::take_completion();
	if (!completion) {
		if (allow_deferred) submit_deferred_save();
		return;
	}
	if (!completion->success) {
		persistence_error = completion->error.empty()
			? "The conversation transaction failed." : completion->error;
		return;
	}
	const bool replaces_conversation =
		completion->operation == aida::conversation_store::operation_t::switch_conversation ||
		completion->operation == aida::conversation_store::operation_t::new_conversation ||
		completion->operation == aida::conversation_store::operation_t::fork_conversation;
	if (replaces_conversation &&
		(conversation_fingerprint() != conversation_ui_transaction().source_fingerprint ||
		 completion->source_revision != conversation_ui_transaction().source_revision)) {
		persistence_error = "The active conversation changed before the loaded transaction could be published.";
		return;
	}
	std::string publication_error;
	const bool publishes_catalog =
		completion->operation == aida::conversation_store::operation_t::save ||
		completion->operation == aida::conversation_store::operation_t::switch_conversation ||
		completion->operation == aida::conversation_store::operation_t::new_conversation ||
		completion->operation == aida::conversation_store::operation_t::refresh_catalog ||
		completion->operation == aida::conversation_store::operation_t::delete_conversation ||
		completion->operation == aida::conversation_store::operation_t::set_pinned ||
		completion->operation == aida::conversation_store::operation_t::fork_conversation ||
		completion->operation == aida::conversation_store::operation_t::export_markdown;
	bool catalog_published = false;
	bool local_catalog_changed = false;
	auto upsert_local_summary = [&](const aida::conversation_store::summary_t& summary) {
		auto found = std::find_if(history.begin(), history.end(),
			[&](const ConversationSummary& current) { return current.id == summary.id; });
		ConversationSummary replacement{summary.id, summary.title, summary.created,
			summary.message_count, summary.pinned, summary.revision};
		if (found == history.end()) history.push_back(std::move(replacement));
		else *found = std::move(replacement);
		local_catalog_changed = true;
	};
	if (publishes_catalog && completion->catalog_authoritative) {
		if (completion->source_catalog_generation != catalog_generation) {
			publication_error = "Conversation history changed before the catalog result could be published.";
		} else {
			std::vector<ConversationSummary> replacement;
			replacement.reserve(completion->catalog.size());
			for (const auto& summary : completion->catalog) {
				replacement.push_back({summary.id, summary.title, summary.created,
					summary.message_count, summary.pinned, summary.revision});
			}
			history = std::move(replacement);
			publish_conversation_catalog();
			++catalog_generation;
			catalog_published = true;
		}
	}
	if (completion->committed_summary) {
		const auto& committed = *completion->committed_summary;
		if (committed.id == current_id) current_revision = committed.revision;
		if (committed.id == current_id) current_identity_uncommitted = false;
		if (!catalog_published) upsert_local_summary(committed);
	}
	if (completion->loaded) {
		const auto& loaded = *completion->loaded;
		if (completion->operation == aida::conversation_store::operation_t::load_evidence) {
			if (loaded.id == current_id)
				aida::automation_ui::apply_persisted_evidence(loaded.id, loaded.evidence);
		} else {
			if (!catalog_published && !loaded.id.empty()) {
				upsert_local_summary({loaded.id, loaded.title, loaded.created,
					static_cast<int>(loaded.messages.size()), loaded.pinned,
					loaded.revision});
			}
			std::vector<ChatMessage> messages;
			messages.reserve(loaded.messages.size());
			for (const auto& persisted : loaded.messages) {
				ChatMessage message;
				message.text = persisted.text;
				message.thinking_text = persisted.thinking_text;
				message.is_user = persisted.is_user;
				message.has_thinking = persisted.has_thinking;
				message.timestamp = persisted.timestamp;
				message.input_tokens = persisted.input_tokens;
				message.output_tokens = persisted.output_tokens;
				message.cache_read_tokens = persisted.cache_read_tokens;
				message.cache_write_tokens = persisted.cache_write_tokens;
				message.model_id = persisted.model_id;
				messages.push_back(std::move(message));
			}
			g_chat_messages = std::move(messages);
			current_id = loaded.id;
			current_revision = loaded.revision;
			current_identity_uncommitted = false;
			aida::automation_ui::request_chat_composer_clear();
			aida::automation_ui::request_chat_scroll_to_bottom();
			aida::automation_ui::apply_persisted_evidence(loaded.id, loaded.evidence);
			chat_bind_session(loaded.id);
		}
	}
	if (completion->operation == aida::conversation_store::operation_t::delete_conversation &&
		completion->target_id == current_id) {
		g_chat_messages.clear();
		current_id.clear();
		current_revision = 0;
		current_identity_uncommitted = false;
		aida::automation_ui::request_chat_composer_clear();
		aida::automation_ui::request_chat_scroll_to_bottom();
		aida::automation_ui::apply_persisted_evidence({}, {});
		chat_bind_session({});
	}
	if (!catalog_published &&
		completion->operation == aida::conversation_store::operation_t::delete_conversation) {
		const auto previous_size = history.size();
		history.erase(std::remove_if(history.begin(), history.end(),
			[&](const ConversationSummary& summary) {
				return summary.id == completion->target_id;
			}), history.end());
		local_catalog_changed = local_catalog_changed || history.size() != previous_size;
	}
	if (!catalog_published && local_catalog_changed) {
		std::stable_sort(history.begin(), history.end(), [](const auto& left,
			const auto& right) {
			if (left.pinned != right.pinned) return left.pinned > right.pinned;
			if (left.created != right.created) return left.created > right.created;
			return left.id < right.id;
		});
		publish_conversation_catalog();
		++catalog_generation;
	}
	persistence_error = !publication_error.empty() ? std::move(publication_error) :
		completion->partial ? completion->error : std::string{};
	if (allow_deferred) submit_deferred_save();
	notify_catalog_changed();
}

bool conversations::commit_shutdown(std::string& error)
{
	if (g_chat_messages.empty() && current_id.empty()) {
		error.clear();
		return true;
	}
	aida::conversation_store::request_t request;
	request.operation = aida::conversation_store::operation_t::save;
	request.current = capture_conversation_snapshot(true);
	request.catalog_generation = catalog_generation;
	return aida::conversation_store::commit_lifecycle(std::move(request), error);
}

std::shared_ptr<const std::vector<ConversationSummary>> conversations::catalog_snapshot()
{
	return published_history;
}

void conversations::set_catalog_change_hook(std::function<void()> hook)
{
	catalog_change_hook_slot() = std::move(hook);
}
