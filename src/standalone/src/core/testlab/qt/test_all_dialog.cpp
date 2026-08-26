#include "test_all_dialog.hpp"

#include "test_all_controller.hpp"

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/theme/aida_tokens.hpp"

#include <QCloseEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <chrono>

namespace aida::qt::testlab {

namespace {

	constexpr int k_log_block_limit = 512;

	QColor severityColor(test_all_features::overlay_log_severity_t severity) {
		const auto& t = theme::tokens();
		switch (severity) {
		case test_all_features::overlay_log_severity_t::success: return t.success;
		case test_all_features::overlay_log_severity_t::warning: return t.warning;
		case test_all_features::overlay_log_severity_t::error:   return t.error;
		default: return QColor();
		}
	}

	void setLabelVariant(QWidget* widget, const char* variant) {
		if (variant != nullptr)
			widget->setProperty("aidaVariant", QString::fromLatin1(variant));
		else
			widget->setProperty("aidaVariant", QVariant());
		theme::stylesheet::repolish(widget);
	}

}

TestAllDialog::TestAllDialog(TestAllController* controller, QWidget* parent)
	: bridge::AidaDialog(parent), controller_(controller)
{
	setObjectName(QStringLiteral("aida.test_all.dialog"));
	setWindowTitle(QStringLiteral("Test All Features"));
	setModal(false);
	setMinimumSize(600, 400);
	resize(720, 520);

	const auto& t = theme::tokens();
	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
	root->setSpacing(t.spacing.sm);

	auto* button_row = new QHBoxLayout();
	button_row->setSpacing(t.spacing.md);

	start_button_ = new QPushButton(QStringLiteral("TEST ALL FEATURES"), this);
	start_button_->setObjectName(QStringLiteral("aida.test_all.start"));
	start_button_->setProperty("aidaVariant", "primary");
	start_button_->setDefault(true);
	start_button_->setToolTip(QStringLiteral("Start the full feature sweep (Enter)"));
	connect(start_button_, &QPushButton::clicked, this, [this] {
		if (controller_ != nullptr)
			controller_->requestStart();
	});
	button_row->addWidget(start_button_);

	cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
	cancel_button_->setObjectName(QStringLiteral("aida.test_all.cancel"));
	cancel_button_->setToolTip(QStringLiteral("Request cancellation of the running sweep"));
	connect(cancel_button_, &QPushButton::clicked, this, [this] {
		if (controller_ != nullptr)
			controller_->requestCancel();
	});
	button_row->addWidget(cancel_button_);

	phase_label_ = new QLabel(this);
	phase_label_->setObjectName(QStringLiteral("aida.test_all.phase"));
	phase_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	phase_label_->installEventFilter(this);
	button_row->addWidget(phase_label_, 1);
	root->addLayout(button_row);

	target_label_ = new QLabel(this);
	target_label_->setObjectName(QStringLiteral("aida.test_all.target"));
	root->addWidget(target_label_);

	auto* counter_row = new QHBoxLayout();
	counter_row->setSpacing(t.spacing.md);

	total_label_ = new QLabel(this);
	total_label_->setObjectName(QStringLiteral("aida.test_all.total"));
	counter_row->addWidget(total_label_);

	passed_label_ = new QLabel(this);
	passed_label_->setObjectName(QStringLiteral("aida.test_all.passed"));
	passed_label_->setProperty("aidaVariant", QStringLiteral("success"));
	counter_row->addWidget(passed_label_);

	failed_label_ = new QLabel(this);
	failed_label_->setObjectName(QStringLiteral("aida.test_all.failed"));
	failed_label_->setProperty("aidaVariant", QStringLiteral("error"));
	counter_row->addWidget(failed_label_);

	skipped_label_ = new QLabel(this);
	skipped_label_->setObjectName(QStringLiteral("aida.test_all.skipped"));
	skipped_label_->setProperty("aidaVariant", QStringLiteral("warning"));
	counter_row->addWidget(skipped_label_);

	progress_ = new QProgressBar(this);
	progress_->setObjectName(QStringLiteral("aida.test_all.progress"));
	progress_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	progress_->setTextVisible(false);
	counter_row->addWidget(progress_, 1);
	root->addLayout(counter_row);

	log_caption_ = new QLabel(QStringLiteral("TEST LOG"), this);
	log_caption_->setObjectName(QStringLiteral("aida.test_all.log_label"));
	root->addWidget(log_caption_);

	log_ = new QPlainTextEdit(this);
	log_->setObjectName(QStringLiteral("aida.test_all.log"));
	log_->setReadOnly(true);
	log_->setFont(theme::fonts::codeRegular());
	log_->setMaximumBlockCount(k_log_block_limit);
	log_->setLineWrapMode(QPlainTextEdit::NoWrap);
	log_->setPlaceholderText(QStringLiteral(
		"No run yet. TEST ALL FEATURES writes per-feature results, diagnostics, and crash evidence here."));
	root->addWidget(log_, 1);

	log_path_label_ = new QLabel(this);
	log_path_label_->setObjectName(QStringLiteral("aida.test_all.log_path"));
	log_path_label_->setFont(theme::fonts::caption());
	log_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	root->addWidget(log_path_label_);

	target_log_path_label_ = new QLabel(this);
	target_log_path_label_->setObjectName(QStringLiteral("aida.test_all.target_log_path"));
	target_log_path_label_->setFont(theme::fonts::caption());
	target_log_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	root->addWidget(target_log_path_label_);

	if (controller_ != nullptr) {
		connect(controller_, &TestAllController::snapshotChanged, this, [this] { refreshFromController(); });
		log_path_label_->setText(QStringLiteral("Log file: %1").arg(QString::fromStdString(controller_->fullTestLogPath())));
		target_log_path_label_->setText(QStringLiteral("Target log: %1").arg(QString::fromStdString(controller_->fullTestTargetLogPath())));
	}
	refreshFromController();
}

void TestAllDialog::closeEvent(QCloseEvent* event) {
	test_all_features::set_overlay_visible(false);
	QDialog::closeEvent(event);
}

void TestAllDialog::reject() {
	test_all_features::set_overlay_visible(false);
	QDialog::reject();
}

bool TestAllDialog::eventFilter(QObject* watched, QEvent* event) {
	if (watched == phase_label_ && event->type() == QEvent::Resize)
		updatePhaseLabel();
	return QDialog::eventFilter(watched, event);
}

void TestAllDialog::updatePhaseLabel() {
	if (controller_ == nullptr)
		return;
	const std::string& phase = controller_->phaseLabel();
	if (phase.empty()) {
		phase_label_->clear();
		phase_label_->setVisible(false);
		return;
	}
	const QString phase_text = QStringLiteral("Phase: %1").arg(QString::fromStdString(phase));
	const QFontMetrics metrics(phase_label_->font());
	const int available = phase_label_->width();
	phase_label_->setText(available > 0 ? metrics.elidedText(phase_text, Qt::ElideMiddle, available) : phase_text);
	phase_label_->setToolTip(phase_text);
	phase_label_->setVisible(true);
}

void TestAllDialog::refreshFromController() {
	if (controller_ == nullptr)
		return;
	const auto refresh_start = std::chrono::steady_clock::now();

	const auto& run = controller_->runSnapshot();
	start_button_->setEnabled(!run.running);
	cancel_button_->setEnabled(run.running);

	updatePhaseLabel();

	if (run.target_pid != 0 && run.driver_attached) {
		target_label_->setText(QStringLiteral("Target pid: %1   Driver: ATTACHED").arg(run.target_pid));
		setLabelVariant(target_label_, "success");
	} else if (run.target_pid != 0) {
		target_label_->setText(QStringLiteral("Target pid: %1   Driver: NOT ATTACHED").arg(run.target_pid));
		setLabelVariant(target_label_, "warning");
	} else {
		target_label_->setText(QStringLiteral("Target pid: (none)   Driver: not attached"));
		setLabelVariant(target_label_, nullptr);
	}

	total_label_->setText(QStringLiteral("Total: %1").arg(run.total));
	passed_label_->setText(QStringLiteral("Passed: %1").arg(run.passed));
	failed_label_->setText(QStringLiteral("Failed: %1").arg(run.failed));
	skipped_label_->setText(QStringLiteral("Skipped: %1").arg(run.skipped));
	const int done = run.passed + run.failed + run.skipped;
	if (run.total > 0) {
		progress_->setRange(0, run.total);
		progress_->setValue(done);
		progress_->setVisible(true);
	} else {
		progress_->setVisible(false);
	}
	const char* progress_variant = run.running ? "accent" : (run.failed > 0 ? "error" : "success");
	if (progress_->property("aidaVariant") != QVariant(QString::fromUtf8(progress_variant))) {
		progress_->setProperty("aidaVariant", QString::fromUtf8(progress_variant));
		theme::stylesheet::repolish(progress_);
	}

	log_caption_->setText(controller_->logTailBusy()
		? QStringLiteral("TEST LOG (snapshot busy)")
		: QStringLiteral("TEST LOG"));

	rebuildLogText();

	const std::uint64_t elapsed_us = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - refresh_start).count());
	test_all_features::note_overlay_render_elapsed(elapsed_us);
}

void TestAllDialog::rebuildLogText() {
	if (controller_ == nullptr)
		return;
	const std::uint64_t version = controller_->logTailVersion();
	if (version == applied_log_version_)
		return;
	applied_log_version_ = version;

	const auto& lines = controller_->logTail();
	QString text;
	text.reserve(static_cast<qsizetype>(lines.size()) * 160);
	for (const auto& line : lines) {
		QString line_text = QString::fromStdString(line.text);
		while (line_text.endsWith(u'\n') || line_text.endsWith(u'\r'))
			line_text.chop(1);
		text.append(line_text);
		text.append(u'\n');
	}

	QScrollBar* bar = log_->verticalScrollBar();
	const int previous_value = bar->value();
	const bool was_at_bottom = previous_value >= bar->maximum();

	log_->setPlainText(text);

	QList<QTextEdit::ExtraSelection> selections;
	QTextDocument* doc = log_->document();
	QTextBlock block = doc->begin();
	std::size_t index = 0;
	while (block.isValid() && index < lines.size()) {
		const QColor color = severityColor(lines[index].severity);
		if (color.isValid()) {
			QTextEdit::ExtraSelection selection;
			selection.cursor = QTextCursor(block);
			selection.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
			selection.format.setForeground(color);
			selections.append(selection);
		}
		block = block.next();
		++index;
	}
	log_->setExtraSelections(selections);

	if (was_at_bottom)
		bar->setValue(bar->maximum());
	else
		bar->setValue((std::min)(previous_value, bar->maximum()));
}

TestAllDialog* createTestAllDialog(TestAllController* controller, QWidget* parent) {
	return new TestAllDialog(controller, parent);
}

}
