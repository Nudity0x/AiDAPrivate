#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <functional>


struct ConversationSummary {
	std::string id;
	std::string title;
	int64_t     created = 0;
	int         msg_count = 0;
	bool        pinned = false;
	std::uint64_t revision = 0;
};

namespace conversations {
	extern std::vector<ConversationSummary> history;
	extern std::shared_ptr<const std::vector<ConversationSummary>> published_history;
	extern std::string current_id;
	extern bool browser_open;
	extern std::uint64_t current_revision;
	extern bool current_identity_uncommitted;
	extern std::uint64_t catalog_generation;
	extern std::string persistence_error;

	void save_current();
	void load_conversation(const std::string& id);
	void new_chat();
	void refresh_history();
	void delete_conversation(const std::string& id, std::uint64_t reviewed_revision);
	bool set_pinned(const std::string& id, bool pinned);
	bool fork_conversation(const std::string& id, std::string& forked_id);
	bool export_markdown(const std::string& id, const std::string& output_path,
		std::string& error);
	void process_store_completion(bool allow_deferred = true);
	bool commit_shutdown(std::string& error);
	std::shared_ptr<const std::vector<ConversationSummary>> catalog_snapshot();

	void set_catalog_change_hook(std::function<void()> hook);
}
