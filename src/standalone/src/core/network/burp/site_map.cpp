#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef small
#undef small
#endif

#include "site_map.hpp"
#include "burp_logger.hpp"
#include "scope.hpp"
#include "qt/network/burp/site_map_bridge.hpp"

#include "../../infra/event_bus.hpp"
#include "../../infra/executor.hpp"
#include "helpers/diag_log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace aida {
namespace burp {
namespace sitemap {

namespace {

struct host_key_t
{
    std::string host;
    uint16_t    port = 0;
    bool        tls = false;
    bool operator<(const host_key_t& o) const noexcept
    {
        if (host != o.host) return host < o.host;
        if (port != o.port) return port < o.port;
        return tls < o.tls;
    }
};

constexpr size_t kMaxCachedTreeRows = 65536;
constexpr size_t kMaxCachedTreeTextBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxCachedExchangeRows = 65536;

struct state_t
{
    std::mutex                                       mtx;
    std::map<host_key_t, std::shared_ptr<host_node_t>> hosts;
    std::map<uint64_t, exchange_observed_t>          by_id;
    std::atomic<uint64_t>                            next_id{1};
    std::atomic<uint64_t>                            selected_exchange_id{0};
    std::atomic<size_t>                              exchange_count{0};
    std::atomic<bool>                                initialized{false};
    aida::events::subscription_handle_t              exchange_sub;
    std::mutex                                       err_mtx;
    std::string                                      last_err;

    std::map<host_key_t, std::deque<exchange_row_t>> exchange_index;
    std::mutex                                       cache_mtx;
    std::shared_ptr<const site_map_tree_snapshot_t>  tree_snapshot =
        std::make_shared<const site_map_tree_snapshot_t>();
    std::atomic<uint64_t>                            tree_snapshot_revision{0};
    bool                                             tree_cache_limited = false;
    std::string                                      tree_cache_filter;
    uint64_t                                         tree_query_revision = 1;
    std::atomic<bool>                                tree_rebuild_dirty{true};
    std::atomic<bool>                                tree_rebuild_inflight{false};
    std::atomic<uint64_t>                            topology_revision{1};
    std::atomic<bool>                                shutting_down{false};
    std::atomic<uint64_t>                            tree_retry_after_ms{0};
    std::atomic<uint32_t>                            tree_retry_attempt{0};
};

state_t& s()
{
    static state_t st;
    return st;
}

uint64_t now_ms()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void split_path_segments(const std::string& path, std::vector<std::string>& out)
{
    out.clear();
    size_t i = 0;
    if (path.empty() || path[0] != '/') { out.emplace_back("/"); return; }
    while (i < path.size()) {
        if (path[i] == '/') { ++i; continue; }
        size_t end = path.find('/', i);
        if (end == std::string::npos) { out.emplace_back(path.substr(i)); break; }
        out.emplace_back(path.substr(i, end - i));
        i = end + 1;
    }
}

std::string bounded_display(std::string value, size_t limit)
{
    if (value.size() <= limit) return value;
    value.resize(limit - 3);
    value.append("...");
    return value;
}

uint64_t monotonic_ms()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

exchange_row_t make_exchange_row(const exchange_observed_t& e)
{
    exchange_row_t row;
    row.id = e.id;
    row.timestamp_ms = e.timestamp_ms;
    row.method = bounded_display(e.method.empty() ? "GET" : e.method, 32);
    row.path = e.path;
    if (!e.query.empty()) {
        row.path.push_back('?');
        row.path.append(e.query);
    }
    row.path = bounded_display(std::move(row.path), 1024);
    row.status_code = e.status_code;
    row.response_size = e.resp_body.size();
    row.latency_ms = e.latency_ms;
    return row;
}

void insert_exchange_index(state_t& st, const host_key_t& key, const exchange_observed_t& e)
{
    auto& rows = st.exchange_index[key];
    exchange_row_t row = make_exchange_row(e);
    const auto pos = std::upper_bound(rows.begin(), rows.end(), row,
        [](const exchange_row_t& lhs, const exchange_row_t& rhs) {
            if (lhs.timestamp_ms != rhs.timestamp_ms)
                return lhs.timestamp_ms < rhs.timestamp_ms;
            return lhs.id < rhs.id;
        });
    rows.insert(pos, std::move(row));
}

void remove_exchange_index(state_t& st, const exchange_observed_t& e)
{
    const host_key_t key{e.host, e.port, e.scheme == "https" || e.scheme == "wss"};
    const auto index_it = st.exchange_index.find(key);
    if (index_it == st.exchange_index.end()) return;
    auto& rows = index_it->second;
    if (!rows.empty() && rows.front().id == e.id) {
        rows.pop_front();
    } else if (!rows.empty() && rows.back().id == e.id) {
        rows.pop_back();
    } else {
        const auto row_it = std::find_if(rows.begin(), rows.end(),
            [&e](const exchange_row_t& row) { return row.id == e.id; });
        if (row_it != rows.end()) rows.erase(row_it);
    }
    if (rows.empty()) st.exchange_index.erase(index_it);
}

void remove_exchange_from_leaf(state_t& st, const exchange_observed_t& e)
{
    const host_key_t key{e.host, e.port, e.scheme == "https" || e.scheme == "wss"};
    const auto host = st.hosts.find(key);
    if (host == st.hosts.end() || !host->second || !host->second->root) return;
    std::vector<std::string> segments;
    split_path_segments(e.path, segments);
    auto node = host->second->root;
    for (const auto& segment : segments) {
        const auto child = node->children.find(segment);
        if (child == node->children.end()) return;
        node = child->second;
    }
    const auto exchange = std::find_if(node->exchanges.begin(), node->exchanges.end(),
        [&e](const exchange_observed_t& candidate) { return candidate.id == e.id; });
    if (exchange != node->exchanges.end()) node->exchanges.erase(exchange);
}

bool insert_into_tree(state_t& st, const exchange_observed_t& e)
{
    host_key_t key{e.host, e.port, e.scheme == "https" || e.scheme == "wss"};
    bool topology_changed = false;
    auto it = st.hosts.find(key);
    if (it == st.hosts.end()) {
        auto h = std::make_shared<host_node_t>();
        h->host = e.host;
        h->port = e.port;
        h->tls  = key.tls;
        h->root = std::make_shared<path_node_t>();
        h->root->segment = "/";
        h->in_scope = scope::in_scope_components(e.scheme, e.host, e.port, e.path);
        h->total_requests = 0;
        h->issue_count = 0;
        it = st.hosts.emplace(key, h).first;
        topology_changed = true;
    }
    auto host = it->second;
    host->last_seen_ms = e.timestamp_ms != 0 ? e.timestamp_ms : now_ms();
    host->total_requests++;
    if (e.status_code >= 500) host->issue_count++;

    std::vector<std::string> segs;
    split_path_segments(e.path, segs);

    auto cur = host->root;
    cur->total_requests++;
    cur->last_seen_ms = host->last_seen_ms;
    cur->in_scope = scope::in_scope_components(e.scheme, e.host, e.port, e.path);

    for (const auto& seg : segs) {
        auto sit = cur->children.find(seg);
        if (sit == cur->children.end()) {
            auto n = std::make_shared<path_node_t>();
            n->segment = seg;
            sit = cur->children.emplace(seg, n).first;
            topology_changed = true;
        }
        cur = sit->second;
        cur->total_requests++;
        cur->last_seen_ms = host->last_seen_ms;
        cur->in_scope = scope::in_scope_components(e.scheme, e.host, e.port, e.path);
        cur->last_status = e.status_code;
    }

    const auto exchange_pos = std::upper_bound(cur->exchanges.begin(), cur->exchanges.end(), e,
        [](const exchange_observed_t& lhs, const exchange_observed_t& rhs) {
            if (lhs.timestamp_ms != rhs.timestamp_ms)
                return lhs.timestamp_ms < rhs.timestamp_ms;
            return lhs.id < rhs.id;
        });
    cur->exchanges.insert(exchange_pos, e);
    if (cur->exchanges.size() > 512) {
        const auto erase_count = static_cast<decltype(cur->exchanges)::difference_type>(cur->exchanges.size() - 512);
        cur->exchanges.erase(cur->exchanges.begin(), cur->exchanges.begin() + erase_count);
    }
    const auto existing = st.by_id.find(e.id);
    if (existing != st.by_id.end()) {
        remove_exchange_index(st, existing->second);
        remove_exchange_from_leaf(st, existing->second);
    }
    st.by_id[e.id] = e;
    insert_exchange_index(st, key, e);
    if (st.by_id.size() > kMaxCachedExchangeRows) {
        const auto oldest = st.by_id.begin();
        remove_exchange_index(st, oldest->second);
        remove_exchange_from_leaf(st, oldest->second);
        st.by_id.erase(oldest);
    }
    st.exchange_count.store(st.by_id.size());
    return topology_changed;
}

std::string normalized_source(const exchange_observed_t& e);
std::string source_label_for(const exchange_observed_t& e);
void request_tree_cache_rebuild();

void store_exchange(exchange_observed_t e)
{
    auto& st = s();
    bool topology_changed = false;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        if (e.id == 0) e.id = st.next_id.fetch_add(1);
        if (e.timestamp_ms == 0) e.timestamp_ms = now_ms();
        const std::string source = normalized_source(e);
        const std::string label = source_label_for(e);
        diag::log_tagged_fmt("burp", "site_map_store_exchange id=%llu source=%s source_label=%s host=%s path=%s status=%d",
            static_cast<unsigned long long>(e.id),
            source.c_str(),
            label.c_str(),
            e.host.c_str(),
            e.path.c_str(),
            e.status_code);
        topology_changed = insert_into_tree(st, e);
    }
    if (topology_changed) {
        st.topology_revision.fetch_add(1);
        request_tree_cache_rebuild();
    }
}

void handle_exchange(const exchange_observed_t& evt)
{
    if (![&]() {
        ::aida::infra::executor::submission_t sub;
        sub.owner_subsystem = "burp.site_map";
        sub.label = "site_map.store_exchange";
        sub.thread_class = "bounded_task";
        sub.domain = aida::infra::executor::domain_t::feature_worker;
        sub.priority = 3;
        sub.body = [evt]() { store_exchange(evt); };
        return ::aida::infra::executor::submit(std::move(sub)).submitted;
    }())
        diag::log_tagged("burp", "site_map_executor_post_failed");
}

std::string path_join(const std::string& parent, const std::string& seg)
{
    if (parent.empty() || parent == "/") return std::string("/") + (seg == "/" ? std::string() : seg);
    if (seg == "/") return parent;
    return parent + "/" + seg;
}

std::string normalized_source(const exchange_observed_t& e)
{
    return e.source.empty() ? std::string("proxy") : e.source;
}

std::string source_label_for(const exchange_observed_t& e)
{
    const std::string source = normalized_source(e);
    logger::source_t src = logger::source_t::proxy;
    if (logger::parse_source(source, src)) return logger::source_label(src);
    return source;
}

}

bool initialize()
{
    auto& st = s();
    st.shutting_down.store(false);
    bool expected = false;
    if (!st.initialized.compare_exchange_strong(expected, true)) return true;
    st.exchange_sub = aida::events::subscribe(kExchangeObservedEvent,
        [](const exchange_observed_t& e) { handle_exchange(e); });
    diag::log_tagged("burp", "site_map_initialized");
    return true;
}

void shutdown()
{
    auto& st = s();
    st.shutting_down.store(true);
    st.tree_rebuild_dirty.store(false);
    if (!st.initialized.exchange(false)) return;
    if (st.exchange_sub.valid()) aida::events::unsubscribe(st.exchange_sub);
}

void ingest_exchange(const exchange_observed_t& e)
{
    store_exchange(e);
}

uint64_t get_selected_exchange_id()
{
    return s().selected_exchange_id.load();
}

void set_selected_exchange_id(uint64_t id)
{
    s().selected_exchange_id.store(id);
}

void clear_selection()
{
    auto& st = s();
    st.selected_exchange_id.store(0);
}

bool find_exchange(uint64_t id, exchange_observed_t& out)
{
    auto& st = s();
    std::lock_guard<std::mutex> lk(st.mtx);
    const auto it = st.by_id.find(id);
    if (it == st.by_id.end()) return false;
    out = it->second;
    return true;
}

bool exchange_exists(uint64_t id)
{
    auto& st = s();
    std::lock_guard<std::mutex> lk(st.mtx);
    return st.by_id.find(id) != st.by_id.end();
}

std::vector<exchange_observed_t> list_exchanges_for(const std::string& host, uint16_t port, const std::string& path)
{
    auto& st = s();
    std::vector<exchange_observed_t> out;
    std::lock_guard<std::mutex> lk(st.mtx);
    for (const auto& kv : st.hosts) {
        if (kv.first.host != host || (port != 0 && kv.first.port != port)) continue;
        std::vector<std::string> segs;
        split_path_segments(path, segs);
        auto cur = kv.second->root;
        bool ok = true;
        for (const auto& seg : segs) {
            auto sit = cur->children.find(seg);
            if (sit == cur->children.end()) { ok = false; break; }
            cur = sit->second;
        }
        if (ok && cur) out.insert(out.end(), cur->exchanges.begin(), cur->exchanges.end());
    }
    return out;
}

std::vector<exchange_observed_t> list_all_exchanges()
{
    auto& st = s();
    std::vector<exchange_observed_t> out;
    std::lock_guard<std::mutex> lk(st.mtx);
    out.reserve(st.by_id.size());
    for (const auto& kv : st.by_id)
        out.push_back(kv.second);
    return out;
}

bool import_exchanges(const std::vector<exchange_observed_t>& exchanges, bool replace_existing)
{
    if (replace_existing)
        clear_all();
    for (const auto& exchange : exchanges)
        ingest_exchange(exchange);
    return true;
}

std::vector<host_summary_t> list_hosts(bool scope_only)
{
    auto& st = s();
    std::vector<host_summary_t> out;
    std::lock_guard<std::mutex> lk(st.mtx);
    for (const auto& kv : st.hosts) {
        const auto& h = *kv.second;
        if (scope_only && !h.in_scope) continue;
        host_summary_t hs;
        hs.host = h.host;
        hs.port = h.port;
        hs.tls  = h.tls;
        hs.in_scope = h.in_scope;
        hs.total_requests = h.total_requests;
        hs.issue_count = h.issue_count;
        out.push_back(hs);
    }
    return out;
}

namespace {
void collect_paths_rec(const std::shared_ptr<path_node_t>& n, const std::string& prefix, std::vector<std::string>& out)
{
    if (!n) return;
    const std::string here = (prefix.empty() && n->segment == "/") ? std::string("/") : path_join(prefix, n->segment);
    if (!n->exchanges.empty()) out.push_back(here);
    for (const auto& c : n->children) collect_paths_rec(c.second, here, out);
}
}

std::vector<std::string> list_paths(const std::string& host, uint16_t port)
{
    auto& st = s();
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(st.mtx);
    for (const auto& kv : st.hosts) {
        if (kv.first.host != host || (port != 0 && kv.first.port != port)) continue;
        collect_paths_rec(kv.second->root, std::string(), out);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void send_to(uint64_t exchange_id, const std::string& target, const std::string& source_view)
{
    send_to_action_t evt;
    evt.exchange_id = exchange_id;
    evt.target = target;
    evt.source_view = source_view;
    aida::events::publish(kSendToActionEvent, evt);
    diag::log_tagged_fmt("burp", "site_map_send_to id=%llu target=%s",
        static_cast<unsigned long long>(exchange_id), target.c_str());
}

namespace {

std::string base64_encode(const std::vector<uint8_t>& data)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                            static_cast<uint32_t>(data[i + 2]);
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6) & 0x3F]);
        out.push_back(tbl[v & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        const size_t rem = data.size() - i;
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (rem > 1) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(rem > 1 ? tbl[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

}

nlohmann::json exchange_to_json(const exchange_observed_t& e, bool include_bodies)
{
    nlohmann::json j;
    j["id"]            = e.id;
    j["timestamp_ms"]  = e.timestamp_ms;
    j["method"]        = e.method;
    j["scheme"]        = e.scheme;
    j["host"]          = e.host;
    j["port"]          = e.port;
    j["path"]          = e.path;
    j["query"]         = e.query;
    j["status_code"]   = e.status_code;
    j["reason"]        = e.reason_phrase;
    j["latency_ms"]    = e.latency_ms;
    j["is_websocket"]  = e.is_websocket;
    j["is_h2"]         = e.is_h2;
    j["tls_version"]   = e.tls_version;
    j["alpn"]          = e.alpn;
    j["client_addr"]   = e.client_addr;
    j["client_port"]   = e.client_port;
    j["source"]        = normalized_source(e);
    j["source_label"]  = source_label_for(e);

    nlohmann::json rh = nlohmann::json::array();
    for (const auto& kv : e.req_headers) {
        nlohmann::json h;
        h["name"]  = kv.first;
        h["value"] = kv.second;
        rh.push_back(h);
    }
    j["request_headers"] = rh;

    nlohmann::json sh = nlohmann::json::array();
    for (const auto& kv : e.resp_headers) {
        nlohmann::json h;
        h["name"]  = kv.first;
        h["value"] = kv.second;
        sh.push_back(h);
    }
    j["response_headers"] = sh;

    j["request_size"]  = e.req_body.size();
    j["response_size"] = e.resp_body.size();
    if (include_bodies) {
        j["request_body_base64"]  = base64_encode(e.req_body);
        j["response_body_base64"] = base64_encode(e.resp_body);
    }
    return j;
}

std::string last_error()
{
    auto& st = s();
    std::lock_guard<std::mutex> lk(st.err_mtx);
    return st.last_err;
}

size_t total_exchanges()
{
    return s().exchange_count.load();
}

void clear_all()
{
    auto& st = s();
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.hosts.clear();
        st.by_id.clear();
        st.exchange_index.clear();
        st.exchange_count.store(0);
        st.selected_exchange_id.store(0);
        st.topology_revision.fetch_add(1);
    }
    request_tree_cache_rebuild();
}

namespace {

struct tree_build_budget_t
{
    bool        limited = false;
    std::size_t retained_bytes = 0;
    std::size_t node_count = 0;
};

bool tree_budget_accept(tree_build_budget_t& budget, std::size_t bytes)
{
    if (budget.node_count >= kMaxCachedTreeRows ||
        bytes > kMaxCachedTreeTextBytes - budget.retained_bytes) {
        budget.limited = true;
        return false;
    }
    ++budget.node_count;
    budget.retained_bytes += bytes;
    return true;
}

std::shared_ptr<const site_map_node_t> build_path_snapshot(
    state_t& st, const std::shared_ptr<path_node_t>& node,
    const std::string& host_name, const std::string& path, const std::string& filter,
    tree_build_budget_t& budget)
{
    if (!node)
        return nullptr;
    std::string segment;
    bool in_scope = true;
    std::size_t total_requests = 0;
    std::uint64_t last_seen = 0;
    int last_status = 0;
    std::vector<std::pair<std::string, std::shared_ptr<path_node_t>>> children;
    {
        std::lock_guard<std::mutex> model_lk(st.mtx);
        segment = node->segment;
        in_scope = node->in_scope;
        total_requests = node->total_requests;
        last_seen = node->last_seen_ms;
        last_status = node->last_status;
        children.reserve(node->children.size());
        for (const auto& child : node->children)
            children.push_back(child);
    }

    std::vector<std::shared_ptr<const site_map_node_t>> child_nodes;
    child_nodes.reserve(children.size());
    bool child_matched = false;
    for (const auto& child : children) {
        const std::string child_path = path_join(path, child.first);
        auto child_node = build_path_snapshot(st, child.second, host_name,
            child_path, filter, budget);
        if (child_node) {
            child_matched = true;
            child_nodes.push_back(std::move(child_node));
        }
        if (budget.limited)
            break;
    }

    const bool matches = filter.empty() || path.find(filter) != std::string::npos ||
        host_name.find(filter) != std::string::npos;
    if (!matches && !child_matched)
        return nullptr;

    const std::size_t bytes = segment.size() + path.size() + host_name.size();
    if (!tree_budget_accept(budget, bytes))
        return nullptr;

    auto out = std::make_shared<site_map_node_t>();
    out->is_host = false;
    out->segment = segment;
    out->host = host_name;
    out->port = 0;
    out->tls = false;
    out->in_scope = in_scope;
    out->path = path;
    out->display = segment.empty() ? "/" : segment;
    out->total_requests = total_requests;
    out->last_seen_ms = last_seen;
    out->last_status = last_status;
    out->children = std::move(child_nodes);
    return out;
}

void build_tree_snapshot(state_t& st)
{
    std::string filter;
    uint64_t query_revision = 0;
    const uint64_t topology_revision = st.topology_revision.load();
    {
        std::lock_guard<std::mutex> cache_lk(st.cache_mtx);
        filter = st.tree_cache_filter;
        query_revision = st.tree_query_revision;
    }

    auto snapshot = std::make_shared<site_map_tree_snapshot_t>();
    snapshot->query_revision = query_revision;
    snapshot->topology_revision = topology_revision;
    tree_build_budget_t budget;

    std::vector<std::pair<host_key_t, std::shared_ptr<host_node_t>>> hosts;
    {
        std::lock_guard<std::mutex> model_lk(st.mtx);
        hosts.reserve((std::min)(kMaxCachedTreeRows, st.hosts.size()));
        for (const auto& host : st.hosts) {
            if (hosts.size() >= kMaxCachedTreeRows) {
                budget.limited = true;
                break;
            }
            hosts.push_back(host);
        }
    }

    for (const auto& kv : hosts) {
        std::string host_name;
        uint16_t host_port = 0;
        bool host_tls = false;
        bool host_in_scope = true;
        std::size_t host_requests = 0;
        std::size_t host_issues = 0;
        uint64_t host_last_seen = 0;
        std::shared_ptr<path_node_t> root;
        {
            std::lock_guard<std::mutex> model_lk(st.mtx);
            if (!kv.second)
                continue;
            host_name = kv.second->host;
            host_port = kv.second->port;
            host_tls = kv.second->tls;
            host_in_scope = kv.second->in_scope;
            host_requests = kv.second->total_requests;
            host_issues = kv.second->issue_count;
            host_last_seen = kv.second->last_seen_ms;
            root = kv.second->root;
        }

        std::vector<std::shared_ptr<const site_map_node_t>> child_nodes;
        if (root) {
            std::vector<std::pair<std::string, std::shared_ptr<path_node_t>>> children;
            {
                std::lock_guard<std::mutex> model_lk(st.mtx);
                children.reserve(root->children.size());
                for (const auto& child : root->children)
                    children.push_back(child);
            }
            child_nodes.reserve(children.size());
            bool child_matched = false;
            for (const auto& child : children) {
                const std::string child_path = path_join(std::string(), child.first);
                auto child_node = build_path_snapshot(st, child.second, host_name,
                    child_path, filter, budget);
                if (child_node) {
                    child_matched = true;
                    child_nodes.push_back(std::move(child_node));
                }
                if (budget.limited)
                    break;
            }
            const bool host_matches = filter.empty() ||
                host_name.find(filter) != std::string::npos;
            if (!host_matches && !child_matched)
                continue;
        } else if (!filter.empty() && host_name.find(filter) == std::string::npos) {
            continue;
        }

        const std::size_t bytes = host_name.size() + 32;
        if (!tree_budget_accept(budget, bytes))
            break;
        auto host_node_out = std::make_shared<site_map_node_t>();
        host_node_out->is_host = true;
        host_node_out->host = host_name;
        host_node_out->port = host_port;
        host_node_out->tls = host_tls;
        host_node_out->in_scope = host_in_scope;
        host_node_out->issue_count = host_issues;
        host_node_out->total_requests = host_requests;
        host_node_out->last_seen_ms = host_last_seen;
        char header[512];
        std::snprintf(header, sizeof(header), "%s://%s:%u  [%zu]",
            host_tls ? "https" : "http", host_name.c_str(), host_port, host_requests);
        host_node_out->display = header;
        host_node_out->children = std::move(child_nodes);
        snapshot->hosts.push_back(std::move(host_node_out));
        if (budget.limited)
            break;
    }

    snapshot->limited = budget.limited;
    std::lock_guard<std::mutex> cache_lk(st.cache_mtx);
    if (query_revision == st.tree_query_revision &&
        topology_revision == st.topology_revision.load()) {
        snapshot->error.clear();
        st.tree_snapshot = std::move(snapshot);
        st.tree_cache_limited = budget.limited;
        st.tree_snapshot_revision.fetch_add(1);
        st.tree_retry_attempt.store(0);
        st.tree_retry_after_ms.store(0);
        std::lock_guard<std::mutex> err_lk(st.err_mtx);
        st.last_err.clear();
    } else {
        st.tree_rebuild_dirty.store(true);
    }
}

void request_tree_cache_rebuild()
{
    auto& st = s();
    if (st.shutting_down.load()) return;
    st.tree_rebuild_dirty.store(true);
    const uint64_t now = monotonic_ms();
    if (now < st.tree_retry_after_ms.load()) return;
    bool expected = false;
    if (!st.tree_rebuild_inflight.compare_exchange_strong(expected, true)) return;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "burp.site_map";
    submission.label = "site_map.rebuild_tree_cache";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 2;
    submission.body = [] {
        auto& state = s();
        state.tree_rebuild_dirty.store(false);
        try {
            build_tree_snapshot(state);
        } catch (const std::exception& ex) {
            state.tree_rebuild_dirty.store(true);
            const uint32_t attempt = std::min<uint32_t>(
                state.tree_retry_attempt.fetch_add(1), 5);
            state.tree_retry_after_ms.store(monotonic_ms() +
                std::min<uint64_t>(10000, 250ull << attempt));
            {
                std::lock_guard<std::mutex> err_lk(state.err_mtx);
                state.last_err = std::string("Site-map index rebuild failed: ") + ex.what();
            }
        } catch (...) {
            state.tree_rebuild_dirty.store(true);
            const uint32_t attempt = std::min<uint32_t>(
                state.tree_retry_attempt.fetch_add(1), 5);
            state.tree_retry_after_ms.store(monotonic_ms() +
                std::min<uint64_t>(10000, 250ull << attempt));
            {
                std::lock_guard<std::mutex> err_lk(state.err_mtx);
                state.last_err = "Site-map index rebuild failed";
            }
        }
        state.tree_rebuild_inflight.store(false);
        if (state.tree_rebuild_dirty.load()) request_tree_cache_rebuild();
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        st.tree_rebuild_inflight.store(false);
        const uint32_t attempt = std::min<uint32_t>(st.tree_retry_attempt.fetch_add(1), 5);
        const uint64_t delay = std::min<uint64_t>(10000, 250ull << attempt);
        st.tree_retry_after_ms.store(now + delay);
        std::lock_guard<std::mutex> err_lk(st.err_mtx);
        st.last_err = "Site-map index rebuild could not be scheduled";
    }
}

}

std::shared_ptr<const site_map_tree_snapshot_t> tree_snapshot()
{
    auto& st = s();
    std::lock_guard<std::mutex> cache_lk(st.cache_mtx);
    return st.tree_snapshot;
}

std::uint64_t tree_snapshot_revision()
{
    return s().tree_snapshot_revision.load(std::memory_order_acquire);
}

void set_tree_filter(const std::string& filter)
{
    auto& st = s();
    {
        std::lock_guard<std::mutex> cache_lk(st.cache_mtx);
        if (st.tree_cache_filter == filter)
            return;
        st.tree_cache_filter = filter;
        ++st.tree_query_revision;
    }
    request_tree_cache_rebuild();
}

void request_tree_rebuild()
{
    request_tree_cache_rebuild();
}

std::vector<exchange_row_t> exchange_rows_for(const std::string& host, std::uint16_t port,
                                              bool tls, const std::string& path)
{
    auto& st = s();
    std::vector<exchange_row_t> rows;
    const host_key_t key{host, port, tls};
    std::lock_guard<std::mutex> lk(st.mtx);
    if (path.empty()) {
        const auto index = st.exchange_index.find(key);
        if (index == st.exchange_index.end())
            return rows;
        rows.reserve(index->second.size());
        for (const auto& row : index->second)
            rows.push_back(row);
        return rows;
    }
    const auto host_it = st.hosts.find(key);
    if (host_it == st.hosts.end() || !host_it->second || !host_it->second->root)
        return rows;
    std::vector<std::string> segments;
    split_path_segments(path, segments);
    auto node = host_it->second->root;
    for (const auto& segment : segments) {
        const auto child = node->children.find(segment);
        if (child == node->children.end())
            return rows;
        node = child->second;
    }
    rows.reserve(node->exchanges.size());
    for (const auto& exchange : node->exchanges)
        rows.push_back(make_exchange_row(exchange));
    return rows;
}

}
}
}
