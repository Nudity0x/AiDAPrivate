#pragma once

#include <QTableView>

#include <vector>

namespace aida::qt::scanner {

class AddressListModel;
class AddressItemDelegate;
class MemoryInteractionBridge;

class AddressListView : public QTableView {
	Q_OBJECT
public:
	explicit AddressListView(QWidget* parent = nullptr);
	~AddressListView() override;

	void bind(AddressListModel* model, MemoryInteractionBridge* bridge,
		const QString& owner_view_id);

	AddressListModel* address_model() const noexcept { return model_; }

Q_SIGNALS:
	void freezeToggleRequested(int row);
	void editDescriptionRequested(int row);
	void changeTypeRequested(int row);
	void changeValueRequested(int row);
	void removeRequested(const std::vector<int>& rows);
	void contextMenuRequested(const QPoint& global_pos, int row, int origin);

protected:
	void keyPressEvent(QKeyEvent* event) override;

private:
	AddressListModel* model_ = nullptr;
	AddressItemDelegate* delegate_ = nullptr;
	MemoryInteractionBridge* bridge_ = nullptr;
	QString owner_view_id_;
};

}
