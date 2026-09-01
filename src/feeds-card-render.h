#pragma once

#include <cstdint>

#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QString>

#include <graphics/graphics.h>

// Shared card-drawing primitives for the two Feeds "card" sources — the chat
// popup (feeds_chat_popup) and the lower third (feeds_lower_third). Both draw
// the same visual object: a dark rounded card with a hard drop shadow, an
// orange accent, a circular avatar with a ring, and an orange name pill.
//
// Everything here was lifted verbatim out of feeds-chat-popup-source.cpp. The
// extraction is deliberately mechanical — each routine reproduces the popup's
// original drawing and blending exactly, and the popup now calls these instead
// of its own inline copies — so the shipped popup's behaviour is unchanged and
// the two sources cannot drift apart.
//
// Threading: the QPainter helpers are pure CPU drawing and run on whatever
// thread owns the QImage. UploadImageToTexture and DrawClippedSlide touch the
// GPU and require an active OBS graphics context (i.e. call them from
// video_render, which already runs on the graphics thread).

namespace feeds {
namespace card {

// Shared palette. The card body is a near-black rounded rect; the accent is
// the orange used for both the avatar ring and the name pill.
inline constexpr const char* kCardBodyColor      = "#222222";
inline constexpr const char* kAccentColor        = "#FFA500";
// Drop shadow is a hard offset copy of the card at this alpha (no gaussian).
inline constexpr int         kShadowAlpha        = 80;
// Flat neutral disc drawn in place of a null avatar.
inline constexpr int         kAvatarFallbackGrey = 120;

// Slide animation duration per direction, shared by both sources. Ease-out
// cubic: smooth deceleration on arrival, smooth acceleration on departure.
inline constexpr float kSlideDurationSeconds = 0.5f;

// Ease-out cubic. Decelerates as t approaches 1 — a smooth landing on the way
// in, and a smooth lift-off on the way out when the curve runs in reverse.
float EaseOutCubic(float t);

// Drop shadow then card body, in that order so the body draws over its own
// shadow. Leaves the painter with NoPen and the body brush set — callers set
// their own pen/brush before drawing text (the popup relied on exactly this).
void DrawCardBody(QPainter& p, const QRect& rect, int cornerRadius,
                  int shadowOffset);

// Circular-clipped avatar with an accent ring. A null image renders the flat
// neutral disc instead. `rect` is expected to be square.
//
// The ring is inset by half its stroke width so it sits visually centred on
// the circle's edge: Qt strokes are centre-aligned by default, so without the
// inset the outer half clips beyond the bounding box. Both the stroke width
// and the inset are clamped to at least 1px so the ring never disappears at
// sub-1080p canvas scales.
void DrawAvatarCircle(QPainter& p, const QRect& rect, const QImage& avatar,
                      int borderWidth);

// Accent-coloured rounded pill sized to fit `text`, with the card body colour
// as its text colour. hPad / vPad are PER SIDE, so the pill's footprint is
// textWidth + 2*hPad wide. The pill font is derived from the painter's current
// font (family and style preserved, forced bold at fontPixelSize) — matching
// how the popup built its pill font from the message font. Returns the rect it
// drew into so a caller can lay out beneath or beside it.
QRect DrawPill(QPainter& p, const QPoint& topLeft, const QString& text,
               int fontPixelSize, int hPad, int vPad, int cornerRadius);

// Upload an RGBA8888 QImage to a fresh gs_texture, destroying whatever `tex`
// currently points at first (and repointing it at the new texture, or at null
// on failure).
//
// QImage rows are normally tightly packed at 4 bytes per pixel for RGBA8888,
// so the fast path hands libobs the image's own bits. The per-scanline memcpy
// is defence in depth against a strided image (unusual width, debug build,
// future Qt version). Graphics context required.
void UploadImageToTexture(const QImage& img, uint32_t width, uint32_t height,
                          gs_texture_t*& tex);

// Composite `tex` into the scene translated by (xOffset, yOffset) and clipped
// to a width x height box — the "slide out from behind an invisible barrier"
// effect. Pass a Y offset for the popup's slide-up, an X offset for the lower
// third's slide-in-from-the-left; both at once works too.
//
// OBS_SOURCE_CUSTOM_DRAW sources draw straight to the scene render target with
// no per-source clipping, so a raw translated draw would show the card
// spilling outside its declared bounding box. Routing through an intermediate
// texrender lets the GPU scissor discard whatever lands outside it.
//
// Two blend subtleties, both of which were real bugs:
//   * Phase 1 writes into the pre-cleared (fully transparent) texrender with
//     REPLACE blending (src=ONE, dst=ZERO), writing the card's RGBA verbatim.
//     Normal SRCALPHA/INVSRCALPHA against a transparent destination squares
//     the alpha — dst.a = src.a * src.a — which quietly made the drop shadow
//     roughly 3x too transparent after the final composite.
//   * Phase 2 uses a SEPARATE alpha factor (src_a = ONE), matching OBS's own
//     default composite blend (gs_reset_blend_state). The non-separate form
//     squares the destination alpha, corrupting the accumulated alpha of a
//     nested scene so its opaque siblings composite semi-transparent into the
//     parent scene.
//
// A null `texrender` (allocation failed at create time) falls back to a direct
// translated draw. The card is then visible outside its box during the slide,
// which is better than not rendering at all. Graphics context required.
void DrawClippedSlide(gs_texrender_t* texrender, gs_texture_t* tex,
                      uint32_t width, uint32_t height,
                      float xOffset, float yOffset);

}  // namespace card
}  // namespace feeds
