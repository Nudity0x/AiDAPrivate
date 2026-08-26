#pragma once

#include <memory>
#include <string>

namespace aida::qt::docking {
class AidaDockHost;
}

namespace aida::qt::bridge {
class MenuBridge;
}

namespace aida::qt::registry {
struct view_instance_id_t;
}

namespace aida::qt::documents {

class AidaDocumentModel;
class AidaDocumentController;

struct document_domain_t {
    AidaDocumentModel* model = nullptr;
    AidaDocumentController* controller = nullptr;
};

document_domain_t& document_domain();

void install_document_domain(docking::AidaDockHost* host, bridge::MenuBridge* menus = nullptr);

}
