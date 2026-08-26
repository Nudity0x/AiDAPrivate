#pragma once

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStyledItemDelegate>
#include <QVariant>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/debugger/debugger_engine.hpp"
#include "core/debugger/debugger_interaction_context.hpp"
#include "core/analysis/pe_parser.hpp"
#include "core/runtime/standalone_driver.hpp"

class QItemSelectionModel;
class QTableView;
class QTimer;

namespace seh_view {
struct seh_entry_t;
struct seh_diagnostics_t;
}

namespace aida::qt::debugger {

// Custom model roles (delegate fetch batching via multiData; one row lookup
// fills every requested role). TooltipTextRole is surfaced to the views as
// Qt::ToolTipRole by DebuggerTableModelBase::data/multiData (the delegate's
// helpEvent only queries Qt::ToolTipRole).
enum DebuggerRole {
    AddressRole = Qt::UserRole + 1,
    ValueHexRole,
    FlashRole,
    StateTokenRole,
    StateKindRole,
    TooltipTextRole,
    RowIdRole
};

struct model_column_t {
    QString label;
    int width = 0;
    bool stretch = false;
};

// Base for every flat debugger list model. Fixed-height QTableView consumer;
// no QSortFilterProxyModel anywhere (sort/filter inside the model).
class DebuggerTableModelBase : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit DebuggerTableModelBase(std::vector<model_column_t> columns,
                                    QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override final;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override final;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    QVariant data(const QModelIndex& index, int role) const override final;
    void multiData(const QModelIndex& index,
                   QModelRoleDataSpan roleDataSpan) const override;

    virtual quint64 rowId(int row) const = 0;
    int rowForId(quint64 id) const;

    virtual debugger_interaction::context_t contextForRow(int row) const;

    const std::vector<model_column_t>& columns() const noexcept { return columns_; }

Q_SIGNALS:
    void snapshotApplied(quint64 generation);

protected:
    virtual int implRowCount() const = 0;
    virtual QVariant cellData(int row, int column, int role) const = 0;

private:
    std::vector<model_column_t> columns_;
};

// Selection persistence across resets (IDs, not indices — beginResetModel
// invalidates all persistent indexes).
QSet<quint64> capture_selected_row_ids(const DebuggerTableModelBase& model,
                                       const QItemSelectionModel* selection);
void restore_selected_row_ids(DebuggerTableModelBase& model, QTableView* view,
                              const QSet<quint64>& ids, quint64 focus_id);

// Typed row container with generation-fenced snapshot application.
template <typename T>
class DebuggerRowsModel : public DebuggerTableModelBase {
public:
    using rows_t = std::vector<T>;
    using const_rows_ptr = std::shared_ptr<const rows_t>;

    explicit DebuggerRowsModel(std::vector<model_column_t> columns,
                               QObject* parent = nullptr)
        : DebuggerTableModelBase(std::move(columns), parent),
          rows_(std::make_shared<const rows_t>()) {}

    const_rows_ptr rows() const noexcept { return rows_; }
    quint64 generation() const noexcept { return generation_; }
    const T* rowAt(int row) const {
        return (row >= 0 && row < static_cast<int>(rows_->size()))
            ? &(*rows_)[static_cast<std::size_t>(row)] : nullptr;
    }

    void applySnapshot(const_rows_ptr rows, quint64 generation) {
        if (!rows)
            return;
        if (generation == generation_ && rows == rows_)
            return;
        beginResetModel();
        rows_ = std::move(rows);
        generation_ = generation;
        rowsReplaced();
        endResetModel();
        Q_EMIT snapshotApplied(generation_);
    }

    quint64 rowId(int row) const override {
        const T* item = rowAt(row);
        return item ? idFor(*item, row) : 0;
    }

protected:
    virtual void rowsReplaced() {}

protected:
    int implRowCount() const override {
        return static_cast<int>(rows_->size());
    }
    QVariant cellData(int row, int column, int role) const override {
        const T* item = rowAt(row);
        return item ? cellFor(*item, row, column, role) : QVariant();
    }

    virtual QVariant cellFor(const T& item, int row, int column,
                             int role) const = 0;
    virtual quint64 idFor(const T& item, int row) const = 0;

private:
    const_rows_ptr rows_;
    quint64 generation_ = 0;
};

// Decaying change-flash map (ports cpu_reg_flash/prev_reg decay
// exp(-3*dt), threshold 0.005; thread_state_flash/prev_thread_state).
// Self-stopping 16ms PreciseTimer; emits dataChanged over the value columns.
class FlashMap : public QObject {
    Q_OBJECT
public:
    explicit FlashMap(DebuggerTableModelBase* model,
                      const std::vector<int>& columns);

    void mark(int row);
    qreal value(int row) const;
    void clear();

private Q_SLOTS:
    void decayTick();

private:
    DebuggerTableModelBase* model_ = nullptr;
    std::vector<int> columns_;
    QHash<int, qreal> flash_;
    QTimer* timer_ = nullptr;
    QElapsedTimer elapsed_;
    qint64 last_tick_ns_ = 0;
};

// State-pill delegate: draws the StateTokenRole text inside a rounded pill
// colored by StateKindRole ("success"/"warning"/"error"/"info"/"accent"/
// "neutral"/"dim"). Used by the breakpoints State column, the threads State
// column, and the trace REC badge cell.
class StatePillDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit StatePillDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
};

class RegistersModel : public DebuggerTableModelBase {
    Q_OBJECT
public:
    enum RegisterGroup { GroupGeneral = 0, GroupSegment = 1, GroupDebug = 2 };

    explicit RegistersModel(QObject* parent = nullptr);

    void applyRegisters(const debugger_engine::register_set_t& registers,
                        quint32 pid, quint32 active_tid);
    const debugger_engine::register_set_t& registers() const noexcept {
        return registers_;
    }
    static int registerRowCount() noexcept { return 30; }
    static const char* registerName(int row) noexcept;
    static bool registerEditable(int row) noexcept;
    static quint64 registerValue(const debugger_engine::register_set_t& regs,
                                 int row) noexcept;

    quint64 rowId(int row) const override;
    debugger_interaction::context_t contextForRow(int row) const override;
    FlashMap* flash() const noexcept { return flash_; }

protected:
    int implRowCount() const override;
    QVariant cellData(int row, int column, int role) const override;

private:
    debugger_engine::register_set_t registers_{};
    bool prev_initialized_ = false;
    FlashMap* flash_ = nullptr;
};

class BreakpointsModel : public DebuggerRowsModel<debugger_engine::breakpoint_t> {
    Q_OBJECT
public:
    enum Column { State = 0, Address, Type, Hits, Name, Condition, ColumnCount };

    explicit BreakpointsModel(QObject* parent = nullptr);

    debugger_interaction::context_t contextForRow(int row) const override;

protected:
    QVariant cellFor(const debugger_engine::breakpoint_t& item, int row,
                     int column, int role) const override;
    quint64 idFor(const debugger_engine::breakpoint_t& item,
                  int row) const override;
};

class CallStackModel : public DebuggerRowsModel<debugger_engine::stack_frame_t> {
    Q_OBJECT
public:
    enum Column { Frame = 0, Symbol, Address, ReturnAddress, ColumnCount };

    explicit CallStackModel(QObject* parent = nullptr);

    debugger_interaction::context_t contextForRow(int row) const override;

protected:
    QVariant cellFor(const debugger_engine::stack_frame_t& item, int row,
                     int column, int role) const override;
    quint64 idFor(const debugger_engine::stack_frame_t& item,
                  int row) const override;
};

class ThreadsModel : public DebuggerRowsModel<debugger_engine::cached_thread_t> {
    Q_OBJECT
public:
    enum Column { Tid = 0, Priority, State, Rip, ColumnCount };

    explicit ThreadsModel(QObject* parent = nullptr);

    void applyThreads(const_rows_ptr rows, quint64 generation);
    debugger_interaction::context_t contextForRow(int row) const override;
    FlashMap* flash() const noexcept { return flash_; }

protected:
    QVariant cellFor(const debugger_engine::cached_thread_t& item, int row,
                     int column, int role) const override;
    quint64 idFor(const debugger_engine::cached_thread_t& item,
                  int row) const override;

private:
    FlashMap* flash_ = nullptr;
    QHash<quint32, quint32> prev_state_;
};

class WatchesModel : public DebuggerRowsModel<debugger_engine::watch_entry_t> {
    Q_OBJECT
public:
    enum Column { Expression = 0, ResolvedAddress, Value, ColumnCount };

    explicit WatchesModel(QObject* parent = nullptr);

    void setRegisters(const debugger_engine::register_set_t& registers);
    debugger_interaction::context_t contextForRow(int row) const override;

protected:
    QVariant cellFor(const debugger_engine::watch_entry_t& item, int row,
                     int column, int role) const override;
    quint64 idFor(const debugger_engine::watch_entry_t& item,
                  int row) const override;

private:
    debugger_engine::register_set_t registers_{};
};

class HandlesModel : public DebuggerRowsModel<debugger_engine::handle_info_t> {
    Q_OBJECT
public:
    enum Column { Handle = 0, Type, Access, Name, ColumnCount };

    explicit HandlesModel(QObject* parent = nullptr);

    debugger_interaction::context_t contextForRow(int row) const override;

protected:
    QVariant cellFor(const debugger_engine::handle_info_t& item, int row,
                     int column, int role) const override;
    quint64 idFor(const debugger_engine::handle_info_t& item,
                  int row) const override;
};

class TraceModel : public DebuggerRowsModel<debugger_engine::trace_record_t> {
    Q_OBJECT
public:
    enum Column { Index = 0, Address, Instruction, ColumnCount };

    explicit TraceModel(QObject* parent = nullptr);

    void setFilter(const QString& filter);
    QString filter() const { return filter_; }
    void applyTrace(const_rows_ptr rows, quint64 generation);

    debugger_interaction::context_t contextForRow(int row) const override;

protected:
    QVariant cellFor(const debugger_engine::trace_record_t& item, int row,
                     int column, int role) const override;
    quint64 idFor(const debugger_engine::trace_record_t& item,
                  int row) const override;
    int implRowCount() const override;
    void rowsReplaced() override;

private:
    void rebuildFilter();

    QString filter_;
    std::vector<int> filtered_;
};

class StringsModel : public DebuggerRowsModel<debugger_engine::string_ref_t> {
    Q_OBJECT
public:
    enum Column { Address = 0, String, Module, ColumnCount };

    explicit StringsModel(QObject* parent = nullptr);

    void setFilter(const QString& filter);
    void applyStrings(const_rows_ptr rows, quint64 generation);

    debugger_interaction::context_t contextForRow(int row) const override;

protected:
    QVariant cellFor(const debugger_engine::string_ref_t& item, int row,
                     int column, int role) const override;
    quint64 idFor(const debugger_engine::string_ref_t& item,
                  int row) const override;
    int implRowCount() const override;
    void rowsReplaced() override;

private:
    void rebuildFilter();

    QString filter_;
    std::vector<int> filtered_;
};

class BookmarksModel : public DebuggerTableModelBase {
    Q_OBJECT
public:
    enum Column { Index = 0, Address, Label, ColumnCount };

    explicit BookmarksModel(QObject* parent = nullptr);

    void applyBookmarks(std::vector<std::uint64_t> bookmarks,
                        std::map<std::uint64_t, std::string> labels);
    quint64 rowId(int row) const override;
    debugger_interaction::context_t contextForRow(int row) const override;

protected:
    int implRowCount() const override;
    QVariant cellData(int row, int column, int role) const override;

private:
    std::vector<std::uint64_t> bookmarks_;
    std::map<std::uint64_t, std::string> labels_;
};

class ModulesModel : public DebuggerRowsModel<driver_bridge::module_info_t> {
    Q_OBJECT
public:
    enum Column { Name = 0, Base, Size, Path, ColumnCount };

    explicit ModulesModel(QObject* parent = nullptr);

    void setFilter(const QString& filter);
    void applyModules(const_rows_ptr rows, quint64 generation);

    debugger_interaction::context_t contextForRow(int row) const override;

protected:
    QVariant cellFor(const driver_bridge::module_info_t& item, int row,
                     int column, int role) const override;
    quint64 idFor(const driver_bridge::module_info_t& item,
                  int row) const override;
    int implRowCount() const override;
    void rowsReplaced() override;

private:
    void rebuildFilter();

    QString filter_;
    std::vector<int> filtered_;
};

class PatchesModel : public DebuggerTableModelBase {
    Q_OBJECT
public:
    enum Column { State = 0, Address, Original, Patched, Description, ColumnCount };

    explicit PatchesModel(QObject* parent = nullptr);

    struct row_t {
        std::uint64_t address = 0;
        std::size_t patched_size = 0;
        QString original;
        QString patched;
        QString description;
        bool active = false;
    };

    void applyRows(std::vector<row_t> rows, quint64 publication_generation,
                   std::size_t total_count);
    quint64 publicationGeneration() const noexcept { return generation_; }
    std::size_t totalCount() const noexcept { return total_count_; }
    const row_t* patchRow(int row) const;

    quint64 rowId(int row) const override;
    debugger_interaction::context_t contextForRow(int row) const override;

protected:
    int implRowCount() const override;
    QVariant cellData(int row, int column, int role) const override;

private:
    std::vector<row_t> rows_;
    quint64 generation_ = 0;
    std::size_t total_count_ = 0;
};

class SehModel : public DebuggerTableModelBase {
    Q_OBJECT
public:
    enum Column { Index = 0, Frame, Handler, Module, ColumnCount };

    explicit SehModel(QObject* parent = nullptr);

    void applySeh(std::shared_ptr<const std::vector<seh_view::seh_entry_t>> entries,
                  quint64 generation);
    quint64 rowId(int row) const override;
    debugger_interaction::context_t contextForRow(int row) const override;

protected:
    int implRowCount() const override;
    QVariant cellData(int row, int column, int role) const override;

private:
    std::shared_ptr<const std::vector<seh_view::seh_entry_t>> entries_;
    quint64 generation_ = 0;
};

class MemoryRegionsModel : public DebuggerRowsModel<debugger_engine::memory_region_t> {    Q_OBJECT
public:
    enum Column { Address = 0, Size, Protect, State, Type, Module, Info, ColumnCount };

    explicit MemoryRegionsModel(QObject* parent = nullptr);

    void setFilter(const QString& filter);
    QString filter() const { return filter_; }
    void applyRegions(const_rows_ptr rows, quint64 generation);

    quint64 committedBytes() const noexcept { return committed_bytes_; }
    int rwxCount() const noexcept { return rwx_count_; }

    debugger_interaction::context_t contextForRow(int row) const override;

protected:
    QVariant cellFor(const debugger_engine::memory_region_t& item, int row,
                     int column, int role) const override;
    quint64 idFor(const debugger_engine::memory_region_t& item,
                  int row) const override;
    int implRowCount() const override;
    void rowsReplaced() override;

private:
    void rebuildFilter();

    QString filter_;
    std::vector<int> filtered_;
    quint64 committed_bytes_ = 0;
    int rwx_count_ = 0;
};

class ModuleExportsModel
    : public DebuggerRowsModel<pe_parser::export_entry_t> {
    Q_OBJECT
public:
    explicit ModuleExportsModel(QObject* parent = nullptr);

protected:
    QVariant cellFor(const pe_parser::export_entry_t& item, int row,
                     int column, int role) const override;
    quint64 idFor(const pe_parser::export_entry_t& item,
                  int row) const override;
};

class ModuleImportsModel
    : public DebuggerRowsModel<pe_parser::import_entry_t> {
    Q_OBJECT
public:
    explicit ModuleImportsModel(QObject* parent = nullptr);

protected:
    QVariant cellFor(const pe_parser::import_entry_t& item, int row,
                     int column, int role) const override;
    quint64 idFor(const pe_parser::import_entry_t& item,
                  int row) const override;
};

}
