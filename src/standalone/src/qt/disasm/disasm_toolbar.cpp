#include "qt/disasm/disasm_toolbar.hpp"

#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/theme/aida_tokens.hpp"

#include "core/ui/application_ui_runtime.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QToolButton>

namespace aida::qt::disasm {

namespace {

QToolButton* make_tool_button(QWidget* parent, const QString& id, const QString& label,
                              const QString& tooltip)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("aida.disasm.toolbar.") + id);
    button->setText(label);
    button->setToolTip(tooltip);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

struct analysis_action_t {
    const char* id;
    const char* label;
};

constexpr analysis_action_t k_analysis_actions[] = {
    {"analysis.modify.rename", "Rename"},
    {"analysis.navigate.xrefs", "Xrefs"},
    {"analysis.decompile_or_focus_pseudocode", "Decompile"},
    {"analysis.navigate.graph", "Graph"},
    {"analysis.modify.patch", "Patch"},
};

QString action_tooltip(const aida::ui::application_ui::action_presentation_t& presentation)
{
    QString tooltip = QString::fromStdString(presentation.description);
    if (!presentation.shortcut.empty())
        tooltip += QStringLiteral(" (") +
            QString::fromStdString(presentation.shortcut) + QStringLiteral(")");
    if (!presentation.enabled && !presentation.disabled_reason.empty())
        tooltip += QStringLiteral("\n") +
            QString::fromStdString(presentation.disabled_reason);
    return tooltip;
}

}

DisasmToolbar::DisasmToolbar(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("aida.disasm.toolbar"));
    setFrameShape(QFrame::NoFrame);
    setProperty("aidaRole", QStringLiteral("toolbar"));
    layout_ = new QHBoxLayout(this);
    const auto& t = theme::tokens();
    layout_->setContentsMargins(t.toolbar.padding_x, t.toolbar.padding_y,
        t.toolbar.padding_x, t.toolbar.padding_y);
    layout_->setSpacing(t.toolbar.group_gap);

    back_ = make_tool_button(this, QStringLiteral("back"), QStringLiteral("Back"),
        QStringLiteral("Navigate to the previous location"));
    forward_ = make_tool_button(this, QStringLiteral("forward"), QStringLiteral("Forward"),
        QStringLiteral("Navigate to the next location"));
    goto_ = make_tool_button(this, QStringLiteral("goto"), QStringLiteral("Go to"),
        QStringLiteral("Go to an address or symbol"));
    rebase_ = make_tool_button(this, QStringLiteral("rebase"), QStringLiteral("Rebase"),
        QStringLiteral("Set the listing image base"));
    layout_->addWidget(back_);
    layout_->addWidget(forward_);
    layout_->addWidget(goto_);
    layout_->addWidget(rebase_);

    connect(back_, &QToolButton::clicked, this, [this] {
        Q_EMIT actionInvoked(QStringLiteral("analysis.navigate.back"));
    });
    connect(forward_, &QToolButton::clicked, this, [this] {
        Q_EMIT actionInvoked(QStringLiteral("analysis.navigate.forward"));
    });
    connect(goto_, &QToolButton::clicked, this, [this] {
        Q_EMIT actionInvoked(QStringLiteral("analysis.navigate.goto"));
    });
    connect(rebase_, &QToolButton::clicked, this, [this] {
        Q_EMIT actionInvoked(QStringLiteral("analysis.modify.rebase"));
    });

    bytes_ = new QCheckBox(QStringLiteral("Bytes"), this);
    bytes_->setObjectName(QStringLiteral("aida.disasm.toolbar.bytes"));
    bytes_->setToolTip(QStringLiteral("Show the encoded instruction bytes column"));
    layout_->addWidget(bytes_);
    connect(bytes_, &QCheckBox::toggled, this, [this](bool checked) {
        Q_EMIT showBytesChanged(checked);
    });

    format_ = new QComboBox(this);
    format_->setObjectName(QStringLiteral("aida.disasm.toolbar.addr_format"));
    format_->addItem(QStringLiteral("VA"));
    format_->addItem(QStringLiteral("RVA"));
    format_->addItem(QStringLiteral("File"));
    format_->setMinimumContentsLength(4);
    format_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    format_->setToolTip(QStringLiteral(
        "Address display format: virtual address, relative virtual address, or file offset"));
    layout_->addWidget(format_);
    connect(format_, &QComboBox::currentIndexChanged, this, [this](int index) {
        Q_EMIT addrFormatChanged(index);
    });

    section_ = new QComboBox(this);
    section_->setObjectName(QStringLiteral("aida.disasm.toolbar.section"));
    section_->setMinimumContentsLength(18);
    section_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    section_->setToolTip(QStringLiteral("Restrict the listing to one executable section"));
    layout_->addWidget(section_);
    connect(section_, &QComboBox::currentIndexChanged, this, [this](int index) {
        int true_index = -1;
        if (index > 0 && static_cast<std::size_t>(index - 1) < section_true_indices_.size())
            true_index = section_true_indices_[static_cast<std::size_t>(index - 1)];
        Q_EMIT sectionChanged(true_index);
    });

    for (const auto& action : k_analysis_actions) {
        auto* button = make_tool_button(this,
            QString::fromLatin1(action.id).replace(u'.', u'_'),
            QString::fromLatin1(action.label), QString());
        layout_->addWidget(button);
        analysis_buttons_.push_back(button);
        const QString id = QString::fromLatin1(action.id);
        connect(button, &QToolButton::clicked, this, [this, id] {
            Q_EMIT actionInvoked(id);
        });
    }
    more_ = make_tool_button(this, QStringLiteral("more"), QStringLiteral("More"),
        QStringLiteral("Analysis actions that do not fit the toolbar"));
    more_->setPopupMode(QToolButton::InstantPopup);
    more_menu_ = new QMenu(more_);
    more_menu_->setToolTipsVisible(true);
    more_->setMenu(more_menu_);
    more_->setVisible(false);
    layout_->addWidget(more_);
    layout_->addStretch(1);
    refresh_actions();
}

void DisasmToolbar::resizeEvent(QResizeEvent* event)
{
    QFrame::resizeEvent(event);
    updateOverflow();
}

void DisasmToolbar::updateOverflow()
{
    const auto margins = layout_->contentsMargins();
    int required = margins.left() + margins.right();
    int visible_count = 0;
    const auto add = [&](const QWidget* widget, bool include) {
        if (!widget || !include)
            return;
        required += widget->sizeHint().width();
        ++visible_count;
    };
    add(back_, true);
    add(forward_, true);
    add(goto_, true);
    add(rebase_, rebase_->isVisibleTo(this));
    add(bytes_, true);
    add(format_, true);
    add(section_, !section_true_indices_.empty());
    for (const auto* button : analysis_buttons_)
        add(button, true);
    add(more_, compact_);
    if (visible_count > 1)
        required += layout_->spacing() * (visible_count - 1);
    const bool want_compact = width() < required;
    if (want_compact != compact_)
        setCompact(want_compact);
}

void DisasmToolbar::setCompact(bool compact)
{
    compact_ = compact;
    for (auto* button : analysis_buttons_)
        button->setVisible(!compact);
    more_->setVisible(compact);
    section_->setMinimumContentsLength(compact ? 6 : 18);
    if (compact)
        rebuildOverflowMenu();
}

void DisasmToolbar::rebuildOverflowMenu()
{
    if (!more_menu_)
        return;
    more_menu_->clear();
    for (const auto& entry : k_analysis_actions) {
        const auto presentation = aida::ui::application_ui::present_action(entry.id);
        const QString label = presentation.label.empty()
            ? QString::fromLatin1(entry.label)
            : QString::fromStdString(presentation.label);
        auto* action = more_menu_->addAction(label);
        action->setObjectName(QStringLiteral("aida.") + QString::fromLatin1(entry.id));
        action->setEnabled(presentation.enabled);
        const QString tooltip = action_tooltip(presentation);
        if (!tooltip.isEmpty())
            action->setToolTip(tooltip);
        const QString id = QString::fromLatin1(entry.id);
        connect(action, &QAction::triggered, this, [this, id] {
            Q_EMIT actionInvoked(id);
        });
    }
}

void DisasmToolbar::set_can_rebase(bool can)
{
    rebase_->setVisible(can);
}

void DisasmToolbar::set_sections(const std::vector<std::string>& names,
                                 const std::vector<int>& true_indices, int active)
{
    QSignalBlocker blocker(section_);
    if (names != section_names_ || true_indices != section_true_indices_) {
        section_names_ = names;
        section_true_indices_ = true_indices;
        section_->clear();
        section_->addItem(QStringLiteral("All executable ranges"));
        for (const auto& name : names)
            section_->addItem(QString::fromStdString(name));
    }
    int current = 0;
    for (std::size_t index = 0; index < true_indices.size(); ++index) {
        if (true_indices[index] == active) {
            current = static_cast<int>(index) + 1;
            break;
        }
    }
    if (section_->currentIndex() != current)
        section_->setCurrentIndex(current);
    const QString tooltip = current > 0 && current <= section_->count()
        ? QStringLiteral("Listing restricted to section %1").arg(section_->itemText(current))
        : QStringLiteral("Restrict the listing to one executable section");
    if (section_->toolTip() != tooltip)
        section_->setToolTip(tooltip);
    section_->setVisible(!names.empty());
}

void DisasmToolbar::set_addr_format(disasm_view::addr_format_t format)
{
    QSignalBlocker blocker(format_);
    format_->setCurrentIndex(static_cast<int>(format));
}

void DisasmToolbar::set_show_bytes(bool show)
{
    QSignalBlocker blocker(bytes_);
    bytes_->setChecked(show);
}

void DisasmToolbar::refresh_actions()
{
    const auto apply = [](QToolButton* button, const char* id) {
        const auto presentation = aida::ui::application_ui::present_action(id);
        button->setEnabled(presentation.enabled);
        button->setToolTip(action_tooltip(presentation));
        if (!presentation.label.empty())
            button->setText(QString::fromStdString(presentation.label));
    };
    apply(back_, "analysis.navigate.back");
    apply(forward_, "analysis.navigate.forward");
    apply(goto_, "analysis.navigate.goto");
    apply(rebase_, "analysis.modify.rebase");
    for (std::size_t index = 0; index < analysis_buttons_.size(); ++index)
        apply(analysis_buttons_[index], k_analysis_actions[index].id);
    if (compact_)
        rebuildOverflowMenu();
    updateOverflow();
}

DisasmGotoStrip::DisasmGotoStrip(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("aida.disasm.goto_strip"));
    setFrameShape(QFrame::NoFrame);
    setProperty("aidaRole", QStringLiteral("toolbar"));
    auto* layout = new QHBoxLayout(this);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.toolbar.padding_x, t.toolbar.padding_y,
        t.toolbar.padding_x, t.toolbar.padding_y);
    layout->setSpacing(t.toolbar.group_gap);
    edit_ = new QLineEdit(this);
    edit_->setObjectName(QStringLiteral("aida.disasm.goto_strip.input"));
    edit_->setPlaceholderText(QStringLiteral("VA, RVA, or symbol"));
    edit_->setToolTip(QStringLiteral(
        "Enter a virtual address, RVA, file offset, or symbol name, then press Enter"));
    edit_->installEventFilter(this);
    layout->addWidget(edit_, 1);
    go_ = make_tool_button(this, QStringLiteral("submit"), QStringLiteral("Go"),
        QStringLiteral("Navigate to the resolved address"));
    go_->setObjectName(QStringLiteral("aida.disasm.goto_strip.submit"));
    close_ = make_tool_button(this, QStringLiteral("close"), QStringLiteral("Close"),
        QStringLiteral("Close address search"));
    close_->setObjectName(QStringLiteral("aida.disasm.goto_strip.close"));
    layout->addWidget(go_);
    layout->addWidget(close_);
    connect(edit_, &QLineEdit::returnPressed, this, [this] {
        Q_EMIT submitted(edit_->text());
    });
    connect(go_, &QToolButton::clicked, this, [this] {
        Q_EMIT submitted(edit_->text());
    });
    connect(close_, &QToolButton::clicked, this, [this] {
        hide_strip();
        Q_EMIT closed();
    });
    hide();
}

void DisasmGotoStrip::show_strip()
{
    strip_visible_ = true;
    show();
    edit_->setFocus(Qt::ShortcutFocusReason);
    edit_->selectAll();
}

void DisasmGotoStrip::hide_strip()
{
    strip_visible_ = false;
    hide();
}

bool DisasmGotoStrip::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == edit_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            hide_strip();
            Q_EMIT closed();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

}
