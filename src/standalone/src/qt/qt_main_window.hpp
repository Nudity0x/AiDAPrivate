#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QMainWindow>

#include <DockManager.h>

#include <functional>
#include <string>

namespace aida::qt {
namespace docking {
class AidaDockHost;
}

class AidaMainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit AidaMainWindow(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
    ~AidaMainWindow() override;

    void setExitReviewGateHook(std::function<bool()> hook);
    void setFileOpenHandler(std::function<void(const std::string&)> handler);

    ads::CDockManager* dockManager() const;
    docking::AidaDockHost* dockHost() const;

    void applyDwmBackdrop();

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    std::function<bool()> exit_review_gate_hook_;
    std::function<void(const std::string&)> file_open_handler_;
    docking::AidaDockHost* dock_host_ = nullptr;
    bool dwm_backdrop_applied_ = false;
};

HWND main_window_handle();

}
