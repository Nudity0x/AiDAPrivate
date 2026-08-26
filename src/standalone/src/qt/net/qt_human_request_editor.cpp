#include "qt/net/qt_human_request_editor.hpp"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedLayout>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_search_field.hpp"

namespace aida::qt::net {

namespace {

constexpr qsizetype kMaxMatchCount = 10000;
constexpr qsizetype kOversizedPreviewChars = 4095;

QString truncateToByteCap(const QString& text, qsizetype maxBytes)
{
    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() <= maxBytes)
        return text;
    qsizetype cut = 0;
    qsizetype i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8.at(i));
        const qsizetype len = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        if (i + len > maxBytes)
            break;
        i += len;
        cut = i;
    }
    return QString::fromUtf8(utf8.constData(), cut);
}

}

QtByteCappedPlainTextEdit::QtByteCappedPlainTextEdit(QWidget* parent)
    : QPlainTextEdit(parent)
{
    connect(document(), &QTextDocument::contentsChanged, this,
        &QtByteCappedPlainTextEdit::enforceCap);
}

void QtByteCappedPlainTextEdit::setMaxBytes(qsizetype maxBytes)
{
    max_bytes_ = (std::max)(maxBytes, static_cast<qsizetype>(0));
    enforceCap();
}

void QtByteCappedPlainTextEdit::keyPressEvent(QKeyEvent* event)
{
    const QString text = event->text();
    if (!text.isEmpty() && !isReadOnly()) {
        const QTextCursor cursor = textCursor();
        const qsizetype selectionLength = cursor.selectedText().size();
        const qsizetype projected = document()->characterCount() - selectionLength +
            text.size();
        if (projected > max_bytes_) {
            Q_EMIT overCapacity();
            return;
        }
    }
    QPlainTextEdit::keyPressEvent(event);
}

void QtByteCappedPlainTextEdit::insertFromMimeData(const QMimeData* source)
{
    if (source == nullptr || !source->hasText() || isReadOnly()) {
        QPlainTextEdit::insertFromMimeData(source);
        return;
    }
    const QByteArray current = toPlainText().toUtf8();
    const QTextCursor cursor = textCursor();
    const qsizetype selectionBytes = cursor.selectedText().toUtf8().size();
    const qsizetype remaining = max_bytes_ - (current.size() - selectionBytes);
    QString incoming = source->text();
    if (incoming.toUtf8().size() > remaining) {
        incoming = truncateToByteCap(incoming, (std::max)(remaining, static_cast<qsizetype>(0)));
        QMimeData truncated;
        truncated.setText(incoming);
        QPlainTextEdit::insertFromMimeData(&truncated);
        Q_EMIT overCapacity();
        return;
    }
    QPlainTextEdit::insertFromMimeData(source);
}

void QtByteCappedPlainTextEdit::enforceCap()
{
    if (enforcing_)
        return;
    const QString text = toPlainText();
    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() <= max_bytes_)
        return;
    enforcing_ = true;
    const int anchor = textCursor().anchor();
    const QString truncated = truncateToByteCap(text, max_bytes_);
    setPlainText(truncated);
    QTextCursor cursor = textCursor();
    cursor.setPosition((std::min)(anchor, static_cast<int>(truncated.size())));
    setTextCursor(cursor);
    enforcing_ = false;
    Q_EMIT overCapacity();
}

HttpRequestHighlighter::HttpRequestHighlighter(Grammar grammar, QTextDocument* parent)
    : QSyntaxHighlighter(parent), grammar_(grammar)
{
    const auto& t = theme::tokens();
    colors_.keyword = t.syn_keyword;
    colors_.string = t.syn_string;
    colors_.type = t.syn_type;
    colors_.error = t.error;
    colors_.text = t.text_primary;
}

void HttpRequestHighlighter::setColors(const Colors& colors)
{
    colors_ = colors;
    rehighlight();
}

void HttpRequestHighlighter::highlightBlock(const QString& text)
{
    int state = previousBlockState();
    if (state == -1)
        state = grammar_ == Grammar::FullRequest ? 0 : 1;

    QTextCharFormat keywordFormat;
    keywordFormat.setForeground(colors_.keyword);
    QTextCharFormat stringFormat;
    stringFormat.setForeground(colors_.string);
    QTextCharFormat typeFormat;
    typeFormat.setForeground(colors_.type);
    QTextCharFormat errorFormat;
    errorFormat.setForeground(colors_.error);

    if (state == 0) {
        const std::string line = text.toStdString();
        if (!http_text::validate_request_line(line)) {
            setFormat(0, static_cast<int>(text.size()), errorFormat);
        } else {
            const int first = static_cast<int>(text.indexOf(QLatin1Char(' ')));
            const int second = static_cast<int>(text.indexOf(QLatin1Char(' '), first + 1));
            setFormat(0, first, keywordFormat);
            setFormat(first + 1, second - first - 1, stringFormat);
            setFormat(second + 1, static_cast<int>(text.size()) - second - 1, keywordFormat);
        }
        setCurrentBlockState(1);
        return;
    }
    if (state == 1) {
        QString line = text;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.isEmpty()) {
            setCurrentBlockState(2);
            return;
        }
        const int colon = static_cast<int>(line.indexOf(QLatin1Char(':')));
        const std::string name = colon < 0 ? std::string()
            : line.left(colon).toStdString();
        if (colon < 0 || !http_text::valid_header_name(name)) {
            setFormat(0, static_cast<int>(text.size()), errorFormat);
        } else {
            setFormat(0, colon, typeFormat);
        }
        setCurrentBlockState(1);
        return;
    }
    setCurrentBlockState(2);
}

QtHumanRequestEditor::QtHumanRequestEditor(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.network.request-editor"));
    buildUi();
    loadBytes(QByteArray());
    refreshChrome();
}

void QtHumanRequestEditor::buildUi()
{
    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(t.spacing.xs);

    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(t.spacing.xs);
    raw_button_ = new QPushButton(QStringLiteral("Raw"), this);
    raw_button_->setObjectName(QStringLiteral("aida.network.request-editor.mode.raw"));
    raw_button_->setCheckable(true);
    pretty_button_ = new QPushButton(QStringLiteral("Pretty"), this);
    pretty_button_->setObjectName(QStringLiteral("aida.network.request-editor.mode.pretty"));
    pretty_button_->setCheckable(true);
    mode_group_ = new QButtonGroup(this);
    mode_group_->setExclusive(true);
    mode_group_->addButton(raw_button_);
    mode_group_->addButton(pretty_button_);
    raw_button_->setChecked(true);
    toolbar->addWidget(raw_button_);
    toolbar->addWidget(pretty_button_);

    search_edit_ = new QLineEdit(this);
    search_edit_->setObjectName(QStringLiteral("aida.network.request-editor.find.query"));
    search_edit_->setPlaceholderText(QStringLiteral("Find in Raw"));
    search_edit_->setClearButtonEnabled(true);
    search_edit_->setMaximumWidth(field_width_chars(search_edit_, 24));
    toolbar->addWidget(search_edit_);
    prev_button_ = new QToolButton(this);
    prev_button_->setObjectName(QStringLiteral("aida.network.request-editor.find.previous"));
    prev_button_->setText(QStringLiteral("Prev"));
    toolbar->addWidget(prev_button_);
    next_button_ = new QToolButton(this);
    next_button_->setObjectName(QStringLiteral("aida.network.request-editor.find.next"));
    next_button_->setText(QStringLiteral("Next"));
    toolbar->addWidget(next_button_);
    match_label_ = new QLabel(this);
    match_label_->setProperty("aidaTone", QStringLiteral("dim"));
    toolbar->addWidget(match_label_);
    toolbar->addStretch(1);
    dirty_label_ = new QLabel(this);
    dirty_label_->setProperty("aidaTone", QStringLiteral("dim"));
    toolbar->addWidget(dirty_label_);
    root->addLayout(toolbar);

    auto* contentHost = new QWidget(this);
    stack_ = new QStackedLayout(contentHost);
    stack_->setStackingMode(QStackedLayout::StackOne);

    raw_edit_ = new QtByteCappedPlainTextEdit(contentHost);
    raw_edit_->setObjectName(QStringLiteral("aida.network.request-editor.raw"));
    raw_edit_->setFont(theme::fonts::codeRegular());
    raw_highlighter_ = new HttpRequestHighlighter(
        HttpRequestHighlighter::Grammar::FullRequest, raw_edit_->document());
    stack_->addWidget(raw_edit_);

    auto* prettyPage = new QWidget(contentHost);
    auto* prettyLayout = new QVBoxLayout(prettyPage);
    prettyLayout->setContentsMargins(0, 0, 0, 0);
    prettyLayout->setSpacing(t.spacing.xs);
    request_line_edit_ = new QLineEdit(prettyPage);
    request_line_edit_->setObjectName(
        QStringLiteral("aida.network.request-editor.pretty.request-line"));
    request_line_edit_->setFont(theme::fonts::codeRegular());
    prettyLayout->addWidget(request_line_edit_);
    pretty_splitter_ = new QSplitter(Qt::Vertical, prettyPage);
    pretty_splitter_->setOpaqueResize(true);
    pretty_splitter_->setChildrenCollapsible(false);
    headers_edit_ = new QtByteCappedPlainTextEdit(pretty_splitter_);
    headers_edit_->setObjectName(
        QStringLiteral("aida.network.request-editor.pretty.headers"));
    headers_edit_->setFont(theme::fonts::codeRegular());
    headers_highlighter_ = new HttpRequestHighlighter(
        HttpRequestHighlighter::Grammar::HeadersOnly, headers_edit_->document());
    pretty_splitter_->addWidget(headers_edit_);
    body_edit_ = new QtByteCappedPlainTextEdit(pretty_splitter_);
    body_edit_->setObjectName(QStringLiteral("aida.network.request-editor.pretty.body"));
    body_edit_->setFont(theme::fonts::codeRegular());
    pretty_splitter_->addWidget(body_edit_);
    pretty_splitter_->setSizes({ 120, 200 });
    prettyLayout->addWidget(pretty_splitter_, 1);

    pretty_footer_ = new QWidget(prettyPage);
    auto* footerLayout = new QHBoxLayout(pretty_footer_);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setSpacing(t.spacing.xs);
    apply_button_ = new QPushButton(QStringLiteral("Apply to Raw"), pretty_footer_);
    apply_button_->setObjectName(
        QStringLiteral("aida.network.request-editor.pretty.apply"));
    footerLayout->addWidget(apply_button_);
    discard_button_ = new QPushButton(QStringLiteral("Discard Pretty Edits"), pretty_footer_);
    discard_button_->setObjectName(
        QStringLiteral("aida.network.request-editor.pretty.discard"));
    footerLayout->addWidget(discard_button_);
    pretty_error_label_ = new QLabel(pretty_footer_);
    pretty_error_label_->setProperty("aidaTone", QStringLiteral("error"));
    pretty_error_label_->setWordWrap(true);
    footerLayout->addWidget(pretty_error_label_, 1);
    prettyLayout->addWidget(pretty_footer_);
    stack_->addWidget(prettyPage);
    root->addWidget(contentHost, 1);

    notice_label_ = new QLabel(this);
    notice_label_->setWordWrap(true);
    notice_label_->setProperty("aidaTone", QStringLiteral("warning"));
    notice_label_->setVisible(false);
    root->addWidget(notice_label_);

    connect(raw_button_, &QPushButton::clicked, this, [this] { setMode(Mode::Raw); });
    connect(pretty_button_, &QPushButton::clicked, this, [this] { setMode(Mode::Pretty); });
    connect(search_edit_, &QLineEdit::textChanged, this, [this](const QString& query) {
        search_query_ = query;
        recomputeMatches();
        if (!matches_.empty())
            selectActiveMatch();
        Q_EMIT searchChanged();
    });
    connect(prev_button_, &QToolButton::clicked, this, [this] { stepMatch(-1); });
    connect(next_button_, &QToolButton::clicked, this, [this] { stepMatch(1); });
    connect(raw_edit_, &QPlainTextEdit::textChanged, this,
        &QtHumanRequestEditor::onRawTextChanged);
    connect(request_line_edit_, &QLineEdit::textChanged, this, [this](const QString&) {
        onPrettyChanged(false);
    });
    connect(headers_edit_, &QPlainTextEdit::textChanged, this, [this] {
        onPrettyChanged(false);
    });
    connect(body_edit_, &QPlainTextEdit::textChanged, this, [this] {
        onPrettyChanged(true);
    });
    connect(apply_button_, &QPushButton::clicked, this,
        &QtHumanRequestEditor::applyPretty);
    connect(discard_button_, &QPushButton::clicked, this,
        &QtHumanRequestEditor::discardPretty);
    const auto bubbleOverCapacity = [this] { Q_EMIT overCapacity(); };
    connect(raw_edit_, &QtByteCappedPlainTextEdit::overCapacity, this, bubbleOverCapacity);
    connect(headers_edit_, &QtByteCappedPlainTextEdit::overCapacity, this, bubbleOverCapacity);
    connect(body_edit_, &QtByteCappedPlainTextEdit::overCapacity, this, bubbleOverCapacity);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QSize QtHumanRequestEditor::sizeHint() const
{
    return QSize(320, 200);
}

void QtHumanRequestEditor::setConfig(const Config& cfg)
{
    const bool capacityChanged = cfg.maxBytes != config_.maxBytes;
    config_ = cfg;
    config_.maxBytes = (std::max)(config_.maxBytes, static_cast<qsizetype>(1024));
    raw_edit_->setMaxBytes(config_.maxBytes);
    headers_edit_->setMaxBytes(config_.maxBytes);
    body_edit_->setMaxBytes(config_.maxBytes);
    setObjectName(QStringLiteral("aida.network.request-editor.") + config_.stableId);
    if (capacityChanged)
        loadBytes(pushed_bytes_);
    apply_button_->setEnabled(editableEffective());
    refreshChrome();
    refreshValidity();
}

void QtHumanRequestEditor::setAuthority(const QString& identity, const QString& authorityUtf8)
{
    setAuthority(identity, authorityUtf8.toUtf8());
}

void QtHumanRequestEditor::setAuthority(const QString& identity, const QByteArray& authorityBytes)
{
    const qsizetype maxBytes = (std::max)(config_.maxBytes, static_cast<qsizetype>(1024));
    if (identity == identity_ && authorityBytes == pushed_bytes_ &&
        maxBytes == raw_edit_->maxBytes())
        return;
    identity_ = identity;
    loadBytes(authorityBytes);
}

QString QtHumanRequestEditor::authority() const
{
    return QString::fromStdString(authority_);
}

void QtHumanRequestEditor::setMode(Mode mode)
{
    if (mode == mode_)
        return;
    if (mode == Mode::Pretty && !parsed_ && !pretty_dirty_)
        return;
    mode_ = mode;
    const QSignalBlocker rawBlocker(raw_button_);
    const QSignalBlocker prettyBlocker(pretty_button_);
    raw_button_->setChecked(mode_ == Mode::Raw);
    pretty_button_->setChecked(mode_ == Mode::Pretty);
    stack_->setCurrentIndex(mode_ == Mode::Raw ? 0 : 1);
    recomputeMatches();
    refreshChrome();
    Q_EMIT modeChanged(mode_);
    Q_EMIT searchChanged();
}

void QtHumanRequestEditor::setEditable(bool editable)
{
    if (config_.editable == editable)
        return;
    config_.editable = editable;
    refreshChrome();
    refreshValidity();
}

void QtHumanRequestEditor::markClean()
{
    setRawDirty(false);
}

void QtHumanRequestEditor::setSearchQuery(const QString& query)
{
    if (search_edit_->text() == query) {
        search_query_ = query;
        recomputeMatches();
        Q_EMIT searchChanged();
        return;
    }
    search_edit_->setText(query);
}

bool QtHumanRequestEditor::isValid() const
{
    return editableEffective() &&
        (config_.allowEmpty || !authority_.empty()) &&
        !pretty_dirty_ && effectiveError().isEmpty();
}

QString QtHumanRequestEditor::effectiveError() const
{
    if (config_.allowEmpty && authority_.empty())
        return QString();
    return error_;
}

bool QtHumanRequestEditor::editableEffective() const
{
    return config_.editable && !oversized_ && !binary_;
}

void QtHumanRequestEditor::loadBytes(const QByteArray& bytes)
{
    pushed_bytes_ = bytes;
    authority_.assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    const qsizetype maxBytes = (std::max)(config_.maxBytes, static_cast<qsizetype>(1024));
    oversized_ = authority_.size() > static_cast<std::size_t>(maxBytes);
    binary_ = http_text::contains_binary_bytes(authority_);

    const std::string_view rawView(authority_);
    const std::string_view display = oversized_
        ? rawView.substr(0, (std::min)(rawView.size(),
            static_cast<std::size_t>(kOversizedPreviewChars)))
        : rawView;
    assignRawDocument(QString::fromUtf8(display.data(),
        static_cast<qsizetype>(display.size())));

    const auto parsed = http_text::parse_pretty(authority_,
        static_cast<std::size_t>(maxBytes));
    parsed_ = parsed.ok;
    line_ending_ = QString::fromStdString(parsed.line_ending);
    assignPrettyDocuments(parsed);
    setError(QString::fromStdString(parsed.error));

    setRawDirty(false);
    setPrettyDirty(false);
    pretty_revision_ = 0;
    validated_pretty_revision_ = 0;
    validation_pending_ = false;
    pretty_candidate_.clear();
    if (mode_ != Mode::Raw) {
        mode_ = Mode::Raw;
        const QSignalBlocker rawBlocker(raw_button_);
        const QSignalBlocker prettyBlocker(pretty_button_);
        raw_button_->setChecked(true);
        pretty_button_->setChecked(false);
        stack_->setCurrentIndex(0);
        Q_EMIT modeChanged(mode_);
    }
    recomputeMatches();
    refreshChrome();
    refreshValidity();
}

void QtHumanRequestEditor::assignRawDocument(const QString& text)
{
    const QSignalBlocker blocker(raw_edit_);
    raw_edit_->setPlainText(text);
}

void QtHumanRequestEditor::assignPrettyDocuments(const http_text::parsed_request_t& parsed)
{
    const QSignalBlocker lineBlocker(request_line_edit_);
    const QSignalBlocker headersBlocker(headers_edit_);
    const QSignalBlocker bodyBlocker(body_edit_);
    request_line_edit_->setText(QString::fromStdString(parsed.request_line));
    headers_edit_->setPlainText(QString::fromStdString(parsed.headers));
    body_edit_->setPlainText(QString::fromStdString(parsed.body));
}

void QtHumanRequestEditor::onRawTextChanged()
{
    if (!editableEffective())
        return;
    authority_ = raw_edit_->toPlainText().toStdString();
    pushed_bytes_ = QByteArray(authority_.data(), static_cast<qsizetype>(authority_.size()));
    setRawDirty(true);
    const auto parsed = http_text::parse_pretty(authority_,
        static_cast<std::size_t>(raw_edit_->maxBytes()));
    parsed_ = parsed.ok;
    line_ending_ = QString::fromStdString(parsed.line_ending);
    assignPrettyDocuments(parsed);
    setPrettyDirty(false);
    pretty_revision_ = 0;
    validated_pretty_revision_ = 0;
    pretty_candidate_.clear();
    setError(QString::fromStdString(parsed.error));
    setRawDirty(true);
    recomputeMatches();
    refreshChrome();
    refreshValidity();
    Q_EMIT authorityChanged();
}

void QtHumanRequestEditor::onPrettyChanged(bool bodyEdited)
{
    if (!editableEffective())
        return;
    markPrettyChanged();
    if (bodyEdited) {
        recomputeMatches();
        Q_EMIT searchChanged();
    }
    schedulePrettyValidation();
}

void QtHumanRequestEditor::markPrettyChanged()
{
    if (++pretty_revision_ == 0)
        ++pretty_revision_;
    setPrettyDirty(true);
}

void QtHumanRequestEditor::schedulePrettyValidation()
{
    if (validation_pending_)
        return;
    validation_pending_ = true;
    QTimer::singleShot(0, this, [this] {
        validation_pending_ = false;
        validatePrettyNow();
    });
}

void QtHumanRequestEditor::validatePrettyNow()
{
    if (!pretty_dirty_ || validated_pretty_revision_ == pretty_revision_)
        return;
    const std::string requestLine = request_line_edit_->text().toStdString();
    const std::string headers = headers_edit_->toPlainText().toStdString();
    const std::string body = body_edit_->toPlainText().toStdString();
    const std::string lineEnding = line_ending_.toStdString();
    const std::size_t maxBytes = static_cast<std::size_t>(raw_edit_->maxBytes());

    const bool validLine = http_text::validate_request_line(requestLine);
    std::string validationError;
    const bool validHeaders = http_text::validate_headers(headers, validationError);
    pretty_candidate_.clear();
    if (validLine && validHeaders) {
        std::size_t candidateSize = requestLine.size();
        const auto include = [&candidateSize, maxBytes](std::size_t length) {
            if (length > maxBytes - candidateSize)
                return false;
            candidateSize += length;
            return true;
        };
        const bool bounded = include(lineEnding.size()) &&
            (headers.empty() || (include(headers.size()) && include(lineEnding.size()))) &&
            include(lineEnding.size()) && include(body.size());
        if (!bounded) {
            validationError = "The edited request exceeds this editor's bounded capacity.";
        } else {
            std::string candidate;
            candidate.reserve(candidateSize);
            candidate.append(requestLine);
            candidate.append(lineEnding);
            if (!headers.empty()) {
                candidate.append(headers);
                candidate.append(lineEnding);
            }
            candidate.append(lineEnding);
            candidate.append(body);
            pretty_candidate_ = QString::fromStdString(candidate);
        }
    } else if (!validLine) {
        validationError = "The request line must contain method, target, and HTTP version.";
    }
    setError(QString::fromStdString(validationError));
    validated_pretty_revision_ = pretty_revision_;
    refreshChrome();
    refreshValidity();
}

void QtHumanRequestEditor::applyPretty()
{
    if (!pretty_dirty_ || !editableEffective() || !error_.isEmpty())
        return;
    authority_ = pretty_candidate_.toStdString();
    pushed_bytes_ = QByteArray(authority_.data(), static_cast<qsizetype>(authority_.size()));
    assignRawDocument(QString::fromStdString(authority_));
    setPrettyDirty(false);
    setRawDirty(true);
    setError(QString());
    recomputeMatches();
    refreshChrome();
    refreshValidity();
    Q_EMIT authorityChanged();
}

void QtHumanRequestEditor::discardPretty()
{
    if (!pretty_dirty_ || !editableEffective())
        return;
    const auto parsed = http_text::parse_pretty(authority_,
        static_cast<std::size_t>(raw_edit_->maxBytes()));
    parsed_ = parsed.ok;
    line_ending_ = QString::fromStdString(parsed.line_ending);
    assignPrettyDocuments(parsed);
    setError(QString::fromStdString(parsed.error));
    setPrettyDirty(false);
    pretty_revision_ = 0;
    validated_pretty_revision_ = 0;
    pretty_candidate_.clear();
    recomputeMatches();
    refreshChrome();
    refreshValidity();
}

QString QtHumanRequestEditor::currentSource() const
{
    return mode_ == Mode::Raw ? raw_edit_->toPlainText() : body_edit_->toPlainText();
}

QPlainTextEdit* QtHumanRequestEditor::visibleEdit() const
{
    return mode_ == Mode::Raw ? static_cast<QPlainTextEdit*>(raw_edit_)
                              : static_cast<QPlainTextEdit*>(body_edit_);
}

void QtHumanRequestEditor::recomputeMatches()
{
    matches_.clear();
    active_match_ = -1;
    const QString source = currentSource();
    if (!search_query_.isEmpty()) {
        qsizetype offset = 0;
        while (offset <= source.size() &&
               matches_.size() < static_cast<std::size_t>(kMaxMatchCount)) {
            const qsizetype found = source.indexOf(search_query_, offset);
            if (found < 0)
                break;
            matches_.push_back(found);
            offset = found + (std::max)(search_query_.size(), static_cast<qsizetype>(1));
        }
        if (!matches_.empty())
            active_match_ = 0;
    }
    applyMatchSelections();
    refreshChrome();
}

void QtHumanRequestEditor::applyMatchSelections()
{
    const auto& t = theme::tokens();
    QList<QTextEdit::ExtraSelection> selections;
    if (!search_query_.isEmpty()) {
        selections.reserve(static_cast<qsizetype>(matches_.size()));
        for (qsizetype i = 0; i < static_cast<qsizetype>(matches_.size()); ++i) {
            QTextEdit::ExtraSelection selection;
            QTextCursor cursor(visibleEdit()->document());
            const qsizetype begin = matches_[static_cast<std::size_t>(i)];
            cursor.setPosition(static_cast<int>(begin));
            cursor.setPosition(static_cast<int>(begin + search_query_.size()),
                QTextCursor::KeepAnchor);
            selection.cursor = cursor;
            if (static_cast<int>(i) == active_match_) {
                selection.format.setBackground(t.selection_strong);
                selection.format.setForeground(t.text_primary);
            } else {
                selection.format.setBackground(t.selection);
            }
            selections.append(selection);
        }
    }
    raw_edit_->setExtraSelections(mode_ == Mode::Raw ? selections : QList<QTextEdit::ExtraSelection>());
    body_edit_->setExtraSelections(mode_ == Mode::Pretty ? selections : QList<QTextEdit::ExtraSelection>());
}

void QtHumanRequestEditor::selectActiveMatch()
{
    if (active_match_ < 0 || active_match_ >= static_cast<int>(matches_.size()))
        return;
    auto* edit = visibleEdit();
    const qsizetype begin = matches_[static_cast<std::size_t>(active_match_)];
    QTextCursor cursor(edit->document());
    cursor.setPosition(static_cast<int>(begin));
    cursor.setPosition(static_cast<int>(begin + search_query_.size()),
        QTextCursor::KeepAnchor);
    edit->setTextCursor(cursor);
    edit->centerCursor();
}

void QtHumanRequestEditor::stepMatch(int direction)
{
    if (matches_.empty())
        return;
    const int count = static_cast<int>(matches_.size());
    active_match_ = ((active_match_ + direction) % count + count) % count;
    applyMatchSelections();
    selectActiveMatch();
    refreshChrome();
}

void QtHumanRequestEditor::setRawDirty(bool dirty)
{
    if (raw_dirty_ == dirty)
        return;
    raw_dirty_ = dirty;
    Q_EMIT dirtyChanged(raw_dirty_);
    refreshChrome();
}

void QtHumanRequestEditor::setPrettyDirty(bool dirty)
{
    if (pretty_dirty_ == dirty)
        return;
    pretty_dirty_ = dirty;
    Q_EMIT hasUnappliedPrettyChanged(pretty_dirty_);
    refreshChrome();
}

void QtHumanRequestEditor::setError(const QString& error)
{
    if (error_ == error)
        return;
    error_ = error;
    pretty_error_label_->setText(error_);
    refreshChrome();
}

void QtHumanRequestEditor::refreshChrome()
{
    const bool editableEffectiveNow = editableEffective();
    search_edit_->setPlaceholderText(mode_ == Mode::Raw
        ? QStringLiteral("Find in Raw") : QStringLiteral("Find in Body"));
    raw_edit_->setReadOnly(!editableEffectiveNow);
    request_line_edit_->setReadOnly(!editableEffectiveNow);
    headers_edit_->setReadOnly(!editableEffectiveNow);
    body_edit_->setReadOnly(!editableEffectiveNow);

    pretty_button_->setEnabled(parsed_ || pretty_dirty_);
    const bool hasMatches = !matches_.empty();
    prev_button_->setEnabled(hasMatches);
    next_button_->setEnabled(hasMatches);
    if (hasMatches)
        match_label_->setText(QStringLiteral("%1/%2")
            .arg(active_match_ + 1)
            .arg(static_cast<quint64>(matches_.size())));
    else if (!search_query_.isEmpty())
        match_label_->setText(QStringLiteral("0/0"));
    else
        match_label_->clear();

    if (pretty_dirty_)
        dirty_label_->setText(QStringLiteral("UNAPPLIED"));
    else if (raw_dirty_)
        dirty_label_->setText(QStringLiteral("MODIFIED"));
    else
        dirty_label_->clear();

    const bool canApply = pretty_dirty_ && editableEffectiveNow && error_.isEmpty();
    apply_button_->setEnabled(canApply);
    discard_button_->setEnabled(pretty_dirty_ && editableEffectiveNow);

    QString notice;
    if (oversized_) {
        notice = QStringLiteral("Read-only: %1 bytes exceeds the %2-byte editor limit.")
            .arg(static_cast<quint64>(authority_.size()))
            .arg(static_cast<qint64>((std::max)(config_.maxBytes,
                static_cast<qsizetype>(1024))));
    } else if (binary_) {
        notice = QStringLiteral("Read-only: binary or invalid UTF-8 bytes cannot be represented safely by this text editor.");
    } else if (!error_.isEmpty() && mode_ == Mode::Raw) {
        notice = error_;
    }
    notice_label_->setText(notice);
    notice_label_->setVisible(!notice.isEmpty());
    pretty_footer_->setVisible(mode_ == Mode::Pretty);
}

void QtHumanRequestEditor::refreshValidity()
{
    const QString currentError = effectiveError();
    const bool valid = editableEffective() &&
        (config_.allowEmpty || !authority_.empty()) &&
        !pretty_dirty_ && currentError.isEmpty();
    if (valid == valid_ && currentError == last_validity_error_)
        return;
    valid_ = valid;
    last_validity_error_ = currentError;
    Q_EMIT validityChanged(valid_, currentError);
}

}
