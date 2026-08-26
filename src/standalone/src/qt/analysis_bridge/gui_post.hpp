#pragma once

#include <functional>

namespace aida::qt {

using gui_post_fn = std::function<void(std::function<void()>)>;

void set_gui_post(gui_post_fn fn);
gui_post_fn gui_post();
bool gui_post_installed();
void gui_post_or_run(std::function<void()> fn);

}
