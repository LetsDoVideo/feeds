// feeds-card-render.cpp — shared card-drawing primitives.
//
// Extracted from feeds-chat-popup-source.cpp so the chat popup and the lower
// third render the same card and share the non-obvious GPU work. See the
// header for the contract and for the two blend subtleties in
// DrawClippedSlide; the comments here stay light because the header carries
// the reasoning.

#include "feeds-card-render.h"

#include <obs-module.h>   // obs_get_base_effect

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cstring>
#include <vector>

namespace feeds {
namespace card {

float EaseOutCubic(float t) {
    float f = 1.0f - t;
    return 1.0f - f * f * f;
}

void DrawCardBody(QPainter& p, const QRect& rect, int cornerRadius,
                  int shadowOffset) {
    // Drop shadow first so the body draws over it.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, kShadowAlpha));
    p.drawRoundedRect(rect.translated(shadowOffset, shadowOffset),
                      cornerRadius, cornerRadius);

    p.setBrush(QColor(kCardBodyColor));
    p.drawRoundedRect(rect, cornerRadius, cornerRadius);
}

void DrawAvatarCircle(QPainter& p, const QRect& rect, const QImage& avatar,
                      int borderWidth) {
    const int stroke = std::max(1, borderWidth);
    const int inset  = std::max(1, stroke / 2);

    p.save();
    QPainterPath circle;
    circle.addEllipse(rect);
    p.setClipPath(circle);
    if (!avatar.isNull()) {
        p.drawImage(rect,
                    avatar.scaled(rect.width(), rect.height(),
                                  Qt::KeepAspectRatioByExpanding,
                                  Qt::SmoothTransformation));
    } else {
        p.fillRect(rect, QColor(kAvatarFallbackGrey, kAvatarFallbackGrey,
                                kAvatarFallbackGrey));
    }
    p.restore();

    QPen ring(QColor(kAccentColor));
    ring.setWidth(stroke);
    p.setPen(ring);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(rect.adjusted(inset, inset, -inset, -inset));
}

QRect DrawPill(QPainter& p, const QPoint& topLeft, const QString& text,
               int fontPixelSize, int hPad, int vPad, int cornerRadius) {
    QFont pillFont = p.font();
    pillFont.setBold(true);
    pillFont.setPixelSize(fontPixelSize);
    p.setFont(pillFont);

    QFontMetrics pillFm(pillFont);
    const int textWidth  = pillFm.horizontalAdvance(text);
    const int textHeight = pillFm.height();

    const QRect pillRect(topLeft.x(), topLeft.y(),
                         textWidth  + 2 * hPad,
                         textHeight + 2 * vPad);

    p.setBrush(QColor(kAccentColor));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(pillRect, cornerRadius, cornerRadius);
    p.setPen(QColor(kCardBodyColor));
    p.drawText(pillRect, Qt::AlignCenter, text);

    return pillRect;
}

void UploadImageToTexture(const QImage& img, uint32_t width, uint32_t height,
                          gs_texture_t*& tex) {
    const uint8_t* pixels = nullptr;
    std::vector<uint8_t> tight;
    if (img.bytesPerLine() == (int)width * 4) {
        pixels = img.constBits();
    } else {
        tight.resize((size_t)width * height * 4);
        for (uint32_t y = 0; y < height; ++y) {
            memcpy(tight.data() + (size_t)y * width * 4,
                   img.constScanLine((int)y),
                   (size_t)width * 4);
        }
        pixels = tight.data();
    }

    if (tex) {
        gs_texture_destroy(tex);
        tex = nullptr;
    }
    tex = gs_texture_create(width, height, GS_RGBA, 1, &pixels, 0);
}

void DrawClippedSlide(gs_texrender_t* texrender, gs_texture_t* tex,
                      uint32_t width, uint32_t height,
                      float xOffset, float yOffset) {
    if (!tex) return;

    if (texrender) {
        // Phase 1: render the card (with its translation) into a box-sized
        // intermediate texture. Anything translated outside (0,0)-(w,h) is
        // discarded by the texrender's scissor.
        gs_texrender_reset(texrender);
        if (gs_texrender_begin(texrender, width, height)) {
            struct vec4 clear_color = { 0.0f, 0.0f, 0.0f, 0.0f };
            gs_clear(GS_CLEAR_COLOR, &clear_color, 1.0f, 0);
            gs_ortho(0.0f, (float)width,
                     0.0f, (float)height,
                     -100.0f, 100.0f);

            gs_blend_state_push();
            gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

            gs_matrix_push();
            gs_matrix_translate3f(xOffset, yOffset, 0.0f);

            gs_effect_t* eff   = obs_get_base_effect(OBS_EFFECT_DEFAULT);
            gs_eparam_t* param = gs_effect_get_param_by_name(eff, "image");
            gs_effect_set_texture(param, tex);
            while (gs_effect_loop(eff, "Draw")) {
                gs_draw_sprite(tex, 0, width, height);
            }

            gs_matrix_pop();
            gs_blend_state_pop();

            gs_texrender_end(texrender);
        }

        // Phase 2: composite the (clipped) intermediate into the scene at the
        // source's natural position — no translation here, the slide offset is
        // already baked into the texrender contents.
        gs_texture_t* clipped = gs_texrender_get_texture(texrender);
        if (clipped) {
            gs_blend_state_push();
            gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA,
                                       GS_BLEND_ONE,      GS_BLEND_INVSRCALPHA);

            gs_effect_t* eff   = obs_get_base_effect(OBS_EFFECT_DEFAULT);
            gs_eparam_t* param = gs_effect_get_param_by_name(eff, "image");
            gs_effect_set_texture(param, clipped);
            while (gs_effect_loop(eff, "Draw")) {
                gs_draw_sprite(clipped, 0, width, height);
            }

            gs_blend_state_pop();
        }
        return;
    }

    // Fallback: texrender allocation failed at create time. Direct draw with
    // translation — the card is visible outside its box during the slide.
    gs_blend_state_push();
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA,
                               GS_BLEND_ONE,      GS_BLEND_INVSRCALPHA);

    gs_matrix_push();
    gs_matrix_translate3f(xOffset, yOffset, 0.0f);

    gs_effect_t* eff   = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t* param = gs_effect_get_param_by_name(eff, "image");
    gs_effect_set_texture(param, tex);
    while (gs_effect_loop(eff, "Draw")) {
        gs_draw_sprite(tex, 0, width, height);
    }

    gs_matrix_pop();
    gs_blend_state_pop();
}

}  // namespace card
}  // namespace feeds
