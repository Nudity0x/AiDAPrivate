#pragma once

#include <QTableView>

class QVariantAnimation;

namespace aida::qt::scanner {

class ScanResultsModel;
class ScanResultDelegate;
class MemoryInteractionBridge;

class ScanResultsView : public QTableView {
	Q_OBJECT
public:
	explicit ScanResultsView(QWidget* parent = nullptr);
	~ScanResultsView() override;

	void bind(ScanResultsModel* model, MemoryInteractionBridge* bridge,
		const QString& owner_view_id);

	ScanResultsModel* scan_model() const noexcept { return model_; }
	void sync_sort_indicator(int field, bool descending);

Q_SIGNALS:
	void sortRequested(int logical_column);
	void rowActivated(int source_row);
	void contextMenuRequested(const QPoint& global_pos, int source_row, int origin);

protected:
	void resizeEvent(QResizeEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;

private:
	void applyResponsiveColumns();
	void tickFlash();

	ScanResultsModel* model_ = nullptr;
	ScanResultDelegate* delegate_ = nullptr;
	MemoryInteractionBridge* bridge_ = nullptr;
	QVariantAnimation* flash_anim_ = nullptr;
	QString owner_view_id_;
};

}
