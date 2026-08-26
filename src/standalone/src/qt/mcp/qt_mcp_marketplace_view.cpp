#include "qt/mcp/qt_mcp_marketplace_view.hpp"

#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedLayout>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "core/ai/standalone_chat.hpp"
#include "core/mcp/mcp_marketplace.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/docking/dock_host.hpp"
#include "qt/registry/qt_view_registry.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_icons.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_headers.hpp"
#include "qt/widgets/aida_search_field.hpp"
#include "qt/widgets/aida_state_view.hpp"
#include "qt/widgets/aida_tool_button.hpp"

namespace aida::qt::mcp {

namespace {

std::string lower_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

QString truncate_text(const QString& s, qsizetype max_len) {
    if (s.size() <= max_len)
        return s;
    return s.left(max_len) + QStringLiteral("\u2026");
}

QString format_count(int64_t v) {
    char buf[32];
    if (v >= 1000000)
        std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(v) / 1000000.0);
    else if (v >= 1000)
        std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(v) / 1000.0);
    else
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return QString::fromLatin1(buf);
}

void append_meta_piece(QString& out, const QString& piece) {
    if (piece.isEmpty())
        return;
    if (!out.isEmpty())
        out += QStringLiteral("  ");
    out += piece;
}

bool is_pkg_installed(const std::string& name) {
    const auto installed = ::mcp_marketplace::get_installed();
    for (const auto& s : installed)
        if (s.package_name == name)
            return true;
    return false;
}

bool get_installed_server(const std::string& name,
                          ::mcp_marketplace::installed_server_t& out) {
    const auto installed = ::mcp_marketplace::get_installed();
    for (const auto& srv : installed) {
        if (srv.package_name == name) {
            out = srv;
            return true;
        }
    }
    return false;
}

QString package_source_label(const ::mcp_marketplace::package_info_t& p) {
    if (!p.repository.empty())
        return QString::fromStdString(p.repository);
    if (!p.homepage.empty())
        return QString::fromStdString(p.homepage);
    return QStringLiteral("No repository metadata from registry");
}

QColor package_glyph_color(const QString& name) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto bytes = name.toStdString();
    for (const char c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return QColor::fromHsl(static_cast<int>(hash % 360ULL), 140, 130);
}

}

AidaMcpMarketplaceController::AidaMcpMarketplaceController(QObject* parent)
    : QObject(parent) {}

AidaMcpMarketplaceController& AidaMcpMarketplaceController::instance() {
    static AidaMcpMarketplaceController* controller = [] {
        return new AidaMcpMarketplaceController();
    }();
    return *controller;
}

void AidaMcpMarketplaceController::install() {
    if (installed_)
        return;
    installed_ = true;
    ::mcp_marketplace::set_install_output_hook([](const std::string& line) {
        QMetaObject::invokeMethod(&instance(), [line] {
            Q_EMIT instance().installLogAppended(QString::fromStdString(line));
        }, Qt::QueuedConnection);
    });
    tick_timer_ = new QTimer(this);
    tick_timer_->setInterval(500);
    tick_timer_->setTimerType(Qt::CoarseTimer);
    connect(tick_timer_, &QTimer::timeout, this, [this] {
        ::mcp_marketplace::tick();
        refreshViewState();
    });
    tick_timer_->start();
    diag::log_tagged("qt_mcp_market", "marketplace_tick_wired interval_ms=500");
}

void AidaMcpMarketplaceController::shutdown() {
    if (tick_timer_)
        tick_timer_->stop();
    ::mcp_marketplace::set_install_output_hook(nullptr);
    ::mcp_marketplace::shutdown();
    installed_ = false;
}

void AidaMcpMarketplaceController::refreshViewState() {
    const auto search_state = ::mcp_marketplace::get_search_state();
    if (search_state == ::mcp_marketplace::search_state_t::done &&
        last_search_state_ != ::mcp_marketplace::search_state_t::done) {
        Q_EMIT searchResultsReady();
    }
    if (search_state == ::mcp_marketplace::search_state_t::error_state) {
        const std::string error = ::mcp_marketplace::get_search_error();
        if (!error.empty() && error != last_search_error_) {
            last_search_error_ = error;
            Q_EMIT searchFailed(QString::fromStdString(error));
        }
    }
    last_search_state_ = search_state;

    const auto install_state = ::mcp_marketplace::get_install_state();
    if (install_state != last_install_state_) {
        const bool finished =
            last_install_state_ == ::mcp_marketplace::install_state_t::installing &&
            (install_state == ::mcp_marketplace::install_state_t::done ||
             install_state == ::mcp_marketplace::install_state_t::error_state);
        last_install_state_ = install_state;
        if (finished) {
            const bool success = install_state == ::mcp_marketplace::install_state_t::done;
            const std::string error = success
                ? std::string() : ::mcp_marketplace::get_install_error();
            Q_EMIT installFinished(success, QString(),
                QString::fromStdString(error));
            Q_EMIT installedChanged();
        }
    }
}

AidaMarketplaceCardWidget::AidaMarketplaceCardWidget(
    const ::mcp_marketplace::package_info_t& package, QWidget* parent)
    : QFrame(parent), package_(package) {
    setObjectName(QStringLiteral("aida.mcp.marketplace.card"));
    setFrameShape(QFrame::StyledPanel);
    setProperty("aidaRole", QStringLiteral("listcard"));
    setFocusPolicy(Qt::StrongFocus);

    const auto& t = theme::tokens();
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(t.panel.padding_compact, t.panel.padding_compact,
        t.panel.padding_compact, t.panel.padding_compact);
    root->setSpacing(t.spacing.md);

    const int glyph_side = t.control.height_lg;
    glyph_label_ = new QLabel(this);
    glyph_label_->setObjectName(QStringLiteral("aida.mcp.marketplace.card.glyph"));
    glyph_label_->setFixedSize(glyph_side, glyph_side);
    const qreal dpr = (std::max)(devicePixelRatioF(), 1.0);
    QPixmap pixmap(QSize(glyph_side, glyph_side) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(package_glyph_color(QString::fromStdString(package_.name)));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QRectF(0.0, 0.0, glyph_side, glyph_side));
        painter.setPen(theme::tokens().text_primary);
        painter.setFont(theme::fonts::strong());
        const QString name = QString::fromStdString(package_.name);
        painter.drawText(QRectF(0.0, 0.0, glyph_side, glyph_side), Qt::AlignCenter,
            name.isEmpty() ? QStringLiteral("?") : name.left(1).toUpper());
    }
    glyph_label_->setPixmap(pixmap);
    root->addWidget(glyph_label_, 0, Qt::AlignTop);

    auto* text_col = new QVBoxLayout();
    text_col->setSpacing(t.spacing.xxs);
    full_name_ = package_.display_name.empty()
        ? QString::fromStdString(package_.name)
        : QString::fromStdString(package_.display_name);
    name_label_ = new QLabel(full_name_, this);
    name_label_->setObjectName(QStringLiteral("aida.mcp.marketplace.card.name"));
    name_label_->setFont(theme::fonts::strong());
    name_label_->setToolTip(full_name_);
    text_col->addWidget(name_label_);
    meta_label_ = new QLabel(this);
    meta_label_->setObjectName(QStringLiteral("aida.mcp.marketplace.card.meta"));
    meta_label_->setFont(theme::fonts::caption());
    meta_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    append_meta_piece(full_meta_, QString::fromStdString(package_.author));
    append_meta_piece(full_meta_, QString::fromStdString(
        ::mcp_marketplace::registry_label(package_.registry)));
    if (package_.weekly_downloads > 0)
        append_meta_piece(full_meta_, format_count(package_.weekly_downloads) +
            QStringLiteral("/wk"));
    meta_label_->setText(full_meta_);
    meta_label_->setToolTip(full_meta_);
    text_col->addWidget(meta_label_);
    description_label_ = new QLabel(truncate_text(
        QString::fromStdString(package_.description), 240), this);
    description_label_->setObjectName(QStringLiteral("aida.mcp.marketplace.card.description"));
    description_label_->setWordWrap(true);
    description_label_->setFont(theme::fonts::caption());
    const QString full_description = QString::fromStdString(package_.description);
    if (full_description.size() > 240)
        description_label_->setToolTip(full_description);
    text_col->addWidget(description_label_);
    root->addLayout(text_col, 1);

    install_button_ = new QPushButton(this);
    install_button_->setObjectName(QStringLiteral("aida.mcp.marketplace.card.install"));
    root->addWidget(install_button_, 0, Qt::AlignVCenter);
    connect(install_button_, &QPushButton::clicked, this, [this] {
        if (is_pkg_installed(package_.name))
            Q_EMIT openDetails(QString::fromStdString(package_.name));
        else
            Q_EMIT reviewInstall(QString::fromStdString(package_.name));
    });
    refreshInstallButton();
}

void AidaMarketplaceCardWidget::refreshInstallButton() {
    const bool installed = is_pkg_installed(package_.name);
    const bool installing = ::mcp_marketplace::get_install_state() ==
        ::mcp_marketplace::install_state_t::installing;
    install_button_->setText(installed ? QStringLiteral("Installed")
        : installing ? QStringLiteral("Installing")
        : QStringLiteral("Review"));
    install_button_->setEnabled(!installing);
    install_button_->setToolTip(installed
        ? QStringLiteral("Installed. Manage this server from the MCP Servers settings page.")
        : installing
            ? QStringLiteral("A package operation is already in progress.")
            : QStringLiteral("Review the staged install before anything is enabled."));
}

void AidaMarketplaceCardWidget::setSelected(bool selected) {
    const bool current = property("aidaState").toString() ==
        QStringLiteral("selected");
    if (current == selected)
        return;
    setProperty("aidaState",
        selected ? QStringLiteral("selected") : QString());
    style()->unpolish(this);
    style()->polish(this);
}

void AidaMarketplaceCardWidget::relayout_text() {
    if (name_label_ && name_label_->width() > 1)
        name_label_->setText(name_label_->fontMetrics().elidedText(
            full_name_, Qt::ElideRight, name_label_->width()));
    if (meta_label_ && meta_label_->width() > 1)
        meta_label_->setText(meta_label_->fontMetrics().elidedText(
            full_meta_, Qt::ElideRight, meta_label_->width()));
}

void AidaMarketplaceCardWidget::contextMenuEvent(QContextMenuEvent* event) {
    Q_EMIT contextMenuRequested(QString::fromStdString(package_.name), event->globalPos());
    event->accept();
}

void AidaMarketplaceCardWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        Q_EMIT openDetails(QString::fromStdString(package_.name));
        event->accept();
        return;
    }
    QFrame::mouseReleaseEvent(event);
}

void AidaMarketplaceCardWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space) {
        Q_EMIT openDetails(QString::fromStdString(package_.name));
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

void AidaMarketplaceCardWidget::resizeEvent(QResizeEvent* event) {
    relayout_text();
    QFrame::resizeEvent(event);
}

AidaMcpInstallReviewDialog::AidaMcpInstallReviewDialog(
    const ::mcp_marketplace::package_info_t& package, QWidget* parent)
    : bridge::AidaDialog(parent), package_(package) {
    setObjectName(QStringLiteral("aida.mcp.marketplace.install_review"));
    setWindowTitle(QStringLiteral("Review MCP Install"));
    setWindowModality(Qt::ApplicationModal);
    const auto& t = theme::tokens();
    setMinimumSize(2 * t.row.property_label_w + static_cast<int>(t.shell.min_panel_w),
                   3 * static_cast<int>(t.shell.min_panel_w) + t.panel.padding);
    resize(4 * t.row.property_label_w + t.panel.overlay_margin,
           5 * static_cast<int>(t.shell.min_panel_w) + t.panel.header_h);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding,
        t.panel.padding, t.panel.padding);
    root->setSpacing(t.spacing.sm);

    auto* title = new QLabel(package_.display_name.empty()
        ? QString::fromStdString(package_.name)
        : QString::fromStdString(package_.display_name), this);
    title->setObjectName(QStringLiteral("aida.mcp.marketplace.install_review.title"));
    title->setFont(theme::fonts::h2());
    title->setWordWrap(true);
    root->addWidget(title);
    auto* name = new QLabel(QString::fromStdString(package_.name), this);
    name->setObjectName(QStringLiteral("aida.mcp.marketplace.install_review.name"));
    name->setFont(theme::fonts::caption());
    name->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(name);

    auto* badges = new QHBoxLayout();
    badges->setSpacing(t.spacing.xs);
    auto* registry = new QLabel(QString::fromStdString(
        ::mcp_marketplace::registry_label(package_.registry)), this);
    registry->setObjectName(QStringLiteral("aida.mcp.marketplace.install_review.registry"));
    registry->setFont(theme::fonts::caption());
    registry->setProperty("aidaVariant", QStringLiteral("neutral"));
    badges->addWidget(registry);
    auto* version = new QLabel(package_.version.empty()
        ? QStringLiteral("latest") : QString::fromStdString(package_.version), this);
    version->setObjectName(QStringLiteral("aida.mcp.marketplace.install_review.version"));
    version->setFont(theme::fonts::caption());
    version->setProperty("aidaVariant", QStringLiteral("neutral"));
    badges->addWidget(version);
    badges->addStretch(1);
    root->addLayout(badges);

    const auto preview = ::mcp_marketplace::preview_install(package_);
    auto* grid = new QFormLayout();
    grid->setHorizontalSpacing(t.spacing.md);
    grid->setVerticalSpacing(t.spacing.xs);
    auto add_row = [grid](const QString& label, const QString& value) {
        auto* value_label = new QLabel(value.isEmpty() ? QStringLiteral("-") : value);
        value_label->setWordWrap(true);
        value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addRow(label, value_label);
    };
    add_row(QStringLiteral("Install path"),
        QString::fromStdString(preview.install_path));
    add_row(QStringLiteral("Transport"), QString::fromStdString(preview.transport));
    add_row(QStringLiteral("Launch command"), QString::fromStdString(
        ::mcp_marketplace::launch_command_preview(preview)));
    add_row(QStringLiteral("Source"), package_source_label(package_));
    if (package_.weekly_downloads > 0)
        add_row(QStringLiteral("Weekly downloads"),
            format_count(package_.weekly_downloads));
    root->addLayout(grid);

    auto* warning = new QLabel(QStringLiteral(
        "Marketplace packages are unverified third-party code."), this);
    warning->setObjectName(QStringLiteral("aida.mcp.marketplace.install_review.warning"));
    warning->setFont(theme::fonts::bodyEm());
    warning->setProperty("aidaVariant", QStringLiteral("warning"));
    root->addWidget(warning, 0, Qt::AlignLeft);
    auto* policy = new QLabel(QStringLiteral(
        "MCP servers may expose tools that mutate files, process memory, browser state, "
        "proxy state, sandbox state, or analysis sessions. Install only stages the package. "
        "It will remain disabled with auto-connect off until you enable and connect it "
        "from MCP Servers."), this);
    policy->setObjectName(QStringLiteral("aida.mcp.marketplace.install_review.policy"));
    policy->setWordWrap(true);
    policy->setFont(theme::fonts::caption());
    policy->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(policy);
    root->addStretch(1);

    auto* buttons = new QDialogButtonBox(this);
    confirm_button_ = buttons->addButton(QStringLiteral("Install Disabled"),
        QDialogButtonBox::AcceptRole);
    confirm_button_->setObjectName(
        QStringLiteral("aida.mcp.marketplace.install_review.confirm"));
    buttons->addButton(QDialogButtonBox::Cancel);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        Q_EMIT installConfirmed(package_);
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void AidaMcpInstallReviewDialog::review(const ::mcp_marketplace::package_info_t& package,
                                        QWidget* parent) {
    auto* dialog = new AidaMcpInstallReviewDialog(package, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

AidaMcpMarketplaceView::AidaMcpMarketplaceView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.ai.mcp_marketplace"));
    controller_ = &AidaMcpMarketplaceController::instance();
    controller_->install();
    buildUi();

    connect(controller_, &AidaMcpMarketplaceController::searchResultsReady, this,
            &AidaMcpMarketplaceView::rebuildResults);
    connect(controller_, &AidaMcpMarketplaceController::searchFailed, this,
            [this](const QString& error) {
        chrome::toast_error(QStringLiteral("Search error: %1")
            .arg(truncate_text(error, 120)), 4.0);
        results_status_->setProperty("aidaVariant", QStringLiteral("error"));
        results_status_->style()->unpolish(results_status_);
        results_status_->style()->polish(results_status_);
        results_status_->setText(QStringLiteral("Search failed: %1").arg(error));
        if (results_.empty()) {
            empty_state_->setState(widgets::AidaStateView::State::Error);
            empty_state_->setTitle(QStringLiteral("Registry search failed"));
            empty_state_->setMessage(truncate_text(error, 160));
            results_stack_->setCurrentWidget(empty_state_);
        }
    });
    connect(controller_, &AidaMcpMarketplaceController::installFinished, this,
            [this](bool success, const QString&, const QString& error) {
        if (installing_pkg_.empty())
            return;
        if (success) {
            chrome::toast_info(QStringLiteral("Installed disabled: %1")
                .arg(QString::fromStdString(installing_pkg_)), 3.5);
            appendInstallLog(QStringLiteral("Install complete. The server is disabled; "
                "enable it from MCP Servers settings."));
        } else {
            chrome::toast_error(QStringLiteral("Install failed: %1")
                .arg(truncate_text(error, 120)), 5.0);
            appendInstallLog(QStringLiteral("Install failed."));
        }
        installing_pkg_.clear();
        rebuildResults();
        refreshDetail();
    });
    connect(controller_, &AidaMcpMarketplaceController::installLogAppended, this,
            &AidaMcpMarketplaceView::appendInstallLog);
    connect(controller_, &AidaMcpMarketplaceController::installedChanged, this, [this] {
        rebuildResults();
        refreshDetail();
    });
}

void AidaMcpMarketplaceView::buildUi() {
    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding,
        t.panel.padding, t.panel.padding);
    root->setSpacing(t.spacing.sm);

    auto* header = new widgets::AidaViewHeader(QStringLiteral("MCP Marketplace"),
        QStringLiteral("Discover and stage MCP servers from the npm registry"), this);
    root->addWidget(header);

    auto* search_row = new QHBoxLayout();
    search_row->setSpacing(t.spacing.xs);
    search_edit_ = new widgets::AidaSearchField(QStringLiteral(
        "Search the npm MCP registry (empty = server)"), this);
    search_edit_->setObjectName(QStringLiteral("aida.mcp.marketplace.search"));
    search_button_ = new QPushButton(QStringLiteral("Search"), this);
    search_button_->setObjectName(QStringLiteral("aida.mcp.marketplace.search.go"));
    search_button_->setToolTip(QStringLiteral("Search the npm MCP registry"));
    search_row->addWidget(search_edit_, 1);
    search_row->addWidget(search_button_);
    root->addLayout(search_row);

    results_status_ = new QLabel(this);
    results_status_->setObjectName(QStringLiteral("aida.mcp.marketplace.status"));
    results_status_->setFont(theme::fonts::caption());
    results_status_->setProperty("aidaVariant", QStringLiteral("secondary"));
    results_status_->setVisible(false);
    root->addWidget(results_status_);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setObjectName(QStringLiteral("aida.mcp.marketplace.splitter"));
    root->addWidget(splitter_, 1);

    auto* results_container = new QWidget(splitter_);
    results_container->setObjectName(QStringLiteral("aida.mcp.marketplace.results"));
    results_stack_ = new QStackedLayout(results_container);
    results_stack_->setContentsMargins(0, 0, 0, 0);

    results_scroll_ = new QScrollArea(results_container);
    results_scroll_->setObjectName(QStringLiteral("aida.mcp.marketplace.results.scroll"));
    results_scroll_->setWidgetResizable(true);
    results_host_ = new QWidget();
    results_host_->setObjectName(QStringLiteral("aida.mcp.marketplace.results.host"));
    results_layout_ = new QVBoxLayout(results_host_);
    results_layout_->setContentsMargins(0, 0, 0, 0);
    results_layout_->setSpacing(t.spacing.xs);
    results_layout_->addStretch(1);
    results_scroll_->setWidget(results_host_);
    results_stack_->addWidget(results_scroll_);

    empty_state_ = new widgets::AidaStateView(results_container);
    empty_state_->setObjectName(QStringLiteral("aida.mcp.marketplace.empty"));
    results_stack_->addWidget(empty_state_);
    results_stack_->setCurrentWidget(empty_state_);
    presentEmptyState();
    splitter_->addWidget(results_container);

    detail_panel_ = new QWidget(this);
    detail_panel_->setObjectName(QStringLiteral("aida.mcp.marketplace.detail"));
    detail_panel_->setMinimumWidth(3 * static_cast<int>(t.shell.min_panel_w) + t.panel.padding);
    auto* detail = new QVBoxLayout(detail_panel_);
    detail->setContentsMargins(t.panel.padding_compact, t.panel.padding_compact,
        t.panel.padding_compact, t.panel.padding_compact);
    detail->setSpacing(t.spacing.xs);
    auto* detail_top = new QHBoxLayout();
    detail_top->setSpacing(t.spacing.xs);
    detail_title_ = new QLabel(detail_panel_);
    detail_title_->setObjectName(QStringLiteral("aida.mcp.marketplace.detail.title"));
    detail_title_->setFont(theme::fonts::h2());
    detail_title_->setWordWrap(true);
    detail_close_ = new widgets::AidaToolButton(
        theme::icons::icon(QStringLiteral("clear-x")),
        QStringLiteral("Close details"), detail_panel_);
    detail_close_->setObjectName(QStringLiteral("aida.mcp.marketplace.detail.close"));
    detail_top->addWidget(detail_title_, 1);
    detail_top->addWidget(detail_close_, 0, Qt::AlignTop);
    detail->addLayout(detail_top);
    detail_name_ = new QLabel(detail_panel_);
    detail_name_->setObjectName(QStringLiteral("aida.mcp.marketplace.detail.name"));
    detail_name_->setFont(theme::fonts::caption());
    detail_name_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detail->addWidget(detail_name_);
    detail_description_ = new QLabel(detail_panel_);
    detail_description_->setObjectName(
        QStringLiteral("aida.mcp.marketplace.detail.description"));
    detail_description_->setWordWrap(true);
    detail->addWidget(detail_description_);
    detail_badges_ = new QLabel(detail_panel_);
    detail_badges_->setObjectName(QStringLiteral("aida.mcp.marketplace.detail.badges"));
    detail_badges_->setFont(theme::fonts::caption());
    detail->addWidget(detail_badges_);
    auto* prov_title = new QLabel(QStringLiteral("Provenance"), detail_panel_);
    prov_title->setObjectName(QStringLiteral("aida.mcp.marketplace.detail.provenance_title"));
    prov_title->setFont(theme::fonts::bodyEm());
    detail->addWidget(prov_title);
    auto* prov_host = new QWidget(detail_panel_);
    prov_host->setObjectName(QStringLiteral("aida.mcp.marketplace.detail.provenance"));
    detail_provenance_ = new QVBoxLayout(prov_host);
    detail_provenance_->setContentsMargins(0, 0, 0, 0);
    detail_provenance_->setSpacing(t.spacing.xxs);
    detail->addWidget(prov_host);
    detail_risk_ = new QLabel(detail_panel_);
    detail_risk_->setObjectName(QStringLiteral("aida.mcp.marketplace.detail.risk"));
    detail_risk_->setFont(theme::fonts::caption());
    detail_risk_->setWordWrap(true);
    detail->addWidget(detail_risk_);
    detail_tags_ = new QLabel(detail_panel_);
    detail_tags_->setObjectName(QStringLiteral("aida.mcp.marketplace.detail.tags"));
    detail_tags_->setFont(theme::fonts::caption());
    detail_tags_->setWordWrap(true);
    detail_tags_->setProperty("aidaVariant", QStringLiteral("secondary"));
    detail->addWidget(detail_tags_);
    detail_install_button_ = new QPushButton(QStringLiteral("Review"), detail_panel_);
    detail_install_button_->setObjectName(
        QStringLiteral("aida.mcp.marketplace.detail.install"));
    detail->addWidget(detail_install_button_, 0, Qt::AlignLeft);
    auto* log_title = new QLabel(QStringLiteral("Install log"), detail_panel_);
    log_title->setObjectName(QStringLiteral("aida.mcp.marketplace.detail.log_title"));
    log_title->setFont(theme::fonts::bodyEm());
    log_title->setVisible(false);
    detail->addWidget(log_title);
    install_log_ = new QPlainTextEdit(detail_panel_);
    install_log_->setObjectName(QStringLiteral("aida.mcp.marketplace.detail.install_log"));
    install_log_->setReadOnly(true);
    install_log_->setMaximumBlockCount(64);
    install_log_->setFont(theme::fonts::codeRegular());
    install_log_->setVisible(false);
    detail->addWidget(install_log_, 1);
    detail->addStretch(1);
    detail_panel_->setVisible(false);
    splitter_->addWidget(detail_panel_);
    splitter_->setStretchFactor(0, 1);

    connect(search_edit_, &QLineEdit::returnPressed, this,
            &AidaMcpMarketplaceView::startSearch);
    connect(search_edit_, &widgets::AidaSearchField::cleared, this,
            &AidaMcpMarketplaceView::startSearch);
    connect(search_button_, &QPushButton::clicked, this,
            &AidaMcpMarketplaceView::startSearch);
    connect(detail_close_, &QToolButton::clicked, this, [this] {
        selected_pkg_.clear();
        detail_panel_->setVisible(false);
        mark_selected_cards();
    });
    connect(detail_install_button_, &QPushButton::clicked, this, [this] {
        if (selected_pkg_.empty())
            return;
        for (const auto& package : results_) {
            if (package.name == selected_pkg_) {
                if (is_pkg_installed(package.name))
                    return;
                AidaMcpInstallReviewDialog* dialog =
                    new AidaMcpInstallReviewDialog(package, this);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                connect(dialog, &AidaMcpInstallReviewDialog::installConfirmed, this,
                        [this](const ::mcp_marketplace::package_info_t& p) {
                    beginInstall(p);
                });
                dialog->open();
                return;
            }
        }
    });

    state_poll_ = new QTimer(this);
    state_poll_->setInterval(500);
    state_poll_->setTimerType(Qt::CoarseTimer);
    connect(state_poll_, &QTimer::timeout, this, [this] {
        const bool installing = ::mcp_marketplace::get_install_state() ==
            ::mcp_marketplace::install_state_t::installing;
        search_button_->setEnabled(::mcp_marketplace::get_search_state() !=
            ::mcp_marketplace::search_state_t::searching);
        for (int i = 0; i < results_layout_->count(); ++i) {
            if (auto* card = qobject_cast<AidaMarketplaceCardWidget*>(
                    results_layout_->itemAt(i)->widget()))
                card->refreshInstallButton();
        }
        if (installing)
            refreshDetail();
    });
}

void AidaMcpMarketplaceView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!first_search_done_) {
        first_search_done_ = true;
        startSearch();
    }
    if (!state_poll_->isActive())
        state_poll_->start();
}

void AidaMcpMarketplaceView::hideEvent(QHideEvent* event) {
    state_poll_->stop();
    QWidget::hideEvent(event);
}

void AidaMcpMarketplaceView::startSearch() {
    std::string q = search_edit_->text().trimmed().toStdString();
    if (q.empty())
        q = "server";
    ::mcp_marketplace::search_async(q, ::mcp_marketplace::registry_t::npm);
    results_status_->setProperty("aidaVariant", QStringLiteral("secondary"));
    results_status_->style()->unpolish(results_status_);
    results_status_->style()->polish(results_status_);
    results_status_->setText(QStringLiteral("Searching the npm MCP registry\u2026"));
    results_status_->setVisible(true);
    if (results_.empty()) {
        empty_state_->setState(widgets::AidaStateView::State::Loading);
        empty_state_->setTitle(QStringLiteral("Searching the registry"));
        empty_state_->setMessage(QStringLiteral(
            "Querying the npm MCP registry. Results appear here."));
        results_stack_->setCurrentWidget(empty_state_);
    }
}

void AidaMcpMarketplaceView::presentEmptyState() {
    empty_state_->setState(widgets::AidaStateView::State::Empty);
    empty_state_->setTitle(QStringLiteral("Discover MCP servers"));
    empty_state_->setMessage(QStringLiteral(
        "Search the npm MCP registry to find servers. Installed servers stay disabled "
        "until you enable them from MCP Servers settings."));
}

void AidaMcpMarketplaceView::mark_selected_cards() {
    for (int i = 0; i < results_layout_->count(); ++i) {
        if (auto* card = qobject_cast<AidaMarketplaceCardWidget*>(
                results_layout_->itemAt(i)->widget()))
            card->setSelected(card->package().name == selected_pkg_);
    }
}

void AidaMcpMarketplaceView::rebuildResults() {
    results_ = ::mcp_marketplace::get_search_results();
    while (QLayoutItem* item = results_layout_->takeAt(0)) {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    if (results_.empty()) {
        results_stack_->setCurrentWidget(empty_state_);
        empty_state_->setState(widgets::AidaStateView::State::Empty);
        empty_state_->setTitle(QStringLiteral("No packages found"));
        empty_state_->setMessage(QStringLiteral(
            "The registry returned no packages for this query. Try a broader search term."));
    } else {
        for (const auto& package : results_) {
            auto* card = new AidaMarketplaceCardWidget(package, results_host_);
            connect(card, &AidaMarketplaceCardWidget::openDetails, this,
                    [this](const QString& name) {
                selected_pkg_ = name.toStdString();
                refreshDetail();
            });
            connect(card, &AidaMarketplaceCardWidget::reviewInstall, this,
                    [this](const QString& name) {
                for (const auto& package : results_) {
                    if (package.name == name.toStdString()) {
                        AidaMcpInstallReviewDialog* dialog =
                            new AidaMcpInstallReviewDialog(package, this);
                        dialog->setAttribute(Qt::WA_DeleteOnClose);
                        connect(dialog, &AidaMcpInstallReviewDialog::installConfirmed, this,
                                [this](const ::mcp_marketplace::package_info_t& p) {
                            beginInstall(p);
                        });
                        dialog->open();
                        return;
                    }
                }
            });
            connect(card, &AidaMarketplaceCardWidget::contextMenuRequested, this,
                    &AidaMcpMarketplaceView::openPackageMenu);
            results_layout_->addWidget(card);
        }
        results_stack_->setCurrentWidget(results_scroll_);
    }
    results_layout_->addStretch(1);
    results_status_->setProperty("aidaVariant", QStringLiteral("secondary"));
    results_status_->style()->unpolish(results_status_);
    results_status_->style()->polish(results_status_);
    results_status_->setText(QStringLiteral("%1 results")
        .arg(static_cast<int>(results_.size())));
    results_status_->setVisible(true);
    refreshDetail();
}

void AidaMcpMarketplaceView::refreshDetail() {
    if (selected_pkg_.empty()) {
        detail_panel_->setVisible(false);
        mark_selected_cards();
        return;
    }
    const auto found = std::find_if(results_.begin(), results_.end(),
        [this](const auto& package) { return package.name == selected_pkg_; });
    if (found == results_.end()) {
        selected_pkg_.clear();
        detail_panel_->setVisible(false);
        mark_selected_cards();
        return;
    }
    mark_selected_cards();
    const auto& package = *found;
    detail_panel_->setVisible(true);
    detail_title_->setText(package.display_name.empty()
        ? QString::fromStdString(package.name)
        : QString::fromStdString(package.display_name));
    detail_name_->setText(QString::fromStdString(package.name));
    detail_description_->setText(QString::fromStdString(package.description));

    QStringList badges;
    if (!package.version.empty())
        badges << QStringLiteral("v%1").arg(QString::fromStdString(package.version));
    if (!package.author.empty())
        badges << QStringLiteral("by %1").arg(QString::fromStdString(package.author));
    if (package.weekly_downloads > 0)
        badges << QStringLiteral("%1/wk").arg(format_count(package.weekly_downloads));
    detail_badges_->setText(badges.join(QStringLiteral("  ")));

    while (QLayoutItem* item = detail_provenance_->takeAt(0)) {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    ::mcp_marketplace::installed_server_t installed_srv;
    const bool installed = get_installed_server(package.name, installed_srv);
    const auto preview = installed ? installed_srv
        : ::mcp_marketplace::preview_install(package);
    const auto add_fact = [this](const QString& label, const QString& value) {
        auto* row = new QLabel(QStringLiteral("%1: %2").arg(label,
            truncate_text(value, 48)));
        row->setFont(theme::fonts::caption());
        row->setProperty("aidaVariant", QStringLiteral("secondary"));
        row->setWordWrap(true);
        row->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detail_provenance_->addWidget(row);
    };
    add_fact(QStringLiteral("Registry"),
        QString::fromStdString(::mcp_marketplace::registry_label(package.registry)));
    add_fact(QStringLiteral("Source"), package_source_label(package));
    add_fact(QStringLiteral("Install root"), QString::fromStdString(preview.install_path));
    add_fact(QStringLiteral("Transport"), QString::fromStdString(preview.transport));
    add_fact(QStringLiteral("Launch"),
        QString::fromStdString(::mcp_marketplace::launch_command_preview(preview)));

    const char* risk_variant = "warning";
    if (installed) {
        detail_risk_->setText(installed_srv.enabled
            ? QStringLiteral("Installed server is enabled; review exposed tools before use.")
            : QStringLiteral("Installed but disabled. Enable and connect from MCP Servers."));
        risk_variant = installed_srv.enabled ? "warning" : "secondary";
    } else {
        detail_risk_->setText(QStringLiteral(
            "Unverified third-party code. Install does not enable or connect it."));
    }
    if (detail_risk_->property("aidaVariant").toString() !=
        QLatin1String(risk_variant)) {
        detail_risk_->setProperty("aidaVariant", QString::fromLatin1(risk_variant));
        detail_risk_->style()->unpolish(detail_risk_);
        detail_risk_->style()->polish(detail_risk_);
    }

    if (!package.keywords_str.empty()) {
        QStringList tags;
        const QString keywords = QString::fromStdString(package.keywords_str);
        for (const auto& part : keywords.split(QLatin1Char(','))) {
            const QString tag = part.trimmed();
            if (!tag.isEmpty())
                tags << tag;
        }
        detail_tags_->setText(tags.isEmpty() ? QString()
            : QStringLiteral("Tags: %1").arg(tags.join(QStringLiteral(", "))));
    } else {
        detail_tags_->clear();
    }

    const bool installing = installing_pkg_ == package.name &&
        ::mcp_marketplace::get_install_state() ==
            ::mcp_marketplace::install_state_t::installing;
    detail_install_button_->setText(installed ? QStringLiteral("Installed")
        : installing ? QStringLiteral("Installing")
        : QStringLiteral("Review"));
    detail_install_button_->setEnabled(!installed && !installing);
    detail_install_button_->setToolTip(installed
        ? QStringLiteral("Installed. Manage this server from the MCP Servers settings page.")
        : installing
            ? QStringLiteral("This package is being installed.")
            : QStringLiteral("Review the staged install before anything is enabled."));
    install_log_->setVisible(!installing_pkg_.empty() &&
        installing_pkg_ == package.name);
}

void AidaMcpMarketplaceView::appendInstallLog(const QString& line) {
    install_log_->appendPlainText(line);
}

void AidaMcpMarketplaceView::beginInstall(const ::mcp_marketplace::package_info_t& package) {
    installing_pkg_ = package.name;
    install_log_->clear();
    install_log_->setVisible(true);
    appendInstallLog(QStringLiteral("Installing %1\u2026")
        .arg(QString::fromStdString(package.name)));
    appendInstallLog(QStringLiteral("Server will remain disabled after install."));
    ::mcp_marketplace::install_async(package);
    refreshDetail();
}

void AidaMcpMarketplaceView::openPackageMenu(const QString& package_name,
                                             const QPoint& global_pos) {
    const auto found = std::find_if(results_.begin(), results_.end(),
        [&](const auto& package) { return package.name == package_name.toStdString(); });
    if (found == results_.end())
        return;
    const auto package = *found;
    ::mcp_marketplace::installed_server_t installed_server;
    const bool installed = get_installed_server(package.name, installed_server);
    const bool installing = installing_pkg_ == package.name &&
        ::mcp_marketplace::get_install_state() ==
            ::mcp_marketplace::install_state_t::installing;

    const auto validate = [this, package]() -> bool {
        return std::find_if(results_.begin(), results_.end(),
            [&](const auto& item) {
                return item.name == package.name && item.version == package.version &&
                    item.registry == package.registry;
            }) != results_.end();
    };

    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setObjectName(QStringLiteral("aida.mcp.marketplace.card.menu"));
    menu->setToolTipsVisible(true);
    auto* open_details = menu->addAction(QStringLiteral("Open details"));
    auto* review_install = menu->addAction(QStringLiteral("Review install"));
    review_install->setEnabled(!installed && !installing);
    if (installed)
        review_install->setToolTip(QStringLiteral(
            "This package is already installed. Open Details to review its "
            "disabled/enabled state and launch provenance."));
    else if (installing)
        review_install->setToolTip(QStringLiteral(
            "Installation is already in progress. Wait for the current package operation "
            "to finish."));
    auto* copy_name = menu->addAction(QStringLiteral("Copy name"));
    auto* copy_version = menu->addAction(QStringLiteral("Copy version"));
    copy_version->setEnabled(!package.version.empty());
    if (package.version.empty())
        copy_version->setToolTip(QStringLiteral(
            "The registry result did not provide a package version"));
    auto* copy_registry = menu->addAction(QStringLiteral("Copy registry"));
    auto* copy_source = menu->addAction(QStringLiteral("Copy source"));
    auto* copy_launch = menu->addAction(QStringLiteral("Copy launch preview"));

    connect(open_details, &QAction::triggered, this, [this, validate, package] {
        if (!validate())
            return;
        selected_pkg_ = package.name;
        refreshDetail();
    });
    connect(review_install, &QAction::triggered, this, [this, validate, package] {
        if (!validate()) {
            chrome::toast_warning(QStringLiteral(
                "The marketplace result changed; select the package again"), 4.0);
            return;
        }
        AidaMcpInstallReviewDialog* dialog = new AidaMcpInstallReviewDialog(package, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &AidaMcpInstallReviewDialog::installConfirmed, this,
                [this](const ::mcp_marketplace::package_info_t& p) {
            beginInstall(p);
        });
        dialog->open();
    });
    connect(copy_name, &QAction::triggered, this, [validate, package] {
        if (validate())
            clipboard::set_text(QString::fromStdString(package.name));
    });
    connect(copy_version, &QAction::triggered, this, [validate, package] {
        if (validate() && !package.version.empty())
            clipboard::set_text(QString::fromStdString(package.version));
    });
    connect(copy_registry, &QAction::triggered, this, [validate, package] {
        if (validate())
            clipboard::set_text(QString::fromStdString(
                ::mcp_marketplace::registry_label(package.registry)));
    });
    connect(copy_source, &QAction::triggered, this, [validate, package] {
        if (validate())
            clipboard::set_text(package_source_label(package));
    });
    connect(copy_launch, &QAction::triggered, this, [validate, package, installed,
                                                     installed_server] {
        if (!validate())
            return;
        const auto preview = installed ? installed_server
            : ::mcp_marketplace::preview_install(package);
        clipboard::set_text(QString::fromStdString(
            ::mcp_marketplace::launch_command_preview(preview)));
    });
    menu->popup(global_pos);
}

void install_mcp_marketplace_domain(docking::AidaDockHost* host) {
    if (!host)
        return;
    AidaMcpMarketplaceController::instance().install();
    const auto result = host->install_view_factory(
        registry::stable_view_id_t("view.ai.mcp_marketplace"),
        [](QWidget* parent, const registry::view_instance_id_t&) -> QWidget* {
            return new AidaMcpMarketplaceView(parent);
        });
    if (!result.ok())
        diag::log_tagged_fmt("qt_mcp_market",
            "view_factory_install_failed view=%s status=%d detail=%s",
            "view.ai.mcp_marketplace", static_cast<int>(result.status),
            result.detail.c_str());
    aida::automation_ui::add_ui_shutdown_hook([] {
        AidaMcpMarketplaceController::instance().shutdown();
    });
    diag::log_tagged("qt_mcp_market", "mcp_marketplace_domain_installed");
}

}
