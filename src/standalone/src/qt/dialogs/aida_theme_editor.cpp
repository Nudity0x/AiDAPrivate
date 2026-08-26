#include "qt/dialogs/aida_theme_editor.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QIcon>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/settings/theme_transfer_service.hpp"
#include "helpers/diag_log.hpp"
#include "qt/bridge/dialogs.hpp"
#include "qt/bridge/interaction_context_provider.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_line_edit.hpp"

namespace aida::qt::dialogs {

namespace {

QColor abgrToColor(std::uint32_t abgr)
{
    return QColor(static_cast<int>(abgr & 0xFF),
                  static_cast<int>((abgr >> 8) & 0xFF),
                  static_cast<int>((abgr >> 16) & 0xFF),
                  static_cast<int>((abgr >> 24) & 0xFF));
}

std::uint32_t colorToAbgr(const QColor& color)
{
    return static_cast<std::uint32_t>(color.red()) |
           (static_cast<std::uint32_t>(color.green()) << 8) |
           (static_cast<std::uint32_t>(color.blue()) << 16) |
           (static_cast<std::uint32_t>(color.alpha()) << 24);
}

QPixmap colorSwatch(const QColor& color, qreal dpr)
{
    const auto& t = theme::tokens();
    const qreal base = static_cast<qreal>(t.control.input_h - 2 * t.spacing.sm);
    const qreal scale = dpr > 0.0 ? dpr : 1.0;
    QPixmap pixmap(static_cast<int>(base * scale), static_cast<int>(base * scale));
    pixmap.fill(Qt::transparent);
    {
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(t.border_subtle, 1));
        p.setBrush(color);
        p.drawRoundedRect(pixmap.rect().adjusted(1, 1, -1, -1),
                          t.radius.sm * scale, t.radius.sm * scale);
    }
    pixmap.setDevicePixelRatio(scale);
    return pixmap;
}

QString colorCaption(const QString& label, const QColor& color)
{
    return QStringLiteral("%1  #%2%3%4%5")
        .arg(label,
             QString::number(color.alpha(), 16).rightJustified(2, QLatin1Char('0')),
             QString::number(color.red(), 16).rightJustified(2, QLatin1Char('0')),
             QString::number(color.green(), 16).rightJustified(2, QLatin1Char('0')),
             QString::number(color.blue(), 16).rightJustified(2, QLatin1Char('0')))
        .toUpper();
}

}

AidaThemeEditorDialog::AidaThemeEditorDialog(QWidget* parent)
    : bridge::AidaDialog(parent)
{
    setObjectName(QStringLiteral("aida.theme_editor"));
    setWindowTitle(QStringLiteral("Theme Editor"));
    setModal(false);

    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
                             t.panel.padding);
    root->setSpacing(t.spacing.sm);

    auto* header = new QLabel(this);
    header->setFont(theme::fonts::strong());
    header->setObjectName(QStringLiteral("aida.theme_editor.header"));
    root->addWidget(header);

    root->addWidget(new QLabel(QStringLiteral("Name"), this));
    name_edit_ = new widgets::AidaLineEdit(this);
    name_edit_->setObjectName(QStringLiteral("aida.theme_editor.name"));
    name_edit_->setMaxLength(96);
    bridge::InteractionContextProvider::mark_text_input(name_edit_);
    root->addWidget(name_edit_);

    auto make_swatch = [this, &root](QPushButton*& out, const QString& object_name) {
        out = new QPushButton(this);
        out->setObjectName(object_name);
        out->setCursor(Qt::PointingHandCursor);
        out->setMinimumHeight(theme::tokens().control.input_h);
        root->addWidget(out);
    };
    make_swatch(accent_button_, QStringLiteral("aida.theme_editor.accent"));
    connect(accent_button_, &QPushButton::clicked, this, [this] { onPickAccent(); });
    make_swatch(bg_button_, QStringLiteral("aida.theme_editor.bg"));
    connect(bg_button_, &QPushButton::clicked, this, [this] {
        onPickSwatch(&chrome::AidaCatalogTheme::bg_base, QStringLiteral("Background"));
    });
    make_swatch(panel_button_, QStringLiteral("aida.theme_editor.panel"));
    connect(panel_button_, &QPushButton::clicked, this, [this] {
        onPickSwatch(&chrome::AidaCatalogTheme::panel_bg, QStringLiteral("Panel Background"));
    });
    make_swatch(header_button_, QStringLiteral("aida.theme_editor.header_bg"));
    connect(header_button_, &QPushButton::clicked, this, [this] {
        onPickSwatch(&chrome::AidaCatalogTheme::panel_header, QStringLiteral("Panel Header"));
    });
    make_swatch(title_button_, QStringLiteral("aida.theme_editor.title_bg"));
    connect(title_button_, &QPushButton::clicked, this, [this] {
        onPickSwatch(&chrome::AidaCatalogTheme::title_bar, QStringLiteral("Title Bar"));
    });

    auto* icon_row = new QHBoxLayout();
    icon_pick_ = new QPushButton(QStringLiteral("Choose Image File..."), this);
    icon_pick_->setObjectName(QStringLiteral("aida.theme_editor.icon_pick"));
    connect(icon_pick_, &QPushButton::clicked, this, [this] { onPickIcon(); });
    icon_row->addWidget(icon_pick_);
    icon_clear_ = new QPushButton(QStringLiteral("Clear Icon"), this);
    icon_clear_->setObjectName(QStringLiteral("aida.theme_editor.icon_clear"));
    connect(icon_clear_, &QPushButton::clicked, this, [this] {
        draft_.icon_file_path.clear();
        draft_.icon_index = -1;
        syncFromDraft();
    });
    icon_row->addWidget(icon_clear_);
    icon_row->addStretch(1);
    root->addLayout(icon_row);
    icon_label_ = new QLabel(this);
    icon_label_->setObjectName(QStringLiteral("aida.theme_editor.icon_path"));
    icon_label_->setFont(theme::fonts::caption());
    icon_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    root->addWidget(icon_label_);

    transfer_strip_ = new QWidget(this);
    transfer_strip_->setObjectName(QStringLiteral("aida.theme_editor.transfer_strip"));
    auto* strip_layout = new QHBoxLayout(transfer_strip_);
    strip_layout->setContentsMargins(0, 0, 0, 0);
    transfer_label_ = new QLabel(transfer_strip_);
    transfer_label_->setFont(theme::fonts::caption());
    transfer_label_->setWordWrap(true);
    strip_layout->addWidget(transfer_label_, 1);
    retry_button_ = new QPushButton(QStringLiteral("Retry theme operation"), transfer_strip_);
    retry_button_->setObjectName(QStringLiteral("aida.theme_editor.retry"));
    connect(retry_button_, &QPushButton::clicked, this, [this] {
        if (aida::theme_transfer::request_retry())
            chrome::AidaThemeCatalogController::instance().clearLastError();
        refreshTransferStrip();
    });
    strip_layout->addWidget(retry_button_);
    transfer_strip_->setVisible(false);
    root->addWidget(transfer_strip_);

    error_label_ = new QLabel(this);
    error_label_->setObjectName(QStringLiteral("aida.theme_editor.error"));
    error_label_->setFont(theme::fonts::caption());
    error_label_->setWordWrap(true);
    error_label_->setProperty("aidaVariant", QStringLiteral("error"));
    error_label_->setVisible(false);
    root->addWidget(error_label_);

    auto* buttons = new QHBoxLayout();
    delete_button_ = new QPushButton(QStringLiteral("Delete"), this);
    delete_button_->setObjectName(QStringLiteral("aida.theme_editor.delete"));
    delete_button_->setProperty("aidaVariant", QStringLiteral("destructive"));
    connect(delete_button_, &QPushButton::clicked, this, [this] { onDeleteReview(); });
    buttons->addWidget(delete_button_);
    buttons->addStretch(1);
    export_button_ = new QPushButton(QStringLiteral("Export"), this);
    export_button_->setObjectName(QStringLiteral("aida.theme_editor.export"));
    connect(export_button_, &QPushButton::clicked, this, [this] { onExport(); });
    buttons->addWidget(export_button_);
    save_button_ = new QPushButton(QStringLiteral("Save"), this);
    save_button_->setObjectName(QStringLiteral("aida.theme_editor.save"));
    save_button_->setDefault(true);
    connect(save_button_, &QPushButton::clicked, this, [this] { onSave(); });
    buttons->addWidget(save_button_);
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_button_->setObjectName(QStringLiteral("aida.theme_editor.cancel"));
    connect(cancel_button_, &QPushButton::clicked, this, [this] { reject(); });
    buttons->addWidget(cancel_button_);
    root->addLayout(buttons);

    connect(name_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        save_button_->setEnabled(!text.trimmed().isEmpty());
    });

    header->setText(QStringLiteral("Create Theme"));
    setMinimumSize(360, 420);
    resize(420, 620);
}

void AidaThemeEditorDialog::editTheme(int catalog_index)
{
    auto& catalog = chrome::AidaThemeCatalogController::instance();
    const auto& themes = catalog.customThemes();
    if (catalog_index < 0 || catalog_index >= static_cast<int>(themes.size()))
        return;
    editing_index_ = catalog_index;
    draft_ = themes[static_cast<std::size_t>(catalog_index)];
    if (auto* header = findChild<QLabel*>(QStringLiteral("aida.theme_editor.header")))
        header->setText(QStringLiteral("Edit Theme"));
    delete_button_->setVisible(true);
    error_label_->clear();
    error_label_->setVisible(false);
    syncFromDraft();
    refreshTransferStrip();
    open();
}

void AidaThemeEditorDialog::createTheme()
{
    auto& catalog = chrome::AidaThemeCatalogController::instance();
    editing_index_ = -1;
    draft_ = chrome::AidaCatalogTheme{};
    draft_.name = "My Theme " + std::to_string(catalog.customThemes().size() + 1);
    if (auto* header = findChild<QLabel*>(QStringLiteral("aida.theme_editor.header")))
        header->setText(QStringLiteral("Create Theme"));
    delete_button_->setVisible(false);
    error_label_->clear();
    error_label_->setVisible(false);
    syncFromDraft();
    refreshTransferStrip();
    open();
}

void AidaThemeEditorDialog::syncFromDraft()
{
    name_edit_->setText(QString::fromStdString(draft_.name));
    refreshSwatches();
    if (draft_.icon_file_path.empty()) {
        icon_label_->clear();
        icon_label_->setVisible(false);
    } else {
        const std::string& path = draft_.icon_file_path;
        const auto slash = path.find_last_of("\\/");
        icon_label_->setText(QStringLiteral("File: %1").arg(QString::fromStdString(
            slash == std::string::npos ? path : path.substr(slash + 1))));
        icon_label_->setVisible(true);
    }
}

void AidaThemeEditorDialog::refreshSwatches()
{
    const qreal dpr = devicePixelRatioF();
    const QColor accent = QColor::fromRgbF(draft_.accent[0], draft_.accent[1], draft_.accent[2]);
    accent_button_->setIcon(QIcon(colorSwatch(accent, dpr)));
    accent_button_->setText(colorCaption(QStringLiteral("Accent Color"), accent));
    bg_button_->setIcon(QIcon(colorSwatch(abgrToColor(draft_.bg_base), dpr)));
    bg_button_->setText(colorCaption(QStringLiteral("Background"), abgrToColor(draft_.bg_base)));
    panel_button_->setIcon(QIcon(colorSwatch(abgrToColor(draft_.panel_bg), dpr)));
    panel_button_->setText(colorCaption(QStringLiteral("Panel Background"),
        abgrToColor(draft_.panel_bg)));
    header_button_->setIcon(QIcon(colorSwatch(abgrToColor(draft_.panel_header), dpr)));
    header_button_->setText(colorCaption(QStringLiteral("Panel Header"),
        abgrToColor(draft_.panel_header)));
    title_button_->setIcon(QIcon(colorSwatch(abgrToColor(draft_.title_bar), dpr)));
    title_button_->setText(colorCaption(QStringLiteral("Title Bar"),
        abgrToColor(draft_.title_bar)));
}

void AidaThemeEditorDialog::onPickAccent()
{
    const QColor initial = QColor::fromRgbF(draft_.accent[0], draft_.accent[1], draft_.accent[2]);
    const QColor picked = QColorDialog::getColor(initial, this,
        QStringLiteral("Accent Color"), QColorDialog::DontUseNativeDialog);
    if (!picked.isValid())
        return;
    draft_.accent[0] = static_cast<float>(picked.redF());
    draft_.accent[1] = static_cast<float>(picked.greenF());
    draft_.accent[2] = static_cast<float>(picked.blueF());
    refreshSwatches();
}

void AidaThemeEditorDialog::onPickSwatch(std::uint32_t chrome::AidaCatalogTheme::*field,
                                         const QString& label)
{
    const QColor initial = abgrToColor(draft_.*field);
    const QColor picked = QColorDialog::getColor(initial, this, label,
        QColorDialog::ShowAlphaChannel | QColorDialog::DontUseNativeDialog);
    if (!picked.isValid())
        return;
    draft_.*field = colorToAbgr(picked);
    refreshSwatches();
}

void AidaThemeEditorDialog::onPickIcon()
{
    static const char k_theme_icon_filter[] =
        "Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0"
        "All files (*.*)\0*.*\0\0";
    const auto picked = dialogs::open_file(this, QStringLiteral("Choose Theme Icon"),
        k_theme_icon_filter);
    if (!picked)
        return;
    QString error;
    if (!validateIconPath(*picked, error)) {
        error_label_->setText(error);
        error_label_->setVisible(true);
        diag::log_tagged_critical_fmt("render", "custom_theme_icon_rejected path_len=%llu",
            static_cast<unsigned long long>(picked->size()));
        return;
    }
    draft_.icon_index = -1;
    draft_.icon_file_path = *picked;
    error_label_->clear();
    error_label_->setVisible(false);
    syncFromDraft();
}

bool AidaThemeEditorDialog::validateIconPath(const std::string& path, QString& error)
{
    if (path.empty()) {
        error = QStringLiteral("The icon path is empty.");
        return false;
    }
    if (path.rfind("\\\\", 0) == 0 || path.rfind("//", 0) == 0) {
        error = QStringLiteral("Network paths are not allowed for theme icons.");
        return false;
    }
    if (path.size() > MAX_PATH * 4) {
        error = QStringLiteral("The icon path exceeds its length bound.");
        return false;
    }
    if (path.size() >= 2 && path[1] == ':') {
        char root[4] = { path[0], ':', '\\', '\0' };
        if (GetDriveTypeA(root) == DRIVE_REMOTE) {
            error = QStringLiteral("Remote drives are not allowed for theme icons.");
            return false;
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
        error = QStringLiteral("The icon file could not be read.");
        return false;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        error = QStringLiteral("The icon path is a directory.");
        return false;
    }
    const unsigned long long size =
        (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) |
        static_cast<unsigned long long>(data.nFileSizeLow);
    if (size == 0 || size > 32ull * 1024ull * 1024ull) {
        error = QStringLiteral("The icon file exceeds the 32 MiB bound.");
        return false;
    }
    error.clear();
    return true;
}

void AidaThemeEditorDialog::refreshTransferStrip()
{
    const auto status = aida::theme_transfer::status();
    const QString catalog_error = chrome::AidaThemeCatalogController::instance().lastError();
    const QString displayed = !catalog_error.isEmpty() ? catalog_error
        : QString::fromStdString(status.error);
    const bool show = status.pending || status.failed || !displayed.isEmpty();
    transfer_strip_->setVisible(show);
    if (!show)
        return;
    transfer_label_->setText(!displayed.isEmpty() ? displayed
        : QString::fromStdString(status.stage));
    retry_button_->setVisible(status.retryable);
}

void AidaThemeEditorDialog::onSave()
{
    draft_.name = name_edit_->text().toStdString();
    auto& catalog = chrome::AidaThemeCatalogController::instance();
    std::string error;
    if (!catalog.saveCustomTheme(draft_, editing_index_, error)) {
        error_label_->setText(QString::fromStdString(error));
        error_label_->setVisible(true);
        return;
    }
    error_label_->clear();
    error_label_->setVisible(false);
    Q_EMIT saved();
    accept();
}

void AidaThemeEditorDialog::onExport()
{
    draft_.name = name_edit_->text().toStdString();
    static const char k_theme_export_filter[] = "AiDA Theme (*.json)\0*.json\0\0";
    const auto target = dialogs::save_file(this, QStringLiteral("Export Theme"),
        k_theme_export_filter, QStringLiteral("json"),
        QString::fromStdString(draft_.name));
    if (!target)
        return;
    const auto requested = aida::theme_transfer::request_export(*target,
        chrome::AidaThemeCatalogController::toTransfer(draft_));
    auto& catalog = chrome::AidaThemeCatalogController::instance();
    if (requested == aida::theme_transfer::request_result_t::queued ||
        requested == aida::theme_transfer::request_result_t::preview_recorded) {
        catalog.clearLastError();
    } else if (requested == aida::theme_transfer::request_result_t::busy) {
        catalog.reportError(QStringLiteral("A theme file operation is already running."));
    } else if (requested == aida::theme_transfer::request_result_t::rejected) {
        catalog.reportError(QStringLiteral("The theme export request failed validation or scheduling."));
    }
    catalog.noteTransferActivity();
    refreshTransferStrip();
}

void AidaThemeEditorDialog::onDeleteReview()
{
    if (editing_index_ < 0)
        return;
    auto& catalog = chrome::AidaThemeCatalogController::instance();
    const int reviewed_index = editing_index_;
    const QString reviewed_name = QString::fromStdString(draft_.name);

    auto* confirm = new bridge::AidaDialog(this);
    confirm->setObjectName(QStringLiteral("aida.theme_editor.delete_confirm"));
    confirm->setWindowTitle(QStringLiteral("Delete Theme"));
    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(confirm);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
                               t.panel.padding);
    layout->setSpacing(t.spacing.sm);
    auto* verb = new QLabel(QStringLiteral("Delete custom theme"), confirm);
    verb->setFont(theme::fonts::strong());
    layout->addWidget(verb);
    auto* target = new QLabel(QStringLiteral("Target: %1").arg(reviewed_name), confirm);
    layout->addWidget(target);
    auto* scope_label = new QLabel(QStringLiteral("Scope: The persisted custom-theme catalog"), confirm);
    scope_label->setFont(theme::fonts::caption());
    scope_label->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(scope_label);
    auto* effect = new QLabel(QStringLiteral(
        "The theme is removed. If active, the custom-theme selection is cleared."), confirm);
    effect->setWordWrap(true);
    layout->addWidget(effect);
    auto* reversible = new QLabel(QStringLiteral(
        "Re-import or recreate the theme to restore it."), confirm);
    reversible->setFont(theme::fonts::caption());
    reversible->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(reversible);
    auto* prerequisite = new QLabel(confirm);
    prerequisite->setWordWrap(true);
    prerequisite->setFont(theme::fonts::caption());
    layout->addWidget(prerequisite);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Cancel, confirm);
    auto* confirm_button = box->addButton(QStringLiteral("Delete"),
        QDialogButtonBox::DestructiveRole);
    confirm_button->setObjectName(QStringLiteral("aida.theme_editor.delete_confirm.button"));
    confirm_button->setProperty("aidaVariant", QStringLiteral("destructive"));
    if (auto* cancel = box->button(QDialogButtonBox::Cancel)) {
        cancel->setObjectName(QStringLiteral("aida.theme_editor.delete_confirm.cancel"));
        cancel->setDefault(true);
    }
    connect(box, &QDialogButtonBox::rejected, confirm, &QDialog::reject);
    layout->addWidget(box);
    confirm->setMinimumSize(340, 230);

    auto& scope = confirm->add_revalidate_scope({
        [&catalog, reviewed_index, reviewed_name] {
            const auto& themes = catalog.customThemes();
            if (reviewed_index < 0 || reviewed_index >= static_cast<int>(themes.size()))
                return QStringLiteral("<gone>");
            return QString::fromStdString(
                themes[static_cast<std::size_t>(reviewed_index)].name);
        },
        {}}, QStringLiteral("The reviewed theme changed or no longer exists. Cancel and select it again."));
    Q_UNUSED(scope);
    const bool target_current = reviewed_index >= 0 &&
        reviewed_index < static_cast<int>(catalog.customThemes().size()) &&
        QString::fromStdString(catalog.customThemes()[static_cast<std::size_t>(reviewed_index)].name) ==
            reviewed_name;
    prerequisite->setText(target_current
        ? QStringLiteral("The reviewed catalog entry is unchanged.")
        : QStringLiteral("The reviewed theme changed or no longer exists. Cancel and select it again."));
    confirm_button->setEnabled(target_current);
    connect(confirm_button, &QPushButton::clicked, confirm, [&] {
        std::string error;
        if (!catalog.deleteCustomTheme(reviewed_index, error)) {
            error_label_->setText(QString::fromStdString(error));
            error_label_->setVisible(true);
            confirm->reject();
            return;
        }
        confirm->accept();
        Q_EMIT saved();
        accept();
    });
    confirm->setAttribute(Qt::WA_DeleteOnClose, true);
    confirm->open();
}

}
