#include "qt/ai/qt_skill_manager_view.hpp"

#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QTabBar>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cstdint>

#include <windows.h>
#include <shellapi.h>

#include "core/ai/agent_registry.hpp"
#include "core/infra/event_bus.hpp"
#include "core/ui/chat_markdown.hpp"
#include "qt/ai/qt_ai_chat_delegate.hpp"
#include "qt/ai/qt_ai_chat_dialogs.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_chip.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::ai {

namespace {

std::string lower_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

QString source_label(const aida::skills::skill_metadata_t& meta) {
    if (meta.source == "remote")
        return QStringLiteral("remote");
    if (meta.source == "global")
        return QStringLiteral("built-in");
    return QStringLiteral("project");
}

QColor source_color(const aida::skills::skill_metadata_t& meta) {
    const auto& t = theme::tokens();
    if (meta.source == "remote")
        return t.accent;
    if (meta.source == "global")
        return t.info;
    return t.success;
}

int classify_skill(const aida::skills::skill_metadata_t& meta) {
    if (meta.source == "remote")
        return 2;
    if (meta.source == "global")
        return 0;
    return 1;
}

void open_path_in_shell(const std::string& path, bool select_in_explorer) {
    if (path.empty())
        return;
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wlen <= 0)
        return;
    std::wstring wpath(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
    if (!wpath.empty() && wpath.back() == L'\0')
        wpath.pop_back();
    if (select_in_explorer) {
        std::wstring args = L"/select,\"" + wpath + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

}

AidaSkillListModel::AidaSkillListModel(QObject* parent) : QAbstractListModel(parent) {}

int AidaSkillListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(rows_.size());
}

QVariant AidaSkillListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(rows_.size()))
        return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
        return QString::fromStdString(row.name);
    case Qt::ToolTipRole:
        return QString::fromStdString(row.file_path);
    case Qt::UserRole:
        return QString::fromStdString(row.name);
    default:
        return {};
    }
}

void AidaSkillListModel::setTab(int tab) {
    if (tab_ == tab)
        return;
    tab_ = tab;
    reloadFrom(publication_);
}

void AidaSkillListModel::setSearch(const QString& text) {
    if (search_ == text)
        return;
    search_ = text;
    reloadFrom(publication_);
}

void AidaSkillListModel::setAgentFilter(const QString& agent_name) {
    if (agent_filter_ == agent_name)
        return;
    agent_filter_ = agent_name;
    reloadFrom(publication_);
}

void AidaSkillListModel::reloadFrom(
    const aida::skill_manager_service::snapshot_ptr& publication) {
    beginResetModel();
    publication_ = publication;
    rows_.clear();
    if (publication_) {
        const std::string filter_lower = lower_copy(search_.toStdString());
        const std::string agent_name = agent_filter_.toStdString();
        const auto* agent = agent_name.empty() ? nullptr : aida::agent::get(agent_name);
        rows_.reserve(publication_->skills.size());
        for (const auto& meta : publication_->skills) {
            if (classify_skill(meta) != tab_)
                continue;
            if (!agent_name.empty()) {
                if (publication_->disabled.count(meta.name) > 0)
                    continue;
                if (!meta.agent_slugs.empty() &&
                    std::find(meta.agent_slugs.begin(), meta.agent_slugs.end(),
                        agent_name) == meta.agent_slugs.end())
                    continue;
                if (agent != nullptr &&
                    (aida::agent::evaluate_ruleset(agent->permissions, "skill", meta.name) ==
                        aida::agent::permission_rule_t::action_t::deny ||
                     aida::agent::evaluate_ruleset(agent->permissions, "skill_path",
                        meta.file_path) ==
                        aida::agent::permission_rule_t::action_t::deny))
                    continue;
            }
            if (!filter_lower.empty()) {
                const std::string name_lower = lower_copy(meta.name);
                const std::string desc_lower = lower_copy(meta.description);
                if (name_lower.find(filter_lower) == std::string::npos &&
                    desc_lower.find(filter_lower) == std::string::npos)
                    continue;
            }
            rows_.push_back(meta);
        }
    }
    endResetModel();
}

const aida::skills::skill_metadata_t* AidaSkillListModel::skillAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
}

bool AidaSkillListModel::enabledAt(int row) const {
    const auto* meta = skillAt(row);
    if (meta == nullptr)
        return false;
    if (!publication_)
        return true;
    return publication_->disabled.count(meta->name) == 0;
}

AidaSkillListDelegate::AidaSkillListDelegate(AidaSkillListModel* model, QObject* parent)
    : QStyledItemDelegate(parent), model_(model) {}

QRect AidaSkillListDelegate::toggleRect(const QRect& row) const {
    const auto& t = theme::tokens();
    const int w = t.control.icon_button + t.control.checkbox;
    const int h = t.control.checkbox + t.spacing.xs;
    return QRect(row.right() - t.spacing.sm - w, row.center().y() - h / 2, w, h);
}

void AidaSkillListDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const {
    const auto* meta = model_->skillAt(index.row());
    if (meta == nullptr)
        return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto& t = theme::tokens();
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
    painter->setPen(QPen(selected ? t.accent : t.border_subtle, selected ? 1.5 : 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(card, t.radius.lg, t.radius.lg);

    const int avatar_side = t.control.icon_button + t.spacing.xs;
    const QRect avatar_rect(card.left() + t.spacing.sm + t.spacing.xxs,
                            card.center().y() - avatar_side / 2,
                            avatar_side, avatar_side);
    painter->setBrush(source_color(*meta));
    painter->setPen(Qt::NoPen);
    painter->drawEllipse(avatar_rect);
    painter->setPen(widgets::with_alpha(t.text_primary, 0.90));
    painter->setFont(theme::fonts::ui(700, t.control.icon_glyph));
    const QString name = QString::fromStdString(meta->name);
    painter->drawText(avatar_rect, Qt::AlignCenter,
        name.isEmpty() ? QStringLiteral("?") : name.left(1).toUpper());

    const int text_x = avatar_rect.right() + t.spacing.sm + t.spacing.xxs;
    const int text_right = toggleRect(card).left() - t.spacing.sm;
    const int name_h = t.spacing.lg + t.spacing.xxs;
    painter->setFont(theme::fonts::strong());
    painter->setPen(t.text_primary);
    const QRect name_rect(text_x, card.top() + t.spacing.sm, text_right - text_x, name_h);
    const QFontMetricsF name_fm(theme::fonts::strong());
    painter->drawText(name_rect, Qt::AlignLeft | Qt::AlignVCenter,
        name_fm.elidedText(name, Qt::ElideRight, name_rect.width()));

    const QFont caption_font = theme::fonts::caption();
    const QFontMetricsF caption_fm(caption_font);
    painter->setFont(caption_font);
    painter->setPen(t.text_dim);
    const QRect path_rect(text_x, card.top() + t.spacing.sm + name_h + t.spacing.xxs,
                          text_right - text_x, t.spacing.lg);
    painter->drawText(path_rect, Qt::AlignLeft | Qt::AlignVCenter,
        caption_fm.elidedText(QString::fromStdString(meta->file_path),
            Qt::ElideMiddle, path_rect.width()));

    const QString source_text = source_label(*meta);
    const int pill_h = t.spacing.lg;
    const int pill_w = qRound(caption_fm.horizontalAdvance(source_text)) + t.spacing.md * 2;
    const QRect pill_rect(text_x,
                          card.top() + t.spacing.sm + name_h + t.spacing.xxs +
                              t.spacing.lg + t.spacing.xs,
                          pill_w, pill_h);
    painter->setPen(Qt::NoPen);
    painter->setBrush(widgets::with_alpha(source_color(*meta), 0.82));
    painter->drawRoundedRect(pill_rect, pill_h * 0.5, pill_h * 0.5);
    painter->setPen(widgets::with_alpha(t.text_primary, 0.94));
    painter->drawText(pill_rect, Qt::AlignCenter, source_text);

    const QRect toggle = toggleRect(card);
    const bool enabled = model_->enabledAt(index.row());
    painter->setPen(Qt::NoPen);
    painter->setBrush(enabled ? t.success : t.panel_header);
    painter->drawRoundedRect(toggle, toggle.height() * 0.5, toggle.height() * 0.5);
    painter->setPen(QPen(t.border_subtle, 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(toggle, toggle.height() * 0.5, toggle.height() * 0.5);
    const int knob_d = t.control.icon_glyph;
    const QRect knob(enabled ? toggle.right() - knob_d - t.radius.xs
                             : toggle.left() + t.radius.xs,
        toggle.center().y() - knob_d / 2, knob_d, knob_d);
    painter->setPen(Qt::NoPen);
    painter->setBrush(enabled ? t.bg_base : t.text_dim);
    painter->drawEllipse(knob);
    painter->restore();
}

QSize AidaSkillListDelegate::sizeHint(const QStyleOptionViewItem& option,
                                      const QModelIndex& index) const {
    Q_UNUSED(option);
    Q_UNUSED(index);
    const auto& t = theme::tokens();
    return QSize(t.row.property_label_w + t.control.icon_button,
                 t.spacing.sm * 2 + (t.spacing.lg + t.spacing.xxs) + t.spacing.xxs +
                 t.spacing.lg + t.spacing.xs + t.spacing.lg);
}

bool AidaSkillListDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                        const QStyleOptionViewItem& option,
                                        const QModelIndex& index) {
    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const auto& t = theme::tokens();
        const QRect card = option.rect.adjusted(t.spacing.xxs, t.spacing.xxs,
                                                -t.spacing.xxs, -t.spacing.xxs);
        if (mouse->button() == Qt::LeftButton &&
            toggleRect(card).contains(mouse->pos())) {
            const auto* meta = model_->skillAt(index.row());
            if (meta != nullptr) {
                Q_EMIT enableToggled(QString::fromStdString(meta->name),
                    !model_->enabledAt(index.row()));
                return true;
            }
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

AidaSkillManagerView::AidaSkillManagerView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.ai.skills"));
    buildUi();
}

AidaSkillManagerView::~AidaSkillManagerView() = default;

void AidaSkillManagerView::buildUi() {
    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.spacing.sm + t.spacing.xxs, t.spacing.sm,
                             t.spacing.sm + t.spacing.xxs, t.spacing.sm);
    root->setSpacing(t.spacing.xs + t.spacing.xxs);

    auto* header = new QHBoxLayout();
    auto* title_block = new QVBoxLayout();
    title_block->setSpacing(t.spacing.xxs);
    auto* title = new QLabel(QStringLiteral("Skills"), this);
    title->setFont(theme::fonts::h1());
    auto* subtitle = new QLabel(QStringLiteral(
        "Discover, filter, inspect, and manage reusable analysis workflows."), this);
    subtitle->setFont(theme::fonts::caption());
    subtitle->setProperty("aidaVariant", QStringLiteral("secondary"));
    status_label_ = new QLabel(this);
    status_label_->setFont(theme::fonts::caption());
    status_label_->setProperty("aidaVariant", QStringLiteral("warning"));
    status_label_->setVisible(false);
    title_block->addWidget(title);
    title_block->addWidget(subtitle);
    title_block->addWidget(status_label_);
    header->addLayout(title_block, 1);
    root->addLayout(header);

    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(t.spacing.sm);
    search_edit_ = new QLineEdit(this);
    search_edit_->setObjectName(QStringLiteral("aida.ai.skills.search"));
    search_edit_->setPlaceholderText(QStringLiteral("Search skills"));
    search_edit_->setClearButtonEnabled(true);
    agent_combo_ = new QComboBox(this);
    agent_combo_->setObjectName(QStringLiteral("aida.ai.skills.agent_filter"));
    agent_combo_->setToolTip(QStringLiteral("Show only skills available to this agent"));
    agent_combo_->addItem(QStringLiteral("All agents"), QString());
    const auto primaries = aida::agent::primary_agents();
    for (const auto* agent : primaries) {
        if (agent != nullptr && !agent->hidden)
            agent_combo_->addItem(QString::fromStdString(agent->name),
                QString::fromStdString(agent->name));
    }
    refresh_button_ = new QPushButton(QStringLiteral("Refresh"), this);
    refresh_button_->setToolTip(QStringLiteral("Re-scan skill sources"));
    url_edit_ = new QLineEdit(this);
    url_edit_->setObjectName(QStringLiteral("aida.ai.skills.url"));
    url_edit_->setPlaceholderText(QStringLiteral("https://remote-skills-index.json"));
    add_url_button_ = new QPushButton(QStringLiteral("Add URL"), this);
    add_url_button_->setToolTip(QStringLiteral("Register a remote skill index URL"));
    toolbar->addWidget(search_edit_, 3);
    toolbar->addWidget(agent_combo_, 2);
    toolbar->addWidget(refresh_button_);
    toolbar->addWidget(url_edit_, 4);
    toolbar->addWidget(add_url_button_);
    root->addLayout(toolbar);

    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("aida.ai.skills.tabs"));
    tabs_->addTab(QStringLiteral("Built-in"));
    tabs_->addTab(QStringLiteral("Project"));
    tabs_->addTab(QStringLiteral("Remote"));
    tabs_->setExpanding(false);
    root->addWidget(tabs_);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter_, 1);

    auto* left = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(t.spacing.xs);
    model_ = new AidaSkillListModel(this);
    delegate_ = new AidaSkillListDelegate(model_, this);
    list_ = new QListView(left);
    list_->setObjectName(QStringLiteral("aida.ai.skills.list"));
    list_->setModel(model_);
    list_->setItemDelegate(delegate_);
    list_->setUniformItemSizes(false);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setMouseTracking(true);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    left_layout->addWidget(list_, 1);

    remote_panel_ = new QWidget(left);
    remote_layout_ = new QVBoxLayout(remote_panel_);
    remote_layout_->setContentsMargins(t.spacing.xs, t.spacing.xs,
                                       t.spacing.xs, t.spacing.xs);
    remote_layout_->setSpacing(t.spacing.xs);
    remote_panel_->setVisible(false);
    left_layout->addWidget(remote_panel_);
    splitter_->addWidget(left);

    auto* detail = new QWidget(this);
    auto* detail_layout = new QVBoxLayout(detail);
    detail_layout->setContentsMargins(t.panel.padding, t.panel.padding,
                                      t.panel.padding, t.panel.padding);
    detail_layout->setSpacing(t.spacing.xs + t.spacing.xxs);
    detail_title_ = new QLabel(detail);
    detail_title_->setFont(theme::fonts::h2());
    detail_source_ = new QLabel(detail);
    detail_source_->setFont(theme::fonts::caption());
    detail_source_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detail_description_ = new QLabel(detail);
    detail_description_->setWordWrap(true);
    detail_path_ = new QLabel(detail);
    detail_path_->setFont(theme::fonts::caption());
    detail_path_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detail_path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* detail_buttons = new QHBoxLayout();
    reveal_button_ = new QPushButton(QStringLiteral("Reveal"), detail);
    reveal_button_->setToolTip(QStringLiteral("Reveal the skill file in Explorer"));
    open_file_button_ = new QPushButton(QStringLiteral("Open file"), detail);
    open_file_button_->setToolTip(QStringLiteral("Open the skill file with the system handler"));
    enable_button_ = new QPushButton(QStringLiteral("Disable"), detail);
    enable_button_->setToolTip(QStringLiteral(
        "Enable or disable this skill for agent injection"));
    detail_buttons->addWidget(reveal_button_);
    detail_buttons->addWidget(open_file_button_);
    detail_buttons->addWidget(enable_button_);
    detail_buttons->addStretch(1);
    detail_agents_ = new QLabel(detail);
    detail_agents_->setWordWrap(true);
    detail_hints_ = new QLabel(detail);
    detail_hints_->setWordWrap(true);
    detail_hints_->setFont(theme::fonts::caption());
    detail_layout->addWidget(detail_title_);
    detail_layout->addWidget(detail_source_);
    detail_layout->addWidget(detail_description_);
    detail_layout->addWidget(detail_path_);
    detail_layout->addLayout(detail_buttons);
    detail_layout->addWidget(detail_agents_);
    detail_layout->addWidget(detail_hints_);
    detail_layout->addStretch(1);
    splitter_->addWidget(detail);

    auto* preview_host = new QWidget(this);
    auto* preview_layout = new QVBoxLayout(preview_host);
    preview_layout->setContentsMargins(0, 0, 0, 0);
    preview_layout->setSpacing(t.spacing.xs);
    render_toggle_ = new QPushButton(QStringLiteral("Render"), preview_host);
    render_toggle_->setObjectName(QStringLiteral("aida.ai.skills.render_toggle"));
    render_toggle_->setCheckable(true);
    render_toggle_->setChecked(true);
    render_toggle_->setToolTip(QStringLiteral(
        "Render the skill instructions as markdown, or show the raw text"));
    preview_ = new QTextBrowser(preview_host);
    preview_->setObjectName(QStringLiteral("aida.ai.skills.preview"));
    preview_->setOpenLinks(false);
    preview_layout->addWidget(render_toggle_, 0, Qt::AlignRight);
    preview_layout->addWidget(preview_, 1);
    splitter_->addWidget(preview_host);
    splitter_->setStretchFactor(0, 2);
    splitter_->setStretchFactor(1, 3);
    splitter_->setStretchFactor(2, 3);

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(500);

    connect(search_edit_, &QLineEdit::textChanged, model_, &AidaSkillListModel::setSearch);
    connect(agent_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        model_->setAgentFilter(agent_combo_->currentData().toString());
    });
    connect(refresh_button_, &QPushButton::clicked, this, [this] {
        std::string error;
        serviceRequest(aida::skill_manager_service::request_reindex(&error), error);
    });
    connect(add_url_button_, &QPushButton::clicked, this, [this] {
        const std::string url = url_edit_->text().trimmed().toStdString();
        if (url.empty())
            return;
        std::string error;
        if (serviceRequest(aida::skill_manager_service::request_add_remote_url(url, &error),
                error)) {
            url_edit_->clear();
            tabs_->setCurrentIndex(2);
        }
    });
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        model_->setTab(index);
        remote_panel_->setVisible(index == 2);
        refreshRemotePanel();
    });
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current, const QModelIndex&) {
        const auto* meta = current.isValid() ? model_->skillAt(current.row()) : nullptr;
        selected_skill_name_ = meta != nullptr ? meta->name : std::string();
        refreshDetail();
        refreshPreview();
        if (meta != nullptr)
            onResolveRequested(QString::fromStdString(meta->name));
    });
    connect(list_, &QListView::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = list_->indexAt(pos);
        if (index.isValid())
            openContextMenu(index, list_->viewport()->mapToGlobal(pos));
    });
    connect(delegate_, &AidaSkillListDelegate::enableToggled, this,
            [this](const QString& name, bool enabled) {
        const auto publication = aida::skill_manager_service::snapshot();
        const bool operation_pending = publication && publication->state ==
            aida::skill_manager_service::operation_state_t::loading;
        if (operation_pending)
            return;
        std::string error;
        serviceRequest(aida::skill_manager_service::request_set_enabled(
            name.toStdString(), enabled, &error), error);
    });
    connect(reveal_button_, &QPushButton::clicked, this, [this] {
        const auto publication = aida::skill_manager_service::snapshot();
        if (const auto* meta = aida::skill_manager_service::find(publication,
                selected_skill_name_))
            open_path_in_shell(meta->file_path, true);
    });
    connect(open_file_button_, &QPushButton::clicked, this, [this] {
        const auto publication = aida::skill_manager_service::snapshot();
        if (const auto* meta = aida::skill_manager_service::find(publication,
                selected_skill_name_))
            open_path_in_shell(meta->file_path, false);
    });
    connect(enable_button_, &QPushButton::clicked, this, [this] {
        const auto publication = aida::skill_manager_service::snapshot();
        const bool operation_pending = publication && publication->state ==
            aida::skill_manager_service::operation_state_t::loading;
        if (operation_pending)
            return;
        const auto* meta = aida::skill_manager_service::find(publication,
            selected_skill_name_);
        if (meta == nullptr)
            return;
        const bool enabled = !publication || publication->disabled.count(meta->name) == 0;
        std::string error;
        serviceRequest(aida::skill_manager_service::request_set_enabled(
            meta->name, !enabled, &error), error);
    });
    connect(render_toggle_, &QPushButton::toggled, this, [this](bool checked) {
        preview_rendered_ = checked;
        refreshPreview();
    });
    connect(preview_, &QTextBrowser::anchorClicked, this, [](const QUrl& url) {
        if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
            QDesktopServices::openUrl(url);
    });
    connect(poll_timer_, &QTimer::timeout, this, &AidaSkillManagerView::pollService);

    aida::skill_manager_service::begin_frame();
    applyPublication(aida::skill_manager_service::snapshot());
    refreshDetail();
    refreshPreview();
}

void AidaSkillManagerView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    pollService();
    if (!poll_timer_->isActive())
        poll_timer_->start();
}

void AidaSkillManagerView::hideEvent(QHideEvent* event) {
    poll_timer_->stop();
    QWidget::hideEvent(event);
}

bool AidaSkillManagerView::serviceRequest(bool accepted, const std::string& error) {
    if (accepted)
        return true;
    chrome::toast_error(QString::fromStdString(
        error.empty() ? "Skills operation was rejected" : error), 5.0);
    return false;
}

void AidaSkillManagerView::pollService() {
    aida::skill_manager_service::begin_frame();
    applyPublication(aida::skill_manager_service::snapshot());
}

void AidaSkillManagerView::applyPublication(
    const aida::skill_manager_service::snapshot_ptr& publication) {
    if (!publication || publication->generation == observed_service_generation_)
        return;
    observed_service_generation_ = publication->generation;
    const bool operation_pending = publication->state ==
        aida::skill_manager_service::operation_state_t::loading;
    refresh_button_->setEnabled(!operation_pending);
    add_url_button_->setEnabled(!operation_pending);
    if (operation_pending) {
        status_label_->setText(QString::fromStdString(publication->operation));
        status_label_->setVisible(true);
    }
    if (publication->state == aida::skill_manager_service::operation_state_t::failed) {
        requested_resolve_name_.clear();
        last_error_ = publication->detail;
        status_label_->setText(QString::fromStdString(last_error_));
        status_label_->setVisible(true);
        chrome::toast_error(QString::fromStdString(publication->detail), 5.0);
    } else if (publication->state == aida::skill_manager_service::operation_state_t::succeeded) {
        last_error_.clear();
        if (!operation_pending)
            status_label_->setVisible(false);
        if (!publication->operation.empty())
            chrome::toast_info(QString::fromStdString(publication->detail), 3.5);
    }
    if (!selected_skill_name_.empty() &&
        !aida::skill_manager_service::find(publication, selected_skill_name_)) {
        selected_skill_name_.clear();
        refreshDetail();
        refreshPreview();
    }
    if (!selected_skill_name_.empty() &&
        publication->resolved_name != selected_skill_name_ && !operation_pending &&
        requested_resolve_name_ != selected_skill_name_) {
        std::string error;
        const bool accepted = aida::skill_manager_service::request_resolve(
            selected_skill_name_, &error);
        if (serviceRequest(accepted, error))
            requested_resolve_name_ = selected_skill_name_;
    }
    if (publication->resolved_name == requested_resolve_name_)
        requested_resolve_name_.clear();
    model_->reloadFrom(publication);
    refreshRemotePanel();
    refreshDetail();
    refreshPreview();
}

void AidaSkillManagerView::onResolveRequested(const QString& name) {
    const auto publication = aida::skill_manager_service::snapshot();
    if (!publication || publication->resolved_name == name.toStdString())
        return;
    std::string error;
    const bool accepted = aida::skill_manager_service::request_resolve(
        name.toStdString(), &error);
    if (serviceRequest(accepted, error))
        requested_resolve_name_ = name.toStdString();
}

void AidaSkillManagerView::refreshDetail() {
    const auto publication = aida::skill_manager_service::snapshot();
    const auto* meta = selected_skill_name_.empty()
        ? nullptr : aida::skill_manager_service::find(publication, selected_skill_name_);
    const bool has = meta != nullptr;
    detail_title_->setText(has ? QString::fromStdString(meta->name)
                               : QStringLiteral("Pick a skill"));
    detail_source_->setText(has ? source_label(*meta) : QString());
    detail_description_->setText(has ? QString::fromStdString(meta->description)
        : QStringLiteral(
            "Select a skill from the list to view its metadata, agent slugs and placeholder hints."));
    detail_path_->setText(has && !meta->file_path.empty()
        ? QStringLiteral("Path: %1").arg(QString::fromStdString(meta->file_path))
        : QString());
    reveal_button_->setEnabled(has && !meta->file_path.empty());
    open_file_button_->setEnabled(has && !meta->file_path.empty());
    const bool enabled = has && (!publication || publication->disabled.count(meta->name) == 0);
    enable_button_->setEnabled(has);
    enable_button_->setText(enabled ? QStringLiteral("Disable") : QStringLiteral("Enable"));
    if (has) {
        QString agents;
        if (meta->agent_slugs.empty()) {
            agents = QStringLiteral("Agent slugs: (none, available to all primary agents)");
        } else {
            QStringList slugs;
            for (const auto& slug : meta->agent_slugs)
                slugs << QString::fromStdString(slug);
            agents = QStringLiteral("Agent slugs: %1").arg(slugs.join(QStringLiteral(", ")));
        }
        detail_agents_->setText(agents);
        QStringList hints;
        if (publication && publication->resolved_name == meta->name) {
            for (const auto& hint : publication->resolved_hints)
                hints << QString::fromStdString(hint);
        }
        detail_hints_->setText(hints.isEmpty()
            ? QString()
            : QStringLiteral("Arguments: %1").arg(hints.join(QStringLiteral(", "))));
    } else {
        detail_agents_->clear();
        detail_hints_->clear();
    }
}

void AidaSkillManagerView::refreshPreview() {
    const auto publication = aida::skill_manager_service::snapshot();
    QString body;
    if (publication && !selected_skill_name_.empty() &&
        publication->resolved_name == selected_skill_name_)
        body = QString::fromStdString(publication->resolved_body);
    if (body.isEmpty()) {
        preview_->setPlainText(QString());
        return;
    }
    if (!preview_rendered_) {
        preview_->setFont(theme::fonts::codeRegular());
        preview_->setPlainText(body);
        return;
    }
    preview_->setFont(theme::fonts::body());
    const std::string text = body.toStdString();
    auto spans = chat_render::parse_markdown(text);
    auto document = AidaMarkdownDocumentBuilder::build(spans, text.size(), nullptr,
        nullptr, nullptr);
    if (document) {
        auto* owned = document.release();
        owned->setParent(preview_);
        preview_->setDocument(owned);
    }
}

void AidaSkillManagerView::refreshRemotePanel() {
    while (QLayoutItem* item = remote_layout_->takeAt(0)) {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    if (tabs_->currentIndex() != 2)
        return;
    const auto publication = aida::skill_manager_service::snapshot();
    const bool operation_pending = publication && publication->state ==
        aida::skill_manager_service::operation_state_t::loading;
    auto* title = new QLabel(QStringLiteral("Remote sources"), remote_panel_);
    title->setFont(theme::fonts::bodyEm());
    remote_layout_->addWidget(title);
    if (!publication || publication->remote_urls.empty()) {
        auto* empty = new QLabel(QStringLiteral(
            "No remote URLs registered. Use the toolbar above to add one."), remote_panel_);
        empty->setFont(theme::fonts::caption());
        empty->setProperty("aidaVariant", QStringLiteral("secondary"));
        empty->setWordWrap(true);
        remote_layout_->addWidget(empty);
        return;
    }
    for (const auto& url : publication->remote_urls) {
        auto* row_host = new QWidget(remote_panel_);
        auto* row_layout = new QVBoxLayout(row_host);
        row_layout->setContentsMargins(theme::tokens().spacing.xs, theme::tokens().spacing.xs,
                                       theme::tokens().spacing.xs, theme::tokens().spacing.xs);
        row_layout->setSpacing(theme::tokens().spacing.xxs);
        auto* url_row = new QHBoxLayout();
        auto* url_label = new QLabel(QString::fromStdString(url), row_host);
        url_label->setFont(theme::fonts::caption());
        url_label->setToolTip(QString::fromStdString(url));
        url_row->addWidget(url_label, 1);
        auto* fetch = new QPushButton(QStringLiteral("Fetch"), row_host);
        fetch->setEnabled(!operation_pending);
        auto* remove = new QPushButton(QStringLiteral("Remove"), row_host);
        remove->setEnabled(!operation_pending);
        url_row->addWidget(fetch);
        url_row->addWidget(remove);
        row_layout->addLayout(url_row);
        connect(fetch, &QPushButton::clicked, this, [this, url] {
            std::string error;
            serviceRequest(aida::skill_manager_service::request_fetch_remote(url, &error),
                error);
        });
        connect(remove, &QPushButton::clicked, this, [this, url] {
            std::string error;
            serviceRequest(aida::skill_manager_service::request_remove_remote_url(url, &error),
                error);
        });
        const auto index_it = publication->remote_indices.find(url);
        if (index_it != publication->remote_indices.end()) {
            for (const auto& entry : index_it->second.entries) {
                auto* entry_row = new QHBoxLayout();
                auto* name_label = new QLabel(QString::fromStdString(entry.name), row_host);
                name_label->setFont(theme::fonts::caption());
                entry_row->addWidget(name_label, 1);
                const bool installing = operation_pending &&
                    publication->operation == "Install remote skill";
                if (installing) {
                    auto* busy = new QLabel(QStringLiteral("Installing..."), row_host);
                    busy->setFont(theme::fonts::caption());
                    entry_row->addWidget(busy);
                } else {
                    auto* install = new QPushButton(QStringLiteral("Install"), row_host);
                    install->setEnabled(!operation_pending);
                    entry_row->addWidget(install);
                    connect(install, &QPushButton::clicked, this, [this, url,
                            name = entry.name] {
                        std::string error;
                        serviceRequest(aida::skill_manager_service::request_install(
                            url, name, &error), error);
                    });
                }
                row_layout->addLayout(entry_row);
            }
        }
        remote_layout_->addWidget(row_host);
    }
}

void AidaSkillManagerView::openContextMenu(const QModelIndex& index, const QPoint& global_pos) {
    const auto publication = aida::skill_manager_service::snapshot();
    const auto* meta = model_->skillAt(index.row());
    if (meta == nullptr)
        return;
    const bool operation_pending = publication && publication->state ==
        aida::skill_manager_service::operation_state_t::loading;
    const std::string retained_name = meta->name;
    const std::string retained_path = meta->file_path;
    const std::string retained_source = meta->source;
    const std::uint64_t retained_generation = publication ? publication->generation : 0;
    const bool enabled = !publication || publication->disabled.count(retained_name) == 0;

    auto* menu = new QMenu(this);
    auto* copy_name = menu->addAction(QStringLiteral("Copy name"));
    auto* copy_path = menu->addAction(QStringLiteral("Copy path"));
    copy_path->setEnabled(!retained_path.empty());
    auto* open_file = menu->addAction(QStringLiteral("Open file"));
    open_file->setEnabled(!retained_path.empty());
    auto* reload = menu->addAction(QStringLiteral("Reload catalog"));
    reload->setEnabled(!operation_pending);
    auto* toggle_enabled = menu->addAction(enabled ? QStringLiteral("Disable")
                                                   : QStringLiteral("Enable"));
    toggle_enabled->setEnabled(!operation_pending);
    auto* uninstall = menu->addAction(QStringLiteral("Uninstall..."));
    uninstall->setEnabled(retained_source == "remote" && !operation_pending);
    if (retained_source != "remote")
        uninstall->setToolTip(QStringLiteral(
            "Built-in and local skills are source-managed; only installed remote skills can be uninstalled here"));

    const auto validate = [retained_name, retained_path, retained_generation]() -> bool {
        const auto live = aida::skill_manager_service::snapshot();
        const auto* current = aida::skill_manager_service::find(live, retained_name);
        return live && live->generation == retained_generation && current != nullptr &&
            current->file_path == retained_path;
    };
    connect(copy_name, &QAction::triggered, this, [validate, retained_name] {
        if (!validate())
            return;
        clipboard::set_text(QString::fromStdString(retained_name));
    });
    connect(copy_path, &QAction::triggered, this, [validate, retained_path] {
        if (!validate() || retained_path.empty())
            return;
        clipboard::set_text(QString::fromStdString(retained_path));
    });
    connect(open_file, &QAction::triggered, this, [validate, retained_path] {
        if (!validate() || retained_path.empty())
            return;
        open_path_in_shell(retained_path, true);
    });
    connect(reload, &QAction::triggered, this, [this, validate] {
        if (!validate())
            return;
        std::string error;
        serviceRequest(aida::skill_manager_service::request_reindex(&error), error);
    });
    connect(toggle_enabled, &QAction::triggered, this, [this, validate, retained_name,
                                                        enabled] {
        if (!validate())
            return;
        std::string error;
        serviceRequest(aida::skill_manager_service::request_set_enabled(
            retained_name, !enabled, &error), error);
    });
    connect(uninstall, &QAction::triggered, this, [this, validate, retained_name,
                                                   retained_generation] {
        if (!validate()) {
            chrome::toast_warning(QStringLiteral(
                "The skill catalog changed; select the skill again"), 4.0);
            return;
        }
        onUninstallRequested(QString::fromStdString(retained_name), retained_generation);
    });
    menu->popup(global_pos);
}

void AidaSkillManagerView::onUninstallRequested(const QString& name, quint64 generation) {
    aida_confirm_request_t request;
    request.verb = QStringLiteral("Uninstall");
    request.target = name;
    request.scope = QStringLiteral(
        "The installed remote skill and its local managed files");
    request.effect = QStringLiteral(
        "Removes this remote skill from AiDA and makes it unavailable to agents.");
    request.reversibility = QStringLiteral(
        "Reinstall the skill from its remote source to recover it.");
    request.prerequisite = QString();
    request.confirm_label = QStringLiteral("Uninstall Skill");
    request.destructive = true;
    request.confirm_enabled = true;
    AidaConfirmDialog::request(request, this, [this, name, generation] {
        const auto publication = aida::skill_manager_service::snapshot();
        const auto name_std = name.toStdString();
        const bool still_available =
            aida::skill_manager_service::find(publication, name_std) != nullptr;
        if (!still_available) {
            chrome::toast_warning(QStringLiteral(
                "The selected remote skill is no longer installed; refresh the skill catalog."),
                4.0);
            return;
        }
        std::string error;
        if (serviceRequest(aida::skill_manager_service::request_uninstall(
                name_std, generation, &error), error)) {
            if (selected_skill_name_ == name_std) {
                selected_skill_name_.clear();
                refreshDetail();
                refreshPreview();
            }
        }
    });
}

}
