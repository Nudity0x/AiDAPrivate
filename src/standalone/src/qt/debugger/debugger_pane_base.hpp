#pragma once

#include <QWidget>

#include <QPointer>
#include <QString>

#include <cstdint>
#include <vector>

class QHBoxLayout;
class QVBoxLayout;
class QShowEvent;
class QHideEvent;
class QContextMenuEvent;
class QKeyEvent;
class QTableView;

namespace aida::qt::widgets {
class AidaStateView;
}

namespace aida::qt::debugger {

class DebuggerActionBridge;
class DebuggerTableModelBase;

// Shared pane chrome: optional toolbar row + content + the empty-target
// callout. Context menus open via contextMenuEvent/aboutToShow revalidation;
// Qt::Key_Menu and Shift+F10 map to the retained context-key behavior.
class DebuggerPaneBase : public QWidget {
    Q_OBJECT
public:
    explicit DebuggerPaneBase(QWidget* parent = nullptr);
    ~DebuggerPaneBase() override;

    void setToolBar(QWidget* toolbar);
    void setContent(QWidget* content);
    QWidget* content() const noexcept { return content_; }

    void setEmptyTargetText(const QString& title, const QString& body);

    // Attached-but-empty callout (no rows yet); shown instead of the content
    // when hasTargetContent() is true but hasContentRows() is false.
    void setEmptyContentText(const QString& title, const QString& body);

    void setLoadingText(const QString& title, const QString& body);
    void setErrorText(const QString& title, const QString& body);

    void setSessionDriven(bool driven);

    // The stable view id (e.g. "view.debug.breakpoints") used as the retained
    // context owner view.
    void setOwnerViewId(const char* id);
    const char* ownerViewId() const noexcept { return owner_view_id_; }

public Q_SLOTS:
    void refreshPane();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

    // Pane hooks.
    virtual void onShown() {}
    virtual void onHidden() {}
    virtual void onSessionTick();
    virtual void onSessionStateChanged(int status, quint32 pid,
                                       quint64 stopGeneration);
    // Default: refresh-driven empty-target gating; override for panes with
    // richer callouts.
    virtual bool hasTargetContent() const;
    // Default: attached panes always have rows; table panes override with a
    // model rowCount check so the attached-but-empty callout can show.
    virtual bool hasContentRows() const;
    virtual bool isContentLoading() const;
    virtual bool contentError(QString* detail) const;
    virtual void openContextMenuAt(const QPoint& globalPos);

    void updateOverlayState();
    bool paneVisible_ = false;

    // Standard table wiring: fixed defaultSectionSize before setModel, row
    // selection, custom context menu -> selection bridge + entity menu.
    void wireTable(QTableView* view, DebuggerTableModelBase* model,
                   bool extendedSelection = true);

    void registerContextMenuTable(QTableView* view,
                                  DebuggerTableModelBase* model);

private:
    enum class OverlayMode { none, no_target, no_rows, loading, error };

    struct WiredContextTable {
        QPointer<QTableView> view;
        QPointer<DebuggerTableModelBase> model;
    };

    QVBoxLayout* layout_ = nullptr;
    QWidget* toolbar_host_ = nullptr;
    QHBoxLayout* toolbar_layout_ = nullptr;
    QWidget* content_ = nullptr;
    widgets::AidaStateView* empty_view_ = nullptr;
    QString empty_title_;
    QString empty_body_;
    QString empty_content_title_;
    QString empty_content_body_;
    QString loading_title_ = QStringLiteral("Loading");
    QString loading_body_ = QStringLiteral(
        "The debugger engine is populating this pane.");
    QString error_title_ = QStringLiteral("Snapshot failed");
    QString error_body_ = QStringLiteral(
        "The debugger engine could not publish a snapshot; the pane retries automatically.");
    OverlayMode overlay_mode_ = OverlayMode::none;
    bool session_driven_ = true;
    const char* owner_view_id_ = "view.debug.cpu";
    std::vector<WiredContextTable> context_tables_;
};

}
