#include "qt/ai/qt_command_palette.hpp"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScreen>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

#include "core/ui/application_ui_runtime.hpp"
#include "qt/ai/qt_ai_chat_view.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::ai {

namespace {

enum class category_t : int {
    commands = 0,
    ide_actions,
    files,
    agents,
    skills,
    mcp,
    ai_actions,
};

category_t classify(const aida::commands::command_t& c) {
    if (!c.application_action_id.empty())
        return category_t::ide_actions;
    switch (c.source) {
    case aida::commands::command_source_t::agent:
        return category_t::agents;
    case aida::commands::command_source_t::skill:
        return category_t::skills;
    case aida::commands::command_source_t::mcp:
        return category_t::mcp;
    case aida::commands::command_source_t::builtin:
    default:
        if (!c.template_text.empty())
            return category_t::ai_actions;
        return category_t::commands;
    }
}

const char* category_label(category_t cat) {
    switch (cat) {
    case category_t::commands: return "COMMANDS";
    case category_t::ide_actions: return "IDE ACTIONS";
    case category_t::files: return "FILES";
    case category_t::agents: return "AGENTS";
    case category_t::skills: return "SKILLS";
    case category_t::mcp: return "MCP";
    case category_t::ai_actions: return "AI ACTIONS";
    default: return "";
    }
}

QColor source_color(aida::commands::command_source_t source) {
    const auto& t = theme::tokens();
    switch (source) {
    case aida::commands::command_source_t::builtin: return t.accent;
    case aida::commands::command_source_t::mcp: return t.info;
    case aida::commands::command_source_t::skill: return t.success;
    case aida::commands::command_source_t::agent: return t.warning;
    }
    return t.text_secondary;
}

void draw_source_glyph(QPainter* painter, const QRectF& zone,
                       aida::commands::command_source_t source, const QColor& color) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const QPointF c = zone.center();
    const qreal r = zone.width() * 0.5;
    painter->setPen(QPen(color, 1.4));
    painter->setBrush(Qt::NoBrush);
    switch (source) {
    case aida::commands::command_source_t::builtin: {
        painter->drawLine(QPointF(c.x(), c.y() - r * 0.9), QPointF(c.x() + r * 0.55, c.y()));
        painter->drawLine(QPointF(c.x() + r * 0.55, c.y()), QPointF(c.x(), c.y() + r * 0.9));
        painter->drawLine(QPointF(c.x(), c.y() + r * 0.9), QPointF(c.x() - r * 0.55, c.y()));
        painter->drawLine(QPointF(c.x() - r * 0.55, c.y()), QPointF(c.x(), c.y() - r * 0.9));
        painter->setBrush(color);
        painter->drawEllipse(c, r * 0.14, r * 0.14);
        break;
    }
    case aida::commands::command_source_t::mcp: {
        const QPointF a(c.x() - r * 0.65, c.y() + r * 0.30);
        const QPointF b(c.x() + r * 0.65, c.y() + r * 0.30);
        const QPointF top(c.x(), c.y() - r * 0.55);
        painter->drawLine(a, top);
        painter->drawLine(top, b);
        painter->drawLine(a, b);
        painter->setBrush(color);
        painter->drawEllipse(a, r * 0.18, r * 0.18);
        painter->drawEllipse(b, r * 0.18, r * 0.18);
        painter->drawEllipse(top, r * 0.18, r * 0.18);
        break;
    }
    case aida::commands::command_source_t::skill: {
        const QPointF p0(c.x() + r * 0.10, c.y() - r * 0.90);
        const QPointF p1(c.x() - r * 0.45, c.y() + r * 0.05);
        const QPointF p2(c.x() + r * 0.05, c.y() + r * 0.05);
        const QPointF p3(c.x() - r * 0.10, c.y() + r * 0.90);
        const QPointF p4(c.x() + r * 0.55, c.y() - r * 0.10);
        painter->drawPolygon(QPolygonF() << p0 << p4 << p2 << p1);
        painter->drawPolygon(QPolygonF() << p2 << p4 << p3 << p2);
        break;
    }
    case aida::commands::command_source_t::agent: {
        painter->setBrush(color);
        painter->drawEllipse(c, r * 0.85, r * 0.85);
        break;
    }
    }
    painter->restore();
}

}

AidaCommandModel::AidaCommandModel(QObject* parent) : QAbstractListModel(parent) {
    rebuild();
}

int AidaCommandModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(rows_.size());
}

QVariant AidaCommandModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(rows_.size()))
        return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    if (row.header || row.notice) {
        if (role == Qt::DisplayRole)
            return row.category_label;
        return {};
    }
    switch (role) {
    case Qt::DisplayRole:
        return QString::fromStdString(row.command.display_name.empty()
            ? row.command.name : row.command.display_name);
    case Qt::ToolTipRole: {
        const QString name = QString::fromStdString(row.command.display_name.empty()
            ? row.command.name : row.command.display_name);
        const QString description = QString::fromStdString(row.command.description);
        return description.isEmpty() ? name
            : QStringLiteral("%1\n%2").arg(name, description);
    }
    case Qt::UserRole:
        return QString::fromStdString(row.command.name);
    default:
        return {};
    }
}

Qt::ItemFlags AidaCommandModel::flags(const QModelIndex& index) const {
    if (!index.isValid())
        return Qt::NoItemFlags;
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    if (row.header || row.notice)
        return Qt::NoItemFlags;
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

void AidaCommandModel::setQuery(const QString& query) {
    if (query_ == query)
        return;
    query_ = query;
    rebuild();
}

void AidaCommandModel::rebuild() {
    beginResetModel();
    rows_.clear();
    auto hits = aida::commands::fuzzy_search(query_.toStdString(), 256);
    if (hits.empty()) {
        row_t notice;
        notice.notice = true;
        notice.category_label = query_.isEmpty()
            ? QStringLiteral("No commands are registered.")
            : QStringLiteral("No commands match \"%1\".").arg(query_);
        rows_.push_back(std::move(notice));
        endResetModel();
        return;
    }
    if (query_.isEmpty()) {
        std::sort(hits.begin(), hits.end(), [](const aida::commands::command_t& a,
                                               const aida::commands::command_t& b) {
            const category_t ca = classify(a);
            const category_t cb = classify(b);
            if (ca != cb)
                return static_cast<int>(ca) < static_cast<int>(cb);
            return a.name < b.name;
        });
        bool any_two_categories = false;
        category_t first_seen = category_t::commands;
        bool first_set = false;
        for (const auto& hit : hits) {
            if (!first_set) {
                first_seen = classify(hit);
                first_set = true;
            } else if (classify(hit) != first_seen) {
                any_two_categories = true;
                break;
            }
        }
        category_t previous = category_t::commands;
        bool have_previous = false;
        for (auto& hit : hits) {
            const category_t cat = classify(hit);
            if (any_two_categories && (!have_previous || cat != previous)) {
                row_t header;
                header.header = true;
                header.category_label = QString::fromLatin1(category_label(cat));
                rows_.push_back(std::move(header));
                previous = cat;
                have_previous = true;
            }
            row_t row;
            row.command = std::move(hit);
            rows_.push_back(std::move(row));
        }
    } else {
        rows_.reserve(hits.size());
        for (auto& hit : hits) {
            row_t row;
            row.command = std::move(hit);
            rows_.push_back(std::move(row));
        }
    }
    endResetModel();
}

const aida::commands::command_t* AidaCommandModel::commandAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    const auto& entry = rows_[static_cast<std::size_t>(row)];
    if (entry.header || entry.notice)
        return nullptr;
    return &entry.command;
}

bool AidaCommandModel::isHeaderAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return false;
    return rows_[static_cast<std::size_t>(row)].header;
}

bool AidaCommandModel::isNoticeAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return false;
    return rows_[static_cast<std::size_t>(row)].notice;
}

int AidaCommandModel::firstCommandRow() const {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (!rows_[i].header && !rows_[i].notice)
            return static_cast<int>(i);
    }
    return -1;
}

int AidaCommandModel::nextCommandRow(int row, int delta) const {
    if (rows_.empty())
        return -1;
    const int count = static_cast<int>(rows_.size());
    int next = row;
    for (int steps = 0; steps < count; ++steps) {
        next = (next + delta + count) % count;
        const auto& candidate = rows_[static_cast<std::size_t>(next)];
        if (!candidate.header && !candidate.notice)
            return next;
    }
    return -1;
}

AidaCommandDelegate::AidaCommandDelegate(AidaCommandModel* model, QObject* parent)
    : QStyledItemDelegate(parent), model_(model) {}

void AidaCommandDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
    if (!index.isValid())
        return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto& t = theme::tokens();
    const int line_h = t.spacing.lg + t.spacing.xxs;
    if (model_->isHeaderAt(index.row())) {
        painter->setFont(theme::fonts::caption());
        painter->setPen(t.text_dim);
        painter->drawText(option.rect.adjusted(t.spacing.sm + t.spacing.xxs, 0, 0, 0),
            Qt::AlignLeft | Qt::AlignVCenter,
            index.data(Qt::DisplayRole).toString());
        painter->restore();
        return;
    }
    if (model_->isNoticeAt(index.row())) {
        QFont notice_font = theme::fonts::caption();
        notice_font.setItalic(true);
        painter->setFont(notice_font);
        painter->setPen(t.text_dim);
        painter->drawText(option.rect.adjusted(t.spacing.sm + t.spacing.xxs, 0,
                                               -(t.spacing.sm + t.spacing.xxs), 0),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetricsF(notice_font).elidedText(index.data(Qt::DisplayRole).toString(),
                Qt::ElideRight,
                option.rect.width() - (t.spacing.sm + t.spacing.xxs) * 2));
        painter->restore();
        return;
    }
    const auto* command = model_->commandAt(index.row());
    if (command == nullptr) {
        painter->restore();
        return;
    }
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
    const QRect card = option.rect.adjusted(t.spacing.xxs, t.panel.border,
                                            -t.spacing.xxs, -t.panel.border);
    if (selected || hovered) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(selected ? t.selection : t.hover_wash);
        painter->drawRoundedRect(card, t.radius.md, t.radius.md);
    }
    if (selected) {
        painter->setPen(QPen(t.accent, 1.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(card, t.radius.md, t.radius.md);
    }

    const int glyph_side = t.spacing.lg + t.spacing.xxs;
    const QRect glyph_rect(card.left() + t.spacing.sm, card.center().y() - glyph_side / 2,
                           glyph_side, glyph_side);
    draw_source_glyph(painter, glyph_rect, command->source, source_color(command->source));

    const int text_x = glyph_rect.right() + t.spacing.sm + t.spacing.xxs;

    int reserve_right = t.spacing.md;
    QRect hint_rect;
    QRect shortcut_rect;
    if (selected) {
        const QFontMetricsF chip_fm(theme::fonts::caption());
        const int chip_pad_x = t.spacing.sm;
        const int chip_h = line_h;
        const QString hint = QStringLiteral("Enter run · Tab preview");
        const int hint_w = qRound(chip_fm.horizontalAdvance(hint)) + chip_pad_x * 2;
        hint_rect = QRect(card.right() - hint_w - t.spacing.sm,
                          card.center().y() - chip_h / 2, hint_w, chip_h);
        int right_edge = hint_rect.left() - t.spacing.xs - t.spacing.xxs;
        if (!command->shortcut.empty()) {
            const QString shortcut = QString::fromStdString(command->shortcut);
            const int shortcut_w = qRound(chip_fm.horizontalAdvance(shortcut)) +
                chip_pad_x * 2;
            shortcut_rect = QRect(right_edge - shortcut_w, hint_rect.top(),
                                  shortcut_w, chip_h);
            right_edge = shortcut_rect.left() - t.spacing.xs - t.spacing.xxs;
        }
        reserve_right = card.right() - right_edge + t.spacing.sm;
    }

    const QFont name_font = theme::fonts::strong();
    const QFontMetricsF name_fm(name_font);
    const QString name = QString::fromStdString(command->display_name.empty()
        ? command->name : command->display_name);
    const int name_avail = (std::max)(card.right() - reserve_right - text_x, 0);
    const int name_w = (std::min)(qRound(name_fm.horizontalAdvance(name)) + t.spacing.xs,
                                  name_avail);
    const QRect name_rect(text_x, card.top() + t.spacing.xs, name_w, line_h);
    const QString drawn_name = name_fm.elidedText(name, Qt::ElideRight, name_rect.width());
    const QString query = model_->query();
    if (name_w <= t.spacing.xs || drawn_name.isEmpty()) {
        painter->restore();
        return;
    }
    if (!query.isEmpty() && command->enabled) {
        QVector<bool> matched(drawn_name.size(), false);
        int needle = 0;
        for (int i = 0; i < drawn_name.size() && needle < query.size(); ++i) {
            if (drawn_name.at(i).toLower() == query.at(needle).toLower()) {
                matched[i] = true;
                ++needle;
            }
        }
        qreal run_x = name_rect.left();
        int run_start = 0;
        while (run_start < drawn_name.size()) {
            int run_end = run_start + 1;
            while (run_end < drawn_name.size() &&
                   matched[run_end] == matched[run_start])
                ++run_end;
            const QString run_text = drawn_name.mid(run_start, run_end - run_start);
            QFont run_font = name_font;
            if (matched[run_start]) {
                run_font.setWeight(QFont::Bold);
                painter->setFont(run_font);
                painter->setPen(t.accent_hover);
            } else {
                painter->setFont(run_font);
                painter->setPen(command->enabled ? t.text_primary : t.disabled);
            }
            const QFontMetricsF run_fm(run_font);
            painter->drawText(QPointF(run_x,
                widgets::text_baseline_centered(name_rect, run_fm)), run_text);
            run_x += run_fm.horizontalAdvance(run_text);
            run_start = run_end;
        }
    } else {
        painter->setFont(name_font);
        painter->setPen(command->enabled ? t.text_primary : t.disabled);
        painter->drawText(name_rect, Qt::AlignLeft | Qt::AlignVCenter, drawn_name);
    }

    painter->setFont(theme::fonts::codeRegular());
    painter->setPen(t.text_dim);
    const QString slash_name = QStringLiteral("/%1").arg(QString::fromStdString(command->name));
    const QRect slash_rect(name_rect.right() + t.spacing.xs + t.spacing.xxs,
                           card.top() + t.spacing.xs,
                           card.right() - reserve_right - name_rect.right() -
                               t.spacing.xs - t.spacing.xxs,
                           line_h);
    if (slash_rect.width() > t.spacing.section + t.spacing.sm)
        painter->drawText(slash_rect, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetricsF(theme::fonts::codeRegular()).elidedText(
                slash_name, Qt::ElideRight, slash_rect.width()));

    painter->setFont(theme::fonts::caption());
    painter->setPen(t.text_secondary);
    const QRect desc_rect(text_x, card.top() + t.spacing.xs + line_h + t.spacing.xxs,
                          card.right() - reserve_right - text_x, t.spacing.lg);
    painter->drawText(desc_rect, Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetricsF(theme::fonts::caption()).elidedText(
            QString::fromStdString(command->description), Qt::ElideRight, desc_rect.width()));

    if (selected) {
        painter->setFont(theme::fonts::caption());
        if (shortcut_rect.isValid()) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(t.panel_header);
            painter->drawRoundedRect(shortcut_rect, t.radius.sm, t.radius.sm);
            painter->setPen(t.text_secondary);
            painter->drawText(shortcut_rect, Qt::AlignCenter,
                              QString::fromStdString(command->shortcut));
        }
        painter->setPen(Qt::NoPen);
        painter->setBrush(t.panel_header);
        painter->drawRoundedRect(hint_rect, t.radius.sm, t.radius.sm);
        painter->setPen(t.text_dim);
        painter->drawText(hint_rect, Qt::AlignCenter,
                          QStringLiteral("Enter run · Tab preview"));
    }
    painter->restore();
}

QSize AidaCommandDelegate::sizeHint(const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const {
    const auto& t = theme::tokens();
    Q_UNUSED(option);
    if (model_->isHeaderAt(index.row()) || model_->isNoticeAt(index.row()))
        return QSize(t.shell.min_panel_w, t.row.compact);
    return QSize(t.shell.min_panel_w * 2,
                 t.spacing.xs * 2 + (t.spacing.lg + t.spacing.xxs) * 2 + t.spacing.xxs);
}

AidaPalettePreviewPane::AidaPalettePreviewPane(QWidget* parent) : QWidget(parent) {
    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding,
                             t.panel.padding, t.panel.padding);
    root->setSpacing(t.spacing.xs + t.spacing.xxs);
    title_ = new QLabel(this);
    title_->setFont(theme::fonts::strong());
    description_ = new QLabel(this);
    description_->setWordWrap(true);
    description_->setFont(theme::fonts::caption());
    description_->setProperty("aidaVariant", QStringLiteral("secondary"));
    body_ = new QPlainTextEdit(this);
    body_->setReadOnly(true);
    body_->setFont(theme::fonts::codeRegular());
    hints_ = new QLabel(this);
    hints_->setWordWrap(true);
    hints_->setFont(theme::fonts::caption());
    source_path_ = new QLabel(this);
    source_path_->setFont(theme::fonts::caption());
    source_path_->setProperty("aidaVariant", QStringLiteral("secondary"));
    source_path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(title_);
    root->addWidget(description_);
    root->addWidget(body_, 1);
    root->addWidget(hints_);
    root->addWidget(source_path_);
    clearPreview();
}

void AidaPalettePreviewPane::clearPreview() {
    title_->setText(QStringLiteral("No selection"));
    description_->clear();
    body_->clear();
    hints_->clear();
    source_path_->clear();
}

void AidaPalettePreviewPane::showCommand(const aida::commands::command_t& command) {
    const QString display = QString::fromStdString(command.display_name.empty()
        ? command.name : command.display_name);
    title_->setText(QStringLiteral("%1  ·  /%2")
        .arg(display, QString::fromStdString(command.name)));
    description_->setText(QString::fromStdString(command.description));
    if (!command.template_text.empty()) {
        body_->setPlainText(QString::fromStdString(command.template_text));
        QList<QTextEdit::ExtraSelection> selections;
        const QString text = body_->toPlainText();
        const auto& t = theme::tokens();
        for (int i = 0; i < text.size(); ++i) {
            if (text.at(i) != QLatin1Char('$'))
                continue;
            int len = 0;
            if (i + 1 < text.size() && text.at(i + 1) >= QLatin1Char('1') &&
                text.at(i + 1) <= QLatin1Char('9'))
                len = 2;
            else if (text.mid(i, 10) == QStringLiteral("$ARGUMENTS"))
                len = 10;
            if (len == 0)
                continue;
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(body_->document());
            selection.cursor.setPosition(i);
            selection.cursor.setPosition(i + len, QTextCursor::KeepAnchor);
            selection.format.setForeground(t.accent);
            selection.format.setFontWeight(QFont::Bold);
            selections.push_back(selection);
            i += len - 1;
        }
        body_->setExtraSelections(selections);
        QStringList hints;
        for (const auto& hint : command.placeholder_hints)
            hints << QString::fromStdString(hint);
        hints_->setText(hints.isEmpty() ? QString()
            : QStringLiteral("Arguments: %1").arg(hints.join(QStringLiteral(", "))));
    } else if (command.source == aida::commands::command_source_t::agent) {
        QStringList lines;
        if (command.agent_override.has_value())
            lines << QStringLiteral("Agent: %1")
                .arg(QString::fromStdString(*command.agent_override));
        if (command.model_override.has_value())
            lines << QStringLiteral("Model: %1")
                .arg(QString::fromStdString(*command.model_override));
        if (command.subtask)
            lines << QStringLiteral("Runs as a subtask");
        body_->setPlainText(lines.join(QStringLiteral("\n")));
        hints_->clear();
    } else {
        body_->setPlainText(QString());
        QStringList hints;
        for (const auto& hint : command.placeholder_hints)
            hints << QString::fromStdString(hint);
        hints_->setText(hints.isEmpty() ? QString()
            : QStringLiteral("Arguments: %1").arg(hints.join(QStringLiteral(", "))));
    }
    source_path_->setText(command.source_path.empty() ? QString()
        : QStringLiteral("Source: %1").arg(QString::fromStdString(command.source_path)));
}

AidaCommandPaletteDialog::AidaCommandPaletteDialog(QWidget* parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("aida.command_palette"));
    setProperty("aidaRole", QStringLiteral("dialog"));
    setMinimumSize(480, 320);

    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding,
                             t.panel.padding, t.panel.padding);
    root->setSpacing(t.spacing.xs + t.spacing.xxs);
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("aida.command_palette.input"));
    input_->setPlaceholderText(
        QStringLiteral("Search commands, files, agents, or AI actions..."));
    input_->setClearButtonEnabled(true);
    root->addWidget(input_);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setObjectName(QStringLiteral("aida.command_palette.splitter"));
    model_ = new AidaCommandModel(this);
    list_ = new QListView(this);
    list_->setObjectName(QStringLiteral("aida.command_palette.list"));
    delegate_ = new AidaCommandDelegate(model_, list_);
    list_->setModel(model_);
    list_->setItemDelegate(delegate_);
    list_->setUniformItemSizes(false);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setMouseTracking(true);
    list_->setAccessibleName(QStringLiteral("Command results"));
    splitter_->addWidget(list_);
    preview_ = new AidaPalettePreviewPane(this);
    preview_->setObjectName(QStringLiteral("aida.command_palette.preview"));
    splitter_->addWidget(preview_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    root->addWidget(splitter_, 1);

    connect(input_, &QLineEdit::textChanged, this, [this](const QString& text) {
        model_->setQuery(text);
        const int first = model_->firstCommandRow();
        if (first >= 0)
            list_->setCurrentIndex(model_->index(first));
        syncPreview();
    });
    connect(input_, &QLineEdit::returnPressed, this, [this] {
        const QModelIndex current = list_->currentIndex();
        const auto* command = model_->commandAt(current.isValid() ? current.row()
            : model_->firstCommandRow());
        if (command != nullptr)
            executeSelection(*command);
    });
    connect(list_, &QListView::activated, this, [this](const QModelIndex& index) {
        const auto* command = model_->commandAt(index.row());
        if (command != nullptr)
            executeSelection(*command);
    });
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { syncPreview(); });
    input_->installEventFilter(this);
}

void AidaCommandPaletteDialog::toggleInteractive(QWidget* anchor) {
    static AidaCommandPaletteDialog* dialog = nullptr;
    if (dialog != nullptr && dialog->isVisible()) {
        dialog->close();
        return;
    }
    if (QWidget* popup = QApplication::activePopupWidget())
        popup->close();
    if (dialog == nullptr)
        dialog = new AidaCommandPaletteDialog(anchor);
    dialog->model_->setQuery(QString());
    dialog->input_->clear();
    const int first = dialog->model_->firstCommandRow();
    if (first >= 0)
        dialog->list_->setCurrentIndex(dialog->model_->index(first));
    dialog->syncPreview();
    if (anchor != nullptr) {
        if (QWidget* window = anchor->window()) {
            const QRect host = window->geometry();
            const QSize max_size(720, 540);
            dialog->resize(max_size.boundedTo(host.size() * 0.8));
            dialog->move(host.center() - dialog->rect().center());
        }
    }
    dialog->show();
    dialog->raise();
    dialog->input_->setFocus();
}

void AidaCommandPaletteDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    input_->setFocus();
}

void AidaCommandPaletteDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    preview_->setVisible(width() >= 680 && height() >= 420);
}

bool AidaCommandPaletteDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == input_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Down:
            moveSelection(1);
            return true;
        case Qt::Key_Up:
            moveSelection(-1);
            return true;
        case Qt::Key_Tab:
            preview_->setVisible(!preview_->isVisible());
            return true;
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void AidaCommandPaletteDialog::moveSelection(int delta) {
    const QModelIndex current = list_->currentIndex();
    const int from = current.isValid() ? current.row() : model_->firstCommandRow();
    if (from < 0)
        return;
    const int next = model_->nextCommandRow(from, delta);
    if (next < 0)
        return;
    const QModelIndex target = model_->index(next);
    list_->setCurrentIndex(target);
    list_->scrollTo(target);
}

void AidaCommandPaletteDialog::syncPreview() {
    const QModelIndex current = list_->currentIndex();
    const auto* command = current.isValid() ? model_->commandAt(current.row()) : nullptr;
    if (command != nullptr)
        preview_->showCommand(*command);
    else
        preview_->clearPreview();
}

void AidaCommandPaletteDialog::executeSelection(const aida::commands::command_t& command) {
    if (!command.enabled) {
        chrome::toast_warning(QString::fromStdString(command.disabled_reason.empty()
            ? "This action is unavailable" : command.disabled_reason), 6.0);
        return;
    }
    if (!command.application_action_id.empty()) {
        const auto result = aida::ui::application_ui::execute_action(
            command.application_action_id.c_str(),
            aida::ui::action_invocation_source_t::command_palette);
        if (result.status == aida::ui::action_execution_status_t::confirmation_required ||
            result.status == aida::ui::action_execution_status_t::review_required) {
            close();
            return;
        }
        if (!result.executed())
            return;
        close();
        return;
    }
    std::vector<std::string> args;
    std::string out;
    const bool ok = aida::commands::execute(command.name, args, out);
    if (!ok) {
        chrome::toast_error(QStringLiteral("/%1: %2")
            .arg(QString::fromStdString(command.name),
                 QString::fromStdString(aida::commands::last_error())), 6.0);
        close();
        return;
    }
    const bool is_programmatic =
        (command.source == aida::commands::command_source_t::builtin &&
            command.template_text.empty()) ||
        (command.source == aida::commands::command_source_t::agent);
    if (is_programmatic) {
        if (!out.empty())
            chrome::toast_info(QString::fromStdString(out), 6.0);
    } else {
        if (!out.empty())
            AidaChatController::instance().appendUserMessage(out);
    }
    close();
}

}
