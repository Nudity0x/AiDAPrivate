#include "core/editor/hex_view.hpp"

#include "qt/editor/aida_hex_document.hpp"

namespace hex_view {

using aida::qt::editor::AidaHexDocumentRegistry;

void activate(const disasm_view::workspace_context_t& context)
{
    auto state = AidaHexDocumentRegistry::instance().stateFor(context);
    if (state)
        state->activate(context);
}

bool focus_address(const disasm_view::workspace_context_t& context,
                   const aida::analysis::address_t& address, std::string* error)
{
    auto state = AidaHexDocumentRegistry::instance().stateFor(context);
    if (!state) {
        if (error) *error = "The selected workspace has no hex provider.";
        return false;
    }
    return state->focusAddress(context, address, error);
}

bool request_live_memory(const disasm_view::workspace_context_t& context,
                         std::uint64_t address, std::size_t size)
{
    auto state = AidaHexDocumentRegistry::instance().stateFor(context);
    if (!state)
        return false;
    return state->requestLiveMemory(context, address, size);
}

void close(const disasm_view::workspace_context_t& context)
{
    AidaHexDocumentRegistry::instance().close(context);
}

bool active(const disasm_view::workspace_context_t& context)
{
    auto state = AidaHexDocumentRegistry::instance().stateFor(context);
    if (!state)
        return false;
    return state->isActive(context);
}

std::string source_name(const disasm_view::workspace_context_t& context)
{
    auto state = AidaHexDocumentRegistry::instance().stateFor(context);
    if (!state)
        return {};
    return state->sourceName(context);
}

std::string last_error(const disasm_view::workspace_context_t& context)
{
    auto state = AidaHexDocumentRegistry::instance().stateFor(context);
    if (!state)
        return "TARGET_NOT_FOUND: workspace is unavailable";
    return state->lastError();
}

}
