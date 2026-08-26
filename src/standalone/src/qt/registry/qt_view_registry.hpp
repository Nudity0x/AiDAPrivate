#pragma once

#include "qt/registry/qt_view_descriptor.hpp"
#include "qt/registry/view_catalog.hpp"

#include <QObject>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aida::qt::registry {

class qt_view_registry_t : public QObject {
    Q_OBJECT
public:
    struct host_hooks_t {
        std::function<view_operation_result_t(const qt_view_descriptor_t& descriptor,
                                              const view_instance_id_t& id,
                                              const std::string& display_name)> open_instance;
        std::function<view_operation_result_t(const view_instance_id_t& id)> focus_instance;
        std::function<view_operation_result_t(const view_instance_id_t& id)> close_instance;
        std::function<view_operation_result_t(const view_instance_id_t& id)> erase_instance;
        std::function<void(const view_instance_id_t& id)> deactivate_instance;
    };

    explicit qt_view_registry_t(QObject* parent = nullptr);
    ~qt_view_registry_t() override;

    void set_host_hooks(host_hooks_t hooks);
    void set_hub_activation_hook(std::function<void(hub_kind_t hub, int subview)> hook);
    void set_workspace_available_hook(std::function<bool()> hook);
    void set_image_available_hook(std::function<bool()> hook);

    std::size_t register_catalog(const qt_view_factory_t& placeholder_factory);
    view_operation_result_t register_view(qt_view_descriptor_t descriptor);
    view_operation_result_t install_view_factory(const stable_view_id_t& id,
                                                 qt_view_factory_t factory,
                                                 std::optional<content_policy_t> policy = std::nullopt);
    bool is_ported(const stable_view_id_t& id) const;

    const qt_view_descriptor_t* find_descriptor(const stable_view_id_t& id) const noexcept;
    const view_instance_state_t* find_instance(const view_instance_id_t& id) const noexcept;
    stable_view_id_t canonical_view_id(const stable_view_id_t& id) const;
    view_instance_id_t instance_for(const stable_view_id_t& id) const;

    capability_state_t evaluate(const stable_view_id_t& id,
                                const interaction_context_t& context) const;
    view_operation_result_t open(const view_instance_id_t& id,
                                 const interaction_context_t& context,
                                 std::string display_name = {});
    view_operation_result_t ensure_identity(const view_instance_id_t& id,
                                            std::string display_name = {});
    view_operation_result_t focus(const view_instance_id_t& id);
    view_operation_result_t open_or_focus(const view_instance_id_t& id,
                                          const interaction_context_t& context,
                                          std::string display_name = {});
    view_operation_result_t close(const view_instance_id_t& id);
    view_operation_result_t reopen_last_closed(const interaction_context_t& context);
    view_operation_result_t open_default_missing(const interaction_context_t& context);
    view_operation_result_t erase_closed_instance(const view_instance_id_t& id);

    void sync_instance_visibility(const view_instance_id_t& id, bool open,
                                  bool record_history);
    void note_hub_subview(hub_kind_t hub, int subview);
    std::optional<stable_view_id_t> hub_active_view(hub_kind_t hub) const;

    bool consume_focus_request(const view_instance_id_t& id) noexcept;
    void update_focus(const std::optional<view_instance_id_t>& focused);
    bool is_open(const view_instance_id_t& id) const noexcept;
    bool can_reopen_last_closed() const noexcept { return !closed_history_.empty(); }
    std::optional<view_instance_id_t> focused_instance() const;

    void for_each_descriptor(const std::function<void(const qt_view_descriptor_t&)>& visitor) const;
    void for_each_instance(const std::function<void(const qt_view_descriptor_t&,
                                                    const view_instance_state_t&)>& visitor,
                           bool open_only = false) const;
    void for_each_menu_entry(const std::function<void(const menu_entry_t&)>& visitor);

    std::size_t descriptor_count() const noexcept { return descriptors_.size(); }
    std::size_t instance_count() const noexcept { return instances_.size(); }
    std::uint64_t revision() const noexcept { return revision_; }
    std::uint64_t visibility_revision() const noexcept { return visibility_revision_; }

    interaction_context_t& context() noexcept { return context_; }
    const interaction_context_t& context() const noexcept { return context_; }

    std::string persistence_fingerprint() const noexcept;

Q_SIGNALS:
    void descriptorsChanged();
    void instancesChanged();
    void visibilityRevisionChanged();
    void focusedInstanceChanged();

private:
    static view_operation_result_t validate_descriptor(const qt_view_descriptor_t& descriptor);
    view_operation_result_t validate_instance(const qt_view_descriptor_t& descriptor,
                                              const view_instance_id_t& id) const;
    view_instance_state_t& ensure_instance(const qt_view_descriptor_t& descriptor,
                                           const view_instance_id_t& id,
                                           std::string display_name);
    view_instance_state_t* find_instance(const view_instance_id_t& id) noexcept;
    void close_bookkeeping(const view_instance_id_t& id, bool record_history);

    std::map<stable_view_id_t, qt_view_descriptor_t> descriptors_;
    std::map<view_instance_id_t, view_instance_state_t> instances_;
    std::vector<view_instance_id_t> closed_history_;
    std::optional<view_instance_id_t> focused_instance_;
    std::uint64_t focus_sequence_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t visibility_revision_ = 0;
    interaction_context_t context_;
    host_hooks_t hooks_;
    std::function<void(hub_kind_t, int)> hub_activation_hook_;
    std::function<bool()> workspace_available_hook_;
    std::function<bool()> image_available_hook_;
    std::map<hub_kind_t, stable_view_id_t> hub_active_member_;
};

}
