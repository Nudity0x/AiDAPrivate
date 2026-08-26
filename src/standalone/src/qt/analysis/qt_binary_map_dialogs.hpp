#pragma once

#include <QDialog>

#include <cstdint>
#include <memory>

#include "qt/analysis/qt_binary_map_types.hpp"

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace aida::qt::analysis {

// "Change Protection" modal (07 sec. 6.6, DRIVER WRITE safety-relevant). open() +
// modal, never exec() (S7). While open the dialog revalidates the retained
// binding on a 250 ms timer; Apply is disabled when the binding stales (this
// replaces the per-frame reviewed_region_current check).
class QtChangeProtectionDialog : public QDialog {
    Q_OBJECT
public:
    QtChangeProtectionDialog(
        std::shared_ptr<QtBinaryMapViewState> state,
        qt_binary_map_live_target_binding_t binding, std::uint64_t address,
        std::uint64_t size, std::uint32_t current_protect, QWidget* parent = nullptr);

private Q_SLOTS:
    void revalidate();
    void apply();

private:
    std::shared_ptr<QtBinaryMapViewState> state_;
    qt_binary_map_live_target_binding_t binding_;
    std::uint64_t address_ = 0;
    std::uint64_t size_ = 0;
    std::uint32_t current_protect_ = 0;
    QComboBox* combo_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* apply_button_ = nullptr;
    QTimer* timer_ = nullptr;
};

}
