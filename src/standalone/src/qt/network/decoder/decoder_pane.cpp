#include "qt/network/decoder/decoder_pane.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSizePolicy>
#include <QSplitter>
#include <QVBoxLayout>

#include <mutex>
#include <utility>
#include <cstdio>

#include "core/analysis/decoder_pipeline.hpp"
#include "core/infra/executor.hpp"
#include "helpers/diag_log.hpp"
#include "qt/network/bounded_plain_text_edit.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"

namespace aida::qt::net {

namespace {

std::mutex g_decoder_stage_mutex;
bool g_decoder_stage_pending = false;
std::string g_decoder_stage_text;
std::function<void()> g_decoder_stage_hook;

}

void decoder_stage_input(const std::string& utf8_text) {
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(g_decoder_stage_mutex);
        g_decoder_stage_pending = true;
        g_decoder_stage_text = utf8_text;
        hook = g_decoder_stage_hook;
    }
    if (hook)
        hook();
}

void set_decoder_stage_hook(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(g_decoder_stage_mutex);
    g_decoder_stage_hook = std::move(hook);
}

DecoderPipelineModel::DecoderPipelineModel(QObject* parent)
    : QAbstractListModel(parent) {}

int DecoderPipelineModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(steps_.size());
}

QVariant DecoderPipelineModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const step_t* step = stepAt(index.row());
    if (!step)
        return {};
    if (role == Qt::DisplayRole)
        return QStringLiteral("%1. %2")
            .arg(index.row() + 1)
            .arg(QString::fromStdString(step->transform_name));
    return {};
}

void DecoderPipelineModel::multiData(const QModelIndex& index,
                                     QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

void DecoderPipelineModel::addStep(const std::string& transformId) {
    const int row = static_cast<int>(steps_.size());
    beginInsertRows(QModelIndex(), row, row);
    steps_.push_back(step_t{transformId, {}});
    endInsertRows();
}

bool DecoderPipelineModel::removeRow(int row) {
    if (row < 0 || row >= static_cast<int>(steps_.size()))
        return false;
    beginRemoveRows(QModelIndex(), row, row);
    steps_.erase(steps_.begin() + static_cast<ptrdiff_t>(row));
    endRemoveRows();
    return true;
}

void DecoderPipelineModel::clearSteps() {
    if (steps_.empty())
        return;
    beginResetModel();
    steps_.clear();
    endResetModel();
}

const DecoderPipelineModel::step_t* DecoderPipelineModel::stepAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(steps_.size()))
        return nullptr;
    return &steps_[static_cast<std::size_t>(row)];
}

std::vector<DecoderPipelineModel::step_t> DecoderPipelineModel::steps() const {
    return steps_;
}

DecoderPane::DecoderPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.decoder"));

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);

    auto* pipelinePanel = new QWidget(splitter);
    auto* pipelineLayout = new QVBoxLayout(pipelinePanel);
    pipelineLayout->setContentsMargins(0, 0, 0, 0);
    pipelineLayout->setSpacing(t.spacing.sm);
    auto* pipelineTitle = new QLabel(QStringLiteral("Pipeline"), pipelinePanel);
    pipelineTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    pipelineLayout->addWidget(pipelineTitle);

    auto* addRow = new QHBoxLayout();
    addRow->setSpacing(t.spacing.sm);
    transform_combo_ = new QComboBox(pipelinePanel);
    addRow->addWidget(transform_combo_, 1);
    add_button_ = new widgets::AidaButton(QStringLiteral("Add"), pipelinePanel);
    add_button_->setKind(widgets::AidaButton::Kind::Secondary);
    add_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    addRow->addWidget(add_button_);
    pipelineLayout->addLayout(addRow);

    pipeline_model_ = new DecoderPipelineModel(pipelinePanel);
    pipeline_view_ = new QListView(pipelinePanel);
    pipeline_view_->setObjectName(QStringLiteral("aida.view.network.decoder.pipeline"));
    pipeline_view_->setModel(pipeline_model_);
    pipeline_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    pipeline_view_->setUniformItemSizes(true);
    pipeline_view_->setAlternatingRowColors(true);
    pipelineLayout->addWidget(pipeline_view_, 1);

    auto* pipelineButtons = new QHBoxLayout();
    pipelineButtons->setSpacing(t.spacing.sm);
    remove_button_ = new widgets::AidaButton(QStringLiteral("Remove"), pipelinePanel);
    remove_button_->setKind(widgets::AidaButton::Kind::Ghost);
    remove_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    pipelineButtons->addWidget(remove_button_);
    clear_button_ = new widgets::AidaButton(QStringLiteral("Clear Pipeline"), pipelinePanel);
    clear_button_->setKind(widgets::AidaButton::Kind::Ghost);
    clear_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    pipelineButtons->addWidget(clear_button_);
    pipelineButtons->addStretch(1);
    execute_button_ = new widgets::AidaButton(QStringLiteral("Execute"), pipelinePanel);
    execute_button_->setKind(widgets::AidaButton::Kind::Primary);
    execute_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    pipelineButtons->addWidget(execute_button_);
    pipelineLayout->addLayout(pipelineButtons);
    splitter->addWidget(pipelinePanel);

    auto* ioPanel = new QWidget(splitter);
    auto* ioLayout = new QVBoxLayout(ioPanel);
    ioLayout->setContentsMargins(0, 0, 0, 0);
    ioLayout->setSpacing(t.spacing.xs);
    auto* ioSplitter = new QSplitter(Qt::Vertical, ioPanel);
    ioSplitter->setOpaqueResize(true);
    ioSplitter->setChildrenCollapsible(false);

    auto* inputPanel = new QWidget(ioSplitter);
    auto* inputLayout = new QVBoxLayout(inputPanel);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->setSpacing(t.spacing.xs);
    auto* inputTitle = new QLabel(QStringLiteral("Input"), inputPanel);
    inputTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    inputLayout->addWidget(inputTitle);
    input_ = new BoundedPlainTextEdit(16383, inputPanel);
    input_->setFont(theme::fonts::codeRegular());
    inputLayout->addWidget(input_, 1);
    ioSplitter->addWidget(inputPanel);

    auto* outputPanel = new QWidget(ioSplitter);
    auto* outputLayout = new QVBoxLayout(outputPanel);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->setSpacing(t.spacing.xs);
    auto* outputHeader = new QHBoxLayout();
    outputHeader->setSpacing(t.spacing.sm);
    auto* outputTitle = new QLabel(QStringLiteral("Output"), outputPanel);
    outputTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    outputHeader->addWidget(outputTitle);
    outputHeader->addStretch(1);
    status_label_ = new QLabel(outputPanel);
    status_label_->setObjectName(QStringLiteral("aida.view.network.decoder.status"));
    status_label_->setProperty("aidaTone", QStringLiteral("accent"));
    status_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status_label_->setVisible(false);
    outputHeader->addWidget(status_label_);
    outputLayout->addLayout(outputHeader);
    output_ = new QPlainTextEdit(outputPanel);
    output_->setReadOnly(true);
    output_->setFont(theme::fonts::codeRegular());
    output_->setPlaceholderText(QStringLiteral("Execute the pipeline to see the transformed output"));
    outputLayout->addWidget(output_, 1);
    ioSplitter->addWidget(outputPanel);
    ioSplitter->setStretchFactor(0, 45);
    ioSplitter->setStretchFactor(1, 55);
    ioLayout->addWidget(ioSplitter, 1);
    splitter->addWidget(ioPanel);
    splitter->setStretchFactor(0, 30);
    splitter->setStretchFactor(1, 70);

    layout->addWidget(splitter, 1);

    connect(add_button_, &QAbstractButton::clicked, this, [this] {
        const int index = transform_combo_->currentIndex();
        if (index < 0)
            return;
        const std::string id = transform_combo_->itemData(index).toString().toStdString();
        if (id.empty())
            return;
        diag::log_tagged_fmt("network", "decoder_step_added name='%s' pipeline_size=%zu",
            id.c_str(), pipeline_model_->rowCount() + 1);
        pipeline_model_->addStep(id);
    });
    connect(remove_button_, &QAbstractButton::clicked, this, [this] {
        const QModelIndex current = pipeline_view_->currentIndex();
        if (!current.isValid())
            return;
        const int row = current.row();
        if (!pipeline_model_->removeRow(row))
            return;
        const int count = pipeline_model_->rowCount();
        if (count == 0) {
            pipeline_view_->clearSelection();
            pipeline_view_->setCurrentIndex(QModelIndex());
        } else if (row < count) {
            pipeline_view_->setCurrentIndex(pipeline_model_->index(row, 0));
        } else {
            pipeline_view_->setCurrentIndex(pipeline_model_->index(count - 1, 0));
        }
    });
    connect(clear_button_, &QAbstractButton::clicked, this, [this] {
        diag::log_tagged_fmt("network", "decoder_pipeline_cleared prev_size=%zu",
            pipeline_model_->rowCount());
        pipeline_model_->clearSteps();
        pipeline_view_->clearSelection();
        pipeline_view_->setCurrentIndex(QModelIndex());
    });
    connect(execute_button_, &QAbstractButton::clicked, this, [this] { executePipeline(); });

    rebuildTransformCombo();

    {
        std::string staged;
        bool pending = false;
        {
            std::lock_guard<std::mutex> lock(g_decoder_stage_mutex);
            pending = g_decoder_stage_pending;
            if (pending) {
                staged = std::move(g_decoder_stage_text);
                g_decoder_stage_text.clear();
                g_decoder_stage_pending = false;
            }
        }
        set_decoder_stage_hook([pane = QPointer<DecoderPane>(this)] {
            if (!pane)
                return;
            QMetaObject::invokeMethod(pane.data(), [pane] {
                std::string stagedNow;
                {
                    std::lock_guard<std::mutex> lock(g_decoder_stage_mutex);
                    if (!g_decoder_stage_pending)
                        return;
                    stagedNow = std::move(g_decoder_stage_text);
                    g_decoder_stage_text.clear();
                    g_decoder_stage_pending = false;
                }
                if (pane)
                    pane->stageInput(QString::fromStdString(stagedNow));
            }, Qt::QueuedConnection);
        });
        if (pending)
            stageInput(QString::fromStdString(staged));
    }

    setContent(content);
}

DecoderPane::~DecoderPane() {
    std::lock_guard<std::mutex> lock(g_decoder_stage_mutex);
    g_decoder_stage_hook = nullptr;
}

void DecoderPane::onPaneShown() {
    rebuildTransformCombo();
}

void DecoderPane::stageInput(const QString& text) {
    input_->setPlainText(text);
}

void DecoderPane::rebuildTransformCombo() {
    const auto transforms = decoder_pipeline::registry::instance().all();
    QVariant current;
    if (transform_combo_->count() > 0)
        current = transform_combo_->currentData();
    transform_combo_->clear();
    int restore = -1;
    for (const auto* transform : transforms) {
        if (!transform)
            continue;
        const QString id = QString::fromStdString(transform->id);
        transform_combo_->addItem(QString::fromStdString(transform->name), id);
        if (current.isValid() && current.toString() == id)
            restore = transform_combo_->count() - 1;
    }
    if (restore >= 0)
        transform_combo_->setCurrentIndex(restore);
}

void DecoderPane::executePipeline() {
    const QByteArray inputBytes = input_->toPlainText().toUtf8();
    const std::vector<DecoderPipelineModel::step_t> steps = pipeline_model_->steps();
    std::vector<uint8_t> data(inputBytes.begin(), inputBytes.end());
    diag::log_tagged_fmt("network", "decoder_execute steps=%zu input_size=%zu",
        steps.size(), data.size());
    const std::uint64_t generation = execute_generation_.fetch_add(
        1, std::memory_order_acq_rel) + 1;
    execute_button_->setEnabled(false);
    status_label_->setText(QStringLiteral("Executing..."));
    set_label_tone(status_label_, "accent");
    status_label_->setVisible(true);

    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "decoder_execute";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    QPointer<DecoderPane> pane(this);
    submission.body = [pane, generation, data = std::move(data), steps]() mutable {
        bool failed = false;
        std::string output;
        for (const auto& step : steps) {
            std::map<std::string, std::string> params;
            for (const auto& p : step.params)
                params[p.first] = p.second;
            auto result = decoder_pipeline::apply_single(step.transform_name, data, params);
            if (result.success) {
                data = std::move(result.data);
            } else {
                output = "Error at '" + step.transform_name + "': " + result.error;
                diag::log_tagged_fmt("network", "decoder_execute_step_failed step='%s' err='%s'",
                    step.transform_name.c_str(), result.error.c_str());
                data.clear();
                failed = true;
                break;
            }
        }
        if (!failed)
            diag::log_tagged_fmt("network", "decoder_execute_done out_size=%zu", data.size());
        bool present = failed;
        if (!data.empty()) {
            bool printable = true;
            for (uint8_t b : data) {
                if (b != '\n' && b != '\r' && b != '\t' && (b < 32 || b > 126)) {
                    printable = false;
                    break;
                }
            }
            if (printable) {
                output.assign(data.begin(), data.end());
            } else {
                output.clear();
                for (size_t off = 0; off < data.size(); off += 16) {
                    char line[128];
                    int pos = snprintf(line, sizeof(line), "%04zx  ", off);
                    for (size_t j = 0; j < 16; j++) {
                        if (off + j < data.size())
                            pos += snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos),
                                "%02x ", static_cast<unsigned>(data[off + j]));
                        else
                            pos += snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), "   ");
                    }
                    for (size_t j = 0; j < 16 && off + j < data.size(); j++) {
                        uint8_t c = data[off + j];
                        line[pos++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
                    }
                    line[pos] = '\0';
                    output += line;
                    output += '\n';
                }
            }
            present = true;
        }
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(),
            [pane, generation, present, failed, output = std::move(output)]() {
                if (pane->execute_generation_.load(std::memory_order_acquire) != generation)
                    return;
                pane->execute_button_->setEnabled(true);
                if (failed) {
                    pane->output_->clear();
                    const QString full = QString::fromStdString(output);
                    QString shown = full;
                    if (shown.size() > 160)
                        shown = shown.left(160) + QStringLiteral("...");
                    pane->status_label_->setText(shown);
                    pane->status_label_->setToolTip(full);
                    set_label_tone(pane->status_label_, "error");
                    pane->status_label_->setVisible(true);
                } else {
                    if (present)
                        pane->output_->setPlainText(QString::fromStdString(output));
                    pane->status_label_->setVisible(false);
                }
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        execute_generation_.fetch_add(1, std::memory_order_acq_rel);
        execute_button_->setEnabled(true);
        status_label_->setVisible(false);
    }
}

}
