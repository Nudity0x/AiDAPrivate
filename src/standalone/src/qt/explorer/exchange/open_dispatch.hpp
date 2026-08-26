#pragma once

#include <QObject>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

class QWidget;

#include "core/analysis/workspace/analysis_workspace.hpp"
#include "core/analysis/workspace/workspace_registry.hpp"

class QTimer;

namespace aida::qt::documents {
class AidaDocumentController;
}

namespace aida::qt::editor {
class AidaImageView;
}

namespace aida::qt::explorer {

class AidaExplorerModel;
struct HexPreviewOperation;

class AidaOpenDispatch : public QObject {
    Q_OBJECT
public:
    explicit AidaOpenDispatch(QObject* parent = nullptr);
    ~AidaOpenDispatch() override;

    static AidaOpenDispatch& instance();

    void setViewFocusHook(std::function<void(const std::string& view_id)> hook);
    void setDocumentController(documents::AidaDocumentController* controller) noexcept;
    void setImageView(editor::AidaImageView* view) noexcept;
    void setExplorerModel(AidaExplorerModel* model) noexcept;
    void setDialogParent(QWidget* parent) noexcept;
    editor::AidaImageView* imageView() const noexcept { return image_view_; }
    documents::AidaDocumentController* documentController() const noexcept { return documents_; }

    void cancelHexFallback();
    void retryHexFallback();
    void dismissHexFallback();
    void presentHexFallbackDialog();

    void openPath(const std::string& path);
    void requestOpenConfirmation(const std::string& path);
    void asyncHexFallback(const std::string& path, bool archive);

    bool hexFallbackLoading() const;
    QString hexFallbackPath() const;
    QString hexFallbackError() const;

Q_SIGNALS:
    void openConfirmationRequested(const QString& path, const QString& filename);
    void hexFallbackStateChanged();

private Q_SLOTS:
    void onConfirmPollTimer();

private:
    void completeHexFallbackSuccess(
        std::shared_ptr<HexPreviewOperation> operation, std::uint64_t serial,
        std::string path,
        aida::analysis::workspace_result_t<std::shared_ptr<aida::analysis::analysis_workspace_t>> result);
    void completeHexFallbackFailure(std::uint64_t serial, std::string path, std::string error);
    void completeHexFallbackCancelled(std::uint64_t serial, std::string path);
    void finishHexFallbackFailure(const std::shared_ptr<HexPreviewOperation>& operation,
                                  std::uint64_t serial, const std::string& path, std::string error);
    void cancelHexFallbackOperation(const std::shared_ptr<HexPreviewOperation>& operation,
                                    std::uint64_t serial, const std::string& path);
    void timeoutHexFallbackOperation(const std::shared_ptr<HexPreviewOperation>& operation,
                                     std::uint64_t serial, const std::string& path);
    bool hexFallbackIsCurrent(std::uint64_t serial);
    void presentOpenConfirmation(const std::string& path, const std::string& filename);

    std::function<void(const std::string&)> view_focus_hook_;
    documents::AidaDocumentController* documents_ = nullptr;
    editor::AidaImageView* image_view_ = nullptr;
    AidaExplorerModel* explorer_ = nullptr;
    QWidget* dialog_parent_ = nullptr;
    QTimer* confirm_poll_timer_ = nullptr;

    std::mutex hex_mutex_;
    std::uint64_t hex_serial_ = 0;
    bool hex_loading_ = false;
    bool hex_cancellation_requested_ = false;
    bool hex_cancelled_ = false;
    bool hex_archive_ = false;
    std::string hex_path_;
    std::string hex_error_;
    aida::infra::taskflow_runtime::job_handle_t hex_task_;
    std::optional<aida::analysis::workspace_admission_handle_t> hex_admission_;
};

void open_path(const std::string& path);
bool image_active();

}
