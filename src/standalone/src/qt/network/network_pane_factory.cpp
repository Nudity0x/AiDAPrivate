#include "qt/network/network_pane_factory.hpp"

#include "qt/network/burp/cookie_jar_pane.hpp"
#include "qt/network/burp/logger_pane.hpp"
#include "qt/network/burp/scope_pane.hpp"
#include "qt/network/burp/site_map_pane.hpp"
#include "qt/network/burp/upstream_pane.hpp"
#include "qt/network/monitor/bandwidth_pane.hpp"
#include "qt/network/monitor/capture_pane.hpp"
#include "qt/network/monitor/connections_pane.hpp"
#include "qt/network/monitor/dns_pane.hpp"
#include "qt/network/monitor/filters_pane.hpp"
#include "qt/network/monitor/intercept_pane.hpp"
#include "qt/network/monitor/keylog_pane.hpp"
#include "qt/network/monitor/pcap_export_pane.hpp"
#include "qt/network/monitor/proxy_pane.hpp"
#include "qt/net/net_editors_install.hpp"
#include "qt/network/network_attack_factory.hpp"

namespace aida::qt::net {

namespace {

using network_view::sub_tab_t;

const char* k_tab_names[] = {
    "Connections", "Capture", "Intercept", "Proxy",
    "DNS", "Filters", "Bandwidth", "Repeater", "KeyLog",
    "PCAP", "Fuzzer", "Offensive", "WebSocket", "Scripting", "Decoder",
    "Site Map", "Scope", "Cookies", "Scanner", "Recon",
    "Intruder", "Collaborator", "Sequencer", "Comparer",
    "JWT Lab", "Match/Replace", "Session", "API",
    "WS Editor", "H/2 Editor", "Logger", "CSP",
    "Upstream", "Browser", "Reports", "Headless"
};

const char* k_tab_short_names[] = {
    "Conn", "Cap", "Int", "Prx",
    "DNS", "Filt", "BW", "Rep", "KL",
    "PCAP", "Fuz", "Off", "WS", "Scr", "Dec",
    "Site", "Scope", "Cook", "Scan", "Recon",
    "Intr", "Collab", "Seq", "Cmp",
    "JWT", "M/R", "Sess", "API",
    "WSe", "H2e", "Log", "CSP",
    "Up", "Brw", "Rpt", "HL"
};

const char* k_tab_view_ids[] = {
    "view.network.connections", "view.network.capture", "view.network.intercept",
    "view.network.proxy", "view.network.dns", "view.network.filters",
    "view.network.bandwidth", "view.network.repeater", "view.network.keylog",
    "view.network.pcap", "view.network.fuzzer", "view.network.offensive",
    "view.network.websocket", "view.network.scripting", "view.network.decoder",
    "view.network.site_map", "view.network.scope", "view.network.cookies",
    "view.network.scanner", "view.network.recon", "view.network.intruder",
    "view.network.collaborator", "view.network.sequencer", "view.network.comparer",
    "view.network.jwt_lab", "view.network.match_replace", "view.network.session",
    "view.network.api", "view.network.ws_editor", "view.network.h2_editor",
    "view.network.logger", "view.network.csp", "view.network.upstream",
    "view.network.browser", "view.network.reports", "view.network.headless"
};

const std::vector<network_nav_group_t> k_nav_groups = {
    { "Monitor", "Mon", "Driver capture and packet evidence",
        { sub_tab_t::connections, sub_tab_t::capture, sub_tab_t::dns, sub_tab_t::filters,
          sub_tab_t::bandwidth, sub_tab_t::keylog, sub_tab_t::pcap_export } },
    { "Proxy", "Proxy", "MITM, intercept, replay, logs, and scope",
        { sub_tab_t::proxy, sub_tab_t::intercept, sub_tab_t::repeater, sub_tab_t::logger,
          sub_tab_t::scope, sub_tab_t::cookies, sub_tab_t::upstream } },
    { "Web", "Web", "Camoufox-only browser automation and web scans",
        { sub_tab_t::browser, sub_tab_t::headless, sub_tab_t::sitemap, sub_tab_t::recon,
          sub_tab_t::scanner, sub_tab_t::reports, sub_tab_t::csp } },
    { "API", "API", "API request workbench", { sub_tab_t::api } },
    { "Attack", "Attack", "Fuzzing, sessions, tokens, and comparison tools",
        { sub_tab_t::intruder, sub_tab_t::fuzzer, sub_tab_t::offensive, sub_tab_t::jwt,
          sub_tab_t::mr, sub_tab_t::session, sub_tab_t::collab, sub_tab_t::sequencer,
          sub_tab_t::comparer } },
    { "Protocol", "Proto", "WebSocket, HTTP/2, and decoding tools",
        { sub_tab_t::websocket, sub_tab_t::ws_edit, sub_tab_t::h2_edit, sub_tab_t::decoder } },
    { "Automation", "Auto", "Network scripting and workflow automation",
        { sub_tab_t::scripting } },
};

}

const char* network_tab_name(network_view::sub_tab_t tab) noexcept {
    const int index = static_cast<int>(tab);
    if (index < 0 || index >= static_cast<int>(sub_tab_t::COUNT))
        return "Network";
    return k_tab_names[index];
}

const char* network_tab_short_name(network_view::sub_tab_t tab) noexcept {
    const int index = static_cast<int>(tab);
    if (index < 0 || index >= static_cast<int>(sub_tab_t::COUNT))
        return "Network";
    return k_tab_short_names[index];
}

bool network_tab_requires_target(network_view::sub_tab_t tab) noexcept {
    switch (tab) {
    case sub_tab_t::connections:
    case sub_tab_t::capture:
    case sub_tab_t::dns:
    case sub_tab_t::filters:
    case sub_tab_t::bandwidth:
    case sub_tab_t::keylog:
    case sub_tab_t::pcap_export:
        return true;
    default:
        return false;
    }
}

const char* network_view_id_for_tab(network_view::sub_tab_t tab) noexcept {
    const int index = static_cast<int>(tab);
    if (index < 0 || index >= static_cast<int>(sub_tab_t::COUNT))
        return "view.network";
    return k_tab_view_ids[index];
}

network_view::sub_tab_t network_tab_for_view_id(std::string_view viewId, bool& found) noexcept {
    found = true;
    for (int index = 0; index < static_cast<int>(sub_tab_t::COUNT); ++index) {
        if (viewId == k_tab_view_ids[index])
            return static_cast<sub_tab_t>(index);
    }
    found = false;
    return sub_tab_t::connections;
}

const std::vector<network_nav_group_t>& network_nav_groups() {
    return k_nav_groups;
}

int network_nav_group_for_tab(network_view::sub_tab_t tab) noexcept {
    for (std::size_t groupIndex = 0; groupIndex < k_nav_groups.size(); ++groupIndex) {
        for (const auto member : k_nav_groups[groupIndex].tabs) {
            if (member == tab)
                return static_cast<int>(groupIndex);
        }
    }
    return 0;
}

QWidget* createNetworkPane(network_view::sub_tab_t tab, QWidget* parent) {
    using sub_tab_t = network_view::sub_tab_t;
    switch (tab) {
    case sub_tab_t::connections: return new ConnectionsPane(parent);
    case sub_tab_t::capture:     return new CapturePane(parent);
    case sub_tab_t::intercept:   return new InterceptPane(parent);
    case sub_tab_t::proxy:       return new ProxyPane(parent);
    case sub_tab_t::dns:         return new DnsPane(parent);
    case sub_tab_t::filters:     return new FiltersPane(parent);
    case sub_tab_t::bandwidth:   return new BandwidthPane(parent);
    case sub_tab_t::keylog:      return new KeylogPane(parent);
    case sub_tab_t::pcap_export: return new PcapExportPane(parent);
    case sub_tab_t::sitemap:     return new SiteMapPane(parent);
    case sub_tab_t::scope:       return new ScopePane(parent);
    case sub_tab_t::cookies:     return new CookieJarPane(parent);
    case sub_tab_t::logger:      return new LoggerPane(parent);
    case sub_tab_t::upstream:    return new UpstreamPane(parent);
    default: {
        if (auto* editor_pane = create_network_editor_pane(tab, parent))
            return editor_pane;
        if (auto* attack_pane = create_network_attack_pane(tab, parent))
            return attack_pane;
        return nullptr;
    }
    }
}

}
