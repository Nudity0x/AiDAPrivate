#include "qt/analysis/qt_stealth_view.hpp"

#include <QAbstractTableModel>
#include <QComboBox>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QStackedLayout>
#include <QTabBar>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "core/disasm/disasm_view.hpp"
#include "core/runtime/standalone_driver.hpp"
#include "core/ui/context_menu_contract.hpp"
#include "core/ui/shell_host_contract.hpp"
#include "helpers/diag_log.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::analysis {

namespace {

class QtStealthFindingsModel : public QAbstractTableModel {
public:
    enum class Column : int {
        severity = 0,
        category = 1,
        address = 2,
        finding = 3,
        details = 4,
        column_count = 5
    };

    explicit QtStealthFindingsModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setContent(std::shared_ptr<const std::vector<stealth_engine::finding_t>> findings,
                    std::vector<std::size_t> filtered) {
        beginResetModel();
        findings_ = std::move(findings);
        filtered_ = std::move(filtered);
        endResetModel();
    }

    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(filtered_.size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
    }

    const stealth_engine::finding_t* rowAt(int view_row,
                                           std::size_t* source_index) const noexcept {
        if (!findings_ || view_row < 0 ||
            static_cast<std::size_t>(view_row) >= filtered_.size())
            return nullptr;
        const std::size_t source = filtered_[static_cast<std::size_t>(view_row)];
        if (source >= findings_->size()) return nullptr;
        if (source_index) *source_index = source;
        return &(*findings_)[source];
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.parent().isValid()) return {};
        const auto* finding = rowAt(index.row(), nullptr);
        if (!finding) return {};
        const auto& tokens = theme::tokens();
        if (role == Qt::DisplayRole) {
            switch (static_cast<Column>(index.column())) {
            case Column::severity:
                return QString::fromLatin1(
                    stealth_engine::severity_name(finding->severity));
            case Column::category:
                return QString::fromLatin1(
                    stealth_engine::category_name(finding->category));
            case Column::address:
                if (finding->address == 0) return QStringLiteral("-");
                return QStringLiteral("0x%1").arg(finding->address, 0, 16);
            case Column::finding: return QString::fromStdString(finding->title);
            case Column::details: return QString::fromStdString(finding->detail);
            default: return {};
            }
        }
        if (role == Qt::ForegroundRole) {
            if (index.column() == static_cast<int>(Column::severity)) {
                switch (finding->severity) {
                case stealth_engine::finding_severity_t::critical: return tokens.error;
                case stealth_engine::finding_severity_t::high: return tokens.warning;
                case stealth_engine::finding_severity_t::medium: return tokens.warning;
                case stealth_engine::finding_severity_t::low: return tokens.success;
                case stealth_engine::finding_severity_t::info: return tokens.info;
                }
                return tokens.text_dim;
            }
            if (index.column() == static_cast<int>(Column::address))
                return tokens.syn_address;
            return {};
        }
        if (role == Qt::ToolTipRole && !finding->detail.empty())
            return QString::fromStdString(finding->detail);
        return {};
    }

    void multiData(const QModelIndex& index, QModelRoleDataSpan span) const override {
        for (QModelRoleData& roleData : span) {
            switch (roleData.role()) {
            case Qt::DisplayRole:
            case Qt::ForegroundRole:
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
        case Column::severity: return QStringLiteral("Severity");
        case Column::category: return QStringLiteral("Category");
        case Column::address: return QStringLiteral("Address");
        case Column::finding: return QStringLiteral("Finding");
        case Column::details: return QStringLiteral("Details");
        default: return {};
        }
    }

private:
    std::shared_ptr<const std::vector<stealth_engine::finding_t>> findings_;
    std::vector<std::size_t> filtered_;
};

class QtStealthHooksModel : public QAbstractTableModel {
public:
    enum class Column : int {
        target = 0,
        trampoline = 1,
        size = 2,
        peb = 3,
        active = 4,
        column_count = 5
    };

    explicit QtStealthHooksModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setHooks(std::vector<stealth_engine::hook_entry_t> hooks, bool peb_spoofed) {
        beginResetModel();
        hooks_ = std::move(hooks);
        peb_spoofed_ = peb_spoofed;
        endResetModel();
    }

    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(hooks_.size());
    }
    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(Column::column_count);
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.parent().isValid()) return {};
        if (index.row() < 0 || static_cast<std::size_t>(index.row()) >= hooks_.size())
            return {};
        const auto& hook = hooks_[static_cast<std::size_t>(index.row())];
        const auto& tokens = theme::tokens();
        if (role == Qt::DisplayRole) {
            switch (static_cast<Column>(index.column())) {
            case Column::target:
                return QStringLiteral("0x%1").arg(hook.target_addr, 0, 16);
            case Column::trampoline:
                return QStringLiteral("0x%1").arg(hook.trampoline_addr, 0, 16);
            case Column::size: return QString::number(hook.hook_size);
            case Column::peb: return peb_spoofed_ ? QStringLiteral("yes")
                : QStringLiteral("no");
            case Column::active: return hook.active ? QStringLiteral("active")
                : QStringLiteral("removed");
            default: return {};
            }
        }
        if (role == Qt::ForegroundRole) {
            switch (static_cast<Column>(index.column())) {
            case Column::target: return tokens.text_address;
            case Column::trampoline: return tokens.text_dim;
            case Column::peb:
                return peb_spoofed_ ? tokens.success : tokens.error;
            case Column::active:
                return hook.active ? tokens.success : tokens.error;
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
        case Column::target: return QStringLiteral("Target Address");
        case Column::trampoline: return QStringLiteral("Trampoline");
        case Column::size: return QStringLiteral("Size");
        case Column::peb: return QStringLiteral("PEB");
        case Column::active: return QStringLiteral("Active");
        default: return {};
        }
    }

private:
    std::vector<stealth_engine::hook_entry_t> hooks_;
    bool peb_spoofed_ = false;
};

}

QtStealthView::QtStealthView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.analysis.protection"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("aida.stealth.tabs"));
    tabs_->addTab(QStringLiteral("Protection Scan"));
    tabs_->addTab(QStringLiteral("Stealth Status"));
    layout->addWidget(tabs_);
    auto* stack_host = new QWidget(this);
    stack_ = new QStackedLayout(stack_host);
    stack_->setStackingMode(QStackedLayout::StackOne);
    stack_->addWidget(buildScanPage());
    stack_->addWidget(buildStatusPage());
    layout->addWidget(stack_host, 1);
    connect(tabs_, &QTabBar::currentChanged, stack_,
        &QStackedLayout::setCurrentIndex);
    connect(tabs_, &QTabBar::currentChanged, this, [](int index) {
        QtAnalysisBridge::instance().noteStealthTabFromWidget(index);
    });

    timer_ = new QTimer(this);
    timer_->setInterval(66);
    connect(timer_, &QTimer::timeout, this, [this] {
        pollScan();
        pollStatus();
    });

    QtAnalysisBridge::instance().registerStealthView(this);
    const int pending = QtAnalysisBridge::instance().stealthTab();
    if (pending > 0 && pending < tabs_->count()) setInnerTab(pending);
}

int QtStealthView::innerTab() const noexcept {
    return tabs_ ? tabs_->currentIndex() : 0;
}

void QtStealthView::setInnerTab(int index) {
    if (!tabs_ || !stack_) return;
    if (index < 0 || index >= tabs_->count()) return;
    if (tabs_->currentIndex() == index) return;
    tabs_->setCurrentIndex(index);
}

QWidget* QtStealthView::buildScanPage() {
    const auto& tokens = theme::tokens();
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new QWidget(page);
    toolbar->setObjectName(QStringLiteral("aida.stealth.toolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    toolbar_layout->setSpacing(tokens.toolbar.group_gap);
    auto* scan = new QToolButton(toolbar);
    scan->setObjectName(QStringLiteral("aida.stealth.scan"));
    scan->setText(QStringLiteral("Scan"));
    scan->setToolTip(QStringLiteral(
        "Scan the attached process for protection mechanisms"));
    auto* stop = new QToolButton(toolbar);
    stop->setObjectName(QStringLiteral("aida.stealth.stop"));
    stop->setText(QStringLiteral("Stop"));
    stop->setToolTip(QStringLiteral(
        "Request cancellation of the active protection scan"));
    auto* clear = new QToolButton(toolbar);
    clear->setObjectName(QStringLiteral("aida.stealth.clear"));
    clear->setText(QStringLiteral("Clear"));
    clear->setToolTip(QStringLiteral("Clear the retained protection findings"));
    toolbar_layout->addWidget(scan);
    toolbar_layout->addWidget(stop);
    toolbar_layout->addWidget(clear);
    severity_combo_ = new QComboBox(toolbar);
    severity_combo_->setObjectName(QStringLiteral("aida.stealth.severity"));
    severity_combo_->addItems({QStringLiteral("All severities"),
        QStringLiteral("Critical"), QStringLiteral("High"), QStringLiteral("Medium"),
        QStringLiteral("Low"), QStringLiteral("Info")});
    severity_combo_->setToolTip(QStringLiteral("Filter findings by severity"));
    toolbar_layout->addWidget(severity_combo_);
    category_combo_ = new QComboBox(toolbar);
    category_combo_->setObjectName(QStringLiteral("aida.stealth.category"));
    category_combo_->addItems({QStringLiteral("All categories"),
        QStringLiteral("AC Driver"), QStringLiteral("Memory Guard"),
        QStringLiteral("Suspicious Module"), QStringLiteral("Thread"),
        QStringLiteral("Debug State"), QStringLiteral("Hook"),
        QStringLiteral("WFP Callback")});
    category_combo_->setToolTip(QStringLiteral("Filter findings by category"));
    toolbar_layout->addWidget(category_combo_);
    toolbar_layout->addStretch(1);
    layout->addWidget(toolbar);

    scan_progress_ = new QProgressBar(page);
    scan_progress_->setObjectName(QStringLiteral("aida.stealth.progress"));
    scan_progress_->setVisible(false);
    layout->addWidget(scan_progress_);
    scan_status_ = new QLabel(page);
    scan_status_->setObjectName(QStringLiteral("aida.stealth.scan_status"));
    scan_status_->setProperty("aidaVariant", QStringLiteral("secondary"));
    scan_status_->setVisible(false);
    layout->addWidget(scan_status_);

    auto* model = new QtStealthFindingsModel(page);
    findings_model_ = model;
    findings_table_ = new QTableView(page);
    findings_table_->setModel(model);
    findings_table_->setObjectName(QStringLiteral("aida.stealth.findings"));
    findings_table_->verticalHeader()->setVisible(false);
    findings_table_->verticalHeader()->setDefaultSectionSize(tokens.table.row_h);
    findings_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = findings_table_->horizontalHeader();
    using Column = QtStealthFindingsModel::Column;
    const QFontMetricsF ui_metrics(findings_table_->font());
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const auto with_cell_pad = [&tokens](int content) {
        return content + 2 * tokens.table.cell_pad_x + tokens.spacing.xs;
    };
    horizontal->setSectionResizeMode(static_cast<int>(Column::severity),
        QHeaderView::Fixed);
    findings_table_->setColumnWidth(static_cast<int>(Column::severity),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Severity"))),
            static_cast<int>(ui_metrics.horizontalAdvance(
                QStringLiteral("Critical"))))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::category),
        QHeaderView::Fixed);
    findings_table_->setColumnWidth(static_cast<int>(Column::category),
        with_cell_pad(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Suspicious Module")))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::address),
        QHeaderView::Fixed);
    findings_table_->setColumnWidth(static_cast<int>(Column::address),
        with_cell_pad(static_cast<int>(code_metrics.horizontalAdvance(
            QStringLiteral("0xDDDDDDDDDDDDDDDD")))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::finding),
        QHeaderView::Stretch);
    horizontal->setSectionResizeMode(static_cast<int>(Column::details),
        QHeaderView::Stretch);
    horizontal->setStretchLastSection(false);
    findings_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    findings_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    findings_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    findings_table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    findings_table_->setShowGrid(false);
    findings_table_->setAlternatingRowColors(true);
    findings_table_->setContextMenuPolicy(Qt::CustomContextMenu);

    scan_state_view_ = new widgets::AidaStateView(page);
    scan_state_view_->setVisible(false);
    layout->addWidget(scan_state_view_, 1);
    layout->addWidget(findings_table_, 1);

    auto* summary = new QWidget(page);
    summary->setObjectName(QStringLiteral("aida.stealth.summary"));
    auto* summary_layout = new QHBoxLayout(summary);
    summary_layout->setContentsMargins(tokens.toolbar.padding_x,
        tokens.toolbar.padding_y, tokens.toolbar.padding_x,
        tokens.toolbar.padding_y);
    summary_layout->setSpacing(tokens.spacing.md);
    summary_label_ = new QLabel(summary);
    summary_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    summary_critical_ = new QLabel(summary);
    summary_critical_->setProperty("aidaVariant", QStringLiteral("error"));
    summary_high_ = new QLabel(summary);
    summary_high_->setProperty("aidaVariant", QStringLiteral("warning"));
    summary_rest_ = new QLabel(summary);
    summary_rest_->setProperty("aidaVariant", QStringLiteral("secondary"));
    summary_layout->addWidget(summary_label_);
    summary_layout->addWidget(summary_critical_);
    summary_layout->addWidget(summary_high_);
    summary_layout->addWidget(summary_rest_);
    summary_layout->addStretch(1);
    layout->addWidget(summary);

    connect(scan, &QToolButton::clicked, this, [] {
        diag::log_tagged("stealth", "view_scan_request");
        stealth_engine::run_protection_scan();
    });
    connect(stop, &QToolButton::clicked, this, [] {
        diag::log_tagged("stealth", "view_scan_stop");
        stealth_engine::stop_protection_scan();
    });
    connect(clear, &QToolButton::clicked, this, [this] {
        std::size_t cleared = 0;
        if (stealth_engine::clear_protection_findings(cleared)) {
            selected_finding_ = -1;
            diag::log_tagged_fmt("stealth", "view_findings_cleared count=%zu", cleared);
        }
    });
    connect(severity_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        severity_filter_ = index - 1;
    });
    connect(category_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        category_filter_ = index - 1;
    });
    connect(findings_table_, &QTableView::activated, this,
            [this, model](const QModelIndex& index) {
        std::size_t source = 0;
        const auto* finding = model->rowAt(index.row(), &source);
        if (!finding || finding->address == 0) return;
        const auto context = disasm_view::capture_selected_workspace();
        QtAnalysisBridge::instance().navigateTo(context.workspace, finding->address,
            "document.disassembly");
    });
    connect(findings_table_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const auto index = findings_table_->indexAt(pos);
        if (index.isValid())
            showFindingMenu(findings_table_->viewport()->mapToGlobal(pos), index.row());
    });
    return page;
}

QWidget* QtStealthView::buildStatusPage() {
    const auto& tokens = theme::tokens();
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(tokens.panel.padding_compact,
        tokens.panel.padding_compact, tokens.panel.padding_compact,
        tokens.panel.padding_compact);
    layout->setSpacing(tokens.spacing.sm);
    status_state_ = new QLabel(page);
    status_state_->setObjectName(QStringLiteral("aida.stealth.status_state"));
    status_target_ = new QLabel(page);
    status_target_->setObjectName(QStringLiteral("aida.stealth.status_target"));
    status_target_->setProperty("aidaVariant", QStringLiteral("secondary"));
    status_detail_ = new QLabel(page);
    status_detail_->setObjectName(QStringLiteral("aida.stealth.status_detail"));
    status_detail_->setProperty("aidaVariant", QStringLiteral("secondary"));
    status_detail_->setWordWrap(true);
    layout->addWidget(status_state_);
    layout->addWidget(status_target_);
    layout->addWidget(status_detail_);

    cards_host_ = new QWidget(page);
    cards_host_->setObjectName(QStringLiteral("aida.stealth.cards"));
    auto* cards_layout = new QHBoxLayout(cards_host_);
    cards_layout->setContentsMargins(0, 0, 0, 0);
    cards_layout->setSpacing(tokens.spacing.sm);
    const auto make_card = [card_parent = cards_host_, cards_layout](const QString& label) {
        auto* frame = new QFrame(card_parent);
        frame->setObjectName(QStringLiteral("aida.stealth.card"));
        frame->setProperty("aidaRole", QStringLiteral("card"));
        auto* frame_layout = new QVBoxLayout(frame);
        auto* name = new QLabel(label, frame);
        name->setProperty("aidaVariant", QStringLiteral("secondary"));
        auto* value = new QLabel(frame);
        frame_layout->addWidget(value);
        frame_layout->addWidget(name);
        cards_layout->addWidget(frame);
        return value;
    };
    card_pid_ = make_card(QStringLiteral("Target PID"));
    card_peb_ = make_card(QStringLiteral("PEB Spoofed"));
    card_rdtsc_ = make_card(QStringLiteral("RDTSC Hook"));
    layout->addWidget(cards_host_);
    cards_host_->setVisible(false);

    auto* model = new QtStealthHooksModel(page);
    hooks_model_ = model;
    hooks_table_ = new QTableView(page);
    hooks_table_->setModel(model);
    hooks_table_->setObjectName(QStringLiteral("aida.stealth.hooks"));
    hooks_table_->verticalHeader()->setVisible(false);
    hooks_table_->verticalHeader()->setDefaultSectionSize(tokens.table.row_h);
    hooks_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto* horizontal = hooks_table_->horizontalHeader();
    using Column = QtStealthHooksModel::Column;
    const QFontMetricsF ui_metrics(hooks_table_->font());
    const QFontMetricsF code_metrics(theme::fonts::codeRegular());
    const auto with_cell_pad = [&tokens](int content) {
        return content + 2 * tokens.table.cell_pad_x + tokens.spacing.xs;
    };
    const int hex_w = with_cell_pad(static_cast<int>(
        code_metrics.horizontalAdvance(QStringLiteral("0xDDDDDDDDDDDDDDDD"))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::target),
        QHeaderView::Fixed);
    hooks_table_->setColumnWidth(static_cast<int>(Column::target), hex_w);
    horizontal->setSectionResizeMode(static_cast<int>(Column::trampoline),
        QHeaderView::Fixed);
    hooks_table_->setColumnWidth(static_cast<int>(Column::trampoline), hex_w);
    horizontal->setSectionResizeMode(static_cast<int>(Column::size), QHeaderView::Fixed);
    hooks_table_->setColumnWidth(static_cast<int>(Column::size),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Size"))),
            static_cast<int>(code_metrics.horizontalAdvance(
                QStringLiteral("16384"))))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::peb), QHeaderView::Fixed);
    hooks_table_->setColumnWidth(static_cast<int>(Column::peb),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("PEB"))),
            static_cast<int>(ui_metrics.horizontalAdvance(QStringLiteral("yes"))))));
    horizontal->setSectionResizeMode(static_cast<int>(Column::active), QHeaderView::Fixed);
    hooks_table_->setColumnWidth(static_cast<int>(Column::active),
        with_cell_pad((std::max)(static_cast<int>(ui_metrics.horizontalAdvance(
            QStringLiteral("Active"))),
            static_cast<int>(ui_metrics.horizontalAdvance(
                QStringLiteral("removed"))))));
    horizontal->setStretchLastSection(true);
    hooks_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    hooks_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    hooks_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    hooks_table_->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    hooks_table_->setShowGrid(false);
    hooks_table_->setAlternatingRowColors(true);

    stealth_state_view_ = new widgets::AidaStateView(page);
    stealth_state_view_->setState(widgets::AidaStateView::State::Empty);
    stealth_state_view_->setTitle(QStringLiteral("Stealth auto-armed"));
    stealth_state_view_->setMessage(QStringLiteral(
        "Attach a process to install default anti-debug protection automatically."));
    layout->addWidget(stealth_state_view_, 1);
    layout->addWidget(hooks_table_, 1);
    return page;
}

void QtStealthView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    timer_->start();
}

void QtStealthView::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

void QtStealthView::pollScan() {
    const bool scanning =
        stealth_engine::g_scan.scanning.load(std::memory_order_acquire);
    const auto findings = stealth_engine::capture_protection_findings();
    const std::uint64_t generation =
        stealth_engine::g_scan.generation.load(std::memory_order_acquire);
    const bool filters_changed = severity_filter_ != -1 || category_filter_ != -1;
    if (generation != findings_generation_ || findings != findings_ ||
        filters_changed || !findings_) {
        findings_ = findings;
        findings_generation_ = generation;
        filtered_findings_.clear();
        if (findings_) {
            filtered_findings_.reserve(findings_->size());
            for (std::size_t index = 0; index < findings_->size(); ++index) {
                const auto& finding = (*findings_)[index];
                if (severity_filter_ >= 0 &&
                    static_cast<int>(finding.severity) != 4 - severity_filter_)
                    continue;
                if (category_filter_ >= 0 &&
                    static_cast<int>(finding.category) != category_filter_)
                    continue;
                filtered_findings_.push_back(index);
            }
        }
        if (!findings_ || selected_finding_ < 0 ||
            static_cast<std::size_t>(selected_finding_) >= findings_->size())
            selected_finding_ = -1;
        auto* model = static_cast<QtStealthFindingsModel*>(findings_model_);
        if (model)
            model->setContent(findings_, filtered_findings_);
    }
    const auto status = stealth_engine::capture_protection_scan_status();
    if (scanning) {
        scan_progress_->setVisible(true);
        scan_progress_->setRange(0, 100);
        scan_progress_->setValue(static_cast<int>(
            stealth_engine::g_scan.progress.load(std::memory_order_acquire) * 100.f));
        scan_progress_->setFormat(status && !status->empty()
            ? QString::fromStdString(*status) : QStringLiteral("Scanning"));
        scan_status_->setVisible(false);
    } else {
        scan_progress_->setVisible(false);
        scan_status_->setVisible(status && !status->empty());
        scan_status_->setText(status ? QString::fromStdString(*status) : QString());
    }
    int critical = 0, high = 0, medium = 0, low = 0, info = 0;
    if (findings_) {
        for (const std::size_t index : filtered_findings_) {
            switch ((*findings_)[index].severity) {
            case stealth_engine::finding_severity_t::critical: ++critical; break;
            case stealth_engine::finding_severity_t::high: ++high; break;
            case stealth_engine::finding_severity_t::medium: ++medium; break;
            case stealth_engine::finding_severity_t::low: ++low; break;
            case stealth_engine::finding_severity_t::info: ++info; break;
            }
        }
    }
    summary_label_->setText(QStringLiteral("%1 shown").arg(filtered_findings_.size()));
    summary_critical_->setText(QStringLiteral("%1 critical").arg(critical));
    summary_high_->setText(QStringLiteral("%1 high").arg(high));
    summary_rest_->setText(QStringLiteral("Medium %1  Low %2  Info %3")
        .arg(medium).arg(low).arg(info));
    refreshScanPresentation();
}

void QtStealthView::pollStatus() {
    const bool stealth_active = stealth_engine::is_active();
    const std::uint32_t attached_pid = driver_bridge::attached_pid();
    const std::string status_str = stealth_engine::get_status();
    status_state_->setText(stealth_active
        ? QStringLiteral("Automatic stealth active")
        : QStringLiteral("Automatic stealth idle"));
    status_target_->setText(attached_pid != 0
        ? QStringLiteral("Attached PID %1").arg(attached_pid)
        : QStringLiteral("Attach a process to arm stealth automatically"));
    status_detail_->setText(QString::fromStdString(status_str));

    std::vector<stealth_engine::hook_entry_t> hooks_copy;
    bool peb_ok = false;
    bool rdtsc_ok = false;
    std::uint32_t session_pid = 0;
    {
        std::lock_guard<std::mutex> lock(stealth_engine::g_state.mutex);
        hooks_copy = stealth_engine::g_state.session.hooks;
        peb_ok = stealth_engine::g_state.session.peb_spoofed;
        rdtsc_ok = stealth_engine::g_state.session.rdtsc_hooked;
        session_pid = stealth_engine::g_state.session.pid;
    }
    auto* model = static_cast<QtStealthHooksModel*>(hooks_model_);
    if (model)
        model->setHooks(hooks_copy, peb_ok);
    const bool show_cards = hooks_copy.empty() && stealth_active;
    cards_host_->setVisible(show_cards);
    card_pid_->setText(QString::number(session_pid));
    card_peb_->setText(peb_ok ? QStringLiteral("Active") : QStringLiteral("Inactive"));
    card_rdtsc_->setText(rdtsc_ok ? QStringLiteral("Active") : QStringLiteral("Inactive"));
    const bool empty_idle = hooks_copy.empty() && !stealth_active;
    stealth_state_view_->setVisible(empty_idle);
    hooks_table_->setVisible(!empty_idle && !hooks_copy.empty());
}

void QtStealthView::refreshScanPresentation() {
    const bool scanning =
        stealth_engine::g_scan.scanning.load(std::memory_order_acquire);
    const bool has_findings = findings_ && !findings_->empty();
    if (!has_findings && !scanning) {
        scan_state_view_->setState(widgets::AidaStateView::State::Empty);
        scan_state_view_->setTitle(QStringLiteral("No protection findings"));
        scan_state_view_->setMessage(QStringLiteral(
            "Run a scan to inspect the attached process for protection mechanisms."));
        scan_state_view_->setVisible(true);
        findings_table_->setVisible(false);
        return;
    }
    if (filtered_findings_.empty() && !scanning) {
        scan_state_view_->setState(widgets::AidaStateView::State::Empty);
        scan_state_view_->setTitle(QStringLiteral("No matching findings"));
        scan_state_view_->setMessage(QStringLiteral(
            "No retained finding matches the selected severity and category filters."));
        scan_state_view_->setVisible(true);
        findings_table_->setVisible(false);
        return;
    }
    scan_state_view_->setVisible(false);
    findings_table_->setVisible(true);
}

void QtStealthView::showFindingMenu(const QPoint& global_pos, int view_row) {
    auto* model = static_cast<QtStealthFindingsModel*>(findings_model_);
    if (!model) return;
    std::size_t source_index = 0;
    const auto* finding = model->rowAt(view_row, &source_index);
    if (!finding) return;
    selected_finding_ = static_cast<int>(source_index);
    const std::uint64_t generation = findings_generation_;
    const auto findings = findings_;
    aida::ui::application_ui::retained_entity_context_t retained;
    retained.owner_id = "analysis.protection.finding";
    retained.entity_id = std::to_string(source_index) + ":" + finding->title;
    retained.entity_generation = generation;
    retained.active_view = aida::ui::stable_view_id_t("view.analysis.protection");
    retained.validate_identity = [source_index, generation, findings] {
        if (stealth_engine::g_scan.generation.load(std::memory_order_acquire) !=
                generation ||
            stealth_engine::capture_protection_findings() != findings)
            return aida::ui::capability_state_t::unavailable(
                "The protection scan publication changed; select the finding again");
        return source_index < findings->size()
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(
                "The retained finding no longer exists");
    };
    const auto add_action = [&retained](const char* id, bool enabled,
        const char* reason, auto invoke) {
        aida::ui::application_ui::retained_entity_action_t action;
        action.action_id = id;
        action.capability = enabled
            ? aida::ui::capability_state_t::available()
            : aida::ui::capability_state_t::unavailable(reason);
        action.invoke = std::move(invoke);
        retained.actions.push_back(std::move(action));
    };
    const std::uint64_t address = finding->address;
    add_action("analysis.protection.finding.follow_disassembly", address != 0,
        "This finding has no concrete address", [address] {
            const auto context = disasm_view::capture_selected_workspace();
            QtAnalysisBridge::instance().navigateTo(context.workspace, address,
                "document.disassembly");
            return aida::ui::action_handler_result_t::completed();
        });
    add_action("analysis.protection.finding.copy_address", address != 0,
        "This finding has no concrete address", [address] {
            char text[32]{};
            std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(address));
            clipboard::set_text(QString::fromLatin1(text));
            return aida::ui::action_handler_result_t::completed();
        });
    const std::string title = finding->title;
    const std::string detail = finding->detail;
    const std::string module = finding->module;
    add_action("analysis.protection.finding.copy_title", true, "", [title] {
        clipboard::set_text(QString::fromStdString(title));
        return aida::ui::action_handler_result_t::completed();
    });
    add_action("analysis.protection.finding.copy_details", !detail.empty(),
        "This finding has no detail text", [detail] {
            clipboard::set_text(QString::fromStdString(detail));
            return aida::ui::action_handler_result_t::completed();
        });
    add_action("analysis.protection.finding.copy_module", !module.empty(),
        "This finding is not associated with a module", [module] {
            clipboard::set_text(QString::fromStdString(module));
            return aida::ui::action_handler_result_t::completed();
        });
    QtAnalysisBridge::instance().showRetainedMenu(retained,
        aida::ui::context_menu_open_origin_t::pointer, global_pos, this);
}

}
