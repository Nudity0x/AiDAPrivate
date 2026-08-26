#pragma once

#include <string>

#include "core/network/network_view.hpp"

namespace aida::burp::cookie_jar {

struct reviewed_context_view_t
{
    network_view::artifact_identity_t identity;
    std::string path;
    bool current = false;
    std::string reason;
};

bool reviewed_context_snapshot(reviewed_context_view_t& out);
bool revalidate_reviewed_context();
void clear_reviewed_context();
std::int64_t parse_cookie_expires(const std::string& text);

}
