#include "qt/disasm/disasm_row_cache.hpp"

namespace aida::qt::disasm {

DisasmRowCache::DisasmRowCache(QObject* parent) : QObject(parent) {}

DisasmRowCache::~DisasmRowCache()
{
    unbind();
}

void DisasmRowCache::bind(const std::shared_ptr<disasm_view::state_t>& state)
{
    unbind();
    if (!state)
        return;
    state_ = state;
    const QPointer<DisasmRowCache> guard(this);
    format_target_ = std::make_shared<analysis_bridge::format_delivery_fn>(
        [guard](analysis_bridge::format_page_delivery_t delivery) {
            if (guard)
                guard->mergePage(std::move(delivery));
        });
    xref_target_ = std::make_shared<analysis_bridge::xref_delivery_fn>(
        [guard](analysis_bridge::xref_delivery_t delivery) {
            if (guard)
                guard->mergeXref(std::move(delivery));
        });
    export_target_ = std::make_shared<analysis_bridge::export_delivery_fn>(
        [guard](analysis_bridge::export_delivery_t delivery) {
            if (guard)
                guard->mergeExport(std::move(delivery));
        });
    analysis_bridge::set_format_delivery_target(state, format_target_);
    analysis_bridge::set_xref_delivery_target(state, xref_target_);
    analysis_bridge::set_export_delivery_target(state, export_target_);
}

void DisasmRowCache::clearAll()
{
    formatted_.clear();
    ++revision_;
    Q_EMIT revisionChanged(revision_);
}

void DisasmRowCache::unbind()
{
    if (state_)
        analysis_bridge::clear_delivery_targets(state_);
    state_.reset();
    format_target_.reset();
    xref_target_.reset();
    export_target_.reset();
    formatted_.clear();
}

void DisasmRowCache::mergePage(analysis_bridge::format_page_delivery_t delivery)
{
    if (delivery.reset) {
        formatted_.clear();
        ++revision_;
        Q_EMIT revisionChanged(revision_);
        return;
    }
    if (!delivery.error.empty()) {
        ++revision_;
        Q_EMIT revisionChanged(revision_);
        return;
    }
    if (fence_ && !fence_(delivery.generation, delivery.analysis_revision,
            delivery.overlay_revision))
        return;
    for (auto& item : delivery.rows)
        formatted_.insert(static_cast<quint64>(item.first), std::move(item.second));
    ++revision_;
    Q_EMIT revisionChanged(revision_);
}

void DisasmRowCache::mergeXref(analysis_bridge::xref_delivery_t delivery)
{
    Q_EMIT xrefResults(delivery.address, std::move(delivery.results),
        QString::fromStdString(delivery.error));
}

void DisasmRowCache::mergeExport(analysis_bridge::export_delivery_t delivery)
{
    Q_EMIT exportStatusChanged(QString::fromStdString(delivery.status),
        QString::fromStdString(delivery.error));
}

}
