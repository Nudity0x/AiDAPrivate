#pragma once

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QModelIndex>
#include <QPersistentModelIndex>
#include <QVariant>
#include <QVector>

#include <memory>
#include <vector>

#include "core/network/network_view.hpp"
#include "qt/network/network_pane_base.hpp"
#include "qt/network/shared/snapshot_table_model.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QSplitter;
class QStackedLayout;
class QTableView;
class QTimer;
class QVariantAnimation;

namespace aida::qt::widgets {
class AidaButton;
class AidaStateView;
}

namespace aida::qt::net {

class CaptureModel : public RingTableModel<network_view::packet_entry_t> {
public:
    enum Column { Index = 0, Time, Src, Dst, Proto, Info, ColumnCount };

    explicit CaptureModel(QObject* parent = nullptr);

    void applyFilterSpec(quint32 pid, quint16 port, quint8 protocol, const QString& text);
    int visibleIndexForKey(quint64 timestamp, quint32 pid, quint16 srcPort,
                           quint16 dstPort, quint32 payloadSize) const noexcept;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

protected:
    QVariant cellData(const network_view::packet_entry_t& row, int column, int role) const override;

private:
    quint32 filterPid_ = 0;
    quint16 filterPort_ = 0;
    quint8 filterProtocol_ = 0;
    QString filterText_;
};

class CaptureLiveBadge : public QWidget {
    Q_OBJECT
public:
    explicit CaptureLiveBadge(QWidget* parent = nullptr);

    void setState(bool running, bool starting, bool stopping, double packetsPerSecond);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool running_ = false;
    bool starting_ = false;
    bool stopping_ = false;
    double rate_ = 0.0;
    QVariantAnimation* pulse_ = nullptr;
    qreal pulseValue_ = 1.0;
};

class CaptureDetailWidget : public QWidget {
    Q_OBJECT
public:
    explicit CaptureDetailWidget(QWidget* parent = nullptr);

    void showPacket(const network_view::packet_entry_t& packet, int packetIndex);
    void clearPacket();

private:
    QLabel* titleLabel_ = nullptr;
    QLabel* metaLabel_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QPlainTextEdit* hexView_ = nullptr;
    widgets::AidaButton* copyButton_ = nullptr;
    QString currentPayloadText_;
};

class CapturePane : public NetworkPaneBase {
    Q_OBJECT
public:
    explicit CapturePane(QWidget* parent = nullptr);

protected:
    void onPaneShown() override;
    void onPaneHidden() override;

private:
    void onBatch(std::shared_ptr<const std::vector<network_view::packet_entry_t>> batch,
                 quint64 trimmedFromFront);
    void clearCapture();
    void updateDetail();
    void updateEmptyState();
    void openContextMenu(const QPoint& viewportPos);
    void showContextForPacket(int visibleRow, const QPoint& globalPos,
                              aida::ui::context_menu_open_origin_t origin);
    void refreshButtons();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    CaptureModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QStackedLayout* tableStack_ = nullptr;
    widgets::AidaStateView* emptyView_ = nullptr;
    CaptureLiveBadge* badge_ = nullptr;
    CaptureDetailWidget* detail_ = nullptr;
    QSplitter* splitter_ = nullptr;
    widgets::AidaButton* startStopButton_ = nullptr;
    widgets::AidaButton* clearButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QSpinBox* filterPid_ = nullptr;
    QSpinBox* filterPort_ = nullptr;
    QComboBox* filterProtocol_ = nullptr;
    QLineEdit* filterText_ = nullptr;
    QCheckBox* autoScroll_ = nullptr;
    QTimer* rateTimer_ = nullptr;
    QElapsedTimer rateClock_;
    quint64 rateWindowCount_ = 0;
    double rateEma_ = 0.0;
    QPersistentModelIndex selected_;
    bool showDetail_ = true;
};

}
