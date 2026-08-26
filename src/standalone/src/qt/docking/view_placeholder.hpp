#pragma once

#include "qt/registry/qt_view_descriptor.hpp"

#include <QWidget>

#include <string>

namespace aida::qt::docking {

class AidaViewPlaceholder : public QWidget {
    Q_OBJECT
public:
    explicit AidaViewPlaceholder(std::string view_id, QWidget* parent = nullptr);
    ~AidaViewPlaceholder() override;

    const std::string& view_id() const noexcept { return view_id_; }

private:
    std::string view_id_;
};

QWidget* placeholder_view_factory(QWidget* parent, const registry::view_instance_id_t& instance);

}
