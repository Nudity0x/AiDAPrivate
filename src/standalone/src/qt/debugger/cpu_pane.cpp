#include "qt/debugger/cpu_pane.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QResizeEvent>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "helpers/diag_log.hpp"

#include "core/debugger/debugger_engine.hpp"
#include "core/debugger/debugger_view.hpp"
#include "core/runtime/standalone_driver.hpp"

#include "qt/debugger/debugger_action_bridge.hpp"
#include "qt/debugger/debugger_models.hpp"
#include "qt/debugger/debugger_run_toolbar.hpp"
#include "qt/debugger/debugger_selection_bridge.hpp"
#include "qt/debugger/debugger_status_strip.hpp"
#include "qt/debugger/dialogs/register_edit_dialog.hpp"
#include "qt/debugger/disasm_slice_widget.hpp"
#include "qt/debugger/rflags_chips_widget.hpp"
#include "qt/debugger/stack_quad_widget.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/widgets/aida_pill.hpp"

namespace aida::qt::debugger {

namespace {

// Register change-flash overlay: warning wash at alpha flash*0.55 + accent
// stripe at flash*0.85 over the value cell (ports draw_cpu_reg_row).
class RegisterFlashDelegate : public QStyledItemDelegate {
public:
    explicit RegisterFlashDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        const qreal flash = index.data(FlashRole).toReal();
        if (flash > 0.001) {
            const auto& t = theme::tokens();
            painter->save();
            painter->fillRect(option.rect,
                widgets::with_alpha(t.warning, flash * 0.55));
            painter->fillRect(
                QRectF(option.rect.left(), option.rect.top(), 3,
                    option.rect.height()),
                widgets::with_alpha(t.warning, flash * 0.85));
            painter->restore();
        }
    }
};

}

CpuPaneWidget::CpuPaneWidget(Surface surface, QWidget* parent)
    : DebuggerPaneBase(parent), surface_(surface) {
    setObjectName(surface == Surface::integrated
        ? QStringLiteral("aida.view.debug.cpu")
        : surface == Surface::registers_only
            ? QStringLiteral("aida.view.debug.registers")
            : QStringLiteral("aida.view.debug.stack"));
    setOwnerViewId(surface == Surface::integrated ? "view.debug.cpu"
        : surface == Surface::registers_only ? "view.debug.registers"
            : "view.debug.stack");
    setEmptyTargetText(QStringLiteral("No target attached"),
        QStringLiteral("Attach or launch a target to begin a live debugging session."));
    setEmptyContentText(QStringLiteral("Waiting for CPU state"),
        QStringLiteral(
            "Registers, disassembly, and stack appear once the engine "
            "publishes the first snapshot for this target."));
    setLoadingText(QStringLiteral("Reading CPU state"),
        QStringLiteral(
            "The engine is capturing registers, disassembly, and stack."));

    if (surface_ == Surface::integrated) {
        run_toolbar_ = new DebuggerRunToolBar(this);
        auto* run_bar = new QWidget(this);
        auto* run_bar_layout = new QHBoxLayout(run_bar);
        run_bar_layout->setContentsMargins(0, 0, 0, 0);
        run_bar_layout->setSpacing(theme::tokens().toolbar.group_gap);
        run_bar_layout->addWidget(run_toolbar_, 1);
        run_bar_layout->addWidget(run_toolbar_->statusPill(), 0);
        setToolBar(run_bar);
    }

    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);

    root_splitter_ = new QSplitter(Qt::Horizontal, body);
    root_splitter_->setChildrenCollapsible(false);

    auto* left = new QWidget(root_splitter_);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(0);

    registers_model_ = new RegistersModel(this);
    registers_view_ = new QTableView(left);
    registers_view_->setModel(registers_model_);
    registers_view_->setObjectName(surface_ == Surface::registers_only
        ? QStringLiteral("aida.view.debug.registers.table")
        : QStringLiteral("aida.view.debug.cpu.registers"));
    registers_view_->verticalHeader()->setVisible(false);
    registers_view_->verticalHeader()->setSectionResizeMode(
        QHeaderView::Fixed);
    registers_view_->verticalHeader()->setDefaultSectionSize(
        theme::tokens().table.row_h);
    registers_view_->horizontalHeader()->setSectionResizeMode(0,
        QHeaderView::Interactive);
    registers_view_->setColumnWidth(0, registers_model_->columns()[0].width);
    registers_view_->horizontalHeader()->setStretchLastSection(true);
    registers_view_->setShowGrid(false);
    registers_view_->setAlternatingRowColors(true);
    registers_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    registers_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    registers_view_->setItemDelegateForColumn(1,
        new RegisterFlashDelegate(registers_view_));
    registers_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    left_layout->addWidget(registers_view_, 1);

    flags_widget_ = new RflagsChipsWidget(left);
    flags_widget_->setObjectName(QStringLiteral("aida.view.debug.cpu.rflags"));
    left_layout->addWidget(flags_widget_, 0);

    root_splitter_->addWidget(left);

    if (surface_ != Surface::registers_only) {
        right_splitter_ = new QSplitter(Qt::Vertical, root_splitter_);
        right_splitter_->setChildrenCollapsible(false);
        if (surface_ == Surface::integrated) {
            disasm_widget_ = new DisasmSliceWidget(right_splitter_);
            disasm_widget_->setObjectName(
                QStringLiteral("aida.view.debug.cpu.disasm"));
            right_splitter_->addWidget(disasm_widget_);
        }
        stack_widget_ = new StackQuadWidget(right_splitter_);
        stack_widget_->setObjectName(surface_ == Surface::stack_only
            ? QStringLiteral("aida.view.debug.stack.quad")
            : QStringLiteral("aida.view.debug.cpu.stack"));
        right_splitter_->addWidget(stack_widget_);
        if (disasm_widget_)
            right_splitter_->setStretchFactor(0, 55);
        right_splitter_->setStretchFactor(disasm_widget_ ? 1 : 0, 45);
        root_splitter_->addWidget(right_splitter_);
    }
    if (surface_ == Surface::integrated) {
        root_splitter_->setStretchFactor(0, 40);
        root_splitter_->setStretchFactor(1, 60);
    }

    body_layout->addWidget(root_splitter_, 1);

    if (surface_ == Surface::integrated) {
        status_strip_ = new DebuggerStatusStrip(body);
        status_strip_->setPanelLabel(QStringLiteral("CPU"));
        body_layout->addWidget(status_strip_, 0);
    }

    setContent(body);

    flags_widget_->setVisible(surface_ != Surface::stack_only);

    registers_timer_ = new QTimer(this);
    registers_timer_->setInterval(120);
    registers_timer_->setTimerType(Qt::CoarseTimer);
    connect(registers_timer_, &QTimer::timeout, this,
        &CpuPaneWidget::refreshRegisters);

    disasm_timer_ = new QTimer(this);
    disasm_timer_->setInterval(220);
    disasm_timer_->setTimerType(Qt::CoarseTimer);
    connect(disasm_timer_, &QTimer::timeout, this, [this] {
        if (disasm_widget_)
            disasm_widget_->tick();
    });

    stack_timer_ = new QTimer(this);
    stack_timer_->setInterval(220);
    stack_timer_->setTimerType(Qt::CoarseTimer);
    connect(stack_timer_, &QTimer::timeout, this, [this] {
        if (stack_widget_)
            stack_widget_->tick();
    });

    connect(registers_view_->selectionModel(), &QItemSelectionModel::currentChanged,
        this, [this](const QModelIndex& current, const QModelIndex&) {
            selection_bridge::publish_rows(*registers_model_,
                registers_view_->selectionModel(), current);
        });
    connect(registers_view_, &QTableView::customContextMenuRequested, this,
        [this](const QPoint& pos) {
            const QModelIndex index = registers_view_->indexAt(pos);
            if (!index.isValid())
                return;
            selection_bridge::publish_single(*registers_model_, index.row());
            DebuggerActionBridge::instance().showEntityMenu(
                registers_model_->contextForRow(index.row()),
                aida::ui::context_menu_open_origin_t::pointer,
                registers_view_->viewport()->mapToGlobal(pos), registers_view_);
        });
    registerContextMenuTable(registers_view_, registers_model_);
    connect(registers_view_, &QAbstractItemView::activated, this,
        [this](const QModelIndex& index) {
            if (RegistersModel::registerEditable(index.row()))
                openRegisterEditor(index.row());
        });
    connect(flags_widget_, &RflagsChipsWidget::flagToggleRequested, this,
        &CpuPaneWidget::openRflagsEditor);
    if (disasm_widget_) {
        connect(disasm_widget_, &DisasmSliceWidget::rowSelected, this,
            [this](int row) {
                selection_bridge::publish_context(
                    disasm_widget_->contextForRow(row));
            });
        connect(disasm_widget_, &DisasmSliceWidget::contextRowRequested, this,
            [this](int row, const QPoint& globalPos) {
                const auto context = disasm_widget_->contextForRow(row);
                selection_bridge::publish_context(context);
                DebuggerActionBridge::instance().showEntityMenu(context,
                    aida::ui::context_menu_open_origin_t::pointer, globalPos,
                    disasm_widget_);
            });
        connect(disasm_widget_, &DisasmSliceWidget::branchFollowRequested, this,
            [](std::uint64_t target) {
                diag::log_tagged_fmt("cpu_view",
                    "disasm_dclick_follow target=0x%llx",
                    static_cast<unsigned long long>(target));
                debugger_view::jump_to_disasm(target);
            });
    }
    if (stack_widget_) {
        connect(stack_widget_, &StackQuadWidget::rowSelected, this,
            [this](int row) {
                selection_bridge::publish_context(
                    stack_widget_->contextForRow(row));
            });
        connect(stack_widget_, &StackQuadWidget::contextRowRequested, this,
            [this](int row, const QPoint& globalPos) {
                const auto context = stack_widget_->contextForRow(row);
                selection_bridge::publish_context(context);
                DebuggerActionBridge::instance().showEntityMenu(context,
                    aida::ui::context_menu_open_origin_t::pointer, globalPos,
                    stack_widget_);
            });
    }
}

bool CpuPaneWidget::hasTargetContent() const {
    return driver_bridge::attached_pid() != 0;
}

bool CpuPaneWidget::hasContentRows() const {
    return registers_seen_;
}

bool CpuPaneWidget::isContentLoading() const {
    return debugger_engine::g_state.refresh_in_flight.load(
        std::memory_order_acquire);
}

void CpuPaneWidget::onSessionStateChanged(int status, quint32 pid,
                                          quint64 stopGeneration) {
    if (pid != last_seen_pid_) {
        last_seen_pid_ = pid;
        registers_seen_ = false;
    }
    DebuggerPaneBase::onSessionStateChanged(status, pid, stopGeneration);
}

void CpuPaneWidget::onShown() {
    registers_timer_->start();
    if (disasm_widget_)
        disasm_timer_->start();
    if (stack_widget_)
        stack_timer_->start();
    refreshRegisters();
}

void CpuPaneWidget::onHidden() {
    registers_timer_->stop();
    disasm_timer_->stop();
    stack_timer_->stop();
}

void CpuPaneWidget::onSessionTick() {
    updateOverlayState();
}

void CpuPaneWidget::resizeEvent(QResizeEvent* event) {
    updateNarrowGuard();
    DebuggerPaneBase::resizeEvent(event);
}

void CpuPaneWidget::updateNarrowGuard() {
    const auto& t = theme::tokens();
    if (flags_widget_) {
        const bool show_flags = surface_ == Surface::integrated ||
            (height() >= flags_widget_->minimumSizeHint().height() +
                    10 * t.table.row_h &&
                registers_view_ && registers_view_->width() >=
                    flags_widget_->sizeHint().width());
        flags_widget_->setVisible(surface_ != Surface::stack_only &&
            show_flags);
    }
    if (surface_ != Surface::integrated)
        return;
    const auto& grid = theme::fonts::monoGrid();
    const qreal cell_w = grid.valid && grid.cell_w > 0.0
        ? grid.cell_w
        : QFontMetricsF(theme::fonts::codeRegular()).horizontalAdvance(u'0');
    const int registers_min = registers_model_->columns()[0].width +
        qRound(16.0 * cell_w) + t.spacing.md;
    const int disasm_min = qRound((17.0 + 26.0 + 9.0 + 5.0) * cell_w);
    const int min_integrated = registers_min + disasm_min +
        2 * t.splitter.thickness;
    const bool narrow = width() < min_integrated;
    if (!narrow_notice_) {
        auto* notice = new widgets::AidaNotice(
            QStringLiteral("Debugger pane too narrow"),
            QStringLiteral("Widen the debugger pane to view CPU registers, "
                "disassembly, and stack side-by-side."),
            widgets::AidaSemantic::Info, this);
        notice->setObjectName(QStringLiteral("aida.view.debug.cpu.narrow"));
        narrow_notice_ = notice;
    }
    narrow_notice_->setGeometry(rect());
    narrow_notice_->setVisible(narrow);
    if (narrow)
        narrow_notice_->raise();
    if (narrow && !narrow_logged_) {
        narrow_logged_ = true;
        diag::log_tagged_fmt("responsive",
            "debugger_view cpu_pane too_narrow w=%d min=%d overlay_shown=1",
            width(), min_integrated);
    } else if (!narrow && narrow_logged_) {
        narrow_logged_ = false;
    }
}

void CpuPaneWidget::refreshRegisters() {
    if (!paneVisible_)
        return;
    debugger_engine::request_refresh(120);
    const auto registers = debugger_engine::cached_registers();
    const quint32 attached = driver_bridge::attached_pid();
    registers_model_->applyRegisters(registers, attached,
        debugger_engine::g_state.active_tid);
    if (attached != 0 && !registers_seen_) {
        registers_seen_ = true;
        updateOverlayState();
    }
    flags_widget_->setRflags(registers.rflags);
    if (disasm_widget_)
        disasm_widget_->setRip(registers.rip);
    if (stack_widget_)
        stack_widget_->setRsp(registers.rsp);
}

void CpuPaneWidget::openRegisterEditor(int row) {
    const auto context = registers_model_->contextForRow(row);
    RegisterEditDialog::openFor(context, RegistersModel::registerName(row),
        RegistersModel::registerValue(registers_model_->registers(), row),
        this);
}

void CpuPaneWidget::openRflagsEditor(const QString& flagName,
                                     std::uint64_t current,
                                     std::uint64_t toggled) {
    (void)current;
    const auto context = debugger_interaction::capture(
        debugger_interaction::kind_t::register_value, 0, current, 17, 0, 0,
        "RFLAGS", flagName.toStdString());
    RegisterEditDialog::openFor(context, "RFLAGS", toggled, this);
}

}
