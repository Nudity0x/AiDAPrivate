#include "qt/network/scripting/scripting_pane.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QItemSelectionModel>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedLayout>
#include <QSyntaxHighlighter>
#include <QTableView>
#include <QTextCharFormat>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/infra/executor.hpp"
#include "core/network/network_view.hpp"
#include "core/ui/task_center.hpp"
#include "core/ui/toast_notification.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/network/bounded_plain_text_edit.hpp"
#include "qt/network/shared/network_format.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::net {

namespace {

QColor log_level_color(script_engine::log_level level) {
    const auto& t = theme::tokens();
    switch (level) {
        case script_engine::log_level::error:   return t.error;
        case script_engine::log_level::warn:    return t.warning;
        case script_engine::log_level::output:  return t.success;
        case script_engine::log_level::command: return t.accent;
        case script_engine::log_level::debug:   return t.text_dim;
        case script_engine::log_level::info:
        default:                                return t.text_secondary;
    }
}

const char* log_level_tag(script_engine::log_level level) {
    switch (level) {
        case script_engine::log_level::error:   return "ERR ";
        case script_engine::log_level::warn:    return "WARN";
        case script_engine::log_level::output:  return "OUT ";
        case script_engine::log_level::command: return "CMD ";
        case script_engine::log_level::debug:   return "DBG ";
        case script_engine::log_level::info:
        default:                                return "INFO";
    }
}

void format_log_timestamp(std::uint64_t wall_seconds, char* out, std::size_t out_size) {
    if (out_size == 0) return;
    time_t t = static_cast<time_t>(wall_seconds);
    std::tm tm_buf{};
    bool ok = localtime_s(&tm_buf, &t) == 0;
    if (!ok) {
        snprintf(out, out_size, "--:--:--");
        return;
    }
    snprintf(out, out_size, "%02d:%02d:%02d",
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
}

bool read_script_file_exact(const std::string& path, std::string& contents, std::string& error) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Cannot open script file (Win32 " + std::to_string(GetLastError()) + ")";
        return false;
    }
    LARGE_INTEGER size{};
    bool success = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart < 32768;
    if (!success)
        error = size.QuadPart >= 32768 ? "Script is larger than the 32 KiB inline editor limit"
                                      : "Cannot determine script size";
    if (success) {
        contents.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(contents.size() - offset, static_cast<std::size_t>(32768)));
            DWORD read = 0;
            if (!ReadFile(file, contents.data() + offset, chunk, &read, nullptr) || read != chunk) {
                error = "Script read failed or was partial (Win32 " + std::to_string(GetLastError()) + ")";
                success = false;
                break;
            }
            offset += read;
        }
    }
    CloseHandle(file);
    if (success && contents.find('\0') != std::string::npos) {
        contents.clear();
        error = "Script contains embedded NUL bytes and cannot be opened in the text editor";
        success = false;
    }
    return success;
}

}

class ScriptLuaHighlighter : public QSyntaxHighlighter {
public:
    explicit ScriptLuaHighlighter(QTextDocument* parent) : QSyntaxHighlighter(parent) {
        keyword_format_.setForeground(theme::tokens().syn_keyword);
        comment_format_.setForeground(theme::tokens().syn_comment);
        string_format_.setForeground(theme::tokens().syn_string);
    }

protected:
    void highlightBlock(const QString& text) override {
        static const char* k_keywords[] = {
            "and", "break", "do", "else", "elseif", "end", "false", "for",
            "function", "if", "in", "local", "nil", "not", "or", "repeat",
            "return", "then", "true", "until", "while"
        };
        int index = 0;
        const int length = text.length();
        while (index < length) {
            const QChar c = text.at(index);
            if (c == QLatin1Char('-') && index + 1 < length &&
                text.at(index + 1) == QLatin1Char('-')) {
                setFormat(index, length - index, comment_format_);
                break;
            }
            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                const QChar quote = c;
                int end = index + 1;
                while (end < length) {
                    if (text.at(end) == QLatin1Char('\\')) {
                        end += 2;
                        continue;
                    }
                    if (text.at(end) == quote)
                        break;
                    ++end;
                }
                if (end >= length) end = length - 1;
                setFormat(index, end - index + 1, string_format_);
                index = end + 1;
                continue;
            }
            if (c.isLetter() || c == QLatin1Char('_')) {
                int end = index + 1;
                while (end < length &&
                       (text.at(end).isLetterOrNumber() || text.at(end) == QLatin1Char('_')))
                    ++end;
                const QString word = text.mid(index, end - index);
                for (const char* keyword : k_keywords) {
                    if (word == QLatin1String(keyword)) {
                        setFormat(index, end - index, keyword_format_);
                        break;
                    }
                }
                index = end;
                continue;
            }
            ++index;
        }
    }

private:
    QTextCharFormat keyword_format_;
    QTextCharFormat comment_format_;
    QTextCharFormat string_format_;
};

class ScriptLogHighlighter : public QSyntaxHighlighter {
public:
    explicit ScriptLogHighlighter(QTextDocument* parent) : QSyntaxHighlighter(parent) {}

protected:
    void highlightBlock(const QString& text) override {
        const auto& t = theme::tokens();
        if (text.length() < 14 || text.at(2) != QLatin1Char(':') ||
            text.at(5) != QLatin1Char(':') || text.at(8) != QLatin1Char(' '))
            return;
        const QString tag = text.mid(9, 4);
        QColor color = t.text_secondary;
        if (tag == QLatin1String("ERR ")) color = log_level_color(script_engine::log_level::error);
        else if (tag == QLatin1String("WARN")) color = log_level_color(script_engine::log_level::warn);
        else if (tag == QLatin1String("OUT ")) color = log_level_color(script_engine::log_level::output);
        else if (tag == QLatin1String("CMD ")) color = log_level_color(script_engine::log_level::command);
        else if (tag == QLatin1String("DBG ")) color = log_level_color(script_engine::log_level::debug);
        else if (tag == QLatin1String("INFO")) color = log_level_color(script_engine::log_level::info);
        else return;
        QTextCharFormat time_format;
        time_format.setForeground(t.text_dim);
        setFormat(0, 9, time_format);
        QTextCharFormat level_format;
        level_format.setForeground(color);
        setFormat(9, text.length() - 9, level_format);
        static const QRegularExpression badge_pattern(QStringLiteral("  x\\d+$"));
        const auto match = badge_pattern.match(text);
        if (match.hasMatch()) {
            QTextCharFormat badge_format;
            badge_format.setForeground(t.text_dim);
            setFormat(static_cast<int>(match.capturedStart()),
                      static_cast<int>(match.capturedLength()), badge_format);
        }
    }
};

ScriptLibraryModel::ScriptLibraryModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int ScriptLibraryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int ScriptLibraryModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ScriptLibraryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const script_row_t* row = rowAt(index.row());
    if (!row)
        return {};
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Name: return QString::fromStdString(row->name);
        case State: return !row->loaded ? QStringLiteral("UNLOADED")
            : (row->enabled ? QStringLiteral("ENABLED") : QStringLiteral("PAUSED"));
        case File: {
            if (row->path.empty())
                return QString();
            return QString::fromStdString(std::filesystem::path(row->path).filename().string());
        }
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        const auto& t = theme::tokens();
        switch (index.column()) {
        case State:
            return !row->loaded ? t.text_dim : (row->enabled ? t.success : t.warning);
        case File: return t.text_dim;
        case Name:
        default: return row->enabled ? t.text_primary : t.text_secondary;
        }
    }
    return {};
}

QVariant ScriptLibraryModel::headerData(int section, Qt::Orientation orientation,
                                        int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Name: return QStringLiteral("Name");
    case State: return QStringLiteral("State");
    case File: return QStringLiteral("File");
    default: return {};
    }
}

void ScriptLibraryModel::multiData(const QModelIndex& index,
                                   QModelRoleDataSpan roleDataSpan) const {
    for (auto& roleData : roleDataSpan)
        roleData.setData(data(index, roleData.role()));
}

const script_row_t* ScriptLibraryModel::rowAt(int row) const noexcept {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

int ScriptLibraryModel::indexOfName(const std::string& name) const {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].name == name)
            return static_cast<int>(i);
    }
    return -1;
}

void ScriptLibraryModel::upsertLoaded(const std::string& name, const std::string& path,
                                      int& rowOut) {
    const int existing = indexOfName(name);
    if (existing >= 0) {
        auto& row = rows_[static_cast<std::size_t>(existing)];
        row.path = path;
        row.loaded = true;
        row.enabled = true;
        rowOut = existing;
        Q_EMIT dataChanged(index(existing, 0), index(existing, ColumnCount - 1));
        return;
    }
    const int row = static_cast<int>(rows_.size());
    beginInsertRows(QModelIndex(), row, row);
    rows_.push_back(script_row_t{name, path, true, true});
    endInsertRows();
    rowOut = row;
}

bool ScriptLibraryModel::markUnloaded(const std::string& name) {
    const int existing = indexOfName(name);
    if (existing < 0)
        return false;
    rows_[static_cast<std::size_t>(existing)].loaded = false;
    rows_[static_cast<std::size_t>(existing)].enabled = false;
    Q_EMIT dataChanged(index(existing, 0), index(existing, ColumnCount - 1));
    return true;
}

bool ScriptLibraryModel::setEnabled(const std::string& name, bool enabled) {
    const int existing = indexOfName(name);
    if (existing < 0)
        return false;
    rows_[static_cast<std::size_t>(existing)].enabled = enabled;
    Q_EMIT dataChanged(index(existing, 0), index(existing, ColumnCount - 1));
    return true;
}

ScriptingPane::ScriptingPane(QWidget* parent)
    : NetworkPaneBase(parent) {
    setObjectName(QStringLiteral("aida.view.network.scripting"));

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    const auto& t = theme::tokens();
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.sm);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setOpaqueResize(true);
    splitter->setChildrenCollapsible(false);

    auto* libraryPanel = new QWidget(splitter);
    auto* libraryLayout = new QVBoxLayout(libraryPanel);
    libraryLayout->setContentsMargins(0, 0, 0, 0);
    libraryLayout->setSpacing(t.spacing.sm);
    auto* libraryHeader = new QHBoxLayout();
    libraryHeader->setSpacing(t.spacing.sm);
    auto* libraryTitle = new QLabel(QStringLiteral("Script Library"), libraryPanel);
    libraryTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    libraryHeader->addWidget(libraryTitle);
    libraryHeader->addStretch(1);
    op_status_ = new QLabel(QStringLiteral("Working..."), libraryPanel);
    op_status_->setObjectName(QStringLiteral("aida.view.network.scripting.op_status"));
    op_status_->setProperty("aidaTone", QStringLiteral("accent"));
    op_status_->setVisible(false);
    libraryHeader->addWidget(op_status_);
    library_meta_ = new QLabel(libraryPanel);
    library_meta_->setProperty("aidaTone", QStringLiteral("dim"));
    libraryHeader->addWidget(library_meta_);
    engine_dot_ = new QLabel(QStringLiteral("\xE2\x97\x8F"), libraryPanel);
    engine_dot_->setProperty("aidaTone", QStringLiteral("dim"));
    engine_dot_->setToolTip(QStringLiteral("Script engine status"));
    libraryHeader->addWidget(engine_dot_);
    libraryLayout->addLayout(libraryHeader);

    library_model_ = new ScriptLibraryModel(libraryPanel);
    auto* libraryTableHost = new QWidget(libraryPanel);
    library_stack_ = new QStackedLayout(libraryTableHost);
    library_stack_->setStackingMode(QStackedLayout::StackOne);
    library_stack_->setContentsMargins(0, 0, 0, 0);
    library_table_ = new QTableView(libraryTableHost);
    library_table_->setObjectName(QStringLiteral("aida.view.network.scripting.library"));
    library_table_->verticalHeader()->hide();
    library_table_->verticalHeader()->setDefaultSectionSize(t.table.row_h);
    library_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    library_table_->horizontalHeader()->setStretchLastSection(true);
    library_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    library_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    library_table_->setAlternatingRowColors(true);
    library_table_->setShowGrid(false);
    library_table_->setModel(library_model_);
    library_stack_->addWidget(library_table_);
    library_empty_ = new widgets::AidaStateView(widgets::AidaStateView::State::Empty,
        QStringLiteral("No scripts"),
        QStringLiteral("Load a Lua hook script to extend Network handling."),
        libraryTableHost);
    library_empty_->setObjectName(QStringLiteral("aida.view.network.scripting.empty"));
    library_empty_->setActionLabel(QStringLiteral("Load Script"));
    library_stack_->addWidget(library_empty_);
    libraryLayout->addWidget(libraryTableHost, 1);

    load_button_ = new widgets::AidaButton(QStringLiteral("Load Script"), libraryPanel);
    load_button_->setKind(widgets::AidaButton::Kind::Primary);
    load_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    libraryLayout->addWidget(load_button_);
    auto* footerRow = new QHBoxLayout();
    footerRow->setSpacing(t.spacing.sm);
    unload_button_ = new widgets::AidaButton(QStringLiteral("Unload"), libraryPanel);
    unload_button_->setKind(widgets::AidaButton::Kind::Secondary);
    unload_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    footerRow->addWidget(unload_button_);
    toggle_button_ = new widgets::AidaButton(QStringLiteral("Pause"), libraryPanel);
    toggle_button_->setKind(widgets::AidaButton::Kind::Secondary);
    toggle_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    footerRow->addWidget(toggle_button_);
    libraryLayout->addLayout(footerRow);
    open_button_ = new widgets::AidaButton(QStringLiteral("Open in Editor"), libraryPanel);
    open_button_->setKind(widgets::AidaButton::Kind::Ghost);
    open_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    libraryLayout->addWidget(open_button_);
    splitter->addWidget(libraryPanel);

    auto* rightPanel = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(t.spacing.xs);
    auto* rightSplitter = new QSplitter(Qt::Vertical, rightPanel);
    rightSplitter->setOpaqueResize(true);
    rightSplitter->setChildrenCollapsible(false);

    auto* editorPanel = new QWidget(rightSplitter);
    auto* editorLayout = new QVBoxLayout(editorPanel);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(t.spacing.xs);
    auto* editorHeader = new QHBoxLayout();
    editorHeader->setSpacing(t.spacing.sm);
    auto* editorTitle = new QLabel(QStringLiteral("Editor"), editorPanel);
    editorTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    editorHeader->addWidget(editorTitle);
    editorHeader->addStretch(1);
    editor_meta_ = new QLabel(editorPanel);
    editor_meta_->setProperty("aidaTone", QStringLiteral("dim"));
    editorHeader->addWidget(editor_meta_);
    editorLayout->addLayout(editorHeader);
    editor_ = new BoundedPlainTextEdit(32767, editorPanel);
    editor_->setFont(theme::fonts::codeRegular());
    editor_->setPlaceholderText(
        QStringLiteral("-- write a Lua hook, e.g. function on_request(req) ... end"));
    new ScriptLuaHighlighter(editor_->document());
    editorLayout->addWidget(editor_, 1);
    auto* editorButtons = new QHBoxLayout();
    editorButtons->setSpacing(t.spacing.sm);
    run_button_ = new widgets::AidaButton(QStringLiteral("Run Script"), editorPanel);
    run_button_->setKind(widgets::AidaButton::Kind::Primary);
    run_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    editorButtons->addWidget(run_button_);
    clear_editor_button_ = new widgets::AidaButton(QStringLiteral("Clear"), editorPanel);
    clear_editor_button_->setKind(widgets::AidaButton::Kind::Ghost);
    clear_editor_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    editorButtons->addWidget(clear_editor_button_);
    copy_button_ = new widgets::AidaButton(QStringLiteral("Copy"), editorPanel);
    copy_button_->setKind(widgets::AidaButton::Kind::Ghost);
    copy_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    editorButtons->addWidget(copy_button_);
    editorButtons->addStretch(1);
    editorLayout->addLayout(editorButtons);
    rightSplitter->addWidget(editorPanel);

    auto* consolePanel = new QWidget(rightSplitter);
    auto* consoleLayout = new QVBoxLayout(consolePanel);
    consoleLayout->setContentsMargins(0, 0, 0, 0);
    consoleLayout->setSpacing(t.spacing.xs);
    auto* consoleTitle = new QLabel(QStringLiteral("Console"), consolePanel);
    consoleTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    consoleLayout->addWidget(consoleTitle);
    auto* consoleRow = new QHBoxLayout();
    consoleRow->setSpacing(t.spacing.sm);
    auto* prompt = new QLabel(QStringLiteral(">"), consolePanel);
    prompt->setFont(theme::fonts::codeRegular());
    prompt->setProperty("aidaTone", QStringLiteral("accent"));
    consoleRow->addWidget(prompt);
    console_input_ = new QLineEdit(consolePanel);
    console_input_->setMaxLength(511);
    console_input_->setFont(theme::fonts::codeRegular());
    console_input_->setPlaceholderText(QStringLiteral("print(2 + 2)"));
    consoleRow->addWidget(console_input_, 1);
    exec_button_ = new widgets::AidaButton(QStringLiteral("Exec"), consolePanel);
    exec_button_->setKind(widgets::AidaButton::Kind::Secondary);
    exec_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    consoleRow->addWidget(exec_button_);
    consoleLayout->addLayout(consoleRow);
    rightSplitter->addWidget(consolePanel);

    auto* logPanel = new QWidget(rightSplitter);
    auto* logLayout = new QVBoxLayout(logPanel);
    logLayout->setContentsMargins(0, 0, 0, 0);
    logLayout->setSpacing(t.spacing.xs);
    auto* logHeader = new QHBoxLayout();
    logHeader->setSpacing(t.spacing.sm);
    auto* logTitle = new QLabel(QStringLiteral("Engine Log"), logPanel);
    logTitle->setProperty("aidaTone", QStringLiteral("titleAccent"));
    logHeader->addWidget(logTitle);
    logHeader->addStretch(1);
    log_status_ = new QLabel(logPanel);
    log_status_->setObjectName(QStringLiteral("aida.view.network.scripting.log_status"));
    log_status_->setProperty("aidaTone", QStringLiteral("error"));
    log_status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    log_status_->setVisible(false);
    logHeader->addWidget(log_status_);
    auto_scroll_ = new QCheckBox(QStringLiteral("Auto-scroll"), logPanel);
    auto_scroll_->setChecked(true);
    logHeader->addWidget(auto_scroll_);
    clear_log_button_ = new widgets::AidaButton(QStringLiteral("Clear"), logPanel);
    clear_log_button_->setKind(widgets::AidaButton::Kind::Ghost);
    clear_log_button_->setControlSize(widgets::AidaButton::ControlSize::Small);
    logHeader->addWidget(clear_log_button_);
    logLayout->addLayout(logHeader);
    log_view_ = new QPlainTextEdit(logPanel);
    log_view_->setObjectName(QStringLiteral("aida.view.network.scripting.log"));
    log_view_->setReadOnly(true);
    log_view_->setFont(theme::fonts::codeRegular());
    log_view_->setMaximumBlockCount(2048);
    log_view_->setPlaceholderText(QStringLiteral(
        "Run a script or console command to see engine log output"));
    log_highlighter_ = new ScriptLogHighlighter(log_view_->document());
    logLayout->addWidget(log_view_, 1);
    rightSplitter->addWidget(logPanel);

    rightSplitter->setStretchFactor(0, 52);
    rightSplitter->setStretchFactor(1, 12);
    rightSplitter->setStretchFactor(2, 36);
    rightLayout->addWidget(rightSplitter, 1);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 13);
    splitter->setStretchFactor(1, 37);
    layout->addWidget(splitter, 1);

    connect(load_button_, &QAbstractButton::clicked, this, [this] { loadScriptFromDialog(); });
    connect(library_empty_, &widgets::AidaStateView::actionTriggered, this, [this] {
        if (load_button_->isEnabled())
            load_button_->click();
    });
    connect(library_model_, &QAbstractItemModel::rowsInserted, this,
        [this] { updateEmptyState(); });
    connect(library_model_, &QAbstractItemModel::rowsRemoved, this,
        [this] { updateEmptyState(); });
    connect(library_model_, &QAbstractItemModel::modelReset, this,
        [this] { updateEmptyState(); });
    updateEmptyState();
    connect(unload_button_, &QAbstractButton::clicked, this, [this] {
        const QModelIndex current = library_table_->currentIndex();
        const auto* row = library_model_->rowAt(current.isValid() ? current.row() : -1);
        if (!row || !row->loaded)
            return;
        diag::log_tagged_fmt("network", "script_unload_clicked name='%s'", row->name.c_str());
        requestScriptUnload(row->name);
    });
    connect(toggle_button_, &QAbstractButton::clicked, this, [this] {
        const QModelIndex current = library_table_->currentIndex();
        const auto* row = library_model_->rowAt(current.isValid() ? current.row() : -1);
        if (!row)
            return;
        const bool enable = !row->enabled;
        diag::log_tagged_fmt("network", "script_toggle_enabled name='%s' enabled=%d",
            row->name.c_str(), enable ? 1 : 0);
        requestScriptToggle(row->name, enable);
    });
    connect(open_button_, &QAbstractButton::clicked, this, [this] {
        const QModelIndex current = library_table_->currentIndex();
        const auto* row = library_model_->rowAt(current.isValid() ? current.row() : -1);
        if (!row || row->path.empty())
            return;
        requestScriptOpen(row->path);
    });
    connect(library_table_->selectionModel(), &QItemSelectionModel::currentChanged,
        this, [this](const QModelIndex&, const QModelIndex&) { refreshButtons(); });
    connect(run_button_, &QAbstractButton::clicked, this, [this] {
        const std::string source = editor_->toPlainText().toStdString();
        if (source.empty())
            return;
        diag::log_tagged_fmt("network", "script_editor_run size=%zu", source.size());
        requestScriptEvaluate(std::move(source));
    });
    connect(clear_editor_button_, &QAbstractButton::clicked, this, [this] {
        if (!editor_->toPlainText().isEmpty())
            editor_->clear();
    });
    connect(copy_button_, &QAbstractButton::clicked, this, [this] {
        const QString text = editor_->toPlainText();
        if (!text.isEmpty())
            clipboard::set_text(text);
    });
    connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
        refreshEditorMeta();
        refreshButtons();
    });

    const auto consoleSubmit = [this] {
        const std::string command = console_input_->text().toStdString();
        if (command.empty())
            return;
        diag::log_tagged_fmt("network", "script_console_exec size=%zu", command.size());
        requestScriptConsole(command);
        console_input_->clear();
    };
    connect(console_input_, &QLineEdit::returnPressed, this, consoleSubmit);
    connect(exec_button_, &QAbstractButton::clicked, this, consoleSubmit);
    connect(clear_log_button_, &QAbstractButton::clicked, this, [this] {
        if (runtime_snapshot_ && !runtime_snapshot_->log.empty())
            requestScriptLogClear();
    });

    snapshot_timer_ = new QTimer(this);
    snapshot_timer_->setInterval(250);
    connect(snapshot_timer_, &QTimer::timeout, this, [this] { requestRuntimeSnapshot(); });

    refreshLibraryMeta();
    refreshEditorMeta();
    refreshButtons();
    setContent(content);
}

ScriptingPane::~ScriptingPane() = default;

std::string ScriptingPane::registerNetworkOperation(const char* action, const char* label,
                                                    std::string target) {
    const std::string id = "network.operation.scripting." +
        std::to_string(++operation_sequence_);
    aida::ui::task_center::task_registration_t registration;
    registration.id = id;
    registration.source = "human";
    registration.owner = "network";
    registration.owner_view = "view.network.scripting";
    registration.owner_action = action ? action : "network.operation";
    registration.target = std::move(target);
    registration.label = label ? label : "Network operation";
    registration.stage = "Queued";
    registration.progress = -1.0f;
    registration.cancellation_is_safe = false;
    registration.callbacks.focus = [] {
        (void)network_view::open_view("view.network.scripting");
    };
    if (!aida::ui::task_center::register_task(std::move(registration)))
        return {};
    return id;
}

void ScriptingPane::finishNetworkOperation(const std::string& id, bool success,
                                           std::string stage, std::string summary) {
    if (id.empty())
        return;
    (void)aida::ui::task_center::update_task(
        id,
        success ? aida::ui::task_center::task_state_t::completed
                : aida::ui::task_center::task_state_t::failed,
        1.0f, std::move(stage), std::move(summary));
}

void ScriptingPane::onPaneShown() {
    requestRuntimeSnapshot(true);
    snapshot_timer_->start();
}

void ScriptingPane::onPaneHidden() {
    snapshot_timer_->stop();
}

void ScriptingPane::requestRuntimeSnapshot(bool force) {
    const std::uint64_t now = network_now_ms();
    const std::uint64_t last = snapshot_requested_ms_.load(std::memory_order_acquire);
    if (!force && last != 0 && now >= last && now - last < 250)
        return;
    bool expected = false;
    if (!snapshot_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    snapshot_requested_ms_.store(now, std::memory_order_release);
    QPointer<ScriptingPane> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "script_snapshot";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [pane]() {
        auto snapshot = std::make_shared<runtime_snapshot_t>();
        try {
            snapshot->initialized = script_engine::is_initialized();
            snapshot->hook_count = script_engine::registered_hook_count();
            snapshot->log = script_engine::get_log(2048);
        } catch (...) {
            snapshot.reset();
        }
        if (pane) {
            QMetaObject::invokeMethod(pane.data(),
                [pane, snapshot = std::move(snapshot)]() mutable {
                    pane->applySnapshot(std::move(snapshot));
                }, Qt::QueuedConnection);
        } else {
            snapshot.reset();
        }
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted)
        snapshot_pending_.store(false, std::memory_order_release);
}

void ScriptingPane::applySnapshot(
    const std::shared_ptr<const runtime_snapshot_t>& snapshot) {
    snapshot_pending_.store(false, std::memory_order_release);
    if (!snapshot)
        return;
    runtime_snapshot_ = snapshot;
    refreshLibraryMeta();
    rebuildLogText(snapshot);
    refreshButtons();
}

void ScriptingPane::rebuildLogText(
    const std::shared_ptr<const runtime_snapshot_t>& snapshot) {
    const auto& entries = snapshot->log;
    const std::size_t size = entries.size();
    const std::uint64_t last_wall = entries.empty() ? 0 : entries.back().wall_seconds;
    const std::uint32_t last_repeat = entries.empty() ? 1 : entries.back().repeat_count;
    if (size == applied_log_size_ && last_wall == applied_log_last_wall_seconds_ &&
        last_repeat == applied_log_last_repeat_)
        return;
    applied_log_size_ = size;
    applied_log_last_wall_seconds_ = last_wall;
    applied_log_last_repeat_ = last_repeat;

    QString text;
    bool sawError = false;
    QString lastError;
    for (const auto& entry : entries) {
        char ts[16];
        format_log_timestamp(entry.wall_seconds, ts, sizeof(ts));
        QString line = QString::fromLatin1(ts);
        line += QLatin1Char(' ');
        line += QLatin1String(log_level_tag(entry.level));
        line += QLatin1Char(' ');
        if (!entry.script_name.empty() && entry.script_name != "console" &&
            entry.script_name != "engine") {
            line += QLatin1Char('[');
            line += QString::fromStdString(entry.script_name);
            line += QStringLiteral("] ");
        }
        line += QString::fromStdString(entry.message);
        if (entry.repeat_count > 1)
            line += QStringLiteral("  x%1").arg(static_cast<unsigned>(entry.repeat_count));
        if (entry.level == script_engine::log_level::error) {
            sawError = true;
            lastError = QString::fromStdString(entry.message);
        }
        text += line;
        text += QLatin1Char('\n');
    }
    log_view_->setPlainText(text);
    if (!sawError) {
        log_status_->setVisible(false);
    } else {
        QString shown = lastError;
        if (shown.size() > 160)
            shown = shown.left(160) + QStringLiteral("...");
        log_status_->setText(QStringLiteral("Latest error: %1").arg(shown));
        log_status_->setToolTip(lastError);
        log_status_->setVisible(true);
    }
    if (auto_scroll_->isChecked())
        log_view_->verticalScrollBar()->setValue(log_view_->verticalScrollBar()->maximum());
}

void ScriptingPane::updateEmptyState() {
    if (!library_stack_ || !library_empty_ || !library_table_ || !library_model_)
        return;
    library_stack_->setCurrentWidget(library_model_->rowCount() == 0
        ? static_cast<QWidget*>(library_empty_) : static_cast<QWidget*>(library_table_));
}

void ScriptingPane::refreshLibraryMeta() {
    const std::size_t hooks = runtime_snapshot_ ? runtime_snapshot_->hook_count : 0;
    const bool up = runtime_snapshot_ && runtime_snapshot_->initialized;
    library_meta_->setText(QStringLiteral("%1 hooks").arg(hooks));
    set_label_tone(engine_dot_, up ? "success" : "error");
    engine_dot_->setToolTip(up ? QStringLiteral("Script engine initialized")
                               : QStringLiteral("Script engine unavailable"));
}

void ScriptingPane::refreshEditorMeta() {
    const QString text = editor_->toPlainText();
    const qsizetype chars = text.toUtf8().size();
    const int lines = static_cast<int>(text.count(QLatin1Char('\n'))) + 1;
    editor_meta_->setText(QStringLiteral("%1 lines  -  %2 chars").arg(lines).arg(chars));
}

void ScriptingPane::refreshButtons() {
    const bool opPending = operation_pending_.load(std::memory_order_acquire);
    const bool openPending = open_pending_.load(std::memory_order_acquire);
    const QModelIndex current = library_table_->currentIndex();
    const auto* row = library_model_->rowAt(current.isValid() ? current.row() : -1);
    const bool hasSelection = row != nullptr;
    load_button_->setEnabled(!opPending);
    unload_button_->setEnabled(hasSelection && row->loaded && !opPending);
    unload_button_->setText(hasSelection && row->loaded ? QStringLiteral("Unload")
                                                        : QStringLiteral("Unloaded"));
    toggle_button_->setEnabled(hasSelection && !opPending);
    toggle_button_->setText(hasSelection && row->enabled ? QStringLiteral("Pause")
                                                         : QStringLiteral("Enable"));
    open_button_->setEnabled(hasSelection && !row->path.empty() && !openPending);
    run_button_->setEnabled(!editor_->toPlainText().isEmpty() && !opPending);
    clear_editor_button_->setEnabled(!editor_->toPlainText().isEmpty());
    copy_button_->setEnabled(!editor_->toPlainText().isEmpty());
    exec_button_->setEnabled(!opPending);
    clear_log_button_->setEnabled(runtime_snapshot_ && !runtime_snapshot_->log.empty() &&
        !opPending);
    op_status_->setVisible(opPending || openPending);
}

void ScriptingPane::loadScriptFromDialog() {
    static const char k_lua_open_filter[] =
        "Lua Scripts (*.lua)\0*.lua\0"
        "All files (*.*)\0*.*\0\0";
    const auto picked = dialogs::open_file(this, QStringLiteral("Load Lua Script"),
        k_lua_open_filter);
    if (!picked)
        return;
    diag::log_tagged_fmt("network", "script_load_dialog_pick path='%s'", picked->c_str());
    requestScriptLoad(*picked);
}

void ScriptingPane::requestScriptLoad(std::string path) {
    bool expected = false;
    if (!operation_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = operation_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string task_id = registerNetworkOperation(
        "network.scripts.load", "Load network automation script", path);
    QPointer<ScriptingPane> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "script_load";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [pane, serial, path = std::move(path), task_id]() {
        bool success = false;
        std::string error;
        std::string name;
        try {
            success = script_engine::load_script(path);
            name = std::filesystem::path(path).stem().string();
            if (!success)
                error = "Script engine rejected the selected file";
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Script load failed";
        }
        finishNetworkOperation(task_id, success, success ? "Completed" : "Failed",
            success ? "Loaded " + name : error);
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(),
            [pane, serial, success, path, name, error = std::move(error)]() {
                if (pane->operation_serial_.load(std::memory_order_acquire) != serial)
                    return;
                if (success) {
                    int row = -1;
                    pane->library_model_->upsertLoaded(name, path, row);
                    if (row >= 0)
                        pane->library_table_->setCurrentIndex(pane->library_model_->index(row, 0));
                    toast_notification::push("Loaded script: " + name,
                        toast_notification::toast_type_t::info);
                } else {
                    toast_notification::push(error.empty() ? "Script load failed" : error,
                        toast_notification::toast_type_t::error);
                }
                pane->operation_pending_.store(false, std::memory_order_release);
                pane->refreshButtons();
                pane->requestRuntimeSnapshot(true);
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        operation_pending_.store(false, std::memory_order_release);
        finishNetworkOperation(task_id, false, "Rejected", "Executor rejected script load");
        refreshButtons();
    } else {
        refreshButtons();
    }
}

void ScriptingPane::requestScriptUnload(std::string name) {
    bool expected = false;
    if (!operation_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = operation_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string task_id = registerNetworkOperation(
        "network.scripts.unload", "Unload network automation script", name);
    QPointer<ScriptingPane> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "script_unload";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [pane, serial, name = std::move(name), task_id]() {
        bool success = false;
        std::string error;
        try {
            success = script_engine::unload_script(name);
            if (!success)
                error = "Script engine did not unload " + name;
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Script unload failed";
        }
        finishNetworkOperation(task_id, success, success ? "Completed" : "Failed",
            success ? "Unloaded " + name : error);
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(),
            [pane, serial, name, success, error = std::move(error)]() {
                if (pane->operation_serial_.load(std::memory_order_acquire) != serial)
                    return;
                if (success)
                    static_cast<void>(pane->library_model_->markUnloaded(name));
                else
                    toast_notification::push(error.empty() ? "Script unload failed" : error,
                        toast_notification::toast_type_t::error);
                pane->operation_pending_.store(false, std::memory_order_release);
                pane->refreshButtons();
                pane->requestRuntimeSnapshot(true);
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        operation_pending_.store(false, std::memory_order_release);
        finishNetworkOperation(task_id, false, "Rejected", "Executor rejected script unload");
        refreshButtons();
    } else {
        refreshButtons();
    }
}

void ScriptingPane::requestScriptToggle(std::string name, bool enable) {
    bool expected = false;
    if (!operation_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = operation_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string task_id = registerNetworkOperation(
        enable ? "network.scripts.enable" : "network.scripts.pause",
        enable ? "Enable network automation script" : "Pause network automation script", name);
    QPointer<ScriptingPane> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = enable ? "script_enable" : "script_pause";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [pane, serial, name = std::move(name), enable, task_id]() {
        bool success = true;
        std::string error;
        try {
            script_engine::set_script_enabled(name, enable);
        } catch (const std::exception& exception) {
            success = false;
            error = exception.what();
        } catch (...) {
            success = false;
            error = "Script state change failed";
        }
        finishNetworkOperation(task_id, success, success ? "Completed" : "Failed",
            success ? name + (enable ? " enabled" : " paused") : error);
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(),
            [pane, serial, name, enable, success, error = std::move(error)]() {
                if (pane->operation_serial_.load(std::memory_order_acquire) != serial)
                    return;
                if (success)
                    static_cast<void>(pane->library_model_->setEnabled(name, enable));
                else
                    toast_notification::push(error.empty() ? "Script state change failed" : error,
                        toast_notification::toast_type_t::error);
                pane->operation_pending_.store(false, std::memory_order_release);
                pane->refreshButtons();
                pane->requestRuntimeSnapshot(true);
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        operation_pending_.store(false, std::memory_order_release);
        finishNetworkOperation(task_id, false, "Rejected", "Executor rejected script state change");
        refreshButtons();
    } else {
        refreshButtons();
    }
}

void ScriptingPane::requestScriptEvaluate(std::string source) {
    bool expected = false;
    if (!operation_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = operation_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::size_t source_size = source.size();
    const std::string task_id = registerNetworkOperation(
        "network.scripts.evaluate", "Evaluate network automation script",
        std::to_string(source_size) + " bytes");
    QPointer<ScriptingPane> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "script_editor_run";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [pane, serial, source = std::move(source), source_size, task_id]() {
        bool success = false;
        std::string error;
        try {
            success = script_engine::load_script_source("_editor_", source);
            if (!success)
                error = "Script engine rejected the editor source";
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Script evaluation failed";
        }
        finishNetworkOperation(task_id, success, success ? "Completed" : "Failed",
            success ? std::to_string(source_size) + " bytes evaluated" : error);
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(),
            [pane, serial, success, error = std::move(error)]() {
                if (pane->operation_serial_.load(std::memory_order_acquire) != serial)
                    return;
                if (!success)
                    toast_notification::push(error.empty() ? "Script evaluation failed" : error,
                        toast_notification::toast_type_t::error);
                pane->operation_pending_.store(false, std::memory_order_release);
                pane->refreshButtons();
                pane->requestRuntimeSnapshot(true);
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        operation_pending_.store(false, std::memory_order_release);
        finishNetworkOperation(task_id, false, "Rejected", "Executor rejected script evaluation");
        refreshButtons();
    } else {
        refreshButtons();
    }
}

void ScriptingPane::requestScriptConsole(std::string command) {
    bool expected = false;
    if (!operation_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = operation_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string task_id = registerNetworkOperation(
        "network.scripts.console", "Execute network script console command",
        std::to_string(command.size()) + " bytes");
    QPointer<ScriptingPane> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "script_console_exec";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [pane, serial, command = std::move(command), task_id]() {
        bool success = true;
        std::string output;
        std::string error;
        try {
            output = script_engine::execute(command);
        } catch (const std::exception& exception) {
            success = false;
            error = exception.what();
        } catch (...) {
            success = false;
            error = "Script console execution failed";
        }
        finishNetworkOperation(task_id, success, success ? "Completed" : "Failed",
            success ? std::to_string(output.size()) + " output bytes" : error);
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(),
            [pane, serial, success, error = std::move(error)]() {
                if (pane->operation_serial_.load(std::memory_order_acquire) != serial)
                    return;
                if (!success)
                    toast_notification::push(error.empty() ? "Script console execution failed" : error,
                        toast_notification::toast_type_t::error);
                pane->operation_pending_.store(false, std::memory_order_release);
                pane->refreshButtons();
                pane->requestRuntimeSnapshot(true);
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        operation_pending_.store(false, std::memory_order_release);
        finishNetworkOperation(task_id, false, "Rejected", "Executor rejected script console command");
        refreshButtons();
    } else {
        refreshButtons();
    }
}

void ScriptingPane::requestScriptLogClear() {
    bool expected = false;
    if (!operation_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::uint64_t serial = operation_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::string task_id = registerNetworkOperation(
        "network.scripts.log.clear", "Clear network script log", "engine log");
    QPointer<ScriptingPane> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "script_log_clear";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [pane, serial, task_id]() {
        bool success = true;
        std::string error;
        try {
            script_engine::clear_log();
        } catch (const std::exception& exception) {
            success = false;
            error = exception.what();
        } catch (...) {
            success = false;
            error = "Script log clear failed";
        }
        finishNetworkOperation(task_id, success, success ? "Completed" : "Failed",
            success ? "Script log cleared" : error);
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(),
            [pane, serial, success, error = std::move(error)]() {
                if (pane->operation_serial_.load(std::memory_order_acquire) != serial)
                    return;
                if (!success)
                    toast_notification::push(error.empty() ? "Script log clear failed" : error,
                        toast_notification::toast_type_t::error);
                pane->operation_pending_.store(false, std::memory_order_release);
                pane->refreshButtons();
                pane->requestRuntimeSnapshot(true);
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        operation_pending_.store(false, std::memory_order_release);
        finishNetworkOperation(task_id, false, "Rejected", "Executor rejected script log clear");
        refreshButtons();
    } else {
        refreshButtons();
    }
}

void ScriptingPane::requestScriptOpen(std::string path) {
    bool expected = false;
    if (!open_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    const std::string task_id = registerNetworkOperation(
        "network.scripts.open", "Open network script in editor", path);
    QPointer<ScriptingPane> pane(this);
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "network.view";
    submission.label = "script_open";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::diagnostics;
    submission.priority = 3;
    submission.body = [pane, path = std::move(path), task_id]() {
        std::string contents;
        std::string error;
        const bool success = read_script_file_exact(path, contents, error);
        finishNetworkOperation(task_id, success, success ? "Completed" : "Failed",
            success ? std::to_string(contents.size()) + " bytes loaded" : error);
        if (!pane)
            return;
        QMetaObject::invokeMethod(pane.data(),
            [pane, success, contents = std::move(contents), error = std::move(error)]() {
                if (success)
                    pane->editor_->setPlainText(QString::fromStdString(contents));
                else
                    toast_notification::push(error.empty() ? "Script open failed" : error,
                        toast_notification::toast_type_t::error);
                pane->open_pending_.store(false, std::memory_order_release);
                pane->refreshButtons();
            }, Qt::QueuedConnection);
    };
    if (!aida::infra::executor::submit(std::move(submission)).submitted) {
        open_pending_.store(false, std::memory_order_release);
        finishNetworkOperation(task_id, false, "Rejected", "Executor rejected script open");
        refreshButtons();
    } else {
        refreshButtons();
    }
}

}
