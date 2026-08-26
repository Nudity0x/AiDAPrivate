#pragma once

#include <cstdint>
#include <string>

namespace aida::qt::scanner {

enum class scan_command_t : std::uint8_t {
	first_scan,
	next_scan,
	stop_scan,
	undo_scan,
	new_scan
};

struct scan_command_state_t {
	bool enabled = false;
	std::string disabled_reason;
};

struct scan_command_result_t {
	bool succeeded = false;
	std::string detail;
};

scan_command_state_t scan_command_capability(scan_command_t command);
scan_command_result_t execute_scan_command(scan_command_t command);

}
