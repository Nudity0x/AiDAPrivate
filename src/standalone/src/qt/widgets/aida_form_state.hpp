#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace aida::qt::widgets {

struct validation_error_t {
    std::string field_id;
    std::string message;
};

class form_state_t {
public:
    void clear();
    void reject(std::string field_id, std::string message);
    bool valid() const;
    const char* error_for(const char* field_id) const;
    const std::vector<validation_error_t>& errors() const;
    void request_first_invalid_focus();
    bool consume_focus_request(const char* field_id);

private:
    std::vector<validation_error_t> errors_;
    std::string focus_field_;
};

inline void form_state_t::clear()
{
    errors_.clear();
    focus_field_.clear();
}

inline void form_state_t::reject(std::string field_id, std::string message)
{
    if (field_id.empty() || message.empty())
        return;
    const auto found = std::find_if(errors_.begin(), errors_.end(),
        [&](const validation_error_t& error) { return error.field_id == field_id; });
    if (found == errors_.end())
        errors_.push_back({ std::move(field_id), std::move(message) });
    else
        found->message = std::move(message);
}

inline bool form_state_t::valid() const
{
    return errors_.empty();
}

inline const char* form_state_t::error_for(const char* field_id) const
{
    if (!field_id)
        return nullptr;
    const auto found = std::find_if(errors_.begin(), errors_.end(),
        [&](const validation_error_t& error) { return error.field_id == field_id; });
    return found == errors_.end() ? nullptr : found->message.c_str();
}

inline const std::vector<validation_error_t>& form_state_t::errors() const
{
    return errors_;
}

inline void form_state_t::request_first_invalid_focus()
{
    focus_field_ = errors_.empty() ? std::string() : errors_.front().field_id;
}

inline bool form_state_t::consume_focus_request(const char* field_id)
{
    if (!field_id || focus_field_ != field_id)
        return false;
    focus_field_.clear();
    return true;
}

}
