#include "qt/network/network_hub_widget.hpp"

#include <QWidget>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <string>

#include "core/runtime/standalone_driver.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "core/session/analysis_session.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/network_pane_factory.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_chip.hpp"

namespace aida::qt::net {

NetworkHeaderWidget::NetworkHeaderWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.spacing.sm, t.panel.padding, t.spacing.sm);
    layout->setSpacing(t.spacing.sm);

    auto* titleBlock = new QWidget(this);
    auto* titleLayout = new QVBoxLayout(titleBlock);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(0);
    title_ = new QLabel("Network", titleBlock);
    title_->setProperty("aidaTone", QStringLiteral("heading"));
    subtitle_ = new QLabel(titleBlock);
    subtitle_->setProperty("aidaTone", QStringLiteral("secondary"));
    titleLayout->addWidget(title_);
    titleLayout->addWidget(subtitle_);
    layout->addWidget(titleBlock, 1);

    primary_ = new widgets::AidaButton(this);
    primary_->setKind(widgets::AidaButton::Kind::Primary);
    primary_->setControlSize(widgets::AidaButton::ControlSize::Small);
    primary_->setObjectName(QStringLiteral("aida.network.header.primary"));
    connect(primary_, &QAbstractButton::clicked, this, [this] {
        Q_EMIT primaryCaptureAction();
    });
    secondary_ = new widgets::AidaButton("Open Capture", this);
    secondary_->setKind(widgets::AidaButton::Kind::Secondary);
    secondary_->setControlSize(widgets::AidaButton::ControlSize::Small);
    secondary_->setObjectName(QStringLiteral("aida.network.header.secondary"));
    connect(secondary_, &QAbstractButton::clicked, this, [this] {
        Q_EMIT openCaptureRequested();
    });
    layout->addWidget(primary_);
    layout->addWidget(secondary_);
    relayoutForWidth();
}

void NetworkHeaderWidget::setSubtitle(const QString& subtitle) {
    subtitle_->setText(subtitle);
}

void NetworkHeaderWidget::setCaptureState(bool running, bool pending) {
    capture_running_ = running;
    capture_pending_ = pending;
    primary_->setText(running ? "Stop capture" : "Start capture");
    primary_->setEnabled(!pending);
}

void NetworkHeaderWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayoutForWidth();
}

void NetworkHeaderWidget::relayoutForWidth() {
    const int w = width();
    const bool hasTarget = analysis_session::has_active_target();
    primary_->setVisible(hasTarget && !capture_pending_ && w >= 680);
    secondary_->setVisible(w >= 840);
    subtitle_->setVisible(w >= 620);
}

NetworkStatusWidget::NetworkStatusWidget(QWidget* parent)
    : QWidget(parent) {
    const auto& t = theme::tokens();
    setFixedHeight(t.status_bar.height);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(t.status_bar.padding_x, 0, t.status_bar.padding_x, 0);
    layout->setSpacing(t.status_bar.item_gap);
    target_ = new QLabel(this);
    capture_ = new QLabel(this);
    packets_ = new QLabel(this);
    tool_ = new QLabel(this);
    for (auto* label : { target_, capture_, packets_, tool_ })
        label->setProperty("aidaTone", QStringLiteral("secondary"));
    layout->addWidget(target_);
    layout->addWidget(capture_);
    layout->addWidget(packets_);
    layout->addStretch(1);
    layout->addWidget(tool_);
    relayoutForWidth();
}

void NetworkStatusWidget::refresh() {
    const std::uint32_t attachedPid = driver_bridge::attached_pid();
    const bool hasTarget = analysis_session::has_active_target() || attachedPid != 0;
    target_->setText(QStringLiteral("Target: %1")
        .arg(attachedPid != 0 ? QString::number(attachedPid) : QStringLiteral("none")));
    set_label_tone(target_, hasTarget ? "success" : "warning");

    const bool running = network_view::g_state.cap_running.load(std::memory_order_acquire);
    const bool pending = network_view::g_state.cap_start_pending.load(std::memory_order_acquire) ||
        network_view::g_state.cap_stop_pending.load(std::memory_order_acquire);
    capture_->setText(QStringLiteral("Capture: %1")
        .arg(pending ? "transitioning" : running ? "running" : "idle"));
    set_label_tone(capture_, pending ? "info" : running ? "success" : "secondary");

    const std::size_t packetCount = network_view::capture_buffered_count();
    packets_->setText(QStringLiteral("Packets: %1").arg(static_cast<quint64>(packetCount)));
    set_label_tone(packets_, packetCount > 0 ? "info" : "secondary");
}

void NetworkStatusWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayoutForWidth();
}

void NetworkStatusWidget::relayoutForWidth() {
    const int w = width();
    packets_->setVisible(w >= 620);
    tool_->setVisible(w >= 820);
}

void NetworkStatusWidget::setToolName(const QString& name) {
    tool_->setText(QStringLiteral("Tool: %1").arg(name));
    set_label_tone(tool_, "accent");
}

NetworkHubWidget::NetworkHubWidget(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.network.hub"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    header_ = new NetworkHeaderWidget(this);
    layout->addWidget(header_);

    chipRow_ = new QWidget(this);
    auto* chipLayout = new QHBoxLayout(chipRow_);
    const auto& t = theme::tokens();
    chipLayout->setContentsMargins(t.panel.padding, t.spacing.xs, t.panel.padding, t.spacing.xs);
    chipLayout->setSpacing(t.spacing.xs);
    chipGroup_ = new QButtonGroup(this);
    chipGroup_->setExclusive(true);
    int chipIndex = 0;
    for (const auto& group : network_nav_groups()) {
        auto* chip = new widgets::AidaChip(QString::fromLatin1(group.label), chipRow_);
        chip->setCheckable(true);
        chip->setToolTip(QStringLiteral("%1\n%2").arg(group.label).arg(group.status));
        chipGroup_->addButton(chip, chipIndex);
        chipLayout->addWidget(chip);
        if (chipIndex == 0)
            chip->setChecked(true);
        ++chipIndex;
    }
    chipLayout->addStretch(1);
    connect(chipGroup_, &QButtonGroup::idClicked, this, [this](int id) {
        activateGroup(id);
    });
    layout->addWidget(chipRow_);

    tabBar_ = new QTabBar(this);
    tabBar_->setMovable(false);
    tabBar_->setTabsClosable(false);
    tabBar_->setExpanding(false);
    layout->addWidget(tabBar_);

    stackHost_ = new QWidget(this);
    stack_ = new QStackedLayout(stackHost_);
    stack_->setStackingMode(QStackedLayout::StackOne);
    layout->addWidget(stackHost_, 1);

    status_ = new NetworkStatusWidget(this);
    layout->addWidget(status_);

    const int tabCount = static_cast<int>(network_view::sub_tab_t::COUNT);
    pages_.assign(static_cast<std::size_t>(tabCount), nullptr);
    pageCreated_.assign(static_cast<std::size_t>(tabCount), false);

    connect(tabBar_, &QTabBar::currentChanged, this, [this](int index) {
        if (index < 0)
            return;
        const int tabValue = tabBar_->tabData(index).toInt();
        activateTab(static_cast<network_view::sub_tab_t>(tabValue));
    });
    connect(header_, &NetworkHeaderWidget::primaryCaptureAction, this, [] {
        const bool running = network_view::g_state.cap_running.load(std::memory_order_acquire);
        aida::ui::application_ui::execute_action(
            running ? "network.capture.stop" : "network.capture.start",
            aida::ui::action_invocation_source_t::toolbar);
    });
    connect(header_, &NetworkHeaderWidget::openCaptureRequested, this, [this] {
        activateGroup(network_nav_group_for_tab(network_view::sub_tab_t::capture));
        activateTab(network_view::sub_tab_t::capture);
    });

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(250);
    connect(pollTimer_, &QTimer::timeout, this, [this] {
        refreshHeader();
        status_->refresh();
    });

    rebuildTabBar();
    activateTab(network_view::sub_tab_t::connections);
    refreshHeader();
    status_->refresh();
    pollTimer_->start();
}

void NetworkHubWidget::activateGroup(int groupIndex) {
    if (groupIndex < 0 || groupIndex >= static_cast<int>(network_nav_groups().size()))
        return;
    if (current_group_ == groupIndex)
        return;
    current_group_ = groupIndex;
    header_->setSubtitle(QString::fromLatin1(network_nav_groups()[static_cast<std::size_t>(groupIndex)].status));
    rebuildTabBar();
    const auto& group = network_nav_groups()[static_cast<std::size_t>(groupIndex)];
    if (!group.tabs.empty())
        activateTab(group.tabs.front());
}

void NetworkHubWidget::activateTab(network_view::sub_tab_t tab) {
    if (activating_)
        return;
    activating_ = true;
    QWidget* page = ensurePage(tab);
    if (page) {
        stack_->setCurrentWidget(page);
        current_tab_ = tab;
        current_pane_ = qobject_cast<NetworkPaneBase*>(page);
        status_->setToolName(QString::fromLatin1(network_tab_name(tab)));
        status_->refresh();
        const int groupIndex = network_nav_group_for_tab(tab);
        if (groupIndex != current_group_) {
            current_group_ = groupIndex;
            header_->setSubtitle(QString::fromLatin1(
                network_nav_groups()[static_cast<std::size_t>(groupIndex)].status));
            rebuildTabBar();
        }
        for (int i = 0; i < tabBar_->count(); ++i) {
            if (tabBar_->tabData(i).toInt() == static_cast<int>(tab)) {
                if (tabBar_->currentIndex() != i)
                    tabBar_->setCurrentIndex(i);
                break;
            }
        }
    }
    activating_ = false;
}

void NetworkHubWidget::setTab(network_view::sub_tab_t tab) {
    const int groupIndex = network_nav_group_for_tab(tab);
    if (groupIndex != current_group_)
        activateGroup(groupIndex);
    activateTab(tab);
}

QWidget* NetworkHubWidget::ensurePage(network_view::sub_tab_t tab) {
    const int index = static_cast<int>(tab);
    if (index < 0 || index >= static_cast<int>(pages_.size()))
        return nullptr;
    if (pageCreated_[static_cast<std::size_t>(index)])
        return pages_[static_cast<std::size_t>(index)];
    QWidget* content = createNetworkPane(tab, stackHost_);
    if (!content) {
        content = new QWidget(stackHost_);
        auto* emptyLayout = new QVBoxLayout(content);
        auto* label = new QLabel(QStringLiteral("%1 is not ported yet.")
            .arg(network_tab_name(tab)), content);
        label->setProperty("aidaTone", QStringLiteral("secondary"));
        label->setAlignment(Qt::AlignCenter);
        emptyLayout->addWidget(label);
    }
    pages_[static_cast<std::size_t>(index)] = content;
    pageCreated_[static_cast<std::size_t>(index)] = true;
    stack_->addWidget(content);
    return content;
}

void NetworkHubWidget::rebuildTabBar() {
    const QSignalBlocker blocker(tabBar_);
    while (tabBar_->count() > 0)
        tabBar_->removeTab(0);
    const auto& group = network_nav_groups()[static_cast<std::size_t>(current_group_)];
    for (const auto tab : group.tabs) {
        const int index = tabBar_->addTab(QString::fromLatin1(network_tab_name(tab)));
        tabBar_->setTabData(index, static_cast<int>(tab));
        tabBar_->setTabToolTip(index, QString::fromLatin1(network_tab_name(tab)));
    }
    for (int i = 0; i < tabBar_->count(); ++i) {
        if (tabBar_->tabData(i).toInt() == static_cast<int>(current_tab_)) {
            tabBar_->setCurrentIndex(i);
            break;
        }
    }
}

void NetworkHubWidget::refreshHeader() {
    const bool running = network_view::g_state.cap_running.load(std::memory_order_acquire);
    const bool pending = network_view::g_state.cap_start_pending.load(std::memory_order_acquire) ||
        network_view::g_state.cap_stop_pending.load(std::memory_order_acquire);
    header_->setCaptureState(running, pending);
}

namespace {

hub_content_factory_t g_hub_content_factory;

}

void register_network_hub_content_factory(hub_content_factory_t factory) {
    g_hub_content_factory = std::move(factory);
}

QWidget* create_network_hub_content(QWidget* parent) {
    return g_hub_content_factory ? g_hub_content_factory(parent) : nullptr;
}

}
