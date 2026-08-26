#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#ifdef small
#undef small
#endif

#include "cookie_jar.hpp"
#include "burp_events.hpp"
#include "../network_view.hpp"
#include "qt/network/burp/cookie_jar_bridge.hpp"

#include "../../infra/event_bus.hpp"
#include "../../infra/executor.hpp"
#include "helpers/diag_log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace aida {
namespace burp {
namespace cookie_jar {

namespace {

struct host_jar_t
{
    std::vector<parsed_cookie_t> cookies;
};

struct state_t
{
    std::mutex                          mtx;
    std::map<std::string, host_jar_t>   jars;
    std::atomic<bool>                   initialized{false};
    aida::events::subscription_handle_t exchange_sub;
    std::mutex                          err_mtx;
    std::string                         last_err;

    network_view::artifact_identity_t   reviewed_context;
    std::string                         reviewed_path;
    bool                                reviewed_context_current = false;
    std::string                         reviewed_context_reason;
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

int64_t now_ms()
{
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void trim(std::string& v)
{
    size_t b = 0;
    while (b < v.size() && (v[b] == ' ' || v[b] == '\t')) ++b;
    size_t e = v.size();
    while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t' || v[e - 1] == '\r' || v[e - 1] == '\n')) --e;
    v = v.substr(b, e - b);
}

int parse_int_field(const std::string& v, int def = 0)
{
    int out = 0;
    bool any = false;
    bool neg = false;
    size_t i = 0;
    if (!v.empty() && (v[0] == '-' || v[0] == '+')) { neg = (v[0] == '-'); i = 1; }
    for (; i < v.size(); ++i) {
        if (v[i] < '0' || v[i] > '9') break;
        out = out * 10 + (v[i] - '0');
        any = true;
    }
    if (!any) return def;
    return neg ? -out : out;
}

int64_t parse_int64_field(const std::string& v, int64_t def = 0)
{
    int64_t out = 0;
    bool any = false;
    bool neg = false;
    size_t i = 0;
    if (!v.empty() && (v[0] == '-' || v[0] == '+')) { neg = (v[0] == '-'); i = 1; }
    for (; i < v.size(); ++i) {
        if (v[i] < '0' || v[i] > '9') break;
        out = out * 10 + (v[i] - '0');
        any = true;
    }
    if (!any) return def;
    return neg ? -out : out;
}

int month_from_name(const std::string& m)
{
    static const char* names[] = {"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
    std::string l = ascii_lower(m);
    for (int i = 0; i < 12; i++) if (l.compare(0, 3, names[i]) == 0) return i;
    return -1;
}

int64_t parse_http_date(const std::string& s_in)
{
    if (s_in.empty()) return 0;
    std::string normalized;
    normalized.reserve(s_in.size());
    for (char c : s_in) {
        if (c == ',' || c == '-' || c == '/' || c == ':' || c == 'T') normalized.push_back(' ');
        else                                                          normalized.push_back(c);
    }

    std::istringstream is(normalized);
    std::string tok;
    int day = 0, month = -1, year = 0, hour = 0, minute = 0, second = 0;
    while (is >> tok) {
        if (tok.size() == 3) {
            const int m = month_from_name(tok);
            if (m >= 0) { month = m; continue; }
        }
        if (tok.size() >= 4 && tok[0] >= '0' && tok[0] <= '9') {
            const int n = parse_int_field(tok);
            if (n >= 1900) { year = n; continue; }
        }
        if (tok.size() <= 2 && tok[0] >= '0' && tok[0] <= '9') {
            const int n = parse_int_field(tok);
            if (day == 0 && n >= 1 && n <= 31) { day = n; continue; }
        }
        if (tok.find_first_not_of("0123456789") == std::string::npos) {
            const int n = parse_int_field(tok);
            if (hour == 0 && n < 24) { hour = n; continue; }
            if (minute == 0 && n < 60) { minute = n; continue; }
            if (second == 0 && n < 60) { second = n; continue; }
        }
    }
    if (month < 0 || year == 0) return 0;

    std::tm tmv = {};
    tmv.tm_year = year - 1900;
    tmv.tm_mon  = month;
    tmv.tm_mday = day == 0 ? 1 : day;
    tmv.tm_hour = hour;
    tmv.tm_min  = minute;
    tmv.tm_sec  = second;

    const time_t local_t = _mkgmtime(&tmv);
    if (local_t == static_cast<time_t>(-1)) return 0;
    return static_cast<int64_t>(local_t) * 1000;
}

bool domain_matches(const std::string& cookie_domain, const std::string& request_host)
{
    if (cookie_domain.empty()) return false;
    std::string cd = ascii_lower(cookie_domain);
    if (!cd.empty() && cd[0] == '.') cd = cd.substr(1);
    const std::string rh = ascii_lower(request_host);
    if (cd == rh) return true;
    if (rh.size() > cd.size()) {
        const size_t off = rh.size() - cd.size();
        if (rh.compare(off, cd.size(), cd) == 0 && rh[off - 1] == '.') return true;
    }
    return false;
}

bool path_matches(const std::string& cookie_path, const std::string& request_path)
{
    if (cookie_path.empty() || cookie_path == "/") return true;
    if (request_path.size() < cookie_path.size()) return false;
    if (request_path.compare(0, cookie_path.size(), cookie_path) != 0) return false;
    if (request_path.size() == cookie_path.size()) return true;
    if (cookie_path.back() == '/') return true;
    if (request_path[cookie_path.size()] == '/') return true;
    return false;
}

void handle_exchange_observed(const exchange_observed_t& e)
{
    {
        ::aida::infra::executor::submission_t sub;
        sub.owner_subsystem = "burp.cookie_jar";
        sub.label = "cookie_jar.observe_exchange";
        sub.thread_class = "bounded_task";
        sub.domain = aida::infra::executor::domain_t::feature_worker;
        sub.priority = 3;
        sub.body = [e]() {
        ingest_set_cookie_headers(e.host, e.resp_headers);
    };
        (void)::aida::infra::executor::submit(std::move(sub));
    }
}

bool request_artifact_kind(network_view::artifact_kind_t kind)
{
    return kind == network_view::artifact_kind_t::exchange ||
        kind == network_view::artifact_kind_t::request ||
        kind == network_view::artifact_kind_t::repeater_request ||
        kind == network_view::artifact_kind_t::sitemap_request ||
        kind == network_view::artifact_kind_t::api_request ||
        kind == network_view::artifact_kind_t::scanner_request ||
        kind == network_view::artifact_kind_t::intercept_request;
}

}

bool stage_reviewed_context(const network_view::artifact_identity_t& identity,
                            const std::string& request_path,
                            std::string& unavailable_reason)
{
    if (!identity.valid() || !request_artifact_kind(identity.kind) ||
        identity.target_host.empty() || identity.target_port == 0 ||
        identity.target_host.size() > 255U || request_path.size() > 2048U ||
        identity.raw_protocol) {
        unavailable_reason = "Cookie Jar requires a current retained HTTP/1 target with bounded host and path metadata.";
        return false;
    }
    network_view::artifact_snapshot_t snapshot;
    if (!network_view::resolve_artifact(identity, snapshot, unavailable_reason)) return false;
    auto& st = s();
    st.reviewed_context = identity;
    st.reviewed_path = request_path.empty() ? "/" : request_path;
    st.reviewed_context_current = true;
    st.reviewed_context_reason.clear();
    unavailable_reason.clear();
    return true;
}

bool reviewed_context_snapshot(reviewed_context_view_t& out)
{
    auto& st = s();
    if (!st.reviewed_context.valid())
        return false;
    out.identity = st.reviewed_context;
    out.path = st.reviewed_path;
    out.current = st.reviewed_context_current;
    out.reason = st.reviewed_context_reason;
    return true;
}

bool revalidate_reviewed_context()
{
    auto& st = s();
    if (!st.reviewed_context.valid())
        return false;
    network_view::artifact_snapshot_t snapshot;
    std::string reason;
    st.reviewed_context_current = network_view::resolve_artifact(
        st.reviewed_context, snapshot, reason);
    st.reviewed_context_reason = st.reviewed_context_current
        ? std::string() : (reason.empty() ? "The retained source is stale." : std::move(reason));
    return st.reviewed_context_current;
}

void clear_reviewed_context()
{
    auto& st = s();
    st.reviewed_context = {};
    st.reviewed_path.clear();
    st.reviewed_context_current = false;
    st.reviewed_context_reason.clear();
}

std::int64_t parse_cookie_expires(const std::string& text)
{
    return parse_http_date(text);
}

std::string same_site_str(same_site_t s)
{
    switch (s) {
        case same_site_t::lax:    return "Lax";
        case same_site_t::strict: return "Strict";
        case same_site_t::none:   return "None";
        default:                  return "";
    }
}

same_site_t parse_same_site(const std::string& v)
{
    const std::string l = ascii_lower(v);
    if (l == "lax")    return same_site_t::lax;
    if (l == "strict") return same_site_t::strict;
    if (l == "none")   return same_site_t::none;
    return same_site_t::unset;
}

bool initialize()
{
    auto& st = s();
    bool expected = false;
    if (!st.initialized.compare_exchange_strong(expected, true)) return true;
    load_from_disk();
    st.exchange_sub = aida::events::subscribe(kExchangeObservedEvent,
        [](const exchange_observed_t& e) { handle_exchange_observed(e); });
    diag::log_tagged("burp", "cookie_jar_initialized");
    return true;
}

void shutdown()
{
    auto& st = s();
    if (!st.initialized.exchange(false)) return;
    if (st.exchange_sub.valid()) aida::events::unsubscribe(st.exchange_sub);
    save_to_disk();
}

bool parse_set_cookie(const std::string& set_cookie_value, const std::string& request_host, parsed_cookie_t& out)
{
    if (set_cookie_value.empty()) return false;
    out = parsed_cookie_t{};
    out.created_unix_ms = now_ms();

    std::vector<std::string> attrs;
    {
        size_t start = 0;
        for (size_t i = 0; i < set_cookie_value.size(); i++) {
            if (set_cookie_value[i] == ';') {
                attrs.push_back(set_cookie_value.substr(start, i - start));
                start = i + 1;
            }
        }
        if (start < set_cookie_value.size()) attrs.push_back(set_cookie_value.substr(start));
    }
    if (attrs.empty()) return false;

    std::string first = attrs[0];
    trim(first);
    const size_t eq = first.find('=');
    if (eq == std::string::npos) return false;
    out.name = first.substr(0, eq);
    trim(out.name);
    out.value = first.substr(eq + 1);
    trim(out.value);
    if (out.name.empty()) return false;

    int64_t max_age_seconds = -1;

    for (size_t i = 1; i < attrs.size(); i++) {
        std::string a = attrs[i];
        trim(a);
        if (a.empty()) continue;
        const size_t aeq = a.find('=');
        std::string key, val;
        if (aeq == std::string::npos) { key = a; }
        else { key = a.substr(0, aeq); val = a.substr(aeq + 1); }
        trim(key);
        trim(val);
        const std::string lk = ascii_lower(key);
        if (lk == "domain") {
            out.domain = ascii_lower(val);
            if (!out.domain.empty() && out.domain[0] == '.') out.domain = out.domain.substr(1);
        } else if (lk == "path") {
            out.path = val;
        } else if (lk == "expires") {
            out.has_expires = true;
            out.expires_unix_ms = parse_http_date(val);
        } else if (lk == "max-age") {
            max_age_seconds = parse_int64_field(val, -1);
        } else if (lk == "secure") {
            out.secure = true;
        } else if (lk == "httponly") {
            out.http_only = true;
        } else if (lk == "samesite") {
            out.same_site = parse_same_site(val);
        }
    }

    if (max_age_seconds >= 0) {
        out.has_expires = true;
        out.expires_unix_ms = out.created_unix_ms + max_age_seconds * 1000;
    }

    if (out.domain.empty()) {
        out.domain = ascii_lower(request_host);
        out.host_only = true;
    }
    if (out.path.empty()) out.path = "/";
    return true;
}

void set_cookie(const std::string& host, const parsed_cookie_t& c)
{
    auto& st = s();
    const std::string key = ascii_lower(c.domain.empty() ? host : c.domain);
    if (key.empty() || c.name.empty()) return;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        auto& jar = st.jars[key];
        for (auto it = jar.cookies.begin(); it != jar.cookies.end(); ++it) {
            if (it->name == c.name && it->path == c.path && ascii_lower(it->domain) == ascii_lower(c.domain)) {
                *it = c;
                save_to_disk();
                aida::events::publish(kCookieChangedEvent, cookie_changed_t{key, c.name, "update"});
                return;
            }
        }
        jar.cookies.push_back(c);
    }
    save_to_disk();
    aida::events::publish(kCookieChangedEvent, cookie_changed_t{key, c.name, "set"});
}

void ingest_set_cookie_headers(const std::string& request_host,
                               const std::vector<std::pair<std::string, std::string>>& resp_headers)
{
    for (const auto& h : resp_headers) {
        if (ascii_lower(h.first) != "set-cookie") continue;
        parsed_cookie_t pc;
        if (parse_set_cookie(h.second, request_host, pc)) set_cookie(request_host, pc);
    }
}

std::vector<parsed_cookie_t> cookies_for(const std::string& host, const std::string& path, bool tls)
{
    auto& st = s();
    std::vector<parsed_cookie_t> out;
    const int64_t now = now_ms();
    std::lock_guard<std::mutex> lk(st.mtx);
    for (const auto& kv : st.jars) {
        for (const auto& c : kv.second.cookies) {
            if (c.has_expires && c.expires_unix_ms > 0 && c.expires_unix_ms <= now) continue;
            if (c.secure && !tls) continue;
            if (!domain_matches(c.domain.empty() ? kv.first : c.domain, host)) continue;
            if (!path_matches(c.path, path)) continue;
            if (c.host_only && ascii_lower(c.domain) != ascii_lower(host)) {
                if (ascii_lower(kv.first) != ascii_lower(host)) continue;
            }
            out.push_back(c);
        }
    }
    std::sort(out.begin(), out.end(), [](const parsed_cookie_t& a, const parsed_cookie_t& b) {
        if (a.path.size() != b.path.size()) return a.path.size() > b.path.size();
        return a.created_unix_ms < b.created_unix_ms;
    });
    return out;
}

std::string build_cookie_header(const std::string& host, const std::string& path, bool tls)
{
    const auto cs = cookies_for(host, path, tls);
    std::string out;
    bool first = true;
    for (const auto& c : cs) {
        if (!first) out.append("; ");
        out.append(c.name);
        out.push_back('=');
        out.append(c.value);
        first = false;
    }
    return out;
}

std::vector<parsed_cookie_t> list_for_host(const std::string& host)
{
    auto& st = s();
    std::vector<parsed_cookie_t> out;
    std::lock_guard<std::mutex> lk(st.mtx);
    const auto it = st.jars.find(ascii_lower(host));
    if (it != st.jars.end()) out = it->second.cookies;
    return out;
}

std::vector<parsed_cookie_t> list_all()
{
    auto& st = s();
    std::vector<parsed_cookie_t> out;
    std::lock_guard<std::mutex> lk(st.mtx);
    for (const auto& kv : st.jars) {
        for (const auto& c : kv.second.cookies) out.push_back(c);
    }
    return out;
}

bool delete_cookie(const std::string& host, const std::string& name, const std::string& path)
{
    auto& st = s();
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        const auto it = st.jars.find(ascii_lower(host));
        if (it != st.jars.end()) {
            auto& v = it->second.cookies;
            for (auto cit = v.begin(); cit != v.end(); ) {
                const bool path_ok = path.empty() ? true : (cit->path == path);
                if (cit->name == name && path_ok) {
                    cit = v.erase(cit);
                    removed = true;
                } else {
                    ++cit;
                }
            }
            if (v.empty()) st.jars.erase(it);
        }
    }
    if (removed) {
        save_to_disk();
        aida::events::publish(kCookieChangedEvent, cookie_changed_t{ascii_lower(host), name, "delete"});
    }
    return removed;
}

void clear_for_host(const std::string& host)
{
    auto& st = s();
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.jars.erase(ascii_lower(host));
    }
    save_to_disk();
    aida::events::publish(kCookieChangedEvent, cookie_changed_t{ascii_lower(host), "", "clear_host"});
}

void clear_all()
{
    auto& st = s();
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.jars.clear();
    }
    save_to_disk();
    aida::events::publish(kCookieChangedEvent, cookie_changed_t{"", "", "clear_all"});
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
    base += "\\cookies.json";
    return base;
}

bool save_to_disk()
{
    auto& st = s();
    nlohmann::json root = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        for (const auto& kv : st.jars) {
            for (const auto& c : kv.second.cookies) {
                nlohmann::json j;
                j["host_key"]         = kv.first;
                j["name"]             = c.name;
                j["value"]            = c.value;
                j["domain"]           = c.domain;
                j["path"]             = c.path;
                j["expires_unix_ms"]  = c.expires_unix_ms;
                j["has_expires"]      = c.has_expires;
                j["secure"]           = c.secure;
                j["http_only"]        = c.http_only;
                j["host_only"]        = c.host_only;
                j["same_site"]        = same_site_str(c.same_site);
                j["created_unix_ms"]  = c.created_unix_ms;
                root.push_back(j);
            }
        }
    }
    const std::string path = storage_path();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        set_err("failed to open cookies.json for write");
        return false;
    }
    const std::string dump = root.dump(2);
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
    try { arr = nlohmann::json::parse(data, nullptr, false); }
    catch (...) { set_err("cookies.json parse failed"); return false; }
    if (arr.is_discarded() || !arr.is_array()) {
        set_err("cookies.json not an array");
        return false;
    }
    std::map<std::string, host_jar_t> loaded;
    for (const auto& j : arr) {
        if (!j.is_object()) continue;
        const std::string key = j.value("host_key", std::string());
        if (key.empty()) continue;
        parsed_cookie_t c;
        c.name             = j.value("name", std::string());
        c.value            = j.value("value", std::string());
        c.domain           = j.value("domain", std::string());
        c.path             = j.value("path", std::string("/"));
        c.expires_unix_ms  = j.value("expires_unix_ms", static_cast<int64_t>(0));
        c.has_expires      = j.value("has_expires", false);
        c.secure           = j.value("secure", false);
        c.http_only        = j.value("http_only", false);
        c.host_only        = j.value("host_only", false);
        c.same_site        = parse_same_site(j.value("same_site", std::string()));
        c.created_unix_ms  = j.value("created_unix_ms", static_cast<int64_t>(0));
        if (c.name.empty()) continue;
        loaded[ascii_lower(key)].cookies.push_back(c);
    }
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.jars = std::move(loaded);
    }
    return true;
}

bool export_netscape(const std::string& file_path)
{
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    if (!out) { set_err("export: failed to open file"); return false; }
    out << "# Netscape HTTP Cookie File\n";
    out << "# https://curl.se/docs/http-cookies.html\n";
    out << "# This file was generated by AiDA Burp Cookie Jar\n\n";
    const auto all = list_all();
    for (const auto& c : all) {
        const std::string domain = c.domain.empty() ? "" : (c.host_only ? c.domain : std::string(".") + c.domain);
        const std::string include_sub = c.host_only ? "FALSE" : "TRUE";
        const std::string secure = c.secure ? "TRUE" : "FALSE";
        const int64_t expires = c.has_expires ? (c.expires_unix_ms / 1000) : 0;
        out << domain << '\t' << include_sub << '\t' << c.path << '\t' << secure << '\t'
            << expires << '\t' << c.name << '\t' << c.value << '\n';
    }
    return true;
}

bool import_netscape(const std::string& file_path)
{
    std::ifstream in(file_path, std::ios::binary);
    if (!in) { set_err("import: failed to open file"); return false; }
    std::string line;
    size_t added = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cols;
        {
            size_t start = 0;
            for (size_t i = 0; i < line.size(); i++) {
                if (line[i] == '\t') { cols.push_back(line.substr(start, i - start)); start = i + 1; }
            }
            cols.push_back(line.substr(start));
        }
        if (cols.size() < 7) continue;
        parsed_cookie_t c;
        c.created_unix_ms = now_ms();
        c.domain = cols[0];
        if (!c.domain.empty() && c.domain[0] == '.') { c.domain = c.domain.substr(1); c.host_only = false; }
        else                                           { c.host_only = true; }
        c.path   = cols[2];
        c.secure = (ascii_lower(cols[3]) == "true");
        const int64_t expires_sec = parse_int64_field(cols[4], 0);
        if (expires_sec > 0) { c.has_expires = true; c.expires_unix_ms = expires_sec * 1000; }
        c.name   = cols[5];
        c.value  = cols[6];
        if (c.name.empty()) continue;
        set_cookie(c.domain, c);
        ++added;
    }
    diag::log_tagged_fmt("burp", "cookie_import_netscape file=%s added=%zu", file_path.c_str(), added);
    return true;
}

std::string last_error()
{
    auto& st = s();
    std::lock_guard<std::mutex> lk(st.err_mtx);
    return st.last_err;
}

}
}
}
