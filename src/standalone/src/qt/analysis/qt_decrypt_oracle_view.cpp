#include "qt/analysis/qt_decrypt_oracle_view.hpp"

#include <QAbstractTableModel>
#include <QApplication>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/disasm/disasm_view.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::analysis {

namespace {

class QtDecryptOracleModel : public QAbstractTableModel {
public:
    enum class Column : int {
        source_func = 0,
        enc_offset = 1,
        string = 2,
        conf = 3,
        len = 4,
        column_count = 5
    };

    explicit QtDecryptOracleModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setResults(
        std::shared_ptr<const std::vector<decrypt_oracle::decrypted_string_t>> results) {
        beginResetModel();
        results_ = std::move(results);
        endResetModel();
    }

    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() || !results_ ? 0 : static_cast<int>(results_->size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
    }

    const decrypt_oracle::decrypted_string_t* rowAt(int row) const noexcept {
        if (!results_ || row < 0 || static_cast<std::size_t>(row) >= results_->size())
            return nullptr;
        return &(*results_)[static_cast<std::size_t>(row)];
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.parent().isValid()) return {};
        const auto* row = rowAt(index.row());
        if (!row) return {};
        const auto& tokens = theme::tokens();
        if (role == Qt::DisplayRole) {
            switch (static_cast<Column>(index.column())) {
            case Column::source_func: {
                char buf[24]{};
                std::snprintf(buf, sizeof(buf), "0x%llX",
                    static_cast<unsigned long long>(row->source_function));
                return QString::fromLatin1(buf);
            }
            case Column::enc_offset: {
                char buf[24]{};
                std::snprintf(buf, sizeof(buf), "0x%llX",
                    static_cast<unsigned long long>(row->encrypted_offset));
                return QString::fromLatin1(buf);
            }
            case Column::string: {
                std::string display = row->decrypted;
                if (display.size() > 80) display = display.substr(0, 77) + "...";
                return QString::fromStdString(display);
            }
            case Column::conf:
                return QStringLiteral("%1%").arg(
                    static_cast<int>(row->confidence * 100.f));
            case Column::len: return QString::number(row->length);
            default: return {};
            }
        }
        if (role == Qt::UserRole) return row->confidence;
        if (role == Qt::ToolTipRole)
            return QString::fromStdString(row->decrypted);
        if (role == Qt::ForegroundRole) {
            switch (static_cast<Column>(index.column())) {
            case Column::source_func: return tokens.syn_address;
            case Column::enc_offset: return tokens.text_dim;
            case Column::string: return tokens.success;
            case Column::conf:
            case Column::len: return tokens.text_dim;
            default: return {};
            }
        }
        if (role == Qt::FontRole) {
            switch (static_cast<Column>(index.column())) {
            case Column::source_func:
            case Column::enc_offset:
            case Column::len:
                return aida::qt::theme::fonts::codeRegular();
            default: return {};
            }
        }
        return {};
    }

    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override {
        for (QModelRoleData& roleData : span) {
            switch (roleData.role()) {
            case Qt::DisplayRole:
            case Qt::ForegroundRole:
            case Qt::FontRole:
            case Qt::UserRole:
            case Qt::ToolTipRole:
                roleData.setData(data(index, roleData.role()));
                break;
            default:
                roleData.clearData();
                break;
            }
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (static_cast<Column>(section)) {
        case Column::source_func: return QStringLiteral("Source Func");
        case Column::enc_offset: return QStringLiteral("Enc Offset");
        case Column::string: return QStringLiteral("Decrypted String");
        case Column::conf: return QStringLiteral("Conf");
        case Column::len: return QStringLiteral("Len");
        default: return {};
        }
    }

private:
    std::shared_ptr<const std::vector<decrypt_oracle::decrypted_string_t>> results_;
};

// Confidence cell: 7 px fill bar + percentage text (07 sec. 7.2, delegate draws
// directly; owner rows skip CE_ItemViewItem per Q4).
class QtDecryptConfidenceDelegate : public QStyledItemDelegate {
public:
    explicit QtDecryptConfidenceDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        const float confidence = index.data(Qt::UserRole).toFloat();
        const auto& tokens = theme::tokens();
        const QRect cell = option.rect.adjusted(tokens.spacing.xs, 0,
            -tokens.spacing.xs, 0);
        const int bar_h = tokens.spacing.sm;
        const qreal bar_radius = bar_h * 0.5;
        const int bar_w = cell.width() / 2;
        const QRect bar(cell.left(), cell.center().y() - bar_h / 2, bar_w, bar_h);
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::tokens().panel_header);
        painter->drawRoundedRect(bar, bar_radius, bar_radius);
        QColor fill = confidence > 0.75f ? tokens.success
            : confidence > 0.5f ? tokens.warning : tokens.error;
        painter->setBrush(fill);
        painter->drawRoundedRect(
            QRectF(bar.left(), bar.top(), bar_w * confidence, bar_h),
            bar_radius, bar_radius);
        painter->setPen(tokens.text_dim);
        painter->drawText(QRect(bar.right() + tokens.spacing.xs, cell.top(),
                cell.right() - bar.right() - tokens.spacing.xs, cell.height()),
            Qt::AlignVCenter | Qt::AlignLeft,
            QStringLiteral("%1%").arg(static_cast<int>(confidence * 100.f)));
        painter->restore();
    }
};

}

QtDecryptOracleView::QtDecryptOracleView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.memory.decrypt"));
    const auto& tokens = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("aida.decrypt.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    const QFontMetricsF ui_metrics(font());
    const int address_width = static_cast<int>(ui_metrics.horizontalAdvance(
        QStringLiteral("0xDDDDDDDDDDDDDDDD"))) + 2 * tokens.table.cell_pad_x +
        tokens.spacing.lg;
    const int size_width = static_cast<int>(ui_metrics.horizontalAdvance(
        QStringLiteral("4294967295"))) + 2 * tokens.table.cell_pad_x +
        tokens.spacing.lg;
    address_edit_ = new widgets::AidaLineEdit(
        QStringLiteral("Encrypted region (hex)"), toolbar);
    address_edit_->setObjectName(QStringLiteral("aida.decrypt.address"));
    address_edit_->setMinimumWidth(address_width);
    address_edit_->setToolTip(QStringLiteral(
        "Base address of the encrypted region, in hexadecimal"));
    const auto& engine = decrypt_oracle::g_state;
    address_edit_->setText(QString::fromLatin1(engine.address_input));
    toolbar_layout->addWidget(address_edit_);
    size_edit_ = new widgets::AidaLineEdit(QStringLiteral("Size"), toolbar);
    size_edit_->setObjectName(QStringLiteral("aida.decrypt.size"));
    size_edit_->setMinimumWidth(size_width);
    size_edit_->setToolTip(QStringLiteral(
        "Number of bytes to scan, decimal or 0x-prefixed hexadecimal"));
    size_edit_->setText(QString::fromLatin1(engine.size_input));
    toolbar_layout->addWidget(size_edit_);
    auto* scan = new QPushButton(QStringLiteral("Scan & Decrypt"), toolbar);
    scan->setObjectName(QStringLiteral("aida.decrypt.scan"));
    scan->setToolTip(QStringLiteral(
        "Scan the region's xref feeders and run the decrypt oracle"));
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), toolbar);
    cancel->setObjectName(QStringLiteral("aida.decrypt.cancel"));
    cancel->setToolTip(QStringLiteral("Cancel the active scan"));
    toolbar_layout->addWidget(scan);
    toolbar_layout->addWidget(cancel);
    toolbar_layout->addStretch(1);
    layout->addWidget(toolbar);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("aida.decrypt.progress"));
    progress_->setFormat(QStringLiteral("%v / %m xrefs"));
    progress_->setVisible(false);
    layout->addWidget(progress_);

    auto* stats = new QWidget(this);
    stats->setObjectName(QStringLiteral("aida.decrypt.stats"));
    auto* stats_layout = new QHBoxLayout(stats);
    stats_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    stats_layout->setSpacing(tokens.spacing.lg);
    const auto make_stat = [this, stats, stats_layout](const QString& label,
                                                       const char* object_name) {
        auto* name = new QLabel(label, stats);
        name->setObjectName(QStringLiteral("aida.decrypt.stat.name.") +
            QString::fromLatin1(object_name));
        name->setProperty("aidaVariant", QStringLiteral("secondary"));
        auto* value = new QLabel(QStringLiteral("0"), stats);
        value->setObjectName(QStringLiteral("aida.decrypt.stat.value.") +
            QString::fromLatin1(object_name));
        stats_layout->addWidget(name);
        stats_layout->addWidget(value);
        return value;
    };
    stat_found_ = make_stat(QStringLiteral("Found"), "found");
    stat_avg_conf_ = make_stat(QStringLiteral("Avg Conf"), "avg_conf");
    stat_high_conf_ = make_stat(QStringLiteral("High Conf"), "high_conf");
    stat_total_len_ = make_stat(QStringLiteral("Total Len"), "total_len");
    stats_layout->addStretch(1);
    layout->addWidget(stats);

    status_label_ = new QLabel(this);
    status_label_->setObjectName(QStringLiteral("aida.decrypt.status"));
    status_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(status_label_);

    auto* model = new QtDecryptOracleModel(this);
    model_ = model;
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setObjectName(QStringLiteral("aida.decrypt.table"));
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(tokens.table.row_h);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = table_->horizontalHeader();
    using Column = QtDecryptOracleModel::Column;
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const int hex_column_w = static_cast<int>(code_metrics.horizontalAdvance(
        QStringLiteral("0xDDDDDDDDDDDDDDDD"))) + 2 * tokens.table.cell_pad_x +
        tokens.spacing.xs;
    horizontal->setSectionResizeMode(static_cast<int>(Column::source_func),
        QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::source_func), hex_column_w);
    horizontal->setSectionResizeMode(static_cast<int>(Column::enc_offset),
        QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::enc_offset), hex_column_w);
    horizontal->setSectionResizeMode(static_cast<int>(Column::string),
        QHeaderView::Stretch);
    horizontal->setSectionResizeMode(static_cast<int>(Column::conf), QHeaderView::Fixed);
    const int conf_w = 2 * tokens.table.cell_pad_x + tokens.spacing.xs +
        (std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Conf"))),
            static_cast<int>(ui_metrics.horizontalAdvance(QStringLiteral("100%"))) +
                tokens.control.height_lg + tokens.spacing.xs);
    table_->setColumnWidth(static_cast<int>(Column::conf), conf_w);
    table_->setItemDelegateForColumn(static_cast<int>(Column::conf),
        new QtDecryptConfidenceDelegate(this));
    horizontal->setSectionResizeMode(static_cast<int>(Column::len), QHeaderView::Fixed);
    table_->setColumnWidth(static_cast<int>(Column::len),
        (std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Len"))),
            static_cast<int>(code_metrics.horizontalAdvance(
                QStringLiteral("4096000")))) + 2 * tokens.table.cell_pad_x +
            tokens.spacing.xs);
    horizontal->setStretchLastSection(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);

    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.decrypt.state_view"));
    state_view_->setVisible(false);
    layout->addWidget(state_view_, 1);
    layout->addWidget(table_, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(66);
    connect(timer_, &QTimer::timeout, this, [this] { pollEngine(); });

    connect(scan, &QPushButton::clicked, this, [this] {
        auto& oracle = decrypt_oracle::g_state;
        const auto address_text = address_edit_->text().toStdString();
        const auto size_text = size_edit_->text().toStdString();
        std::strncpy(oracle.address_input, address_text.c_str(),
            sizeof(oracle.address_input) - 1);
        std::strncpy(oracle.size_input, size_text.c_str(),
            sizeof(oracle.size_input) - 1);
        std::uint64_t addr = 0;
        std::uint64_t size = 4096;
        if (oracle.address_input[0])
            addr = std::strtoull(oracle.address_input, nullptr, 16);
        if (oracle.size_input[0])
            size = std::strtoull(oracle.size_input, nullptr, 0);
        if (addr != 0)
            decrypt_oracle::scan_and_decrypt(addr, size);
        pollEngine();
    });
    connect(cancel, &QPushButton::clicked, this, [] {
        decrypt_oracle::g_state.cancel.store(true);
    });
    connect(table_, &QTableView::activated, this, [this](const QModelIndex& index) {
        auto* model = static_cast<QtDecryptOracleModel*>(model_);
        const auto* row = model ? model->rowAt(index.row()) : nullptr;
        if (!row) return;
        const auto context = disasm_view::capture_selected_workspace();
        QtAnalysisBridge::instance().navigateTo(context.workspace,
            row->source_function, "document.disassembly");
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
        selected_row_ = current.isValid() ? current.row() : -1;
    });
    table_->installEventFilter(this);

    refreshPresentation();
}

void QtDecryptOracleView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    timer_->start();
}

void QtDecryptOracleView::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

void QtDecryptOracleView::pollEngine() {
    auto& oracle = decrypt_oracle::g_state;
    const bool scanning = oracle.scanning.load();
    const auto results = decrypt_oracle::capture_results();
    const bool results_changed = results != summary_source_;
    if (results_changed)
        updateSummary(results);
    auto* model = static_cast<QtDecryptOracleModel*>(model_);
    if (model && results && results_changed)
        model->setResults(results);
    if (scanning) {
        const int done = oracle.processed_xrefs.load();
        const int total = oracle.total_xrefs.load();
        progress_->setVisible(true);
        progress_->setRange(0, (std::max)(total, 0));
        progress_->setValue((std::min)(done, (std::max)(total, 0)));
        progress_->setFormat(QStringLiteral("%1 / %2 xrefs   %3%")
            .arg(done).arg(total)
            .arg(static_cast<int>(oracle.progress.load() * 100.f)));
    } else {
        progress_->setVisible(false);
        std::string status;
        {
            std::lock_guard<std::mutex> lock(oracle.mutex);
            status = oracle.status_text;
        }
        status_label_->setText(QString::fromStdString(status));
    }
    refreshPresentation();
}

void QtDecryptOracleView::updateSummary(
    const std::shared_ptr<const std::vector<decrypt_oracle::decrypted_string_t>>&
        results) {
    summary_source_ = results;
    int found = 0;
    int strong = 0;
    int total_length = 0;
    double average = 0.0;
    for (const auto& result : *results) {
        average += result.confidence;
        if (result.confidence >= 0.9f) ++strong;
        total_length += result.length;
    }
    found = static_cast<int>(results->size());
    if (found > 0) average /= static_cast<double>(found);
    stat_found_->setText(QString::number(found));
    stat_avg_conf_->setText(QStringLiteral("%1%").arg(
        static_cast<int>(average * 100.0)));
    stat_high_conf_->setText(QString::number(strong));
    stat_total_len_->setText(QStringLiteral("%1 B").arg(total_length));
}

void QtDecryptOracleView::copySelectedString() {
    auto* model = static_cast<QtDecryptOracleModel*>(model_);
    const auto index = table_->currentIndex();
    const auto* row = model && index.isValid() ? model->rowAt(index.row()) : nullptr;
    if (!row) return;
    clipboard::set_text(QString::fromStdString(row->decrypted));
}

bool QtDecryptOracleView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers().testFlag(Qt::ControlModifier) &&
            (key->key() == Qt::Key_C || key->key() == Qt::Key_Insert)) {
            copySelectedString();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QtDecryptOracleView::refreshPresentation() {
    const bool scanning = decrypt_oracle::g_state.scanning.load();
    const bool empty = !summary_source_ || summary_source_->empty();
    if (empty && !scanning) {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No decrypted strings"));
        state_view_->setMessage(QStringLiteral(
            "Enter an encrypted region address and click Scan & Decrypt to recover strings."));
        state_view_->setVisible(true);
        table_->setVisible(false);
        return;
    }
    state_view_->setVisible(false);
    table_->setVisible(true);
}

}
