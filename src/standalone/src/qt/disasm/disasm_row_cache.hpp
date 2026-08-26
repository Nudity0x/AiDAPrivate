#pragma once

#include "core/disasm/disasm_view.hpp"
#include "qt/analysis_bridge/disasm_workspace_model.hpp"

#include <QHash>
#include <QObject>
#include <QPointer>

#include <cstdint>
#include <memory>
#include <optional>

namespace aida::qt::disasm {

class DisasmRowCache : public QObject {
    Q_OBJECT
public:
    explicit DisasmRowCache(QObject* parent = nullptr);
    ~DisasmRowCache() override;

    void bind(const std::shared_ptr<disasm_view::state_t>& state);
    void unbind();
    bool bound() const noexcept { return static_cast<bool>(state_); }

    using fence_fn_t = std::function<bool(quint64 generation, quint64 analysis_revision,
                                          quint64 overlay_revision)>;
    void set_fence(fence_fn_t fence) { fence_ = std::move(fence); }

    std::optional<disasm_view::formatted_instruction_t> find(
        aida::analysis::entity_id_t instruction) const noexcept
    {
        const auto it = formatted_.constFind(static_cast<quint64>(instruction));
        return it == formatted_.constEnd()
            ? std::nullopt
            : std::optional<disasm_view::formatted_instruction_t>(it.value());
    }

    quint64 revision() const noexcept { return revision_; }

    void clearAll();

Q_SIGNALS:
    void revisionChanged(quint64 revision);
    void xrefResults(aida::analysis::address_t address,
                     std::vector<disasm_view::xref_popup_entry_t> results,
                     QString error);
    void exportStatusChanged(QString status, QString error);

private:
    void mergePage(analysis_bridge::format_page_delivery_t delivery);
    void mergeXref(analysis_bridge::xref_delivery_t delivery);
    void mergeExport(analysis_bridge::export_delivery_t delivery);

    std::shared_ptr<disasm_view::state_t> state_;
    std::shared_ptr<analysis_bridge::format_delivery_fn> format_target_;
    std::shared_ptr<analysis_bridge::xref_delivery_fn> xref_target_;
    std::shared_ptr<analysis_bridge::export_delivery_fn> export_target_;
    QHash<quint64, disasm_view::formatted_instruction_t> formatted_;
    fence_fn_t fence_;
    quint64 revision_ = 0;
};

}
