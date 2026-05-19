// feeds-chat-overlay-source.cpp — Feeds Chat Overlay OBS source.
//
// Renders a persistent Twitch-style chat overlay: small circular avatar
// followed by bold-orange username and plain-white message text, stacked
// vertically with the newest message at the bottom. Semi-transparent dark
// background for readability over busy stream content.
//
// Mirrors the popup source's two-phase texrender pipeline (paint to QImage
// on CPU, upload to gs_texture, composite via gs_texrender for bbox
// clipping) but skips the animation state machine — the overlay is
// always-on while in scene, no toggle.
//
// O1 wires source type, default position (top-right, 25% of canvas wide),
// and renders a hardcoded list of test messages so layout/scaling can be
// validated visually. O2 replaces the test list with a real history fed
// by the chat IPC handler. Subsequent commits add scroll behaviour (O3)
// and message-arrival animation (O4).

#include "feeds-chat-overlay-source.h"

#include <obs-module.h>
#include <graphics/graphics.h>

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QString>
#include <QTextDocument>

#include <algorithm>
#include <cstddef>
#include <map>
#include <mutex>
#include <vector>

// Avatar cache lives in plugin-main.cpp — externally linked so both the
// popup and the overlay sources can consume it. Same lookup-by-sender-id
// pattern the popup uses.
extern std::mutex                       g_avatarCacheMutex;
extern std::map<unsigned int, QImage>   g_avatarCache;
extern QImage                           g_fallbackAvatar;

namespace {

// ---------------------------------------------------------------------------
// Reference layout at 1080p (canvas 1920×1080, overlay 480 px = 25% wide).
// All pixel dimensions multiply through `scale = overlayWidth / 480.0f`
// inside RenderOverlayToImage so the overlay keeps the same proportions
// at any canvas resolution — same idiom the popup uses for its 1536/1920
// reference.
// ---------------------------------------------------------------------------
static constexpr int MAX_VISIBLE_MESSAGES = 6;

// Width = 25% of the active canvas. Queried per render so the overlay
// re-sizes correctly if the streamer changes canvas resolution. Falls
// back to 480 (25% of 1920) if obs_get_video_info fails — only
// possible before OBS finishes initialising.
static uint32_t GetOverlayCanvasWidth() {
    struct obs_video_info ovi;
    if (obs_get_video_info(&ovi)) {
        return (uint32_t)((float)ovi.base_width * 0.25f);
    }
    return 480;
}

// ---------------------------------------------------------------------------
// Message record. Shared shape with what the popup consumes from the dock,
// so a future centralised history module can hand the same record type to
// both sources without translation. O1 uses sender_name + content for
// rendering; sender_id drives the avatar lookup; timestamp is reserved for
// O2/O3 ordering and trimming.
// ---------------------------------------------------------------------------
struct OverlayMessage {
    unsigned int sender_id   = 0;
    QString      sender_name;
    QString      content;
    qint64       timestamp   = 0;
};

// Hardcoded test data. Replaced in O2 by real chat history pushed in from
// the chat IPC handler. Five entries so wrapping behaviour, emoji rendering,
// and the bottom-newest stacking can be eyeballed at multiple canvas sizes
// without a meeting running.
static const std::vector<OverlayMessage> g_testMessages = {
    {1001, QStringLiteral("Alice"),   QStringLiteral("Hello everyone!"),                              1779200000},
    {1002, QStringLiteral("Bob"),     QStringLiteral("Good morning"),                                  1779200060},
    {1003, QStringLiteral("Charlie"), QStringLiteral("This is a longer test message to see wrapping"), 1779200120},
    {1004, QStringLiteral("Dana"),    QString::fromUtf8("\xF0\x9F\x91\x8B"),                           1779200180},  // 👋
    {1005, QStringLiteral("Eve"),     QStringLiteral("Looks good!"),                                   1779200240},
};

// ---------------------------------------------------------------------------
// Per-instance state. Same layout as the popup's data struct minus the
// visibility/animation fields — the overlay renders unconditionally while
// the scene-item is visible.
// ---------------------------------------------------------------------------
struct FeedsChatOverlayData {
    obs_source_t*   source        = nullptr;

    QImage          rendered_image;
    gs_texture_t*   texture       = nullptr;
    bool            texture_dirty = true;
    gs_texrender_t* texrender     = nullptr;

    // Fallback dimensions at 1080p — first RenderOverlayToImage call
    // queries OBS for the real canvas size and overwrites both. Pre-render
    // fcr_get_width / fcr_get_height queries see a sensible stand-in
    // rather than zero.
    uint32_t        width  = 480;
    uint32_t        height = 600;
};

// Instance registry — separate from the popup's so future commits can fan
// out new-message events to every overlay instance without walking popup
// instances too. O1 doesn't read this list, but the create/destroy hooks
// maintain it so O2 can lean on it directly.
std::mutex                              g_overlayInstancesMutex;
std::vector<FeedsChatOverlayData*>      g_overlayInstances;

// ---------------------------------------------------------------------------
// Repaint the overlay's QImage from the supplied message list. Newest
// messages render at the bottom; older messages stack upward and stop
// once the next row would push above the top padding (overflow is dropped
// rather than scrolled — scroll behaviour lands in O3).
// ---------------------------------------------------------------------------
static void RenderOverlayToImage(FeedsChatOverlayData* d,
                                 const std::vector<OverlayMessage>& messages) {
    const int   overlayWidth = (int)GetOverlayCanvasWidth();
    const float scale        = (float)overlayWidth / 480.0f;

    // Scaled layout values. Same `(int)(literal * scale)` idiom the popup
    // uses; the literals are the 1080p reference dimensions.
    const int BG_PADDING            = (int)(12 * scale);
    const int BG_CORNER_RADIUS      = (int)(4  * scale);
    const int ROW_SPACING           = (int)(8  * scale);
    const int AVATAR_SIZE           = (int)(50 * scale);
    const int ROW_INNER_SPACING     = (int)(10 * scale);
    const int USER_FONT_PIXEL_SIZE  = (int)(26 * scale);
    const int MSG_FONT_PIXEL_SIZE   = (int)(24 * scale);

    // Bbox height reserves MAX_VISIBLE_MESSAGES rows of bare avatar height
    // plus inter-row spacing plus background padding. Rows whose wrapped
    // text exceeds avatar height push upward and clip at the top — visible
    // by design (better to drop the oldest than to grow the bbox
    // unpredictably).
    const int rowsReserveH = MAX_VISIBLE_MESSAGES * AVATAR_SIZE +
                             (MAX_VISIBLE_MESSAGES - 1) * ROW_SPACING;
    const int totalHeight  = rowsReserveH + 2 * BG_PADDING;

    d->width  = (uint32_t)overlayWidth;
    d->height = (uint32_t)totalHeight;

    if (d->rendered_image.size() != QSize((int)d->width, (int)d->height) ||
        d->rendered_image.format() != QImage::Format_RGBA8888) {
        d->rendered_image = QImage((int)d->width, (int)d->height,
                                   QImage::Format_RGBA8888);
    }
    d->rendered_image.fill(Qt::transparent);

    QPainter p(&d->rendered_image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Semi-transparent dark background (alpha 153 ≈ 60% opacity).
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 153));
    QRect bgRect(0, 0, (int)d->width, (int)d->height);
    p.drawRoundedRect(bgRect, BG_CORNER_RADIUS, BG_CORNER_RADIUS);

    const size_t take = std::min<size_t>(messages.size(),
                                          (size_t)MAX_VISIBLE_MESSAGES);

    const int rowX     = BG_PADDING;
    const int rowWidth = (int)d->width  - 2 * BG_PADDING;
    const int textWidth = rowWidth - AVATAR_SIZE - ROW_INNER_SPACING;
    int       curY     = (int)d->height - BG_PADDING;

    // Iterate newest → oldest over the tail of the list, stacking upward
    // from the bottom-inside-padding edge.
    for (auto it = messages.rbegin();
         it != messages.rbegin() + (std::ptrdiff_t)take; ++it) {

        const OverlayMessage& m = *it;

        // Lay out "<bold orange username> <plain white message>" as a
        // single wrapped HTML paragraph so the username styling stays
        // inline with the message text (rather than forcing the message
        // to a new line). Mixed font sizes are honoured by the text
        // layout engine — lines size to the tallest run.
        QTextDocument doc;
        doc.setDocumentMargin(0);
        doc.setTextWidth(textWidth);
        const QString html = QStringLiteral(
            "<span style='color:#FFA500; font-weight:bold; font-size:%1px'>%2</span>"
            " "
            "<span style='color:#FFFFFF; font-size:%3px'>%4</span>")
            .arg(USER_FONT_PIXEL_SIZE)
            .arg(m.sender_name.toHtmlEscaped())
            .arg(MSG_FONT_PIXEL_SIZE)
            .arg(m.content.toHtmlEscaped());
        doc.setHtml(html);

        const int textHeight = (int)doc.size().height();
        const int rowHeight  = std::max(AVATAR_SIZE, textHeight);
        const int rowY       = curY - rowHeight;
        if (rowY < BG_PADDING) break;  // would clip above top — stop.

        // Circular avatar at the row's left edge.
        QImage avatar;
        {
            std::lock_guard<std::mutex> lock(g_avatarCacheMutex);
            auto cacheIt = g_avatarCache.find(m.sender_id);
            avatar = (cacheIt != g_avatarCache.end())
                       ? cacheIt->second
                       : g_fallbackAvatar;
        }
        const QRect avatarRect(rowX, rowY, AVATAR_SIZE, AVATAR_SIZE);
        p.save();
        QPainterPath circle;
        circle.addEllipse(avatarRect);
        p.setClipPath(circle);
        if (!avatar.isNull()) {
            p.drawImage(avatarRect,
                        avatar.scaled(AVATAR_SIZE, AVATAR_SIZE,
                                      Qt::KeepAspectRatioByExpanding,
                                      Qt::SmoothTransformation));
        } else {
            p.fillRect(avatarRect, QColor(120, 120, 120));
        }
        p.restore();

        // Text block to the right of the avatar.
        p.save();
        p.translate(rowX + AVATAR_SIZE + ROW_INNER_SPACING, rowY);
        doc.drawContents(&p);
        p.restore();

        curY = rowY - ROW_SPACING;
    }

    p.end();
}

// ---------------------------------------------------------------------------
// Upload the current QImage to GPU. Called from video_render when dirty;
// graphics context is already active (custom-draw path).
// ---------------------------------------------------------------------------
static void RegenerateTexture(FeedsChatOverlayData* d) {
    RenderOverlayToImage(d, g_testMessages);

    const QImage& img = d->rendered_image;
    const uint8_t* pixels = nullptr;
    std::vector<uint8_t> tight;
    if (img.bytesPerLine() == (int)d->width * 4) {
        pixels = img.constBits();
    } else {
        tight.resize((size_t)d->width * d->height * 4);
        for (uint32_t y = 0; y < d->height; ++y) {
            memcpy(tight.data() + (size_t)y * d->width * 4,
                   img.constScanLine((int)y),
                   (size_t)d->width * 4);
        }
        pixels = tight.data();
    }

    if (d->texture) {
        gs_texture_destroy(d->texture);
        d->texture = nullptr;
    }
    d->texture = gs_texture_create(d->width, d->height, GS_RGBA, 1,
                                   &pixels, 0);
    d->texture_dirty = false;
}

// ---------------------------------------------------------------------------
// OBS source callbacks. `fcr_` prefix for "Feeds Chat oveRlay" — keeps
// the popup's `fcp_` and the overlay's `fcr_` visibly distinct in tracebacks.
// ---------------------------------------------------------------------------
static const char* fcr_get_name(void*) {
    // Z-prefix sorts the overlay next to Zoom Participant / Zoom
    // Screenshare / Zoom Chat Popup in the picker.
    return "Zoom Chat Overlay";
}

static void* fcr_create(obs_data_t* /*settings*/, obs_source_t* source) {
    FeedsChatOverlayData* d = new FeedsChatOverlayData();
    d->source = source;

    obs_enter_graphics();
    d->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    obs_leave_graphics();

    {
        std::lock_guard<std::mutex> lock(g_overlayInstancesMutex);
        g_overlayInstances.push_back(d);
    }
    return d;
}

static void fcr_destroy(void* data) {
    FeedsChatOverlayData* d = static_cast<FeedsChatOverlayData*>(data);
    if (!d) return;

    {
        std::lock_guard<std::mutex> lock(g_overlayInstancesMutex);
        g_overlayInstances.erase(
            std::remove(g_overlayInstances.begin(), g_overlayInstances.end(), d),
            g_overlayInstances.end());
    }

    if (d->texture || d->texrender) {
        obs_enter_graphics();
        if (d->texture) {
            gs_texture_destroy(d->texture);
            d->texture = nullptr;
        }
        if (d->texrender) {
            gs_texrender_destroy(d->texrender);
            d->texrender = nullptr;
        }
        obs_leave_graphics();
    }
    delete d;
}

static uint32_t fcr_get_width(void* data) {
    FeedsChatOverlayData* d = static_cast<FeedsChatOverlayData*>(data);
    return d ? d->width : 0;
}

static uint32_t fcr_get_height(void* data) {
    FeedsChatOverlayData* d = static_cast<FeedsChatOverlayData*>(data);
    return d ? d->height : 0;
}

static obs_properties_t* fcr_get_properties(void* /*data*/) {
    return obs_properties_create();
}

static void fcr_video_render(void* data, gs_effect_t* /*effect*/) {
    FeedsChatOverlayData* d = static_cast<FeedsChatOverlayData*>(data);
    if (!d) return;

    if (d->texture_dirty || !d->texture) {
        RegenerateTexture(d);
    }
    if (!d->texture) return;

    // Two-phase texrender pipeline mirrors the popup's: phase 1 copies
    // the source texture into a bbox-sized intermediate with replace
    // blending (preserves the painted alpha verbatim), phase 2 composites
    // that intermediate into the scene with standard SRCALPHA blending.
    // Even without an active animation the intermediate gives us
    // automatic bbox clipping for free, which matters once O3 adds
    // scroll-out behaviour at the top edge.
    if (d->texrender) {
        gs_texrender_reset(d->texrender);
        if (gs_texrender_begin(d->texrender, d->width, d->height)) {
            struct vec4 clear_color = { 0.0f, 0.0f, 0.0f, 0.0f };
            gs_clear(GS_CLEAR_COLOR, &clear_color, 1.0f, 0);
            gs_ortho(0.0f, (float)d->width,
                     0.0f, (float)d->height,
                     -100.0f, 100.0f);

            gs_blend_state_push();
            gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

            gs_effect_t* eff   = obs_get_base_effect(OBS_EFFECT_DEFAULT);
            gs_eparam_t* param = gs_effect_get_param_by_name(eff, "image");
            gs_effect_set_texture(param, d->texture);
            while (gs_effect_loop(eff, "Draw")) {
                gs_draw_sprite(d->texture, 0, d->width, d->height);
            }

            gs_blend_state_pop();
            gs_texrender_end(d->texrender);
        }

        gs_texture_t* clipped = gs_texrender_get_texture(d->texrender);
        if (clipped) {
            gs_blend_state_push();
            gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

            gs_effect_t* eff   = obs_get_base_effect(OBS_EFFECT_DEFAULT);
            gs_eparam_t* param = gs_effect_get_param_by_name(eff, "image");
            gs_effect_set_texture(param, clipped);
            while (gs_effect_loop(eff, "Draw")) {
                gs_draw_sprite(clipped, 0, d->width, d->height);
            }

            gs_blend_state_pop();
        }
        return;
    }

    // Fallback (texrender allocation failed): direct draw with alpha
    // blend. Visually correct so long as nothing tries to clip beyond
    // the bbox; good enough that the source still works under
    // graphics-memory pressure.
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

    gs_effect_t* eff   = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t* param = gs_effect_get_param_by_name(eff, "image");
    gs_effect_set_texture(param, d->texture);
    while (gs_effect_loop(eff, "Draw")) {
        gs_draw_sprite(d->texture, 0, d->width, d->height);
    }

    gs_blend_state_pop();
}

static struct obs_source_info feeds_chat_overlay_info = {};

}  // namespace

namespace feeds {

void RegisterChatOverlaySource() {
    feeds_chat_overlay_info.id             = "feeds_chat_overlay";
    feeds_chat_overlay_info.type           = OBS_SOURCE_TYPE_INPUT;
    feeds_chat_overlay_info.output_flags   = OBS_SOURCE_VIDEO |
                                             OBS_SOURCE_CUSTOM_DRAW;
    feeds_chat_overlay_info.get_name       = fcr_get_name;
    feeds_chat_overlay_info.create         = fcr_create;
    feeds_chat_overlay_info.destroy        = fcr_destroy;
    feeds_chat_overlay_info.get_width      = fcr_get_width;
    feeds_chat_overlay_info.get_height     = fcr_get_height;
    feeds_chat_overlay_info.get_properties = fcr_get_properties;
    feeds_chat_overlay_info.video_render   = fcr_video_render;
    feeds_chat_overlay_info.icon_type      = OBS_ICON_TYPE_TEXT;
    obs_register_source(&feeds_chat_overlay_info);
}

}  // namespace feeds
