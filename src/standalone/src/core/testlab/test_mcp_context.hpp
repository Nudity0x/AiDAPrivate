#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace test_all_features {

struct mcp_phase_context_t {
    std::atomic<int>* passed = nullptr;
    std::atomic<int>* failed = nullptr;
    std::atomic<int>* skipped = nullptr;
    std::function<bool()> cancelled;

    bool validate(std::string& reason) const {
        if (passed == nullptr) reason = "passed counter missing";
        else if (failed == nullptr) reason = "failed counter missing";
        else if (skipped == nullptr) reason = "skipped counter missing";
        else if (!cancelled) reason = "cancellation callback missing";
        else return true;
        return false;
    }
};

}
