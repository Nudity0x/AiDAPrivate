#pragma once

#include "qt/registry/qt_view_registry.hpp"

#include <QWidget>

#include <vector>

class QTabBar;
class QStackedLayout;
class QShowEvent;

namespace aida::qt::docking {

class AidaHubWidget : public QWidget {
    Q_OBJECT
public:
    AidaHubWidget(registry::hub_kind_t hub, registry::qt_view_registry_t* registry,
                  QWidget* parent = nullptr, bool defer_pages = false);
    ~AidaHubWidget() override;

    registry::hub_kind_t hub() const noexcept { return hub_; }
    int current_subview() const;
    int subview_count() const noexcept;
    void set_subview(int subview);
    void rebuild_page(int subview);
    void ensure_current_page();

Q_SIGNALS:
    void subviewActivated(int subview);

protected:
    void showEvent(QShowEvent* event) override;

private:
    struct member_t {
        registry::stable_view_id_t id;
        int subview = 0;
        bool created = false;
    };

    void ensure_page(int index);
    void activate_index(int index);
    void activate_subview(int subview);

    registry::hub_kind_t hub_ = registry::hub_kind_t::none;
    registry::qt_view_registry_t* registry_ = nullptr;
    QTabBar* tab_bar_ = nullptr;
    QWidget* stack_host_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    std::vector<member_t> members_;
    bool activating_ = false;
    bool pages_deferred_ = false;
    int pending_subview_ = -1;
};

}
