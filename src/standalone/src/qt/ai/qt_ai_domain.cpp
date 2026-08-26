#include "qt/ai/qt_ai_domain.hpp"

#include "helpers/diag_log.hpp"
#include "qt/docking/dock_host.hpp"

namespace aida::qt::ai {

ai_domain_t& ai_domain() {
    static ai_domain_t domain;
    return domain;
}

void open_ai_view(const std::string& view_id) {
    auto& domain = ai_domain();
    if (!domain.host)
        return;
    const auto result = domain.host->open_or_focus(registry::stable_view_id_t(view_id));
    if (!result.ok()) {
        diag::log_tagged_fmt("qt_ai", "open_view_failed view=%s detail=%s",
            view_id.c_str(), result.detail.c_str());
    }
}

}
