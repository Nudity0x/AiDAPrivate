#include "qt/editor/aida_image_view.hpp"

#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "core/infra/executor.hpp"
#include "helpers/diag_log.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_motion.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_state_view.hpp"

namespace aida::qt::editor {

AidaImageCanvas::AidaImageCanvas(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.image.canvas"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Image canvas"));
    setAccessibleDescription(QStringLiteral(
        "Image view. Arrow keys pan, + and - zoom, Home resets the view."));
    const int min_edge = theme::tokens().grid * 16;
    setMinimumSize(min_edge, min_edge);
    ease_timer_ = new QTimer(this);
    ease_timer_->setInterval(16);
    connect(ease_timer_, &QTimer::timeout, this, &AidaImageCanvas::easeTick);
}

void AidaImageCanvas::setImage(const QImage& image)
{
    image_ = image;
    setFitToView();
    update();
}

void AidaImageCanvas::clearImage()
{
    image_ = QImage();
    zoom_ = 1.0;
    target_zoom_ = 1.0;
    pan_x_ = 0.0;
    pan_y_ = 0.0;
    fit_to_view_ = true;
    update();
}

void AidaImageCanvas::setFitToView()
{
    fit_to_view_ = true;
    if (image_.isNull())
        return;
    const qreal cw = width();
    const qreal ch = height();
    if (cw <= 1.0 || ch <= 1.0)
        return;
    const qreal fx = cw / image_.width();
    const qreal fy = ch / image_.height();
    qreal f = std::min(fx, fy);
    if (f > 1.0) f = 1.0;
    target_zoom_ = f;
    zoom_ = f;
    pan_x_ = 0.0;
    pan_y_ = 0.0;
    Q_EMIT zoomChanged(zoom_);
    update();
}

void AidaImageCanvas::resetView()
{
    setFitToView();
}

void AidaImageCanvas::easeTick()
{
    const qreal diff = target_zoom_ - zoom_;
    if (std::fabs(diff) < 0.0001) {
        zoom_ = target_zoom_;
        ease_timer_->stop();
        update();
        return;
    }
    const qreal k = std::min(20.0 * 0.016, 1.0);
    zoom_ += diff * k;
    Q_EMIT zoomChanged(zoom_);
    update();
}

void AidaImageCanvas::paintEvent(QPaintEvent* event)
{
    const auto& t = theme::tokens();
    QPainter painter(this);
    painter.fillRect(event->rect(), t.bg_base);
    if (image_.isNull())
        return;
    const qreal cw = width();
    const qreal ch = height();
    const qreal disp_w = image_.width() * zoom_;
    const qreal disp_h = image_.height() * zoom_;
    const qreal cx = (cw - disp_w) * 0.5 + pan_x_;
    const qreal cy = (ch - disp_h) * 0.5 + pan_y_;
    if (std::fabs(zoom_ - 1.0) > 0.001)
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QRectF(cx, cy, disp_w, disp_h), image_);
    if (hasFocus()) {
        const qreal inset = static_cast<qreal>(t.control.focus_ring) * 0.5;
        painter.setPen(QPen(t.border_focus, static_cast<qreal>(t.control.focus_ring)));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset));
    }
}

void AidaImageCanvas::stepZoom(qreal factor)
{
    const qreal prev = target_zoom_;
    target_zoom_ = std::clamp(target_zoom_ * factor, 0.05, 32.0);
    fit_to_view_ = false;
    const qreal ratio = prev > 0.0001 ? target_zoom_ / prev : 1.0;
    pan_x_ *= ratio;
    pan_y_ *= ratio;
    if (theme::AidaMotion::reducedMotion()) {
        zoom_ = target_zoom_;
        Q_EMIT zoomChanged(zoom_);
        update();
        return;
    }
    if (!ease_timer_->isActive())
        ease_timer_->start();
}

void AidaImageCanvas::keyPressEvent(QKeyEvent* event)
{
    if (image_.isNull() || event->modifiers()) {
        QWidget::keyPressEvent(event);
        return;
    }
    const qreal pan_step = static_cast<qreal>(theme::tokens().spacing.section);
    switch (event->key()) {
    case Qt::Key_Left:
        pan_x_ += pan_step;
        fit_to_view_ = false;
        update();
        event->accept();
        return;
    case Qt::Key_Right:
        pan_x_ -= pan_step;
        fit_to_view_ = false;
        update();
        event->accept();
        return;
    case Qt::Key_Up:
        pan_y_ += pan_step;
        fit_to_view_ = false;
        update();
        event->accept();
        return;
    case Qt::Key_Down:
        pan_y_ -= pan_step;
        fit_to_view_ = false;
        update();
        event->accept();
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        stepZoom(1.15);
        event->accept();
        return;
    case Qt::Key_Minus:
        stepZoom(1.0 / 1.15);
        event->accept();
        return;
    case Qt::Key_Home:
        resetView();
        event->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void AidaImageCanvas::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    update();
}

void AidaImageCanvas::focusOutEvent(QFocusEvent* event)
{
    QWidget::focusOutEvent(event);
    update();
}

void AidaImageCanvas::wheelEvent(QWheelEvent* event)
{
    if (image_.isNull()) {
        QWidget::wheelEvent(event);
        return;
    }
    const qreal degrees = event->angleDelta().y() / 8.0 / 15.0;
    if (std::fabs(degrees) < 0.001)
        return;
    const qreal prev = target_zoom_;
    const qreal factor = degrees > 0.0 ? 1.15 : 1.0 / 1.15;
    target_zoom_ = std::clamp(target_zoom_ * factor, 0.05, 32.0);
    fit_to_view_ = false;
    const qreal mx = event->position().x() - width() * 0.5 - pan_x_;
    const qreal my = event->position().y() - height() * 0.5 - pan_y_;
    const qreal ratio = prev > 0.0001 ? target_zoom_ / prev : 1.0;
    pan_x_ -= mx * (ratio - 1.0);
    pan_y_ -= my * (ratio - 1.0);
    if (theme::AidaMotion::reducedMotion()) {
        zoom_ = target_zoom_;
        Q_EMIT zoomChanged(zoom_);
        update();
        event->accept();
        return;
    }
    if (!ease_timer_->isActive())
        ease_timer_->start();
    event->accept();
}

void AidaImageCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && !image_.isNull()) {
        panning_ = true;
        pan_start_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void AidaImageCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (panning_ && (event->buttons() & Qt::LeftButton)) {
        const QPoint delta = event->pos() - pan_start_;
        if (std::abs(delta.x()) >= 2 || std::abs(delta.y()) >= 2) {
            pan_x_ += delta.x();
            pan_y_ += delta.y();
            pan_start_ = event->pos();
            fit_to_view_ = false;
            update();
        }
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void AidaImageCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        resetView();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void AidaImageCanvas::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (fit_to_view_ && !image_.isNull())
        setFitToView();
}

AidaImageView::AidaImageView(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("aida.document.image.primary"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("aida.image.header"));
    header->setFixedHeight(theme::tokens().table.header_h);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(theme::tokens().spacing.md, 0,
        theme::tokens().spacing.md, 0);
    name_label_ = new QLabel(header);
    info_label_ = new QLabel(header);
    header_layout->addWidget(name_label_, 1);
    header_layout->addWidget(info_label_, 0);
    layout->addWidget(header);
    canvas_ = new AidaImageCanvas(this);
    layout->addWidget(canvas_, 1);
    state_view_ = new widgets::AidaStateView(this);
    state_view_->setObjectName(QStringLiteral("aida.image.state_view"));
    layout->addWidget(state_view_, 1);
    connect(canvas_, &AidaImageCanvas::zoomChanged, this, [this](qreal) {
        refreshInfoLabel();
    });
    syncCanvasState();
}

AidaImageView::~AidaImageView() = default;

QString AidaImageView::lastError() const
{
    return error_;
}

bool AidaImageView::isImageAdmission(const QString& extension_lower)
{
    static const QSet<QString> supported = [] {
        QSet<QString> result;
        const QList<QByteArray> formats = QImageReader::supportedImageFormats();
        for (const QByteArray& format : formats)
            result.insert(QString::fromLatin1(format.toLower()));
        return result;
    }();
    return supported.contains(extension_lower);
}

bool AidaImageView::load(const QString& path)
{
    clear();
    path_ = path;
    const int slash = path.lastIndexOf(QLatin1Char('/')) > 0
        ? path.lastIndexOf(QLatin1Char('/'))
        : path.lastIndexOf(QLatin1Char('\\'));
    filename_ = slash >= 0 ? path.mid(slash + 1) : path;
    loading_.store(true, std::memory_order_release);
    active_.store(true, std::memory_order_release);
    name_label_->setText(filename_);
    refreshInfoLabel();

    diag::log_tagged_fmt("qt_image_view", "load_begin path=%s", path.toUtf8().constData());
    const quint64 serial = load_serial_.fetch_add(1, std::memory_order_acq_rel) + 1;
    aida::infra::executor::submission_t submission;
    submission.owner_subsystem = "image_view";
    submission.label = "image_view.decode";
    submission.thread_class = "bounded_task";
    submission.domain = aida::infra::executor::domain_t::feature_worker;
    submission.priority = 3;
    submission.body = [path, serial, this]() {
        QImage image;
        QString error;
        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (!reader.read(&image))
            error = reader.errorString();
        const bool posted = QMetaObject::invokeMethod(this, [this, serial, path,
                image = std::move(image), error]() mutable {
            onDecodeResult(serial, path, std::move(image), error);
        }, Qt::QueuedConnection);
        if (!posted) {
            diag::log_tagged_fmt("qt_image_view",
                "decode_result_dispatch_failed path=%s", path.toUtf8().constData());
        }
    };
    const auto submitted = aida::infra::executor::submit(std::move(submission));
    if (!submitted.submitted) {
        loading_.store(false, std::memory_order_release);
        error_ = QString::fromStdString("The image decode worker could not be scheduled: " +
            submitted.reject_reason);
        refreshInfoLabel();
        return false;
    }
    return true;
}

void AidaImageView::onDecodeResult(quint64 serial, QString path, QImage image, QString error)
{
    if (load_serial_.load(std::memory_order_acquire) != serial)
        return;
    loading_.store(false, std::memory_order_release);
    if (!error.isEmpty()) {
        error_ = error;
        ready_.store(false, std::memory_order_release);
        canvas_->clearImage();
        refreshInfoLabel();
        Q_EMIT loadFinished(path, false);
        return;
    }
    error_.clear();
    canvas_->setImage(image);
    ready_.store(true, std::memory_order_release);
    refreshInfoLabel();
    diag::log_tagged_fmt("qt_image_view", "load_done path=%s w=%d h=%d",
        path.toUtf8().constData(), image.width(), image.height());
    Q_EMIT loadFinished(path, true);
}

void AidaImageView::clear()
{
    canvas_->clearImage();
    path_.clear();
    filename_.clear();
    error_.clear();
    name_label_->clear();
    info_label_->clear();
    loading_.store(false, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
    syncCanvasState();
}

void AidaImageView::syncCanvasState()
{
    const bool loading = loading_.load(std::memory_order_acquire);
    const bool show_state = loading || !canvas_->hasImage();
    canvas_->setVisible(!show_state);
    state_view_->setVisible(show_state);
    if (!show_state)
        return;
    if (loading) {
        state_view_->setState(widgets::AidaStateView::State::Loading);
        state_view_->setTitle(QStringLiteral("Decoding image"));
        state_view_->setMessage(QStringLiteral(
            "The image is being decoded on a bounded worker thread."));
        state_view_->setActionLabel(QString());
    } else if (!error_.isEmpty()) {
        state_view_->setState(widgets::AidaStateView::State::Error);
        state_view_->setTitle(QStringLiteral("Image could not be decoded"));
        state_view_->setMessage(error_);
        state_view_->setActionLabel(QString());
    } else {
        state_view_->setState(widgets::AidaStateView::State::Empty);
        state_view_->setTitle(QStringLiteral("No image loaded"));
        state_view_->setMessage(QStringLiteral(
            "Open an image artifact to inspect it here."));
        state_view_->setActionLabel(QString());
    }
}

void AidaImageView::refreshInfoLabel()
{
    if (loading_.load(std::memory_order_acquire)) {
        info_label_->setText(QStringLiteral("Decoding image..."));
        syncCanvasState();
        return;
    }
    if (!error_.isEmpty()) {
        info_label_->setText(QStringLiteral("Image error: %1").arg(error_));
        syncCanvasState();
        return;
    }
    if (canvas_->hasImage()) {
        info_label_->setText(QStringLiteral("%1x%2  zoom %3%")
            .arg(canvas_->imageSize().width())
            .arg(canvas_->imageSize().height())
            .arg(qRound(canvas_->zoom() * 100.0)));
        syncCanvasState();
        return;
    }
    info_label_->clear();
    syncCanvasState();
}

}
