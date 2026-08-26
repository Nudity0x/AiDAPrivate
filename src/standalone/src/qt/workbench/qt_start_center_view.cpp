#include "qt/workbench/qt_start_center_view.hpp"

#include <QFontMetrics>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStyle>
#include <QStyleOptionButton>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/session/analysis_session.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/application_ui_runtime.hpp"
#include "core/ui/shell_host_contract.hpp"

#include "qt/analysis/qt_analysis_bridge.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/explorer/exchange/open_dispatch.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_headers.hpp"

namespace aida::qt::workbench {

namespace {

QString path_leaf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return QString::fromStdString(
        pos == std::string::npos ? path : path.substr(pos + 1));
}

QString escape_mnemonics(QString text) {
    return text.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

class StartCenterCardButton : public QPushButton {
public:
    StartCenterCardButton(QString primary, QString secondary, QString tooltip,
                          QWidget* parent = nullptr)
        : QPushButton(parent), primary_(std::move(primary)),
          secondary_(std::move(secondary)) {
        setProperty("aidaRole", QStringLiteral("cardButton"));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAutoDefault(true);
        setDefault(false);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setToolTip(tooltip.isEmpty() ? secondary_ : std::move(tooltip));
        setToolTipDuration(-1);
    }

    QSize sizeHint() const override {
        const auto& t = theme::tokens();
        const QFontMetrics metrics(font());
        const int height = 2 * metrics.lineSpacing() + 2 * t.spacing.sm +
            2 * t.panel.border;
        return QSize(2 * static_cast<int>(t.shell.min_panel_w), height);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const auto& t = theme::tokens();
        const int avail = width() - 2 * t.spacing.md - 2 * t.panel.border;
        if (avail != elided_avail_) {
            elided_avail_ = avail;
            const QFontMetrics metrics(font());
            elided_primary_ = metrics.elidedText(primary_, Qt::ElideRight,
                (std::max)(0, avail));
            elided_secondary_ = metrics.elidedText(secondary_, Qt::ElideMiddle,
                (std::max)(0, avail));
        }
        QStyleOptionButton option;
        initStyleOption(&option);
        option.text = elided_primary_ + QLatin1Char('\n') + elided_secondary_;
        QPainter painter(this);
        style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
    }

private:
    QString primary_;
    QString secondary_;
    QString elided_primary_;
    QString elided_secondary_;
    int elided_avail_ = -1;
};

class StartCenterGrid : public QWidget {
public:
    explicit StartCenterGrid(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("aida.start_center.continue.grid"));
        const auto& t = theme::tokens();
        min_card_w_ = 2 * static_cast<int>(t.shell.min_panel_w);
        gap_ = t.spacing.sm;
        grid_ = new QGridLayout(this);
        grid_->setContentsMargins(0, 0, 0, 0);
        grid_->setSpacing(gap_);
    }

    void addCard(QWidget* card) {
        cards_.push_back(card);
        relayout(columnsForWidth((std::max)(width(), 1)));
    }

    int cardCount() const { return static_cast<int>(cards_.size()); }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        relayout(columnsForWidth(event->size().width()));
    }

private:
    int columnsForWidth(int available) const {
        const int pitch = min_card_w_ + gap_;
        return (std::max)(1, (available + gap_) / (std::max)(1, pitch));
    }

    void relayout(int columns) {
        if (columns == columns_ && grid_->count() == static_cast<int>(cards_.size()))
            return;
        columns_ = columns;
        while (auto* item = grid_->takeAt(0))
            delete item;
        for (int column = 0; column < grid_->columnCount(); ++column)
            grid_->setColumnStretch(column, 0);
        for (int column = 0; column < columns_; ++column)
            grid_->setColumnStretch(column, 1);
        for (int index = 0; index < static_cast<int>(cards_.size()); ++index)
            grid_->addWidget(cards_[static_cast<std::size_t>(index)],
                index / columns_, index % columns_);
    }

    QGridLayout* grid_ = nullptr;
    std::vector<QWidget*> cards_;
    int columns_ = 0;
    int min_card_w_ = 0;
    int gap_ = 0;
};

QPushButton* make_list_action(QWidget* parent, QString instance,
                              const QString& label, const QString& tooltip) {
    auto* button = new QPushButton(label, parent);
    button->setObjectName(QStringLiteral("aida.start_center.action.") +
        std::move(instance));
    button->setProperty("aidaRole", QStringLiteral("listAction"));
    button->setToolTip(tooltip);
    button->setToolTipDuration(-1);
    button->setAutoDefault(true);
    button->setDefault(false);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return button;
}

}

QtStartCenterView::QtStartCenterView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.start_center"));
    const auto& t = theme::tokens();
    setMinimumWidth(3 * static_cast<int>(t.shell.min_panel_w) + 2 * t.panel.padding);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("aida.start_center.scroll"));
    scroll->setWidgetResizable(true);
    auto* host = new QWidget(scroll);
    columns_layout_ = new QVBoxLayout(host);
    columns_layout_->setContentsMargins(t.panel.padding, t.panel.padding,
        t.panel.padding, t.panel.padding);
    columns_layout_->setSpacing(t.spacing.md);
    scroll->setWidget(host);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
    rebuild();
}

void QtStartCenterView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    rebuild();
}

void QtStartCenterView::rebuild() {
    QLayoutItem* item = nullptr;
    while ((item = columns_layout_->takeAt(0)) != nullptr) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    auto* header = new QLabel(QStringLiteral("Start Center"), this);
    header->setObjectName(QStringLiteral("aida.start_center.title"));
    header->setFont(theme::fonts::h1());
    columns_layout_->addWidget(header);
    auto* subtitle = new QLabel(QStringLiteral(
        "Open analysis and programming work in one dockable reverse-engineering IDE."),
        this);
    subtitle->setObjectName(QStringLiteral("aida.start_center.subtitle"));
    subtitle->setProperty("aidaVariant", QStringLiteral("secondary"));
    subtitle->setWordWrap(true);
    columns_layout_->addWidget(subtitle);
    columns_layout_->addWidget(buildActionsPanel());
    columns_layout_->addWidget(buildPresetsPanel());
    columns_layout_->addWidget(buildContinuePanel());
    columns_layout_->addWidget(buildRecoveryPanel());
    columns_layout_->addStretch(1);
}

QWidget* QtStartCenterView::buildActionsPanel() {
    const auto& t = theme::tokens();
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(t.spacing.xs);
    auto* header = new widgets::AidaSectionHeader(QStringLiteral("Start"), panel);
    header->setObjectName(QStringLiteral("aida.start_center.header.start"));
    layout->addWidget(header);
    struct action_t {
        const char* id;
        const char* label;
        const char* description;
    };
    static const action_t k_actions[] = {
        {"tools.load_binary", "Open Binary...",
            "Open a binary and create an analysis session"},
        {"file.open_folder", "Open Folder / Source Project...",
            "Open a source or mixed workspace"},
        {"tools.attach_process", "Attach to Process...",
            "Attach to a live process"},
        {"debugger.launch", "Launch Target...",
            "Launch a target under the debugger"},
        {"file.restore_previous_session", "Restore Previous Session",
            "Open the most recent closed binary or analysis session"},
        {"file.new", "New Programming File",
            "Create an untitled code document"},
        {"file.open", "Open Programming File...",
            "Open a source, script, configuration or data file"},
    };
    for (const auto& entry : k_actions) {
        const auto presentation =
            aida::ui::application_ui::present_action(entry.id);
        auto* button = make_list_action(panel, QString::fromLatin1(entry.id),
            QString::fromLatin1(entry.label),
            QString::fromLatin1(entry.description));
        button->setEnabled(presentation.enabled);
        button->setVisible(presentation.visible);
        connect(button, &QPushButton::clicked, this, [id = entry.id] {
            static_cast<void>(aida::ui::application_ui::execute_action(id,
                aida::ui::action_invocation_source_t::toolbar));
            aida::qt::analysis::QtAnalysisBridge::instance().host()
                ->dismiss_start_center_when_work_available();
        });
        layout->addWidget(button);
    }
    return panel;
}

QWidget* QtStartCenterView::buildPresetsPanel() {
    const auto& t = theme::tokens();
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(t.spacing.xs);
    auto* header = new widgets::AidaSectionHeader(QStringLiteral("Workspaces"), panel);
    header->setObjectName(QStringLiteral("aida.start_center.header.workspaces"));
    layout->addWidget(header);
    for (const auto& preset : aida::ui::k_workspace_presets) {
        if (preset.id == aida::ui::workspace_preset_t::safe) continue;
        const std::string action_id =
            std::string("workspace.switch.") + std::string(preset.stable_id);
        auto* button = make_list_action(panel,
            QString::fromUtf8(preset.stable_id.data(),
                static_cast<int>(preset.stable_id.size())),
            QString::fromUtf8(preset.display_name.data(),
                static_cast<int>(preset.display_name.size())),
            QString::fromUtf8(preset.description.data(),
                static_cast<int>(preset.description.size())));
        connect(button, &QPushButton::clicked, this, [action_id] {
            static_cast<void>(aida::ui::application_ui::execute_action(
                action_id.c_str(),
                aida::ui::action_invocation_source_t::toolbar));
        });
        layout->addWidget(button);
    }
    return panel;
}

QWidget* QtStartCenterView::buildContinuePanel() {
    const auto& t = theme::tokens();
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(t.spacing.xs);
    auto* header = new widgets::AidaSectionHeader(QStringLiteral("Continue"), panel);
    header->setObjectName(QStringLiteral("aida.start_center.header.continue"));
    layout->addWidget(header);
    auto* grid = new StartCenterGrid(panel);
    const std::size_t session_count = analysis_session::session_count();
    for (std::size_t index = 0; index < (std::min)(session_count, std::size_t{6});
         ++index) {
        const auto session = analysis_session::session_handle_at(index);
        if (!session) continue;
        const QString name = session->filename.empty()
            ? path_leaf(session->path)
            : QString::fromStdString(session->filename);
        auto* card = new StartCenterCardButton(escape_mnemonics(name),
            escape_mnemonics(QString::fromStdString(session->path)),
            QString::fromStdString(session->path), grid);
        card->setObjectName(QStringLiteral("aida.start_center.card.session.") +
            QString::number(index));
        connect(card, &QPushButton::clicked, this, [index] {
            analysis_session::switch_session(index);
            aida::qt::analysis::QtAnalysisBridge::instance().openView(
                "document.disassembly");
        });
        grid->addCard(card);
    }
    std::vector<std::string> recent_paths;
    bool recent_parse_failed = false;
    if (!g_sa_settings.recent_workspaces_json.empty()) {
        const auto json = nlohmann::json::parse(
            g_sa_settings.recent_workspaces_json, nullptr, false);
        if (!json.is_discarded() && json.is_array()) {
            for (const auto& value : json)
                if (value.is_string())
                    recent_paths.push_back(value.get<std::string>());
        } else {
            recent_parse_failed = true;
        }
    }
    std::size_t closed_count = 0;
    for (const auto& path : recent_paths) {
        bool is_open = false;
        for (std::size_t index = 0; index < session_count; ++index) {
            const auto session = analysis_session::session_handle_at(index);
            if (session && session->path == path) { is_open = true; break; }
        }
        if (is_open) continue;
        auto* card = new StartCenterCardButton(
            escape_mnemonics(path_leaf(path)),
            escape_mnemonics(QString::fromStdString(path)),
            QString::fromStdString(path), grid);
        card->setObjectName(QStringLiteral("aida.start_center.card.recent.") +
            QString::number(closed_count));
        connect(card, &QPushButton::clicked, this, [path] {
            aida::qt::explorer::open_path(path);
        });
        grid->addCard(card);
        if (++closed_count == 8) break;
    }
    if (recent_parse_failed) {
        auto* error = new QLabel(QStringLiteral(
            "The stored recent-workspaces list could not be parsed."), panel);
        error->setObjectName(QStringLiteral("aida.start_center.continue.error"));
        error->setProperty("aidaVariant", QStringLiteral("error"));
        error->setWordWrap(true);
        layout->addWidget(error);
    }
    if (grid->cardCount() == 0) {
        grid->setVisible(false);
        if (!recent_parse_failed) {
            auto* empty = new QLabel(QStringLiteral(
                "No recent work. Use Open Binary or Open Folder to begin."), panel);
            empty->setObjectName(QStringLiteral("aida.start_center.continue.empty"));
            empty->setProperty("aidaVariant", QStringLiteral("secondary"));
            empty->setWordWrap(true);
            layout->addWidget(empty);
        }
    } else {
        layout->addWidget(grid);
    }
    auto* browse = make_list_action(panel, QStringLiteral("browse_recent"),
        QStringLiteral("Browse All Recent Work"),
        QStringLiteral("Open the full Recent view with every remembered workspace"));
    connect(browse, &QPushButton::clicked, this, [] {
        aida::qt::analysis::QtAnalysisBridge::instance().openView("view.recent");
    });
    layout->addWidget(browse);
    return panel;
}

QWidget* QtStartCenterView::buildRecoveryPanel() {
    const auto& t = theme::tokens();
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(t.spacing.xs);
    auto* header = new widgets::AidaSectionHeader(
        QStringLiteral("Recovery and status"), panel);
    header->setObjectName(QStringLiteral("aida.start_center.header.recovery"));
    layout->addWidget(header);
    static const struct { const char* id; const char* label; const char* tip; }
        k_actions[] = {
        {"view.focus.view.background_tasks", "Background Tasks",
            "Review long-running work and exact cancellation capability"},
        {"view.focus.view.diagnostics", "Diagnostics",
            "Review persistent failures, stable diagnostic IDs and recovery actions"},
        {"tools.settings", "Settings",
            "Configure the IDE without leaving the current workspace"},
        {"workspace.safe", "Activate Safe Layout",
            "Recover a broken layout without discarding project or session state"},
    };
    for (const auto& entry : k_actions) {
        auto* button = make_list_action(panel, QString::fromLatin1(entry.id),
            QString::fromLatin1(entry.label), QString::fromLatin1(entry.tip));
        connect(button, &QPushButton::clicked, this, [id = entry.id] {
            static_cast<void>(aida::ui::application_ui::execute_action(id,
                aida::ui::action_invocation_source_t::toolbar));
        });
        layout->addWidget(button);
    }
    return panel;
}

}
