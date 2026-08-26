#include "qt/debugger/debugger_models.hpp"

#include <QElapsedTimer>
#include <QItemSelectionModel>
#include <QPainter>
#include <QTableView>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/debugger/debugger_view.hpp"
#include "core/debugger/memory_map_view.hpp"
#include "core/debugger/seh_view.hpp"

#include "qt/theme/aida_tokens.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::debugger {

namespace {

QColor token_color(const char* token) {
    const auto& t = theme::tokens();
    const QLatin1StringView tokenView(token);
    if (tokenView == "error") return t.error;
    if (tokenView == "warning") return t.warning;
    if (tokenView == "success") return t.success;
    if (tokenView == "info") return t.info;
    if (tokenView == "accent") return t.accent;
    if (tokenView == "dim") return t.text_dim;
    return t.text_secondary;
}

QString hex64(std::uint64_t value) {
    return QString::asprintf("%016llX", static_cast<unsigned long long>(value));
}

QString hex0x64(std::uint64_t value) {
    return QStringLiteral("0x") + hex64(value);
}

} // namespace

StatePillDelegate::StatePillDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {
}

void StatePillDelegate::paint(QPainter* painter,
                              const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    QStyleOptionViewItem base(option);
    initStyleOption(&base, index);
    painter->save();
    painter->setClipRect(base.rect, Qt::IntersectClip);
    if (base.state & QStyle::State_Selected) {
        painter->fillRect(base.rect, base.palette.highlight());
    }
    const QString token = index.data(StateTokenRole).toString();
    const QString kind = index.data(StateKindRole).toString();
    const auto& t = theme::tokens();
    QColor color = t.text_secondary;
    if (kind == u"success") color = t.success;
    else if (kind == u"warning") color = t.warning;
    else if (kind == u"error") color = t.error;
    else if (kind == u"info") color = t.info;
    else if (kind == u"accent") color = t.accent;
    else if (kind == u"dim") color = t.text_dim;
    const QFont font = theme::fonts::caption();
    painter->setFont(font);
    const QFontMetricsF fm(font);
    const qreal text_w = fm.horizontalAdvance(token);
    const qreal dot_d = t.status_bar.dot;
    const qreal pill_h = fm.height() + 2.0 * t.spacing.xxs;
    const qreal pill_w = 2.0 * t.spacing.sm + dot_d + t.spacing.xs + text_w;
    const qreal pill_x = base.rect.left() + t.spacing.xs;
    const qreal pill_y = base.rect.top() +
        (base.rect.height() - pill_h) * 0.5;
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(widgets::with_alpha(color, 0.22));
    painter->drawRoundedRect(QRectF(pill_x, pill_y, pill_w, pill_h),
        pill_h * 0.5, pill_h * 0.5);
    painter->setPen(QPen(widgets::with_alpha(color, 0.55), 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(QRectF(pill_x, pill_y, pill_w, pill_h),
        pill_h * 0.5, pill_h * 0.5);
    const qreal dot_cx = pill_x + t.spacing.sm + dot_d * 0.5;
    painter->setBrush(widgets::with_alpha(color, 0.55));
    painter->drawEllipse(QPointF(dot_cx, pill_y + pill_h * 0.5),
        dot_d * 0.5, dot_d * 0.5);
    painter->setPen(color);
    painter->drawText(QPointF(dot_cx + dot_d * 0.5 + t.spacing.xs,
            pill_y + widgets::text_baseline_centered(
                QRectF(0, pill_y, 1, pill_h), fm)),
        token);
    const qreal flash = index.data(FlashRole).toReal();
    if (flash > 0.001) {
        painter->setRenderHint(QPainter::Antialiasing, false);
        painter->fillRect(base.rect,
            widgets::with_alpha(t.warning, flash * 0.55));
        painter->fillRect(
            QRectF(base.rect.left(), base.rect.top(), 3,
                base.rect.height()),
            widgets::with_alpha(t.warning, flash * 0.85));
    }
    painter->restore();
    if (base.state & QStyle::State_HasFocus) {
        painter->save();
        painter->setClipRect(base.rect, Qt::IntersectClip);
        widgets::paint_focus_ring(*painter,
            QRectF(base.rect).adjusted(1.0, 1.0, -1.0, -1.0),
            t.radius.sm, 0.95);
        painter->restore();
    }
}

DebuggerTableModelBase::DebuggerTableModelBase(std::vector<model_column_t> columns,
                                               QObject* parent)
    : QAbstractTableModel(parent), columns_(std::move(columns)) {
}

int DebuggerTableModelBase::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : implRowCount();
}

int DebuggerTableModelBase::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(columns_.size());
}

QVariant DebuggerTableModelBase::headerData(int section, Qt::Orientation orientation,
                                            int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole &&
        section >= 0 && section < static_cast<int>(columns_.size()))
        return columns_[static_cast<std::size_t>(section)].label;
    return QVariant();
}

QVariant DebuggerTableModelBase::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.column() < 0 ||
        index.column() >= static_cast<int>(columns_.size()))
        return QVariant();
    if (role == RowIdRole)
        return QVariant::fromValue(rowId(index.row()));
    if (role == Qt::ToolTipRole)
        return cellData(index.row(), index.column(), TooltipTextRole);
    return cellData(index.row(), index.column(), role);
}

void DebuggerTableModelBase::multiData(const QModelIndex& index,
                                       QModelRoleDataSpan roleDataSpan) const {
    if (!index.isValid()) {
        for (QModelRoleData& d : roleDataSpan)
            d.clearData();
        return;
    }
    const int row = index.row();
    const int column = index.column();
    if (column < 0 || column >= static_cast<int>(columns_.size())) {
        for (QModelRoleData& d : roleDataSpan)
            d.clearData();
        return;
    }
    for (QModelRoleData& d : roleDataSpan) {
        const int role = d.role();
        if (role == RowIdRole)
            d.setData(QVariant::fromValue(rowId(row)));
        else if (role == Qt::ToolTipRole)
            d.setData(cellData(row, column, TooltipTextRole));
        else
            d.setData(cellData(row, column, role));
    }
}

int DebuggerTableModelBase::rowForId(quint64 id) const {
    const int count = implRowCount();
    for (int row = 0; row < count; ++row)
        if (rowId(row) == id)
            return row;
    return -1;
}

debugger_interaction::context_t DebuggerTableModelBase::contextForRow(int) const {
    return {};
}

QSet<quint64> capture_selected_row_ids(const DebuggerTableModelBase& model,
                                       const QItemSelectionModel* selection) {
    QSet<quint64> ids;
    if (!selection)
        return ids;
    const auto ranges = selection->selection();
    for (const auto& range : ranges) {
        for (int row = range.top(); row <= range.bottom(); ++row) {
            const quint64 id = model.rowId(row);
            if (id != 0)
                ids.insert(id);
        }
    }
    return ids;
}

void restore_selected_row_ids(DebuggerTableModelBase& model, QTableView* view,
                              const QSet<quint64>& ids, quint64 focus_id) {
    if (!view || ids.isEmpty())
        return;
    auto* selection = view->selectionModel();
    if (!selection)
        return;
    QItemSelection restored;
    for (const quint64 id : ids) {
        const int row = model.rowForId(id);
        if (row >= 0)
            restored.select(model.index(row, 0),
                model.index(row, model.columnCount() - 1));
    }
    if (restored.isEmpty())
        return;
    selection->select(restored,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    if (focus_id != 0) {
        const int focus_row = model.rowForId(focus_id);
        if (focus_row >= 0)
            selection->setCurrentIndex(model.index(focus_row, 0),
                QItemSelectionModel::NoUpdate);
    }
}

FlashMap::FlashMap(DebuggerTableModelBase* model,
                   const std::vector<int>& columns)
    : QObject(model), model_(model), columns_(columns) {
    timer_ = new QTimer(this);
    timer_->setInterval(16);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &FlashMap::decayTick);
}

void FlashMap::mark(int row) {
    if (theme::AidaMotion::reducedMotion())
        return;
    flash_.insert(row, 1.0);
    if (!timer_->isActive()) {
        last_tick_ns_ = 0;
        elapsed_.invalidate();
        timer_->start();
    }
}

qreal FlashMap::value(int row) const {
    return flash_.value(row, 0.0);
}

void FlashMap::clear() {
    flash_.clear();
    if (timer_->isActive())
        timer_->stop();
}

void FlashMap::decayTick() {
    if (flash_.isEmpty()) {
        timer_->stop();
        return;
    }
    if (!elapsed_.isValid()) {
        elapsed_.start();
        last_tick_ns_ = 0;
    }
    const qint64 now_ns = elapsed_.nsecsElapsed();
    const qreal dt = last_tick_ns_ == 0
        ? 0.016
        : static_cast<qreal>(now_ns - last_tick_ns_) / 1000000000.0;
    last_tick_ns_ = now_ns;
    int min_row = 0;
    int max_row = 0;
    bool any = false;
    for (auto it = flash_.begin(); it != flash_.end();) {
        it.value() *= std::exp(-3.0 * dt);
        if (it.value() < 0.005) {
            it = flash_.erase(it);
            continue;
        }
        if (!any) {
            min_row = max_row = it.key();
            any = true;
        } else {
            min_row = std::min(min_row, it.key());
            max_row = std::max(max_row, it.key());
        }
        ++it;
    }
    if (!any) {
        timer_->stop();
        return;
    }
    if (model_ && !columns_.empty() && model_->rowCount() > 0) {
        const int lo = std::max(0, min_row);
        const int hi = std::min(model_->rowCount() - 1, max_row);
        if (lo <= hi) {
            for (const int column : columns_) {
                Q_EMIT model_->dataChanged(model_->index(lo, column),
                    model_->index(hi, column), {FlashRole});
            }
        }
    }
}

RegistersModel::RegistersModel(QObject* parent)
    : DebuggerTableModelBase({{QStringLiteral("Name"), 96, false},
                              {QStringLiteral("Value"), 0, true}}, parent) {
    flash_ = new FlashMap(this, {1});
}

const char* RegistersModel::registerName(int row) noexcept {
    static const char* names[30] = {
        "RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP",
        "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
        "RIP", "RFLAGS", "CS", "SS", "DS", "ES", "FS", "GS",
        "DR0", "DR1", "DR2", "DR3", "DR6", "DR7"
    };
    return (row >= 0 && row < 30) ? names[row] : "";
}

bool RegistersModel::registerEditable(int row) noexcept {
    return row >= 0 && row < 30 && !(row >= 18 && row <= 23);
}

quint64 RegistersModel::registerValue(const debugger_engine::register_set_t& regs,
                                      int row) noexcept {
    switch (row) {
        case 0: return regs.rax; case 1: return regs.rbx;
        case 2: return regs.rcx; case 3: return regs.rdx;
        case 4: return regs.rsi; case 5: return regs.rdi;
        case 6: return regs.rbp; case 7: return regs.rsp;
        case 8: return regs.r8; case 9: return regs.r9;
        case 10: return regs.r10; case 11: return regs.r11;
        case 12: return regs.r12; case 13: return regs.r13;
        case 14: return regs.r14; case 15: return regs.r15;
        case 16: return regs.rip; case 17: return regs.rflags;
        case 18: return regs.cs; case 19: return regs.ss;
        case 20: return regs.ds; case 21: return regs.es;
        case 22: return regs.fs; case 23: return regs.gs;
        case 24: return regs.dr0; case 25: return regs.dr1;
        case 26: return regs.dr2; case 27: return regs.dr3;
        case 28: return regs.dr6; case 29: return regs.dr7;
        default: return 0;
    }
}

void RegistersModel::applyRegisters(
    const debugger_engine::register_set_t& registers, quint32 pid,
    quint32 active_tid) {
    (void)pid;
    (void)active_tid;
    if (!prev_initialized_) {
        registers_ = registers;
        prev_initialized_ = true;
        Q_EMIT dataChanged(index(0, 1), index(29, 1), {Qt::DisplayRole});
        diag::log_tagged_fmt("cpu_view", "prev_reg_initialized rows=%d", 30);
        return;
    }
    int lo = -1;
    int hi = -1;
    for (int row = 0; row < 30; ++row) {
        const quint64 previous = registerValue(registers_, row);
        const quint64 current = registerValue(registers, row);
        if (previous != current) {
            flash_->mark(row);
            diag::log_tagged_fmt("cpu_view", "reg_change name=%s new=0x%llx",
                registerName(row), static_cast<unsigned long long>(current));
            if (lo < 0) lo = row;
            hi = row;
        }
    }
    registers_ = registers;
    if (lo >= 0)
        Q_EMIT dataChanged(index(lo, 1), index(hi, 1), {Qt::DisplayRole});
}

int RegistersModel::implRowCount() const {
    return 30;
}

quint64 RegistersModel::rowId(int row) const {
    return static_cast<quint64>(row) + 1;
}

debugger_interaction::context_t RegistersModel::contextForRow(int row) const {
    if (row < 0 || row >= 30)
        return {};
    return debugger_interaction::capture(
        debugger_interaction::kind_t::register_value, 0,
        registerValue(registers_, row), row, 0, 0, registerName(row));
}

QVariant RegistersModel::cellData(int row, int column, int role) const {
    if (row < 0 || row >= 30)
        return QVariant();
    const auto& t = theme::tokens();
    const bool segment = row >= 18 && row <= 23;
    const bool debug = row >= 24;
    const quint64 value = registerValue(registers_, row);
    if (role == Qt::DisplayRole) {
        if (column == 0)
            return QString::fromLatin1(registerName(row));
        if (segment)
            return QString::asprintf("%04X",
                static_cast<unsigned>(value & 0xFFFFu));
        return hex64(value);
    }
    if (role == Qt::ForegroundRole) {
        if (column == 0) {
            if (segment) return t.info;
            if (debug) return t.warning;
            return t.text_primary;
        }
        if (value == 0) return t.text_dim;
        if (debug) return t.warning;
        if (segment) return t.info;
        if (value >= 0x00010000ULL && value < 0x00007FFFFFFFFFFFULL)
            return t.text_address;
        return t.syn_number;
    }
    if (role == Qt::FontRole) {
        return column == 0 ? theme::fonts::bodyEm() : theme::fonts::codeRegular();
    }
    if (role == FlashRole)
        return column == 1 ? flash_->value(row) : 0.0;
    if (role == AddressRole || role == ValueHexRole)
        return QVariant::fromValue(value);
    if (role == TooltipTextRole) {
        if (row == 17)
            return QString::fromStdString(
                debugger_engine::format_flags(registers_.rflags));
        return QString::asprintf("%s = 0x%016llX (%llu)", registerName(row),
            static_cast<unsigned long long>(value),
            static_cast<unsigned long long>(value));
    }
    return QVariant();
}

BreakpointsModel::BreakpointsModel(QObject* parent)
    : DebuggerRowsModel({{QStringLiteral("State"), 72, false},
                         {QStringLiteral("Address"), 200, false},
                         {QStringLiteral("Type"), 90, false},
                         {QStringLiteral("Hits"), 60, false},
                         {QStringLiteral("Name"), 160, false},
                         {QStringLiteral("Condition"), 0, true}}, parent) {
}

debugger_interaction::context_t BreakpointsModel::contextForRow(int row) const {
    const auto* bp = rowAt(row);
    if (!bp)
        return {};
    return debugger_interaction::capture(debugger_interaction::kind_t::breakpoint,
        bp->address, debugger_view::breakpoint_fingerprint(*bp), row, 0,
        static_cast<std::uint64_t>(bp->size), bp->name, bp->condition);
}

QVariant BreakpointsModel::cellFor(const debugger_engine::breakpoint_t& item,
                                   int row, int column, int role) const {
    (void)row;
    const auto& t = theme::tokens();
    const bool enabled = item.state == debugger_engine::bp_state_t::enabled;
    const bool unresolved = item.persistent_definition && !item.definition_resolved;
    const bool install_error = item.install_state ==
        debugger_engine::breakpoint_install_state_t::error;
    const bool install_pending = item.install_state ==
        debugger_engine::breakpoint_install_state_t::requested ||
        item.install_state == debugger_engine::breakpoint_install_state_t::installing ||
        item.install_state == debugger_engine::breakpoint_install_state_t::removing;
    const char* state_label = install_error ? "ERROR" :
        (install_pending ? "PENDING" : (unresolved ? "STALE" : (enabled ? "ON" : "OFF")));
    if (role == Qt::DisplayRole) {
        switch (column) {
            case State: return QString::fromLatin1(state_label);
            case Address:
                if (item.persistent_definition && !item.definition_module.empty())
                    return QString::asprintf("%s+0x%llX",
                        item.definition_module.c_str(),
                        static_cast<unsigned long long>(item.definition_module_offset));
                return hex64(item.address);
            case Type: {
                static const char* type_names[] = {
                    "SW", "HW EXEC", "HW WRITE", "HW READ", "MEM"
                };
                int type_index = static_cast<int>(item.type);
                if (type_index < 0 || type_index >= 5)
                    type_index = 0;
                return QString::fromLatin1(type_names[type_index]);
            }
            case Hits: return QVariant::fromValue(item.hit_count);
            case Name:
                return QString::fromStdString(item.name);
            case Condition:
                if (!item.condition.empty())
                    return QString::fromStdString(item.condition);
                if (unresolved && !item.definition_status.empty())
                    return QString::fromStdString(item.definition_status);
                return QVariant();
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == State) {
            if (install_error) return t.error;
            if (install_pending) return t.warning;
            if (unresolved) return t.warning;
            if (enabled) return t.success;
            return t.text_secondary;
        }
        if (column == Address) return t.text_address;
        if (column == Name && unresolved) return t.warning;
        if (column == Name || column == Condition) return t.text_secondary;
        return QVariant();
    }
    if (role == Qt::FontRole &&
        (column == Address || column == Condition))
        return theme::fonts::codeRegular();
    if (role == StateTokenRole && column == State)
        return QString::fromLatin1(state_label);
    if (role == StateKindRole && column == State) {
        if (install_error) return QStringLiteral("error");
        if (install_pending || unresolved) return QStringLiteral("warning");
        if (enabled) return QStringLiteral("success");
        return QStringLiteral("neutral");
    }
    if (role == AddressRole)
        return QVariant::fromValue(item.address);
    if (role == TooltipTextRole) {
        QStringList lines;
        lines << QStringLiteral("Address: ") + hex0x64(item.address);
        if (!item.name.empty())
            lines << QStringLiteral("Name: ") + QString::fromStdString(item.name);
        if (!item.condition.empty())
            lines << QStringLiteral("Condition: ") +
                QString::fromStdString(item.condition);
        if (!item.log_text.empty())
            lines << QStringLiteral("Log: ") + QString::fromStdString(item.log_text);
        lines << QStringLiteral("Hits: ") + QString::number(item.hit_count);
        if (!item.install_detail.empty())
            lines << QString::fromStdString(item.install_detail);
        return lines.join(QStringLiteral("\n"));
    }
    return QVariant();
}

quint64 BreakpointsModel::idFor(const debugger_engine::breakpoint_t& item,
                                int row) const {
    (void)row;
    return debugger_view::breakpoint_fingerprint(item);
}

CallStackModel::CallStackModel(QObject* parent)
    : DebuggerRowsModel({{QStringLiteral("#"), 44, false},
                         {QStringLiteral("Symbol"), 0, true},
                         {QStringLiteral("Address"), 150, false},
                         {QStringLiteral("Return"), 150, false}}, parent) {
}

debugger_interaction::context_t CallStackModel::contextForRow(int row) const {
    const auto* frame = rowAt(row);
    if (!frame)
        return {};
    return debugger_interaction::capture(debugger_interaction::kind_t::stack_frame,
        frame->address, frame->return_addr, row, 0, frame->module_size,
        frame->function_name, frame->module_name);
}

QVariant CallStackModel::cellFor(const debugger_engine::stack_frame_t& item,
                                 int row, int column, int role) const {
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Frame: return QString::asprintf("#%d", row);
            case Symbol: {
                const QString module = item.module_name.empty()
                    ? QStringLiteral("<unknown>")
                    : QString::fromStdString(item.module_name);
                const QString function = item.function_name.empty()
                    ? QStringLiteral("?")
                    : QString::fromStdString(item.function_name);
                return QStringLiteral("%1!%2 + 0x%3").arg(module, function)
                    .arg(item.module_offset, 0, 16);
            }
            case Address: return hex0x64(item.address);
            case ReturnAddress: return hex0x64(item.return_addr);
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Frame) return t.text_dim;
        if (column == Symbol) return t.syn_function;
        if (column == Address || column == ReturnAddress) return t.text_address;
    }
    if (role == Qt::FontRole && column != Frame)
        return theme::fonts::codeRegular();
    if (role == AddressRole)
        return QVariant::fromValue(item.address);
    if (role == TooltipTextRole) {
        const QString module = item.module_name.empty()
            ? QStringLiteral("<unknown>")
            : QString::fromStdString(item.module_name);
        const QString function = item.function_name.empty()
            ? QStringLiteral("?")
            : QString::fromStdString(item.function_name);
        QStringList lines;
        lines << QStringLiteral("%1!%2 + 0x%3").arg(module, function)
            .arg(item.module_offset, 0, 16);
        lines << QStringLiteral("Frame: ") + hex0x64(item.address);
        lines << QStringLiteral("Return: ") + hex0x64(item.return_addr);
        if (!item.module_path.empty())
            lines << QString::fromStdString(item.module_path);
        if (!item.symbol_status.empty() && item.symbol_status != "not_attempted")
            lines << QStringLiteral("Symbols: ") +
                QString::fromStdString(item.symbol_status);
        return lines.join(QStringLiteral("\n"));
    }
    return QVariant();
}

quint64 CallStackModel::idFor(const debugger_engine::stack_frame_t& item,
                              int row) const {
    return item.address ^ (item.return_addr << 1) ^ static_cast<quint64>(row);
}

ThreadsModel::ThreadsModel(QObject* parent)
    : DebuggerRowsModel({{QStringLiteral("TID"), 90, false},
                         {QStringLiteral("Priority"), 80, false},
                         {QStringLiteral("State"), 130, false},
                         {QStringLiteral("RIP"), 0, true}}, parent) {
    flash_ = new FlashMap(this, {State});
}

void ThreadsModel::applyThreads(const_rows_ptr rows, quint64 generation) {
    if (rows && generation != this->generation()) {
        for (std::size_t i = 0; i < rows->size(); ++i) {
            const auto& incoming = (*rows)[i];
            const auto previous = prev_state_.constFind(incoming.tid);
            if (previous != prev_state_.constEnd() &&
                previous.value() != incoming.state)
                flash_->mark(static_cast<int>(i));
            prev_state_.insert(incoming.tid, incoming.state);
        }
        for (auto it = prev_state_.begin(); it != prev_state_.end();) {
            bool present = false;
            for (const auto& incoming : *rows) {
                if (incoming.tid == it.key()) {
                    present = true;
                    break;
                }
            }
            if (!present) it = prev_state_.erase(it);
            else ++it;
        }
    }
    applySnapshot(std::move(rows), generation);
}

debugger_interaction::context_t ThreadsModel::contextForRow(int row) const {
    const auto* thread = rowAt(row);
    if (!thread)
        return {};
    return debugger_interaction::capture(debugger_interaction::kind_t::thread,
        thread->rip, 0, row, thread->tid);
}

QVariant ThreadsModel::cellFor(const debugger_engine::cached_thread_t& item,
                               int row, int column, int role) const {
    const auto& t = theme::tokens();
    const char* state_str = "UNKNOWN";
    const char* state_kind = "neutral";
    switch (item.state) {
        case 0: state_str = "INITIALIZED"; state_kind = "info"; break;
        case 1: state_str = "READY";       state_kind = "info"; break;
        case 2: state_str = "RUNNING";     state_kind = "success"; break;
        case 3: state_str = "STANDBY";     state_kind = "info"; break;
        case 4: state_str = "TERMINATED";  state_kind = "error"; break;
        case 5: state_str = "WAITING";     state_kind = "warning"; break;
        case 6: state_str = "TRANSITION";  state_kind = "warning"; break;
        default: break;
    }
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Tid: return QString::number(item.tid);
            case Priority: return QString::number(item.priority);
            case State: return QString::fromLatin1(state_str);
            case Rip:
                return item.rip != 0 ? hex0x64(item.rip) : QVariant();
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Tid) return t.text_address;
        if (column == Priority) return t.text_secondary;
        if (column == State) return token_color(state_kind);
        if (column == Rip) return t.text_address;
    }
    if (role == Qt::FontRole && (column == Tid || column == Rip))
        return theme::fonts::codeRegular();
    if (role == StateTokenRole && column == State)
        return QString::fromLatin1(state_str);
    if (role == StateKindRole && column == State)
        return QString::fromLatin1(state_kind);
    if (role == FlashRole && column == State)
        return flash_->value(row);
    if (role == AddressRole)
        return QVariant::fromValue(item.rip);
    if (role == RowIdRole)
        return QVariant::fromValue(static_cast<quint64>(item.tid));
    if (role == TooltipTextRole) {
        QStringList lines;
        lines << QStringLiteral("TID %1 (0x%2)")
            .arg(item.tid)
            .arg(item.tid, 0, 16);
        lines << QStringLiteral("State: %1").arg(
            QString::fromLatin1(state_str));
        if (item.rip != 0)
            lines << QStringLiteral("RIP: ") + hex0x64(item.rip);
        return lines.join(QStringLiteral("\n"));
    }
    return QVariant();
}

quint64 ThreadsModel::idFor(const debugger_engine::cached_thread_t& item,
                            int row) const {
    (void)row;
    return static_cast<quint64>(item.tid);
}

WatchesModel::WatchesModel(QObject* parent)
    : DebuggerRowsModel({{QStringLiteral("Expression"), 220, false},
                         {QStringLiteral("Resolved Address"), 180, false},
                         {QStringLiteral("Value"), 0, true}}, parent) {
}

void WatchesModel::setRegisters(
    const debugger_engine::register_set_t& registers) {
    const bool changed =
        registers.rip != registers_.rip || registers.rsp != registers_.rsp ||
        registers.rax != registers_.rax || registers.rbx != registers_.rbx ||
        registers.rcx != registers_.rcx || registers.rdx != registers_.rdx ||
        registers.rsi != registers_.rsi || registers.rdi != registers_.rdi ||
        registers.rbp != registers_.rbp || registers.r8 != registers_.r8 ||
        registers.r9 != registers_.r9 || registers.r10 != registers_.r10 ||
        registers.r11 != registers_.r11 || registers.r12 != registers_.r12 ||
        registers.r13 != registers_.r13 || registers.r14 != registers_.r14 ||
        registers.r15 != registers_.r15;
    if (!changed)
        return;
    registers_ = registers;
    if (implRowCount() > 0)
        Q_EMIT dataChanged(index(0, ResolvedAddress),
            index(implRowCount() - 1, ResolvedAddress), {Qt::DisplayRole});
}

debugger_interaction::context_t WatchesModel::contextForRow(int row) const {
    const auto* watch = rowAt(row);
    if (!watch)
        return {};
    const std::string& display = watch->persistent_expression.empty()
        ? watch->expression : watch->persistent_expression;
    const char* value_text = watch->valid ? watch->value.c_str()
        : (watch->error.empty() ? "<error>" : watch->error.c_str());
    bool deref = false;
    bool ok = false;
    const quint64 resolved = watch->definition_resolved
        ? debugger_view::evaluate_watch_expression(watch->expression, registers_,
            deref, ok)
        : 0;
    return debugger_interaction::capture(debugger_interaction::kind_t::watch,
        ok ? resolved : 0, 0, row, 0, 0, display, value_text);
}

QVariant WatchesModel::cellFor(const debugger_engine::watch_entry_t& item,
                               int row, int column, int role) const {
    (void)row;
    const auto& t = theme::tokens();
    const std::string& display = item.persistent_expression.empty()
        ? item.expression : item.persistent_expression;
    bool deref = false;
    bool ok = false;
    const quint64 resolved = item.definition_resolved
        ? debugger_view::evaluate_watch_expression(item.expression, registers_,
            deref, ok)
        : 0;
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Expression: return QString::fromStdString(display);
            case ResolvedAddress:
                if (!item.definition_resolved)
                    return QStringLiteral("unresolved");
                if (!ok)
                    return QStringLiteral("?");
                return deref
                    ? QStringLiteral("[*] ") + hex0x64(resolved)
                    : hex0x64(resolved);
            case Value:
                return item.valid
                    ? QString::fromStdString(item.value)
                    : (item.error.empty() ? QStringLiteral("<error>")
                        : QString::fromStdString(item.error));
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Expression)
            return item.definition_resolved ? t.text_primary : t.warning;
        if (column == ResolvedAddress) return t.text_address;
        if (column == Value) return item.valid ? t.success : t.error;
    }
    if (role == Qt::FontRole)
        return theme::fonts::codeRegular();
    if (role == AddressRole)
        return QVariant::fromValue(ok ? resolved : 0);
    if (role == TooltipTextRole) {
        QStringList lines;
        lines << QString::fromStdString(display);
        if (item.persistent_definition && !item.definition_module.empty()) {
            lines << QString::asprintf("Persistent definition: %s+0x%llX",
                item.definition_module.c_str(),
                static_cast<unsigned long long>(item.definition_module_offset));
            if (!item.definition_resolved && !item.error.empty())
                lines << QString::fromStdString(item.error);
        }
        return lines.join(QStringLiteral("\n"));
    }
    return QVariant();
}

quint64 WatchesModel::idFor(const debugger_engine::watch_entry_t& item,
                            int row) const {
    (void)row;
    const std::string& key = item.persistent_expression.empty()
        ? item.expression : item.persistent_expression;
    quint64 hash = 1469598103934665603ULL;
    for (const char c : key) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

HandlesModel::HandlesModel(QObject* parent)
    : DebuggerRowsModel({{QStringLiteral("Handle"), 110, false},
                         {QStringLiteral("Type"), 160, false},
                         {QStringLiteral("Access"), 110, false},
                         {QStringLiteral("Name"), 0, true}}, parent) {
}

debugger_interaction::context_t HandlesModel::contextForRow(int row) const {
    const auto* handle = rowAt(row);
    if (!handle)
        return {};
    return debugger_interaction::capture(debugger_interaction::kind_t::handle,
        0, handle->handle, row, 0, 0, handle->name, handle->type_name);
}

QVariant HandlesModel::cellFor(const debugger_engine::handle_info_t& item,
                               int row, int column, int role) const {
    (void)row;
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Handle:
                return QString::asprintf("0x%X",
                    static_cast<unsigned>(item.handle));
            case Type: return QString::fromStdString(item.type_name);
            case Access:
                return QString::asprintf("0x%08X", item.access);
            case Name: return QString::fromStdString(item.name);
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Handle) return t.text_primary;
        if (column == Type) {
            const QLatin1StringView type(item.type_name.c_str());
            if (type == "File") return t.info;
            if (type == "Thread") return t.warning;
            if (type == "Mutant") return t.accent;
            if (type == "Section") return t.error;
            if (type == "Key") return t.success;
            if (type == "Event" || type == "Semaphore") return t.warning;
            return t.text_secondary;
        }
        if (column == Access) return t.text_secondary;
        if (column == Name) return t.text_primary;
    }
    if (role == Qt::FontRole && (column == Handle || column == Access))
        return theme::fonts::codeRegular();
    if (role == AddressRole || role == ValueHexRole)
        return QVariant::fromValue(item.handle);
    if (role == TooltipTextRole) {
        QStringList lines;
        if (!item.name.empty())
            lines << QString::fromStdString(item.name);
        lines << QStringLiteral("Type: ") +
            QString::fromStdString(item.type_name);
        lines << QString::asprintf("Handle: 0x%X  Access: 0x%08X",
            static_cast<unsigned>(item.handle), item.access);
        return lines.join(QStringLiteral("\n"));
    }
    return QVariant();
}

quint64 HandlesModel::idFor(const debugger_engine::handle_info_t& item,
                            int row) const {
    (void)row;
    return item.handle;
}

TraceModel::TraceModel(QObject* parent)
    : DebuggerRowsModel({{QStringLiteral("#"), 60, false},
                         {QStringLiteral("Address"), 170, false},
                         {QStringLiteral("Instruction"), 0, true}}, parent) {
}

void TraceModel::setFilter(const QString& filter) {
    const QString normalized = filter.toLower();
    if (normalized == filter_)
        return;
    filter_ = normalized;
    if (!rows() || rows()->empty()) {
        filtered_.clear();
        return;
    }
    beginResetModel();
    rebuildFilter();
    endResetModel();
}

void TraceModel::applyTrace(const_rows_ptr rows, quint64 generation) {
    applySnapshot(std::move(rows), generation);
}

void TraceModel::rowsReplaced() {
    rebuildFilter();
}

void TraceModel::rebuildFilter() {
    filtered_.clear();
    const auto& source = rows();
    if (!source)
        return;
    filtered_.reserve(source->size());
    for (std::size_t index = 0; index < source->size(); ++index) {
        const auto& record = (*source)[index];
        bool matches = filter_.isEmpty();
        if (!matches) {
            const QString text = QString::fromStdString(record.disasm_text).toLower();
            const QString address = QString::number(
                static_cast<qulonglong>(record.address), 16);
            matches = text.contains(filter_) || address.contains(filter_);
        }
        if (matches)
            filtered_.push_back(static_cast<int>(index));
    }
}

int TraceModel::implRowCount() const {
    return static_cast<int>(filtered_.size());
}

debugger_interaction::context_t TraceModel::contextForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(filtered_.size()))
        return {};
    const auto* record = rowAt(filtered_[static_cast<std::size_t>(row)]);
    if (!record)
        return {};
    return debugger_interaction::capture(
        debugger_interaction::kind_t::trace_record, record->address, 0, row, 0, 0,
        record->disasm_text);
}

QVariant TraceModel::cellFor(const debugger_engine::trace_record_t& item,
                             int row, int column, int role) const {
    (void)row;
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Index: return QVariant::fromValue(item.index);
            case Address: return hex0x64(item.address);
            case Instruction: return QString::fromStdString(item.disasm_text);
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Index) return t.text_dim;
        if (column == Address) return t.text_address;
        if (column == Instruction) return t.text_primary;
    }
    if (role == Qt::FontRole && column != Index)
        return theme::fonts::codeRegular();
    if (role == AddressRole)
        return QVariant::fromValue(item.address);
    if (role == TooltipTextRole)
        return hex0x64(item.address) + QStringLiteral("  ") +
            QString::fromStdString(item.disasm_text);
    return QVariant();
}

quint64 TraceModel::idFor(const debugger_engine::trace_record_t& item,
                          int row) const {
    (void)row;
    return static_cast<quint64>(static_cast<quint32>(item.index));
}

StringsModel::StringsModel(QObject* parent)
    : DebuggerRowsModel({{QStringLiteral("Address"), 170, false},
                         {QStringLiteral("String"), 0, true},
                         {QStringLiteral("Module"), 140, false}}, parent) {
}

void StringsModel::setFilter(const QString& filter) {
    const QString normalized = filter.toLower();
    if (normalized == filter_)
        return;
    filter_ = normalized;
    if (!rows() || rows()->empty()) {
        filtered_.clear();
        return;
    }
    beginResetModel();
    rebuildFilter();
    endResetModel();
}

void StringsModel::applyStrings(const_rows_ptr rows, quint64 generation) {
    applySnapshot(std::move(rows), generation);
}

void StringsModel::rowsReplaced() {
    rebuildFilter();
}

void StringsModel::rebuildFilter() {
    filtered_.clear();
    const auto& source = rows();
    if (!source)
        return;
    filtered_.reserve(source->size());
    for (std::size_t index = 0; index < source->size(); ++index) {
        const auto& item = (*source)[index];
        bool matches = filter_.isEmpty();
        if (!matches) {
            const QString value = QString::fromStdString(item.value).toLower();
            matches = value.contains(filter_);
        }
        if (matches)
            filtered_.push_back(static_cast<int>(index));
    }
}

int StringsModel::implRowCount() const {
    return static_cast<int>(filtered_.size());
}

debugger_interaction::context_t StringsModel::contextForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(filtered_.size()))
        return {};
    const auto* item = rowAt(filtered_[static_cast<std::size_t>(row)]);
    if (!item)
        return {};
    return debugger_interaction::capture(
        debugger_interaction::kind_t::string_value, item->address, 0, row, 0,
        item->value.size(), item->value, item->module_name);
}

QVariant StringsModel::cellFor(const debugger_engine::string_ref_t& item,
                               int row, int column, int role) const {
    (void)row;
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Address: return hex64(item.address);
            case String: {
                const std::size_t display_length =
                    (std::min)(item.value.size(), std::size_t{96});
                QString text = QString::fromLatin1(item.value.data(),
                    static_cast<qsizetype>(display_length));
                if (display_length < item.value.size())
                    text += QStringLiteral("...");
                return text;
            }
            case Module: return QString::fromStdString(item.module_name);
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Address) return t.text_address;
        if (column == String) return t.syn_string;
        if (column == Module) return t.text_dim;
    }
    if (role == Qt::FontRole && column == Address)
        return theme::fonts::codeRegular();
    if (role == AddressRole)
        return QVariant::fromValue(item.address);
    if (role == TooltipTextRole)
        return QString::fromStdString(item.value);
    return QVariant();
}

quint64 StringsModel::idFor(const debugger_engine::string_ref_t& item,
                            int row) const {
    (void)row;
    return item.address;
}

BookmarksModel::BookmarksModel(QObject* parent)
    : DebuggerTableModelBase({{QStringLiteral("#"), 36, false},
                              {QStringLiteral("Address"), 200, false},
                              {QStringLiteral("Label"), 0, true}}, parent) {
}

void BookmarksModel::applyBookmarks(
    std::vector<std::uint64_t> bookmarks,
    std::map<std::uint64_t, std::string> labels) {
    beginResetModel();
    bookmarks_ = std::move(bookmarks);
    labels_ = std::move(labels);
    endResetModel();
    Q_EMIT snapshotApplied(0);
}

int BookmarksModel::implRowCount() const {
    return static_cast<int>(bookmarks_.size());
}

quint64 BookmarksModel::rowId(int row) const {
    return (row >= 0 && row < static_cast<int>(bookmarks_.size()))
        ? bookmarks_[static_cast<std::size_t>(row)] : 0;
}

debugger_interaction::context_t BookmarksModel::contextForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(bookmarks_.size()))
        return {};
    const std::uint64_t address = bookmarks_[static_cast<std::size_t>(row)];
    const auto label = labels_.find(address);
    return debugger_interaction::capture(debugger_interaction::kind_t::bookmark,
        address, 0, row, 0, 0,
        label != labels_.end() ? label->second : std::string());
}

QVariant BookmarksModel::cellData(int row, int column, int role) const {
    if (row < 0 || row >= static_cast<int>(bookmarks_.size()))
        return QVariant();
    const auto& t = theme::tokens();
    const std::uint64_t address = bookmarks_[static_cast<std::size_t>(row)];
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Index: return QString::asprintf("#%d", row);
            case Address: return hex0x64(address);
            case Label: {
                const auto label = labels_.find(address);
                return label != labels_.end()
                    ? QString::fromStdString(label->second) : QVariant();
            }
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Index) return t.text_dim;
        if (column == Address) return t.text_address;
        if (column == Label) return t.text_primary;
    }
    if (role == Qt::FontRole && column == Address)
        return theme::fonts::codeRegular();
    if (role == AddressRole)
        return QVariant::fromValue(address);
    if (role == TooltipTextRole) {
        const auto label = labels_.find(address);
        if (label != labels_.end() && !label->second.empty())
            return hex0x64(address) + QStringLiteral("  ") +
                QString::fromStdString(label->second);
        return hex0x64(address);
    }
    return QVariant();
}

ModulesModel::ModulesModel(QObject* parent)
    : DebuggerRowsModel({{QStringLiteral("Name"), 200, false},
                         {QStringLiteral("Base"), 150, false},
                         {QStringLiteral("Size"), 100, false},
                         {QStringLiteral("Path"), 0, true}}, parent) {
}

void ModulesModel::setFilter(const QString& filter) {
    const QString normalized = filter.toLower();
    if (normalized == filter_)
        return;
    filter_ = normalized;
    if (!rows() || rows()->empty()) {
        filtered_.clear();
        return;
    }
    beginResetModel();
    rebuildFilter();
    endResetModel();
}

void ModulesModel::applyModules(const_rows_ptr rows, quint64 generation) {
    applySnapshot(std::move(rows), generation);
}

void ModulesModel::rowsReplaced() {
    rebuildFilter();
}

void ModulesModel::rebuildFilter() {
    filtered_.clear();
    const auto& source = rows();
    if (!source)
        return;
    filtered_.reserve(source->size());
    for (std::size_t index = 0; index < source->size(); ++index) {
        const auto& item = (*source)[index];
        bool matches = filter_.isEmpty();
        if (!matches) {
            matches = QString::fromStdString(item.name).toLower().contains(filter_) ||
                QString::fromStdString(item.path).toLower().contains(filter_);
        }
        if (matches)
            filtered_.push_back(static_cast<int>(index));
    }
}

int ModulesModel::implRowCount() const {
    return static_cast<int>(filtered_.size());
}

debugger_interaction::context_t ModulesModel::contextForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(filtered_.size()))
        return {};
    const auto* item = rowAt(filtered_[static_cast<std::size_t>(row)]);
    if (!item)
        return {};
    return debugger_interaction::capture(debugger_interaction::kind_t::module,
        item->base, item->size, row, 0, item->size, item->name, item->path);
}

QVariant ModulesModel::cellFor(const driver_bridge::module_info_t& item,
                               int row, int column, int role) const {
    (void)row;
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Name: return QString::fromStdString(item.name);
            case Base: return hex0x64(item.base);
            case Size:
                return QString::fromStdString(
                    memory_map_view::detail::format_size(item.size));
            case Path: return QString::fromStdString(item.path);
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Name) return t.text_primary;
        if (column == Base) return t.text_address;
        if (column == Size) return t.text_secondary;
        if (column == Path) return t.text_dim;
    }
    if (role == Qt::FontRole && column == Base)
        return theme::fonts::codeRegular();
    if (role == AddressRole)
        return QVariant::fromValue(item.base);
    if (role == TooltipTextRole)
        return QString::fromStdString(item.path);
    return QVariant();
}

quint64 ModulesModel::idFor(const driver_bridge::module_info_t& item,
                            int row) const {
    (void)row;
    return item.base;
}

PatchesModel::PatchesModel(QObject* parent)
    : DebuggerTableModelBase({{QStringLiteral("State"), 90, false},
                              {QStringLiteral("Address"), 170, false},
                              {QStringLiteral("Original"), 170, false},
                              {QStringLiteral("Patched"), 170, false},
                              {QStringLiteral("Description"), 0, true}}, parent) {
}

void PatchesModel::applyRows(std::vector<row_t> rows,
                             quint64 publication_generation,
                             std::size_t total_count) {
    beginResetModel();
    rows_ = std::move(rows);
    generation_ = publication_generation;
    total_count_ = total_count;
    endResetModel();
    Q_EMIT snapshotApplied(generation_);
}

const PatchesModel::row_t* PatchesModel::patchRow(int row) const {
    return (row >= 0 && row < static_cast<int>(rows_.size()))
        ? &rows_[static_cast<std::size_t>(row)] : nullptr;
}

int PatchesModel::implRowCount() const {
    return static_cast<int>(rows_.size());
}

quint64 PatchesModel::rowId(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return 0;
    const auto& rowData = rows_[static_cast<std::size_t>(row)];
    quint64 hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            hash ^= static_cast<std::uint8_t>(value >> shift);
            hash *= 1099511628211ULL;
        }
    };
    mix(rowData.address);
    mix(static_cast<std::uint64_t>(rowData.patched_size));
    for (const QChar c : rowData.description)
        mix(static_cast<unsigned>(c.unicode()));
    return hash;
}

debugger_interaction::context_t PatchesModel::contextForRow(int row) const {
    const auto* item = patchRow(row);
    if (!item)
        return {};
    return debugger_interaction::capture(debugger_interaction::kind_t::patch,
        item->address, generation_, row, 0,
        static_cast<std::uint64_t>(item->patched_size),
        item->description.toStdString());
}

QVariant PatchesModel::cellData(int row, int column, int role) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return QVariant();
    const auto& t = theme::tokens();
    const auto& item = rows_[static_cast<std::size_t>(row)];
    if (role == Qt::DisplayRole) {
        switch (column) {
            case State: return item.active ? QStringLiteral("ACTIVE")
                : QStringLiteral("STAGED");
            case Address: return hex0x64(item.address);
            case Original: return item.original;
            case Patched: return item.patched;
            case Description: return item.description;
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == State)
            return item.active ? t.success : t.text_secondary;
        if (column == Address) return t.text_address;
        if (column == Original || column == Patched) return t.syn_number;
        if (column == Description) return t.text_primary;
    }
    if (role == Qt::FontRole && column != State && column != Description)
        return theme::fonts::codeRegular();
    if (role == AddressRole)
        return QVariant::fromValue(item.address);
    if (role == TooltipTextRole) {
        QStringList lines;
        lines << hex0x64(item.address);
        if (!item.description.isEmpty())
            lines << item.description;
        lines << QStringLiteral("Original: ") + item.original;
        lines << QStringLiteral("Patched:  ") + item.patched;
        return lines.join(QStringLiteral("\n"));
    }
    return QVariant();
}

SehModel::SehModel(QObject* parent)
    : DebuggerTableModelBase({{QStringLiteral("#"), 40, false},
                              {QStringLiteral("Frame"), 170, false},
                              {QStringLiteral("Handler"), 170, false},
                              {QStringLiteral("Module"), 0, true}}, parent) {
}

void SehModel::applySeh(
    std::shared_ptr<const std::vector<seh_view::seh_entry_t>> entries,
    quint64 generation) {
    if (entries && generation == generation_ && entries == entries_)
        return;
    beginResetModel();
    entries_ = std::move(entries);
    generation_ = generation;
    endResetModel();
    Q_EMIT snapshotApplied(generation_);
}

int SehModel::implRowCount() const {
    return entries_ ? static_cast<int>(entries_->size()) : 0;
}

quint64 SehModel::rowId(int row) const {
    if (!entries_ || row < 0 || row >= static_cast<int>(entries_->size()))
        return 0;
    return (*entries_)[static_cast<std::size_t>(row)].handler_addr;
}

debugger_interaction::context_t SehModel::contextForRow(int row) const {
    if (!entries_ || row < 0 || row >= static_cast<int>(entries_->size()))
        return {};
    const auto& entry = (*entries_)[static_cast<std::size_t>(row)];
    return debugger_interaction::capture(
        debugger_interaction::kind_t::instruction, entry.handler_addr, 0, row, 0, 0,
        entry.handler_name, entry.module_name);
}

QVariant SehModel::cellData(int row, int column, int role) const {
    if (!entries_ || row < 0 || row >= static_cast<int>(entries_->size()))
        return QVariant();
    const auto& t = theme::tokens();
    const auto& entry = (*entries_)[static_cast<std::size_t>(row)];
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Index: return QString::asprintf("#%d", entry.index);
            case Frame: return hex0x64(entry.frame_addr);
            case Handler: return hex0x64(entry.handler_addr);
            case Module:
                return entry.handler_name.empty()
                    ? QStringLiteral("Unresolved")
                    : QString::fromStdString(entry.handler_name);
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Index) return t.text_dim;
        if (column == Frame) return t.text_secondary;
        if (column == Handler) return t.text_address;
        if (column == Module)
            return entry.handler_name.empty() ? t.text_dim : t.text_primary;
    }
    if (role == Qt::FontRole && (column == Frame || column == Handler))
        return theme::fonts::codeRegular();
    if (role == AddressRole)
        return QVariant::fromValue(entry.handler_addr);
    if (role == TooltipTextRole) {
        if (entry.handler_name.empty())
            return QStringLiteral("Unresolved");
        return QString::fromStdString(entry.handler_name);
    }
    return QVariant();
}

MemoryRegionsModel::MemoryRegionsModel(QObject* parent)
    : DebuggerRowsModel({{QStringLiteral("Address"), 170, false},
                         {QStringLiteral("Size"), 100, false},
                         {QStringLiteral("Protect"), 130, false},
                         {QStringLiteral("State"), 100, false},
                         {QStringLiteral("Type"), 90, false},
                         {QStringLiteral("Module"), 160, false},
                         {QStringLiteral("Info"), 0, true}}, parent) {
}

void MemoryRegionsModel::setFilter(const QString& filter) {
    const QString normalized = filter.toLower();
    if (normalized == filter_)
        return;
    filter_ = normalized;
    if (!rows() || rows()->empty()) {
        filtered_.clear();
        return;
    }
    beginResetModel();
    rebuildFilter();
    endResetModel();
}

void MemoryRegionsModel::applyRegions(const_rows_ptr rows, quint64 generation) {
    if (rows && generation != this->generation()) {
        committed_bytes_ = 0;
        rwx_count_ = 0;
        for (const auto& region : *rows) {
            if (region.state == 0x1000)
                committed_bytes_ += region.size;
            const bool executable = (region.protect & 0xF0) != 0;
            const bool writable = region.protect == 0x04 || region.protect == 0x08 ||
                region.protect == 0x40 || region.protect == 0x80;
            if (executable && writable)
                ++rwx_count_;
        }
    }
    applySnapshot(std::move(rows), generation);
}

void MemoryRegionsModel::rowsReplaced() {
    rebuildFilter();
}

void MemoryRegionsModel::rebuildFilter() {
    filtered_.clear();
    const auto& source = rows();
    if (!source)
        return;
    filtered_.reserve(source->size());
    for (std::size_t index = 0; index < source->size(); ++index) {
        const auto& region = (*source)[index];
        bool matches = filter_.isEmpty();
        if (!matches) {
            matches = QString::fromStdString(region.module_name).toLower()
                    .contains(filter_) ||
                QString::fromStdString(region.info).toLower().contains(filter_);
        }
        if (matches)
            filtered_.push_back(static_cast<int>(index));
    }
}

int MemoryRegionsModel::implRowCount() const {
    return static_cast<int>(filtered_.size());
}

debugger_interaction::context_t MemoryRegionsModel::contextForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(filtered_.size()))
        return {};
    const auto* item = rowAt(filtered_[static_cast<std::size_t>(row)]);
    if (!item)
        return {};
    return debugger_interaction::capture(
        debugger_interaction::kind_t::memory_region, item->base, 0, row, 0,
        item->size, item->module_name, item->info);
}

QVariant MemoryRegionsModel::cellFor(
    const debugger_engine::memory_region_t& item, int row, int column,
    int role) const {
    (void)row;
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Address: return hex64(item.base);
            case Size:
                return QString::fromStdString(
                    memory_map_view::detail::format_size(item.size));
            case Protect:
                return QString::fromStdString(
                    debugger_engine::format_protect(item.protect));
            case State:
                return QString::fromStdString(
                    memory_map_view::detail::format_state(item.state));
            case Type:
                return QString::fromStdString(
                    memory_map_view::detail::format_type(item.type));
            case Module: return QString::fromStdString(item.module_name);
            case Info: return QString::fromStdString(item.info);
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == Address) return t.text_address;
        if (column == Protect) {
            const bool exec = (item.protect & 0xF0) != 0;
            const bool write = item.protect == 0x04 || item.protect == 0x08 ||
                item.protect == 0x40 || item.protect == 0x80;
            if (exec) return t.error;
            if (write) return t.warning;
            return t.success;
        }
        if (column == State) {
            if (item.state == 0x1000) return t.text_primary;
            if (item.state == 0x2000) return t.text_secondary;
            return t.text_dim;
        }
        if (column == Type) {
            if (item.type == 0x1000000) return t.info;
            if (item.type == 0x40000) return t.success;
            return t.text_secondary;
        }
        if (column == Module) return t.text_secondary;
        if (column == Info) return t.text_dim;
        if (column == Size) return t.text_secondary;
    }
    if (role == Qt::FontRole && column == Address)
        return theme::fonts::codeRegular();
    if (role == AddressRole)
        return QVariant::fromValue(item.base);
    if (role == TooltipTextRole) {
        return QString::asprintf("0x%016llX - 0x%016llX\n%s | %s | %s\n%s",
            static_cast<unsigned long long>(item.base),
            static_cast<unsigned long long>(item.base + item.size),
            debugger_engine::format_protect(item.protect).c_str(),
            memory_map_view::detail::format_state(item.state).c_str(),
            memory_map_view::detail::format_type(item.type).c_str(),
            memory_map_view::detail::format_size(item.size).c_str());
    }
    return QVariant();
}

quint64 MemoryRegionsModel::idFor(const debugger_engine::memory_region_t& item,
                                  int row) const {
    (void)row;
    return item.base;
}

ModuleExportsModel::ModuleExportsModel(QObject* parent)
    : DebuggerRowsModel({{QStringLiteral("Ordinal"), 80, false},
                         {QStringLiteral("Name"), 0, true},
                         {QStringLiteral("Address"), 170, false},
                         {QStringLiteral("Forwarded"), 200, false}}, parent) {
}

QVariant ModuleExportsModel::cellFor(const pe_parser::export_entry_t& item,
                                     int row, int column, int role) const {
    (void)row;
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
            case 0: return QVariant::fromValue(item.ordinal);
            case 1: return QString::fromStdString(item.name);
            case 2: return hex0x64(item.address);
            case 3:
                return item.is_forwarded
                    ? QString::fromStdString(item.forward_name) : QVariant();
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == 1) return t.text_primary;
        if (column == 2) return t.text_address;
        if (column == 0 || column == 3) return t.text_secondary;
    }
    if (role == Qt::FontRole && column == 2)
        return theme::fonts::codeRegular();
    if (role == AddressRole)
        return QVariant::fromValue(item.address);
    if (role == TooltipTextRole) {
        QStringList lines;
        lines << QString::fromStdString(item.name);
        lines << hex0x64(item.address);
        if (item.is_forwarded && !item.forward_name.empty())
            lines << QStringLiteral("Forwarded: ") +
                QString::fromStdString(item.forward_name);
        return lines.join(QStringLiteral("\n"));
    }
    return QVariant();
}

quint64 ModuleExportsModel::idFor(const pe_parser::export_entry_t& item,
                                  int row) const {
    (void)row;
    return item.address != 0 ? item.address
        : (static_cast<quint64>(item.ordinal) << 32) ^
            static_cast<quint64>(item.rva);
}

ModuleImportsModel::ModuleImportsModel(QObject* parent)
    : DebuggerRowsModel({{QStringLiteral("Module"), 180, false},
                         {QStringLiteral("Function"), 0, true},
                         {QStringLiteral("IAT Address"), 170, false},
                         {QStringLiteral("Bound"), 170, false}}, parent) {
}

QVariant ModuleImportsModel::cellFor(const pe_parser::import_entry_t& item,
                                     int row, int column, int role) const {
    (void)row;
    const auto& t = theme::tokens();
    if (role == Qt::DisplayRole) {
        switch (column) {
            case 0: return QString::fromStdString(item.module_name);
            case 1:
                return item.function_name.empty()
                    ? QStringLiteral("#%1").arg(item.ordinal)
                    : QString::fromStdString(item.function_name);
            case 2: return hex0x64(item.iat_address);
            case 3:
                return item.bound_address != 0 ? hex0x64(item.bound_address)
                                               : QVariant();
            default: return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        if (column == 0) return t.text_secondary;
        if (column == 1) return t.text_primary;
        if (column == 2 || column == 3) return t.text_address;
    }
    if (role == Qt::FontRole && column >= 2)
        return theme::fonts::codeRegular();
    if (role == AddressRole)
        return QVariant::fromValue(item.iat_address);
    if (role == TooltipTextRole) {
        const QString function = item.function_name.empty()
            ? QStringLiteral("#%1").arg(item.ordinal)
            : QString::fromStdString(item.function_name);
        return QString::fromStdString(item.module_name) +
            QStringLiteral("!") + function + QStringLiteral("\nIAT: ") +
            hex0x64(item.iat_address);
    }
    return QVariant();
}

quint64 ModuleImportsModel::idFor(const pe_parser::import_entry_t& item,
                                  int row) const {
    (void)row;
    quint64 hash = 1469598103934665603ULL;
    for (const char c : item.module_name) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    for (const char c : item.function_name) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    hash ^= item.iat_address;
    return hash;
}

}

