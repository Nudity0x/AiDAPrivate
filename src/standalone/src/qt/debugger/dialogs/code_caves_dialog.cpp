#include "qt/debugger/dialogs/code_caves_dialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "core/debugger/debugger_engine.hpp"
#include "core/debugger/debugger_view.hpp"
#include "core/runtime/standalone_driver.hpp"

#include "qt/debugger/debugger_models.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::debugger {

QPointer<CodeCavesDialog> CodeCavesDialog::active_;

class CodeCavesModel : public DebuggerTableModelBase {
public:
    explicit CodeCavesModel(QObject* parent = nullptr)
        : DebuggerTableModelBase({{QStringLiteral("Address"), 180, false},
                                  {QStringLiteral("Bytes"), 90, false},
                                  {QStringLiteral("Module"), 0, true}},
            parent) {}

    void applyPublication(
        std::shared_ptr<const debugger_view::code_cave_publication_view_t>
            publication) {
        beginResetModel();
        publication_ = std::move(publication);
        endResetModel();
    }

    const debugger_view::code_cave_entry_t* caveAt(int row) const {
        if (!publication_ || row < 0 ||
            row >= static_cast<int>(publication_->results.size()))
            return nullptr;
        return &publication_->results[static_cast<std::size_t>(row)];
    }

    quint64 rowId(int row) const override {
        const auto* cave = caveAt(row);
        return cave ? cave->address : 0;
    }

protected:
    int implRowCount() const override {
        return publication_
            ? static_cast<int>(publication_->results.size()) : 0;
    }

    QVariant cellData(int row, int column, int role) const override {
        const auto* cave = caveAt(row);
        if (!cave)
            return QVariant();
        if (role == Qt::DisplayRole) {
            switch (column) {
                case 0:
                    return QString::asprintf("0x%016llX",
                        static_cast<unsigned long long>(cave->address));
                case 1:
                    return QString::number(static_cast<qulonglong>(cave->size));
                case 2:
                    return QString::fromStdString(cave->module);
                default:
                    return QVariant();
            }
        }
        if (role == Qt::FontRole)
            return theme::fonts::codeRegular();
        if (role == AddressRole)
            return QVariant::fromValue(cave->address);
        if (role == TooltipTextRole)
            return QString::asprintf("0x%016llX  |  %llu bytes  |  %s",
                static_cast<unsigned long long>(cave->address),
                static_cast<unsigned long long>(cave->size),
                cave->module.c_str());
        return QVariant();
    }

private:
    std::shared_ptr<const debugger_view::code_cave_publication_view_t>
        publication_;
};

namespace {

void applyColumnSpec(QTableView* view, const DebuggerTableModelBase* model) {
    for (int section = 0;
         section < static_cast<int>(model->columns().size()); ++section) {
        const auto& column =
            model->columns()[static_cast<std::size_t>(section)];
        if (column.stretch)
            view->horizontalHeader()->setSectionResizeMode(section,
                QHeaderView::Stretch);
        else if (column.width > 0) {
            view->horizontalHeader()->setSectionResizeMode(section,
                QHeaderView::Interactive);
            view->setColumnWidth(section, column.width);
        }
    }
}

}

void CodeCavesDialog::present(QWidget* parent) {
    if (active_) {
        active_->raise();
        active_->activateWindow();
        return;
    }
    auto* dialog = new CodeCavesDialog(parent);
    active_ = dialog;
    dialog->open();
}

CodeCavesDialog::CodeCavesDialog(QWidget* parent)
    : AidaDialog(parent) {
    setObjectName(QStringLiteral("aida.debugger.code_caves"));
    setWindowTitle(QStringLiteral("Find Code Caves"));
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(420, 360);

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(QStringLiteral(
        "Discover bounded 00/CC filler runs in loaded modules. Every result "
        "retains the exact process and debugger stop generation that produced "
        "it."), this);
    intro->setWordWrap(true);
    intro->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    module_filter_edit_ = new QLineEdit(this);
    module_filter_edit_->setObjectName(
        QStringLiteral("aida.debugger.code_caves.filter"));
    module_filter_edit_->setPlaceholderText(
        QStringLiteral("module name (optional)"));
    module_filter_edit_->setMaxLength(127);
    form->addRow(QStringLiteral("Module filter:"), module_filter_edit_);
    minimum_edit_ = new QLineEdit(QStringLiteral("32"), this);
    minimum_edit_->setObjectName(
        QStringLiteral("aida.debugger.code_caves.minimum"));
    minimum_edit_->setMaxLength(15);
    form->addRow(QStringLiteral("Minimum bytes:"), minimum_edit_);
    layout->addLayout(form);

    search_button_ = new QPushButton(QStringLiteral("Search"), this);
    search_button_->setObjectName(
        QStringLiteral("aida.debugger.code_caves.search"));
    connect(search_button_, &QPushButton::clicked, this,
        &CodeCavesDialog::startSearch);
    layout->addWidget(search_button_);

    publication_label_ = new QLabel(this);
    publication_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    publication_label_->setFont(theme::fonts::codeRegular());
    publication_label_->setVisible(false);
    layout->addWidget(publication_label_);

    notice_label_ = new QLabel(this);
    notice_label_->setWordWrap(true);
    notice_label_->setProperty("aidaVariant", QStringLiteral("info"));
    notice_label_->setVisible(false);
    layout->addWidget(notice_label_);

    error_label_ = new QLabel(this);
    error_label_->setWordWrap(true);
    error_label_->setProperty("aidaVariant", QStringLiteral("error"));
    error_label_->setVisible(false);
    layout->addWidget(error_label_);

    results_model_ = new CodeCavesModel(this);
    results_view_ = new QTableView(this);
    results_view_->setObjectName(
        QStringLiteral("aida.debugger.code_caves.results"));
    results_view_->verticalHeader()->setVisible(false);
    results_view_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    results_view_->verticalHeader()->setDefaultSectionSize(
        theme::tokens().table.compact_row_h);
    results_view_->setShowGrid(false);
    results_view_->setAlternatingRowColors(true);
    results_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_view_->setModel(results_model_);
    applyColumnSpec(results_view_, results_model_);
    connect(results_view_->selectionModel(), &QItemSelectionModel::currentChanged,
        this, [this] { pollPublication(); });
    layout->addWidget(results_view_, 1);

    auto* buttons = new QDialogButtonBox(this);
    stage_button_ = buttons->addButton(QStringLiteral("Stage Patch Review"),
        QDialogButtonBox::AcceptRole);
    auto* close_button = buttons->addButton(QStringLiteral("Close"),
        QDialogButtonBox::RejectRole);
    connect(stage_button_, &QPushButton::clicked, this,
        &CodeCavesDialog::stageSelected);
    connect(close_button, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(buttons);

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(250);
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this,
        &CodeCavesDialog::pollPublication);
    poll_timer_->start();
    pollPublication();
}

void CodeCavesDialog::pollPublication() {
    const auto publication = debugger_view::code_cave_publication();
    const bool pending = debugger_view::code_cave_search_pending();
    search_button_->setText(pending ? QStringLiteral("Searching...")
                                    : QStringLiteral("Search"));
    search_button_->setEnabled(!pending);
    if (publication &&
        publication->generation != visible_generation_) {
        visible_generation_ = publication->generation;
        results_model_->applyPublication(publication);
        dialog_error_.clear();
    }
    if (publication && publication->generation != 0) {
        publication_label_->setText(QString::asprintf(
            "PID %u  |  stop generation %llu  |  publication %llu",
            publication->target_pid,
            static_cast<unsigned long long>(
                publication->target_stop_generation),
            static_cast<unsigned long long>(publication->generation)));
        publication_label_->setVisible(true);
    }
    const bool target_current = publication &&
        publication->target_pid != 0 &&
        driver_bridge::attached_pid() == publication->target_pid &&
        debugger_interaction::current_stop_generation() ==
            publication->target_stop_generation &&
        debugger_engine::g_state.status.load(std::memory_order_acquire) ==
            debugger_engine::dbg_status_t::paused;
    QString notice;
    const char* notice_variant = "info";
    if (publication && publication->generation != 0 && !target_current) {
        notice = QStringLiteral(
            "Code-cave results are stale: the attached process or debugger "
            "stop generation changed. Search again before staging a patch "
            "review.");
        notice_variant = "warning";
    } else if (publication && !publication->detail.empty()) {
        notice = QStringLiteral("Code-cave search detail: %1")
            .arg(QString::fromStdString(publication->detail));
    }
    notice_label_->setText(notice);
    notice_label_->setVisible(!notice.isEmpty());
    if (notice_label_->property("aidaVariant") != notice_variant) {
        notice_label_->setProperty("aidaVariant",
            QString::fromLatin1(notice_variant));
        notice_label_->style()->unpolish(notice_label_);
        notice_label_->style()->polish(notice_label_);
    }
    error_label_->setText(dialog_error_);
    error_label_->setVisible(!dialog_error_.isEmpty());

    const auto index = results_view_->currentIndex();
    const bool has_selection = index.isValid() &&
        results_model_->caveAt(index.row()) != nullptr;
    stage_button_->setEnabled(has_selection && target_current && !pending);
}

void CodeCavesDialog::startSearch() {
    std::string error;
    if (!debugger_view::request_code_cave_search(
            module_filter_edit_->text().toStdString(),
            static_cast<std::size_t>(
                minimum_edit_->text().toULongLong(nullptr, 10)),
            &error)) {
        dialog_error_ = error.empty()
            ? QStringLiteral("The code-cave search could not be queued.")
            : QString::fromStdString(error);
    } else {
        dialog_error_.clear();
    }
    pollPublication();
}

void CodeCavesDialog::stageSelected() {
    const auto index = results_view_->currentIndex();
    if (!index.isValid())
        return;
    const auto publication = debugger_view::code_cave_publication();
    std::string error;
    if (!debugger_view::stage_code_cave_review(
            visible_generation_, index.row(), &error)) {
        dialog_error_ = error.empty()
            ? QStringLiteral(
                "The exact code-cave patch review could not be staged.")
            : QString::fromStdString(error);
        pollPublication();
        return;
    }
    dialog_error_.clear();
    accept();
}

}
