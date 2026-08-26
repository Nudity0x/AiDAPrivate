#include "qt/scanner/scanner_dialogs.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "qt/chrome/aida_toast.hpp"
#include "qt/scanner/scanner_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_notice.hpp"

#include "helpers/diag_log.hpp"

namespace aida::qt::scanner {

namespace {

void populate_value_types(QComboBox* combo, int selected)
{
	for (int i = 0; i < static_cast<int>(memory_scanner::value_type_t::COUNT); ++i)
		combo->addItem(QString::fromLatin1(memory_scanner::value_type_name(
			static_cast<memory_scanner::value_type_t>(i))), i);
	combo->setCurrentIndex(selected);
}

QString address_caption(std::uint64_t address)
{
	return QStringLiteral("Address: 0x%1")
		.arg(address, 16, 16, QLatin1Char('0')).toUpper();
}

void polish_dialog_layout(QVBoxLayout* layout)
{
	const auto& tokens = theme::tokens();
	layout->setContentsMargins(tokens.panel.padding, tokens.panel.padding,
		tokens.panel.padding, tokens.panel.padding);
	layout->setSpacing(tokens.spacing.sm);
}

}

AddAddressDialog::AddAddressDialog(std::uint64_t address, int value_type,
	QWidget* parent)
	: bridge::AidaDialog(parent), address_(address)
{
	setObjectName(QStringLiteral("aida.memory.dialog.add_address"));
	setWindowTitle(QStringLiteral("Add to address list"));
	setModal(true);
	setAttribute(Qt::WA_DeleteOnClose);
	auto* layout = new QVBoxLayout(this);
	polish_dialog_layout(layout);
	layout->addWidget(new QLabel(address_caption(address), this));
	auto* caption = new QLabel(QStringLiteral("Description (optional)"), this);
	layout->addWidget(caption);
	description_ = new QLineEdit(this);
	description_->setObjectName(QStringLiteral("aida.memory.dialog.add_address.desc"));
	description_->setMaxLength(191);
	layout->addWidget(description_);
	layout->addWidget(new QLabel(QStringLiteral("Type"), this));
	type_ = new QComboBox(this);
	type_->setObjectName(QStringLiteral("aida.memory.dialog.add_address.type"));
	populate_value_types(type_, value_type);
	layout->addWidget(type_);
	auto* buttons = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Add"));
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::clicked, this,
		[this](QAbstractButton* button) {
			auto* box = qobject_cast<QDialogButtonBox*>(sender());
			if (box && box->buttonRole(button) == QDialogButtonBox::AcceptRole)
				apply();
			else
				reject();
		});
	connect(description_, &QLineEdit::returnPressed, this, [this] { apply(); });
	description_->setFocus();
}

void AddAddressDialog::apply()
{
	const std::string description = description_->text().toStdString();
	const auto type = static_cast<memory_scanner::value_type_t>(
		type_->currentData().toInt());
	ScannerController::instance().add_address(address_, description, type);
	diag::log_tagged_fmt("value_scan", "dialog add_submit desc='%s' vtype=%d",
		description.c_str(), static_cast<int>(type));
	chrome::toast_success(QStringLiteral("Added to address list."), 2.5);
	accept();
}

EditDescriptionDialog::EditDescriptionDialog(int row, std::uint64_t address,
	const QString& current, QWidget* parent)
	: bridge::AidaDialog(parent), row_(row), address_(address)
{
	setObjectName(QStringLiteral("aida.memory.dialog.edit_description"));
	setWindowTitle(QStringLiteral("Edit description"));
	setModal(true);
	setAttribute(Qt::WA_DeleteOnClose);
	auto* layout = new QVBoxLayout(this);
	polish_dialog_layout(layout);
	layout->addWidget(new QLabel(address_caption(address), this));
	layout->addWidget(new QLabel(QStringLiteral("Description"), this));
	description_ = new QLineEdit(this);
	description_->setObjectName(
		QStringLiteral("aida.memory.dialog.edit_description.input"));
	description_->setMaxLength(191);
	description_->setText(current);
	layout->addWidget(description_);
	auto* buttons = new QDialogButtonBox(
		QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::clicked, this,
		[this](QAbstractButton* button) {
			auto* box = qobject_cast<QDialogButtonBox*>(sender());
			if (box && box->buttonRole(button) == QDialogButtonBox::AcceptRole)
				apply();
			else
				reject();
		});
	connect(description_, &QLineEdit::returnPressed, this, [this] { apply(); });
	description_->setFocus();
	description_->selectAll();
}

void EditDescriptionDialog::apply()
{
	ScannerController::instance().edit_description(row_,
		description_->text().toStdString());
	accept();
}

ChangeValueDialog::ChangeValueDialog(int row, QWidget* parent)
	: bridge::AidaDialog(parent), row_(row)
{
	setObjectName(QStringLiteral("aida.memory.dialog.change_value"));
	setWindowTitle(QStringLiteral("Change value"));
	setModal(true);
	setAttribute(Qt::WA_DeleteOnClose);
	auto& state = memory_scanner::g_state;
	QString current_value;
	{
		std::lock_guard<std::mutex> lock(state.address_mutex);
		if (row >= 0 && row < static_cast<int>(state.address_list.size())) {
			const auto& entry = state.address_list[static_cast<std::size_t>(row)];
			address_ = entry.address;
			value_type_ = entry.value_type;
			target_pid_ = entry.target_pid;
			target_epoch_ = entry.target_epoch;
			process_creation_time_100ns_ =
				entry.target_identity.process.creation_time_100ns;
			frozen_ = entry.frozen;
			current_value = QString::fromStdString(memory_scanner::format_value(
				entry.last_value, entry.value_type));
		}
	}
	auto* layout = new QVBoxLayout(this);
	polish_dialog_layout(layout);
	auto* address_label = new QLabel(QStringLiteral("%1  (%2)")
		.arg(QStringLiteral("0x%1").arg(address_, 16, 16, QLatin1Char('0')).toUpper(),
			QString::fromLatin1(memory_scanner::value_type_name(value_type_))), this);
	layout->addWidget(address_label);
	layout->addWidget(new QLabel(QStringLiteral("New value"), this));
	value_ = new QLineEdit(this);
	value_->setObjectName(QStringLiteral("aida.memory.dialog.change_value.input"));
	value_->setMaxLength(255);
	value_->setText(current_value);
	layout->addWidget(value_);
	pending_label_ = new QLabel(
		QStringLiteral("Writing, verifying, and retaining rollback bytes..."), this);
	pending_label_->setEnabled(false);
	pending_label_->setVisible(false);
	layout->addWidget(pending_label_);
	error_ = new widgets::AidaNotice(QStringLiteral("Write rejected"), QString(),
		widgets::AidaSemantic::Error, this);
	error_->setObjectName(QStringLiteral("aida.memory.dialog.change_value.error"));
	error_->hide();
	layout->addWidget(error_);
	auto* buttons = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	write_button_ = buttons->button(QDialogButtonBox::Ok);
	write_button_->setText(QStringLiteral("Write"));
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::clicked, this,
		[this](QAbstractButton* button) {
			auto* box = qobject_cast<QDialogButtonBox*>(sender());
			if (box && box->buttonRole(button) == QDialogButtonBox::AcceptRole)
				apply();
			else
				reject();
		});
	connect(&ScannerController::instance(), &ScannerController::writeCompleted,
		this, [this] { on_write_completed(); });
	connect(&ScannerController::instance(), &ScannerController::stateChanged,
		this, [this] {
			write_button_->setEnabled(
				!ScannerController::instance().write_pending());
			pending_label_->setVisible(
				ScannerController::instance().write_pending());
		});
	value_->setFocus();
	value_->selectAll();
}

void ChangeValueDialog::apply()
{
	auto& controller = ScannerController::instance();
	const auto runtime = controller.runtime_snapshot();
	const auto context = memory_interaction::capture_address(runtime, address_,
		row_, frozen_, {}, target_pid_, target_epoch_, process_creation_time_100ns_);
	const auto capability = memory_interaction::evaluate(
		memory_interaction::capability_t::change_value, context, runtime);
	if (!capability.enabled) {
		chrome::toast_warning(QString::fromLatin1(capability.disabled_reason));
		error_->setMessage(QString::fromLatin1(capability.disabled_reason));
		error_->show();
		return;
	}
	const auto expected = memory_scanner::parse_value(value_->text().toStdString(),
		value_type_, memory_scanner::g_state.config.hex_input);
	if (expected.empty()) {
		chrome::toast_error(QStringLiteral("Value is invalid for the selected type."));
		error_->setMessage(QStringLiteral("Value is invalid for the selected type."));
		error_->show();
		return;
	}
	std::string error;
	if (!controller.request_value_write(context, value_type_, expected, error)) {
		chrome::toast_error(QString::fromStdString(error), 5.0);
		error_->setMessage(QString::fromStdString(error));
		error_->show();
		return;
	}
	diag::log_tagged_fmt("value_scan",
		"dialog edit_value_queued addr=0x%llX pid=%u revision=%llu",
		static_cast<unsigned long long>(address_), context.target_pid,
		static_cast<unsigned long long>(context.scan_revision));
	diag::log_tagged("scan_audit",
		"[scan_audit] memory_scanner write_value queued_for_worker_readback");
	write_button_->setEnabled(false);
	pending_label_->setVisible(true);
}

void ChangeValueDialog::on_write_completed()
{
	auto& controller = ScannerController::instance();
	const auto result = controller.last_write_result();
	if (!result)
		return;
	if (result->verified && result->context.index == row_)
		accept();
}

ChangeTypeDialog::ChangeTypeDialog(int row, int current_type, QWidget* parent)
	: bridge::AidaDialog(parent), row_(row)
{
	setObjectName(QStringLiteral("aida.memory.dialog.change_type"));
	setWindowTitle(QStringLiteral("Change type"));
	setModal(true);
	setAttribute(Qt::WA_DeleteOnClose);
	auto& state = memory_scanner::g_state;
	std::uint64_t address = 0;
	{
		std::lock_guard<std::mutex> lock(state.address_mutex);
		if (row >= 0 && row < static_cast<int>(state.address_list.size()))
			address = state.address_list[static_cast<std::size_t>(row)].address;
	}
	auto* layout = new QVBoxLayout(this);
	polish_dialog_layout(layout);
	layout->addWidget(new QLabel(address_caption(address), this));
	type_ = new QComboBox(this);
	type_->setObjectName(QStringLiteral("aida.memory.dialog.change_type.type"));
	populate_value_types(type_, current_type);
	layout->addWidget(type_);
	auto* buttons = new QDialogButtonBox(
		QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::clicked, this,
		[this](QAbstractButton* button) {
			auto* box = qobject_cast<QDialogButtonBox*>(sender());
			if (box && box->buttonRole(button) == QDialogButtonBox::ApplyRole)
				apply();
			else
				reject();
		});
	type_->setFocus();
}

void ChangeTypeDialog::apply()
{
	ScannerController::instance().change_type(row_,
		static_cast<memory_scanner::value_type_t>(type_->currentData().toInt()));
	accept();
}

}
