#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "qt/network/network_pane_base.hpp"

class QComboBox;
class QLabel;
class QListView;
class QPlainTextEdit;

namespace aida::qt::widgets {
class AidaButton;
}

namespace aida::qt::net {

class BoundedPlainTextEdit;

// Steps of the decoder pipeline, one row per decoder_step_t
// (network_view.hpp:528-531). Mutations go through the model so the view
// stays in sync; execution snapshots a copy for the worker.
class DecoderPipelineModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum class Column { Label = 0 };

    explicit DecoderPipelineModel(QObject* parent = nullptr);

    struct step_t {
        std::string transform_name;
        std::vector<std::pair<std::string, std::string>> params;
    };

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    void multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;

    void addStep(const std::string& transformId);
    bool removeRow(int row);
    void clearSteps();
    const step_t* stepAt(int row) const noexcept;
    std::vector<step_t> steps() const;

private:
    std::vector<step_t> steps_;
};

// DecoderPane ports render_decoder (network_view.cpp:7203-7439). The
// NUL-joined combo statics and the decorative inter-step arrow drawlist are
// dropped per plan 11 section 19; list order conveys the flow. Execute runs
// the apply_single chain off the GUI thread under the execute_generation_
// fence with queued delivery.
class DecoderPane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit DecoderPane(QWidget* parent = nullptr);
    ~DecoderPane() override;

    void stageInput(const QString& text);

protected:
    void onPaneShown() override;

private:
    void rebuildTransformCombo();
    void executePipeline();

    DecoderPipelineModel* pipeline_model_ = nullptr;
    QListView* pipeline_view_ = nullptr;
    QComboBox* transform_combo_ = nullptr;
    widgets::AidaButton* add_button_ = nullptr;
    widgets::AidaButton* remove_button_ = nullptr;
    widgets::AidaButton* clear_button_ = nullptr;
    widgets::AidaButton* execute_button_ = nullptr;
    BoundedPlainTextEdit* input_ = nullptr;
    QPlainTextEdit* output_ = nullptr;
    QLabel* status_label_ = nullptr;
    std::atomic<std::uint64_t> execute_generation_{0};
};

// Staging seam for the "Send to Decoder" exchange action
// (network_view.cpp decoder dispatch): stores the payload and notifies the
// live pane; the pane drains pending staged input on creation.
void decoder_stage_input(const std::string& utf8_text);
void set_decoder_stage_hook(std::function<void()> hook);

}
