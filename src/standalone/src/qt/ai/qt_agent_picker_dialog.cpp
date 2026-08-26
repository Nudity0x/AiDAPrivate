#include "qt/ai/qt_agent_picker_dialog.hpp"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

#include "core/ai/agent_registry.hpp"
#include "core/ai/command_registry.hpp"
#include "core/infra/event_bus.hpp"
#include "qt/ai/qt_ai_domain.hpp"
#include "qt/ai/qt_chat_inject.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::ai {

namespace {

QString agent_lower(const std::string& value) {
    return QString::fromStdString(value).toLower();
}

QColor agent_seed_color(const std::string& seed) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : seed) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    const auto& t = theme::tokens();
    const int hue = static_cast<int>(hash % 360ULL);
    QColor color = QColor::fromHsl(hue, 150, 150);
    if (!color.isValid())
        color = t.accent;
    return color;
}

}

AidaAgentPickerModel::AidaAgentPickerModel(QObject* parent) : QAbstractListModel(parent) {
    refresh();
}

int AidaAgentPickerModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(rows_.size());
}

QVariant AidaAgentPickerModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(rows_.size()))
        return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    if (row.notice) {
        if (role == Qt::DisplayRole)
            return QString::fromStdString(row.description);
        return {};
    }
    switch (role) {
    case Qt::DisplayRole:
        return QString::fromStdString(row.name);
    case Qt::ToolTipRole:
        return QString::fromStdString(row.description);
    case Qt::UserRole:
        return QString::fromStdString(row.name);
    default:
        return {};
    }
}

void AidaAgentPickerModel::setFilter(const QString& query) {
    beginResetModel();
    const std::string needle = query.toLower().toStdString();
    std::vector<row_t> filtered;
    const auto primary = aida::agent::primary_agents();
    filtered.reserve(primary.size());
    for (const auto* info : primary) {
        if (info == nullptr || info->hidden)
            continue;
        row_t row;
        row.name = info->name;
        row.description = info->description;
        row.native = info->native;
        if (!needle.empty()) {
            const int name_score =
                aida::commands::fuzzy_score_text(needle, agent_lower(info->name).toStdString());
            int score = name_score;
            if (score < 0) {
                const int desc_score = aida::commands::fuzzy_score_text(
                    needle, agent_lower(info->description).toStdString());
                score = desc_score < 0 ? -1 : desc_score / 2;
            }
            if (score < 0)
                continue;
            row.score = score;
        }
        filtered.push_back(std::move(row));
    }
    if (!needle.empty()) {
        std::stable_sort(filtered.begin(), filtered.end(),
            [](const row_t& a, const row_t& b) { return a.score > b.score; });
    }
    const std::string active = aida::agent::active_agent_name();
    for (auto& row : filtered)
        row.active = (row.name == active);
    rows_ = std::move(filtered);
    if (rows_.empty()) {
        row_t notice;
        notice.notice = true;
        notice.description = needle.empty()
            ? "No agents are registered."
            : "No agents match \"" + needle + "\".";
        rows_.push_back(std::move(notice));
    }
    endResetModel();
}

void AidaAgentPickerModel::refresh() {
    setFilter(QString());
}

const aida::agent::agent_info_t* AidaAgentPickerModel::agentAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    const auto& row_ref = rows_[static_cast<std::size_t>(row)];
    if (row_ref.notice)
        return nullptr;
    return aida::agent::get(row_ref.name);
}

QString AidaAgentPickerModel::nameAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return {};
    const auto& row_ref = rows_[static_cast<std::size_t>(row)];
    if (row_ref.notice)
        return {};
    return QString::fromStdString(row_ref.name);
}

bool AidaAgentPickerModel::isNoticeAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return false;
    return rows_[static_cast<std::size_t>(row)].notice;
}

AidaAgentPickerDelegate::AidaAgentPickerDelegate(AidaAgentPickerModel* model, QObject* parent)
    : QStyledItemDelegate(parent), model_(model) {}

void AidaAgentPickerDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const {
    if (!index.isValid())
        return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto& t = theme::tokens();
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
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
    const QRect card = option.rect.adjusted(t.spacing.xxs, t.spacing.xxs,
                                            -t.spacing.xxs, -t.spacing.xxs);
    QColor fill = hovered ? t.hover_wash : t.panel_header;
    if (selected)
        fill = t.selection;
    painter->setPen(Qt::NoPen);
    painter->setBrush(fill);
    painter->drawRoundedRect(card, t.radius.lg, t.radius.lg);
    const QColor border = selected ? t.accent : t.border_subtle;
    painter->setPen(QPen(border, selected ? 1.5 : 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(card, t.radius.lg, t.radius.lg);

    const QString name = model_->nameAt(index.row());
    const auto* info = model_->agentAt(index.row());
    const QString description = info != nullptr
        ? QString::fromStdString(info->description) : QString();
    const bool native = info != nullptr && info->native;
    const bool active = info != nullptr && info->name == aida::agent::active_agent_name();

    const int avatar_side = t.control.icon_button;
    const QRect avatar_rect(card.left() + t.spacing.sm + t.spacing.xxs,
                            card.center().y() - avatar_side / 2,
                            avatar_side, avatar_side);
    painter->setBrush(agent_seed_color(name.toStdString()));
    painter->setPen(Qt::NoPen);
    painter->drawEllipse(avatar_rect);
    painter->setPen(widgets::with_alpha(t.text_primary, 0.90));
    const QFont avatar_font = theme::fonts::ui(700, t.control.icon_glyph);
    painter->setFont(avatar_font);
    const QString initial = name.isEmpty() ? QStringLiteral("?") : name.left(1).toUpper();
    painter->drawText(avatar_rect, Qt::AlignCenter, initial);

    const int text_x = avatar_rect.right() + t.spacing.sm + t.spacing.xxs;
    const QFont name_font = theme::fonts::strong();
    const QFontMetricsF name_fm(name_font);
    const int name_h = t.spacing.lg + t.spacing.xxs;
    const QRect name_rect(text_x, card.top() + t.spacing.sm,
                          card.right() - text_x - t.shell.min_panel_w, name_h);
    const QString elided_name = name_fm.elidedText(name, Qt::ElideRight, name_rect.width());
    painter->setFont(name_font);
    painter->setPen(t.text_primary);
    painter->drawText(name_rect, Qt::AlignLeft | Qt::AlignVCenter, elided_name);

    const QFont caption_font = theme::fonts::caption();
    const QFontMetricsF caption_fm(caption_font);
    const int badge_h = t.spacing.md + t.spacing.xxs;
    int badge_x = name_rect.left() + qRound(name_fm.horizontalAdvance(elided_name)) +
        t.spacing.sm;
    if (native) {
        const QString badge = QStringLiteral("native");
        const int badge_w = qRound(caption_fm.horizontalAdvance(badge)) + t.spacing.md;
        const QRect badge_rect(badge_x, card.top() + t.spacing.sm + t.spacing.xxs,
                               badge_w, badge_h);
        painter->setPen(Qt::NoPen);
        painter->setBrush(t.info);
        painter->drawRoundedRect(badge_rect, badge_h * 0.5, badge_h * 0.5);
        painter->setPen(widgets::with_alpha(t.text_primary, 0.94));
        painter->setFont(caption_font);
        painter->drawText(badge_rect, Qt::AlignCenter, badge);
        badge_x += badge_w + t.spacing.xs + t.spacing.xxs;
    }
    if (active) {
        const QString badge = QStringLiteral("active");
        const int badge_w = qRound(caption_fm.horizontalAdvance(badge)) + t.spacing.md;
        const QRect badge_rect(badge_x, card.top() + t.spacing.sm + t.spacing.xxs,
                               badge_w, badge_h);
        painter->setPen(Qt::NoPen);
        painter->setBrush(t.success);
        painter->drawRoundedRect(badge_rect, badge_h * 0.5, badge_h * 0.5);
        painter->setPen(widgets::with_alpha(t.bg_base, 0.94));
        painter->setFont(caption_font);
        painter->drawText(badge_rect, Qt::AlignCenter, badge);
    }

    painter->setFont(caption_font);
    painter->setPen(t.text_secondary);
    const QRect desc_rect(text_x,
                          card.top() + t.spacing.sm + name_h + t.spacing.xxs,
                          card.right() - text_x - t.spacing.md, t.spacing.lg);
    painter->drawText(desc_rect, Qt::AlignLeft | Qt::AlignVCenter,
        caption_fm.elidedText(description, Qt::ElideRight, desc_rect.width()));
    painter->restore();
}

QSize AidaAgentPickerDelegate::sizeHint(const QStyleOptionViewItem& option,
                                        const QModelIndex& index) const {
    Q_UNUSED(option);
    const auto& t = theme::tokens();
    if (model_->isNoticeAt(index.row()))
        return QSize(t.shell.min_panel_w, t.row.compact);
    return QSize(t.row.property_label_w,
                 t.spacing.sm * 2 + (t.spacing.lg + t.spacing.xxs) + t.spacing.xxs +
                 t.spacing.lg);
}

AidaAgentPickerDialog::AidaAgentPickerDialog(QWidget* parent)
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Dialog) {
    setWindowModality(Qt::ApplicationModal);
    const auto& t = theme::tokens();
    setFixedSize(5 * static_cast<int>(t.shell.min_panel_w),
                 3 * t.row.property_label_w + 2 * t.panel.overlay_margin);
    setObjectName(QStringLiteral("aida.ai.agent_picker"));
    setProperty("aidaRole", QStringLiteral("dialog"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.spacing.lg, t.spacing.lg - t.spacing.xxs,
                             t.spacing.lg, t.spacing.md);
    root->setSpacing(t.spacing.sm);

    auto* title_row = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("Switch agent"), this);
    title->setFont(theme::fonts::h2());
    auto* hint = new QLabel(QStringLiteral("Esc to cancel  |  Enter to switch"), this);
    hint->setFont(theme::fonts::caption());
    hint->setProperty("aidaVariant", QStringLiteral("secondary"));
    title_row->addWidget(title, 1);
    title_row->addWidget(hint, 0, Qt::AlignRight | Qt::AlignVCenter);
    root->addLayout(title_row);

    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("aida.ai.agent_picker.filter"));
    filter_->setPlaceholderText(QStringLiteral("Search agents..."));
    filter_->setClearButtonEnabled(true);
    root->addWidget(filter_);

    model_ = new AidaAgentPickerModel(this);
    list_ = new QListView(this);
    list_->setObjectName(QStringLiteral("aida.ai.agent_picker.list"));
    delegate_ = new AidaAgentPickerDelegate(model_, list_);
    list_->setModel(model_);
    list_->setItemDelegate(delegate_);
    list_->setUniformItemSizes(false);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setAccessibleName(QStringLiteral("Available agents"));
    root->addWidget(list_, 1);

    auto* footer = new QHBoxLayout();
    auto* manage = new QPushButton(QStringLiteral("Manage agents..."), this);
    manage->setObjectName(QStringLiteral("aida.ai.agent_picker.manage"));
    manage->setAutoDefault(false);
    manage->setToolTip(QStringLiteral("Open the agent manager"));
    active_label_ = new QLabel(this);
    active_label_->setFont(theme::fonts::caption());
    active_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    footer->addWidget(manage, 0);
    footer->addStretch(1);
    footer->addWidget(active_label_, 0, Qt::AlignRight | Qt::AlignVCenter);
    root->addLayout(footer);

    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        model_->setFilter(text);
        if (model_->rowCount() > 0)
            list_->setCurrentIndex(model_->index(0));
    });
    connect(filter_, &QLineEdit::returnPressed, this, [this] {
        const QModelIndex current = list_->currentIndex();
        if (current.isValid())
            dispatchRow(current.row());
        else if (model_->rowCount() > 0)
            dispatchRow(0);
    });
    connect(list_, &QListView::activated, this, [this](const QModelIndex& index) {
        if (index.isValid())
            dispatchRow(index.row());
    });
    connect(manage, &QPushButton::clicked, this, [this] {
        Q_EMIT manageAgentsRequested();
        reject();
    });
    filter_->installEventFilter(this);
}

void AidaAgentPickerDialog::toggleInteractive(QWidget* anchor) {
    static AidaAgentPickerDialog* dialog = nullptr;
    if (dialog != nullptr && dialog->isVisible()) {
        dialog->reject();
        return;
    }
    if (dialog == nullptr) {
        dialog = new AidaAgentPickerDialog(anchor);
        QObject::connect(dialog, &AidaAgentPickerDialog::manageAgentsRequested, dialog, [] {
            open_ai_view("view.ai.agents");
        });
    }
    dialog->openAndReset();
}

bool AidaAgentPickerDialog::pickerVisible() {
    return QApplication::activeModalWidget() != nullptr &&
        QApplication::activeModalWidget()->objectName() ==
            QStringLiteral("aida.ai.agent_picker");
}

void AidaAgentPickerDialog::openAndReset() {
    filter_->clear();
    model_->setFilter(QString());
    const std::string active = aida::agent::active_agent_name();
    active_label_->setText(QStringLiteral("Active: %1")
        .arg(active.empty() ? QStringLiteral("none")
                            : QString::fromStdString(active)));
    if (model_->rowCount() > 0)
        list_->setCurrentIndex(model_->index(0));
    show();
    raise();
    activateWindow();
    filter_->setFocus();
}

void AidaAgentPickerDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    filter_->setFocus();
}

bool AidaAgentPickerDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == filter_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Down) {
            moveSelection(1);
            return true;
        }
        if (key->key() == Qt::Key_Up) {
            moveSelection(-1);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void AidaAgentPickerDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        filter_->hasFocus()) {
        const QModelIndex current = list_->currentIndex();
        if (current.isValid())
            dispatchRow(current.row());
        return;
    }
    QDialog::keyPressEvent(event);
}

void AidaAgentPickerDialog::moveSelection(int delta) {
    const int count = model_->rowCount();
    if (count <= 0)
        return;
    if (count == 1 && model_->isNoticeAt(0))
        return;
    const QModelIndex current = list_->currentIndex();
    const int row = current.isValid() ? current.row() : 0;
    int next = row;
    for (int steps = 0; steps < count; ++steps) {
        next = (next + delta + count) % count;
        if (!model_->isNoticeAt(next))
            break;
    }
    if (model_->isNoticeAt(next))
        return;
    const QModelIndex target = model_->index(next);
    list_->setCurrentIndex(target);
    list_->scrollTo(target);
}

void AidaAgentPickerDialog::dispatchRow(int row) {
    if (model_->isNoticeAt(row))
        return;
    const auto* info = model_->agentAt(row);
    if (info == nullptr) {
        chrome::toast_warning(QStringLiteral("The agent catalog changed; pick again"), 3.5);
        model_->setFilter(filter_->text());
        return;
    }
    const std::string previous = aida::agent::active_agent_name();
    const std::string name = info->name;
    if (!aida::agent::set_active_agent(name)) {
        chrome::toast_error(QStringLiteral("The agent could not be activated"), 4.0);
        return;
    }
    aida::events::publish(aida::events::event_agent_changed,
        aida::events::agent_changed_t{ std::string{}, previous, name });
    AidaChatInjectBridge::instance().post("@" + name + " ");
    chrome::toast_info(QStringLiteral("Agent: %1").arg(QString::fromStdString(name)), 2.5);
    accept();
}

}
