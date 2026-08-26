#pragma once

#include <QColor>
#include <QFontMetricsF>
#include <QPainter>
#include <QPen>
#include <QRectF>

#include <algorithm>

#include "../theme/aida_tokens.hpp"

namespace aida::qt::widgets {

enum class AidaSemantic {
    Neutral,
    Success,
    Warning,
    Error,
    Info,
    Accent
};

inline qreal clamp01(qreal v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

inline QColor with_alpha(QColor c, qreal factor)
{
    c.setAlphaF(clamp01(c.alphaF() * factor));
    return c;
}

inline QColor mix_colors(const QColor& a, const QColor& b, qreal t)
{
    const qreal u = 1.0 - t;
    QColor out;
    out.setRedF(clamp01(a.redF() * u + b.redF() * t));
    out.setGreenF(clamp01(a.greenF() * u + b.greenF() * t));
    out.setBlueF(clamp01(a.blueF() * u + b.blueF() * t));
    out.setAlphaF(clamp01(a.alphaF() * u + b.alphaF() * t));
    return out;
}

inline QColor lighten_color(const QColor& c, int amount)
{
    const auto clamp8 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    return QColor(clamp8(c.red() + amount), clamp8(c.green() + amount),
        clamp8(c.blue() + amount), c.alpha());
}

inline QColor semantic_color(AidaSemantic kind)
{
    const auto& t = aida::qt::theme::tokens();
    switch (kind) {
    case AidaSemantic::Success: return t.success;
    case AidaSemantic::Warning: return t.warning;
    case AidaSemantic::Error:   return t.error;
    case AidaSemantic::Info:    return t.info;
    case AidaSemantic::Accent:  return t.accent;
    case AidaSemantic::Neutral: return t.text_secondary;
    }
    return t.text_secondary;
}

inline QColor semantic_soft_color(AidaSemantic kind)
{
    const auto& t = aida::qt::theme::tokens();
    switch (kind) {
    case AidaSemantic::Success: return t.success_soft;
    case AidaSemantic::Warning: return t.warning_soft;
    case AidaSemantic::Error:   return t.error_soft;
    case AidaSemantic::Info:    return t.info_soft;
    case AidaSemantic::Accent:  return with_alpha(t.accent, 0.24);
    case AidaSemantic::Neutral: return with_alpha(t.text_secondary, 0.12);
    }
    return with_alpha(t.text_secondary, 0.12);
}

inline const char* semantic_variant_name(AidaSemantic kind)
{
    switch (kind) {
    case AidaSemantic::Success: return "success";
    case AidaSemantic::Warning: return "warning";
    case AidaSemantic::Error:   return "error";
    case AidaSemantic::Info:    return "info";
    case AidaSemantic::Accent:  return "accent";
    case AidaSemantic::Neutral: return "neutral";
    }
    return "neutral";
}

inline const char* semantic_state_name(AidaSemantic kind)
{
    switch (kind) {
    case AidaSemantic::Success: return "live";
    case AidaSemantic::Warning: return "stale";
    case AidaSemantic::Error:   return "breakpoint";
    case AidaSemantic::Info:    return "changed";
    case AidaSemantic::Accent:  return "running";
    case AidaSemantic::Neutral: return nullptr;
    }
    return nullptr;
}

inline QColor semantic_fill_color(AidaSemantic kind)
{
    const auto& t = aida::qt::theme::tokens();
    switch (kind) {
    case AidaSemantic::Success: return t.success_fill;
    case AidaSemantic::Warning: return t.warning_fill;
    case AidaSemantic::Error:   return t.error_fill;
    case AidaSemantic::Info:    return t.info_fill;
    case AidaSemantic::Accent:  return t.accent_fill;
    case AidaSemantic::Neutral: return t.neutral_fill;
    }
    return t.neutral_fill;
}

inline QColor semantic_edge_color(AidaSemantic kind)
{
    const auto& t = aida::qt::theme::tokens();
    switch (kind) {
    case AidaSemantic::Success: return t.success_edge;
    case AidaSemantic::Warning: return t.warning_edge;
    case AidaSemantic::Error:   return t.error_edge;
    case AidaSemantic::Info:    return t.info_edge;
    case AidaSemantic::Accent:  return t.accent_edge;
    case AidaSemantic::Neutral: return t.neutral_edge;
    }
    return t.neutral_edge;
}

inline qreal semantic_fill_alpha()
{
    return aida::qt::theme::tokens().accent_fill.alphaF();
}

inline qreal semantic_edge_alpha()
{
    return aida::qt::theme::tokens().accent_edge.alphaF();
}

inline qreal relative_luminance(const QColor& c)
{
    return 0.299 * c.redF() + 0.587 * c.greenF() + 0.114 * c.blueF();
}

inline void paint_border(QPainter& p, const QRectF& face, qreal radius, const QColor& color)
{
    const qreal w = (std::max)(1.0, qreal(aida::qt::theme::tokens().panel.border));
    const qreal half = w * 0.5;
    const qreal r = (std::max)(0.0, radius - half);
    QPen pen(color, w);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawRoundedRect(face.adjusted(half, half, -half, -half), r, r);
}

inline void paint_focus_ring(QPainter& p, const QRectF& face, qreal radius, qreal alpha_factor)
{
    const auto& t = aida::qt::theme::tokens();
    const qreal w = (std::max)(1.0, qreal(t.control.focus_ring));
    const qreal offset = w * 0.5 + 0.5;
    const QColor ring = with_alpha(t.border_focus, alpha_factor);
    QPen pen(ring, w);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawRoundedRect(face.adjusted(-offset, -offset, offset, offset),
        radius + offset, radius + offset);
}

inline void paint_focus_ring_inner(QPainter& p, const QRectF& frame, qreal radius, qreal alpha_factor)
{
    const auto& t = aida::qt::theme::tokens();
    const qreal w = (std::max)(1.0, qreal(t.control.focus_ring));
    const qreal inset = w * 0.5 + 0.5;
    const qreal r = (std::max)(0.0, radius - inset);
    const QColor ring = with_alpha(t.border_focus, alpha_factor);
    QPen pen(ring, w);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawRoundedRect(frame.adjusted(inset, inset, -inset, -inset), r, r);
}

inline qreal text_baseline_centered(const QRectF& rect, const QFontMetricsF& fm)
{
    return rect.top() + (rect.height() - fm.height()) * 0.5 + fm.ascent();
}

}
