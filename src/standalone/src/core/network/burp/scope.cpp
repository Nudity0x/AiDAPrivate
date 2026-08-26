#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#ifdef small
#undef small
#endif

#include "scope.hpp"
#include "burp_events.hpp"
#include "qt/network/burp/scope_bridge.hpp"

#include "helpers/diag_log.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace aida {
namespace burp {
namespace scope {

namespace {

struct state_t
{
    std::mutex                  mtx;
    std::vector<rule_t>         rules;
    std::atomic<uint64_t>       next_id{1};
    std::atomic<bool>           initialized{false};
    std::mutex                  err_mtx;
    std::string                 last_err;

    std::unordered_map<uint64_t, std::shared_ptr<const std::regex>> compiled_host;
    staged_rule_draft_t           staged;
};

state_t& s()
{
    static state_t st;
    return st;
}

void set_err(const std::string& msg)
{
    auto& st = s();
    std::lock_guard<std::mutex> lk(st.err_mtx);
    st.last_err = msg;
}

std::string ascii_lower(const std::string& v)
{
    std::string r;
    r.reserve(v.size());
    for (char c : v) {
        if (c >= 'A' && c <= 'Z') r.push_back(static_cast<char>(c + 32));
        else                      r.push_back(c);
    }
    return r;
}

bool host_pattern_needs_regex(const std::string& pattern)
{
    return pattern.find_first_of("*?[\\^$+(){}|") != std::string::npos;
}

std::shared_ptr<const std::regex> compile_host_pattern(const std::string& pattern)
{
    try {
        return std::make_shared<const std::regex>(pattern,
            std::regex::ECMAScript | std::regex::icase);
    } catch (...) {
        return nullptr;
    }
}

bool match_host_pattern(const rule_t& r, const std::string& host,
                        const std::unordered_map<uint64_t, std::shared_ptr<const std::regex>>& compiled)
{
    const std::string& pattern = r.host_pattern;
    const std::string pat = ascii_lower(pattern);
    const std::string h = ascii_lower(host);

    if (pat.empty() || pat == "*") return true;
    if (pat == h) return true;

    if (pat.rfind("*.", 0) == 0) {
        const std::string suffix = pat.substr(1);
        return h.size() > suffix.size() &&
               h.compare(h.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    if (host_pattern_needs_regex(pattern)) {
        const auto found = compiled.find(r.id);
        if (found != compiled.end()) {
            if (!found->second)
                return false;
            try {
                return std::regex_match(host, *found->second);
            } catch (...) {
                return false;
            }
        }
        const auto re = compile_host_pattern(pattern);
        if (!re)
            return false;
        try {
            return std::regex_match(host, *re);
        } catch (...) {
            return false;
        }
    }

    if (pat.size() >= 2 && pat[0] == '.' ) {
        return h.size() >= pat.size() - 1 &&
               h.compare(h.size() - (pat.size() - 1), pat.size() - 1, pat.substr(1)) == 0;
    }

    if (h.size() >= pat.size()) {
        const size_t off = h.size() - pat.size();
        if (h.compare(off, pat.size(), pat) == 0) {
            if (off == 0) return true;
            if (h[off - 1] == '.') return true;
        }
    }
    return false;
}

bool rule_matches(const rule_t& r, const std::string& scheme, const std::string& host, uint16_t port, const std::string& path,
                  const std::unordered_map<uint64_t, std::shared_ptr<const std::regex>>& compiled)
{
    if (!r.enabled) return false;
    if (!r.protocol.empty() && ascii_lower(r.protocol) != ascii_lower(scheme)) {
        if (!(r.protocol == "*" || r.protocol == "any")) return false;
    }
    if (r.port != 0 && static_cast<uint16_t>(r.port) != port) return false;
    if (!r.host_pattern.empty() && !match_host_pattern(r, host, compiled)) return false;
    if (!r.path_prefix.empty()) {
        if (path.size() < r.path_prefix.size()) return false;
        if (path.compare(0, r.path_prefix.size(), r.path_prefix) != 0) return false;
    }
    return true;
}

}

parsed_url_t parse_url(const std::string& url)
{
    parsed_url_t out;
    if (url.empty()) return out;
    const size_t scheme_end = url.find("://");
    size_t cursor = 0;
    if (scheme_end != std::string::npos) {
        out.scheme = ascii_lower(url.substr(0, scheme_end));
        cursor = scheme_end + 3;
    } else {
        out.scheme = "http";
    }
    const size_t path_start = url.find('/', cursor);
    std::string authority = (path_start == std::string::npos) ? url.substr(cursor) : url.substr(cursor, path_start - cursor);

    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        out.host = ascii_lower(authority.substr(0, colon));
        const std::string port_str = authority.substr(colon + 1);
        long port_val = 0;
        for (char c : port_str) {
            if (c < '0' || c > '9') { port_val = -1; break; }
            port_val = port_val * 10 + (c - '0');
            if (port_val > 65535) { port_val = -1; break; }
        }
        if (port_val < 0) { out.host = ascii_lower(authority); out.port = 0; }
        else              out.port = static_cast<uint16_t>(port_val);
    } else {
        out.host = ascii_lower(authority);
        out.port = 0;
    }

    if (out.port == 0) {
        if (out.scheme == "https" || out.scheme == "wss") out.port = 443;
        else if (out.scheme == "http" || out.scheme == "ws") out.port = 80;
    }

    if (path_start == std::string::npos) out.path = "/";
    else                                  out.path = url.substr(path_start);

    out.valid = !out.host.empty();
    return out;
}

bool initialize()
{
    auto& st = s();
    bool expected = false;
    if (!st.initialized.compare_exchange_strong(expected, true)) return true;
    load_from_disk();
    diag::log_tagged("burp", "scope_initialized");
    return true;
}

void shutdown()
{
    auto& st = s();
    if (!st.initialized.exchange(false)) return;
    save_to_disk();
}

bool in_scope_components(const std::string& scheme, const std::string& host, uint16_t port, const std::string& path)
{
    auto& st = s();
    std::lock_guard<std::mutex> lk(st.mtx);

    if (st.rules.empty()) return true;

    bool has_includes = false;
    bool include_matched = false;
    for (const auto& r : st.rules) {
        if (r.kind == rule_kind_t::include && r.enabled) {
            has_includes = true;
            if (rule_matches(r, scheme, host, port, path, st.compiled_host)) {
                include_matched = true;
                break;
            }
        }
    }

    if (has_includes && !include_matched) return false;

    for (const auto& r : st.rules) {
        if (r.kind == rule_kind_t::exclude && rule_matches(r, scheme, host, port, path, st.compiled_host)) return false;
    }
    return true;
}

bool in_scope(const std::string& url)
{
    parsed_url_t p = parse_url(url);
    if (!p.valid) return false;
    return in_scope_components(p.scheme, p.host, p.port, p.path);
}

uint64_t add_rule(const rule_t& src)
{
    auto& st = s();
    rule_t r = src;
    if (r.id == 0) r.id = st.next_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        if (host_pattern_needs_regex(r.host_pattern))
            st.compiled_host[r.id] = compile_host_pattern(r.host_pattern);
        st.rules.push_back(r);
    }
    save_to_disk();
    aida::events::publish(kScopeChangedEvent, scope_changed_t{r.id, "add", r.kind == rule_kind_t::exclude});
    return r.id;
}

uint64_t add_include_rule(const std::string& protocol, const std::string& host_pattern, int port, const std::string& path_prefix)
{
    rule_t r;
    r.kind = rule_kind_t::include;
    r.protocol = protocol;
    r.host_pattern = host_pattern;
    r.port = port;
    r.path_prefix = path_prefix;
    r.enabled = true;
    return add_rule(r);
}

uint64_t add_exclude_rule(const std::string& protocol, const std::string& host_pattern, int port, const std::string& path_prefix)
{
    rule_t r;
    r.kind = rule_kind_t::exclude;
    r.protocol = protocol;
    r.host_pattern = host_pattern;
    r.port = port;
    r.path_prefix = path_prefix;
    r.enabled = true;
    return add_rule(r);
}

bool remove_rule(uint64_t rule_id)
{
    auto& st = s();
    bool removed = false;
    bool was_exclude = false;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        for (auto it = st.rules.begin(); it != st.rules.end(); ++it) {
            if (it->id == rule_id) {
                was_exclude = (it->kind == rule_kind_t::exclude);
                st.compiled_host.erase(it->id);
                st.rules.erase(it);
                removed = true;
                break;
            }
        }
    }
    if (removed) {
        save_to_disk();
        aida::events::publish(kScopeChangedEvent, scope_changed_t{rule_id, "remove", was_exclude});
    } else {
        set_err("rule_id not found");
    }
    return removed;
}

bool set_rule_enabled(uint64_t rule_id, bool enabled)
{
    auto& st = s();
    bool changed = false;
    bool was_exclude = false;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        for (auto& r : st.rules) {
            if (r.id == rule_id) {
                r.enabled = enabled;
                was_exclude = (r.kind == rule_kind_t::exclude);
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        save_to_disk();
        aida::events::publish(kScopeChangedEvent, scope_changed_t{rule_id, "enable", was_exclude});
    }
    return changed;
}

void clear_all()
{
    auto& st = s();
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.rules.clear();
        st.compiled_host.clear();
    }
    save_to_disk();
    aida::events::publish(kScopeChangedEvent, scope_changed_t{0, "clear", false});
}

std::vector<rule_t> list_rules()
{
    auto& st = s();
    std::lock_guard<std::mutex> lk(st.mtx);
    return st.rules;
}

std::string last_error()
{
    auto& st = s();
    std::lock_guard<std::mutex> lk(st.err_mtx);
    return st.last_err;
}

bool stage_rule(const std::string& url, rule_kind_t kind, std::string& reason)
{
    const auto parsed = parse_url(url);
    if (!parsed.valid || parsed.host.empty()) {
        reason = "The selected artifact has no valid URL to stage as a scope rule.";
        return false;
    }
    if (parsed.scheme.size() > 15 || parsed.host.size() > 255 || parsed.path.size() > 511) {
        reason = "The selected artifact URL exceeds the bounded scope rule fields.";
        return false;
    }
    {
        auto& st = s();
        std::lock_guard<std::mutex> lk(st.mtx);
        st.staged.present = true;
        st.staged.protocol = parsed.scheme;
        st.staged.host = parsed.host;
        st.staged.port = static_cast<int>(parsed.port);
        st.staged.path = parsed.path;
        st.staged.exclude = kind == rule_kind_t::exclude;
    }
    aida::events::publish(kScopeChangedEvent, scope_changed_t{0, "stage", kind == rule_kind_t::exclude});
    reason.clear();
    return true;
}

bool take_staged_rule_draft(staged_rule_draft_t& out)
{
    auto& st = s();
    std::lock_guard<std::mutex> lk(st.mtx);
    if (!st.staged.present)
        return false;
    out = st.staged;
    st.staged = {};
    return true;
}

std::string storage_path()
{
    PWSTR appdata = nullptr;
    std::string base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, appdata, -1, nullptr, 0, nullptr, nullptr);
        if (needed > 1) {
            base.assign(static_cast<size_t>(needed - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, appdata, -1, base.data(), needed, nullptr, nullptr);
        }
        CoTaskMemFree(appdata);
    }
    if (base.empty()) {
        char buf[MAX_PATH] = {};
        const DWORD len = GetEnvironmentVariableA("USERPROFILE", buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) base.assign(buf, len);
        else                            base = "C:\\Users\\Public";
        base += "\\AppData\\Roaming";
    }
    base += "\\AiDA\\Standalone\\burp";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    base += "\\scope.json";
    return base;
}

nlohmann::json rule_to_json(const rule_t& r)
{
    nlohmann::json j;
    j["id"]            = r.id;
    j["kind"]          = (r.kind == rule_kind_t::exclude) ? "exclude" : "include";
    j["protocol"]      = r.protocol;
    j["host_pattern"]  = r.host_pattern;
    j["port"]          = r.port;
    j["path_prefix"]   = r.path_prefix;
    j["enabled"]       = r.enabled;
    return j;
}

bool rule_from_json(const nlohmann::json& j, rule_t& out)
{
    if (!j.is_object()) return false;
    out = rule_t{};
    if (j.contains("id") && j["id"].is_number_unsigned()) out.id = j["id"].get<uint64_t>();
    const std::string k = j.value("kind", std::string("include"));
    out.kind = (k == "exclude") ? rule_kind_t::exclude : rule_kind_t::include;
    out.protocol     = j.value("protocol", std::string());
    out.host_pattern = j.value("host_pattern", std::string());
    out.port         = j.value("port", 0);
    out.path_prefix  = j.value("path_prefix", std::string());
    out.enabled      = j.value("enabled", true);
    return true;
}

bool save_to_disk()
{
    auto& st = s();
    nlohmann::json arr = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        for (const auto& r : st.rules) arr.push_back(rule_to_json(r));
    }
    const std::string path = storage_path();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        set_err("failed to open scope.json for write");
        return false;
    }
    const std::string dump = arr.dump(2);
    out.write(dump.data(), static_cast<std::streamsize>(dump.size()));
    return true;
}

bool load_from_disk()
{
    auto& st = s();
    const std::string path = storage_path();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string data = ss.str();
    if (data.empty()) return false;

    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(data, nullptr, false);
    } catch (...) {
        set_err("scope.json parse failed");
        return false;
    }
    if (arr.is_discarded() || !arr.is_array()) {
        set_err("scope.json not an array");
        return false;
    }

    uint64_t max_id = 0;
    std::vector<rule_t> loaded;
    loaded.reserve(arr.size());
    for (const auto& j : arr) {
        rule_t r;
        if (!rule_from_json(j, r)) continue;
        if (r.id > max_id) max_id = r.id;
        loaded.push_back(r);
    }
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.compiled_host.clear();
        for (const auto& r : loaded) {
            if (host_pattern_needs_regex(r.host_pattern))
                st.compiled_host[r.id] = compile_host_pattern(r.host_pattern);
        }
        st.rules = std::move(loaded);
    }
    st.next_id.store(max_id + 1);
    return true;
}


}
}
}
