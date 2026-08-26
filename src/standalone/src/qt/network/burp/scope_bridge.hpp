#pragma once

#include <string>

namespace aida::burp::scope {

struct staged_rule_draft_t
{
    bool        present = false;
    std::string protocol;
    std::string host;
    int         port = 0;
    std::string path;
    bool        exclude = false;
};

bool take_staged_rule_draft(staged_rule_draft_t& out);

}
