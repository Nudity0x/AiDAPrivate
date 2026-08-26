#pragma once

#include "core/disasm/disasm_view.hpp"

#include <QFrame>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLineEdit;
class QMenu;
class QToolButton;

namespace aida::qt::disasm {

class DisasmToolbar : public QFrame {
    Q_OBJECT
public:
    explicit DisasmToolbar(QWidget* parent = nullptr);

    void set_can_rebase(bool can);
    void set_sections(const std::vector<std::string>& names,
                      const std::vector<int>& true_indices, int active);
    void set_addr_format(disasm_view::addr_format_t format);
    void set_show_bytes(bool show);
    void refresh_actions();

Q_SIGNALS:
    void actionInvoked(QString action_id);
    void showBytesChanged(bool show);
    void addrFormatChanged(int format);
    void sectionChanged(int section);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateOverflow();
    void setCompact(bool compact);
    void rebuildOverflowMenu();

    QHBoxLayout* layout_ = nullptr;
    QToolButton* back_ = nullptr;
    QToolButton* forward_ = nullptr;
    QToolButton* goto_ = nullptr;
    QToolButton* rebase_ = nullptr;
    QCheckBox* bytes_ = nullptr;
    QComboBox* format_ = nullptr;
    QComboBox* section_ = nullptr;
    QToolButton* more_ = nullptr;
    QMenu* more_menu_ = nullptr;
    bool compact_ = false;
    std::vector<std::string> section_names_;
    std::vector<int> section_true_indices_;
    std::vector<QToolButton*> analysis_buttons_;
};

class DisasmGotoStrip : public QFrame {
    Q_OBJECT
public:
    explicit DisasmGotoStrip(QWidget* parent = nullptr);

    void show_strip();
    void hide_strip();
    bool strip_visible() const noexcept { return strip_visible_; }

Q_SIGNALS:
    void submitted(QString text);
    void closed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLineEdit* edit_ = nullptr;
    QToolButton* go_ = nullptr;
    QToolButton* close_ = nullptr;
    bool strip_visible_ = false;
};

}
