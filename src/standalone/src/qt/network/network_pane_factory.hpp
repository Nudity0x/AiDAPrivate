#pragma once

#include <string_view>
#include <vector>

#include "core/network/network_view.hpp"

class QWidget;

namespace aida::qt::net {

struct network_nav_group_t {
    const char* label;
    const char* short_label;
    const char* status;
    std::vector<network_view::sub_tab_t> tabs;
};

QWidget* createNetworkPane(network_view::sub_tab_t tab, QWidget* parent = nullptr);

const char* network_tab_name(network_view::sub_tab_t tab) noexcept;
const char* network_tab_short_name(network_view::sub_tab_t tab) noexcept;
bool network_tab_requires_target(network_view::sub_tab_t tab) noexcept;
const char* network_view_id_for_tab(network_view::sub_tab_t tab) noexcept;
network_view::sub_tab_t network_tab_for_view_id(std::string_view viewId, bool& found) noexcept;

const std::vector<network_nav_group_t>& network_nav_groups();
int network_nav_group_for_tab(network_view::sub_tab_t tab) noexcept;

}
