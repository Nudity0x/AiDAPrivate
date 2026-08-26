#pragma once

#include <QString>

#include "core/scanner/memory_scanner.hpp"
#include "qt/bridge/aida_dialog.hpp"

class QComboBox;
class QLabel;
class QLineEdit;

namespace aida::qt::widgets {
class AidaNotice;
}

namespace aida::qt::scanner {

class AddAddressDialog : public bridge::AidaDialog {
	Q_OBJECT
public:
	AddAddressDialog(std::uint64_t address, int value_type, QWidget* parent = nullptr);

private:
	void apply();

	std::uint64_t address_ = 0;
	QLineEdit* description_ = nullptr;
	QComboBox* type_ = nullptr;
};

class EditDescriptionDialog : public bridge::AidaDialog {
	Q_OBJECT
public:
	EditDescriptionDialog(int row, std::uint64_t address, const QString& current,
		QWidget* parent = nullptr);

private:
	void apply();

	int row_ = -1;
	std::uint64_t address_ = 0;
	QLineEdit* description_ = nullptr;
};

class ChangeValueDialog : public bridge::AidaDialog {
	Q_OBJECT
public:
	explicit ChangeValueDialog(int row, QWidget* parent = nullptr);

private:
	void apply();
	void on_write_completed();

	int row_ = -1;
	std::uint64_t address_ = 0;
	std::uint32_t target_pid_ = 0;
	std::uint64_t target_epoch_ = 0;
	std::uint64_t process_creation_time_100ns_ = 0;
	memory_scanner::value_type_t value_type_ =
		memory_scanner::value_type_t::int32_val;
	bool frozen_ = false;
	QLineEdit* value_ = nullptr;
	QLabel* pending_label_ = nullptr;
	widgets::AidaNotice* error_ = nullptr;
	QPushButton* write_button_ = nullptr;
};

class ChangeTypeDialog : public bridge::AidaDialog {
	Q_OBJECT
public:
	ChangeTypeDialog(int row, int current_type, QWidget* parent = nullptr);

private:
	void apply();

	int row_ = -1;
	QComboBox* type_ = nullptr;
};

}
