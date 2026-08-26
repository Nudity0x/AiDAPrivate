#include "test_lab_widget.hpp"

#include "../test_lab_format.hpp"

#include "qt/theme/aida_tokens.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_stylesheet.hpp"
#include "qt/widgets/aida_badge.hpp"
#include "qt/widgets/aida_notice.hpp"
#include "qt/widgets/aida_state_view.hpp"
#include "qt/widgets/aida_paint_utils.hpp"
#include "qt/bridge/clipboard.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

namespace aida::qt::testlab {

namespace {

	constexpr int k_evidence_tail_blocks = 512;

	int code_line_height_px() {
		return qRound(QFontMetricsF(aida::qt::theme::fonts::codeRegular()).height());
	}

	int code_view_height_for_lines(int lines) {
		const auto& t = aida::qt::theme::tokens();
		return code_line_height_px() * lines + 2 * t.spacing.xs + 2 * t.panel.border;
	}

	bool summary_matches_filter(TestLabFeatureModel::FilterMode mode,
		const TestLabController::feature_run_summary_t& s) {
		switch (mode) {
			case TestLabFeatureModel::FilterAll:     return true;
			case TestLabFeatureModel::FilterFailed:  return s.state == test_lab::run_state_e::complete && !s.skipped && !s.ok;
			case TestLabFeatureModel::FilterPassed:  return s.state == test_lab::run_state_e::complete && !s.skipped && s.ok;
			case TestLabFeatureModel::FilterSkipped: return s.state == test_lab::run_state_e::complete && s.skipped;
			case TestLabFeatureModel::FilterRunning: return s.state == test_lab::run_state_e::running;
			case TestLabFeatureModel::FilterPending: return s.state == test_lab::run_state_e::idle;
		}
		return true;
	}

	const char* driver_label(test_lab::driver_e d) {
		switch (d) {
			case test_lab::driver_e::whoswho:  return "WHO";
			case test_lab::driver_e::driverless: return "SAFE";
		}
		return "?";
	}

	const char* result_state_label(test_lab::run_state_e s, test_lab::outcome_e outcome) {
		switch (s) {
			case test_lab::run_state_e::idle: return "Idle";
			case test_lab::run_state_e::running: return "Running";
			case test_lab::run_state_e::complete:
				switch (outcome) {
					case test_lab::outcome_e::not_run: return "Not run";
					case test_lab::outcome_e::missing: return "Missing";
					case test_lab::outcome_e::passed: return "Passed";
					case test_lab::outcome_e::failed: return "Failed";
					case test_lab::outcome_e::timed_out: return "Timed out";
					case test_lab::outcome_e::crashed: return "Crashed";
					case test_lab::outcome_e::cancelled: return "Cancelled";
					case test_lab::outcome_e::malformed_result: return "Malformed result";
					case test_lab::outcome_e::integrity_failure: return "Integrity failure";
				}
		}
		return "Idle";
	}

	const char* result_state_badge(test_lab::run_state_e s, test_lab::outcome_e outcome) {
		switch (s) {
			case test_lab::run_state_e::idle: return "IDLE";
			case test_lab::run_state_e::running: return "RUN";
			case test_lab::run_state_e::complete:
				switch (outcome) {
					case test_lab::outcome_e::not_run: return "N/R";
					case test_lab::outcome_e::missing: return "MISS";
					case test_lab::outcome_e::passed: return "PASS";
					case test_lab::outcome_e::failed: return "FAIL";
					case test_lab::outcome_e::timed_out: return "TIME";
					case test_lab::outcome_e::crashed: return "CRASH";
					case test_lab::outcome_e::cancelled: return "CANCEL";
					case test_lab::outcome_e::malformed_result: return "BAD";
					case test_lab::outcome_e::integrity_failure: return "INTEG";
				}
		}
		return "IDLE";
	}

	std::string format_elapsed(std::uint64_t elapsed_us) {
		char buf[64];
		if (elapsed_us >= 1000000ull) {
			std::snprintf(buf, sizeof(buf), "%.2f s", static_cast<double>(elapsed_us) / 1000000.0);
		} else if (elapsed_us >= 1000ull) {
			std::snprintf(buf, sizeof(buf), "%.2f ms", static_cast<double>(elapsed_us) / 1000.0);
		} else {
			std::snprintf(buf, sizeof(buf), "%llu us", static_cast<unsigned long long>(elapsed_us));
		}
		return std::string(buf);
	}

	std::string format_result_summary(const test_lab::feature_t& f, const test_lab::result_t& r) {
		std::string out;
		out.reserve(512);
		out.append("feature: ");
		out.append(f.category != nullptr ? f.category : "?");
		out.append("/");
		out.append(f.name != nullptr ? f.name : "?");
		out.append("\n");
		out.append("driver: ");
		out.append(driver_label(f.driver));
		out.append("\n");
		out.append("status: ");
		out.append(result_state_label(r.state.load(std::memory_order_acquire),
			test_lab::effective_outcome(r, f.driver == test_lab::driver_e::driverless)));
		out.append("\n");
		char buf[128];
		std::snprintf(buf, sizeof(buf), "ntstatus: %s (0x%08X)\n",
			test_lab_format::ntstatus_to_string(r.ntstatus),
			static_cast<unsigned>(static_cast<std::uint32_t>(r.ntstatus)));
		out.append(buf);
		std::snprintf(buf, sizeof(buf), "bytes_returned: %u\nelapsed_us: %llu\nraw_size: %zu\nparsed_fields: %zu\n",
			static_cast<unsigned>(r.bytes_returned),
			static_cast<unsigned long long>(r.elapsed_us),
			r.raw.size(),
			r.parsed.size());
		out.append(buf);
		if (!r.error.empty()) {
			out.append("error: ");
			out.append(r.error);
			out.append("\n");
		}
		return out;
	}

	std::string format_raw_hex(const std::vector<std::uint8_t>& raw) {
		std::string out;
		out.reserve(raw.size() * 3);
		char tmp[8];
		for (std::size_t i = 0; i < raw.size(); ++i) {
			std::snprintf(tmp, sizeof(tmp), "%02X ", static_cast<unsigned>(raw[i]));
			out.append(tmp);
		}
		return out;
	}

	std::string format_parsed_fields(const std::vector<test_lab::parsed_field_t>& parsed) {
		std::string out;
		for (const auto& p : parsed) {
			out.append(p.label);
			out.append(": ");
			out.append(p.value);
			out.append("\n");
		}
		return out;
	}

	QColor status_dot_color(test_lab::run_state_e s, test_lab::outcome_e outcome) {
		const auto& t = aida::qt::theme::tokens();
		switch (s) {
			case test_lab::run_state_e::idle:     return t.text_dim;
			case test_lab::run_state_e::running:  return t.accent;
			case test_lab::run_state_e::complete:
				return outcome == test_lab::outcome_e::passed ? t.success :
					((outcome == test_lab::outcome_e::not_run || outcome == test_lab::outcome_e::cancelled) ? t.warning : t.error);
		}
		return t.text_dim;
	}

	QColor driver_badge_color(test_lab::driver_e d) {
		const auto& t = aida::qt::theme::tokens();
		switch (d) {
			case test_lab::driver_e::whoswho:  return t.accent;
			case test_lab::driver_e::driverless: return t.success;
		}
		return t.text_dim;
	}

	widgets::AidaSemantic status_semantic(test_lab::run_state_e s, test_lab::outcome_e outcome) {
		using widgets::AidaSemantic;
		switch (s) {
			case test_lab::run_state_e::idle:    return AidaSemantic::Neutral;
			case test_lab::run_state_e::running: return AidaSemantic::Accent;
			case test_lab::run_state_e::complete:
				if (outcome == test_lab::outcome_e::passed) return AidaSemantic::Success;
				if (outcome == test_lab::outcome_e::not_run || outcome == test_lab::outcome_e::cancelled) return AidaSemantic::Warning;
				return AidaSemantic::Error;
		}
		return AidaSemantic::Neutral;
	}

	QString format_u32_field(std::uint32_t v, bool hex) {
		char buf[24];
		if (hex) std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(v));
		else     std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(v));
		return QString::fromUtf8(buf);
	}

	QString format_u64_field(std::uint64_t v, bool hex) {
		char buf[32];
		if (hex) std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(v));
		else     std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
		return QString::fromUtf8(buf);
	}

	bool parse_u32_field(const QString& text, bool hex, std::uint32_t& out) {
		QString s = text.trimmed();
		if (hex && s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) s = s.mid(2);
		if (s.isEmpty()) return false;
		bool ok = false;
		const unsigned long long v = s.toULongLong(&ok, hex ? 16 : 10);
		if (!ok || v > 0xFFFFFFFFull) return false;
		out = static_cast<std::uint32_t>(v);
		return true;
	}

	bool parse_u64_field(const QString& text, bool hex, std::uint64_t& out) {
		QString s = text.trimmed();
		if (hex && s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) s = s.mid(2);
		if (s.isEmpty()) return false;
		bool ok = false;
		const unsigned long long v = s.toULongLong(&ok, hex ? 16 : 10);
		if (!ok) return false;
		out = static_cast<std::uint64_t>(v);
		return true;
	}

	class qt_input_form_t final : public test_lab::input_form_t {
	public:
		qt_input_form_t(QFormLayout* layout, test_lab::state_t* state, QWidget* parent,
			std::function<void()> structure_changed)
			: layout_(layout), state_(state), parent_(parent), structure_changed_(std::move(structure_changed)) {}

		void u32(const char* label, std::uint32_t* field, bool hex) override {
			QLineEdit* edit = make_scalar_edit(field, hex);
			layout_->addRow(QString::fromUtf8(label != nullptr ? label : ""), edit);
			last_was_action_ = false;
		}

		void u64(const char* label, std::uint64_t* field, bool hex) override {
			QLineEdit* edit = make_scalar_edit(field, hex);
			layout_->addRow(QString::fromUtf8(label != nullptr ? label : ""), edit);
			last_was_action_ = false;
		}

		void i32(const char* label, std::int32_t* field, std::int32_t lo, std::int32_t hi) override {
			QSpinBox* box = new QSpinBox(parent_);
			box->setObjectName(QStringLiteral("aida.view.test_lab.input.spin"));
			box->setRange(lo, hi);
			box->setValue(*field);
			box->setToolTip(QString::fromUtf8(label != nullptr ? label : ""));
			QObject::connect(box, &QSpinBox::valueChanged, parent_, [field](int v) { *field = static_cast<std::int32_t>(v); });
			layout_->addRow(QString::fromUtf8(label != nullptr ? label : ""), box);
			last_was_action_ = false;
		}

		void text(const char* label, std::string* field, std::size_t max_len) override {
			QLineEdit* edit = new QLineEdit(parent_);
			edit->setObjectName(QStringLiteral("aida.view.test_lab.input.text"));
			const int cap = max_len > 0 ? static_cast<int>(max_len - 1) : 32767;
			edit->setMaxLength(cap);
			edit->setText(QString::fromStdString(*field));
			edit->setToolTip(QString::fromUtf8(label != nullptr ? label : ""));
			QObject::connect(edit, &QLineEdit::textChanged, parent_, [field](const QString& t) { *field = t.toStdString(); });
			layout_->addRow(QString::fromUtf8(label != nullptr ? label : ""), edit);
			last_was_action_ = false;
		}

		void combo(const char* label, std::uint32_t* field, const char* const* items, std::size_t count) override {
			QComboBox* box = new QComboBox(parent_);
			box->setObjectName(QStringLiteral("aida.view.test_lab.input.combo"));
			box->setToolTip(QString::fromUtf8(label != nullptr ? label : ""));
			for (std::size_t i = 0; i < count; ++i)
				box->addItem(QString::fromUtf8(items[i] != nullptr ? items[i] : ""));
			const std::uint32_t cur = *field;
			box->setCurrentIndex(cur < count ? static_cast<int>(cur) : 0);
			const std::function<void()> structure_changed = structure_changed_;
			QObject::connect(box, &QComboBox::activated, parent_, [field, structure_changed](int idx) {
				if (idx < 0) return;
				*field = static_cast<std::uint32_t>(idx);
				if (structure_changed) structure_changed();
			});
			layout_->addRow(QString::fromUtf8(label != nullptr ? label : ""), box);
			last_was_action_ = false;
		}

		void checkbox_u32(const char* label, std::uint32_t* field, std::uint32_t on, std::uint32_t off) override {
			QCheckBox* box = new QCheckBox(QString::fromUtf8(label != nullptr ? label : ""), parent_);
			box->setObjectName(QStringLiteral("aida.view.test_lab.input.check"));
			box->setChecked(*field == on);
			QObject::connect(box, &QCheckBox::toggled, parent_, [field, on, off](bool checked) {
				*field = checked ? on : off;
			});
			layout_->addRow(box);
			last_was_action_ = false;
		}

		void note(const char* text) override {
			QLabel* l = new QLabel(QString::fromUtf8(text != nullptr ? text : ""), parent_);
			l->setObjectName(QStringLiteral("aida.view.test_lab.input.note"));
			l->setWordWrap(true);
			l->setProperty("aidaVariant", "secondary");
			layout_->addRow(l);
			last_was_action_ = false;
		}

		void action(const char* label, void(*fill)(test_lab::state_t&)) override {
			if (!last_was_action_ || last_action_layout_ == nullptr) {
				QWidget* row = new QWidget(parent_);
				QHBoxLayout* h = new QHBoxLayout(row);
				h->setContentsMargins(0, 0, 0, 0);
				h->setSpacing(aida::qt::theme::tokens().spacing.sm);
				last_action_row_ = row;
				last_action_layout_ = h;
				layout_->addRow(row);
				last_was_action_ = true;
			}
			QPushButton* btn = new QPushButton(QString::fromUtf8(label != nullptr ? label : ""), last_action_row_);
			btn->setObjectName(QStringLiteral("aida.view.test_lab.input.action"));
			test_lab::state_t* state = state_;
			QObject::connect(btn, &QPushButton::clicked, parent_, [state, fill]() {
				if (fill != nullptr && state != nullptr) fill(*state);
			});
			last_action_layout_->addWidget(btn);
		}

	private:
		QLineEdit* make_scalar_edit(std::uint32_t* field, bool hex) {
			QLineEdit* edit = make_scalar_edit_shell(field, hex);
			edit->setText(format_u32_field(*field, hex));
			QObject::connect(edit, &QLineEdit::editingFinished, parent_, [edit, field, hex]() {
				std::uint32_t value = 0;
				if (parse_u32_field(edit->text(), hex, value)) *field = value;
				else edit->setText(format_u32_field(*field, hex));
			});
			return edit;
		}

		QLineEdit* make_scalar_edit(std::uint64_t* field, bool hex) {
			QLineEdit* edit = make_scalar_edit_shell(field, hex);
			edit->setText(format_u64_field(*field, hex));
			QObject::connect(edit, &QLineEdit::editingFinished, parent_, [edit, field, hex]() {
				std::uint64_t value = 0;
				if (parse_u64_field(edit->text(), hex, value)) *field = value;
				else edit->setText(format_u64_field(*field, hex));
			});
			return edit;
		}

		template <typename FieldT>
		QLineEdit* make_scalar_edit_shell(FieldT* field, bool hex) {
			(void)field;
			QLineEdit* edit = new QLineEdit(parent_);
			edit->setObjectName(QStringLiteral("aida.view.test_lab.input.scalar"));
			edit->setFont(aida::qt::theme::fonts::codeRegular());
			edit->setToolTip(hex
				? QStringLiteral("Hexadecimal value (0x-prefixed)")
				: QStringLiteral("Decimal value"));
			QRegularExpressionValidator* v = new QRegularExpressionValidator(
				QRegularExpression(hex ? QStringLiteral("0x[0-9A-Fa-f]+") : QStringLiteral("[0-9]+")), edit);
			edit->setValidator(v);
			return edit;
		}

		QFormLayout* layout_ = nullptr;
		test_lab::state_t* state_ = nullptr;
		QWidget* parent_ = nullptr;
		std::function<void()> structure_changed_;
		QWidget* last_action_row_ = nullptr;
		QHBoxLayout* last_action_layout_ = nullptr;
		bool last_was_action_ = false;
	};

	quintptr encode_header_id(int cat) {
		return (static_cast<quintptr>(cat + 1) << 1) | 1u;
	}

	quintptr encode_feature_id(int feature) {
		return (static_cast<quintptr>(feature + 1) << 1);
	}

}

TestLabFeatureModel::TestLabFeatureModel(QObject* parent) : QAbstractItemModel(parent) {
	rebuild();
}

void TestLabFeatureModel::rebuild() {
	beginResetModel();
	categories_.clear();
	position_by_feature_.clear();
	const auto& features = test_lab::all_features();
	position_by_feature_.assign(features.size(), { -1, -1 });
	std::string current_cat;
	for (std::size_t i = 0; i < features.size(); ++i) {
		if (!matchesFilter(static_cast<int>(i))) continue;
		const auto& f = features[i];
		const char* cat = (f.category != nullptr) ? f.category : "Uncategorized";
		if (current_cat != cat || categories_.empty()) {
			current_cat = cat;
			category_t c;
			c.name = current_cat;
			categories_.push_back(std::move(c));
		}
		const int cat_idx = static_cast<int>(categories_.size()) - 1;
		categories_.back().features.push_back(static_cast<int>(i));
		position_by_feature_[i] = { cat_idx, static_cast<int>(categories_.back().features.size()) - 1 };
	}
	endResetModel();
}

void TestLabFeatureModel::setStatusFilter(FilterMode mode) {
	if (filter_ == mode) return;
	filter_ = mode;
	rebuild();
}

bool TestLabFeatureModel::matchesFilter(int feature_index) const {
	if (filter_ == FilterAll) return true;
	TestLabController::feature_run_summary_t summary;
	if (feature_index >= 0 && feature_index < static_cast<int>(summaries_.size()))
		summary = summaries_[static_cast<std::size_t>(feature_index)];
	return summary_matches_filter(filter_, summary);
}

QString TestLabFeatureModel::countsText(const category_t& cat) const {
	if (cat.pass == 0 && cat.fail == 0 && cat.skip == 0 && cat.running == 0)
		return QString();
	char buf[96];
	if (cat.running > 0) {
		std::snprintf(buf, sizeof(buf), "%d run / %d pass / %d fail / %d skip",
			cat.running, cat.pass, cat.fail, cat.skip);
	} else {
		std::snprintf(buf, sizeof(buf), "%d pass / %d fail / %d skip",
			cat.pass, cat.fail, cat.skip);
	}
	return QString::fromUtf8(buf);
}

QString TestLabFeatureModel::tooltipForFeature(int feature_index) const {
	const auto& features = test_lab::all_features();
	if (feature_index < 0 || feature_index >= static_cast<int>(features.size())) return QString();
	const auto& f = features[static_cast<std::size_t>(feature_index)];
	QString tip = QString::fromUtf8(f.category != nullptr ? f.category : "?") + QLatin1Char('/') +
		QString::fromUtf8(f.name != nullptr ? f.name : "?");
	test_lab::run_state_e rs = test_lab::run_state_e::idle;
	test_lab::outcome_e routcome = test_lab::outcome_e::not_run;
	TestLabController::feature_run_summary_t summary;
	if (feature_index < static_cast<int>(summaries_.size()))
		summary = summaries_[static_cast<std::size_t>(feature_index)];
	rs = summary.state;
	routcome = summary.outcome;
	tip += QString::fromUtf8("\nStatus: ") + QString::fromUtf8(result_state_label(rs, routcome));
	if (rs == test_lab::run_state_e::complete) {
		tip += QString::fromUtf8("\nNTSTATUS: ") + QString::fromUtf8(test_lab_format::ntstatus_to_string(summary.ntstatus));
		tip += QString::fromUtf8("\nBytes: %1  Elapsed: %2")
			.arg(static_cast<unsigned>(summary.bytes_returned))
			.arg(QString::fromStdString(format_elapsed(summary.elapsed_us)));
		if (summary.log_line_index != 0)
			tip += QString::fromUtf8("\nEvidence line: %1").arg(static_cast<unsigned long long>(summary.log_line_index));
		if (!summary.error.empty())
			tip += QString::fromUtf8("\nDetail: ") + QString::fromStdString(summary.error);
	}
	return tip;
}

int TestLabFeatureModel::featureIndexFor(const QModelIndex& index) const {
	if (!index.isValid()) return -1;
	const quintptr id = index.internalId();
	if ((id & 1u) != 0u) return -1;
	return static_cast<int>(id >> 1) - 1;
}

QModelIndex TestLabFeatureModel::indexForFeature(int feature_index) const {
	if (feature_index < 0 || feature_index >= static_cast<int>(position_by_feature_.size())) return QModelIndex();
	const auto& pos = position_by_feature_[static_cast<std::size_t>(feature_index)];
	if (pos.first < 0) return QModelIndex();
	return createIndex(pos.second, 0, encode_feature_id(feature_index));
}

QModelIndex TestLabFeatureModel::index(int row, int column, const QModelIndex& parent) const {
	if (column != 0 || row < 0) return QModelIndex();
	if (!parent.isValid()) {
		if (row >= static_cast<int>(categories_.size())) return QModelIndex();
		return createIndex(row, 0, encode_header_id(row));
	}
	const quintptr pid = parent.internalId();
	if ((pid & 1u) == 0u) return QModelIndex();
	const int cat = static_cast<int>(pid >> 1) - 1;
	if (cat < 0 || cat >= static_cast<int>(categories_.size())) return QModelIndex();
	const auto& c = categories_[static_cast<std::size_t>(cat)];
	if (row >= static_cast<int>(c.features.size())) return QModelIndex();
	return createIndex(row, 0, encode_feature_id(c.features[static_cast<std::size_t>(row)]));
}

QModelIndex TestLabFeatureModel::parent(const QModelIndex& child) const {
	if (!child.isValid()) return QModelIndex();
	const quintptr id = child.internalId();
	if ((id & 1u) != 0u) return QModelIndex();
	const int feature = static_cast<int>(id >> 1) - 1;
	if (feature < 0 || feature >= static_cast<int>(position_by_feature_.size())) return QModelIndex();
	const int cat = position_by_feature_[static_cast<std::size_t>(feature)].first;
	if (cat < 0) return QModelIndex();
	return createIndex(cat, 0, encode_header_id(cat));
}

int TestLabFeatureModel::rowCount(const QModelIndex& parent) const {
	if (!parent.isValid()) return static_cast<int>(categories_.size());
	const quintptr pid = parent.internalId();
	if ((pid & 1u) == 0u) return 0;
	const int cat = static_cast<int>(pid >> 1) - 1;
	if (cat < 0 || cat >= static_cast<int>(categories_.size())) return 0;
	return static_cast<int>(categories_[static_cast<std::size_t>(cat)].features.size());
}

int TestLabFeatureModel::columnCount(const QModelIndex& parent) const {
	(void)parent;
	return 1;
}

Qt::ItemFlags TestLabFeatureModel::flags(const QModelIndex& index) const {
	Qt::ItemFlags f = QAbstractItemModel::flags(index);
	if (index.isValid() && (index.internalId() & 1u) != 0u)
		f &= ~Qt::ItemIsSelectable;
	return f;
}

QVariant TestLabFeatureModel::data(const QModelIndex& index, int role) const {
	if (!index.isValid()) return QVariant();
	const quintptr id = index.internalId();
	const bool header = (id & 1u) != 0u;
	if (header) {
		const int cat = static_cast<int>(id >> 1) - 1;
		if (cat < 0 || cat >= static_cast<int>(categories_.size())) return QVariant();
		const category_t& c = categories_[static_cast<std::size_t>(cat)];
		switch (role) {
			case Qt::DisplayRole: return QString::fromStdString(c.name);
			case IsHeaderRole: return true;
			case CountsTextRole: return countsText(c);
			default: return QVariant();
		}
	}
	const int feature = static_cast<int>(id >> 1) - 1;
	const auto& features = test_lab::all_features();
	if (feature < 0 || feature >= static_cast<int>(features.size())) return QVariant();
	const auto& f = features[static_cast<std::size_t>(feature)];
	TestLabController::feature_run_summary_t summary;
	if (feature < static_cast<int>(summaries_.size()))
		summary = summaries_[static_cast<std::size_t>(feature)];
	switch (role) {
		case Qt::DisplayRole: return QString::fromUtf8(f.name != nullptr ? f.name : "");
		case Qt::ToolTipRole: return tooltipForFeature(feature);
		case IsHeaderRole: return false;
		case FeatureIndexRole: return feature;
		case RunStateRole: return static_cast<int>(summary.state);
		case OutcomeRole: return static_cast<int>(summary.outcome);
		case OkRole: return summary.ok;
		case SkippedRole: return summary.skipped;
		case DriverRole: return static_cast<int>(f.driver);
		default: return QVariant();
	}
}

void TestLabFeatureModel::multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const {
	if (!index.isValid()) {
		for (QModelRoleData& d : roleDataSpan) d.setData(QVariant());
		return;
	}
	const quintptr id = index.internalId();
	const bool header = (id & 1u) != 0u;
	const auto& features = test_lab::all_features();
	QString display;
	QVariant counts;
	int feature = -1;
	test_lab::driver_e driver = test_lab::driver_e::whoswho;
	TestLabController::feature_run_summary_t summary;
	if (header) {
		const int cat = static_cast<int>(id >> 1) - 1;
		if (cat >= 0 && cat < static_cast<int>(categories_.size())) {
			display = QString::fromStdString(categories_[static_cast<std::size_t>(cat)].name);
			counts = countsText(categories_[static_cast<std::size_t>(cat)]);
		}
	} else {
		feature = static_cast<int>(id >> 1) - 1;
		if (feature >= 0 && feature < static_cast<int>(features.size())) {
			display = QString::fromUtf8(features[static_cast<std::size_t>(feature)].name != nullptr
				? features[static_cast<std::size_t>(feature)].name : "");
			driver = features[static_cast<std::size_t>(feature)].driver;
			if (feature < static_cast<int>(summaries_.size()))
				summary = summaries_[static_cast<std::size_t>(feature)];
		}
	}
	for (QModelRoleData& d : roleDataSpan) {
		switch (d.role()) {
			case Qt::DisplayRole: d.setData(display); break;
			case IsHeaderRole: d.setData(header); break;
			case CountsTextRole: d.setData(counts); break;
			case FeatureIndexRole: d.setData(header ? QVariant() : QVariant(feature)); break;
			case RunStateRole: d.setData(header ? QVariant() : QVariant(static_cast<int>(summary.state))); break;
			case OutcomeRole: d.setData(header ? QVariant() : QVariant(static_cast<int>(summary.outcome))); break;
			case OkRole: d.setData(header ? QVariant() : QVariant(summary.ok)); break;
			case SkippedRole: d.setData(header ? QVariant() : QVariant(summary.skipped)); break;
			case DriverRole: d.setData(header ? QVariant() : QVariant(static_cast<int>(driver))); break;
			default: d.setData(data(index, d.role())); break;
		}
	}
}

void TestLabFeatureModel::updateSummaries(const std::vector<TestLabController::feature_run_summary_t>& summaries) {
	const std::vector<TestLabController::feature_run_summary_t> old = summaries_;
	summaries_ = summaries;
	const auto& features = test_lab::all_features();
	const int feature_count = static_cast<int>(features.size());
	if (filter_ != FilterAll) {
		bool membership_changed = false;
		for (int i = 0; i < feature_count; ++i) {
			const TestLabController::feature_run_summary_t before =
				i < static_cast<int>(old.size()) ? old[static_cast<std::size_t>(i)] : TestLabController::feature_run_summary_t{};
			const TestLabController::feature_run_summary_t after =
				i < static_cast<int>(summaries_.size()) ? summaries_[static_cast<std::size_t>(i)] : TestLabController::feature_run_summary_t{};
			if (summary_matches_filter(filter_, before) != summary_matches_filter(filter_, after)) {
				membership_changed = true;
				break;
			}
		}
		if (membership_changed) {
			rebuild();
			return;
		}
	}
	for (int i = 0; i < feature_count; ++i) {
		const TestLabController::feature_run_summary_t before =
			i < static_cast<int>(old.size()) ? old[static_cast<std::size_t>(i)] : TestLabController::feature_run_summary_t{};
		const TestLabController::feature_run_summary_t after =
			i < static_cast<int>(summaries_.size()) ? summaries_[static_cast<std::size_t>(i)] : TestLabController::feature_run_summary_t{};
		if (before != after) {
			const QModelIndex idx = indexForFeature(i);
			if (idx.isValid()) {
				const QVector<int> roles = { RunStateRole, OutcomeRole, OkRole, SkippedRole, Qt::ToolTipRole };
				Q_EMIT dataChanged(idx, idx, roles);
			}
		}
	}
	for (std::size_t c = 0; c < categories_.size(); ++c) {
		category_t& cat = categories_[c];
		int pass = 0, fail = 0, skip = 0, running = 0;
		for (int fi : cat.features) {
			if (fi < 0 || fi >= static_cast<int>(summaries_.size())) continue;
			const auto& s = summaries_[static_cast<std::size_t>(fi)];
			if (s.state == test_lab::run_state_e::running) {
				++running;
			} else if (s.state == test_lab::run_state_e::complete) {
				if (s.skipped) ++skip;
				else if (s.ok) ++pass;
				else ++fail;
			}
		}
		if (pass != cat.pass || fail != cat.fail || skip != cat.skip || running != cat.running) {
			cat.pass = pass; cat.fail = fail; cat.skip = skip; cat.running = running;
			const QModelIndex idx = createIndex(static_cast<int>(c), 0, encode_header_id(static_cast<int>(c)));
			const QVector<int> roles = { CountsTextRole };
			Q_EMIT dataChanged(idx, idx, roles);
		}
	}
}

TestLabRowDelegate::TestLabRowDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void TestLabRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
	const auto& t = aida::qt::theme::tokens();
	painter->save();
	painter->setRenderHint(QPainter::Antialiasing, true);
	const QRect r = option.rect;
	const qreal row_radius = qreal(t.radius.sm);
	const qreal badge_radius = qreal(t.radius.xs);
	const qreal badge_h = qreal(t.control.checkbox);
	const qreal pad_x = qreal(t.table.cell_pad_x);
	const qreal gap = qreal(t.spacing.xs);

	if (index.data(TestLabFeatureModel::IsHeaderRole).toBool()) {
		QFont f = option.font;
		f.setBold(true);
		const QFontMetricsF bold_fm(f);
		const qreal baseline = widgets::text_baseline_centered(QRectF(r), bold_fm);
		painter->setFont(f);
		painter->setPen(t.text_secondary);
		const QString name = index.data(Qt::DisplayRole).toString();
		const QString counts = index.data(TestLabFeatureModel::CountsTextRole).toString();
		qreal name_w = qreal(r.width()) - pad_x;
		if (!counts.isEmpty()) {
			const QFontMetricsF plain_fm(option.font);
			name_w -= plain_fm.horizontalAdvance(counts) + gap + pad_x;
		}
		const QString shown_name = bold_fm.elidedText(name, Qt::ElideRight, qMax(0.0, name_w));
		painter->drawText(QPointF(r.left() + pad_x, baseline), shown_name);
		if (!counts.isEmpty()) {
			painter->setFont(option.font);
			painter->setPen(t.text_dim);
			const QFontMetricsF plain_fm(option.font);
			const qreal counts_w = plain_fm.horizontalAdvance(counts);
			painter->drawText(QPointF(r.right() - pad_x - counts_w, baseline), counts);
		}
		painter->restore();
		return;
	}

	const bool selected = (option.state & QStyle::State_Selected) != 0;
	const bool hover = (option.state & QStyle::State_MouseOver) != 0;
	const bool has_focus = (option.state & QStyle::State_HasFocus) != 0;
	const auto rs = static_cast<test_lab::run_state_e>(index.data(TestLabFeatureModel::RunStateRole).toInt());
	const auto outcome = static_cast<test_lab::outcome_e>(index.data(TestLabFeatureModel::OutcomeRole).toInt());
	const auto driver = static_cast<test_lab::driver_e>(index.data(TestLabFeatureModel::DriverRole).toInt());

	if (selected) {
		painter->setPen(Qt::NoPen);
		painter->setBrush(widgets::with_alpha(t.selection_strong, 0.85));
		painter->drawRoundedRect(QRectF(r), row_radius, row_radius);
	} else if (hover) {
		painter->setPen(Qt::NoPen);
		painter->setBrush(widgets::with_alpha(t.hover_wash, 0.55));
		painter->drawRoundedRect(QRectF(r), row_radius, row_radius);
	}

	const qreal mid_y = r.center().y();
	qreal cursor_x = r.left() + pad_x;

	const qreal dot_r = qreal(t.spacing.xs);
	const QColor dot = status_dot_color(rs, outcome);
	painter->setPen(Qt::NoPen);
	painter->setBrush(dot);
	painter->drawEllipse(QPointF(cursor_x + dot_r, mid_y), dot_r, dot_r);
	cursor_x += dot_r * 2.0 + gap;

	const QFont badge_font = aida::qt::theme::fonts::caption();
	const QFontMetricsF badge_fm(badge_font);
	const QString drv_text = QString::fromUtf8(driver_label(driver));
	const qreal badge_w = badge_fm.horizontalAdvance(drv_text) + 2.0 * qreal(t.spacing.sm);
	const QRectF badge_rect(cursor_x, mid_y - badge_h * 0.5, badge_w, badge_h);
	const QColor drv = driver_badge_color(driver);
	painter->setBrush(widgets::with_alpha(drv, 0.35));
	painter->setPen(QPen(widgets::with_alpha(drv, 0.85), 1.0));
	painter->drawRoundedRect(badge_rect, badge_radius, badge_radius);
	painter->setFont(badge_font);
	painter->setPen(t.text_primary);
	painter->drawText(QPointF(badge_rect.left() + qreal(t.spacing.sm),
		widgets::text_baseline_centered(badge_rect, badge_fm)), drv_text);
	cursor_x += badge_w + gap;

	const QString state_text = QString::fromUtf8(result_state_badge(rs, outcome));
	const qreal state_w = badge_fm.horizontalAdvance(state_text) + 2.0 * qreal(t.spacing.sm);
	const QColor state_col = status_dot_color(rs, outcome);
	const QRectF state_rect(qMax(cursor_x, r.right() - pad_x - state_w), mid_y - badge_h * 0.5, state_w, badge_h);
	painter->setBrush(widgets::with_alpha(state_col, 0.16));
	painter->setPen(QPen(widgets::with_alpha(state_col, 0.58), 1.0));
	painter->drawRoundedRect(state_rect, badge_radius, badge_radius);
	painter->setPen(state_col);
	painter->drawText(QPointF(state_rect.left() + qreal(t.spacing.sm),
		widgets::text_baseline_centered(state_rect, badge_fm)), state_text);

	painter->setFont(option.font);
	painter->setPen(selected ? t.text_primary : t.text_secondary);
	const qreal name_w = state_rect.left() - gap - cursor_x;
	const QFontMetricsF fm(option.font);
	const QString name = fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
		qMax(0.0, name_w));
	painter->drawText(QPointF(cursor_x, widgets::text_baseline_centered(QRectF(r), fm)), name);

	if (has_focus) {
		const QRectF face = QRectF(r).adjusted(3.0, 3.0, -3.0, -3.0);
		widgets::paint_focus_ring(*painter, face, row_radius, 0.9);
	}
	painter->restore();
}

QSize TestLabRowDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
	(void)option;
	(void)index;
	const auto& t = aida::qt::theme::tokens();
	return QSize(t.row.property_label_w, t.row.standard);
}

TestLabParsedModel::TestLabParsedModel(QObject* parent) : QAbstractTableModel(parent) {}

void TestLabParsedModel::setFields(const std::vector<test_lab::parsed_field_t>& fields) {
	beginResetModel();
	fields_ = fields;
	endResetModel();
}

int TestLabParsedModel::rowCount(const QModelIndex& parent) const {
	return parent.isValid() ? 0 : static_cast<int>(fields_.size());
}

int TestLabParsedModel::columnCount(const QModelIndex& parent) const {
	return parent.isValid() ? 0 : 2;
}

QVariant TestLabParsedModel::data(const QModelIndex& index, int role) const {
	if (!index.isValid()) return QVariant();
	const int row = index.row();
	if (row < 0 || row >= static_cast<int>(fields_.size())) return QVariant();
	const auto& f = fields_[static_cast<std::size_t>(row)];
	switch (role) {
		case Qt::DisplayRole:
			return QString::fromStdString(index.column() == 0 ? f.label : f.value);
		case Qt::ToolTipRole:
			return QString::fromStdString(index.column() == 0 ? f.label : f.value);
		default:
			return QVariant();
	}
}

QVariant TestLabParsedModel::headerData(int section, Qt::Orientation orientation, int role) const {
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return QVariant();
	return QString::fromUtf8(section == 0 ? "Field" : "Value");
}

void TestLabParsedModel::multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const {
	if (!index.isValid()) {
		for (QModelRoleData& d : roleDataSpan) d.setData(QVariant());
		return;
	}
	const int row = index.row();
	QString text;
	if (row >= 0 && row < static_cast<int>(fields_.size())) {
		const auto& f = fields_[static_cast<std::size_t>(row)];
		text = QString::fromStdString(index.column() == 0 ? f.label : f.value);
	}
	for (QModelRoleData& d : roleDataSpan) {
		switch (d.role()) {
			case Qt::DisplayRole: d.setData(text); break;
			case Qt::ToolTipRole: d.setData(text); break;
			default: d.setData(data(index, d.role())); break;
		}
	}
}

TestLabWidget::TestLabWidget(TestLabController* controller, QWidget* parent)
	: QWidget(parent), controller_(controller)
{
	setObjectName(QStringLiteral("aida.view.test_lab"));
	const auto& t = aida::qt::theme::tokens();

	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding, t.panel.padding);
	root->setSpacing(t.spacing.sm);

	auto* top_bar = new QWidget(this);
	top_bar->setObjectName(QStringLiteral("aida.view.test_lab.topbar"));
	auto* top_layout = new QVBoxLayout(top_bar);
	top_layout->setContentsMargins(0, 0, 0, 0);
	top_layout->setSpacing(t.spacing.xs);
	auto* top_row = new QHBoxLayout();
	top_row->setContentsMargins(0, 0, 0, 0);
	top_row->setSpacing(t.spacing.sm);
	run_all_button_ = new QPushButton(QStringLiteral("Run All Safe Tests"), top_bar);
	run_all_button_->setObjectName(QStringLiteral("aida.view.test_lab.run_all"));
	run_all_button_->setProperty("aidaVariant", "primary");
	connect(run_all_button_, &QPushButton::clicked, this, [this]() { controller_->startRunAllSafe(); });
	run_all_status_ = new QLabel(top_bar);
	run_all_status_->setObjectName(QStringLiteral("aida.view.test_lab.run_all_status"));
	run_all_status_->setProperty("aidaVariant", "secondary");
	run_all_status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	run_all_status_->installEventFilter(this);
	top_row->addWidget(run_all_button_);
	top_row->addWidget(run_all_status_, 1);
	top_layout->addLayout(top_row);
	run_all_progress_ = new QProgressBar(top_bar);
	run_all_progress_->setObjectName(QStringLiteral("aida.view.test_lab.progress"));
	run_all_progress_->setRange(0, 100);
	run_all_progress_->setValue(0);
	run_all_progress_->setTextVisible(false);
	top_layout->addWidget(run_all_progress_);
	root->addWidget(top_bar);

	auto* splitter = new QSplitter(Qt::Horizontal, this);
	splitter->setObjectName(QStringLiteral("aida.view.test_lab.splitter"));
	splitter->setChildrenCollapsible(false);

	auto* left_panel = new QWidget(splitter);
	left_panel->setObjectName(QStringLiteral("aida.view.test_lab.left"));
	left_panel->setMinimumWidth(static_cast<int>(t.shell.min_panel_w) * 2);
	auto* left_layout = new QVBoxLayout(left_panel);
	left_layout->setContentsMargins(0, 0, 0, 0);
	left_layout->setSpacing(t.spacing.xs);
	auto* left_header = new QLabel(QStringLiteral("DRIVER TEST LAB"), left_panel);
	left_header->setObjectName(QStringLiteral("aida.view.test_lab.tree_header"));
	left_header->setProperty("aidaVariant", "secondary");
	left_layout->addWidget(left_header);

	filter_combo_ = new QComboBox(left_panel);
	filter_combo_->setObjectName(QStringLiteral("aida.view.test_lab.filter"));
	filter_combo_->setToolTip(QStringLiteral("Filter the feature list by last run status"));
	filter_combo_->addItem(QStringLiteral("All results"), TestLabFeatureModel::FilterAll);
	filter_combo_->addItem(QStringLiteral("Failed"), TestLabFeatureModel::FilterFailed);
	filter_combo_->addItem(QStringLiteral("Passed"), TestLabFeatureModel::FilterPassed);
	filter_combo_->addItem(QStringLiteral("Skipped"), TestLabFeatureModel::FilterSkipped);
	filter_combo_->addItem(QStringLiteral("Running"), TestLabFeatureModel::FilterRunning);
	filter_combo_->addItem(QStringLiteral("Not run"), TestLabFeatureModel::FilterPending);
	left_layout->addWidget(filter_combo_);

	tree_model_ = new TestLabFeatureModel(this);
	tree_delegate_ = new TestLabRowDelegate(this);
	tree_ = new QTreeView(left_panel);
	tree_->setObjectName(QStringLiteral("aida.view.test_lab.tree"));
	tree_->setUniformRowHeights(true);
	tree_->setHeaderHidden(true);
	tree_->setRootIsDecorated(false);
	tree_->setItemsExpandable(false);
	tree_->setIndentation(0);
	tree_->setSelectionMode(QAbstractItemView::SingleSelection);
	tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
	tree_->setModel(tree_model_);
	tree_->setItemDelegate(tree_delegate_);
	tree_->expandAll();
	tree_stack_ = new QStackedWidget(left_panel);
	tree_stack_->setObjectName(QStringLiteral("aida.view.test_lab.tree_stack"));
	tree_stack_->addWidget(tree_);
	tree_empty_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QStringLiteral("No tests registered"),
		QStringLiteral("The Test Lab registry is empty; feature registration runs at startup."), tree_stack_);
	tree_stack_->addWidget(tree_empty_view_);
	left_layout->addWidget(tree_stack_, 1);
	splitter->addWidget(left_panel);

	auto* scroll = new QScrollArea(splitter);
	scroll->setObjectName(QStringLiteral("aida.view.test_lab.scroll"));
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto* right_content = new QWidget(scroll);
	right_content->setObjectName(QStringLiteral("aida.view.test_lab.content"));
	auto* right_layout = new QVBoxLayout(right_content);
	right_layout->setContentsMargins(t.panel.padding, 0, t.panel.padding, 0);
	right_layout->setSpacing(t.spacing.sm);
	scroll->setWidget(right_content);
	splitter->addWidget(scroll);
	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	const int feature_pane_w = t.row.property_label_w * 2 + t.spacing.lg;
	splitter->setSizes({ feature_pane_w, feature_pane_w * 2 });
	root->addWidget(splitter, 1);

	name_label_ = new QLabel(right_content);
	name_label_->setObjectName(QStringLiteral("aida.view.test_lab.name"));
	name_label_->setFont(aida::qt::theme::fonts::strong());
	name_label_->setWordWrap(true);
	right_layout->addWidget(name_label_);
	summary_label_ = new QLabel(right_content);
	summary_label_->setObjectName(QStringLiteral("aida.view.test_lab.summary"));
	summary_label_->setWordWrap(true);
	summary_label_->setProperty("aidaVariant", "secondary");
	right_layout->addWidget(summary_label_);

	inputs_group_ = new QGroupBox(QStringLiteral("INPUTS"), right_content);
	inputs_group_->setObjectName(QStringLiteral("aida.view.test_lab.inputs"));
	auto* inputs_vbox = new QVBoxLayout(inputs_group_);
	inputs_body_ = new QWidget(inputs_group_);
	inputs_body_->setObjectName(QStringLiteral("aida.view.test_lab.inputs.body"));
	inputs_form_ = new QFormLayout(inputs_body_);
	inputs_form_->setContentsMargins(0, 0, 0, 0);
	inputs_form_->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	inputs_vbox->addWidget(inputs_body_);
	right_layout->addWidget(inputs_group_);

	action_row_ = new QWidget(right_content);
	action_row_->setObjectName(QStringLiteral("aida.view.test_lab.actions"));
	auto* action_hbox = new QHBoxLayout(action_row_);
	action_hbox->setContentsMargins(0, 0, 0, 0);
	action_hbox->setSpacing(t.spacing.sm);
	run_button_ = new QPushButton(QStringLiteral("Run"), action_row_);
	run_button_->setObjectName(QStringLiteral("aida.view.test_lab.run"));
	run_button_->setProperty("aidaVariant", "primary");
	connect(run_button_, &QPushButton::clicked, this, [this]() { controller_->runSelectedFeature(); });
	clear_button_ = new QPushButton(QStringLiteral("Clear"), action_row_);
	clear_button_->setObjectName(QStringLiteral("aida.view.test_lab.clear"));
	clear_button_->setEnabled(false);
	connect(clear_button_, &QPushButton::clicked, this, [this]() { controller_->clearResult(); });
	action_hbox->addWidget(run_button_);
	action_hbox->addWidget(clear_button_);
	action_hbox->addStretch(1);
	right_layout->addWidget(action_row_);

	result_group_ = new QGroupBox(QStringLiteral("RESULT"), right_content);
	result_group_->setObjectName(QStringLiteral("aida.view.test_lab.result"));
	auto* result_vbox = new QVBoxLayout(result_group_);
	result_vbox->setSpacing(t.spacing.sm);

	chip_row_ = new QWidget(result_group_);
	chip_row_->setObjectName(QStringLiteral("aida.view.test_lab.chips"));
	auto* chip_hbox = new QHBoxLayout(chip_row_);
	chip_hbox->setContentsMargins(0, 0, 0, 0);
	chip_hbox->setSpacing(t.spacing.xs);
	chip_status_ = new widgets::AidaBadge(QString(), widgets::AidaSemantic::Neutral, chip_row_);
	chip_ntstatus_ = new widgets::AidaBadge(QString(), widgets::AidaSemantic::Neutral, chip_row_);
	chip_driver_ = new widgets::AidaBadge(QString(), widgets::AidaSemantic::Neutral, chip_row_);
	chip_bytes_ = new widgets::AidaBadge(QString(), widgets::AidaSemantic::Neutral, chip_row_);
	chip_elapsed_ = new widgets::AidaBadge(QString(), widgets::AidaSemantic::Neutral, chip_row_);
	chip_hbox->addWidget(chip_status_);
	chip_hbox->addWidget(chip_ntstatus_);
	chip_hbox->addWidget(chip_driver_);
	chip_hbox->addWidget(chip_bytes_);
	chip_hbox->addWidget(chip_elapsed_);
	chip_hbox->addStretch(1);
	result_vbox->addWidget(chip_row_);

	cached_notice_ = new widgets::AidaNotice(
		QStringLiteral("Cached run summary"),
		QStringLiteral("Showing the cached run summary for this feature. Raw bytes and parsed fields are shown when the selected feature has a direct result snapshot; run-all evidence remains in the log tail below."),
		widgets::AidaSemantic::Info, result_group_);
	cached_notice_->setVisible(false);
	result_vbox->addWidget(cached_notice_);

	busy_notice_ = new widgets::AidaNotice(
		QStringLiteral("Result snapshot busy"),
		QStringLiteral("The worker is updating the selected result. Per-feature run status remains visible in the feature list."),
		widgets::AidaSemantic::Warning, result_group_);
	busy_notice_->setVisible(false);
	result_vbox->addWidget(busy_notice_);

	result_stack_ = new QStackedWidget(result_group_);
	result_stack_->setObjectName(QStringLiteral("aida.view.test_lab.result_stack"));
	result_empty_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QStringLiteral("No execution yet"),
		QStringLiteral("Run this feature or use Run All Safe Tests to collect driver evidence."), result_stack_);
	result_stack_->addWidget(result_empty_view_);
	result_running_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Loading,
		QStringLiteral("Running"),
		QStringLiteral("The worker is executing the selected feature and collecting diagnostics."), result_stack_);
	result_stack_->addWidget(result_running_view_);

	result_complete_ = new QWidget(result_stack_);
	result_complete_->setObjectName(QStringLiteral("aida.view.test_lab.result_complete"));
	auto* complete_vbox = new QVBoxLayout(result_complete_);
	complete_vbox->setContentsMargins(0, 0, 0, 0);
	complete_vbox->setSpacing(t.spacing.sm);

	auto* copy_row = new QWidget(result_complete_);
	copy_row->setObjectName(QStringLiteral("aida.view.test_lab.copy_row"));
	auto* copy_hbox = new QHBoxLayout(copy_row);
	copy_hbox->setContentsMargins(0, 0, 0, 0);
	copy_hbox->setSpacing(t.spacing.xs);
	auto* copy_summary = new QPushButton(QStringLiteral("Copy summary"), copy_row);
	copy_summary->setObjectName(QStringLiteral("aida.view.test_lab.copy_summary"));
	copy_summary->setToolTip(QStringLiteral("Copy the result summary block to the clipboard"));
	auto* copy_parsed = new QPushButton(QStringLiteral("Copy parsed"), copy_row);
	copy_parsed->setObjectName(QStringLiteral("aida.view.test_lab.copy_parsed"));
	copy_parsed->setToolTip(QStringLiteral("Copy the parsed field list to the clipboard"));
	auto* copy_raw = new QPushButton(QStringLiteral("Copy raw"), copy_row);
	copy_raw->setObjectName(QStringLiteral("aida.view.test_lab.copy_raw"));
	copy_raw->setToolTip(QStringLiteral("Copy the raw result bytes as hex to the clipboard"));
	connect(copy_summary, &QPushButton::clicked, this, [this]() {
		const test_lab::feature_t* f = controller_->featureAt(controller_->selectedFeature());
		if (f == nullptr) return;
		aida::qt::clipboard::set_text(QString::fromStdString(format_result_summary(*f, controller_->cachedResult())));
	});
	connect(copy_parsed, &QPushButton::clicked, this, [this]() {
		aida::qt::clipboard::set_text(QString::fromStdString(format_parsed_fields(controller_->cachedResult().parsed)));
	});
	connect(copy_raw, &QPushButton::clicked, this, [this]() {
		aida::qt::clipboard::set_text(QString::fromStdString(format_raw_hex(controller_->cachedResult().raw)));
	});
	copy_hbox->addWidget(copy_summary);
	copy_hbox->addWidget(copy_parsed);
	copy_hbox->addWidget(copy_raw);
	copy_hbox->addStretch(1);
	complete_vbox->addWidget(copy_row);

	error_label_ = new QLabel(result_complete_);
	error_label_->setObjectName(QStringLiteral("aida.view.test_lab.error"));
	error_label_->setWordWrap(true);
	error_label_->setProperty("aidaVariant", "error");
	error_label_->setVisible(false);
	complete_vbox->addWidget(error_label_);

	raw_group_ = new QGroupBox(QStringLiteral("Raw bytes"), result_complete_);
	raw_group_->setObjectName(QStringLiteral("aida.view.test_lab.raw_group"));
	raw_group_->setCheckable(true);
	raw_group_->setChecked(true);
	auto* raw_vbox = new QVBoxLayout(raw_group_);
	raw_stack_ = new QStackedWidget(raw_group_);
	raw_edit_ = new QPlainTextEdit(raw_stack_);
	raw_edit_->setObjectName(QStringLiteral("aida.view.test_lab.raw"));
	raw_edit_->setReadOnly(true);
	raw_edit_->setFont(aida::qt::theme::fonts::codeRegular());
	raw_edit_->setMinimumHeight(code_view_height_for_lines(3));
	raw_edit_->setMaximumHeight(code_view_height_for_lines(7));
	raw_stack_->addWidget(raw_edit_);
	raw_empty_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QStringLiteral("No raw bytes"),
		QStringLiteral("This feature completed without a raw byte buffer."), raw_stack_);
	raw_stack_->addWidget(raw_empty_view_);
	raw_vbox->addWidget(raw_stack_);
	connect(raw_group_, &QGroupBox::toggled, raw_stack_, &QWidget::setVisible);
	complete_vbox->addWidget(raw_group_);

	parsed_group_ = new QGroupBox(QStringLiteral("Parsed fields"), result_complete_);
	parsed_group_->setObjectName(QStringLiteral("aida.view.test_lab.parsed_group"));
	parsed_group_->setCheckable(true);
	parsed_group_->setChecked(true);
	auto* parsed_vbox = new QVBoxLayout(parsed_group_);
	parsed_stack_ = new QStackedWidget(parsed_group_);
	parsed_model_ = new TestLabParsedModel(parsed_stack_);
	parsed_table_ = new QTableView(parsed_stack_);
	parsed_table_->setModel(parsed_model_);
	parsed_table_->setObjectName(QStringLiteral("aida.view.test_lab.parsed"));
	parsed_table_->verticalHeader()->setVisible(false);
	parsed_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
	parsed_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
	parsed_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	parsed_table_->setColumnWidth(0, t.row.property_label_w * 2);
	parsed_table_->setAlternatingRowColors(true);
	parsed_table_->setShowGrid(false);
	parsed_table_->setSelectionMode(QAbstractItemView::NoSelection);
	parsed_table_->setMinimumHeight(t.table.header_h + 3 * t.table.row_h + 2 * t.panel.border);
	parsed_table_->setMaximumHeight(t.table.header_h + 7 * t.table.row_h + 2 * t.panel.border);
	parsed_stack_->addWidget(parsed_table_);
	parsed_empty_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QStringLiteral("No parsed fields"),
		QStringLiteral("The feature did not return structured parsed fields for this run."), parsed_stack_);
	parsed_stack_->addWidget(parsed_empty_view_);
	parsed_vbox->addWidget(parsed_stack_);
	connect(parsed_group_, &QGroupBox::toggled, parsed_stack_, &QWidget::setVisible);
	complete_vbox->addWidget(parsed_group_);

	result_stack_->addWidget(result_complete_);
	result_vbox->addWidget(result_stack_);
	right_layout->addWidget(result_group_);

	no_selection_view_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
		QStringLiteral("No feature selected"),
		QStringLiteral("Select a Test Lab feature to inspect inputs, run status, and per-feature evidence."), right_content);
	right_layout->addWidget(no_selection_view_);

	auto* evidence_group = new QGroupBox(QStringLiteral("RECENT RUN EVIDENCE"), right_content);
	evidence_group->setObjectName(QStringLiteral("aida.view.test_lab.evidence_group"));
	auto* evidence_vbox = new QVBoxLayout(evidence_group);
	evidence_vbox->setSpacing(t.spacing.xs);
	log_path_label_ = new QLabel(evidence_group);
	log_path_label_->setObjectName(QStringLiteral("aida.view.test_lab.log_path"));
	log_path_label_->setWordWrap(true);
	log_path_label_->setProperty("aidaVariant", "secondary");
	evidence_vbox->addWidget(log_path_label_);
	auto* log_btn_row = new QWidget(evidence_group);
	log_btn_row->setObjectName(QStringLiteral("aida.view.test_lab.log_actions"));
	auto* log_btn_hbox = new QHBoxLayout(log_btn_row);
	log_btn_hbox->setContentsMargins(0, 0, 0, 0);
	log_btn_hbox->setSpacing(t.spacing.xs);
	auto* copy_log_path = new QPushButton(QStringLiteral("Copy log path"), log_btn_row);
	copy_log_path->setObjectName(QStringLiteral("aida.view.test_lab.copy_log_path"));
	copy_log_path->setToolTip(QStringLiteral("Copy the full path of the run-all evidence log"));
	connect(copy_log_path, &QPushButton::clicked, this, [this]() {
		aida::qt::clipboard::set_text(QString::fromStdString(controller_->runAllLogPath()));
	});
	auto* open_log_folder = new QPushButton(QStringLiteral("Open log folder"), log_btn_row);
	open_log_folder->setObjectName(QStringLiteral("aida.view.test_lab.open_log_folder"));
	open_log_folder->setToolTip(QStringLiteral("Reveal the evidence log folder in Explorer"));
	connect(open_log_folder, &QPushButton::clicked, this, [this]() { openLogFolder(); });
	log_btn_hbox->addWidget(copy_log_path);
	log_btn_hbox->addWidget(open_log_folder);
	log_btn_hbox->addStretch(1);
	evidence_vbox->addWidget(log_btn_row);
	tail_busy_notice_ = new widgets::AidaNotice(
		QStringLiteral("Log tail snapshot busy"),
		QStringLiteral("Showing the last stable frame."),
		widgets::AidaSemantic::Warning, evidence_group);
	tail_busy_notice_->setVisible(false);
	evidence_vbox->addWidget(tail_busy_notice_);
	tail_edit_ = new QPlainTextEdit(evidence_group);
	tail_edit_->setObjectName(QStringLiteral("aida.view.test_lab.evidence"));
	tail_edit_->setReadOnly(true);
	tail_edit_->setFont(aida::qt::theme::fonts::codeRegular());
	tail_edit_->setMaximumBlockCount(k_evidence_tail_blocks);
	tail_edit_->setMinimumHeight(code_view_height_for_lines(4));
	tail_edit_->setPlaceholderText(QStringLiteral(
		"No run-all evidence yet. Run All Safe Tests writes target launch, driver attach, diagnostics, and result evidence here."));
	tail_edit_->installEventFilter(this);
	evidence_vbox->addWidget(tail_edit_);
	right_layout->addWidget(evidence_group);

	right_layout->addStretch(1);

	run_action_ = new QAction(QStringLiteral("Run selected feature"), this);
	run_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
	run_action_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(run_action_, &QAction::triggered, this, [this]() {
		if (run_button_->isEnabled()) controller_->runSelectedFeature();
	});
	addAction(run_action_);
	run_all_action_ = new QAction(QStringLiteral("Run all safe tests"), this);
	run_all_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return));
	run_all_action_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(run_all_action_, &QAction::triggered, this, [this]() {
		if (run_all_button_->isEnabled()) controller_->startRunAllSafe();
	});
	addAction(run_all_action_);
	clear_action_ = new QAction(QStringLiteral("Clear result"), this);
	clear_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
	clear_action_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(clear_action_, &QAction::triggered, this, [this]() {
		if (clear_button_->isEnabled()) controller_->clearResult();
	});
	addAction(clear_action_);

	const auto shortcut_hint = [](const QAction* action) {
		const QString seq = action->shortcut().toString(QKeySequence::NativeText);
		return seq.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(seq);
	};
	run_all_button_->setToolTip(QStringLiteral("Run every non-destructive feature against the driver%1")
		.arg(shortcut_hint(run_all_action_)));
	run_button_->setToolTip(QStringLiteral("Run the selected feature%1").arg(shortcut_hint(run_action_)));
	clear_button_->setToolTip(QStringLiteral("Clear the current result%1").arg(shortcut_hint(clear_action_)));

	connect(controller_, &TestLabController::featuresChanged, this, [this]() {
		tree_model_->updateSummaries(controller_->cachedSummaries());
	});
	connect(controller_, &TestLabController::resultChanged, this, [this]() { refreshResult(); });
	connect(controller_, &TestLabController::logTailChanged, this, [this]() { refreshEvidence(); });
	connect(controller_, &TestLabController::runAllChanged, this, [this]() { refreshTopBar(); });
	connect(controller_, &TestLabController::selectionChanged, this, [this](int) { applySelectionFromController(); });
	connect(tree_model_, &QAbstractItemModel::modelReset, this, [this]() {
		tree_->expandAll();
		refreshTreeEmptyState();
		syncTreeSelection();
	});
	connect(filter_combo_, &QComboBox::activated, this, [this](int index) {
		tree_model_->setStatusFilter(static_cast<TestLabFeatureModel::FilterMode>(filter_combo_->itemData(index).toInt()));
	});
	connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
		[this](const QModelIndex& current, const QModelIndex&) {
			const int feature = tree_model_->featureIndexFor(current);
			if (feature >= 0)
				controller_->selectFeature(feature);
		});

	tree_model_->updateSummaries(controller_->cachedSummaries());
	applySelectionFromController();
	refreshTreeEmptyState();
	refreshTopBar();
	refreshEvidence();
}

void TestLabWidget::rebuildInputs() {
	rebuild_pending_ = false;
	while (inputs_form_->rowCount() > 0)
		inputs_form_->removeRow(0);
	const test_lab::feature_t* f = controller_->featureAt(controller_->selectedFeature());
	if (f != nullptr && f->render_inputs != nullptr) {
		qt_input_form_t form(inputs_form_, &controller_->inputState(), inputs_body_, [this]() { scheduleRebuildInputs(); });
		f->render_inputs(controller_->inputState(), form);
	} else {
		auto* none_label = new QLabel(QStringLiteral("This feature does not require inputs."), inputs_body_);
		none_label->setObjectName(QStringLiteral("aida.view.test_lab.inputs.none"));
		none_label->setWordWrap(true);
		none_label->setProperty("aidaVariant", "secondary");
		inputs_form_->addRow(none_label);
	}
}

void TestLabWidget::scheduleRebuildInputs() {
	if (rebuild_pending_) return;
	rebuild_pending_ = true;
	QTimer::singleShot(0, this, [this]() { rebuildInputs(); });
}

void TestLabWidget::refreshTopBar() {
	const auto& rs = controller_->cachedRunAll();
	run_all_button_->setEnabled(!rs.active);
	run_all_action_->setEnabled(!rs.active);
	QString status;
	if (rs.active) {
		status = QString::fromUtf8("Running %1 / %2  (%3)   pass=%4 fail=%5 skip=%6")
			.arg(rs.current)
			.arg(rs.total)
			.arg(QString::fromStdString(rs.current_name))
			.arg(rs.ok)
			.arg(rs.fail)
			.arg(rs.skipped);
	} else if (!rs.status_line.empty()) {
		status = QString::fromStdString(rs.status_line);
	} else {
		status = QString::fromStdString("No run yet. Evidence log: " + controller_->runAllLogPath());
	}
	run_all_status_full_ = status;
	updateRunAllStatusText();
	run_all_status_->setToolTip(status);

	const bool has_progress = rs.total > 0;
	double progress = has_progress ? static_cast<double>(rs.current) / static_cast<double>(rs.total) : 0.0;
	if (!rs.active && !rs.status_line.empty() && has_progress)
		progress = 1.0;
	run_all_progress_->setValue(static_cast<int>(progress * 100.0));
	const char* variant = rs.active ? "accent" : (rs.fail > 0 ? "error" : "success");
	if (run_all_progress_->property("aidaVariant") != QVariant(QString::fromUtf8(variant))) {
		run_all_progress_->setProperty("aidaVariant", QString::fromUtf8(variant));
		aida::qt::theme::stylesheet::repolish(run_all_progress_);
	}
}

void TestLabWidget::updateRunAllStatusText() {
	const QString full = run_all_status_full_;
	const int w = run_all_status_->width();
	if (w <= 0) {
		run_all_status_->setText(full);
		return;
	}
	const QFontMetricsF fm(run_all_status_->font());
	run_all_status_->setText(fm.elidedText(full, Qt::ElideMiddle, w));
}

void TestLabWidget::refreshResult() {
	const int sel = controller_->selectedFeature();
	const test_lab::feature_t* f = controller_->featureAt(sel);
	if (f == nullptr) return;

	const bool busy = controller_->resultBusy();
	busy_notice_->setVisible(busy);

	test_lab::run_state_e rs = controller_->cachedResult().state.load(std::memory_order_acquire);
	test_lab::outcome_e displayed_outcome = test_lab::effective_outcome(controller_->cachedResult(),
		f->driver == test_lab::driver_e::driverless);
	std::int32_t ntstatus = controller_->cachedResult().ntstatus;
	std::uint32_t bytes_returned = controller_->cachedResult().bytes_returned;
	std::uint64_t elapsed_us = controller_->cachedResult().elapsed_us;
	std::string error = controller_->cachedResult().error;
	bool using_cached_summary = false;

	if (rs == test_lab::run_state_e::idle && sel >= 0) {
		const auto& summaries = controller_->cachedSummaries();
		if (sel < static_cast<int>(summaries.size())) {
			const auto& cached = summaries[static_cast<std::size_t>(sel)];
			if (cached.state != test_lab::run_state_e::idle) {
				rs = cached.state;
				displayed_outcome = cached.outcome;
				ntstatus = cached.ntstatus;
				bytes_returned = cached.bytes_returned;
				elapsed_us = cached.elapsed_us;
				error = cached.error;
				using_cached_summary = true;
			}
		}
	}

	cached_notice_->setVisible(using_cached_summary && !busy);

	const auto status_kind = status_semantic(rs, displayed_outcome);
	chip_status_->setText(QString::fromUtf8(result_state_label(rs, displayed_outcome)));
	chip_status_->setKind(status_kind);
	chip_status_->setBadgeColor(widgets::semantic_color(status_kind));

	char ntbuf[96];
	std::snprintf(ntbuf, sizeof(ntbuf), "%s / 0x%08X",
		test_lab_format::ntstatus_to_string(ntstatus),
		static_cast<unsigned>(static_cast<std::uint32_t>(ntstatus)));
	chip_ntstatus_->setText(QString::fromUtf8("NTSTATUS: %1").arg(QString::fromUtf8(ntbuf)));
	chip_ntstatus_->setBadgeColor(widgets::semantic_color(status_kind));
	chip_driver_->setText(QString::fromUtf8("Driver: %1").arg(QString::fromUtf8(driver_label(f->driver))));
	chip_driver_->setBadgeColor(driver_badge_color(f->driver));
	chip_bytes_->setText(QString::fromUtf8("Bytes: %1").arg(static_cast<unsigned>(bytes_returned)));
	chip_bytes_->setBadgeColor(aida::qt::theme::tokens().text_secondary);
	chip_elapsed_->setText(QString::fromUtf8("Elapsed: %1").arg(QString::fromStdString(format_elapsed(elapsed_us))));
	chip_elapsed_->setBadgeColor(aida::qt::theme::tokens().text_secondary);

	const bool running = rs == test_lab::run_state_e::running;
	run_button_->setEnabled(!running);
	run_action_->setEnabled(!running);
	run_button_->setText(rs == test_lab::run_state_e::complete ? QStringLiteral("Re-run") : QStringLiteral("Run"));
	clear_button_->setEnabled(rs != test_lab::run_state_e::idle && !running);
	clear_action_->setEnabled(clear_button_->isEnabled());

	if (busy) {
		chip_row_->setVisible(false);
		result_stack_->setVisible(false);
		return;
	}
	chip_row_->setVisible(true);
	result_stack_->setVisible(true);

	if (rs == test_lab::run_state_e::idle) {
		result_stack_->setCurrentWidget(result_empty_view_);
		return;
	}
	if (rs == test_lab::run_state_e::running) {
		result_stack_->setCurrentWidget(result_running_view_);
		return;
	}
	result_stack_->setCurrentWidget(result_complete_);

	if (!error.empty()) {
		error_label_->setText(QString::fromUtf8("error: %1").arg(QString::fromStdString(error)));
		error_label_->setVisible(true);
	} else {
		error_label_->setVisible(false);
	}

	const auto& raw = controller_->cachedResult().raw;
	if (raw.empty() || using_cached_summary) {
		raw_stack_->setCurrentWidget(raw_empty_view_);
	} else {
		std::string dump;
		dump.reserve(raw.size() * 4);
		test_lab_format::render_hex_ascii(raw, dump);
		raw_edit_->setPlainText(QString::fromStdString(dump));
		raw_stack_->setCurrentWidget(raw_edit_);
	}

	const auto& parsed = controller_->cachedResult().parsed;
	if (parsed.empty() || using_cached_summary) {
		parsed_stack_->setCurrentWidget(parsed_empty_view_);
	} else {
		parsed_model_->setFields(parsed);
		parsed_stack_->setCurrentWidget(parsed_table_);
	}
}

void TestLabWidget::refreshEvidence() {
	const QString log_path = QString::fromStdString("Log: " + controller_->runAllLogPath());
	log_path_label_->setText(log_path);
	log_path_label_->setToolTip(log_path);
	tail_busy_notice_->setVisible(controller_->logTailBusy());
	const auto& tail = controller_->cachedLogTail();
	if (tail.empty()) {
		if (tail_last_shown_index_ != 0) {
			tail_edit_->clear();
			tail_last_shown_index_ = 0;
		}
		return;
	}
	if (tail_last_shown_index_ == 0) {
		QString all;
		for (const auto& line : tail) {
			all += QString::fromUtf8("[%1] %2\n")
				.arg(static_cast<unsigned long long>(line.index))
				.arg(QString::fromStdString(line.text));
		}
		tail_edit_->setPlainText(all);
		tail_last_shown_index_ = tail.back().index;
		QScrollBar* bar = tail_edit_->verticalScrollBar();
		bar->setValue(bar->maximum());
	} else {
		for (const auto& line : tail) {
			if (line.index <= tail_last_shown_index_) continue;
			tail_edit_->appendPlainText(QString::fromUtf8("[%1] %2")
				.arg(static_cast<unsigned long long>(line.index))
				.arg(QString::fromStdString(line.text)));
			tail_last_shown_index_ = line.index;
		}
	}
}

void TestLabWidget::refreshTreeEmptyState() {
	const bool any = tree_model_->rowCount(QModelIndex()) > 0;
	tree_stack_->setCurrentWidget(any ? static_cast<QWidget*>(tree_) : static_cast<QWidget*>(tree_empty_view_));
	if (!any) {
		const bool registry_empty = test_lab::all_features().empty();
		tree_empty_view_->setTitle(registry_empty
			? QStringLiteral("No tests registered")
			: QStringLiteral("No matching features"));
		tree_empty_view_->setMessage(registry_empty
			? QStringLiteral("The Test Lab registry is empty; feature registration runs at startup.")
			: QStringLiteral("No features match the current status filter. Choose All results to see every registered feature."));
	}
}

bool TestLabWidget::eventFilter(QObject* watched, QEvent* event) {
	if (watched == run_all_status_ && event->type() == QEvent::Resize) {
		updateRunAllStatusText();
	} else if (watched == tail_edit_ && event->type() == QEvent::Show) {
		QScrollBar* bar = tail_edit_->verticalScrollBar();
		bar->setValue(bar->maximum());
	}
	return QWidget::eventFilter(watched, event);
}

void TestLabWidget::applySelectionFromController() {
	const int sel = controller_->selectedFeature();
	const test_lab::feature_t* f = controller_->featureAt(sel);
	const bool has = f != nullptr;
	name_label_->setVisible(has);
	summary_label_->setVisible(has && f->summary != nullptr && f->summary[0] != '\0');
	inputs_group_->setVisible(has);
	action_row_->setVisible(has);
	result_group_->setVisible(has);
	no_selection_view_->setVisible(!has);
	if (has) {
		const QString full_name = QString::fromUtf8(f->name != nullptr ? f->name : "");
		name_label_->setText(full_name);
		name_label_->setToolTip(full_name);
		summary_label_->setText(QString::fromUtf8(f->summary != nullptr ? f->summary : ""));
		rebuildInputs();
		refreshResult();
	}
	syncTreeSelection();
}

void TestLabWidget::syncTreeSelection() {
	const QModelIndex current = tree_model_->indexForFeature(controller_->selectedFeature());
	if (current.isValid() && tree_->selectionModel()->currentIndex() != current) {
		const QSignalBlocker blocker(tree_->selectionModel());
		tree_->selectionModel()->setCurrentIndex(current, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	} else if (!current.isValid() && tree_->selectionModel()->currentIndex().isValid()) {
		const QSignalBlocker blocker(tree_->selectionModel());
		tree_->selectionModel()->clear();
	}
}

void TestLabWidget::openLogFolder() {
	const std::string path = controller_->runAllLogPath();
	const std::size_t cut = path.find_last_of("\\/");
	if (cut == std::string::npos) return;
	const QString folder = QString::fromStdString(path.substr(0, cut));
	if (folder.isEmpty()) return;
	QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

QWidget* createTestLabWidget(QWidget* parent) {
	return new TestLabWidget(TestLabController::instance(), parent);
}

}
