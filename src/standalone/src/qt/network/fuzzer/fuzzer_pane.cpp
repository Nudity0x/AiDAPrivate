#include "qt/network/fuzzer/fuzzer_pane.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QProgressBar>
#include <QRadioButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedLayout>
#include <QTableView>
#include <QTimer>
#include <QValidator>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "core/ui/task_center.hpp"
#include "helpers/diag_log.hpp"
#include "qt/net/qt_human_request_editor.hpp"
#include "qt/network/bounded_plain_text_edit.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

// PayloadSetsEditor edits the payload set list of the fuzzer config
// (up to 64 sets; per-set name / type / source rows). It mutates the shared
// fuzz_config.payload_sets in place, exactly like the ImGui panel did.
class PayloadSetsEditor : public QWidget {
public:
    explicit PayloadSetsEditor(QWidget* parent = nullptr)
        : QWidget(parent) {
        const auto& t = theme::tokens();
        layout_ = new QVBoxLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(t.spacing.xs);
        auto* headerRow = new QHBoxLayout();
        headerRow->setSpacing(t.spacing.sm);
        auto* title = new QLabel(QStringLiteral("Payload Sets"), this);
        title->setProperty("aidaTone", QStringLiteral("titleAccent"));
        headerRow->addWidget(title);
        auto* hint = new QLabel(QStringLiteral("(one set per exact $value$ or FUZZ marker)"), this);
        hint->setProperty("aidaTone", QStringLiteral("dim"));
        headerRow->addWidget(hint);
        add_button_ = new widgets::AidaButton(QStringLiteral("+"), this);
        add_button_->setKind(widgets::AidaButton::Kind::Ghost);
        add_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
        add_button_->setToolTip(QStringLiteral("Add a payload set"));
        headerRow->addWidget(add_button_);
        count_label_ = new QLabel(this);
        count_label_->setProperty("aidaTone", QStringLiteral("dim"));
        headerRow->addWidget(count_label_);
        remove_button_ = new widgets::AidaButton(QStringLiteral("-"), this);
        remove_button_->setKind(widgets::AidaButton::Kind::Ghost);
        remove_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
        remove_button_->setToolTip(QStringLiteral("Remove the last payload set"));
        headerRow->addWidget(remove_button_);
        headerRow->addStretch(1);
        layout_->addLayout(headerRow);
        sets_host_ = new QWidget(this);
        sets_layout_ = new QVBoxLayout(sets_host_);
        sets_layout_->setContentsMargins(0, 0, 0, 0);
        sets_layout_->setSpacing(t.spacing.sm);
        sets_layout_->addStretch(1);
        layout_->addWidget(sets_host_, 1);

        connect(add_button_, &QAbstractButton::clicked, this, [this] {
            if (!sets_ || sets_->size() >= k_fuzzer_payload_set_limit)
                return;
            sets_->emplace_back();
            rebuildRows();
            emitChanged();
        });
        connect(remove_button_, &QAbstractButton::clicked, this, [this] {
            if (!sets_ || sets_->size() <= 1)
                return;
            sets_->pop_back();
            rebuildRows();
            emitChanged();
        });
    }

    void bind(std::vector<network_view::payload_set_t>* sets,
              std::function<void()> onChanged) {
        sets_ = sets;
        on_changed_ = std::move(onChanged);
        rebuildRows();
    }

private:
    void emitChanged() {
        refreshCountLabel();
        if (on_changed_)
            on_changed_();
    }

    void refreshCountLabel() {
        const std::size_t count = sets_ ? sets_->size() : 0;
        count_label_->setVisible(count >= k_fuzzer_payload_set_limit);
        count_label_->setText(QStringLiteral("%1 of 64 payload sets configured").arg(count));
        add_button_->setEnabled(count < k_fuzzer_payload_set_limit);
        remove_button_->setEnabled(count > 1);
    }

    void rebuildRows() {
        while (auto* item = sets_layout_->takeAt(0)) {
            if (item->widget())
                item->widget()->deleteLater();
            delete item;
        }
        if (!sets_)
            return;
        const auto& t = theme::tokens();
        for (std::size_t index = 0; index < sets_->size(); ++index) {
            auto* group = new QGroupBox(QStringLiteral("Set %1").arg(index + 1), sets_host_);
            auto* form = new QFormLayout(group);
            form->setSpacing(t.spacing.xs);
            auto& set = (*sets_)[index];
            auto* nameEdit = new QLineEdit(QString::fromStdString(set.name), group);
            nameEdit->setMaxLength(127);
            nameEdit->setPlaceholderText(QStringLiteral("Set name"));
            form->addRow(QStringLiteral("Name:"), nameEdit);
            auto* typeCombo = new QComboBox(group);
            typeCombo->addItems({QStringLiteral("Wordlist File"),
                QStringLiteral("Inline List")});
            typeCombo->setCurrentIndex(set.type);
            form->addRow(QStringLiteral("Type:"), typeCombo);
            auto* pathEdit = new QLineEdit(QString::fromStdString(set.source), group);
            pathEdit->setMaxLength(511);
            pathEdit->setPlaceholderText(QStringLiteral("Path to file"));
            auto* inlineEdit = new BoundedPlainTextEdit(511, group);
            inlineEdit->setPlainText(QString::fromStdString(set.source));
            inlineEdit->setFont(theme::fonts::codeRegular());
            inlineEdit->setMaximumHeight(editor_min_height_lines(inlineEdit, 3));
            auto* stackHost = new QWidget(group);
            auto* stackLayout = new QVBoxLayout(stackHost);
            stackLayout->setContentsMargins(0, 0, 0, 0);
            stackLayout->addWidget(pathEdit);
            stackLayout->addWidget(inlineEdit);
            form->addRow(QStringLiteral("Source:"), stackHost);
            inlineEdit->setVisible(set.type == 1);
            pathEdit->setVisible(set.type == 0);
            sets_layout_->addWidget(group);

            connect(nameEdit, &QLineEdit::textChanged, group,
                [this, index](const QString& text) {
                    if (sets_ && index < sets_->size()) {
                        (*sets_)[index].name = text.toStdString();
                        if (on_changed_) on_changed_();
                    }
                });
            connect(typeCombo, &QComboBox::currentIndexChanged, group,
                [this, index, pathEdit, inlineEdit](int type) {
                    if (sets_ && index < sets_->size()) {
                        auto& setRef = (*sets_)[index];
                        setRef.type = type;
                        setRef.source = type == 1
                            ? inlineEdit->toPlainText().toStdString()
                            : pathEdit->text().toStdString();
                        pathEdit->setVisible(type == 0);
                        inlineEdit->setVisible(type == 1);
                        if (on_changed_) on_changed_();
                    }
                });
            connect(pathEdit, &QLineEdit::textChanged, group,
                [this, index](const QString& text) {
                    if (sets_ && index < sets_->size()) {
                        (*sets_)[index].source = text.toStdString();
                        if (on_changed_) on_changed_();
                    }
                });
            connect(inlineEdit, &QPlainTextEdit::textChanged, inlineEdit,
                [this, index, inlineEdit] {
                    if (sets_ && index < sets_->size()) {
                        (*sets_)[index].source = inlineEdit->toPlainText().toStdString();
                        if (on_changed_) on_changed_();
                    }
                });
        }
        sets_layout_->addStretch(1);
        refreshCountLabel();
    }

    std::vector<network_view::payload_set_t>* sets_ = nullptr;
    std::function<void()> on_changed_;
    QVBoxLayout* layout_ = nullptr;
    QWidget* sets_host_ = nullptr;
    QVBoxLayout* sets_layout_ = nullptr;
    widgets::AidaButton* add_button_ = nullptr;
    widgets::AidaButton* remove_button_ = nullptr;
    QLabel* count_label_ = nullptr;
};

FuzzerResultsModel::FuzzerResultsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int FuzzerResultsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0
        : static_cast<int>((std::min)(retained_count_,
            static_cast<std::uint64_t>((std::numeric_limits<int>::max)())));
}

int FuzzerResultsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0
        : static_cast<int>(1 + max_payload_columns_ + 4 + (show_extract_ ? 1 : 0) +
            (show_failures_ ? 1 : 0));
}

QVariant FuzzerResultsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const auto* row = rowAt(index.row());
    if (!row)
        return {};
    const int column = index.column();
    const int payloadColumns = static_cast<int>(max_payload_columns_);
    if (role == Qt::DisplayRole) {
        if (column == 0)
            return QString::number(static_cast<unsigned long long>(row->index));
        if (column >= 1 && column <= payloadColumns) {
            const int payloadIndex = column - 1;
            if (snapshot_->payload_catalog &&
                payloadIndex < static_cast<int>(row->payload_indices.size()) &&
                payloadIndex < static_cast<int>(snapshot_->payload_catalog->size()) &&
                row->payload_indices[static_cast<std::size_t>(payloadIndex)] <
                    (*snapshot_->payload_catalog)[static_cast<std::size_t>(payloadIndex)].size()) {
                const auto& payload = (*snapshot_->payload_catalog)
                    [static_cast<std::size_t>(payloadIndex)]
                    [row->payload_indices[static_cast<std::size_t>(payloadIndex)]];
                return QString::fromStdString(payload.size() > 28
                    ? payload.substr(0, 28) + ".." : payload);
            }
            return QString();
        }
        const int fixed = column - 1 - payloadColumns;
        const bool extractedColumn = show_extract_ &&
            fixed == 4;
        const bool errorColumn = show_failures_ &&
            fixed == (show_extract_ ? 5 : 4);
        switch (fixed) {
        case 0: return row->error.empty()
            ? QString::number(row->status_code) : QStringLiteral("ERR");
        case 1: return QString::number(static_cast<qulonglong>(row->response_len));
        case 2: return QStringLiteral("%1ms")
            .arg(static_cast<unsigned long long>(row->latency_ms));
        case 3: return row->match ? QStringLiteral("YES") : QString();
        default: break;
        }
        if (extractedColumn) {
            if (row->extracted_value.empty())
                return QString();
            return QString::fromStdString(row->extracted_value.size() > 20
                ? row->extracted_value.substr(0, 20) + ".." : row->extracted_value);
        }
        if (errorColumn) {
            if (row->error.empty())
                return QString();
            return QString::fromStdString(row->error.size() > 32
                ? row->error.substr(0, 32) + ".." : row->error);
        }
        return {};
    }
    if (role == Qt::ToolTipRole) {
        if (column >= 1 && column <= payloadColumns) {
            const int payloadIndex = column - 1;
            if (snapshot_->payload_catalog &&
                payloadIndex < static_cast<int>(row->payload_indices.size()) &&
                payloadIndex < static_cast<int>(snapshot_->payload_catalog->size()) &&
                row->payload_indices[static_cast<std::size_t>(payloadIndex)] <
                    (*snapshot_->payload_catalog)[static_cast<std::size_t>(payloadIndex)].size()) {
                return QString::fromStdString((*snapshot_->payload_catalog)
                    [static_cast<std::size_t>(payloadIndex)]
                    [row->payload_indices[static_cast<std::size_t>(payloadIndex)]]);
            }
            return {};
        }
        const int fixed = column - 1 - payloadColumns;
        if (show_extract_ && fixed == 4 && !row->extracted_value.empty())
            return QString::fromStdString(row->extracted_value);
        if (show_failures_ && fixed == (show_extract_ ? 5 : 4) && !row->error.empty())
            return QString::fromStdString(row->error);
        return {};
    }
    if (role == Qt::ForegroundRole) {
        const auto& t = theme::tokens();
        if (column == 0 || (column >= 1 && column <= payloadColumns))
            return t.text_secondary;
        const int fixed = column - 1 - payloadColumns;
        if (fixed == 0)
            return row->error.empty() ? status_code_color(row->status_code) : t.error;
        if (fixed == 3 && row->match)
            return t.success;
        if (show_extract_ && fixed == 4)
            return t.warning;
        if (show_failures_ && fixed == (show_extract_ ? 5 : 4))
            return t.error;
        return t.text_secondary;
    }
    if (role == Qt::BackgroundRole) {
        if (row->match)
            return theme::tokens().success_soft;
    }
    return {};
}

QVariant FuzzerResultsModel::headerData(int section, Qt::Orientation orientation,
                                        int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    const int payloadColumns = static_cast<int>(max_payload_columns_);
    if (section == 0)
        return QStringLiteral("#");
    if (section >= 1 && section <= payloadColumns)
        return QStringLiteral("Payload %1").arg(section);
    const int fixed = section - 1 - payloadColumns;
    switch (fixed) {
    case 0: return QStringLiteral("Status");
    case 1: return QStringLiteral("Length");
    case 2: return QStringLiteral("Time");
    case 3: return QStringLiteral("Match");
    default: break;
    }
    if (show_extract_ && fixed == 4)
        return QStringLiteral("Extracted");
    if (show_failures_ && fixed == (show_extract_ ? 5 : 4))
        return QStringLiteral("Error");
    return {};
}

void FuzzerResultsModel::multiData(const QModelIndex& index,
                                   QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

void FuzzerResultsModel::adopt(
    const std::shared_ptr<const network_view::state_t::fuzzer_results_snapshot_t>& snapshot) {
    const std::uint64_t retained = snapshot ? snapshot->retained_count : 0;
    const std::uint64_t dropped = snapshot ? snapshot->dropped_count : 0;
    const std::size_t maxCols = snapshot ? snapshot->maximum_payload_columns : 1;
    const bool showExtract = snapshot && snapshot->has_extracted_values;
    const bool showFailures = snapshot && snapshot->has_failures;
    const bool shapeChanged = maxCols != max_payload_columns_ ||
        showExtract != show_extract_ || showFailures != show_failures_;
    if (shapeChanged) {
        Q_EMIT layoutAboutToBeChanged();
        snapshot_ = snapshot;
        retained_count_ = retained;
        dropped_count_ = dropped;
        max_payload_columns_ = maxCols;
        show_extract_ = showExtract;
        show_failures_ = showFailures;
        Q_EMIT layoutChanged();
        return;
    }
    if (dropped != dropped_count_) {
        beginResetModel();
        snapshot_ = snapshot;
        retained_count_ = retained;
        dropped_count_ = dropped;
        endResetModel();
        return;
    }
    const std::uint64_t previousRetained = retained_count_;
    snapshot_ = snapshot;
    retained_count_ = retained;
    dropped_count_ = dropped;
    if (retained > previousRetained && previousRetained <=
        static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) {
        const int first = static_cast<int>(previousRetained);
        const int last = static_cast<int>((std::min)(retained,
            static_cast<std::uint64_t>((std::numeric_limits<int>::max)()))) - 1;
        if (last >= first) {
            beginInsertRows(QModelIndex(), first, last);
            endInsertRows();
        }
    } else if (retained < previousRetained) {
        beginResetModel();
        endResetModel();
    } else if (rowCount() > 0) {
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
    }
}

const network_view::state_t::fuzzer_result_t* FuzzerResultsModel::rowAt(int row) const noexcept {
    if (row < 0 || static_cast<std::uint64_t>(row) >= retained_count_ || !snapshot_)
        return nullptr;
    const std::size_t pageIndex = static_cast<std::size_t>(row) / k_fuzzer_page_size;
    const std::size_t rowIndex = static_cast<std::size_t>(row) % k_fuzzer_page_size;
    if (pageIndex >= snapshot_->pages.size() || !snapshot_->pages[pageIndex] ||
        rowIndex >= snapshot_->pages[pageIndex]->rows.size())
        return nullptr;
    return &snapshot_->pages[pageIndex]->rows[rowIndex];
}

bool FuzzerResultsModel::findRowForIndex(std::uint64_t resultIndex, int& rowOut) const {
    if (snapshot_) {
        for (std::size_t page = 0; page < snapshot_->pages.size(); ++page) {
            const auto& rows = snapshot_->pages[page]->rows;
            for (std::size_t row = 0; row < rows.size(); ++row) {
                if (rows[row].index == resultIndex) {
                    rowOut = static_cast<int>(page * k_fuzzer_page_size + row);
                    return true;
                }
            }
        }
    }
    return false;
}

FuzzerPane::FuzzerPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.fuzzer"));
    const auto& t = theme::tokens();

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* title = new QLabel(QStringLiteral("Fuzzer / Intruder"), content);
    title->setProperty("aidaTone", QStringLiteral("titleAccent"));
    layout->addWidget(title);

    auto* targetRow = new QHBoxLayout();
    targetRow->setSpacing(t.spacing.sm);
    targetRow->addWidget(new QLabel(QStringLiteral("Host:"), content));
    host_edit_ = new QLineEdit(content);
    host_edit_->setMaxLength(255);
    host_edit_->setPlaceholderText(QStringLiteral("target.example.com"));
    targetRow->addWidget(host_edit_, 1);
    targetRow->addWidget(new QLabel(QStringLiteral("Port:"), content));
    port_spin_ = new QSpinBox(content);
    port_spin_->setRange(1, 65535);
    port_spin_->setValue(443);
    targetRow->addWidget(port_spin_);
    tls_check_ = new QCheckBox(QStringLiteral("TLS"), content);
    tls_check_->setChecked(true);
    targetRow->addWidget(tls_check_);
    layout->addLayout(targetRow);

    auto* modeRow = new QHBoxLayout();
    modeRow->setSpacing(t.spacing.sm);
    modeRow->addWidget(new QLabel(QStringLiteral("Attack Mode:"), content));
    sniper_radio_ = new QRadioButton(QStringLiteral("Sniper"), content);
    sniper_radio_->setChecked(true);
    modeRow->addWidget(sniper_radio_);
    pitchfork_radio_ = new QRadioButton(QStringLiteral("Pitchfork"), content);
    modeRow->addWidget(pitchfork_radio_);
    clusterbomb_radio_ = new QRadioButton(QStringLiteral("Clusterbomb"), content);
    modeRow->addWidget(clusterbomb_radio_);
    modeRow->addStretch(1);
    layout->addLayout(modeRow);

    sniper_panel_ = new QWidget(content);
    auto* sniperLayout = new QHBoxLayout(sniper_panel_);
    sniperLayout->setContentsMargins(0, 0, 0, 0);
    sniperLayout->setSpacing(t.spacing.sm);
    sniperLayout->addWidget(new QLabel(QStringLiteral("Payload Type:"), sniper_panel_));
    payload_type_combo_ = new QComboBox(sniper_panel_);
    payload_type_combo_->addItems({QStringLiteral("Wordlist File"),
        QStringLiteral("Sequential Numbers"), QStringLiteral("Charset Brute")});
    sniperLayout->addWidget(payload_type_combo_);
    sniperLayout->addWidget(new QLabel(QStringLiteral("Payload Source:"), sniper_panel_));
    payload_source_edit_ = new QLineEdit(sniper_panel_);
    payload_source_edit_->setMaxLength(511);
    payload_source_edit_->setPlaceholderText(QStringLiteral("Source..."));
    sniperLayout->addWidget(payload_source_edit_, 1);
    payload_hint_ = new QLabel(QStringLiteral("(path to wordlist)"), sniper_panel_);
    payload_hint_->setProperty("aidaTone", QStringLiteral("dim"));
    sniperLayout->addWidget(payload_hint_);
    layout->addWidget(sniper_panel_);

    sets_editor_ = new PayloadSetsEditor(content);
    sets_editor_->setVisible(false);
    layout->addWidget(sets_editor_);

    auto* tuningRow = new QHBoxLayout();
    tuningRow->setSpacing(t.spacing.sm);
    tuningRow->addWidget(new QLabel(QStringLiteral("Threads:"), content));
    threads_spin_ = new QSpinBox(content);
    threads_spin_->setRange(1, 32);
    threads_spin_->setValue(4);
    tuningRow->addWidget(threads_spin_);
    tuningRow->addWidget(new QLabel(QStringLiteral("Delay (ms):"), content));
    delay_spin_ = new QSpinBox(content);
    delay_spin_->setRange(0, 60000);
    delay_spin_->setValue(0);
    tuningRow->addWidget(delay_spin_);
    tuningRow->addWidget(new QLabel(QStringLiteral("Maximum requests:"), content));
    maximum_edit_ = new QLineEdit(QStringLiteral("10000"), content);
    auto* maximumValidator = new QIntValidator(1, 1000000, maximum_edit_);
    maximum_edit_->setValidator(maximumValidator);
    tuningRow->addWidget(maximum_edit_);
    reviewed_check_ = new QCheckBox(QStringLiteral("Reviewed"), content);
    tuningRow->addWidget(reviewed_check_);
    tuningRow->addStretch(1);
    layout->addLayout(tuningRow);

    auto* matchRow = new QHBoxLayout();
    matchRow->setSpacing(t.spacing.sm);
    matchRow->addWidget(new QLabel(QStringLiteral("Match Status:"), content));
    match_status_spin_ = new QSpinBox(content);
    match_status_spin_->setRange(0, 99999);
    match_status_spin_->setValue(0);
    match_status_spin_->setSpecialValueText(QStringLiteral("0 (any)"));
    matchRow->addWidget(match_status_spin_);
    stop_on_match_check_ = new QCheckBox(QStringLiteral("Stop on match"), content);
    matchRow->addWidget(stop_on_match_check_);
    matchRow->addWidget(new QLabel(QStringLiteral("Extract literal:"), content));
    extract_edit_ = new QLineEdit(content);
    extract_edit_->setMaxLength(255);
    extract_edit_->setPlaceholderText(QStringLiteral("bounded literal match (optional)"));
    matchRow->addWidget(extract_edit_, 1);
    layout->addLayout(matchRow);

    auto* templateTitle = new QLabel(QStringLiteral("Request Template"), content);
    templateTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    layout->addWidget(templateTitle);
    auto* templateHint = new QLabel(QStringLiteral(
        "Exact injection marker: $value$ or FUZZ (case-sensitive; do not mix)"), content);
    templateHint->setProperty("aidaTone", QStringLiteral("dim"));
    layout->addWidget(templateHint);
    template_editor_ = new QtHumanRequestEditor(content);
    QtHumanRequestEditor::Config templateConfig;
    templateConfig.stableId = QStringLiteral("network-fuzzer-template");
    templateConfig.maxBytes = 65535;
    template_editor_->setConfig(templateConfig);
    template_editor_->setMinimumHeight(editor_min_height_lines(template_editor_, 6));
    layout->addWidget(template_editor_);

    auto* runRow = new QHBoxLayout();
    runRow->setSpacing(t.spacing.sm);
    start_button_ = new widgets::AidaButton(QStringLiteral("Start Fuzzer"), content);
    start_button_->setKind(widgets::AidaButton::Kind::Primary);
    start_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    runRow->addWidget(start_button_);
    start_reason_ = new QLabel(content);
    start_reason_->setProperty("aidaTone", QStringLiteral("dim"));
    runRow->addWidget(start_reason_);
    clear_results_button_ = new widgets::AidaButton(QStringLiteral("Clear Results"), content);
    clear_results_button_->setKind(widgets::AidaButton::Kind::Ghost);
    clear_results_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    runRow->addWidget(clear_results_button_);
    running_label_ = new QLabel(content);
    running_label_->setProperty("aidaTone", QStringLiteral("titleAccent"));
    running_label_->setVisible(false);
    runRow->addWidget(running_label_);
    progress_bar_ = new QProgressBar(content);
    progress_bar_->setVisible(false);
    progress_bar_->setMinimumWidth(field_width_chars(progress_bar_, 32));
    runRow->addWidget(progress_bar_);
    stop_button_ = new widgets::AidaButton(QStringLiteral("Stop"), content);
    stop_button_->setKind(widgets::AidaButton::Kind::Destructive);
    stop_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    stop_button_->setVisible(false);
    runRow->addWidget(stop_button_);
    runRow->addStretch(1);
    layout->addLayout(runRow);

    stage_label_ = new QLabel(content);
    stage_label_->setProperty("aidaTone", QStringLiteral("dim"));
    stage_label_->setVisible(false);
    layout->addWidget(stage_label_);
    error_label_ = new QLabel(content);
    error_label_->setProperty("aidaTone", QStringLiteral("error"));
    error_label_->setVisible(false);
    layout->addWidget(error_label_);

    results_count_label_ = new QLabel(QStringLiteral("Results: 0"), content);
    results_count_label_->setProperty("aidaTone", QStringLiteral("dim"));
    layout->addWidget(results_count_label_);
    results_model_ = new FuzzerResultsModel(content);
    auto* resultsHost = new QWidget(content);
    results_stack_ = new QStackedLayout(resultsHost);
    results_stack_->setStackingMode(QStackedLayout::StackOne);
    results_stack_->setContentsMargins(0, 0, 0, 0);
    results_table_ = new QTableView(resultsHost);
    results_table_->setObjectName(QStringLiteral("aida.view.network.fuzzer.results"));
    results_table_->verticalHeader()->hide();
    results_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    results_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    results_table_->horizontalHeader()->setStretchLastSection(true);
    results_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_table_->setAlternatingRowColors(true);
    results_table_->setShowGrid(false);
    results_table_->setModel(results_model_);
    results_stack_->addWidget(results_table_);
    results_empty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No fuzzer results"),
        QStringLiteral("Configure the template and payload sets above, then Start Fuzzer; results stream in live."),
        resultsHost);
    results_empty_->setObjectName(QStringLiteral("aida.view.network.fuzzer.results_empty"));
    results_stack_->addWidget(results_empty_);
    layout->addWidget(resultsHost, 1);
    updateResultsEmptyState();

    analyze_timer_ = new QTimer(this);
    analyze_timer_->setInterval(300);
    analyze_timer_->setSingleShot(true);
    connect(analyze_timer_, &QTimer::timeout, this, [this] { analyzeTemplate(); });
    progress_timer_ = new QTimer(this);
    progress_timer_->setInterval(200);
    connect(progress_timer_, &QTimer::timeout, this, [this] {
        pullStagedTemplate();
        refreshRunState();
        refreshStartGating();
        refreshResults();
    });
    if (auto* controller = FuzzerController::instance())
        connect(controller, &FuzzerController::resultsPublished, this,
            [this](quint64) { refreshResults(); });

    loadConfigFromState();
    sets_editor_->bind(&network_view::g_state.fuzz_config.payload_sets, [this] {
        refreshStartGating();
    });
    template_editor_->setAuthority(QStringLiteral("network.fuzzer.request-template"),
        QString::fromStdString(network_view::g_state.fuzz_config.base_request));
    last_request_revision_ = network_view::g_state.fuzz_request_revision;
    analyzeTemplate();

    const auto modeChanged = [this] {
        auto& cfg = network_view::g_state.fuzz_config;
        cfg.attack_mode = sniper_radio_->isChecked()
            ? network_view::fuzzer_attack_mode_t::sniper
            : pitchfork_radio_->isChecked()
                ? network_view::fuzzer_attack_mode_t::pitchfork
                : network_view::fuzzer_attack_mode_t::clusterbomb;
        const bool sniper = cfg.attack_mode == network_view::fuzzer_attack_mode_t::sniper;
        sniper_panel_->setVisible(sniper);
        sets_editor_->setVisible(!sniper);
        if (sniper && cfg.payload_sets.empty())
            cfg.payload_sets.emplace_back();
        refreshStartGating();
    };
    connect(sniper_radio_, &QRadioButton::toggled, this, [modeChanged](bool) { modeChanged(); });
    connect(pitchfork_radio_, &QRadioButton::toggled, this, [modeChanged](bool) { modeChanged(); });
    connect(clusterbomb_radio_, &QRadioButton::toggled, this, [modeChanged](bool) { modeChanged(); });
    connect(payload_type_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        network_view::g_state.fuzz_config.payload_type = index;
        payload_hint_->setText(index == 0 ? QStringLiteral("(path to wordlist)")
            : index == 1 ? QStringLiteral("(start-end)") : QStringLiteral("(charset)"));
        refreshStartGating();
    });
    connect(payload_source_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        network_view::g_state.fuzz_config.payload_source = text.toStdString();
        refreshStartGating();
    });
    connect(host_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        network_view::g_state.fuzz_config.host = text.toStdString();
        refreshStartGating();
    });
    connect(port_spin_, &QSpinBox::valueChanged, this, [this](int value) {
        network_view::g_state.fuzz_config.port = static_cast<std::uint16_t>(value);
        refreshStartGating();
    });
    connect(tls_check_, &QCheckBox::toggled, this, [this](bool checked) {
        network_view::g_state.fuzz_config.use_tls = checked;
    });
    connect(threads_spin_, &QSpinBox::valueChanged, this, [this](int value) {
        network_view::g_state.fuzz_config.thread_count = value;
    });
    connect(delay_spin_, &QSpinBox::valueChanged, this, [this](int value) {
        network_view::g_state.fuzz_config.delay_ms = value;
    });
    connect(match_status_spin_, &QSpinBox::valueChanged, this, [this](int value) {
        network_view::g_state.fuzz_config.match_status = value;
    });
    connect(stop_on_match_check_, &QCheckBox::toggled, this, [this](bool checked) {
        network_view::g_state.fuzz_config.stop_on_match = checked;
    });
    connect(extract_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        const std::string value = text.toStdString();
        auto& cfg = network_view::g_state.fuzz_config;
        std::memset(cfg.extract_literal, 0, sizeof(cfg.extract_literal));
        std::memcpy(cfg.extract_literal, value.data(),
            (std::min)(value.size(), sizeof(cfg.extract_literal) - 1));
    });
    connect(template_editor_, &QtHumanRequestEditor::authorityChanged, this, [this] {
        network_view::g_state.fuzz_config.base_request =
            template_editor_->authority().toStdString();
        analyze_timer_->start();
    });
    connect(template_editor_, &QtHumanRequestEditor::validityChanged, this,
        [this](bool, const QString&) { refreshStartGating(); });
    connect(template_editor_, &QtHumanRequestEditor::hasUnappliedPrettyChanged, this,
        [this](bool) { refreshStartGating(); });
    connect(maximum_edit_, &QLineEdit::textEdited, this, [this](const QString& text) {
        bool ok = false;
        const qulonglong value = text.toULongLong(&ok);
        auto& cfg = network_view::g_state.fuzz_config;
        cfg.maximum_requests = ok ? (std::max)(1ULL,
            (std::min)(value, k_fuzzer_absolute_request_limit)) : 1ULL;
        cfg.maximum_requests_reviewed = false;
        reviewed_check_->setChecked(false);
        refreshStartGating();
    });
    connect(reviewed_check_, &QCheckBox::toggled, this, [this](bool checked) {
        network_view::g_state.fuzz_config.maximum_requests_reviewed = checked;
        refreshStartGating();
    });
    connect(start_button_, &QAbstractButton::clicked, this, [this] { startClicked(); });
    connect(stop_button_, &QAbstractButton::clicked, this, [this] { stopClicked(); });
    connect(clear_results_button_, &QAbstractButton::clicked, this, [this] {
        if (auto* controller = FuzzerController::instance())
            controller->clearResults();
        refreshResults();
    });
    connect(results_table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            const auto* row = results_model_->rowAt(current.isValid() ? current.row() : -1);
            if (row) {
                selected_result_index_ = row->index;
                has_result_selection_ = true;
            } else {
                has_result_selection_ = false;
            }
        });
    connect(results_model_, &QAbstractItemModel::modelReset, this, [this] {
        updateResultsEmptyState();
        if (!has_result_selection_)
            return;
        int row = -1;
        if (results_model_->findRowForIndex(selected_result_index_, row))
            results_table_->setCurrentIndex(results_model_->index(row, 0));
    });
    connect(results_model_, &QAbstractItemModel::rowsInserted, this, [this] {
        updateResultsEmptyState();
    });

    refreshRunState();
    refreshStartGating();
    refreshResults();
    setContent(content);
}

void FuzzerPane::onPaneShown() {
    pullStagedTemplate();
    analyzeTemplate();
    progress_timer_->start();
    refreshRunState();
    refreshStartGating();
    refreshResults();
}

void FuzzerPane::onPaneHidden() {
    progress_timer_->stop();
}

void FuzzerPane::analyzeTemplate() {
    template_shape_ = analyze_fuzzer_template(network_view::g_state.fuzz_config.base_request);
    refreshStartGating();
}

void FuzzerPane::pullStagedTemplate() {
    const std::uint64_t revision = network_view::g_state.fuzz_request_revision;
    if (revision == last_request_revision_)
        return;
    last_request_revision_ = revision;
    loadConfigFromState();
    template_editor_->setAuthority(
        QStringLiteral("network.fuzzer.request-template.%1").arg(revision),
        QString::fromStdString(network_view::g_state.fuzz_config.base_request));
    analyzeTemplate();
}

void FuzzerPane::refreshStartGating() {
    auto& state = network_view::g_state;
    const auto& cfg = state.fuzz_config;
    std::string reason;
    if (cfg.host.empty() || cfg.port == 0)
        reason = "Set a valid target host and port.";
    else if (cfg.base_request.empty() || !template_editor_->isValid() ||
        template_editor_->hasUnappliedPretty())
        reason = "Apply a valid raw request template before starting.";
    else if (!template_shape_.error.empty())
        reason = template_shape_.error;
    else if (cfg.payload_sets.empty() || cfg.payload_sets.size() > k_fuzzer_payload_set_limit)
        reason = "Configure 1 to 64 payload sets; " +
            std::to_string(cfg.payload_sets.size()) + " are configured.";
    else if (cfg.attack_mode == network_view::fuzzer_attack_mode_t::sniper &&
        cfg.payload_sets.size() != 1)
        reason = "Sniper requires exactly 1 payload set; " +
            std::to_string(cfg.payload_sets.size()) + " are configured.";
    else if (cfg.attack_mode != network_view::fuzzer_attack_mode_t::sniper &&
        cfg.payload_sets.size() != template_shape_.positions)
        reason = "This mode requires exactly " +
            std::to_string(template_shape_.positions) + " payload sets for " +
            std::to_string(template_shape_.positions) + " injection positions; " +
            std::to_string(cfg.payload_sets.size()) + " are configured.";
    else if (cfg.attack_mode == network_view::fuzzer_attack_mode_t::sniper &&
        cfg.payload_type != 2 && cfg.payload_source.empty())
        reason = "Configure a nonempty Sniper payload source.";
    else if (cfg.attack_mode != network_view::fuzzer_attack_mode_t::sniper &&
        std::any_of(cfg.payload_sets.begin(), cfg.payload_sets.end(),
            [](const network_view::payload_set_t& set) { return set.source.empty(); })) {
        const auto empty_set = std::find_if(cfg.payload_sets.begin(), cfg.payload_sets.end(),
            [](const network_view::payload_set_t& set) { return set.source.empty(); });
        reason = "Every configured payload set requires a nonempty source; set " +
            std::to_string(static_cast<std::size_t>(std::distance(
                cfg.payload_sets.begin(), empty_set)) + 1) +
            " is empty.";
    } else if (cfg.maximum_requests == 0 ||
        cfg.maximum_requests > k_fuzzer_absolute_request_limit ||
        !cfg.maximum_requests_reviewed)
        reason = "Review a maximum request count between 1 and 1,000,000.";
    const bool workerAvailable = state.fuzz_thread_alive.load(std::memory_order_acquire) &&
        !state.fuzz_thread_done.load(std::memory_order_acquire);
    const bool running = state.fuzz_running.load(std::memory_order_acquire);
    const bool canStart = reason.empty() && workerAvailable && !running;
    start_button_->setEnabled(canStart);
    start_reason_->setText(!workerAvailable
        ? QStringLiteral("Network fuzzer worker is unavailable.")
        : QString::fromStdString(reason));
    start_reason_->setVisible(!canStart && !running);
}

void FuzzerPane::refreshRunState() {
    auto& state = network_view::g_state;
    const bool running = state.fuzz_running.load(std::memory_order_acquire);
    start_button_->setVisible(!running);
    clear_results_button_->setVisible(!running);
    start_reason_->setVisible(false);
    running_label_->setVisible(running);
    progress_bar_->setVisible(running);
    stop_button_->setVisible(running);
    if (running) {
        const std::uint64_t progress = state.fuzz_progress.load(std::memory_order_acquire);
        const std::uint64_t total = state.fuzz_total.load(std::memory_order_acquire);
        running_label_->setText(QStringLiteral("Running: %1 / %2")
            .arg(static_cast<unsigned long long>(progress))
            .arg(static_cast<unsigned long long>(total)));
        const int maximum = total > static_cast<std::uint64_t>(
            (std::numeric_limits<int>::max)())
            ? (std::numeric_limits<int>::max)() : static_cast<int>(total);
        progress_bar_->setRange(0, maximum);
        progress_bar_->setValue(progress > static_cast<std::uint64_t>(maximum)
            ? maximum : static_cast<int>(progress));
    }
    std::string lastStage;
    std::string lastError;
    {
        std::lock_guard<std::mutex> lock(state.fuzz_mutex);
        lastStage = state.fuzz_last_stage;
        lastError = state.fuzz_last_error;
    }
    error_label_->setText(QString::fromStdString(lastError));
    error_label_->setVisible(!lastError.empty());
    stage_label_->setText(QString::fromStdString(lastStage));
    stage_label_->setVisible(lastError.empty() && !lastStage.empty() && !running);
    if (!running)
        refreshStartGating();
}

void FuzzerPane::updateResultsEmptyState() {
    if (!results_stack_ || !results_empty_ || !results_table_ || !results_model_)
        return;
    results_stack_->setCurrentWidget(results_model_->rowCount() == 0
        ? static_cast<QWidget*>(results_empty_) : static_cast<QWidget*>(results_table_));
}

void FuzzerPane::refreshResults() {
    const auto snapshot = std::atomic_load_explicit(
        &network_view::g_state.fuzz_results_snapshot, std::memory_order_acquire);
    if (snapshot && snapshot->generation == last_published_generation_)
        return;
    last_published_generation_ = snapshot ? snapshot->generation : 0;
    results_model_->adopt(snapshot);
    updateResultsEmptyState();
    const std::uint64_t retained = results_model_->retainedCount();
    const std::uint64_t dropped = results_model_->droppedCount();
    results_count_label_->setText(dropped == 0
        ? QStringLiteral("Results: %1").arg(static_cast<unsigned long long>(retained))
        : QStringLiteral("Results: %1 retained (%2 older discarded)")
            .arg(static_cast<unsigned long long>(retained))
            .arg(static_cast<unsigned long long>(dropped)));
}

void FuzzerPane::startClicked() {
    auto& state = network_view::g_state;
    persistConfigToState();
    const std::uint64_t generation = state.fuzz_run_generation.fetch_add(
        1, std::memory_order_acq_rel) + 1;
    const std::string task_id = "network.fuzzer." + std::to_string(generation);
    auto* controller = FuzzerController::instance();
    if (!controller)
        return;
    controller->beginRun(state.fuzz_config, task_id);
    state.fuzz_progress.store(0);
    state.fuzz_total.store(0);
    state.fuzz_cancel_requested.store(false, std::memory_order_release);
    aida::ui::task_center::task_registration_t registration;
    registration.id = task_id;
    registration.source = "human";
    registration.owner = "network";
    registration.owner_view = "view.network.fuzzer";
    registration.owner_action = "network.fuzzer.start";
    registration.target = state.fuzz_config.host + ":" + std::to_string(state.fuzz_config.port);
    registration.label = "Network fuzzer";
    registration.stage = "Loading payload sets";
    registration.started_ms = network_now_ms();
    registration.progress = 0.0f;
    registration.cancellation_is_safe = true;
    registration.callbacks.cancel = [generation] {
        if (network_view::g_state.fuzz_run_generation.load(std::memory_order_acquire) != generation)
            return false;
        network_view::g_state.fuzz_cancel_requested.store(true, std::memory_order_release);
        network_view::g_state.fuzz_cv.notify_all();
        return true;
    };
    registration.callbacks.focus = [] {
        (void)network_view::open_view("view.network.fuzzer");
    };
    const bool registered = aida::ui::task_center::register_task(std::move(registration));
    diag::log_tagged_fmt("network", "fuzzer_start_clicked host=%s:%u tls=%d mode=%d threads=%d delay_ms=%d match_status=%d stop_on_match=%d sets=%zu",
        state.fuzz_config.host.c_str(), state.fuzz_config.port,
        state.fuzz_config.use_tls ? 1 : 0,
        static_cast<int>(state.fuzz_config.attack_mode), state.fuzz_config.thread_count,
        state.fuzz_config.delay_ms,
        state.fuzz_config.match_status, state.fuzz_config.stop_on_match ? 1 : 0,
        state.fuzz_config.payload_sets.size());
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "[net_audit] fuzzer start host=%s:%u tls=%d mode=%d threads=%d sets=%zu",
            state.fuzz_config.host.c_str(), static_cast<unsigned>(state.fuzz_config.port),
            state.fuzz_config.use_tls ? 1 : 0,
            static_cast<int>(state.fuzz_config.attack_mode), state.fuzz_config.thread_count,
            state.fuzz_config.payload_sets.size());
        diag::log_tagged("net_audit", buf);
    }
    if (registered) {
        state.fuzz_running.store(true, std::memory_order_release);
        state.fuzz_cv.notify_one();
    } else {
        controller->rejectRun();
    }
    refreshRunState();
    refreshStartGating();
}

void FuzzerPane::stopClicked() {
    auto& state = network_view::g_state;
    const std::uint64_t progress = state.fuzz_progress.load(std::memory_order_acquire);
    const std::uint64_t total = state.fuzz_total.load(std::memory_order_acquire);
    diag::log_tagged_fmt("network", "fuzzer_stop_clicked progress=%llu total=%llu",
        static_cast<unsigned long long>(progress),
        static_cast<unsigned long long>(total));
    std::string task_id;
    {
        std::lock_guard<std::mutex> lock(state.fuzz_mutex);
        task_id = state.fuzz_task_id;
    }
    if (task_id.empty() || !aida::ui::task_center::request_cancel(task_id)) {
        state.fuzz_cancel_requested.store(true, std::memory_order_release);
        state.fuzz_cv.notify_all();
    }
}

void FuzzerPane::persistConfigToState() {
    auto& cfg = network_view::g_state.fuzz_config;
    cfg.host = host_edit_->text().toStdString();
    cfg.port = static_cast<std::uint16_t>(port_spin_->value());
    cfg.use_tls = tls_check_->isChecked();
    cfg.attack_mode = sniper_radio_->isChecked()
        ? network_view::fuzzer_attack_mode_t::sniper
        : pitchfork_radio_->isChecked()
            ? network_view::fuzzer_attack_mode_t::pitchfork
            : network_view::fuzzer_attack_mode_t::clusterbomb;
    cfg.payload_type = payload_type_combo_->currentIndex();
    cfg.payload_source = payload_source_edit_->text().toStdString();
    cfg.thread_count = threads_spin_->value();
    cfg.delay_ms = delay_spin_->value();
    cfg.match_status = match_status_spin_->value();
    cfg.stop_on_match = stop_on_match_check_->isChecked();
    const std::string extract = extract_edit_->text().toStdString();
    std::memset(cfg.extract_literal, 0, sizeof(cfg.extract_literal));
    std::memcpy(cfg.extract_literal, extract.data(),
        (std::min)(extract.size(), sizeof(cfg.extract_literal) - 1));
    cfg.base_request = template_editor_->authority().toStdString();
    bool ok = false;
    const qulonglong maximum = maximum_edit_->text().toULongLong(&ok);
    cfg.maximum_requests = ok ? (std::max)(1ULL,
        (std::min)(maximum, k_fuzzer_absolute_request_limit)) : cfg.maximum_requests;
    cfg.maximum_requests_reviewed = reviewed_check_->isChecked();
}

void FuzzerPane::loadConfigFromState() {
    const auto& cfg = network_view::g_state.fuzz_config;
    host_edit_->setText(QString::fromStdString(cfg.host));
    port_spin_->setValue(cfg.port == 0 ? 443 : cfg.port);
    tls_check_->setChecked(cfg.use_tls);
    const int mode = static_cast<int>(cfg.attack_mode);
    sniper_radio_->setChecked(mode == 0);
    pitchfork_radio_->setChecked(mode == 1);
    clusterbomb_radio_->setChecked(mode == 2);
    payload_type_combo_->setCurrentIndex(cfg.payload_type);
    payload_source_edit_->setText(QString::fromStdString(cfg.payload_source));
    payload_hint_->setText(cfg.payload_type == 0 ? QStringLiteral("(path to wordlist)")
        : cfg.payload_type == 1 ? QStringLiteral("(start-end)")
        : QStringLiteral("(charset)"));
    threads_spin_->setValue(cfg.thread_count);
    delay_spin_->setValue(cfg.delay_ms);
    match_status_spin_->setValue(cfg.match_status);
    stop_on_match_check_->setChecked(cfg.stop_on_match);
    extract_edit_->setText(QString::fromLatin1(cfg.extract_literal));
    maximum_edit_->setText(QString::number(static_cast<qulonglong>(cfg.maximum_requests)));
    reviewed_check_->setChecked(cfg.maximum_requests_reviewed);
    sniper_panel_->setVisible(cfg.attack_mode == network_view::fuzzer_attack_mode_t::sniper);
    sets_editor_->setVisible(cfg.attack_mode != network_view::fuzzer_attack_mode_t::sniper);
    if (cfg.attack_mode == network_view::fuzzer_attack_mode_t::sniper &&
        network_view::g_state.fuzz_config.payload_sets.empty())
        network_view::g_state.fuzz_config.payload_sets.emplace_back();
}

}
