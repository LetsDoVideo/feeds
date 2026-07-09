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
#include "feeds-version.h"

#include <obs-module.h>
#include <graphics/graphics.h>

#include <QDesktopServices>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QString>
#include <QTextDocument>
#include <QUrl>

#include <algorithm>
#include <cstddef>
#include <map>
#include <mutex>
#include <vector>

// Avatar caches live in plugin-main.cpp — externally linked so the overlay can
// consume them at render time. Zoom messages look up by uint sender id (falls
// back to the bundled Feeds logo); YouTube messages look up by string channel id
// in the parallel YT cache (a miss / null entry renders the neutral grey circle).
extern std::mutex                       g_avatarCacheMutex;
extern std::map<unsigned int, QImage>   g_avatarCache;
extern QImage                           g_fallbackAvatar;
extern std::mutex                       g_ytAvatarCacheMutex;
extern std::map<std::string, QImage>    g_ytAvatarCacheByChannel;
// Twitch avatar cache (keyed by user-id) — resolved via unofficial GraphQL in
// plugin-main; a miss / null entry renders the neutral grey circle.
extern std::mutex                       g_twAvatarCacheMutex;
extern std::map<std::string, QImage>    g_twAvatarCacheByUser;

// Tier state from plugin-main.cpp. Overlay is gated at Streamer (>= 2),
// enforced by ReconcileChatOverlaySources via the tier_disabled flag.
// (The create callback no longer gates on login/tier — it always creates a
// dormant source — so the login/popup-helper externs are no longer needed.)
extern int  g_currentTier;

// Tier threshold for the overlay. Streamer (2) and Broadcaster (3) get
// it; Free (0) and Basic (1) don't.
static constexpr int OVERLAY_MIN_TIER = 2;

namespace {

// ---------------------------------------------------------------------------
// OBS Convention B: the source declares its own pixel dimensions through
// editable properties; the scene-item handle drag scales the rendered
// output (same as obs-text). The user controls Width and Visible Messages
// directly; everything else (avatar size, font sizes, padding) is fixed
// at a reference design and does NOT scale with canvas. If the canvas is
// 720p, fonts/avatar render at the same pixel size — the user can resize
// the source via properties or scene-item handles.
// ---------------------------------------------------------------------------

// Settings keys used in obs_data_t blobs and properties UI.
static constexpr const char* S_WIDTH            = "width";
static constexpr const char* S_VISIBLE_MESSAGES = "visible_messages";
static constexpr const char* S_PLATFORM_FILTER  = "platform_filter";

// Per-instance platform filter values. BOTH is 0 (also the default an absent
// setting reads back as, so existing overlays default to Both). ZOOM/YOUTUBE/
// TWITCH equal feeds::ChatMsgOrigin's values so a non-Both filter is just
// "keep if (int)origin == filter".
enum { PLAT_BOTH = 0, PLAT_ZOOM = 1, PLAT_YOUTUBE = 2, PLAT_TWITCH = 3 };

// Width range and step. WIDTH_DEFAULT_PX is the property-system default
// (35% of 1920 ≈ 672 px) — a baseline for sources loaded from saves
// before the per-canvas override in ApplyChatOverlayDefaults runs.
static constexpr int WIDTH_DEFAULT_PX = 672;
static constexpr int WIDTH_MIN_PX     = 200;
static constexpr int WIDTH_MAX_PX     = 3000;
static constexpr int WIDTH_STEP_PX    = 10;

static constexpr int VISIBLE_DEFAULT = 6;
static constexpr int VISIBLE_MIN     = 1;
static constexpr int VISIBLE_MAX     = 20;

// Fixed reference-1080p layout values. These do NOT scale with canvas
// resolution — the user resizes the overlay by changing Width and
// Visible Messages in properties (or by accepting that handle-drag
// stretches, same as obs-text).
static constexpr int BG_PADDING           = 12;
static constexpr int BG_CORNER_RADIUS     = 4;
static constexpr int ROW_SPACING          = 8;
static constexpr int AVATAR_SIZE          = 50;
static constexpr int ROW_INNER_SPACING    = 10;
static constexpr int USER_FONT_PIXEL_SIZE = 26;
static constexpr int MSG_FONT_PIXEL_SIZE  = 24;

// ---------------------------------------------------------------------------
// Message record. Shared shape with what the popup consumes from the dock,
// so a future centralised history module can hand the same record type to
// both sources without translation. O1 uses sender_name + content for
// rendering; sender_id drives the avatar lookup; timestamp is reserved for
// O2/O3 ordering and trimming.
// ---------------------------------------------------------------------------
struct OverlayMessage {
    feeds::ChatMsgOrigin origin = feeds::ChatMsgOrigin::Zoom;
    unsigned int sender_id   = 0;   // Zoom avatar key (0 for YouTube)
    std::string  channel_id;        // YouTube avatar key ("" for Zoom)
    QString      sender_name;
    QString      content;
    qint64       timestamp   = 0;
};

// Bounded ring of recent chat messages — single source of truth for every
// overlay source instance. Mutated from the IPC reader thread (append /
// clear) and read from the graphics thread (render). HISTORY_CAP keeps
// memory predictable across long meetings; only the most recent few are
// typically visible, but extras are retained so a user-resized overlay
// can show more context without losing earlier lines.
static constexpr size_t       HISTORY_CAP = 50;

std::mutex                    g_overlayHistoryMutex;
std::vector<OverlayMessage>   g_overlayHistory;

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

    // True iff this instance is currently locked out by tier (logged in
    // at < OVERLAY_MIN_TIER). Set by feeds::ReconcileChatOverlaySources
    // on login_succeeded. When true, fcr_video_render no-ops and
    // fcr_get_properties returns an upgrade message.
    bool            tier_disabled     = false;

    // Configured via source properties — fcr_update mirrors the settings
    // here, and RenderOverlayToImage uses them as the layout extents.
    // Initial values match the get_defaults values so fcr_get_width /
    // fcr_get_height return something sensible before fcr_update runs.
    uint32_t        width             = (uint32_t)WIDTH_DEFAULT_PX;
    int             visible_messages  = VISIBLE_DEFAULT;
    int             platform_filter   = PLAT_BOTH;   // Zoom / YouTube / Both
    uint32_t        height            = (uint32_t)(
                                            VISIBLE_DEFAULT * AVATAR_SIZE +
                                            (VISIBLE_DEFAULT - 1) * ROW_SPACING +
                                            2 * BG_PADDING);
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
    // Bbox height reserves `visible_messages` rows of bare avatar height
    // plus inter-row spacing plus background padding. Rows whose wrapped
    // text exceeds avatar height push upward and clip at the top — by
    // design (better to drop the oldest than to grow the bbox
    // unpredictably).
    int visible = d->visible_messages;
    if (visible < VISIBLE_MIN) visible = VISIBLE_MIN;
    if (visible > VISIBLE_MAX) visible = VISIBLE_MAX;

    const int rowsReserveH = visible * AVATAR_SIZE +
                             (visible - 1) * ROW_SPACING;
    const int totalHeight  = rowsReserveH + 2 * BG_PADDING;

    // d->width is the user-configured Width from settings; height is
    // derived from Visible Messages here.
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

    // Apply this instance's platform filter to the shared history (BOTH keeps
    // everything; otherwise keep only the matching origin), preserving order so
    // the newest kept messages are still at the end.
    std::vector<const OverlayMessage*> filtered;
    filtered.reserve(messages.size());
    for (const OverlayMessage& m : messages) {
        if (d->platform_filter == PLAT_BOTH ||
            (int)m.origin == d->platform_filter)
            filtered.push_back(&m);
    }

    const size_t take = std::min<size_t>(filtered.size(),
                                          (size_t)visible);

    const int rowX     = BG_PADDING;
    const int rowWidth = (int)d->width  - 2 * BG_PADDING;
    const int textWidth = rowWidth - AVATAR_SIZE - ROW_INNER_SPACING;
    int       curY     = (int)d->height - BG_PADDING;

    // Iterate newest → oldest over the tail of the filtered list, stacking
    // upward from the bottom-inside-padding edge.
    for (auto it = filtered.rbegin();
         it != filtered.rbegin() + (std::ptrdiff_t)take; ++it) {

        const OverlayMessage& m = **it;

        // Lay out "<bold orange username> <plain white message>" as a
        // single wrapped HTML paragraph so the username styling stays
        // inline with the message text (rather than forcing the message
        // to a new line). Mixed font sizes are honoured by the text
        // layout engine — lines size to the tallest run.
        QTextDocument doc;
        doc.setDocumentMargin(0);
        doc.setTextWidth(textWidth);
        // Colon is inside the bold/orange span so it shares the username's
        // styling; the space between spans is plain so the colon hugs the
        // username and the message text reads as "Alice: Hello everyone!".
        const QString html = QStringLiteral(
            "<span style='color:#FFA500; font-weight:bold; font-size:%1px'>%2:</span>"
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

        // Circular avatar at the row's left edge — resolved from the origin-
        // appropriate cache. YouTube: channel-id cache. Twitch: user-id cache
        // (resolved via unofficial GraphQL). Either miss/null leaves `avatar` null
        // -> neutral circle. Zoom: sender-id cache, falling back to the Feeds logo.
        QImage avatar;
        if (m.origin == feeds::ChatMsgOrigin::YouTube) {
            std::lock_guard<std::mutex> lock(g_ytAvatarCacheMutex);
            auto cacheIt = g_ytAvatarCacheByChannel.find(m.channel_id);
            if (cacheIt != g_ytAvatarCacheByChannel.end())
                avatar = cacheIt->second;
        } else if (m.origin == feeds::ChatMsgOrigin::Twitch) {
            std::lock_guard<std::mutex> lock(g_twAvatarCacheMutex);
            auto cacheIt = g_twAvatarCacheByUser.find(m.channel_id);
            if (cacheIt != g_twAvatarCacheByUser.end())
                avatar = cacheIt->second;
        } else {
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

// Mark every overlay instance as dirty so the next video_render rebuilds
// its texture from the current history. Called after history mutations
// from the IPC thread. Holding g_overlayInstancesMutex only — never the
// history mutex — keeps lock ordering simple.
static void MarkAllOverlayInstancesDirty() {
    std::lock_guard<std::mutex> lock(g_overlayInstancesMutex);
    for (FeedsChatOverlayData* d : g_overlayInstances) {
        // bool write read on graphics thread without explicit
        // synchronisation; worst case is a one-frame delay before the
        // new message appears, which is well below human perception.
        d->texture_dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Upload the current QImage to GPU. Called from video_render when dirty;
// graphics context is already active (custom-draw path). Snapshots the
// global history under g_overlayHistoryMutex so the paint sees a stable
// list even if the IPC thread appends mid-render. Doesn't clear
// texture_dirty itself — fcr_video_render does that under
// g_overlayInstancesMutex so a concurrent append from the IPC thread
// can't lose its dirty bit.
// ---------------------------------------------------------------------------
static void RegenerateTexture(FeedsChatOverlayData* d) {
    std::vector<OverlayMessage> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_overlayHistoryMutex);
        snapshot = g_overlayHistory;
    }
    RenderOverlayToImage(d, snapshot);

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
}

// ---------------------------------------------------------------------------
// OBS source callbacks. `fcr_` prefix for "Feeds Chat oveRlay" — keeps
// the popup's `fcp_` and the overlay's `fcr_` visibly distinct in tracebacks.
// ---------------------------------------------------------------------------
static const char* fcr_get_name(void*) {
    // "Feeds" prefix groups the overlay with Feeds Participant /
    // Feeds Screenshare / Feeds Chat Popup in the picker. Display name
    // only — the source ID ("feeds_chat_overlay") is the stable key.
    return "Feeds Chat Overlay";
}

// Populate baseline defaults into the settings blob before the source's
// create callback runs. The per-canvas Width override is applied by
// ApplyChatOverlayDefaults in plugin-main.cpp for fresh creations; this
// just guarantees a sensible value for sources loaded from saves before
// any other code touches their settings.
static void fcr_get_defaults(obs_data_t* settings) {
    obs_data_set_default_int(settings, S_WIDTH,            WIDTH_DEFAULT_PX);
    obs_data_set_default_int(settings, S_VISIBLE_MESSAGES, VISIBLE_DEFAULT);
    obs_data_set_default_int(settings, S_PLATFORM_FILTER,  PLAT_BOTH);
}

// Pull the configured dimensions out of settings, clamp to ranges,
// mirror onto the data struct, and mark the texture dirty so the next
// video_render rebuilds at the new size. Lock the instances mutex so a
// concurrent IPC-thread MarkAllOverlayInstancesDirty can't race the
// dirty flag (same ordering used in fcr_video_render's dirty clear).
static void fcr_update(void* data, obs_data_t* settings) {
    FeedsChatOverlayData* d = static_cast<FeedsChatOverlayData*>(data);
    if (!d || !settings) return;

    int w = (int)obs_data_get_int(settings, S_WIDTH);
    int v = (int)obs_data_get_int(settings, S_VISIBLE_MESSAGES);
    int f = (int)obs_data_get_int(settings, S_PLATFORM_FILTER);
    if (w < WIDTH_MIN_PX) w = WIDTH_MIN_PX;
    if (w > WIDTH_MAX_PX) w = WIDTH_MAX_PX;
    if (v < VISIBLE_MIN)  v = VISIBLE_MIN;
    if (v > VISIBLE_MAX)  v = VISIBLE_MAX;
    if (f != PLAT_ZOOM && f != PLAT_YOUTUBE && f != PLAT_TWITCH)
        f = PLAT_BOTH;  // unknown -> Both

    std::lock_guard<std::mutex> lock(g_overlayInstancesMutex);
    bool changed = false;
    if (d->width != (uint32_t)w) { d->width = (uint32_t)w; changed = true; }
    if (d->visible_messages != v) { d->visible_messages = v; changed = true; }
    if (d->platform_filter != f) { d->platform_filter = f; changed = true; }
    if (changed) d->texture_dirty = true;
}

static void* fcr_create(obs_data_t* settings, obs_source_t* source) {
    // Always create. Returning nullptr from a create callback makes OBS keep
    // an invalid husk that OnSourceCreated removes, and the next save bakes in
    // the loss — so a chat overlay used to vanish from a scene loaded while
    // logged out or below Streamer tier. Create it dormant instead: with no
    // chat data (logged out) it renders blank, and fcr_video_render no-ops
    // while tier_disabled, which ReconcileChatOverlaySources sets on login.
    // The properties panel carries the logged-out / upgrade message. nullptr
    // is left only for genuine failure paths.

    FeedsChatOverlayData* d = new FeedsChatOverlayData();
    d->source = source;

    obs_enter_graphics();
    d->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    obs_leave_graphics();

    {
        std::lock_guard<std::mutex> lock(g_overlayInstancesMutex);
        g_overlayInstances.push_back(d);
    }

    // Seed initial dimensions from settings (defaults populated by
    // fcr_get_defaults are in effect here). fcr_update reads, clamps,
    // and marks dirty.
    fcr_update(d, settings);
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

static obs_properties_t* fcr_get_properties(void* data) {
    // Tier-locked branch: replace the normal Width/Visible-Messages
    // controls with an upgrade message + button. Same shape as
    // fcp_get_properties' tier-locked branch.
    FeedsChatOverlayData* d = static_cast<FeedsChatOverlayData*>(data);
    if (d && d->tier_disabled) {
        obs_properties_t* props = obs_properties_create();
        std::string verLabel = std::string("Feeds (v") +
                               feeds_shared::VERSION + ")";
        obs_properties_add_text(props, "ver_label", verLabel.c_str(),
                                OBS_TEXT_INFO);

        const char* tierName = (g_currentTier == 0) ? "Free" : "Basic";
        std::string msg = std::string(
            "Feeds Chat Overlay is a Streamer-tier feature. ") +
            "Your current tier is " + tierName + ".";
        obs_properties_add_text(props, "tier_disabled_msg", msg.c_str(),
                                OBS_TEXT_INFO);
        obs_properties_add_button(props, "upgrade_btn",
            "Upgrade your plan to enable Feeds Chat Overlay",
            [](obs_properties_t*, obs_property_t*, void*) -> bool {
                QDesktopServices::openUrl(
                    QUrl("https://letsdovideo.com/feeds-upgrade"));
                return true;
            });
        return props;
    }

    obs_properties_t* props = obs_properties_create();
    obs_properties_add_int_slider(props, S_WIDTH,
                                  "Width",
                                  WIDTH_MIN_PX, WIDTH_MAX_PX, WIDTH_STEP_PX);
    obs_properties_add_int(props, S_VISIBLE_MESSAGES,
                           "Visible Messages",
                           VISIBLE_MIN, VISIBLE_MAX, 1);
    // Which platform's chat this overlay instance shows. Must-select (no None);
    // defaults to Both. Values match PLAT_* / ChatMsgOrigin.
    obs_property_t* plat = obs_properties_add_list(
        props, S_PLATFORM_FILTER, "Chat Source",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(plat, "Both",         PLAT_BOTH);
    obs_property_list_add_int(plat, "Zoom Chat",    PLAT_ZOOM);
    obs_property_list_add_int(plat, "YouTube Chat", PLAT_YOUTUBE);
    obs_property_list_add_int(plat, "Twitch Chat",  PLAT_TWITCH);
    return props;
}

static void fcr_video_render(void* data, gs_effect_t* /*effect*/) {
    FeedsChatOverlayData* d = static_cast<FeedsChatOverlayData*>(data);
    if (!d) return;

    // Tier-locked: render nothing. Properties panel surfaces the reason.
    if (d->tier_disabled) return;

    // Clear dirty under the instances mutex so a concurrent
    // MarkAllOverlayInstancesDirty call from the IPC thread can't lose
    // its bit: if a new message arrives after we release the lock, it
    // re-sets dirty=true and we catch it on the next frame.
    bool dirty;
    {
        std::lock_guard<std::mutex> lock(g_overlayInstancesMutex);
        dirty = d->texture_dirty;
        if (dirty) d->texture_dirty = false;
    }

    if (dirty || !d->texture) {
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
    feeds_chat_overlay_info.get_defaults   = fcr_get_defaults;
    feeds_chat_overlay_info.update         = fcr_update;
    feeds_chat_overlay_info.video_render   = fcr_video_render;
    feeds_chat_overlay_info.icon_type      = OBS_ICON_TYPE_TEXT;
    obs_register_source(&feeds_chat_overlay_info);
}

void AppendChatMessageToOverlay(ChatMsgOrigin      origin,
                                unsigned int       senderId,
                                const std::string& channelId,
                                const std::string& senderName,
                                const std::string& content,
                                qint64             timestamp) {
    {
        std::lock_guard<std::mutex> lock(g_overlayHistoryMutex);
        OverlayMessage msg;
        msg.origin      = origin;
        msg.sender_id   = senderId;
        msg.channel_id  = channelId;
        msg.sender_name = QString::fromStdString(senderName);
        msg.content     = QString::fromStdString(content);
        msg.timestamp   = timestamp;
        g_overlayHistory.push_back(std::move(msg));

        // Trim oldest. erase-from-begin on a vector is O(N), but with
        // HISTORY_CAP of 50 and chat-message rates measured in
        // messages-per-minute, the cost is negligible. std::deque or a
        // ring buffer would be the move if the cap ever climbs.
        if (g_overlayHistory.size() > HISTORY_CAP) {
            g_overlayHistory.erase(g_overlayHistory.begin());
        }
    }
    MarkAllOverlayInstancesDirty();
}

void ClearChatOverlay() {
    {
        std::lock_guard<std::mutex> lock(g_overlayHistoryMutex);
        g_overlayHistory.clear();
    }
    MarkAllOverlayInstancesDirty();
}

// Public wrapper over MarkAllOverlayInstancesDirty for the avatar workers (see
// the header). No history mutation — a late avatar changed a cache the render
// path reads, so the existing textures just need to be rebuilt.
void MarkChatOverlayDirty() {
    MarkAllOverlayInstancesDirty();
}

void ReconcileChatOverlaySources() {
    const bool shouldDisable = (g_currentTier < OVERLAY_MIN_TIER);
    std::lock_guard<std::mutex> lock(g_overlayInstancesMutex);
    for (FeedsChatOverlayData* d : g_overlayInstances) {
        if (!d) continue;
        if (d->tier_disabled != shouldDisable) {
            d->tier_disabled = shouldDisable;
            // Mark dirty so a re-enabled instance refreshes its texture
            // on the next render. The render path short-circuits on
            // tier_disabled anyway, so the dirty bit only matters on
            // the disable→enable transition.
            d->texture_dirty = true;
        }
    }
}

}  // namespace feeds
