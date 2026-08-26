#include "qt/docking/view_placeholder.hpp"

#include <QVBoxLayout>

#include <utility>

#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::docking {

AidaViewPlaceholder::AidaViewPlaceholder(std::string view_id, QWidget* parent)
    : QWidget(parent), view_id_(std::move(view_id)) {
    setObjectName(QStringLiteral("aida.view_placeholder"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* state = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("This view is not available"),
        QStringLiteral("View id: %1").arg(QString::fromStdString(view_id_)), this);
    layout->addStretch(1);
    layout->addWidget(state);
    layout->addStretch(1);
}

AidaViewPlaceholder::~AidaViewPlaceholder() = default;

QWidget* placeholder_view_factory(QWidget* parent, const registry::view_instance_id_t& instance) {
    return new AidaViewPlaceholder(instance.view.value(), parent);
}

}
