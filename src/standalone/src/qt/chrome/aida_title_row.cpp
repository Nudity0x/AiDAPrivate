#include "qt/chrome/aida_title_row.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QPainterPath>
#include <QPixmap>
#include <QIcon>
#include <QToolButton>

#include <algorithm>
#include <cmath>

#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_icons.hpp"
#include "qt/theme/aida_theme_controller.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::chrome {

namespace {

const QString& breadcrumb_separator()
{
    static const QString separator(QChar(0x203A));
    return separator;
}

}

QPixmap sunPixmap(qreal size, qreal dpr, const QColor& color)
{
    const qreal scale = dpr > 0.0 ? dpr : 1.0;
    QPixmap pixmap(QSize(static_cast<int>(size * scale), static_cast<int>(size * scale)));
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPointF c(pixmap.width() * 0.5, pixmap.height() * 0.5);
    const qreal unit = scale;
    p.setPen(QPen(color, 1.5 * unit));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, 6.0 * unit, 6.0 * unit);
    for (int ray = 0; ray < 8; ++ray) {
        const qreal angle = ray * 0.785398;
        const qreal rx = std::cos(angle);
        const qreal ry = std::sin(angle);
        p.drawLine(QPointF(c.x() + rx * 8.0 * unit, c.y() + ry * 8.0 * unit),
                   QPointF(c.x() + rx * 9.5 * unit, c.y() + ry * 9.5 * unit));
    }
    pixmap.setDevicePixelRatio(scale);
    return pixmap;
}

QPixmap moonPixmap(qreal size, qreal dpr, const QColor& color)
{
    const qreal scale = dpr > 0.0 ? dpr : 1.0;
    QPixmap pixmap(QSize(static_cast<int>(size * scale), static_cast<int>(size * scale)));
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPointF c(pixmap.width() * 0.5 - 1.0 * scale, pixmap.height() * 0.5);
    p.setPen(QPen(color, 1.5 * scale));
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    path.arcMoveTo(QRectF(c.x() - 7.0 * scale, c.y() - 7.0 * scale,
                          14.0 * scale, 14.0 * scale), 315.0);
    path.arcTo(QRectF(c.x() - 7.0 * scale, c.y() - 7.0 * scale, 14.0 * scale, 14.0 * scale),
               315.0, 255.0);
    p.drawPath(path);
    pixmap.setDevicePixelRatio(scale);
    return pixmap;
}

AidaBreadcrumbWidget::AidaBreadcrumbWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.title_row.breadcrumb"));
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void AidaBreadcrumbWidget::setSegments(const QStringList& segments)
{
    if (segments_ == segments)
        return;
    segments_ = segments;
    setToolTip(segments_.isEmpty() ? QString()
        : segments_.join(QStringLiteral(" %1 ").arg(breadcrumb_separator())));
    rebuildLayout();
    update();
}

QSize AidaBreadcrumbWidget::sizeHint() const
{
    const QFontMetricsF fm(theme::fonts::caption());
    return QSize(static_cast<int>(fm.horizontalAdvance(QLatin1Char('0')) * 30),
                 theme::tokens().status_bar.height - theme::tokens().spacing.xxs * 2);
}

QSize AidaBreadcrumbWidget::minimumSizeHint() const
{
    const QFontMetricsF fm(theme::fonts::caption());
    return QSize(static_cast<int>(fm.horizontalAdvance(breadcrumb_separator()) * 2.0 +
                                  theme::tokens().grid * 4),
                 theme::tokens().status_bar.height - theme::tokens().spacing.xxs * 2);
}

void AidaBreadcrumbWidget::rebuildLayout()
{
    elided_.clear();
    segment_rects_.clear();
    if (segments_.isEmpty())
        return;
    const auto& t = theme::tokens();
    const QFontMetricsF fm(theme::fonts::caption());
    const qreal gap = t.grid * 2.0;
    const qreal separator_w = fm.horizontalAdvance(breadcrumb_separator());
    QStringList remaining = segments_;
    const qreal avail = width() - t.spacing.xs;
    auto joined_width = [&](const QStringList& parts) {
        qreal w = 0.0;
        for (int i = 0; i < parts.size(); ++i) {
            w += fm.horizontalAdvance(parts[i]) + gap * 2.0;
            if (i + 1 < parts.size())
                w += separator_w;
        }
        return w;
    };
    while (remaining.size() > 1 && joined_width(remaining) > avail)
        remaining.removeFirst();
    for (int i = 0; i < remaining.size(); ++i) {
        QString text = remaining[i];
        const qreal right_reserve = (i == remaining.size() - 1) ? 0.0 : t.grid * 12.0;
        const qreal max_w = (std::max)(t.grid * 6.0, avail * 0.5 - right_reserve);
        if (fm.horizontalAdvance(text) > max_w)
            text = fm.elidedText(text, Qt::ElideMiddle, static_cast<int>(max_w));
        elided_.push_back(text);
    }
}

void AidaBreadcrumbWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    rebuildLayout();
    update();
}

void AidaBreadcrumbWidget::mouseMoveEvent(QMouseEvent* event)
{
    int hit = -1;
    for (int i = 0; i < static_cast<int>(segment_rects_.size()); ++i) {
        if (segment_rects_[static_cast<std::size_t>(i)].contains(event->position())) {
            hit = i;
            break;
        }
    }
    if (hit != hovered_) {
        hovered_ = hit;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void AidaBreadcrumbWidget::leaveEvent(QEvent* event)
{
    if (hovered_ != -1) {
        hovered_ = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void AidaBreadcrumbWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    if (elided_.empty())
        return;
    QPainter p(this);
    const auto& t = theme::tokens();
    const QFont font = theme::fonts::caption();
    const QFontMetricsF fm(font);
    p.setFont(font);

    const QString& separator = breadcrumb_separator();
    const qreal separator_w = fm.horizontalAdvance(separator);
    const bool dark = widgets::relative_luminance(t.bg_base) < 0.5;
    qreal x = t.spacing.xs;
    segment_rects_.clear();
    const qreal gap = t.grid * 2.0;
    const qreal baseline = widgets::text_baseline_centered(QRectF(rect()), fm);
    const int count = static_cast<int>(elided_.size());
    for (int i = 0; i < count; ++i) {
        const qreal text_w = fm.horizontalAdvance(elided_[static_cast<std::size_t>(i)]);
        const QRectF seg_rect(x - gap * 0.5, 0, text_w + gap, height());
        segment_rects_.push_back(seg_rect);
        const bool hovered = hovered_ == i;
        if (hovered) {
            p.setPen(Qt::NoPen);
            p.setBrush(widgets::with_alpha(t.text_primary, dark ? 0.08 : 0.06));
            p.drawRoundedRect(seg_rect, t.radius.sm, t.radius.sm);
        }
        p.setPen(hovered || i == count - 1 ? t.text_primary : t.text_secondary);
        p.drawText(QPointF(x, baseline), elided_[static_cast<std::size_t>(i)]);
        x += text_w + gap;
        if (i + 1 < count) {
            p.setPen(t.text_dim);
            p.drawText(QPointF(x, baseline), separator);
            x += separator_w + gap;
        }
        if (x > width())
            break;
    }
}

AidaTitleRow::AidaTitleRow(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.title_row"));
    setAttribute(Qt::WA_StyledBackground, true);
    const auto& t = theme::tokens();
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(static_cast<int>(t.shell.menu_pad_x), 0,
        static_cast<int>(t.shell.menu_pad_x), 0);
    layout->setSpacing(t.spacing.sm);

    logo_ = new QLabel(this);
    logo_->setObjectName(QStringLiteral("aida.title_row.logo"));
    layout->addWidget(logo_);

    wordmark_ = new QLabel(QStringLiteral("AiDA"), this);
    wordmark_->setObjectName(QStringLiteral("aida.title_row.wordmark"));
    wordmark_->setFont(theme::fonts::h2());
    layout->addWidget(wordmark_);

    breadcrumb_ = new AidaBreadcrumbWidget(this);
    layout->addWidget(breadcrumb_, 1);

    pill_ = new QFrame(this);
    pill_->setObjectName(QStringLiteral("aida.title_row.workspace_pill"));
    pill_->setProperty("aidaRole", QStringLiteral("pill"));
    pill_->setToolTip(QStringLiteral("Active workspace"));
    auto* pill_layout = new QHBoxLayout(pill_);
    pill_dot_ = new QLabel(pill_);
    pill_dot_->setObjectName(QStringLiteral("aida.title_row.workspace_pill.dot"));
    pill_layout->addWidget(pill_dot_);
    pill_label_ = new QLabel(pill_);
    pill_label_->setObjectName(QStringLiteral("aida.title_row.workspace_pill.label"));
    pill_label_->setFont(theme::fonts::caption());
    pill_layout->addWidget(pill_label_, 1);
    layout->addWidget(pill_);
    pill_layout_ = pill_layout;

    theme_toggle_ = new QToolButton(this);
    theme_toggle_->setObjectName(QStringLiteral("aida.title_row.theme_toggle"));
    theme_toggle_->setToolTip(QStringLiteral("Toggle dark/light mode; right-click for all themes"));
    theme_toggle_->setAutoRaise(true);
    theme_toggle_->setCursor(Qt::PointingHandCursor);
    theme_toggle_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(theme_toggle_, &QToolButton::clicked, this, [this] {
        Q_EMIT themeToggleRequested();
    });
    connect(theme_toggle_, &QToolButton::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        Q_EMIT themeMenuRequested(theme_toggle_->mapToGlobal(
            QPoint(0, theme_toggle_->height())));
        Q_UNUSED(pos);
    });
    layout->addWidget(theme_toggle_);

    reapplyMetrics();
    refreshThemeIcon();

    connect(&theme::AidaThemeController::instance(),
            &theme::AidaThemeController::themeGenerationChanged,
            this, [this](quint64) {
        reapplyMetrics();
        refreshThemeIcon();
    });
}

void AidaTitleRow::reapplyMetrics()
{
    const auto& t = theme::tokens();
    logo_->setFixedSize(static_cast<int>(t.shell.title_logo),
                        static_cast<int>(t.shell.title_logo));
    pill_->setFixedHeight(static_cast<int>(t.status_bar.height));
    if (pill_layout_) {
        pill_layout_->setContentsMargins(t.spacing.md, 0, t.spacing.md, 0);
        pill_layout_->setSpacing(t.spacing.sm);
    }
    pill_dot_->setFixedSize(t.status_bar.dot, t.status_bar.dot);
    const QFontMetricsF pill_fm(pill_label_->font());
    pill_text_max_w_ = static_cast<int>(pill_fm.horizontalAdvance(QLatin1Char('0')) * 22);
    pill_->setMaximumWidth(t.spacing.md * 2 + t.status_bar.dot + t.spacing.sm +
        pill_text_max_w_);
    theme_toggle_->setFixedSize(static_cast<int>(t.shell.title_control),
                                static_cast<int>(t.shell.title_control));
    refreshLogo();
    applyPill();
    updateGeometry();
}

void AidaTitleRow::refreshLogo()
{
    const int logo_px = (std::max)(1, static_cast<int>(theme::tokens().shell.title_logo));
    const QPixmap source(QStringLiteral(":/img/aidalogo.png"));
    if (!source.isNull()) {
        const qreal dpr = devicePixelRatioF();
        QPixmap scaled = source.scaled(static_cast<int>(logo_px * dpr),
            static_cast<int>(logo_px * dpr),
            Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        logo_->setPixmap(scaled);
    } else {
        logo_->setPixmap(theme::icons::icon(QStringLiteral("logo")).pixmap(logo_px, logo_px));
    }
}

void AidaTitleRow::refreshThemeIcon()
{
    const auto& t = theme::tokens();
    const bool dark = widgets::relative_luminance(t.bg_base) < 0.5;
    const QColor color = t.text_secondary;
    const qreal dpr = devicePixelRatioF();
    const int glyph = (std::max)(1, static_cast<int>(t.shell.title_control -
        t.spacing.xs * 2 - t.panel.border * 2));
    theme_toggle_->setIconSize(QSize(glyph, glyph));
    theme_toggle_->setIcon(QIcon(dark ? sunPixmap(glyph, dpr, color)
                                      : moonPixmap(glyph, dpr, color)));
}

void AidaTitleRow::setBreadcrumbSegments(const QStringList& segments)
{
    breadcrumb_->setSegments(segments);
}

void AidaTitleRow::setWorkspacePillText(const QString& name, bool liveTarget)
{
    if (name == pill_text_ && liveTarget == pill_live_target_)
        return;
    pill_text_ = name;
    pill_live_target_ = liveTarget;
    applyPill();
}

void AidaTitleRow::applyPill()
{
    const QFontMetricsF fm(pill_label_->font());
    pill_label_->setText(fm.elidedText(pill_text_, Qt::ElideRight, pill_text_max_w_));
    pill_->setToolTip(pill_text_.isEmpty()
        ? QStringLiteral("Active workspace")
        : QStringLiteral("Active workspace: %1").arg(pill_text_));
    const auto& t = theme::tokens();
    const QColor dot = pill_live_target_ ? t.accent : t.success;
    const qreal dpr = devicePixelRatioF();
    const int dot_px_size = (std::max)(1, static_cast<int>(t.status_bar.dot * dpr));
    QPixmap dot_px(dot_px_size, dot_px_size);
    dot_px.fill(Qt::transparent);
    {
        QPainter p(&dot_px);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(dot_px.rect());
    }
    dot_px.setDevicePixelRatio(dpr);
    pill_dot_->setPixmap(dot_px);
}

}
