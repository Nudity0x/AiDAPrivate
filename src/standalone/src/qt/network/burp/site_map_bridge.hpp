#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aida::burp::sitemap {

// Immutable tree snapshot node, built by the site-map rebuild worker off the
// live store and adopted by the Qt tree model. A host node (is_host) carries
// the scheme://host:port display; a path node carries its segment + full path.
struct site_map_node_t
{
    bool        is_host = false;
    std::string segment;
    std::string host;
    std::uint16_t port = 0;
    bool        tls = false;
    bool        in_scope = true;
    std::string path;
    std::string display;
    std::size_t total_requests = 0;
    std::uint64_t last_seen_ms = 0;
    int         last_status = 0;
    std::size_t issue_count = 0;
    std::vector<std::shared_ptr<const site_map_node_t>> children;
};

struct site_map_tree_snapshot_t
{
    std::vector<std::shared_ptr<const site_map_node_t>> hosts;
    std::uint64_t query_revision = 0;
    std::uint64_t topology_revision = 0;
    bool        limited = false;
    std::string error;
};

struct exchange_row_t
{
    std::uint64_t id = 0;
    std::uint64_t timestamp_ms = 0;
    std::string   method;
    std::string   path;
    int           status_code = 0;
    std::size_t   response_size = 0;
    std::uint64_t latency_ms = 0;
};

std::shared_ptr<const site_map_tree_snapshot_t> tree_snapshot();
std::uint64_t tree_snapshot_revision();
void set_tree_filter(const std::string& filter);
void request_tree_rebuild();
std::vector<exchange_row_t> exchange_rows_for(const std::string& host, std::uint16_t port,
                                              bool tls, const std::string& path);

}
