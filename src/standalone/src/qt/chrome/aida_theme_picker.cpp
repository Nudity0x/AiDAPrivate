#include "qt/chrome/aida_theme_picker.hpp"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QIcon>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "core/settings/theme_transfer_service.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/chrome/aida_theme_catalog.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_theme_controller.hpp"
#include "qt/theme/aida_tokens.hpp"

namespace aida::qt::chrome {

namespace {

QPixmap swatchPixmap(float r, float g, float b, qreal dpr)
{
    const qreal scale = dpr > 0.0 ? dpr : 1.0;
    const int diameter = theme::tokens().spacing.md;
    QPixmap pixmap(static_cast<int>(diameter * scale), static_cast<int>(diameter * scale));
    pixmap.fill(Qt::transparent);
    {
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor::fromRgbF(r, g, b));
        p.drawEllipse(pixmap.rect());
    }
    pixmap.setDevicePixelRatio(scale);
    return pixmap;
}

class ThemeRowFrame : public QFrame {
public:
    explicit ThemeRowFrame(QWidget* parent = nullptr) : QFrame(parent)
    {
        setObjectName(QStringLiteral("aida.theme_picker.row"));
        const auto& t = theme::tokens();
        setFixedHeight(t.row.compact);
        const QFontMetricsF fm(theme::fonts::caption());
        setMinimumWidth(static_cast<int>(fm.horizontalAdvance(QLatin1Char('0')) * 34));
        setFrameShape(QFrame::NoFrame);
    }
};

QToolButton* makeRowButton(ThemeRowFrame* row, const QString& name, const QPixmap& swatch,
                           bool active)
{
    auto* button = new QToolButton(row);
    button->setObjectName(QStringLiteral("aida.theme_picker.row.button"));
    button->setText(name);
    button->setFont(theme::fonts::caption());
    if (!swatch.isNull())
        button->setIcon(QIcon(swatch));
    const int swatch_box = theme::tokens().spacing.md;
    button->setIconSize(QSize(swatch_box, swatch_box));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setCheckable(true);
    button->setAutoRepeat(false);
    button->setChecked(active);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QLabel* makeSectionHeader(const QString& text, QWidget* parent)
{
    auto* header = new QLabel(text, parent);
    header->setObjectName(QStringLiteral("aida.theme_picker.section"));
    header->setProperty("aidaVariant", QStringLiteral("secondary"));
    header->setFont(theme::fonts::caption());
    const auto& t = theme::tokens();
    header->setContentsMargins(t.spacing.xs, t.spacing.xs, t.spacing.xs, t.spacing.xxs);
    return header;
}

}

AidaThemePickerPopup::AidaThemePickerPopup(QWidget* parent)
    : QWidget(parent, Qt::Popup)
{
    setObjectName(QStringLiteral("aida.theme_picker"));
    setAttribute(Qt::WA_StyledBackground, true);
    root_ = new QVBoxLayout(this);
    const auto& t = theme::tokens();
    root_->setContentsMargins(t.spacing.sm, t.spacing.sm, t.spacing.sm, t.spacing.sm);
    root_->setSpacing(t.spacing.xs);
    rebuild();
    connect(&AidaThemeCatalogController::instance(),
            &AidaThemeCatalogController::catalogChanged, this, [this] { rebuild(); });
    connect(&AidaThemeCatalogController::instance(),
            &AidaThemeCatalogController::activeThemeChanged, this, [this] { rebuild(); });
    connect(&AidaThemeCatalogController::instance(),
            &AidaThemeCatalogController::transferStatusChanged, this,
            [this] { refreshTransferStrip(); });
    connect(&theme::AidaThemeController::instance(),
            &theme::AidaThemeController::themeGenerationChanged, this,
            [this](quint64) { rebuild(); });
}

void AidaThemePickerPopup::openAt(const QPoint& global_pos)
{
    adjustSize();
    const auto& t = theme::tokens();
    QPoint target = global_pos;
    if (QWidget* anchor = parentWidget()) {
        const QRect anchor_rect(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
        int x = global_pos.x();
        if (x + width() > anchor_rect.right())
            x = anchor_rect.right() - width() - t.spacing.sm;
        if (x < anchor_rect.left() + t.spacing.sm)
            x = anchor_rect.left() + t.spacing.sm;
        target.setX(x);
    }
    if (QScreen* screen = QGuiApplication::screenAt(global_pos)) {
        const QRect avail = screen->availableGeometry();
        target.setX((std::max)(avail.left() + t.spacing.xs,
            (std::min)(target.x(), avail.right() - width() - t.spacing.xs)));
        target.setY((std::max)(avail.top() + t.spacing.xs,
            (std::min)(target.y(), avail.bottom() - height() - t.spacing.xs)));
    }
    move(target);
    show();
    raise();
}

void AidaThemePickerPopup::rebuild()
{
    while (QLayoutItem* item = root_->takeAt(0)) {
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
    addBuiltInRows(root_);
    addCustomRows(root_);
    addFooter(root_);
    adjustSize();
}

void AidaThemePickerPopup::addBuiltInRows(QVBoxLayout* layout)
{
    auto& catalog = AidaThemeCatalogController::instance();
    layout->addWidget(makeSectionHeader(QStringLiteral("Built-in Themes"), this));

    struct builtin_t {
        const char* name;
        float accent[3];
    };
    static const builtin_t k_builtins[] = {
        { "AiDA Dark", { 56.f / 255.f, 134.f / 255.f, 240.f / 255.f } },
        { "AiDA Light", { 42.f / 255.f, 104.f / 255.f, 216.f / 255.f } },
        { "Claude Dark", { 0xF4 / 255.f, 0x84 / 255.f, 0x5F / 255.f } },
        { "Claude Light", { 0xC1 / 255.f, 0x5F / 255.f, 0x3C / 255.f } },
    };
    const auto& t = theme::tokens();
    const qreal dpr = devicePixelRatioF();
    for (int i = 0; i < 4; ++i) {
        const bool active = catalog.customActive() < 0 && catalog.builtInActive() == i;
        auto* row = new ThemeRowFrame(this);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(t.spacing.xs);
        auto* button = makeRowButton(row, QString::fromLatin1(k_builtins[i].name),
            swatchPixmap(k_builtins[i].accent[0], k_builtins[i].accent[1],
                         k_builtins[i].accent[2], dpr), active);
        row_layout->addWidget(button, 1);
        const int index = i;
        connect(button, &QToolButton::clicked, this, [this, index] {
            AidaThemeCatalogController::instance().applyBuiltIn(index, true);
            diag::log_tagged_fmt("ui", "theme_changed idx=%d", index);
            close();
        });
        layout->addWidget(row);
    }
}

void AidaThemePickerPopup::addCustomRows(QVBoxLayout* layout)
{
    auto& catalog = AidaThemeCatalogController::instance();
    const auto& customs = catalog.customThemes();
    if (customs.empty())
        return;
    const auto& t = theme::tokens();
    layout->addWidget(makeSectionHeader(QStringLiteral("Custom Themes"), this));

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("aida.theme_picker.scroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* container = new QWidget(scroll);
    auto* rows_layout = new QVBoxLayout(container);
    rows_layout->setContentsMargins(0, 0, 0, 0);
    rows_layout->setSpacing(t.spacing.xxs);

    const qreal dpr = devicePixelRatioF();
    for (std::size_t i = 0; i < customs.size(); ++i) {
        const auto& theme = customs[i];
        const bool active = catalog.customActive() == static_cast<int>(i);
        auto* row = new ThemeRowFrame(container);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(t.spacing.xs);
        auto* button = makeRowButton(row, QString::fromStdString(theme.name),
            swatchPixmap(theme.accent[0], theme.accent[1], theme.accent[2], dpr), active);
        row_layout->addWidget(button, 1);
        auto* edit = new QToolButton(row);
        edit->setObjectName(QStringLiteral("aida.theme_picker.edit"));
        edit->setText(QStringLiteral("Edit"));
        edit->setToolTip(QStringLiteral("Edit this theme"));
        edit->setFont(theme::fonts::caption());
        edit->setCursor(Qt::PointingHandCursor);
        row_layout->addWidget(edit);
        const int index = static_cast<int>(i);
        connect(button, &QToolButton::clicked, this, [this, index] {
            AidaThemeCatalogController::instance().applyCustom(index);
            close();
        });
        connect(edit, &QToolButton::clicked, this, [this, index] {
            Q_EMIT editThemeRequested(index);
            close();
        });
        rows_layout->addWidget(row);
    }
    scroll->setWidget(container);
    constexpr int max_visible_rows = 10;
    const int row_pitch = t.row.compact + t.spacing.xxs;
    const int visible_rows = (std::min)(static_cast<int>(customs.size()), max_visible_rows);
    scroll->setFixedHeight(row_pitch * visible_rows - t.spacing.xxs);
    layout->addWidget(scroll);
}

void AidaThemePickerPopup::addFooter(QVBoxLayout* layout)
{
    auto* create_row = new ThemeRowFrame(this);
    auto* create_layout = new QHBoxLayout(create_row);
    create_layout->setContentsMargins(0, 0, 0, 0);
    auto* create_button = makeRowButton(create_row, QStringLiteral("+ Create New Theme"),
        QPixmap(), false);
    create_button->setProperty("aidaVariant", QStringLiteral("success"));
    create_layout->addWidget(create_button, 1);
    connect(create_button, &QToolButton::clicked, this, [this] {
        Q_EMIT createThemeRequested();
        close();
    });
    layout->addWidget(create_row);

    auto* import_row = new ThemeRowFrame(this);
    auto* import_layout = new QHBoxLayout(import_row);
    import_layout->setContentsMargins(0, 0, 0, 0);
    auto* import_button = makeRowButton(import_row, QStringLiteral("Import Theme..."),
        QPixmap(), false);
    import_button->setProperty("aidaVariant", QStringLiteral("info"));
    import_layout->addWidget(import_button, 1);
    connect(import_button, &QToolButton::clicked, this, [this] { importTheme(); });
    layout->addWidget(import_row);

    const auto& t = theme::tokens();
    transfer_strip_ = new QWidget(this);
    transfer_strip_->setObjectName(QStringLiteral("aida.theme_picker.transfer"));
    auto* strip_layout = new QVBoxLayout(transfer_strip_);
    strip_layout->setContentsMargins(t.spacing.xs, t.spacing.xxs, t.spacing.xs, t.spacing.xxs);
    strip_layout->setSpacing(t.spacing.xxs);
    transfer_label_ = new QLabel(transfer_strip_);
    transfer_label_->setObjectName(QStringLiteral("aida.theme_picker.transfer.label"));
    transfer_label_->setFont(theme::fonts::caption());
    transfer_label_->setWordWrap(true);
    strip_layout->addWidget(transfer_label_);
    retry_button_ = new QPushButton(QStringLiteral("Retry theme operation"), transfer_strip_);
    retry_button_->setObjectName(QStringLiteral("aida.theme_picker.retry"));
    connect(retry_button_, &QPushButton::clicked, this, [this] {
        if (aida::theme_transfer::request_retry())
            AidaThemeCatalogController::instance().clearLastError();
        refreshTransferStrip();
    });
    strip_layout->addWidget(retry_button_);
    layout->addWidget(transfer_strip_);
    refreshTransferStrip();
}

void AidaThemePickerPopup::refreshTransferStrip()
{
    if (!transfer_strip_)
        return;
    const auto status = aida::theme_transfer::status();
    const QString error = AidaThemeCatalogController::instance().lastError();
    const QString displayed = !error.isEmpty() ? error
        : QString::fromStdString(status.error);
    const bool show = status.pending || status.failed || !displayed.isEmpty();
    transfer_strip_->setVisible(show);
    if (!show)
        return;
    if (!displayed.isEmpty())
        transfer_label_->setText(displayed);
    else
        transfer_label_->setText(QString::fromStdString(status.stage));
    retry_button_->setVisible(status.retryable);
    adjustSize();
}

void AidaThemePickerPopup::importTheme()
{
    static const char k_theme_import_filter[] =
        "AiDA Theme (*.json)\0*.json\0"
        "All files (*.*)\0*.*\0\0";
    const auto picked = dialogs::open_file(this,
        QStringLiteral("Import Theme"), k_theme_import_filter);
    if (!picked)
        return;
    const auto requested = aida::theme_transfer::request_import(*picked);
    auto& catalog = AidaThemeCatalogController::instance();
    if (requested == aida::theme_transfer::request_result_t::queued ||
        requested == aida::theme_transfer::request_result_t::preview_recorded) {
        catalog.clearLastError();
    } else if (requested == aida::theme_transfer::request_result_t::busy) {
        catalog.reportError(QStringLiteral("A theme file operation is already running."));
    } else if (requested == aida::theme_transfer::request_result_t::rejected) {
        catalog.reportError(QStringLiteral("The theme import request was rejected."));
    }
    catalog.noteTransferActivity();
    refreshTransferStrip();
    close();
}

}
