#include "qt/docking/hub_dock.hpp"

#include "helpers/diag_log.hpp"
#include "qt/docking/view_placeholder.hpp"

#include <QShowEvent>
#include <QTabBar>
#include <QStackedLayout>
#include <QVBoxLayout>

#include <algorithm>

namespace aida::qt::docking {

AidaHubWidget::AidaHubWidget(registry::hub_kind_t hub,
                             registry::qt_view_registry_t* registry, QWidget* parent,
                             bool defer_pages)
    : QWidget(parent), hub_(hub), registry_(registry), pages_deferred_(defer_pages) {
    const QString hub_name = QString::fromLatin1(registry::hub_kind_name(hub_));
    setObjectName(QStringLiteral("aida.hub_dock.%1").arg(hub_name));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    tab_bar_ = new QTabBar(this);
    tab_bar_->setObjectName(QStringLiteral("aida.hub_dock.%1.tab_bar").arg(hub_name));
    tab_bar_->setMovable(false);
    tab_bar_->setTabsClosable(false);
    tab_bar_->setDocumentMode(true);
    tab_bar_->setDrawBase(false);
    tab_bar_->setExpanding(false);
    tab_bar_->setElideMode(Qt::ElideRight);
    tab_bar_->setUsesScrollButtons(true);
    layout->addWidget(tab_bar_);
    stack_host_ = new QWidget(this);
    stack_host_->setObjectName(QStringLiteral("aida.hub_dock.%1.pages").arg(hub_name));
    stack_ = new QStackedLayout(stack_host_);
    stack_->setStackingMode(QStackedLayout::StackOne);
    layout->addWidget(stack_host_, 1);

    if (registry_) {
        registry_->for_each_descriptor([&](const registry::qt_view_descriptor_t& descriptor) {
            if (descriptor.hub == hub_)
                members_.push_back({descriptor.id, descriptor.hub_subview, false});
        });
    }
    std::sort(members_.begin(), members_.end(),
        [](const member_t& lhs, const member_t& rhs) { return lhs.subview < rhs.subview; });
    for (const auto& member : members_) {
        const auto* descriptor = registry_ ? registry_->find_descriptor(member.id) : nullptr;
        const QString label = QString::fromStdString(descriptor
            ? descriptor->display_name : member.id.value());
        const int index = tab_bar_->addTab(label);
        tab_bar_->setTabData(index, member.subview);
        tab_bar_->setTabToolTip(index, label);
        stack_->insertWidget(index, new AidaViewPlaceholder(member.id.value(), stack_host_));
    }
    connect(tab_bar_, &QTabBar::currentChanged, this, [this](int index) {
        activate_index(index);
    });
    if (!pages_deferred_)
        activate_index(tab_bar_->currentIndex());
}

AidaHubWidget::~AidaHubWidget() = default;

int AidaHubWidget::current_subview() const {
    const int index = tab_bar_->currentIndex();
    if (index < 0)
        return -1;
    return tab_bar_->tabData(index).toInt();
}

int AidaHubWidget::subview_count() const noexcept {
    return static_cast<int>(members_.size());
}

void AidaHubWidget::set_subview(int subview) {
    if (pages_deferred_ && !isVisible()) {
        pending_subview_ = subview;
        return;
    }
    activate_subview(subview);
}

void AidaHubWidget::activate_subview(int subview) {
    for (int index = 0; index < tab_bar_->count(); ++index) {
        if (tab_bar_->tabData(index).toInt() == subview) {
            if (tab_bar_->currentIndex() == index)
                activate_index(index);
            else
                tab_bar_->setCurrentIndex(index);
            return;
        }
    }
    diag::log_tagged_fmt("qt_dock_hub",
        "hub_subview_rejected hub=%s subview=%d reason=unknown_subview",
        registry::hub_kind_name(hub_), subview);
}

void AidaHubWidget::rebuild_page(int subview) {
    for (int index = 0; index < tab_bar_->count(); ++index) {
        if (tab_bar_->tabData(index).toInt() != subview)
            continue;
        if (static_cast<std::size_t>(index) >= members_.size())
            return;
        auto& member = members_[static_cast<std::size_t>(index)];
        if (!member.created)
            return;
        const auto* descriptor = registry_ ? registry_->find_descriptor(member.id) : nullptr;
        if (!descriptor || !descriptor->factory)
            return;
        QWidget* replacement = descriptor->factory(stack_host_,
            registry::view_instance_id_t{member.id, registry::stable_view_instance_key_t{}});
        if (!replacement) {
            diag::log_tagged_critical_fmt("qt_dock_hub",
                "hub_page_factory_null hub=%s view=%s",
                registry::hub_kind_name(hub_), member.id.c_str());
            return;
        }
        QWidget* previous = stack_->widget(index);
        stack_->removeWidget(previous);
        if (previous)
            previous->deleteLater();
        stack_->insertWidget(index, replacement);
        if (tab_bar_->currentIndex() == index)
            stack_->setCurrentIndex(index);
        return;
    }
}

void AidaHubWidget::ensure_page(int index) {
    if (index < 0 || index >= stack_->count() ||
        static_cast<std::size_t>(index) >= members_.size())
        return;
    auto& member = members_[static_cast<std::size_t>(index)];
    if (member.created)
        return;
    const auto* descriptor = registry_ ? registry_->find_descriptor(member.id) : nullptr;
    if (!descriptor || !descriptor->factory)
        return;
    QWidget* content = descriptor->factory(stack_host_,
        registry::view_instance_id_t{member.id, registry::stable_view_instance_key_t{}});
    if (!content) {
        diag::log_tagged_critical_fmt("qt_dock_hub",
            "hub_page_factory_null hub=%s view=%s",
            registry::hub_kind_name(hub_), member.id.c_str());
        return;
    }
    QWidget* filler = stack_->widget(index);
    stack_->removeWidget(filler);
    if (filler)
        filler->deleteLater();
    stack_->insertWidget(index, content);
    member.created = true;
}

void AidaHubWidget::activate_index(int index) {
    if (index < 0 || activating_)
        return;
    activating_ = true;
    ensure_page(index);
    stack_->setCurrentIndex(index);
    activating_ = false;
    Q_EMIT subviewActivated(tab_bar_->tabData(index).toInt());
}

void AidaHubWidget::ensure_current_page() {
    if (pending_subview_ >= 0) {
        const int pending = pending_subview_;
        pending_subview_ = -1;
        activate_subview(pending);
    }
    const int index = tab_bar_->currentIndex();
    if (index >= 0 && static_cast<std::size_t>(index) < members_.size() &&
        !members_[static_cast<std::size_t>(index)].created)
        activate_index(index);
}

void AidaHubWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!pages_deferred_)
        return;
    pages_deferred_ = false;
    ensure_current_page();
}

}
