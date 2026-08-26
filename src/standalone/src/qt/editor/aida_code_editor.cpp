#include "qt/editor/aida_code_editor.hpp"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpressionValidator>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyleHints>
#include <QTextLayout>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "core/debugger/source_debug_service.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/documents/context_menu_hook.hpp"
#include "qt/editor/aida_code_document.hpp"
#include "qt/editor/syntax_roles.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_theme_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_line_edit.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::editor {

namespace {

constexpr qreal k_find_bar_width = 420.0;
constexpr qreal k_goto_bar_width = 240.0;
constexpr int k_autocomplete_max_visible = 8;
constexpr qreal k_autocomplete_width = 280.0;
constexpr qreal k_diff_action_width = 88.0;

using widgets::with_alpha;

bool is_open_bracket(char c) { return c == '(' || c == '[' || c == '{'; }
bool is_close_bracket(char c) { return c == ')' || c == ']' || c == '}'; }

char matching_close_bracket(char open)
{
    switch (open) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '"': return '"';
    case '\'': return '\'';
    default:  return 0;
    }
}

bool is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

int digit_count(int value)
{
    int digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

struct diff_vis_row_t {
    int hunk = -1;
    int line_in_hunk = -1;
    bool is_hunk_head = false;
    code_editor_widget::diff_line_kind_t kind = code_editor_widget::diff_line_kind_t::context;
    const std::string* text = nullptr;
    int old_no = -1;
    int new_no = -1;
};

struct diff_paint_hunk_t {
    code_editor_widget::diff_hunk_state_t state = code_editor_widget::diff_hunk_state_t::pending;
    std::uint64_t stable_id = 0;
    int old_start = 0;
    int old_count = 0;
    int new_start = 0;
    int new_count = 0;
};

struct diff_paint_row_t {
    int hunk = -1;
    bool is_hunk_head = false;
    code_editor_widget::diff_line_kind_t kind = code_editor_widget::diff_line_kind_t::context;
    int old_no = -1;
    int new_no = -1;
    bool has_text = false;
    std::string text;
};

struct diff_paint_snapshot_t {
    std::string origin;
    int total_added = 0;
    int total_removed = 0;
    int pending = 0;
    int old_line_count = 0;
    int new_line_count = 0;
    int total_rows = 0;
    int first_row = 0;
    int selected_hunk = -1;
    qreal scroll_y = 0.0;
    std::vector<diff_paint_hunk_t> hunks;
    std::vector<diff_paint_row_t> rows;
};

std::vector<diff_vis_row_t> build_diff_rows(const code_editor_widget::pending_diff_t& diff)
{
    std::vector<diff_vis_row_t> rows;
    rows.reserve(diff.old_lines.size() + static_cast<std::size_t>(diff.total_added) + diff.hunks.size());
    int old_idx = 0;
    const int gap_ctx = 3;
    for (int hi = 0; hi >= 0 && static_cast<std::size_t>(hi) < diff.hunks.size(); ++hi) {
        const code_editor_widget::diff_hunk_t& h = diff.hunks[static_cast<std::size_t>(hi)];
        int hunk_old_begin = h.old_count > 0 ? h.old_start : old_idx;
        if (h.old_count == 0) {
            for (const auto& dl2 : h.lines)
                if (dl2.kind == code_editor_widget::diff_line_kind_t::context && dl2.old_line >= 0) {
                    hunk_old_begin = dl2.old_line; break;
                }
        }
        int ctx_from = std::max(old_idx, hunk_old_begin - gap_ctx);
        if (hi == 0) ctx_from = std::max(0, hunk_old_begin - gap_ctx);
        if (ctx_from > old_idx && old_idx > 0) {
            diff_vis_row_t sep;
            sep.kind = code_editor_widget::diff_line_kind_t::context;
            rows.push_back(sep);
        }
        for (int li = ctx_from; li >= 0 && li < hunk_old_begin &&
                                  static_cast<std::size_t>(li) < diff.old_lines.size(); ++li) {
            diff_vis_row_t r;
            r.kind = code_editor_widget::diff_line_kind_t::context;
            r.text = &diff.old_lines[static_cast<std::size_t>(li)];
            r.old_no = li + 1;
            r.new_no = -1;
            rows.push_back(r);
        }
        diff_vis_row_t head;
        head.hunk = hi;
        head.is_hunk_head = true;
        rows.push_back(head);
        for (int k = 0; k >= 0 && static_cast<std::size_t>(k) < h.lines.size(); ++k) {
            const code_editor_widget::diff_line_t& dl2 = h.lines[static_cast<std::size_t>(k)];
            diff_vis_row_t r;
            r.hunk = hi;
            r.line_in_hunk = k;
            r.kind = dl2.kind;
            r.text = &dl2.text;
            r.old_no = dl2.old_line >= 0 ? dl2.old_line + 1 : -1;
            r.new_no = dl2.new_line >= 0 ? dl2.new_line + 1 : -1;
            rows.push_back(r);
        }
        int consumed = 0;
        for (const auto& dl2 : h.lines)
            if (dl2.old_line >= 0) consumed++;
        old_idx = hunk_old_begin + consumed;
    }
    const std::size_t tail_to = old_idx >= 0
        ? (std::min)(diff.old_lines.size(), static_cast<std::size_t>(old_idx) + static_cast<std::size_t>(gap_ctx))
        : 0;
    if (old_idx >= 0 && static_cast<std::size_t>(old_idx) < diff.old_lines.size()) {
        for (std::size_t li = static_cast<std::size_t>(old_idx); li < tail_to; ++li) {
            diff_vis_row_t r;
            r.kind = code_editor_widget::diff_line_kind_t::context;
            r.text = &diff.old_lines[li];
            r.old_no = static_cast<int>(li) + 1;
            rows.push_back(r);
        }
    }
    return rows;
}

QStringList breadcrumb_segments(const std::string& filename, int caret_line,
                                AidaCodeDocument& document)
{
    QStringList segments;
    const std::string path = filename.empty() ? std::string("Untitled") : filename;
    const std::size_t last_sep = path.find_last_of("/\\");
    const std::string parent = last_sep != std::string::npos ? path.substr(0, last_sep) : std::string{};
    const std::string name = last_sep != std::string::npos ? path.substr(last_sep + 1) : path;
    if (!parent.empty()) {
        const std::size_t prev_sep = parent.find_last_of("/\\");
        const std::string parent_seg = prev_sep != std::string::npos
            ? parent.substr(prev_sep + 1) : parent;
        if (!parent_seg.empty())
            segments << QString::fromStdString(parent_seg);
    }
    segments << QString::fromStdString(name);

    std::string crumb_func;
    std::string crumb_class;
    const int scan_floor = (std::max)(0, caret_line - 512);
    for (int i = std::min(caret_line, document.lineCount() - 1);
         i >= scan_floor && (crumb_func.empty() || crumb_class.empty()); --i) {
        const std::string& ln = document.lineAt(i);
        if (crumb_func.empty()) {
            const std::size_t paren = ln.find('(');
            if (paren != std::string::npos && paren > 0) {
                std::size_t end = paren;
                while (end > 0 && (ln[end - 1] == ' ' || ln[end - 1] == '\t')) end--;
                if (end > 0) {
                    std::size_t start = end;
                    while (start > 0 && (isalnum((unsigned char)ln[start - 1]) || ln[start - 1] == '_' || ln[start - 1] == ':')) start--;
                    if (end > start && (isalpha((unsigned char)ln[start]) || ln[start] == '_')) {
                        std::string token = ln.substr(start, end - start);
                        if (token != "if" && token != "for" && token != "while" && token != "switch"
                            && token != "return" && token != "catch" && token != "sizeof")
                            crumb_func = token;
                    }
                }
            }
        }
        if (crumb_class.empty()) {
            static const char* prefixes[] = { "class ", "struct ", "namespace " };
            for (auto* pref : prefixes) {
                const std::size_t pos = ln.find(pref);
                if (pos != std::string::npos) {
                    std::size_t s = pos + std::strlen(pref);
                    std::size_t e = s;
                    while (e < ln.size() && (isalnum((unsigned char)ln[e]) || ln[e] == '_' || ln[e] == ':')) e++;
                    if (e > s) crumb_class = ln.substr(s, e - s);
                    break;
                }
            }
        }
    }
    if (!crumb_class.empty()) segments << QString::fromStdString(crumb_class);
    if (!crumb_func.empty()) segments << QString::fromStdString(crumb_func);
    return segments;
}

}

class AidaCodeEditorHeader : public QFrame {
    Q_OBJECT
public:
    explicit AidaCodeEditorHeader(AidaCodeEditor* editor)
        : QFrame(editor), editor_(editor)
    {
        setObjectName(QStringLiteral("aida.code_editor.header"));
        setProperty("aidaRole", QStringLiteral("header"));
        const auto& spacing = theme::tokens().spacing;
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(spacing.md, 0, spacing.sm, 0);
        layout->setSpacing(spacing.xs);
        breadcrumbs_ = new QLabel(this);
        breadcrumbs_->setObjectName(QStringLiteral("aida.code_editor.breadcrumbs"));
        layout->addWidget(breadcrumbs_, 1);
        readonly_ = new QLabel(QStringLiteral("read-only"), this);
        readonly_->setObjectName(QStringLiteral("aida.code_editor.readonly"));
        readonly_->setProperty("aidaVariant", QStringLiteral("warning"));
        readonly_->setVisible(false);
        layout->addWidget(readonly_, 0);
        language_ = new QComboBox(this);
        language_->setObjectName(QStringLiteral("aida.code_editor.language"));
        language_->addItems({QStringLiteral("Auto"), QStringLiteral("C/C++"),
            QStringLiteral("x86 Assembly"), QStringLiteral("Python"),
            QStringLiteral("JSON"), QStringLiteral("Lua")});
        language_->setToolTip(QStringLiteral("Language mode"));
        layout->addWidget(language_, 0);
        setFixedHeight(theme::tokens().tab.inner_h);
        connect(language_, &QComboBox::currentTextChanged, this,
                &AidaCodeEditorHeader::onLanguage);
    }

    void refresh()
    {
        AidaCodeDocument* document = editor_ ? editor_->document() : nullptr;
        if (!document)
            return;
        const auto segments = breadcrumb_segments(document->filename(),
            document->selection().caret_line, *document);
        breadcrumb_full_ = segments.join(QStringLiteral("  \u203A  "));
        breadcrumbs_->setToolTip(breadcrumb_full_);
        applyBreadcrumbElide();
        readonly_->setVisible(document->readOnly());
        readonly_->setToolTip(document->readOnly()
            ? QString::fromStdString(document->readOnlyReason())
            : QString());
        const std::string& override_name = document->languageOverride();
        const QString current = override_name.empty() ? QStringLiteral("Auto")
            : QString::fromStdString(override_name);
        QSignalBlocker blocker(language_);
        language_->setCurrentText(current);
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QFrame::resizeEvent(event);
        applyBreadcrumbElide();
    }

private:
    void applyBreadcrumbElide()
    {
        const int available = breadcrumbs_->contentsRect().width();
        breadcrumbs_->setText(available > 0
            ? QFontMetricsF(breadcrumbs_->font()).elidedText(
                breadcrumb_full_, Qt::ElideMiddle, available)
            : breadcrumb_full_);
    }

private Q_SLOTS:
    void onLanguage(const QString& text)
    {
        AidaCodeDocument* document = editor_ ? editor_->document() : nullptr;
        if (!document)
            return;
        const std::string value = text == QLatin1String("Auto") ? std::string{}
            : text.toStdString();
        static_cast<void>(document->setLanguageOverride(value));
    }

private:
    AidaCodeEditor* editor_ = nullptr;
    QLabel* breadcrumbs_ = nullptr;
    QLabel* readonly_ = nullptr;
    QComboBox* language_ = nullptr;
    QString breadcrumb_full_;
};

class AidaCodeFindOverlay : public QFrame {
    Q_OBJECT
public:
    explicit AidaCodeFindOverlay(AidaCodeEditor* editor)
        : QFrame(editor), editor_(editor)
    {
        setObjectName(QStringLiteral("aida.code_editor.find_overlay"));
        setProperty("aidaRole", QStringLiteral("panel"));
        setFrameShape(QFrame::StyledPanel);
        const auto& spacing = theme::tokens().spacing;
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(spacing.sm, spacing.xs, spacing.sm, spacing.xs);
        layout->setSpacing(spacing.xs);
        auto* row = new QHBoxLayout();
        row->setSpacing(spacing.xs);
        chevron_ = new widgets::AidaButton(QStringLiteral(">"), this);
        chevron_->setKind(widgets::AidaButton::Kind::Ghost);
        chevron_->setToolTip(QStringLiteral("Toggle Replace"));
        row->addWidget(chevron_);
        find_edit_ = new widgets::AidaLineEdit(QStringLiteral("Find"), this);
        find_edit_->setMinimumWidth(static_cast<int>(
            find_edit_->fontMetrics().averageCharWidth() * 24.0) +
            2 * theme::tokens().table.cell_pad_x + spacing.lg);
        row->addWidget(find_edit_, 1);
        case_button_ = new widgets::AidaButton(QStringLiteral("Aa"), this);
        case_button_->setToolTip(QStringLiteral("Match Case"));
        word_button_ = new widgets::AidaButton(QStringLiteral("W"), this);
        word_button_->setToolTip(QStringLiteral("Whole Word"));
        regex_button_ = new widgets::AidaButton(QStringLiteral(".*"), this);
        regex_button_->setToolTip(QStringLiteral("Regular Expression"));
        for (auto* button : {case_button_, word_button_, regex_button_}) {
            button->setKind(widgets::AidaButton::Kind::Ghost);
            button->setCheckable(true);
            row->addWidget(button);
        }
        match_label_ = new QLabel(this);
        row->addWidget(match_label_);
        prev_button_ = new widgets::AidaButton(QStringLiteral("Prev"), this);
        next_button_ = new widgets::AidaButton(QStringLiteral("Next"), this);
        close_button_ = new widgets::AidaButton(QStringLiteral("Close"), this);
        close_button_->setKind(widgets::AidaButton::Kind::Ghost);
        row->addWidget(prev_button_);
        row->addWidget(next_button_);
        row->addWidget(close_button_);
        layout->addLayout(row);

        replace_row_ = new QHBoxLayout();
        replace_row_->setSpacing(theme::tokens().spacing.xs);
        replace_edit_ = new widgets::AidaLineEdit(QStringLiteral("Replace"), this);
        replace_edit_->setMinimumWidth(static_cast<int>(
            replace_edit_->fontMetrics().averageCharWidth() * 24.0) +
            2 * theme::tokens().table.cell_pad_x + spacing.lg);
        replace_row_->addWidget(replace_edit_, 1);
        replace_one_ = new widgets::AidaButton(QStringLiteral("Replace"), this);
        replace_all_ = new widgets::AidaButton(QStringLiteral("All"), this);
        replace_row_->addWidget(replace_one_);
        replace_row_->addWidget(replace_all_);
        auto* replace_holder = new QWidget(this);
        replace_holder->setLayout(replace_row_);
        layout->addWidget(replace_holder);
        replace_holder->setVisible(false);
        replace_holder_ = replace_holder;

        connect(chevron_, &QAbstractButton::clicked, this, [this, replace_holder] {
            replace_holder->setVisible(!replace_holder->isVisible());
            adjustSize();
        });
        connect(find_edit_, &QLineEdit::textChanged, this,
                &AidaCodeFindOverlay::onTextChanged);
        connect(find_edit_, &QLineEdit::returnPressed, this,
                &AidaCodeFindOverlay::onReturnPressed);
        connect(case_button_, &QAbstractButton::toggled, this,
                &AidaCodeFindOverlay::onOptionToggled);
        connect(word_button_, &QAbstractButton::toggled, this,
                &AidaCodeFindOverlay::onOptionToggled);
        connect(regex_button_, &QAbstractButton::toggled, this,
                &AidaCodeFindOverlay::onOptionToggled);
        connect(prev_button_, &QAbstractButton::clicked, this, [this] {
            AidaCodeDocument* document = editor_->document();
            if (document) document->findPrev();
            editor_->viewport()->update();
        });
        connect(next_button_, &QAbstractButton::clicked, this, [this] {
            AidaCodeDocument* document = editor_->document();
            if (document) document->findNext();
            editor_->viewport()->update();
        });
        connect(close_button_, &QAbstractButton::clicked, this, [this] { hideOverlay(); });
        connect(replace_one_, &QAbstractButton::clicked, this, [this] {
            AidaCodeDocument* document = editor_->document();
            if (document) document->replaceCurrent();
        });
        connect(replace_all_, &QAbstractButton::clicked, this, [this] {
            AidaCodeDocument* document = editor_->document();
            if (document) document->replaceAll();
        });
        find_edit_->installEventFilter(this);
        replace_edit_->installEventFilter(this);
    }

    void openWith(bool replace_mode)
    {
        AidaCodeDocument* document = editor_->document();
        if (!document)
            return;
        auto& find = document->find();
        find.visible = true;
        find.replace_mode = replace_mode;
        find_edit_->setText(QString::fromUtf8(find.find_buf));
        replace_edit_->setText(QString::fromUtf8(find.replace_buf));
        case_button_->setChecked(find.case_sensitive);
        word_button_->setChecked(find.whole_word);
        regex_button_->setChecked(find.use_regex);
        replace_holder_->setVisible(replace_mode);
        show();
        adjustSize();
        find_edit_->setFocus(Qt::ShortcutFocusReason);
        find_edit_->selectAll();
    }

    void syncFromDocument()
    {
        AidaCodeDocument* document = editor_->document();
        if (!document)
            return;
        const auto& find = document->find();
        if (find.find_buf[0] == '\0') {
            match_label_->clear();
            return;
        }
        if (document->findLoading()) {
            match_label_->setText(QStringLiteral("Searching..."));
        } else if (find.total_matches == 0) {
            match_label_->setText(QStringLiteral("No results"));
        } else {
            match_label_->setText(QStringLiteral("%1 of %2")
                .arg(find.current_match >= 0 ? find.current_match + 1 : 0)
                .arg(find.total_matches));
        }
        if (!document->findError().empty())
            match_label_->setToolTip(QString::fromStdString(document->findError()));
        else
            match_label_->setToolTip(QString());
    }

    void hideOverlay()
    {
        AidaCodeDocument* document = editor_->document();
        if (document)
            document->find().visible = false;
        hide();
        editor_->setFocus(Qt::OtherFocusReason);
    }

    bool replaceMode() const { return replace_holder_->isVisible(); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) {
                hideOverlay();
                return true;
            }
        }
        return QFrame::eventFilter(watched, event);
    }

private Q_SLOTS:
    void onTextChanged(const QString& text)
    {
        AidaCodeDocument* document = editor_->document();
        if (!document)
            return;
        auto& find = document->find();
        const QByteArray utf8 = text.toUtf8();
        std::snprintf(find.find_buf, sizeof(find.find_buf), "%s", utf8.constData());
        if (std::strcmp(find.find_buf, find_last_buf_) != 0) {
            std::snprintf(find_last_buf_, sizeof(find_last_buf_), "%s", find.find_buf);
            document->findAllMatches();
            if (!find.match_positions.empty()) {
                find.current_match = -1;
                document->findNext();
                editor_->ensureCaretVisible();
            }
        }
        syncFromDocument();
        editor_->viewport()->update();
    }

    void onReturnPressed()
    {
        AidaCodeDocument* document = editor_->document();
        if (!document)
            return;
        if (QApplication::keyboardModifiers() & Qt::ShiftModifier)
            document->findPrev();
        else
            document->findNext();
        editor_->ensureCaretVisible();
        editor_->viewport()->update();
    }

    void onOptionToggled(bool)
    {
        AidaCodeDocument* document = editor_->document();
        if (!document)
            return;
        auto& find = document->find();
        find.case_sensitive = case_button_->isChecked();
        find.whole_word = word_button_->isChecked();
        find.use_regex = regex_button_->isChecked();
        document->findAllMatches();
        if (!find.match_positions.empty()) {
            find.current_match = -1;
            document->findNext();
            editor_->ensureCaretVisible();
        }
        syncFromDocument();
        editor_->viewport()->update();
    }

private:
    AidaCodeEditor* editor_ = nullptr;
    widgets::AidaButton* chevron_ = nullptr;
    widgets::AidaLineEdit* find_edit_ = nullptr;
    widgets::AidaButton* case_button_ = nullptr;
    widgets::AidaButton* word_button_ = nullptr;
    widgets::AidaButton* regex_button_ = nullptr;
    QLabel* match_label_ = nullptr;
    widgets::AidaButton* prev_button_ = nullptr;
    widgets::AidaButton* next_button_ = nullptr;
    widgets::AidaButton* close_button_ = nullptr;
    widgets::AidaLineEdit* replace_edit_ = nullptr;
    widgets::AidaButton* replace_one_ = nullptr;
    widgets::AidaButton* replace_all_ = nullptr;
    QHBoxLayout* replace_row_ = nullptr;
    QWidget* replace_holder_ = nullptr;
    char find_last_buf_[256] = {};
};

class AidaCodeGotoOverlay : public QFrame {
    Q_OBJECT
public:
    explicit AidaCodeGotoOverlay(AidaCodeEditor* editor)
        : QFrame(editor), editor_(editor)
    {
        setObjectName(QStringLiteral("aida.code_editor.goto_overlay"));
        setProperty("aidaRole", QStringLiteral("panel"));
        setFrameShape(QFrame::StyledPanel);
        const auto& spacing = theme::tokens().spacing;
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(spacing.sm, spacing.xs, spacing.sm, spacing.xs);
        layout->setSpacing(spacing.xs);
        line_edit_ = new widgets::AidaLineEdit(QStringLiteral("line"), this);
        line_edit_->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9]+")), line_edit_));
        layout->addWidget(line_edit_, 1);
        go_button_ = new widgets::AidaButton(QStringLiteral("Go"), this);
        go_button_->setKind(widgets::AidaButton::Kind::Primary);
        go_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
        layout->addWidget(go_button_);
        connect(line_edit_, &QLineEdit::returnPressed, this, &AidaCodeGotoOverlay::go);
        connect(go_button_, &QAbstractButton::clicked, this, &AidaCodeGotoOverlay::go);
        line_edit_->installEventFilter(this);
    }

    void openWith()
    {
        AidaCodeDocument* document = editor_->document();
        if (!document)
            return;
        document->goTo().visible = true;
        line_edit_->clear();
        show();
        adjustSize();
        line_edit_->setFocus(Qt::ShortcutFocusReason);
    }

    void hideOverlay()
    {
        AidaCodeDocument* document = editor_->document();
        if (document)
            document->goTo().visible = false;
        hide();
        editor_->setFocus(Qt::OtherFocusReason);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) {
                hideOverlay();
                return true;
            }
        }
        return QFrame::eventFilter(watched, event);
    }

private Q_SLOTS:
    void go()
    {
        AidaCodeDocument* document = editor_->document();
        if (!document)
            return;
        const int n = line_edit_->text().toInt();
        if (n >= 1 && n <= document->lineCount()) {
            document->setCaret(n - 1, 0);
            editor_->ensureCaretVisible();
            hideOverlay();
        }
    }

private:
    AidaCodeEditor* editor_ = nullptr;
    widgets::AidaLineEdit* line_edit_ = nullptr;
    widgets::AidaButton* go_button_ = nullptr;
};

class AidaCodeAutocompletePopup : public QFrame {
    Q_OBJECT
public:
    explicit AidaCodeAutocompletePopup(AidaCodeEditor* editor)
        : QFrame(editor, Qt::ToolTip | Qt::FramelessWindowHint), editor_(editor)
    {
        setObjectName(QStringLiteral("aida.code_editor.autocomplete"));
        setAttribute(Qt::WA_ShowWithoutActivating);
    }

    void refreshGeometry()
    {
        AidaCodeDocument* document = editor_->document();
        if (!document)
            return;
        const auto& autocomplete = document->autocomplete();
        const int total = static_cast<int>(autocomplete.matches.size());
        const int visible = std::min(total, k_autocomplete_max_visible);
        if (visible <= 0) {
            hide();
            return;
        }
        const qreal h = visible * theme::tokens().row.compact + theme::tokens().spacing.sm +
            theme::tokens().spacing.xxs;
        qreal w = k_autocomplete_width;
        if (QScreen* screen = editor_->screen())
            w = std::min(w, static_cast<qreal>(screen->availableGeometry().width()) -
                theme::tokens().spacing.lg);
        setFixedSize(qRound(w), qRound(h));
        QPoint base = editor_->autocompleteAnchor();
        if (QScreen* screen = editor_->screen()) {
            const QRect avail = screen->availableGeometry();
            if (base.y() + height() > avail.bottom() + 1)
                base.setY(qRound(base.y() - editor_->rowHeight() -
                    theme::tokens().spacing.xs - h));
            base.setX(std::clamp(base.x(), avail.left() + 2,
                static_cast<int>(avail.right() + 3 - w)));
            base.setY(std::max(base.y(), avail.top() + 2));
        }
        move(base);
        show();
    }

Q_SIGNALS:
    void accepted(int index);

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);
        AidaCodeDocument* document = editor_->document();
        if (!document)
            return;
        const auto& autocomplete = document->autocomplete();
        const auto& t = theme::tokens();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(rect(), t.bg_elevated);
        painter.setPen(with_alpha(t.border_subtle, 1.0));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        const QFont font = theme::fonts::codeRegular();
        painter.setFont(font);
        const QFontMetricsF fm(font);
        const qreal cell = fm.horizontalAdvance(QLatin1Char('M'));
        const int total = static_cast<int>(autocomplete.matches.size());
        const int visible = std::min(total, k_autocomplete_max_visible);
        int selected = autocomplete.selected;
        if (selected < 0) selected = 0;
        if (selected >= total) selected = total - 1;
        int top = 0;
        if (selected >= k_autocomplete_max_visible) top = selected - k_autocomplete_max_visible + 1;
        if (top > total - visible) top = std::max(0, total - visible);
        std::string lower_pat = autocomplete.partial;
        for (auto& c : lower_pat) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        const int max_cells = std::max(0, static_cast<int>((width() - 6 - 22) / cell));
        const qreal row_h = t.row.compact;
        const qreal rows_y0 = theme::tokens().spacing.xs + 1.0;
        for (int row = 0; row < visible; ++row) {
            const int mi = top + row;
            const QRectF row_rect(3, rows_y0 + row * row_h, width() - 6, row_h);
            if (mi == selected) {
                painter.fillRect(row_rect, with_alpha(t.accent_dim, 0.55));
                painter.fillRect(QRectF(row_rect.left(), row_rect.top(), 2, row_rect.height()),
                    t.accent);
            }
            const std::string& match = autocomplete.matches[static_cast<std::size_t>(mi)];
            const qreal text_y = widgets::text_baseline_centered(row_rect, fm);
            qreal tx = row_rect.left() + 11;
            std::size_t pi = 0;
            std::size_t run_start = 0;
            bool run_hit = false;
            bool run_open = false;
            const int drawn = std::min(match.size(), static_cast<std::size_t>(max_cells));
            const auto flush_run = [&](std::size_t end) {
                if (!run_open || end <= run_start)
                    return;
                painter.setPen(run_hit ? t.accent : with_alpha(t.text_primary, 0.90));
                painter.drawText(QPointF(tx, text_y), QString::fromUtf8(
                    match.data() + run_start, static_cast<qsizetype>(end - run_start)));
                tx += static_cast<qreal>(end - run_start) * cell;
                run_open = false;
            };
            for (std::size_t ci = 0; ci < drawn; ++ci) {
                const char lc = static_cast<char>(tolower(static_cast<unsigned char>(match[ci])));
                const bool hit = pi < lower_pat.size() && lc == lower_pat[pi];
                if (hit) pi++;
                if (run_open && hit == run_hit)
                    continue;
                flush_run(ci);
                run_start = ci;
                run_hit = hit;
                run_open = true;
            }
            flush_run(drawn);
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        AidaCodeDocument* document = editor_->document();
        if (!document)
            return;
        const auto& autocomplete = document->autocomplete();
        const int total = static_cast<int>(autocomplete.matches.size());
        const int visible = std::min(total, k_autocomplete_max_visible);
        int selected = autocomplete.selected;
        int top = 0;
        if (selected >= k_autocomplete_max_visible) top = selected - k_autocomplete_max_visible + 1;
        if (top > total - visible) top = std::max(0, total - visible);
        const int row = static_cast<int>((event->position().y() -
            (theme::tokens().spacing.xs + 1.0)) / theme::tokens().row.compact);
        if (row >= 0 && row < visible)
            Q_EMIT accepted(top + row);
    }

private:
    AidaCodeEditor* editor_ = nullptr;
};

AidaCodeEditor::AidaCodeEditor(AidaCodeDocumentRegistry* registry, quint64 document_id,
                               QWidget* parent)
    : QAbstractScrollArea(parent), registry_(registry), document_id_(document_id)
{
    setObjectName(QStringLiteral("aida.code_editor.canvas"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_StaticContents);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAutoFillBackground(false);

    header_ = new AidaCodeEditorHeader(this);
    setViewportMargins(0, header_->height(), 0, 0);
    header_->setGeometry(0, 0, width(), header_->height());
    header_->show();

    find_overlay_ = new AidaCodeFindOverlay(this);
    find_overlay_->hide();
    goto_overlay_ = new AidaCodeGotoOverlay(this);
    goto_overlay_->hide();
    autocomplete_popup_ = new AidaCodeAutocompletePopup(this);
    autocomplete_popup_->hide();
    connect(autocomplete_popup_, &AidaCodeAutocompletePopup::accepted, this,
            &AidaCodeEditor::acceptAutocomplete);

    blink_timer_ = new QTimer(this);
    {
        const int flash_ms = QGuiApplication::styleHints()->cursorFlashTime();
        if (flash_ms > 0)
            blink_timer_->setInterval(std::max(100, flash_ms / 2));
    }
    connect(blink_timer_, &QTimer::timeout, this, &AidaCodeEditor::onBlinkTick);

    ghost_debounce_ = new QTimer(this);
    ghost_debounce_->setSingleShot(true);
    ghost_debounce_->setInterval(500);
    connect(ghost_debounce_, &QTimer::timeout, this, &AidaCodeEditor::onGhostDebounce);

    ghost_in_ = theme::motion::hover(this);
    connect(ghost_in_, &QVariantAnimation::valueChanged, this, [this](const QVariant&) {
        viewport()->update();
    });

    ghost_absorb_ = theme::motion::hover(this);
    connect(ghost_absorb_, &QVariantAnimation::valueChanged, this, [this](const QVariant&) {
        viewport()->update();
    });

    minimap_hover_anim_ = theme::motion::hover(this);
    connect(minimap_hover_anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant&) {
        viewport()->update();
    });

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (AidaCodeDocument* doc = document())
            doc->setScroll(doc->scrollX(), static_cast<float>(value));
        viewport()->update();
    });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (AidaCodeDocument* doc = document())
            doc->setScroll(static_cast<float>(value), doc->scrollY());
        viewport()->update();
    });

    connect(&theme::AidaThemeController::instance(), &theme::AidaThemeController::themeChanged,
        this, [this] {
            refreshMetrics();
            if (AidaCodeDocument* doc = document())
                doc->recomputeMaxLineCells();
            updateScrollbars();
            repositionOverlays();
            viewport()->update();
        });

    refreshMetrics();
    bindSignals();
    updateScrollbars();
    setMouseTracking(true);
}

AidaCodeEditor::~AidaCodeEditor() = default;

AidaCodeDocument* AidaCodeEditor::document() const noexcept
{
    return registry_ ? registry_->find(document_id_) : nullptr;
}

void AidaCodeEditor::setDocumentId(quint64 document_id)
{
    document_id_ = document_id;
    invalidateLayoutCache();
    if (header_)
        header_->refresh();
    updateScrollbars();
    viewport()->update();
}

void AidaCodeEditor::reloadContent()
{
    invalidateLayoutCache();
    if (header_)
        header_->refresh();
    updateScrollbars();
    viewport()->update();
}

void AidaCodeEditor::bindSignals()
{
    if (!registry_ || signals_bound_)
        return;
    signals_bound_ = true;
    connect(registry_, &AidaCodeDocumentRegistry::metadataChanged, this,
        [this](quint64 changed) {
            if (changed != document_id_)
                return;
            AidaCodeDocument* doc = document();
            if (doc && doc->targetScrollY() != doc->scrollY())
                scheduleScrollTo(doc->targetScrollY());
            updateScrollbars();
            if (header_)
                header_->refresh();
            viewport()->update();
        });
    connect(registry_, &AidaCodeDocumentRegistry::contentChanged, this,
        [this](quint64 changed, quint64) {
            if (changed != document_id_)
                return;
            invalidateLayoutCache();
            updateScrollbars();
            viewport()->update();
        });
    connect(registry_, &AidaCodeDocumentRegistry::foldsChanged, this,
        [this](quint64 changed) {
            if (changed != document_id_)
                return;
            updateScrollbars();
            viewport()->update();
        });
    connect(registry_, &AidaCodeDocumentRegistry::findStateChanged, this,
        [this](quint64 changed) {
            if (changed != document_id_)
                return;
            if (find_overlay_ && find_overlay_->isVisible())
                find_overlay_->syncFromDocument();
            viewport()->update();
        });
    connect(registry_, &AidaCodeDocumentRegistry::streamStateChanged, this,
        [this](quint64 changed) {
            if (changed != document_id_)
                return;
            updateScrollbars();
            viewport()->update();
        });
    connect(registry_, &AidaCodeDocumentRegistry::diffChanged, this,
        [this](quint64 changed) {
            if (changed != document_id_)
                return;
            updateScrollbars();
            viewport()->update();
        });
    connect(registry_, &AidaCodeDocumentRegistry::findOverlayRequested, this,
        [this](quint64 changed, bool replace_mode) {
            if (changed != document_id_)
                return;
            showFindOverlay(replace_mode);
        });
    connect(registry_, &AidaCodeDocumentRegistry::gotoOverlayRequested, this,
        [this](quint64 changed) {
            if (changed != document_id_)
                return;
            showGotoOverlay();
        });
}

void AidaCodeEditor::refreshMetrics()
{
    const int pixel_size = std::clamp(static_cast<int>(g_sa_settings.editor_font_size), 8, 48);
    QFont font = theme::fonts::code(400, pixel_size);
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontInfo info(font);
    if (!info.fixedPitch()) {
        diag::log_tagged_fmt("qt_code_editor",
            "code_font_not_fixed family=%s resolved=%s -- forcing mono grid font",
            font.family().toUtf8().constData(), info.family().toUtf8().constData());
        font = theme::fonts::codeRegular();
    }
    const QFontMetricsF fm(font);
    char_w_ = fm.horizontalAdvance(QChar(u'X'));
    row_h_ = fm.lineSpacing();
    ascent_ = fm.ascent();
    setFont(font);
    viewport()->setFont(font);
    invalidateLayoutCache();
}

void AidaCodeEditor::invalidateLayoutCache()
{
    layout_cache_.clear();
}

void AidaCodeEditor::updateScrollbars()
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    if (doc->hasPendingDiff()) {
        std::unique_lock<std::mutex> diff_lock(doc->diffMutex(), std::try_to_lock);
        if (diff_lock.owns_lock())
            diff_content_rows_ = static_cast<int>(build_diff_rows(doc->pendingDiff()).size());
        const int content_h = static_cast<int>(diff_content_rows_ * row_h_);
        const int body_h = std::max(0,
            static_cast<int>(viewport()->height() - diffHeaderHeight()));
        verticalScrollBar()->setRange(0, std::max(0, content_h - body_h));
        verticalScrollBar()->setSingleStep(std::max(1, static_cast<int>(row_h_)));
        verticalScrollBar()->setPageStep(std::max(1, body_h));
        horizontalScrollBar()->setRange(0, 0);
        return;
    }
    if (doc->cacheDirty())
        doc->rebuildLines();
    doc->rebuildFoldProjection();
    const int visual_count = doc->visibleLines().empty()
        ? doc->lineCount() : static_cast<int>(doc->visibleLines().size());
    const int viewport_h = viewport()->height();
    const qreal content_h_f = visual_count * row_h_;
    const int content_h = content_h_f > static_cast<qreal>((std::numeric_limits<int>::max)())
        ? (std::numeric_limits<int>::max)() : static_cast<int>(content_h_f);
    verticalScrollBar()->setRange(0, std::max(0, content_h - viewport_h));
    verticalScrollBar()->setSingleStep(std::max(1, static_cast<int>(row_h_)));
    verticalScrollBar()->setPageStep(std::max(1, viewport_h));

    const qreal content_w = static_cast<qreal>(doc->maxLineCells()) * char_w_ + char_w_ * 2.0;
    const qreal text_w = viewport()->width() - textX0() - minimapWidth() - char_w_;
    horizontalScrollBar()->setRange(0, std::max(0,
        static_cast<int>(std::ceil(content_w - text_w))));
    horizontalScrollBar()->setSingleStep(std::max(1, static_cast<int>(char_w_)));
    horizontalScrollBar()->setPageStep(std::max(1, static_cast<int>(text_w)));
}

void AidaCodeEditor::scheduleScrollTo(float target_y)
{
    verticalScrollBar()->setValue(static_cast<int>(target_y));
}

void AidaCodeEditor::onBlinkTick()
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    doc->blinkOn() = !doc->blinkOn();
    const QRect caret_rect = caretViewportRect();
    viewport()->update(caret_rect.isValid() ? caret_rect : viewport()->rect());
}

void AidaCodeEditor::onGhostDebounce()
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    doc->ghostStartDebounce();
}

void AidaCodeEditor::ensureCaretVisible()
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    const qreal view_text_w = viewport()->width() - textX0() - minimapWidth() - char_w_;
    const qreal max_scroll_x = horizontalScrollBar()->maximum();
    doc->ensureCaretVisiblePixels(viewport()->height(), row_h_, view_text_w, char_w_,
        max_scroll_x);
    if (doc->targetScrollY() != doc->scrollY())
        scheduleScrollTo(doc->targetScrollY());
    horizontalScrollBar()->setValue(static_cast<int>(doc->scrollX()));
    viewport()->update();
}

QPoint AidaCodeEditor::autocompleteAnchor() const
{
    const AidaCodeDocument* doc = document();
    if (!doc)
        return QPoint();
    const auto& autocomplete = doc->autocomplete();
    const qreal x = textX0() + autocomplete.cursor_col * char_w_ -
        horizontalScrollBar()->value();
    const int visual = doc->visualRowFor(autocomplete.cursor_line);
    const qreal y = (visual + 1) * row_h_ - verticalScrollBar()->value() +
        theme::tokens().spacing.xs;
    return viewport()->mapToGlobal(QPoint(qRound(x), qRound(y)));
}

void AidaCodeEditor::screenToLineCol(const QPointF& pos, int& out_line, int& out_col)
{
    AidaCodeDocument* doc = document();
    if (!doc) {
        out_line = 0;
        out_col = 0;
        return;
    }
    const qreal rel_y = pos.y() + verticalScrollBar()->value();
    const qreal rel_x = pos.x() - textX0() + horizontalScrollBar()->value();
    const int visual = std::clamp(static_cast<int>(rel_y / row_h_), 0,
        (std::max)(0, (doc->visibleLines().empty()
            ? doc->lineCount() : static_cast<int>(doc->visibleLines().size())) - 1));
    out_line = doc->visibleLines().empty() ? visual : doc->logicalLineFor(visual);
    out_col = doc->clampCol(out_line, columnForCells(doc->lineAt(out_line), rel_x / char_w_));
}

qreal AidaCodeEditor::cellsForColumn(const std::string& text, int column) const
{
    const int tab = std::max(1, g_sa_settings.editor_tab_size);
    const int limit = std::clamp(column, 0, static_cast<int>(text.size()));
    qreal cells = 0.0;
    for (int i = 0; i < limit; ++i) {
        const unsigned char byte = static_cast<unsigned char>(text[static_cast<std::size_t>(i)]);
        if ((byte & 0xC0) == 0x80)
            continue;
        if (byte == '\t')
            cells += tab - std::fmod(cells, static_cast<qreal>(tab));
        else
            cells += 1.0;
    }
    return cells;
}

int AidaCodeEditor::columnForCells(const std::string& text, qreal cells) const
{
    const int tab = std::max(1, g_sa_settings.editor_tab_size);
    qreal cursor = 0.0;
    const int length = static_cast<int>(text.size());
    for (int i = 0; i < length; ++i) {
        const unsigned char byte = static_cast<unsigned char>(text[static_cast<std::size_t>(i)]);
        if ((byte & 0xC0) == 0x80)
            continue;
        const qreal advance = byte == '\t'
            ? static_cast<qreal>(tab) - std::fmod(cursor, static_cast<qreal>(tab)) : 1.0;
        if (cells < cursor + advance * 0.5)
            return i;
        cursor += advance;
    }
    return length;
}

qreal AidaCodeEditor::lineCellWidth(const std::string& text) const
{
    return cellsForColumn(text, static_cast<int>(text.size()));
}

QRect AidaCodeEditor::caretViewportRect() const
{
    const AidaCodeDocument* doc = document();
    if (!doc)
        return QRect();
    const int row = doc->visualRowFor(doc->selection().caret_line);
    const qreal cy = row * row_h_ - verticalScrollBar()->value();
    if (cy < -row_h_ || cy > viewport()->height())
        return QRect();
    const qreal cx = textX0() +
        cellsForColumn(doc->lineAt(doc->selection().caret_line),
            doc->selection().caret_col) * char_w_ - horizontalScrollBar()->value();
    return QRect(qFloor(cx) - 2, qFloor(cy) - 1, 6, qCeil(row_h_) + 2)
        .intersected(viewport()->rect());
}

qreal AidaCodeEditor::textX0() const
{
    return gutterWidth() + theme::tokens().spacing.xs;
}

qreal AidaCodeEditor::sourceGutterWidth() const
{
    const AidaCodeDocument* doc = document();
    if (!doc || doc->filepath().empty())
        return 0.0;
    return std::ceil(char_w_ * 2.0);
}

qreal AidaCodeEditor::foldGutterWidth() const
{
    return std::ceil(char_w_ * 2.0);
}

qreal AidaCodeEditor::gutterWidth() const
{
    return sourceGutterWidth() + foldGutterWidth() + lineNumberGutterWidth();
}

qreal AidaCodeEditor::lineNumberGutterWidth() const
{
    if (!g_sa_settings.editor_line_numbers)
        return 0.0;
    return char_w_ * gutterDigits() + theme::tokens().spacing.md;
}

qreal AidaCodeEditor::diffHeaderHeight() const
{
    return theme::tokens().panel.header_h;
}

int AidaCodeEditor::gutterDigits() const
{
    const AidaCodeDocument* doc = document();
    const int lines = doc ? std::max(1, doc->lineCount()) : 1;
    return std::max(5, digit_count(lines));
}

bool AidaCodeEditor::minimapVisible() const
{
    const AidaCodeDocument* doc = document();
    return g_sa_settings.editor_minimap && width() > 360 && doc &&
        !doc->largeFileMode();
}

qreal AidaCodeEditor::minimapWidth() const
{
    return minimapVisible() ? 64.0 : 0.0;
}

QTextLayout* AidaCodeEditor::layoutForLine(int line, const std::string& text)
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return nullptr;
    const quint64 hash = [&] {
        quint64 h = 14695981039346656037ULL;
        for (const char c : text) {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ULL;
        }
        return h;
    }();
    auto found = layout_cache_.find(line);
    if (found != layout_cache_.end() && found->hash == hash)
        return found->layout.get();
    if (layout_cache_.size() > 4096)
        layout_cache_.clear();
    auto layout = std::make_shared<QTextLayout>();
    layout->setFont(font());
    layout->setText(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
    layout->setFormats(line_formats(doc->tokensForLine(line), font()));
    QTextOption text_option;
    text_option.setTabStopDistance(char_w_ *
        static_cast<qreal>(std::max(1, g_sa_settings.editor_tab_size)));
    layout->setTextOption(text_option);
    layout->setCacheEnabled(true);
    layout->beginLayout();
    layout->createLine();
    layout->endLayout();
    layout_entry_t entry;
    entry.hash = hash;
    entry.layout = layout;
    layout_cache_.insert(line, entry);
    return layout.get();
}

void AidaCodeEditor::paintEvent(QPaintEvent* event)
{
    AidaCodeDocument* doc = document();
    QPainter painter(viewport());
    const auto& t = theme::tokens();
    painter.fillRect(event->rect(), t.bg_base);
    if (!doc)
        return;
    if (!doc->active()) {
        painter.setPen(t.text_dim);
        const QString title = QStringLiteral("No text document open");
        painter.drawText(QPointF((viewport()->width() - painter.fontMetrics().horizontalAdvance(title)) * 0.5,
            viewport()->height() * 0.5), title);
        return;
    }
    if (doc->streamLoading()) {
        painter.setPen(t.text_dim);
        painter.drawText(QPointF(theme::tokens().spacing.md, theme::tokens().spacing.xxl),
            QStringLiteral("Building a bounded memory-mapped line index for %1...")
                .arg(QString::fromStdString(doc->filename())));
        return;
    }
    if (!doc->streamError().empty()) {
        painter.setPen(t.text_dim);
        painter.drawText(QPointF(theme::tokens().spacing.md, theme::tokens().spacing.xxl),
            QStringLiteral("Large-file view unavailable"));
        painter.drawText(QPointF(theme::tokens().spacing.md, theme::tokens().spacing.xxl + row_h_),
            QString::fromStdString(doc->streamError()));
        return;
    }

    if (doc->hasPendingDiff()) {
        paintDiffReview(painter, event->rect());
        return;
    }

    if (doc->cacheDirty())
        doc->rebuildLines();
    doc->rebuildFoldProjection();
    const int visual_count = doc->visibleLines().empty()
        ? doc->lineCount() : static_cast<int>(doc->visibleLines().size());
    const QRectF exposed = event->rect();
    const qreal scroll_y = verticalScrollBar()->value();
    const int first_row = std::max(0,
        static_cast<int>((scroll_y + exposed.top()) / row_h_) - 1);
    const int last_row = std::min(visual_count - 1,
        static_cast<int>((scroll_y + exposed.bottom()) / row_h_) + 1);
    if (!doc->isMapped() && first_row <= last_row)
        doc->tokenizeRange(doc->logicalLineFor(first_row) - 8, doc->logicalLineFor(last_row) + 8);

    paintCurrentLine(painter, row_h_);
    paintGutter(painter, first_row, last_row, row_h_);
    paintSelection(painter, first_row, last_row, row_h_, char_w_);
    paintFindMatches(painter, first_row, last_row, row_h_, char_w_);
    paintIndentGuides(painter, first_row, last_row, row_h_, char_w_);
    paintTextLines(painter, first_row, last_row, row_h_, char_w_, exposed);
    paintBracketMatch(painter, row_h_, char_w_);
    paintCaret(painter, row_h_, char_w_);
    paintGhostText(painter, row_h_, char_w_);
    paintMinimap(painter, exposed);
}

void AidaCodeEditor::paintCurrentLine(QPainter& painter, qreal row_h)
{
    AidaCodeDocument* doc = document();
    if (!doc || !g_sa_settings.editor_highlight_line || !doc->hasFocus())
        return;
    const auto& t = theme::tokens();
    const int row = doc->visualRowFor(doc->selection().caret_line);
    const qreal y = row * row_h - verticalScrollBar()->value();
    if (y < -row_h || y > viewport()->height())
        return;
    painter.fillRect(QRectF(0, y, viewport()->width() - minimapWidth(), row_h),
        with_alpha(t.hover_wash, 0.9));
    painter.fillRect(QRectF(0, y, 2, row_h), with_alpha(t.accent, 0.55));
}

void AidaCodeEditor::paintGutter(QPainter& painter, int first_row, int last_row, qreal row_h)
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    const auto& t = theme::tokens();
    const qreal gutter_w = gutterWidth();
    painter.setPen(QPen(with_alpha(t.border_subtle, 1.0), 1));
    painter.drawLine(QPointF(gutter_w, 0), QPointF(gutter_w, viewport()->height()));

    const bool show_ln = g_sa_settings.editor_line_numbers;
    const qreal source_gutter_w = sourceGutterWidth();
    const qreal fold_gutter_w = foldGutterWidth();

    source_debug_service::marker_snapshot_t markers;
    if (source_gutter_w > 0.0)
        markers = source_debug_service::markers_for_path(doc->filepath());

    for (int row = first_row; row <= last_row; ++row) {
        const int line = doc->logicalLineFor(row);
        const qreal y = row * row_h - verticalScrollBar()->value();

        if (source_gutter_w > 0.0 && markers.markers) {
            const auto found = std::lower_bound(markers.markers->begin(),
                markers.markers->end(), static_cast<std::uint32_t>(line + 1),
                [](const source_debug_service::line_marker_t& marker, std::uint32_t line_no) {
                    return marker.line < line_no;
                });
            if (found != markers.markers->end() &&
                found->line == static_cast<std::uint32_t>(line + 1)) {
                QColor marker_color = t.text_dim;
                switch (found->state) {
                case source_debug_service::binding_state_t::bound: marker_color = t.error; break;
                case source_debug_service::binding_state_t::pending: marker_color = t.warning; break;
                case source_debug_service::binding_state_t::unbound: marker_color = t.text_secondary; break;
                case source_debug_service::binding_state_t::stale: marker_color = t.warning; break;
                case source_debug_service::binding_state_t::error: marker_color = t.error; break;
                }
                painter.setPen(Qt::NoPen);
                painter.setBrush(marker_color);
                painter.drawEllipse(QPointF(source_gutter_w * 0.5, y + row_h * 0.5), 4.25, 4.25);
            }
        }

        if (show_ln) {
            painter.setPen(line == doc->selection().caret_line
                ? with_alpha(t.accent, 0.85) : t.text_lineno);
            const QString number = QString::number(line + 1).rightJustified(gutterDigits(), u' ');
            painter.drawText(QPointF(source_gutter_w + fold_gutter_w +
                theme::tokens().spacing.xs, y + ascent_), number);
        }

        const int fold_end = doc->cachedFoldEnd(line);
        if (fold_end > line) {
            const qreal cx = source_gutter_w + fold_gutter_w * 0.5;
            const qreal cy = y + row_h * 0.5;
            const bool collapsed = doc->foldHasEnd(line);
            painter.setPen(Qt::NoPen);
            painter.setBrush(t.text_secondary);
            if (collapsed) {
                const QPointF points[3] = {QPointF(cx - 2, cy - 4), QPointF(cx - 2, cy + 4),
                    QPointF(cx + 3, cy)};
                painter.drawPolygon(points, 3);
            } else {
                const QPointF points[3] = {QPointF(cx - 4, cy - 2), QPointF(cx + 4, cy - 2),
                    QPointF(cx, cy + 3)};
                painter.drawPolygon(points, 3);
            }
        }
    }
}

void AidaCodeEditor::paintSelection(QPainter& painter, int first_row, int last_row,
    qreal row_h, qreal char_w)
{
    AidaCodeDocument* doc = document();
    if (!doc || !doc->selection().has_selection())
        return;
    const auto& t = theme::tokens();
    int l0, c0, l1, c1;
    doc->selectionOrdered(l0, c0, l1, c1);
    const qreal x0 = textX0();
    const qreal code_w = viewport()->width() - minimapWidth();
    const qreal scroll_x = horizontalScrollBar()->value();
    for (int row = first_row; row <= last_row; ++row) {
        const int line = doc->logicalLineFor(row);
        if (line < l0 || line > l1)
            continue;
        const std::string& text = doc->lineAt(line);
        const qreal y = row * row_h - verticalScrollBar()->value();
        qreal sx0, sx1;
        if (line == l0 && line == l1) {
            sx0 = x0 + cellsForColumn(text, c0) * char_w - scroll_x;
            sx1 = x0 + cellsForColumn(text, c1) * char_w - scroll_x;
        } else if (line == l0) {
            sx0 = x0 + cellsForColumn(text, c0) * char_w - scroll_x;
            sx1 = x0 + lineCellWidth(text) * char_w - scroll_x + char_w;
        } else if (line == l1) {
            sx0 = x0 - scroll_x;
            sx1 = x0 + cellsForColumn(text, c1) * char_w - scroll_x;
        } else {
            sx0 = x0 - scroll_x;
            sx1 = x0 + lineCellWidth(text) * char_w - scroll_x + char_w;
        }
        sx0 = std::max(sx0, x0);
        sx1 = std::min(sx1, code_w - theme::tokens().spacing.xs);
        if (sx1 > sx0)
            painter.fillRect(QRectF(sx0, y, sx1 - sx0, row_h), t.selection);
    }
}

void AidaCodeEditor::paintFindMatches(QPainter& painter, int first_row, int last_row,
    qreal row_h, qreal char_w)
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    const auto& find = doc->find();
    if (!find.visible || find.match_positions.empty())
        return;
    const auto& t = theme::tokens();
    const qreal x0 = textX0();
    const qreal code_w = viewport()->width() - minimapWidth();
    const qreal scroll_x = horizontalScrollBar()->value();
    const int first_logical = doc->logicalLineFor(first_row);
    const auto begin = find.match_positions.begin();
    const auto end = find.match_positions.end();
    auto it = std::lower_bound(begin, end, first_logical,
        [](const code_editor_widget::find_match_t& match, int line) {
            return match.line < line;
        });
    for (; it != end; ++it) {
        const auto& m = *it;
        const int match_row = doc->visualRowFor(m.line);
        if (match_row > last_row)
            break;
        if (match_row < first_row || doc->logicalLineFor(match_row) != m.line)
            continue;
        const bool is_active = find.current_match >= 0 &&
            &m == &find.match_positions[static_cast<std::size_t>(find.current_match)];
        const std::string& text = doc->lineAt(m.line);
        const qreal y = match_row * row_h - verticalScrollBar()->value();
        const qreal mx0 = x0 + cellsForColumn(text, m.col) * char_w - scroll_x;
        qreal mx1 = x0 + cellsForColumn(text,
            std::min(static_cast<int>(text.size()), m.col + m.length)) * char_w - scroll_x;
        mx1 = std::min(mx1, code_w - theme::tokens().spacing.xs);
        if (mx1 <= mx0)
            continue;
        painter.fillRect(QRectF(mx0, y, mx1 - mx0, row_h),
            is_active ? with_alpha(t.accent, 0.55) : with_alpha(t.accent_dim, 0.32));
        if (is_active) {
            painter.setPen(QPen(t.accent, 1));
            painter.drawRect(QRectF(mx0 - 1, y, mx1 - mx0 + 2, row_h));
        }
    }
}

void AidaCodeEditor::paintIndentGuides(QPainter& painter, int first_row, int last_row,
    qreal row_h, qreal char_w)
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    const auto& t = theme::tokens();
    const int tab = std::max(1, g_sa_settings.editor_tab_size);
    const qreal x0 = textX0();
    const int max_indent_render = 16;
    for (int row = first_row; row <= last_row; ++row) {
        const int line = doc->logicalLineFor(row);
        const std::string& ln = doc->lineAt(line);
        int leading = 0;
        for (const char c : ln) {
            if (c == ' ') leading++;
            else if (c == '\t') leading += tab;
            else break;
        }
        if (leading <= 0)
            continue;
        const int levels = std::min(max_indent_render, leading / tab);
        const qreal y = row * row_h - verticalScrollBar()->value();
        for (int lv = 1; lv <= levels; ++lv) {
            const qreal gx = x0 + static_cast<qreal>(lv * tab) * char_w -
                horizontalScrollBar()->value() - char_w * 0.5;
            if (gx < x0)
                continue;
            const bool active = doc->selection().caret_line == line && lv * tab <= leading;
            painter.setPen(QPen(active ? with_alpha(t.accent_dim, 0.45)
                                       : with_alpha(t.border_subtle, 0.6), 1));
            painter.drawLine(QPointF(gx, y), QPointF(gx, y + row_h));
        }
    }
}

void AidaCodeEditor::paintTextLines(QPainter& painter, int first_row, int last_row,
    qreal row_h, qreal char_w, const QRectF& clip)
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    const qreal x0 = textX0();
    const qreal text_right = viewport()->width() - minimapWidth();
    painter.save();
    painter.setClipRect(QRectF(x0, clip.top(), std::max(0.0, text_right - x0),
        clip.height()), Qt::IntersectClip);
    for (int row = first_row; row <= last_row; ++row) {
        const int line = doc->logicalLineFor(row);
        const qreal y = row * row_h - verticalScrollBar()->value();
        if (y + row_h < clip.top() || y > clip.bottom())
            continue;
        const std::string& text = doc->lineAt(line);
        if (text.empty())
            continue;
        QTextLayout* layout = layoutForLine(line, text);
        if (!layout)
            continue;
        layout->draw(&painter, QPointF(x0 - horizontalScrollBar()->value(), y));
    }
    painter.restore();
}

void AidaCodeEditor::paintBracketMatch(QPainter& painter, qreal row_h, qreal char_w)
{
    AidaCodeDocument* doc = document();
    if (!doc || !g_sa_settings.editor_bracket_match || !doc->hasFocus() ||
        doc->selection().has_selection())
        return;
    const auto& t = theme::tokens();
    const int caret_line = doc->selection().caret_line;
    const int caret_col = doc->selection().caret_col;
    const std::string& cl = doc->lineAt(caret_line);
    const char here = (caret_col >= 0 && caret_col < static_cast<int>(cl.size()))
        ? cl[static_cast<std::size_t>(caret_col)] : 0;
    const char before = (caret_col > 0 && caret_col - 1 < static_cast<int>(cl.size()))
        ? cl[static_cast<std::size_t>(caret_col - 1)] : 0;
    int bracket_line = -1;
    int bracket_col = -1;
    if (is_open_bracket(here) || is_close_bracket(here)) {
        bracket_line = caret_line;
        bracket_col = caret_col;
    } else if (is_open_bracket(before) || is_close_bracket(before)) {
        bracket_line = caret_line;
        bracket_col = caret_col - 1;
    }
    if (bracket_line < 0)
        return;
    int match_line = -1;
    int match_col = -1;
    char match_ch = 0;
    const bool found = doc->findMatchingBracket(bracket_line, bracket_col,
        match_line, match_col, match_ch);
    const QColor box_color = found ? with_alpha(t.accent, 0.55) : with_alpha(t.error, 0.55);
    const QColor fill_color = found ? with_alpha(t.accent_glow, 0.28) : with_alpha(t.error, 0.18);
    const auto draw_box = [&](int bl, int bc) {
        const qreal bx = textX0() +
            cellsForColumn(doc->lineAt(bl), bc) * char_w - horizontalScrollBar()->value();
        if (bx < textX0() - char_w)
            return;
        const int bracket_row = doc->visualRowFor(bl);
        if (doc->logicalLineFor(bracket_row) != bl)
            return;
        const qreal by = bracket_row * row_h - verticalScrollBar()->value();
        if (by < -row_h || by > viewport()->height())
            return;
        painter.fillRect(QRectF(bx, by, char_w, row_h), fill_color);
        painter.setPen(QPen(box_color, 1));
        painter.drawRect(QRectF(bx, by, char_w, row_h));
    };
    draw_box(bracket_line, bracket_col);
    if (found)
        draw_box(match_line, match_col);
}

void AidaCodeEditor::paintCaret(QPainter& painter, qreal row_h, qreal char_w)
{
    AidaCodeDocument* doc = document();
    if (!doc || !doc->hasFocus() || !doc->blinkOn())
        return;
    const auto& t = theme::tokens();
    const qreal x0 = textX0();
    const qreal cx = x0 + cellsForColumn(doc->lineAt(doc->selection().caret_line),
        doc->selection().caret_col) * char_w - horizontalScrollBar()->value();
    if (cx < x0 - char_w * 0.5)
        return;
    const int row = doc->visualRowFor(doc->selection().caret_line);
    const qreal cy = row * row_h - verticalScrollBar()->value();
    if (cy < -row_h || cy > viewport()->height())
        return;
    painter.setPen(QPen(t.accent_hover, 2.0));
    painter.drawLine(QPointF(cx, cy + 0.5), QPointF(cx, cy + row_h - 0.5));
}

void AidaCodeEditor::paintGhostText(QPainter& painter, qreal row_h, qreal char_w)
{
    AidaCodeDocument* doc = document();
    if (!doc || !g_sa_settings.ghost_text_enabled || !doc->hasFocus() || doc->readOnly())
        return;
    const auto& ghost = doc->ghost();
    if (ghost.text.empty())
        return;
    const auto& t = theme::tokens();
    qreal gv = 0.0;
    if (ghost.visible_for_line == doc->selection().caret_line &&
        ghost.visible_for_col == doc->selection().caret_col) {
        if (ghost_in_->state() != QAbstractAnimation::Running)
            gv = 1.0;
        else
            gv = ghost_in_->currentValue().toReal();
    }
    const qreal absorb = ghost_absorb_->state() == QAbstractAnimation::Running
        ? ghost_absorb_->currentValue().toReal() : 0.0;
    const qreal vis_alpha = (gv * 0.45 + 0.05) * (1 - absorb * 0.6);
    const qreal gx = textX0() + cellsForColumn(doc->lineAt(doc->selection().caret_line),
        doc->selection().caret_col) * char_w - horizontalScrollBar()->value();
    if (gx < textX0() - char_w)
        return;
    const int row = doc->visualRowFor(doc->selection().caret_line);
    const qreal gy = row * row_h - verticalScrollBar()->value();
    if (gy < -row_h || gy > viewport()->height())
        return;
    painter.setPen(with_alpha(t.text_dim, vis_alpha));
    painter.drawText(QPointF(gx, gy + ascent_),
        QString::fromStdString(ghost.text));
}

void AidaCodeEditor::paintMinimap(QPainter& painter, const QRectF& clip)
{
    AidaCodeDocument* doc = document();
    if (!doc || !minimapVisible() || doc->lineCount() <= 1)
        return;
    const auto& t = theme::tokens();
    const qreal mm_w = minimapWidth();
    const QRectF mm_rect(viewport()->width() - mm_w, 0, mm_w, viewport()->height());
    if (!clip.intersects(mm_rect))
        return;
    qreal hover_v = minimap_hover_anim_->currentValue().toReal();
    painter.fillRect(mm_rect, with_alpha(t.bg_elevated, 0.6 + hover_v * 0.2));
    painter.setPen(QPen(with_alpha(t.border_subtle, 1.0), 1));
    painter.drawLine(QPointF(mm_rect.left(), 0), QPointF(mm_rect.left(), viewport()->height()));

    const int visual_count = doc->visibleLines().empty()
        ? doc->lineCount() : static_cast<int>(doc->visibleLines().size());
    const qreal natural_line_h = visual_count > 0
        ? mm_rect.height() / visual_count : 1.0;
    qreal mm_line_h = std::clamp(natural_line_h, 1.0, 4.0);
    const qreal mm_char_step = std::max(0.6,
        (mm_w - theme::tokens().spacing.xs * 2.0) / 80.0);
    const bool sampled = natural_line_h < 1.0;
    const int row_count = sampled
        ? static_cast<int>(std::ceil(mm_rect.height() / mm_line_h)) : visual_count;
    const qreal base_alpha = std::clamp(0.75 + hover_v * 0.20, 0.65, 1.0);

    for (int row = 0; row < row_count; ++row) {
        const int visual = sampled
            ? static_cast<int>(((row + 0.5) / row_count) * visual_count) : row;
        if (visual < 0 || visual >= visual_count)
            break;
        const int line = doc->logicalLineFor(visual);
        const auto& tokens = doc->tokensForLine(line);
        const std::string& text = doc->lineAt(line);
        const qreal ly = mm_rect.top() + row * mm_line_h;
        if (ly + mm_line_h < mm_rect.top())
            continue;
        if (ly > mm_rect.bottom())
            break;
        qreal lx = mm_rect.left() + theme::tokens().spacing.xs;
        for (const auto& tok : tokens) {
            if (tok.type == syntax::token_type::whitespace) {
                for (uint32_t k = 0; k < tok.length; k++) {
                    const std::size_t character_index = static_cast<std::size_t>(tok.start) +
                        static_cast<std::size_t>(k);
                    if (character_index >= text.size())
                        break;
                    lx += text[character_index] == '\t'
                        ? mm_char_step * std::max(1, g_sa_settings.editor_tab_size)
                        : mm_char_step;
                }
                continue;
            }
            if (tok.length == 0)
                continue;
            const std::size_t token_start = static_cast<std::size_t>(tok.start);
            if (token_start >= text.size())
                continue;
            const std::size_t available = text.size() - token_start;
            const uint32_t eff_len = static_cast<uint32_t>(
                std::min(static_cast<std::size_t>(tok.length), available));
            if (eff_len == 0)
                continue;
            QColor tc = token_color(tok.type);
            tc.setAlphaF(std::clamp(tc.alphaF() * base_alpha, 0.0, 1.0));
            qreal seg_w = static_cast<qreal>(eff_len) * mm_char_step;
            if (lx + seg_w > mm_rect.right() - theme::tokens().spacing.xs)
                seg_w = (mm_rect.right() - theme::tokens().spacing.xs) - lx;
            if (seg_w < 0.5) {
                lx += static_cast<qreal>(eff_len) * mm_char_step;
                continue;
            }
            painter.fillRect(QRectF(lx, ly + 0.5, seg_w, mm_line_h - 0.5), tc);
            lx += static_cast<qreal>(eff_len) * mm_char_step;
            if (lx > mm_rect.right() - theme::tokens().spacing.xs)
                break;
        }
    }

    const qreal content_height = static_cast<qreal>(visual_count) * row_h_;
    if (content_height > 0) {
        qreal view_y0 = mm_rect.top() +
            (verticalScrollBar()->value() / std::max(1.0, content_height)) * mm_rect.height();
        qreal view_h = (viewport()->height() / std::max(1.0, content_height)) * mm_rect.height();
        if (view_h < theme::tokens().spacing.md) view_h = theme::tokens().spacing.md;
        if (view_y0 + view_h > mm_rect.bottom()) view_y0 = mm_rect.bottom() - view_h;
        painter.fillRect(QRectF(mm_rect.left() + 1, view_y0, mm_w - 2, view_h),
            with_alpha(t.accent_glow, 0.22 + hover_v * 0.16));
        painter.setPen(QPen(with_alpha(t.accent, 0.55 + hover_v * 0.25), 1));
        painter.drawRect(QRectF(mm_rect.left() + 1, view_y0, mm_w - 2, view_h));
    }

    const int caret_row = doc->visualRowFor(doc->selection().caret_line);
    const qreal caret_mm_y = mm_rect.top() +
        (static_cast<qreal>(caret_row) / std::max(1.0, static_cast<qreal>(visual_count))) *
            mm_rect.height();
    painter.setPen(QPen(with_alpha(t.accent, 0.65), 1));
    painter.drawLine(QPointF(mm_rect.left() + 2, caret_mm_y),
        QPointF(mm_rect.right() - 2, caret_mm_y));
}

bool AidaCodeEditor::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::Resize) {
        header_->setGeometry(0, 0, width(), header_->height());
        updateScrollbars();
        repositionOverlays();
    }
    return QAbstractScrollArea::viewportEvent(event);
}

void AidaCodeEditor::keyPressEvent(QKeyEvent* event)
{
    AidaCodeDocument* doc = document();
    if (!doc) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const bool alt = event->modifiers() & Qt::AltModifier;
    auto& autocomplete = doc->autocomplete();
    const bool autocomplete_active = autocomplete.popup_visible && !autocomplete.matches.empty();

    if (autocomplete_active) {
        if (event->key() == Qt::Key_Up) {
            autocomplete.selected = (autocomplete.selected - 1 +
                static_cast<int>(autocomplete.matches.size())) %
                static_cast<int>(autocomplete.matches.size());
            autocomplete_popup_->update();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Down) {
            autocomplete.selected = (autocomplete.selected + 1) %
                static_cast<int>(autocomplete.matches.size());
            autocomplete_popup_->update();
            event->accept();
            return;
        }
        if (!doc->readOnly() &&
            (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Return ||
             event->key() == Qt::Key_Enter)) {
            acceptAutocomplete(autocomplete.selected);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            hideAutocomplete();
            event->accept();
            return;
        }
    }

    if (!doc->readOnly() && !ctrl && event->key() == Qt::Key_Tab && !shift &&
        !doc->ghost().text.empty()) {
        doc->ghostTabConsumed = true;
        ghost_absorb_->stop();
        ghost_absorb_->start();
        doc->ghostTabAccept();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && !doc->ghost().text.empty()) {
        doc->ghostDismiss();
        event->accept();
        return;
    }

    switch (event->key()) {
    case Qt::Key_Left: {
        int nl = doc->selection().caret_line, nc = doc->selection().caret_col;
        if (ctrl) {
            if (nc == 0 && nl > 0) {
                nl--;
                nc = doc->lineLength(nl);
            } else if (nc > 0) {
                const std::string& ln = doc->lineAt(nl);
                while (nc > 0 && !is_word_char(ln[static_cast<std::size_t>(nc - 1)])) nc--;
                while (nc > 0 && is_word_char(ln[static_cast<std::size_t>(nc - 1)])) nc--;
            }
        } else if (nc > 0) {
            const std::string& ln = doc->lineAt(nl);
            nc--;
            while (nc > 0 &&
                   (static_cast<unsigned char>(ln[static_cast<std::size_t>(nc)]) & 0xC0) == 0x80)
                nc--;
        } else if (nl > 0) {
            nl--;
            nc = doc->lineLength(nl);
        }
        moveCaret(nl, nc, shift);
        event->accept();
        return;
    }
    case Qt::Key_Right: {
        int nl = doc->selection().caret_line, nc = doc->selection().caret_col;
        if (ctrl) {
            if (nc >= doc->lineLength(nl) && nl < doc->lineCount() - 1) {
                nl++;
                nc = 0;
            } else {
                const std::string& ln = doc->lineAt(nl);
                const int len = static_cast<int>(ln.size());
                nc = std::max(0, nc);
                while (nc < len && is_word_char(ln[static_cast<std::size_t>(nc)])) nc++;
                while (nc < len && !is_word_char(ln[static_cast<std::size_t>(nc)])) nc++;
            }
        } else if (nc < doc->lineLength(nl)) {
            const std::string& ln = doc->lineAt(nl);
            const int len = static_cast<int>(ln.size());
            nc++;
            while (nc < len &&
                   (static_cast<unsigned char>(ln[static_cast<std::size_t>(nc)]) & 0xC0) == 0x80)
                nc++;
        } else if (nl < doc->lineCount() - 1) {
            nl++;
            nc = 0;
        }
        moveCaret(nl, nc, shift);
        event->accept();
        return;
    }
    case Qt::Key_Up:
        if (!alt && !autocomplete_active) {
            const int nl = std::max(0, doc->selection().caret_line - 1);
            moveCaret(nl, doc->clampCol(nl, doc->selection().caret_col), shift);
            event->accept();
            return;
        }
        break;
    case Qt::Key_Down:
        if (!alt && !autocomplete_active) {
            const int nl = std::min(doc->lineCount() - 1, doc->selection().caret_line + 1);
            moveCaret(nl, doc->clampCol(nl, doc->selection().caret_col), shift);
            event->accept();
            return;
        }
        break;
    case Qt::Key_Home:
        if (ctrl) moveCaret(0, 0, shift);
        else moveCaret(doc->selection().caret_line, 0, shift);
        event->accept();
        return;
    case Qt::Key_End:
        if (ctrl) moveCaret(doc->lineCount() - 1, doc->lineLength(doc->lineCount() - 1), shift);
        else moveCaret(doc->selection().caret_line, doc->lineLength(doc->selection().caret_line), shift);
        event->accept();
        return;
    case Qt::Key_PageUp:
        pageStep(true);
        event->accept();
        return;
    case Qt::Key_PageDown:
        pageStep(false);
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        if (doc->readOnly() || ctrl || autocomplete_active)
            break;
        std::string indent;
        char prev_ch = 0;
        char next_ch = 0;
        if (doc->selection().caret_line >= 0 &&
            doc->selection().caret_line < static_cast<int>(doc->cache().lines.size())) {
            auto& ln = doc->cache().lines[static_cast<std::size_t>(doc->selection().caret_line)];
            for (char c : ln) {
                if (c == ' ' || c == '\t') indent += c;
                else break;
            }
            const int cc = doc->clampCol(doc->selection().caret_line, doc->selection().caret_col);
            if (cc > 0 && cc - 1 < static_cast<int>(ln.size()))
                prev_ch = ln[static_cast<std::size_t>(cc - 1)];
            if (cc >= 0 && cc < static_cast<int>(ln.size()))
                next_ch = ln[static_cast<std::size_t>(cc)];
        }
        const bool between_pair = (prev_ch == '{' && next_ch == '}') ||
            (prev_ch == '(' && next_ch == ')') || (prev_ch == '[' && next_ch == ']');
        doc->breakUndoCoalescing();
        if (between_pair) {
            const std::string extra(static_cast<std::size_t>(std::max(1, g_sa_settings.editor_tab_size)), ' ');
            doc->insertTextAtCaret("\n" + indent + extra + "\n" + indent);
            const int target_line = doc->selection().caret_line - 1;
            doc->selection().caret_line = doc->selection().anchor_line = doc->clampLine(target_line);
            doc->selection().caret_col = doc->selection().anchor_col =
                static_cast<int>(indent.size()) + static_cast<int>(extra.size());
            doc->selection().active = false;
        } else {
            if (prev_ch == '{' || prev_ch == '(' || prev_ch == '[' || prev_ch == ':')
                indent += std::string(static_cast<std::size_t>(std::max(1, g_sa_settings.editor_tab_size)), ' ');
            doc->insertTextAtCaret("\n" + indent);
        }
        ensureCaretVisible();
        event->accept();
        return;
    }
    case Qt::Key_Backspace: {
        if (doc->readOnly())
            break;
        if (ctrl) {
            if (doc->selection().has_selection()) {
                doc->deleteSelection();
            } else if (doc->selection().caret_col > 0) {
                doc->pushUndo();
                const int caret_line = doc->clampLine(doc->selection().caret_line);
                auto& ln = doc->cache().lines[static_cast<std::size_t>(caret_line)];
                const int col = doc->clampCol(caret_line, doc->selection().caret_col);
                int start = col;
                while (start > 0 && (ln[static_cast<std::size_t>(start - 1)] == ' ' ||
                        ln[static_cast<std::size_t>(start - 1)] == '\t'))
                    start--;
                if (start > 0) {
                    while (start > 0 && (isalnum(static_cast<unsigned char>(ln[static_cast<std::size_t>(start - 1)])) ||
                            ln[static_cast<std::size_t>(start - 1)] == '_'))
                        start--;
                }
                if (start == col)
                    start = col - 1;
                ln.erase(static_cast<std::size_t>(start), static_cast<std::size_t>(col - start));
                doc->selection().caret_col = doc->selection().anchor_col = start;
                doc->rebuildBufferFromLines();
            } else if (doc->selection().caret_line > 0) {
                doc->pushUndoRange(doc->selection().caret_line - 1, doc->selection().caret_line);
                const int prev = doc->selection().caret_line - 1;
                const std::size_t prev_idx = static_cast<std::size_t>(prev);
                const std::size_t caret_idx = static_cast<std::size_t>(doc->selection().caret_line);
                const int prev_len = static_cast<int>(doc->cache().lines[prev_idx].size());
                doc->cache().lines[prev_idx] += doc->cache().lines[caret_idx];
                doc->cache().lines.erase(doc->cache().lines.begin() + static_cast<std::ptrdiff_t>(caret_idx));
                doc->cache().tokens.erase(doc->cache().tokens.begin() + static_cast<std::ptrdiff_t>(caret_idx));
                doc->cache().line_hashes.erase(doc->cache().line_hashes.begin() +
                    static_cast<std::ptrdiff_t>(caret_idx));
                doc->selection().caret_line = doc->selection().anchor_line = prev;
                doc->selection().caret_col = doc->selection().anchor_col = prev_len;
                doc->rebuildBufferFromLines();
            }
            ensureCaretVisible();
            event->accept();
            return;
        }
        if (doc->selection().has_selection()) {
            doc->deleteSelection();
            doc->breakUndoCoalescing();
        } else if (doc->selection().caret_col > 0) {
            doc->pushUndo();
            doc->breakUndoCoalescing();
            const int caret_line = doc->clampLine(doc->selection().caret_line);
            auto& ln = doc->cache().lines[static_cast<std::size_t>(caret_line)];
            const int caret_col = doc->clampCol(caret_line, doc->selection().caret_col);
            int del_start = caret_col - 1;
            while (del_start > 0 &&
                   (static_cast<unsigned char>(ln[static_cast<std::size_t>(del_start)]) & 0xC0) == 0x80)
                del_start--;
            const int del_len = caret_col - del_start;
            const char prev_c = ln[static_cast<std::size_t>(del_start)];
            const char next_c = (caret_col < static_cast<int>(ln.size()))
                ? ln[static_cast<std::size_t>(caret_col)] : 0;
            const bool pair = g_sa_settings.editor_bracket_match &&
                ((prev_c == '(' && next_c == ')') || (prev_c == '[' && next_c == ']') ||
                 (prev_c == '{' && next_c == '}') || (prev_c == '"' && next_c == '"') ||
                 (prev_c == '\'' && next_c == '\''));
            if (pair) ln.erase(static_cast<std::size_t>(del_start), static_cast<std::size_t>(del_len + 1));
            else      ln.erase(static_cast<std::size_t>(del_start), static_cast<std::size_t>(del_len));
            doc->selection().caret_col = doc->selection().anchor_col = del_start;
            doc->rebuildBufferFromLines();
        } else if (doc->selection().caret_line > 0) {
            doc->pushUndoRange(doc->selection().caret_line - 1, doc->selection().caret_line);
            const int prev = doc->selection().caret_line - 1;
            const std::size_t prev_idx = static_cast<std::size_t>(prev);
            const std::size_t caret_idx = static_cast<std::size_t>(doc->selection().caret_line);
            const int prev_len = static_cast<int>(doc->cache().lines[prev_idx].size());
            doc->cache().lines[prev_idx] += doc->cache().lines[caret_idx];
            doc->cache().lines.erase(doc->cache().lines.begin() + static_cast<std::ptrdiff_t>(caret_idx));
            doc->cache().tokens.erase(doc->cache().tokens.begin() + static_cast<std::ptrdiff_t>(caret_idx));
            doc->cache().line_hashes.erase(doc->cache().line_hashes.begin() +
                static_cast<std::ptrdiff_t>(caret_idx));
            doc->selection().caret_line = doc->selection().anchor_line = prev;
            doc->selection().caret_col = doc->selection().anchor_col = prev_len;
            doc->rebuildBufferFromLines();
        }
        ensureCaretVisible();
        event->accept();
        return;
    }
    case Qt::Key_Delete:
        if (!doc->readOnly()) {
            doc->deleteForward();
            ensureCaretVisible();
            event->accept();
            return;
        }
        break;
    case Qt::Key_Tab: {
        if (doc->readOnly() || ctrl)
            break;
        const int tab = std::max(1, g_sa_settings.editor_tab_size);
        if (shift) {
            if (autocomplete_active)
                break;
            if (doc->selection().has_selection()) {
                int l0, c0, l1, c1;
                doc->selectionOrdered(l0, c0, l1, c1);
                static_cast<void>(c0);
                l0 = doc->clampLine(l0);
                l1 = doc->clampLine(l1);
                const int end_line = (c1 == 0 && l1 > l0) ? l1 - 1 : l1;
                doc->pushUndo();
                doc->breakUndoCoalescing();
                for (int li = l0; li <= end_line && li < doc->lineCount(); ++li) {
                    std::string& ln = doc->cache().lines[static_cast<std::size_t>(li)];
                    int removed = 0;
                    if (!ln.empty() && ln[0] == '\t') { ln.erase(0, 1); removed = 1; }
                    else {
                        while (removed < tab && !ln.empty() && ln[0] == ' ') {
                            ln.erase(0, 1);
                            removed++;
                        }
                    }
                    if (li == doc->selection().anchor_line)
                        doc->selection().anchor_col = std::max(0, doc->selection().anchor_col - removed);
                    if (li == doc->selection().caret_line)
                        doc->selection().caret_col = std::max(0, doc->selection().caret_col - removed);
                }
                doc->rebuildBufferFromLines();
            } else {
                const int caret_line = doc->clampLine(doc->selection().caret_line);
                std::string& ln = doc->cache().lines[static_cast<std::size_t>(caret_line)];
                int removed = 0;
                if (!ln.empty() && ln[0] == '\t') { ln.erase(0, 1); removed = 1; }
                else {
                    while (removed < tab && !ln.empty() && ln[0] == ' ') {
                        ln.erase(0, 1);
                        removed++;
                    }
                }
                if (removed > 0) {
                    doc->pushUndo();
                    doc->breakUndoCoalescing();
                    doc->selection().caret_col = doc->selection().anchor_col =
                        std::max(0, doc->selection().caret_col - removed);
                    doc->rebuildBufferFromLines();
                }
            }
        } else {
            if (autocomplete_active)
                break;
            if (doc->selection().has_selection() &&
                doc->selection().anchor_line != doc->selection().caret_line) {
                int l0, c0, l1, c1;
                doc->selectionOrdered(l0, c0, l1, c1);
                static_cast<void>(c0);
                l0 = doc->clampLine(l0);
                l1 = doc->clampLine(l1);
                const int end_line = (c1 == 0 && l1 > l0) ? l1 - 1 : l1;
                doc->pushUndo();
                doc->breakUndoCoalescing();
                const std::string pad(static_cast<std::size_t>(tab), ' ');
                for (int li = l0; li <= end_line && li < doc->lineCount(); ++li) {
                    const std::size_t line_idx = static_cast<std::size_t>(li);
                    if (doc->cache().lines[line_idx].empty()) continue;
                    doc->cache().lines[line_idx].insert(0, pad);
                    if (li == doc->selection().anchor_line) doc->selection().anchor_col += tab;
                    if (li == doc->selection().caret_line)  doc->selection().caret_col += tab;
                }
                doc->rebuildBufferFromLines();
            } else {
                const int col = doc->clampCol(doc->selection().caret_line, doc->selection().caret_col);
                int to_next = tab - (col % tab);
                if (to_next <= 0) to_next = tab;
                const std::string spaces(static_cast<std::size_t>(to_next), ' ');
                doc->insertTextAtCaret(spaces);
                doc->breakUndoCoalescing();
            }
        }
        ensureCaretVisible();
        event->accept();
        return;
    }
    case Qt::Key_Escape:
        if (doc->find().visible) {
            doc->find().visible = false;
            find_overlay_->hide();
        } else if (doc->goTo().visible) {
            doc->goTo().visible = false;
            goto_overlay_->hide();
        } else if (autocomplete.popup_visible) {
            hideAutocomplete();
        }
        setFocus(Qt::OtherFocusReason);
        event->accept();
        return;
    default:
        break;
    }

    if (!ctrl && !doc->readOnly() && !event->text().isEmpty()) {
        handleTextInput(event->text());
        event->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void AidaCodeEditor::handleTextInput(const QString& text)
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    for (const QChar ch : text) {
        const uint32_t cp = ch.unicode();
        if (cp < 32)
            continue;
        std::string utf8;
        if (cp < 0x80) {
            utf8.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x110000) {
            utf8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            continue;
        }

        const char ascii = (cp < 0x80) ? static_cast<char>(cp) : 0;
        bool handled_bracket = false;

        if (g_sa_settings.editor_bracket_match && ascii != 0) {
            const std::string& cl = doc->lineAt(doc->selection().caret_line);
            const int caret_col = doc->selection().caret_col;
            const char next_ch = (caret_col >= 0 && caret_col < static_cast<int>(cl.size()))
                ? cl[static_cast<std::size_t>(caret_col)] : 0;

            if (is_close_bracket(ascii) && next_ch == ascii && !doc->selection().has_selection()) {
                doc->selection().caret_col = doc->selection().anchor_col = doc->selection().caret_col + 1;
                doc->selection().active = false;
                handled_bracket = true;
                doc->breakUndoCoalescing();
            } else if (ascii == '"' && next_ch == '"' && !doc->selection().has_selection()) {
                doc->selection().caret_col = doc->selection().anchor_col = doc->selection().caret_col + 1;
                doc->selection().active = false;
                handled_bracket = true;
                doc->breakUndoCoalescing();
            } else if (is_open_bracket(ascii) ||
                       (ascii == '"' &&
                        (next_ch == 0 || next_ch == ')' || next_ch == ']' ||
                         next_ch == '}' || next_ch == ' ' || next_ch == '\t' ||
                         next_ch == ','))) {
                if (doc->selection().has_selection()) {
                    const std::string sel = doc->selectedText();
                    const char close = (ascii == '"') ? '"' : matching_close_bracket(ascii);
                    doc->insertTextAtCaret(std::string(1, ascii) + sel + std::string(1, close));
                    doc->breakUndoCoalescing();
                } else {
                    const char close = (ascii == '"') ? '"' : matching_close_bracket(ascii);
                    doc->insertTextAtCaret(std::string(1, ascii) + std::string(1, close));
                    doc->selection().caret_col = doc->selection().anchor_col =
                        doc->selection().caret_col - 1;
                    doc->breakUndoCoalescing();
                }
                handled_bracket = true;
            }
        }

        if (!handled_bracket) {
            int kind = 0;
            if (ascii != 0) {
                const bool word = (ascii >= 'a' && ascii <= 'z') ||
                    (ascii >= 'A' && ascii <= 'Z') ||
                    (ascii >= '0' && ascii <= '9') || ascii == '_';
                kind = word ? 1 : 2;
            }
            doc->insertTextAtCaret(utf8, kind);
            if (kind == 2) doc->breakUndoCoalescing();
        }
        ensureCaretVisible();

        if (g_sa_settings.editor_auto_complete && ascii != 0) {
            const bool word_ch = (ascii >= 'a' && ascii <= 'z') ||
                (ascii >= 'A' && ascii <= 'Z') ||
                (ascii >= '0' && ascii <= '9') || ascii == '_';
            if (word_ch) {
                const int caret_line = doc->clampLine(doc->selection().caret_line);
                auto& ln = doc->cache().lines[static_cast<std::size_t>(caret_line)];
                const int cursor = doc->clampCol(caret_line, doc->selection().caret_col);
                int ws = cursor;
                while (ws > 0 && (isalnum(static_cast<unsigned char>(ln[static_cast<std::size_t>(ws - 1)])) || ln[static_cast<std::size_t>(ws - 1)] == '_'))
                    ws--;
                if (cursor > ws) {
                    doc->rebuildAutocomplete(ln.substr(static_cast<std::size_t>(ws),
                        static_cast<std::size_t>(cursor - ws)), caret_line);
                    doc->autocomplete().cursor_col = doc->selection().caret_col;
                    updateAutocompletePopup();
                } else {
                    hideAutocomplete();
                }
            } else {
                hideAutocomplete();
            }
        }
    }
    doc->ghostTabConsumed = false;
    triggerGhostIfIdle();
}

void AidaCodeEditor::moveCaret(int new_line, int new_col, bool shift)
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    if (shift) doc->selection().active = true;
    else {
        doc->selection().active = false;
        doc->selection().anchor_line = new_line;
        doc->selection().anchor_col = new_col;
    }
    doc->selection().caret_line = new_line;
    doc->selection().caret_col = new_col;
    doc->blinkOn() = true;
    doc->breakUndoCoalescing();
    hideAutocomplete();
    doc->ghostTabConsumed = false;
    if (!doc->ghost().text.empty() || doc->ghost().requesting)
        doc->ghostResetForCaretMove();
    triggerGhostIfIdle();
    ensureCaretVisible();
    viewport()->update();
}

void AidaCodeEditor::pageStep(bool up)
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    const int page = std::max(1, static_cast<int>(viewport()->height() / row_h_) - 2);
    const int nl = up ? std::max(0, doc->selection().caret_line - page)
        : std::min(doc->lineCount() - 1, doc->selection().caret_line + page);
    moveCaret(nl, doc->clampCol(nl, doc->selection().caret_col),
        QApplication::keyboardModifiers() & Qt::ShiftModifier);
}

void AidaCodeEditor::acceptAutocomplete(int index)
{
    AidaCodeDocument* doc = document();
    if (!doc || doc->readOnly())
        return;
    auto& autocomplete = doc->autocomplete();
    if (index < 0 || index >= static_cast<int>(autocomplete.matches.size()))
        return;
    const int caret_line = doc->clampLine(doc->selection().caret_line);
    auto& ln = doc->cache().lines[static_cast<std::size_t>(caret_line)];
    const int cursor = doc->clampCol(caret_line, doc->selection().caret_col);
    int ws = cursor;
    while (ws > 0 && (isalnum(static_cast<unsigned char>(ln[static_cast<std::size_t>(ws - 1)])) || ln[static_cast<std::size_t>(ws - 1)] == '_'))
        ws--;
    doc->pushUndo();
    doc->breakUndoCoalescing();
    const std::string& chosen = autocomplete.matches[static_cast<std::size_t>(index)];
    ln.erase(static_cast<std::size_t>(ws), static_cast<std::size_t>(cursor - ws));
    ln.insert(static_cast<std::size_t>(ws), chosen);
    doc->selection().caret_col = doc->selection().anchor_col = ws + static_cast<int>(chosen.size());
    doc->rebuildBufferFromLines();
    hideAutocomplete();
    ensureCaretVisible();
}

void AidaCodeEditor::updateAutocompletePopup()
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    if (!doc->autocomplete().popup_visible || doc->autocomplete().matches.empty()) {
        hideAutocomplete();
        return;
    }
    autocomplete_popup_->refreshGeometry();
}

void AidaCodeEditor::hideAutocomplete()
{
    AidaCodeDocument* doc = document();
    if (doc) {
        doc->autocomplete().popup_visible = false;
        doc->autocomplete().matches.clear();
    }
    autocomplete_popup_->hide();
}

void AidaCodeEditor::updateFindOverlayGeometry()
{
    const auto& spacing = theme::tokens().spacing;
    const QSize hint = find_overlay_->sizeHint();
    const int margin = spacing.sm;
    const int w = std::min(static_cast<int>(k_find_bar_width),
        std::max(120, width() - margin * 2));
    const int x = std::max(margin, width() - w - spacing.xl);
    const int top = header_ ? header_->height() + 2 : 2;
    find_overlay_->setGeometry(x, top, w, std::max(hint.height(), 40));
}

void AidaCodeEditor::updateGotoOverlayGeometry()
{
    const int margin = theme::tokens().spacing.sm;
    const int top = header_ ? header_->height() + 2 : 2;
    const int w = std::min(static_cast<int>(k_goto_bar_width),
        std::max(80, width() - margin * 2));
    goto_overlay_->setGeometry(margin, top, w, goto_overlay_->sizeHint().height());
}

void AidaCodeEditor::repositionOverlays()
{
    if (find_overlay_ && find_overlay_->isVisible())
        updateFindOverlayGeometry();
    if (goto_overlay_ && goto_overlay_->isVisible())
        updateGotoOverlayGeometry();
    if (autocomplete_popup_ && autocomplete_popup_->isVisible())
        autocomplete_popup_->refreshGeometry();
}

void AidaCodeEditor::showFindOverlay(bool replace_mode)
{
    updateFindOverlayGeometry();
    find_overlay_->openWith(replace_mode);
}

void AidaCodeEditor::showGotoOverlay()
{
    updateGotoOverlayGeometry();
    goto_overlay_->openWith();
}

bool AidaCodeEditor::overlaysHaveFocus() const
{
    return (find_overlay_->isVisible() && find_overlay_->hasFocus()) ||
        (goto_overlay_->isVisible() && goto_overlay_->hasFocus());
}

void AidaCodeEditor::applyFocusState()
{
    AidaCodeDocument* doc = document();
    if (!doc || !registry_)
        return;
    aida::ui::application_ui::set_editor_focus(doc->hasFocus(),
        overlaysHaveFocus(), doc->hasPendingDiff());
}

void AidaCodeEditor::triggerGhostIfIdle()
{
    AidaCodeDocument* doc = document();
    if (!doc || !g_sa_settings.ghost_text_enabled || !doc->hasFocus() || doc->readOnly() ||
        doc->isMapped())
        return;
    ghost_debounce_->start();
}

void AidaCodeEditor::mousePressEvent(QMouseEvent* event)
{
    AidaCodeDocument* doc = document();
    if (!doc) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    if (doc->hasPendingDiff() && event->button() == Qt::LeftButton) {
        if (handleDiffMousePress(event->position(), event->modifiers())) {
            viewport()->update();
            event->accept();
            return;
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const QPointF pos = event->position();
        const qreal source_gutter_w = sourceGutterWidth();
        const qreal fold_gutter_w = foldGutterWidth();
        if (source_gutter_w > 0.0 && pos.x() < source_gutter_w) {
            const int marker_line = doc->logicalLineFor(static_cast<int>(
                (pos.y() + verticalScrollBar()->value()) / row_h_));
            doc->selection().anchor_line = doc->selection().caret_line = marker_line;
            doc->selection().anchor_col = doc->selection().caret_col = 0;
            doc->selection().active = false;
            std::string ignored;
            static_cast<void>(source_debug_service::request_toggle(doc->filepath(),
                static_cast<std::uint32_t>(marker_line + 1), &ignored));
            viewport()->update();
            event->accept();
            return;
        }
        if (pos.x() >= source_gutter_w && pos.x() < source_gutter_w + fold_gutter_w) {
            const int line = doc->logicalLineFor(static_cast<int>(
                (pos.y() + verticalScrollBar()->value()) / row_h_));
            if (doc->cachedFoldEnd(line) > line)
                static_cast<void>(doc->toggleFold(line));
            viewport()->update();
            event->accept();
            return;
        }
        if (minimapVisible() && pos.x() >= viewport()->width() - minimapWidth()) {
            dragging_minimap_ = true;
            const qreal local = std::clamp(pos.y() / viewport()->height(), 0.0, 1.0);
            const int visual_count = doc->visibleLines().empty()
                ? doc->lineCount() : static_cast<int>(doc->visibleLines().size());
            const float target = static_cast<float>(local) * std::max(0.0f,
                static_cast<float>(visual_count) * static_cast<float>(row_h_) -
                    viewport()->height() * 0.5f);
            doc->applyScrollTarget(target);
            verticalScrollBar()->setValue(static_cast<int>(target));
            event->accept();
            return;
        }
        int ml, mc;
        screenToLineCol(pos, ml, mc);
        doc->registerClick(click_clock_.elapsed() / 1000.0);
        if (doc->clickCount() >= 3) {
            doc->selection().anchor_line = doc->selection().caret_line = ml;
            doc->selection().anchor_col = 0;
            doc->selection().caret_col = doc->lineLength(ml);
            doc->selection().active = true;
        } else {
            const bool shift = event->modifiers() & Qt::ShiftModifier;
            if (!shift) {
                doc->selection().anchor_line = ml;
                doc->selection().anchor_col = mc;
            }
            doc->selection().caret_line = ml;
            doc->selection().caret_col = mc;
            doc->selection().active = shift;
            mouse_selecting_ = true;
        }
        doc->blinkOn() = true;
        doc->breakUndoCoalescing();
        doc->ghostTabConsumed = false;
        if (!doc->ghost().text.empty() || doc->ghost().requesting)
            doc->ghostResetForCaretMove();
        hideAutocomplete();
        setFocus(Qt::MouseFocusReason);
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void AidaCodeEditor::mouseDoubleClickEvent(QMouseEvent* event)
{
    AidaCodeDocument* doc = document();
    if (!doc) {
        QAbstractScrollArea::mouseDoubleClickEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton) {
        int ml, mc;
        screenToLineCol(event->position(), ml, mc);
        int ws = 0, we = 0;
        {
            auto& ln = doc->lineAt(ml);
            ws = std::clamp(mc, 0, static_cast<int>(ln.size()));
            we = ws;
            while (ws > 0 && is_word_char(ln[static_cast<std::size_t>(ws - 1)])) ws--;
            while (we < static_cast<int>(ln.size()) && is_word_char(ln[static_cast<std::size_t>(we)])) we++;
        }
        doc->selection().anchor_line = doc->selection().caret_line = ml;
        doc->selection().anchor_col = ws;
        doc->selection().caret_col = we;
        doc->selection().active = we > ws;
        mouse_selecting_ = false;
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void AidaCodeEditor::mouseMoveEvent(QMouseEvent* event)
{
    AidaCodeDocument* doc = document();
    if (!doc) {
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    if (doc->hasPendingDiff()) {
        viewport()->update();
        event->accept();
        return;
    }
    if (minimapVisible()) {
        const bool over_minimap = event->position().x() >= viewport()->width() - minimapWidth();
        if (over_minimap && minimap_hover_anim_->state() != QAbstractAnimation::Running &&
            minimap_hover_anim_->currentValue().toReal() < 1.0) {
            minimap_hover_anim_->setDirection(QAbstractAnimation::Forward);
            minimap_hover_anim_->start();
        } else if (!over_minimap && minimap_hover_anim_->state() != QAbstractAnimation::Running &&
            minimap_hover_anim_->currentValue().toReal() > 0.0) {
            minimap_hover_anim_->setDirection(QAbstractAnimation::Backward);
            minimap_hover_anim_->start();
        }
    }
    if (dragging_minimap_ && (event->buttons() & Qt::LeftButton)) {
        const qreal local = std::clamp(event->position().y() / viewport()->height(), 0.0, 1.0);
        const int visual_count = doc->visibleLines().empty()
            ? doc->lineCount() : static_cast<int>(doc->visibleLines().size());
        const float target = static_cast<float>(local) * std::max(0.0f,
            static_cast<float>(visual_count) * static_cast<float>(row_h_) -
                viewport()->height() * 0.5f);
        doc->applyScrollTarget(target);
        verticalScrollBar()->setValue(static_cast<int>(target));
        viewport()->update();
        event->accept();
        return;
    }
    if (mouse_selecting_ && (event->buttons() & Qt::LeftButton)) {
        int ml, mc;
        screenToLineCol(event->position(), ml, mc);
        doc->selection().caret_line = ml;
        doc->selection().caret_col = mc;
        doc->selection().active = true;
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void AidaCodeEditor::mouseReleaseEvent(QMouseEvent* event)
{
    AidaCodeDocument* doc = document();
    if (event->button() == Qt::LeftButton) {
        mouse_selecting_ = false;
        dragging_minimap_ = false;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void AidaCodeEditor::wheelEvent(QWheelEvent* event)
{
    AidaCodeDocument* doc = document();
    if (!doc) {
        QAbstractScrollArea::wheelEvent(event);
        return;
    }
    if (event->modifiers() & Qt::ShiftModifier) {
        QCoreApplication::sendEvent(horizontalScrollBar(), event);
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void AidaCodeEditor::focusInEvent(QFocusEvent* event)
{
    AidaCodeDocument* doc = document();
    if (doc) {
        doc->hasFocus() = true;
        doc->blinkOn() = true;
        if (registry_)
            registry_->selectForActions(document_id_);
        if (blink_timer_->interval() > 0)
            blink_timer_->start();
        applyFocusState();
        triggerGhostIfIdle();
    }
    Q_EMIT focusGained(document_id_);
    QAbstractScrollArea::focusInEvent(event);
}

void AidaCodeEditor::focusOutEvent(QFocusEvent* event)
{
    AidaCodeDocument* doc = document();
    if (doc) {
        doc->hasFocus() = false;
        blink_timer_->stop();
        ghost_debounce_->stop();
        doc->ghostCancelRequest();
        applyFocusState();
    }
    hideAutocomplete();
    QAbstractScrollArea::focusOutEvent(event);
}

void AidaCodeEditor::contextMenuEvent(QContextMenuEvent* event)
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    if (!doc->selection().has_selection() && event->reason() == QContextMenuEvent::Mouse) {
        int line = 0;
        int column = 0;
        screenToLineCol(event->pos(), line, column);
        doc->selection().anchor_line = doc->selection().caret_line = line;
        doc->selection().anchor_col = doc->selection().caret_col = column;
        doc->selection().active = false;
    }
    doc->hasFocus() = true;
    applyFocusState();
    const aida::ui::context_menu_open_origin_t origin =
        event->reason() == QContextMenuEvent::Keyboard
            ? aida::ui::context_menu_open_origin_t::menu_key
            : aida::ui::context_menu_open_origin_t::pointer;
    aida::ui::application_ui::open_editor_context_menu(origin);
    const bool review = doc->hasPendingDiff();
    const aida::ui::stable_menu_id_t menu(review
        ? "menu.editor.review" : "menu.editor.text");
    documents::show_context_menu(menu,
        documents::make_menu_snapshot(aida::ui::stable_view_id_t("document.code"),
            aida::ui::stable_context_type_id_t("context.editor.text")),
        origin, event->globalPos(), this);
    event->accept();
}

void AidaCodeEditor::dragEnterEvent(QDragEnterEvent* event)
{
    const QMimeData* mime = event->mimeData();
    if (mime && mime->hasUrls())
        event->acceptProposedAction();
}

void AidaCodeEditor::dropEvent(QDropEvent* event)
{
    const QMimeData* mime = event->mimeData();
    if (!mime || !mime->hasUrls())
        return;
    const QList<QUrl> urls = mime->urls();
    for (const QUrl& url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString local = url.toLocalFile();
        diag::log_tagged_fmt("qt_code_editor", "editor_drop url=%s",
            local.toUtf8().constData());
        Q_EMIT fileDropRequested(local);
    }
    event->acceptProposedAction();
}

void AidaCodeEditor::paintDiffReview(QPainter& painter, const QRectF& clip)
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    const auto& t = theme::tokens();
    const qreal code_w = viewport()->width();
    const qreal hdr_h = diffHeaderHeight();
    const qreal body_y0 = hdr_h;
    const qreal body_h = viewport()->height() - hdr_h;

    diff_paint_snapshot_t snap;
    {
        std::unique_lock<std::mutex> diff_lock(doc->diffMutex(), std::try_to_lock);
        if (!diff_lock.owns_lock()) {
            painter.fillRect(QRectF(0, 0, code_w, hdr_h), t.panel_header);
            painter.setPen(t.text_dim);
            painter.drawText(QPointF(theme::tokens().spacing.md, theme::tokens().spacing.xxl),
                QStringLiteral("AI edit review is updating..."));
            return;
        }
        const auto& diff = doc->pendingDiff();
        snap.origin = diff.origin;
        snap.total_added = diff.total_added;
        snap.total_removed = diff.total_removed;
        snap.old_line_count = static_cast<int>(diff.old_lines.size());
        snap.new_line_count = static_cast<int>(diff.new_lines.size());
        snap.hunks.reserve(diff.hunks.size());
        for (const auto& h : diff.hunks) {
            if (h.state == code_editor_widget::diff_hunk_state_t::pending)
                ++snap.pending;
            diff_paint_hunk_t copy;
            copy.state = h.state;
            copy.stable_id = h.stable_id;
            copy.old_start = h.old_start;
            copy.old_count = h.old_count;
            copy.new_start = h.new_start;
            copy.new_count = h.new_count;
            snap.hunks.push_back(copy);
        }
        const std::vector<diff_vis_row_t> rows = build_diff_rows(diff);
        snap.total_rows = static_cast<int>(rows.size());
        const qreal content_h = static_cast<qreal>(rows.size()) * row_h_;
        const qreal diff_max_scroll = std::max(0.0, content_h - body_h);
        snap.scroll_y = std::clamp(static_cast<qreal>(verticalScrollBar()->value()),
            0.0, diff_max_scroll);
        const qreal exposed_top = (std::max)(clip.top(), body_y0) - body_y0;
        const qreal exposed_bottom = (std::min)(clip.bottom(), body_y0 + body_h) - body_y0;
        const int diff_first = std::max(0,
            static_cast<int>((snap.scroll_y + exposed_top) / row_h_) - 1);
        const int diff_last = rows.empty() || exposed_bottom <= exposed_top ? -1
            : static_cast<int>((std::min)(rows.size() - 1,
                static_cast<std::size_t>(static_cast<int>((snap.scroll_y + exposed_bottom) / row_h_) + 1)));
        snap.first_row = diff_first;
        if (diff_last >= diff_first) {
            snap.rows.reserve(static_cast<std::size_t>(diff_last - diff_first + 1));
            for (int ri = diff_first; ri <= diff_last; ++ri) {
                const diff_vis_row_t& r = rows[static_cast<std::size_t>(ri)];
                diff_paint_row_t copy;
                copy.hunk = r.hunk;
                copy.is_hunk_head = r.is_hunk_head;
                copy.kind = r.kind;
                copy.old_no = r.old_no;
                copy.new_no = r.new_no;
                copy.has_text = r.text != nullptr;
                if (r.text)
                    copy.text = *r.text;
                snap.rows.push_back(std::move(copy));
            }
        }
        const auto identity = doc->selectedReviewHunkIdentityAssumeLocked();
        if (identity.valid()) {
            for (std::size_t hi = 0; hi < diff.hunks.size(); ++hi) {
                if (diff.hunks[hi].stable_id == identity.stable_hunk_id) {
                    snap.selected_hunk = static_cast<int>(hi);
                    break;
                }
            }
        }
    }
    diff_content_rows_ = snap.total_rows;

    painter.fillRect(QRectF(0, 0, code_w, hdr_h), t.panel_header);
    painter.setPen(QPen(with_alpha(t.border_subtle, 1.0), 1));
    painter.drawLine(QPointF(0, hdr_h - 1), QPointF(code_w, hdr_h - 1));

    const QFont hdr_font = theme::fonts::body();
    std::string title = "AI Edit";
    if (!snap.origin.empty()) title += "  --  " + snap.origin;
    painter.setFont(hdr_font);
    painter.setPen(t.text_primary);
    painter.drawText(QPointF(theme::tokens().spacing.md, 6 + QFontMetricsF(hdr_font).ascent()),
        QString::fromStdString(title));

    char stats_buffer[96];
    snprintf(stats_buffer, sizeof(stats_buffer), "+%d  -%d   %zu hunk%s   %d pending",
        snap.total_added, snap.total_removed, snap.hunks.size(),
        snap.hunks.size() == 1 ? "" : "s", snap.pending);
    painter.setPen(t.text_secondary);
    painter.drawText(QPointF(theme::tokens().spacing.md, 22 + QFontMetricsF(hdr_font).ascent()),
        QString::fromLatin1(stats_buffer));

    const qreal bw = k_diff_action_width;
    const qreal bh = theme::tokens().row.compact;
    const qreal by = (hdr_h - bh) * 0.5;
    const qreal bx_reject = code_w - theme::tokens().spacing.md - bw;
    const qreal bx_accept = bx_reject - theme::tokens().spacing.sm - bw;
    const QPointF mouse = viewport()->mapFromGlobal(QCursor::pos());
    const auto header_button = [&](const QString& label, qreal bx, const QColor& base) {
        const bool hov = mouse.x() >= bx && mouse.x() <= bx + bw &&
            mouse.y() >= by && mouse.y() <= by + bh;
        painter.setPen(QPen(with_alpha(base, hov ? 0.95 : 0.55), 1));
        painter.setBrush(with_alpha(base, hov ? 0.32 : 0.20));
        painter.drawRoundedRect(QRectF(bx, by, bw, bh),
            theme::tokens().radius.md, theme::tokens().radius.md);
        painter.setFont(hdr_font);
        painter.setPen(with_alpha(base, 1.0));
        painter.drawText(QRectF(bx, by, bw, bh), Qt::AlignCenter, label);
    };
    header_button(QStringLiteral("Accept All"), bx_accept, t.success);
    header_button(QStringLiteral("Reject All"), bx_reject, t.error);

    const int diff_digits = std::max(3, digit_count(std::max(snap.old_line_count,
        snap.new_line_count)));
    const qreal gutter_pad = theme::tokens().spacing.xs + theme::tokens().spacing.xxs;
    const qreal diff_gutter_w = gutter_pad + char_w_ * (diff_digits * 2 + 1) +
        theme::tokens().spacing.sm;
    const qreal sign_x = diff_gutter_w + theme::tokens().spacing.xs;
    const qreal diff_text_x = sign_x + char_w_ * 1.6;

    painter.setClipRect(QRectF(0, body_y0, code_w, body_h));

    int new_hover_hunk = -1;
    const bool body_hovered = QRectF(0, body_y0, code_w, body_h).contains(mouse);
    const int selected_hunk = snap.selected_hunk;

    const QFont code_font = font();
    const QFontMetricsF code_fm(code_font);
    for (std::size_t vi = 0; vi < snap.rows.size(); ++vi) {
        const diff_paint_row_t& r = snap.rows[vi];
        const qreal ry = body_y0 + static_cast<qreal>(snap.first_row) * row_h_ +
            static_cast<qreal>(vi) * row_h_ - snap.scroll_y;

        if (r.is_hunk_head) {
            if (r.hunk < 0 || static_cast<std::size_t>(r.hunk) >= snap.hunks.size()) continue;
            const diff_paint_hunk_t& h = snap.hunks[static_cast<std::size_t>(r.hunk)];
            const bool selected = r.hunk == selected_hunk;
            const bool row_hovered = body_hovered && mouse.y() >= ry && mouse.y() < ry + row_h_;
            painter.fillRect(QRectF(0, ry, code_w, row_h_),
                with_alpha(t.accent_glow, selected ? 0.34 : 0.16));
            if (selected) {
                painter.setPen(QPen(with_alpha(t.accent, 0.9), 1.5));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(QRectF(0.5, ry + 0.5, code_w - 1.0, row_h_ - 1.0));
            }
            char hb[64];
            snprintf(hb, sizeof(hb), "@@ -%d,%d +%d,%d @@",
                h.old_start + 1, h.old_count, h.new_start + 1, h.new_count);
            painter.setFont(code_font);
            painter.setPen(t.accent);
            painter.drawText(QPointF(theme::tokens().spacing.md, ry + ascent_),
                QString::fromLatin1(hb));

            const char* state_label =
                h.state == code_editor_widget::diff_hunk_state_t::accepted ? "ACCEPTED" :
                h.state == code_editor_widget::diff_hunk_state_t::rejected ? "REJECTED" : nullptr;
            if (state_label) {
                painter.setPen(h.state == code_editor_widget::diff_hunk_state_t::accepted
                    ? t.success : t.error);
                painter.drawText(QPointF(code_w - theme::tokens().spacing.md -
                    code_fm.horizontalAdvance(QString::fromLatin1(state_label)), ry + ascent_),
                    QString::fromLatin1(state_label));
            } else if (row_hovered || selected) {
                const qreal hbw = char_w_ * 7.0 + theme::tokens().spacing.sm;
                const qreal hbh = row_h_ - theme::tokens().spacing.xs;
                const qreal hbx_rej = code_w - theme::tokens().spacing.md - hbw;
                const qreal hbx_acc = hbx_rej - theme::tokens().spacing.xs -
                    theme::tokens().spacing.xxs - hbw;
                const qreal hby = ry + theme::tokens().spacing.xxs;
                const QFont btn_font = theme::fonts::caption();
                const auto hunk_button = [&](const QString& label, qreal bx, const QColor& base) {
                    const bool hov = mouse.x() >= bx && mouse.x() <= bx + hbw &&
                        mouse.y() >= hby && mouse.y() <= hby + hbh;
                    painter.setPen(QPen(with_alpha(base, hov ? 0.95 : 0.55), 1));
                    painter.setBrush(with_alpha(base, hov ? 0.32 : 0.20));
                    painter.drawRoundedRect(QRectF(bx, hby, hbw, hbh),
                        theme::tokens().radius.sm, theme::tokens().radius.sm);
                    painter.setFont(btn_font);
                    painter.setPen(with_alpha(base, 1.0));
                    painter.drawText(QRectF(bx, hby, hbw, hbh), Qt::AlignCenter, label);
                };
                hunk_button(QStringLiteral("Accept"), hbx_acc, t.success);
                hunk_button(QStringLiteral("Reject"), hbx_rej, t.error);
            }
            if (row_hovered)
                new_hover_hunk = r.hunk;
            continue;
        }

        if (!r.has_text) {
            const qreal midy = ry + row_h_ * 0.5;
            painter.setPen(QPen(with_alpha(t.border_subtle, 1.0), 1));
            for (qreal dx = theme::tokens().spacing.md; dx < code_w - theme::tokens().spacing.md;
                 dx += theme::tokens().spacing.sm)
                painter.drawLine(QPointF(dx, midy), QPointF(dx + 3.0, midy));
            continue;
        }

        const bool is_add = r.kind == code_editor_widget::diff_line_kind_t::added;
        const bool is_rem = r.kind == code_editor_widget::diff_line_kind_t::removed;
        bool hunk_resolved = false;
        QColor wash;
        QColor bar;
        if (is_add) {
            wash = with_alpha(t.success, 0.14);
            bar = with_alpha(t.success, 0.9);
        } else if (is_rem) {
            wash = with_alpha(t.error, 0.14);
            bar = with_alpha(t.error, 0.9);
        }
        if (r.hunk >= 0 && static_cast<std::size_t>(r.hunk) < snap.hunks.size()) {
            const diff_paint_hunk_t& h = snap.hunks[static_cast<std::size_t>(r.hunk)];
            hunk_resolved = h.state != code_editor_widget::diff_hunk_state_t::pending;
            if (h.state == code_editor_widget::diff_hunk_state_t::rejected && is_add) {
                wash = with_alpha(t.text_dim, 0.06);
                bar = with_alpha(t.text_dim, 0.4);
            } else if (h.state == code_editor_widget::diff_hunk_state_t::accepted && is_rem) {
                wash = with_alpha(t.text_dim, 0.06);
                bar = with_alpha(t.text_dim, 0.4);
            }
        }
        if (wash.isValid()) painter.fillRect(QRectF(0, ry, code_w, row_h_), wash);
        if (bar.isValid()) painter.fillRect(QRectF(0, ry, 3.0, row_h_), bar);
        if (r.hunk >= 0 && r.hunk == new_hover_hunk && !hunk_resolved)
            painter.fillRect(QRectF(3.0, ry, code_w - 3.0, row_h_), with_alpha(t.accent, 0.05));
        if (r.hunk >= 0 && r.hunk == selected_hunk && !hunk_resolved)
            painter.fillRect(QRectF(3.0, ry, code_w - 3.0, row_h_), with_alpha(t.accent, 0.09));

        char numbuf[24];
        painter.setFont(code_font);
        painter.setPen(t.text_lineno);
        if (r.old_no > 0) snprintf(numbuf, sizeof(numbuf), "%*d", diff_digits, r.old_no);
        else snprintf(numbuf, sizeof(numbuf), "%*s", diff_digits, "");
        painter.drawText(QPointF(gutter_pad, ry + ascent_), QString::fromLatin1(numbuf));
        if (r.new_no > 0) snprintf(numbuf, sizeof(numbuf), "%*d", diff_digits, r.new_no);
        else snprintf(numbuf, sizeof(numbuf), "%*s", diff_digits, "");
        painter.drawText(QPointF(gutter_pad + char_w_ * (diff_digits + 0.5), ry + ascent_),
            QString::fromLatin1(numbuf));

        const char* sign = is_add ? "+" : (is_rem ? "-" : " ");
        painter.setPen(is_add ? t.success : is_rem ? t.error : t.text_dim);
        painter.drawText(QPointF(sign_x, ry + ascent_), QString::fromLatin1(sign));

        std::vector<syntax::token_t> toks;
        syntax::tokenize(r.text, doc->language(), toks);
        qreal tx = diff_text_x;
        const qreal dim = (is_rem ? 0.92 : 1.0) * (hunk_resolved ? 0.55 : 1.0);
        for (const auto& tk : toks) {
            const std::size_t token_start = static_cast<std::size_t>(tk.start);
            const std::size_t token_length = static_cast<std::size_t>(tk.length);
            if (token_start > r.text.size() || token_length > r.text.size() - token_start) continue;
            if (tk.type == syntax::token_type::whitespace) {
                for (std::size_t kk = 0; kk < token_length; ++kk) {
                    const char c = r.text[token_start + kk];
                    tx += (c == '\t') ? char_w_ * std::max(1, g_sa_settings.editor_tab_size) : char_w_;
                }
                continue;
            }
            QColor col = token_color(tk.type);
            col.setAlphaF(std::clamp(col.alphaF() * dim, 0.0, 1.0));
            painter.setPen(col);
            painter.drawText(QPointF(tx, ry + ascent_),
                QString::fromUtf8(r.text.data() + token_start, static_cast<qsizetype>(token_length)));
            if (is_rem) {
                painter.setPen(QPen(with_alpha(t.error, 0.5), 1));
                painter.drawLine(QPointF(tx, ry + row_h_ * 0.5 + 1.0),
                    QPointF(tx + static_cast<qreal>(token_length) * char_w_, ry + row_h_ * 0.5 + 1.0));
            }
            tx += static_cast<qreal>(token_length) * char_w_;
        }
    }
    painter.setClipping(false);
    painter.setPen(QPen(with_alpha(t.border_subtle, 1.0), 1));
    painter.drawLine(QPointF(diff_gutter_w, body_y0), QPointF(diff_gutter_w, body_y0 + body_h));
    doc->setDiffHoverHunk(new_hover_hunk);
}

bool AidaCodeEditor::handleDiffMousePress(const QPointF& pos, Qt::KeyboardModifiers modifiers)
{
    AidaCodeDocument* doc = document();
    if (!doc || !doc->hasPendingDiff())
        return false;
    static_cast<void>(modifiers);
    std::unique_lock<std::mutex> diff_lock(doc->diffMutex());
    const auto& diff = doc->pendingDiff();
    const qreal hdr_h = diffHeaderHeight();
    const qreal body_h = viewport()->height() - hdr_h;
    const qreal row_h = row_h_;

    const qreal hbw = char_w_ * 7.0 + theme::tokens().spacing.sm;
    const qreal hbh = row_h - theme::tokens().spacing.xs;

    const std::vector<diff_vis_row_t> rows = build_diff_rows(diff);
    const qreal scroll_y = static_cast<qreal>(verticalScrollBar()->value());
    const qreal code_w = viewport()->width();

    const qreal bw = k_diff_action_width;
    const qreal bh = theme::tokens().row.compact;
    const qreal by = (hdr_h - bh) * 0.5;
    const qreal bx_reject = code_w - theme::tokens().spacing.md - bw;
    const qreal bx_accept = bx_reject - theme::tokens().spacing.sm - bw;
    if (pos.y() >= by && pos.y() <= by + bh) {
        if (pos.x() >= bx_accept && pos.x() <= bx_accept + bw) {
            diff_lock.unlock();
            aida::ui::application_ui::execute_action("editor.ai.accept_all",
                aida::ui::action_invocation_source_t::context_menu);
            return true;
        }
        if (pos.x() >= bx_reject && pos.x() <= bx_reject + bw) {
            diff_lock.unlock();
            aida::ui::application_ui::execute_action("editor.ai.reject_all",
                aida::ui::action_invocation_source_t::context_menu);
            return true;
        }
    }

    for (std::size_t ri = 0; ri < rows.size(); ++ri) {
        const diff_vis_row_t& r = rows[ri];
        if (!r.is_hunk_head)
            continue;
        const qreal ry = hdr_h + static_cast<qreal>(ri) * row_h - scroll_y;
        if (ry + row_h < hdr_h || ry > hdr_h + body_h)
            continue;
        if (pos.y() < ry || pos.y() >= ry + row_h)
            continue;
        const qreal hbx_rej = code_w - theme::tokens().spacing.md - hbw;
        const qreal hbx_acc = hbx_rej - theme::tokens().spacing.xs -
            theme::tokens().spacing.xxs - hbw;
        const qreal hby = ry + theme::tokens().spacing.xxs;
        if (r.hunk >= 0 && static_cast<std::size_t>(r.hunk) < diff.hunks.size() &&
            diff.hunks[static_cast<std::size_t>(r.hunk)].state ==
                code_editor_widget::diff_hunk_state_t::pending) {
            if (pos.x() >= hbx_acc && pos.x() <= hbx_acc + hbw && pos.y() >= hby && pos.y() <= hby + hbh) {
                diff_lock.unlock();
                doc->acceptHunk(r.hunk);
                return true;
            }
            if (pos.x() >= hbx_rej && pos.x() <= hbx_rej + hbw && pos.y() >= hby && pos.y() <= hby + hbh) {
                diff_lock.unlock();
                doc->rejectHunk(r.hunk);
                return true;
            }
        }
        diff_lock.unlock();
        doc->selectReviewHunk(r.hunk, true);
        ensureDiffRowVisible(r.hunk);
        return true;
    }
    return false;
}

void AidaCodeEditor::ensureDiffRowVisible(int hunk_index)
{
    AidaCodeDocument* doc = document();
    if (!doc)
        return;
    std::lock_guard<std::mutex> diff_lock(doc->diffMutex());
    const auto& diff = doc->pendingDiff();
    const qreal hdr_h = diffHeaderHeight();
    const qreal body_h = viewport()->height() - hdr_h;
    std::size_t row_index = 0;
    for (std::size_t ri = 0; ri < diff.hunks.size(); ++ri) {
        if (static_cast<int>(ri) == hunk_index)
            break;
        row_index += 1 + diff.hunks[ri].lines.size() + 3;
    }
    const qreal row_offset = static_cast<qreal>(row_index) * row_h_;
    const qreal target = std::max(0.0, row_offset - body_h * 0.30);
    verticalScrollBar()->setValue(static_cast<int>(target));
}

}

#include "aida_code_editor.moc"

