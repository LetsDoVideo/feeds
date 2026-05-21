// feeds-chat-popup-source.cpp — Feeds Chat Popup OBS source.
//
// Renders a chat popup (circular avatar + yellow username pill + dark
// message bubble with drop shadow) modelled on SocialStreamNinja. The
// popup is painted with QPainter into a QImage on the CPU, uploaded
// to a gs_texture_t when content changes, and drawn each frame via
// gs_draw_sprite. CPU work is amortised across frames — only the GPU
// blit happens per-frame in steady state.
//
// Commit 3c wires the dock's click handler into the source: clicking a
// message in the dock toggles a popup showing that message across all
// active popup instances; clicking the same message hides it; clicking
// a different one replaces the content. Animation lands in 3d.

#include "feeds-chat-popup-source.h"
#include "feeds-version.h"

#include <obs-module.h>
#include <graphics/graphics.h>

#include <QDesktopServices>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <vector>

// Avatar cache lives in plugin-main.cpp — declared with external linkage
// there specifically so this TU can read it. The popup is a pure consumer:
// the chat IPC handler populates the cache on receive, and we look up by
// sender_id when rendering.
extern std::mutex                       g_avatarCacheMutex;
extern std::map<unsigned int, QImage>   g_avatarCache;
extern QImage                           g_fallbackAvatar;

// Tier state + helpers from plugin-main.cpp. Popup is gated at Streamer
// (>= 2). The throttled dialog is reused from the participant/screenshare
// create paths so a saved scene loading multiple over-tier sources at
// once doesn't stack popups.
extern bool g_isLoggedIn;
extern bool g_loginAttemptCompleted;
extern int  g_currentTier;
extern bool ShouldShowTierPopup();
extern void ShowTierLimitDialog(const QString& title, const QString& html);

// Tier threshold for the popup. Streamer (2) and Broadcaster (3) get it;
// Free (0) and Basic (1) don't.
static constexpr int POPUP_MIN_TIER = 2;

namespace {

// ---------------------------------------------------------------------------
// Canvas-relative layout. Reference design is a 1920×1080 canvas, where the
// popup is 1536 px wide (80% of canvas) with the avatar/pill/font sizes
// captured below as literals inside RenderPopupToImage. Every pixel
// dimension is multiplied by `scale = canvas_width / 1536.0f` inside that
// function so the popup looks proportionally identical at 720p, 1080p,
// 1440p, 4K, etc. Width comes from GetPopupCanvasWidth(); height is
// content-driven (wrapped-text measurement).
// ---------------------------------------------------------------------------

// Slide animation duration per direction. Ease-out cubic — smooth
// deceleration on arrival, smooth acceleration on departure. Matches
// SSN's visual feel; not a bezier-perfect match (no overshoot).
static constexpr float ANIM_DURATION_SECONDS = 0.5f;

// 80% of the active OBS canvas width. Queried once per content change
// inside RenderPopupToImage so the popup is the same proportion of the
// screen regardless of the streamer's canvas resolution. Falls back to
// 1536 (80% of 1920) if obs_get_video_info fails — shouldn't happen
// during active rendering, but defensive against being called before
// OBS finishes initialising.
static uint32_t GetPopupCanvasWidth() {
    struct obs_video_info ovi;
    if (obs_get_video_info(&ovi)) {
        return (uint32_t)((float)ovi.base_width * 0.8f);
    }
    return 1536;
}

// ---------------------------------------------------------------------------
// Per-instance state. visible/sender_id/sender_name/content/texture_dirty
// are mirrored into each instance from the file-scope globals below via
// UpdateInstanceFromGlobal — protected by g_popupStateMutex on both
// read and write paths. rendered_image/texture are touched only on the
// graphics thread from fcp_video_render and need no extra locking.
// ---------------------------------------------------------------------------
struct FeedsChatPopupData {
    obs_source_t* source = nullptr;

    QImage          rendered_image;
    gs_texture_t*   texture       = nullptr;
    bool            texture_dirty = true;

    // Intermediate render target for slide-animation clipping. The popup
    // is drawn into this bbox-sized buffer (with its Y translation), then
    // the buffer is composited into the scene as a flat sprite. Anything
    // translated outside the buffer's bounds is discarded by the texrender,
    // giving us the "slide behind an invisible barrier" effect that direct
    // CUSTOM_DRAW translation can't provide (sources can draw outside their
    // declared width/height when not going through an intermediate).
    gs_texrender_t* texrender     = nullptr;

    // Width and height start at the 1920p fallback values; first
    // RenderPopupToImage call queries OBS for the real canvas size and
    // corrects both. Pre-render fcp_get_width/height queries see a
    // sensible stand-in rather than zero. Height is BUBBLE_TOP_OFFSET +
    // MIN_BUBBLE_HEIGHT + BOTTOM_MARGIN at scale=1.0 (60 + 130 + 15).
    uint32_t      width  = 1536;
    uint32_t      height = 205;

    // True iff this instance is currently locked out by tier (logged in
    // at < POPUP_MIN_TIER). Set by feeds::ReconcileChatPopupSources on
    // login_succeeded. When true, fcp_video_render no-ops and
    // fcp_get_properties returns an upgrade message instead of the
    // normal (empty) properties panel.
    bool          tier_disabled = false;

    bool          visible    = false;
    unsigned int  sender_id  = 0;
    QString       sender_name;
    QString       content;
};

// ---------------------------------------------------------------------------
// Instance registry — ToggleChatPopup / ClearChatPopup walk this list to
// push state changes to every popup in every scene. Modified on
// fcp_create / fcp_destroy.
// ---------------------------------------------------------------------------
std::mutex                            g_instancesMutex;
std::vector<FeedsChatPopupData*>      g_instances;

// ---------------------------------------------------------------------------
// Current popup state. Single source of truth — instances mirror this.
// Lives at file scope so a freshly-created popup instance can pick up
// whatever is currently showing.
// ---------------------------------------------------------------------------
std::mutex     g_popupStateMutex;
bool           g_popupVisible      = false;
unsigned int   g_popupSenderId     = 0;
QString        g_popupSenderName;
QString        g_popupContent;

// ---------------------------------------------------------------------------
// Slide-animation state. Global so every popup instance animates in lockstep,
// regardless of which scenes they live in. Guarded by g_popupStateMutex along
// with the visibility/content fields above.
//
// Y-offset is tracked as a fraction in [0, 1]: 0 = popup in place at its
// bbox origin; 1 = popup translated fully below its bbox (one bbox-height
// down, i.e. offscreen relative to the bbox). Each phase animates from a
// captured `from` fraction toward a target `to` fraction over
// ANIM_DURATION_SECONDS. Storing `from` (rather than always 0 or 1) lets us
// reverse mid-flight without a visual jump.
//
// The pending slot holds a message that should appear once the current
// AnimatingOut phase completes — set by ToggleChatPopup when the user
// clicks a different message while a popup is showing or animating in.
// Cleared by ClearChatPopup and by same-content dismiss clicks.
// ---------------------------------------------------------------------------
enum class PopupAnimState { Hidden, AnimatingIn, Shown, AnimatingOut };
PopupAnimState g_popupAnimState       = PopupAnimState::Hidden;
float          g_popupAnimElapsed     = 0.0f;
float          g_popupAnimFrom        = 1.0f;
float          g_popupAnimTo          = 1.0f;

bool           g_popupPending         = false;
unsigned int   g_popupPendingSenderId = 0;
QString        g_popupPendingSenderName;
QString        g_popupPendingContent;

static void RenderPopupToImage(FeedsChatPopupData* d,
                               const QString& senderName,
                               const QString& content,
                               const QImage&  avatar) {
    // Width derives from the current OBS canvas (80% of canvas width).
    // `scale` is canvas_width / reference (1536 = 80% of 1920) so every
    // pixel literal below stays proportional at any canvas resolution:
    // scale=1 at 1080p, ~0.67 at 720p, 2 at 4K. Captured once so all
    // layout maths see a consistent value even if the canvas changes
    // mid-paint — unlikely, but cheap to be safe.
    const int   canvasWidth = (int)GetPopupCanvasWidth();
    const float scale       = (float)canvasWidth / 1536.0f;

    // Bubble + message text
    const int BUBBLE_LEFT_MARGIN      = (int)(10  * scale);
    const int BUBBLE_RIGHT_MARGIN     = (int)(10  * scale);
    const int BUBBLE_TOP_OFFSET       = (int)(60  * scale);
    const int BOTTOM_MARGIN           = (int)(15  * scale);
    const int BUBBLE_CORNER_RADIUS    = (int)(4   * scale);
    const int MIN_BUBBLE_HEIGHT       = (int)(130 * scale);
    const int BUBBLE_TEXT_LEFT_PAD    = (int)(145 * scale);  // clears avatar
    const int BUBBLE_TEXT_RIGHT_PAD   = (int)(35  * scale);
    const int BUBBLE_TEXT_TOP_PAD     = (int)(20  * scale);
    const int BUBBLE_TEXT_BOTTOM_PAD  = (int)(20  * scale);
    const int MESSAGE_FONT_PIXEL_SIZE = (int)(32  * scale);

    // Username pill (PILL_H_PAD / PILL_V_PAD are *per-side*, so the
    // pill's footprint is textWidth + 2*PILL_H_PAD wide.)
    const int PILL_X                  = (int)(148 * scale);
    const int PILL_Y                  = (int)(20  * scale);
    const int PILL_H_PAD              = (int)(10  * scale);
    const int PILL_V_PAD              = (int)(6   * scale);
    const int PILL_CORNER_RADIUS      = (int)(6   * scale);
    const int PILL_FONT_PIXEL_SIZE    = (int)(24  * scale);

    // Avatar. AVATAR_BORDER_WIDTH and AVATAR_RING_INSET are clamped to
    // at least 1 so the yellow ring never disappears at sub-1080p
    // resolutions. Inset = border/2 keeps the stroke centred over the
    // circle edge regardless of scale.
    const int AVATAR_X                = (int)(15  * scale);
    const int AVATAR_Y                = (int)(15  * scale);
    const int AVATAR_SIZE             = (int)(128 * scale);
    const int AVATAR_BORDER_WIDTH     = std::max(1, (int)(3 * scale));
    const int AVATAR_RING_INSET       = std::max(1, AVATAR_BORDER_WIDTH / 2);

    // Shadow offset (no real gaussian blur — hard offset).
    const int SHADOW_OFFSET           = (int)(5   * scale);

    // Measure wrapped text first so we know how tall the canvas needs
    // to be. Width is determined; height is content-driven.
    QFont msgFont;
    msgFont.setBold(true);
    msgFont.setPixelSize(MESSAGE_FONT_PIXEL_SIZE);
    QFontMetrics fm(msgFont);
    const int textAreaWidth = canvasWidth
                              - BUBBLE_LEFT_MARGIN - BUBBLE_RIGHT_MARGIN
                              - BUBBLE_TEXT_LEFT_PAD - BUBBLE_TEXT_RIGHT_PAD;
    QRect textBounds = fm.boundingRect(
        0, 0, textAreaWidth, std::numeric_limits<int>::max(),
        Qt::TextWordWrap, content);
    const int bubbleHeight = std::max(
        MIN_BUBBLE_HEIGHT,
        textBounds.height() + BUBBLE_TEXT_TOP_PAD + BUBBLE_TEXT_BOTTOM_PAD);

    // Mutate the source dimensions before sizing the QImage so OBS's
    // next get_width/get_height query sees the new canvas. First frame
    // after a content change reports the old size for a single frame —
    // not visible in practice for the use cases here.
    d->width  = (uint32_t)canvasWidth;
    d->height = (uint32_t)(BUBBLE_TOP_OFFSET + bubbleHeight + BOTTOM_MARGIN);

    if (d->rendered_image.size() != QSize((int)d->width, (int)d->height) ||
        d->rendered_image.format() != QImage::Format_RGBA8888) {
        d->rendered_image = QImage((int)d->width, (int)d->height,
                                   QImage::Format_RGBA8888);
    }
    d->rendered_image.fill(Qt::transparent);

    QPainter p(&d->rendered_image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect bubbleRect(
        BUBBLE_LEFT_MARGIN,
        BUBBLE_TOP_OFFSET,
        canvasWidth - BUBBLE_LEFT_MARGIN - BUBBLE_RIGHT_MARGIN,
        bubbleHeight);

    // Drop shadow first so the bubble draws over it.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 80));
    p.drawRoundedRect(bubbleRect.translated(SHADOW_OFFSET, SHADOW_OFFSET),
                      BUBBLE_CORNER_RADIUS, BUBBLE_CORNER_RADIUS);

    // Bubble body
    p.setBrush(QColor("#222222"));
    p.drawRoundedRect(bubbleRect, BUBBLE_CORNER_RADIUS, BUBBLE_CORNER_RADIUS);

    // Message text inside the bubble, padded left to clear avatar
    p.setFont(msgFont);
    p.setPen(Qt::white);
    QRect textRect = bubbleRect.adjusted(BUBBLE_TEXT_LEFT_PAD,
                                          BUBBLE_TEXT_TOP_PAD,
                                          -BUBBLE_TEXT_RIGHT_PAD,
                                          -BUBBLE_TEXT_BOTTOM_PAD);
    p.drawText(textRect,
               Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
               content);

    // Username pill — yellow rounded rect, dark text, sized to fit
    QFont pillFont = p.font();
    pillFont.setBold(true);
    pillFont.setPixelSize(PILL_FONT_PIXEL_SIZE);
    p.setFont(pillFont);
    QFontMetrics pillFm(pillFont);
    int textWidth  = pillFm.horizontalAdvance(senderName);
    int textHeight = pillFm.height();
    QRect pillRect(PILL_X, PILL_Y,
                   textWidth  + 2 * PILL_H_PAD,
                   textHeight + 2 * PILL_V_PAD);
    p.setBrush(QColor("#FFA500"));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(pillRect, PILL_CORNER_RADIUS, PILL_CORNER_RADIUS);
    p.setPen(QColor("#222222"));
    p.drawText(pillRect, Qt::AlignCenter, senderName);

    // Avatar — circular clip, then a scaled yellow ring on top
    const QRect avatarRect(AVATAR_X, AVATAR_Y, AVATAR_SIZE, AVATAR_SIZE);
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
        // Null fallback — flat neutral grey circle.
        p.fillRect(avatarRect, QColor(120, 120, 120));
    }
    p.restore();

    QPen ring(QColor("#FFA500"));
    ring.setWidth(AVATAR_BORDER_WIDTH);
    p.setPen(ring);
    p.setBrush(Qt::NoBrush);
    // Inset by half the stroke width so the ring sits visually centred
    // over the avatar circle's edge (Qt strokes are centre-aligned by
    // default; without the inset the outer half clips beyond the bbox).
    p.drawEllipse(avatarRect.adjusted( AVATAR_RING_INSET,  AVATAR_RING_INSET,
                                      -AVATAR_RING_INSET, -AVATAR_RING_INSET));

    p.end();
}

// ---------------------------------------------------------------------------
// Upload the current QImage to GPU. Called from video_render when the
// per-instance state has changed. video_render runs on the graphics thread
// so the graphics context is already active — no obs_enter_graphics needed.
//
// senderName/content are passed in as a snapshot to avoid touching d's
// QString members (which the Qt main thread can mutate concurrently via
// UpdateInstanceFromGlobal) while the slow paint is happening.
// ---------------------------------------------------------------------------
static void RegenerateTexture(FeedsChatPopupData* d,
                              unsigned int   senderId,
                              const QString& senderName,
                              const QString& content) {
    QImage avatar;
    {
        std::lock_guard<std::mutex> lock(g_avatarCacheMutex);
        auto it = g_avatarCache.find(senderId);
        if (it != g_avatarCache.end()) {
            avatar = it->second;
        } else {
            // Not in cache (rare — IPC handler usually warms it on
            // arrival). Use the bundled fallback so we still draw a
            // recognisable popup.
            avatar = g_fallbackAvatar;
        }
    }

    RenderPopupToImage(d, senderName, content, avatar);

    // QImage rows are 4-byte aligned for RGBA8888 with a width divisible
    // by 1 — i.e., always tightly packed at 4 bytes per pixel. The
    // memcpy fallback is defence in depth: if Qt ever returns a strided
    // image (unusual width, debug build, future Qt version), we copy.
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
// Mirror the current global popup state into a single instance. Sets
// texture_dirty if anything changed so the next video_render redraws.
// Acquires g_popupStateMutex — must NOT be called with that mutex held.
// ---------------------------------------------------------------------------
static void UpdateInstanceFromGlobal(FeedsChatPopupData* d) {
    std::lock_guard<std::mutex> lock(g_popupStateMutex);

    bool         changed =
        d->visible     != g_popupVisible    ||
        d->sender_id   != g_popupSenderId   ||
        d->sender_name != g_popupSenderName ||
        d->content     != g_popupContent;

    d->visible     = g_popupVisible;
    d->sender_id   = g_popupSenderId;
    d->sender_name = g_popupSenderName;
    d->content     = g_popupContent;

    if (changed) d->texture_dirty = true;
}

// Fan state changes out to every popup instance. Lock order is
// instancesMutex (outer) → popupStateMutex (inner, inside the per-instance
// call) — never reversed elsewhere, so no deadlock.
static void UpdateAllInstances() {
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    for (FeedsChatPopupData* d : g_instances) {
        UpdateInstanceFromGlobal(d);
    }
}

// Ease-out cubic. Decelerates as t approaches 1 — smooth landing on
// arrival, smooth lift-off on departure when curve runs in reverse.
static float EaseOutCubic(float t) {
    float f = 1.0f - t;
    return 1.0f - f * f * f;
}

// Current Y-offset fraction for the popup (0 = in place, 1 = fully below).
// Caller must hold g_popupStateMutex.
static float ComputeCurrentFraction_locked() {
    switch (g_popupAnimState) {
        case PopupAnimState::Hidden: return 1.0f;
        case PopupAnimState::Shown:  return 0.0f;
        case PopupAnimState::AnimatingIn:
        case PopupAnimState::AnimatingOut: {
            float t = g_popupAnimElapsed / ANIM_DURATION_SECONDS;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float e = EaseOutCubic(t);
            return g_popupAnimFrom + (g_popupAnimTo - g_popupAnimFrom) * e;
        }
    }
    return 0.0f;
}

// Advance the global animation by `seconds`. Called once per frame from
// the first popup instance's video_tick (see fcp_video_tick — gating
// stops the elapsed counter from advancing N times per frame when there
// are N popup sources in the scene collection). Short-circuits when no
// animation is in flight, so the steady-state cost is one mutex acquire
// plus one comparison.
static void AdvanceAnimation(float seconds) {
    bool needsRefresh = false;
    {
        std::lock_guard<std::mutex> lock(g_popupStateMutex);
        if (g_popupAnimState != PopupAnimState::AnimatingIn &&
            g_popupAnimState != PopupAnimState::AnimatingOut) {
            return;
        }
        g_popupAnimElapsed += seconds;
        if (g_popupAnimElapsed < ANIM_DURATION_SECONDS) return;

        // Current phase complete — transition. Content/visibility only
        // change on the AnimatingOut → Hidden / → AnimatingIn boundary;
        // AnimatingIn → Shown is a pure state flip that instances don't
        // need to learn about.
        if (g_popupAnimState == PopupAnimState::AnimatingIn) {
            g_popupAnimState   = PopupAnimState::Shown;
            g_popupAnimElapsed = 0.0f;
            g_popupAnimFrom    = 0.0f;
            g_popupAnimTo      = 0.0f;
            return;
        }

        if (g_popupPending) {
            // Promote pending → new slide-in.
            g_popupSenderId          = g_popupPendingSenderId;
            g_popupSenderName        = g_popupPendingSenderName;
            g_popupContent           = g_popupPendingContent;
            g_popupVisible           = true;
            g_popupPending           = false;
            g_popupPendingSenderId   = 0;
            g_popupPendingSenderName.clear();
            g_popupPendingContent.clear();
            g_popupAnimState         = PopupAnimState::AnimatingIn;
            g_popupAnimElapsed       = 0.0f;
            g_popupAnimFrom          = 1.0f;
            g_popupAnimTo            = 0.0f;
        } else {
            g_popupVisible    = false;
            g_popupSenderId   = 0;
            g_popupSenderName.clear();
            g_popupContent.clear();
            g_popupAnimState   = PopupAnimState::Hidden;
            g_popupAnimElapsed = 0.0f;
            g_popupAnimFrom    = 1.0f;
            g_popupAnimTo      = 1.0f;
        }
        needsRefresh = true;
    }
    if (needsRefresh) UpdateAllInstances();
}

// ---------------------------------------------------------------------------
// OBS source callbacks
// ---------------------------------------------------------------------------
static const char* fcp_get_name(void*) {
    // Display name only — source ID ("feeds_chat_popup", in the
    // registration struct below) is the stable key OBS uses in saved
    // scene collections and must never change. The Z-prefix sorts the
    // popup next to Zoom Participant / Zoom Screenshare in the picker.
    return "Zoom Chat Popup";
}

static void* fcp_create(obs_data_t* /*settings*/, obs_source_t* source) {
    // Logged-out gating: refuse creation outright when there's no Zoom
    // session. Runs before the tier check so a logged-out user sees
    // "log in" instead of an "upgrade your plan" message that doesn't
    // apply yet. Throttled-popup helper deduplicates the prompt when a
    // saved scene loads multiple Feeds sources at once. Deferred until
    // login_succeeded / login_failed has come back from the engine —
    // see g_loginAttemptCompleted in plugin-main.cpp.
    if (g_loginAttemptCompleted && !g_isLoggedIn) {
        if (ShouldShowTierPopup()) {
            ShowTierLimitDialog(
                "Feeds - Login Required",
                "Please log in to Zoom to use Feeds.<br><br>"
                "Open the Feeds menu and click \"Login to Zoom\" to get started.");
        }
        return nullptr;
    }

    // Tier gating, mirroring zp_create / zs_create. Only fire the popup
    // when we know the user's tier (post-login) — saved scenes that load
    // before login_succeeded would otherwise block creation on the
    // default tier=0. The plugin-main OnSourceCreated husk-sweep catches
    // the NULL return and removes the placeholder source from any scene
    // it landed in.
    if (g_isLoggedIn && g_currentTier < POPUP_MIN_TIER) {
        if (ShouldShowTierPopup()) {
            ShowTierLimitDialog(
                "Feeds - Upgrade Required",
                "Zoom Chat Popup is a Streamer-tier feature.<br><br>"
                "<a href=\"https://letsdovideo.com/feeds-upgrade\">"
                "Upgrade your plan</a> to enable it.");
        }
        return nullptr;
    }

    FeedsChatPopupData* d = new FeedsChatPopupData();
    d->source = source;

    // Allocate the slide-clip texrender up front. gs_texrender_create
    // requires the graphics context; create is called from the Qt main
    // thread so we enter/leave manually. If allocation fails (rare,
    // graphics-memory pressure), render falls back to direct draw and
    // the source still works — just without slide-edge clipping.
    obs_enter_graphics();
    d->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    obs_leave_graphics();

    {
        std::lock_guard<std::mutex> lock(g_instancesMutex);
        g_instances.push_back(d);
    }

    // Pick up whatever message is currently showing (if any), so a popup
    // added mid-conversation matches what the rest of the system sees.
    UpdateInstanceFromGlobal(d);

    return d;
}

static void fcp_destroy(void* data) {
    FeedsChatPopupData* d = static_cast<FeedsChatPopupData*>(data);
    if (!d) return;

    {
        std::lock_guard<std::mutex> lock(g_instancesMutex);
        g_instances.erase(
            std::remove(g_instances.begin(), g_instances.end(), d),
            g_instances.end());
    }

    // gs_texture_destroy / gs_texrender_destroy need the graphics context —
    // destroy is not guaranteed to be on the graphics thread. Wrap both
    // teardowns in a single enter/leave to amortise the context switch.
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

static uint32_t fcp_get_width(void* data) {
    FeedsChatPopupData* d = static_cast<FeedsChatPopupData*>(data);
    return d ? d->width : 0;
}

static uint32_t fcp_get_height(void* data) {
    FeedsChatPopupData* d = static_cast<FeedsChatPopupData*>(data);
    return d ? d->height : 0;
}

static obs_properties_t* fcp_get_properties(void* data) {
    // Tier-locked branch: replace the (currently empty) normal properties
    // panel with an upgrade message + button. Mirrors zp_properties /
    // zs_properties when their sources are tier_disabled.
    FeedsChatPopupData* d = static_cast<FeedsChatPopupData*>(data);
    if (d && d->tier_disabled) {
        obs_properties_t* props = obs_properties_create();
        std::string verLabel = std::string("Feeds (v") +
                               feeds_shared::VERSION + ")";
        obs_properties_add_text(props, "ver_label", verLabel.c_str(),
                                OBS_TEXT_INFO);

        const char* tierName = (g_currentTier == 0) ? "Free" : "Basic";
        std::string msg = std::string(
            "Zoom Chat Popup is a Streamer-tier feature. ") +
            "Your current tier is " + tierName + ".";
        obs_properties_add_text(props, "tier_disabled_msg", msg.c_str(),
                                OBS_TEXT_INFO);
        obs_properties_add_button(props, "upgrade_btn",
            "Upgrade your plan to enable Zoom Chat Popup",
            [](obs_properties_t*, obs_property_t*, void*) -> bool {
                QDesktopServices::openUrl(
                    QUrl("https://letsdovideo.com/feeds-upgrade"));
                return true;
            });
        return props;
    }

    // No user-tunable settings yet — leave the properties panel empty
    // rather than inventing knobs we don't know we need.
    return obs_properties_create();
}

// Advance global animation state once per frame. OBS calls video_tick for
// every source every frame regardless of scene visibility — we only want
// the global elapsed counter to step once, so gate on "am I the first
// instance in the registry." Briefly takes g_instancesMutex and releases
// it before calling AdvanceAnimation, preserving the instances-outer /
// state-inner lock order used elsewhere.
static void fcp_video_tick(void* data, float seconds) {
    {
        std::lock_guard<std::mutex> lock(g_instancesMutex);
        if (g_instances.empty() || g_instances.front() != data) return;
    }
    AdvanceAnimation(seconds);
}

static void fcp_video_render(void* data, gs_effect_t* /*effect*/) {
    FeedsChatPopupData* d = static_cast<FeedsChatPopupData*>(data);
    if (!d) return;

    // Tier-locked: render nothing so the stream stays clean. Properties
    // panel still tells the user why (see fcp_get_properties).
    if (d->tier_disabled) return;

    // Snapshot per-instance state under lock so an in-flight
    // UpdateInstanceFromGlobal on the Qt main thread can't tear the
    // QStrings or change visibility mid-render. Clear texture_dirty
    // under the same lock — if a new update lands after we release,
    // it'll set dirty=true again and the next frame catches up.
    bool         visible;
    bool         dirty;
    unsigned int senderId;
    QString      senderName;
    QString      content;
    float        animFraction;
    {
        std::lock_guard<std::mutex> lock(g_popupStateMutex);
        visible      = d->visible;
        dirty        = d->texture_dirty;
        senderId     = d->sender_id;
        senderName   = d->sender_name;
        content      = d->content;
        if (dirty) d->texture_dirty = false;
        animFraction = ComputeCurrentFraction_locked();
    }

    if (!visible) return;

    if (dirty || !d->texture) {
        RegenerateTexture(d, senderId, senderName, content);
    }
    if (!d->texture) return;

    // Translate by fraction × bbox-height: 0 = popup sits in its bbox;
    // 1 = popup sits one bbox-height below. Slide-in animates 1 → 0;
    // slide-out animates 0 → 1. OBS_SOURCE_CUSTOM_DRAW draws straight to
    // the scene render target with no per-source clipping, so a raw
    // translated draw would show the popup spilling outside its bbox.
    // We route through gs_texrender below so the bbox naturally clips
    // the slide.
    const float yOffset = animFraction * (float)d->height;

    if (d->texrender) {
        // Phase 1: render the popup (with its Y translation) into a
        // bbox-sized intermediate texture. Anything translated outside
        // (0,0)-(width,height) is discarded by the texrender.
        gs_texrender_reset(d->texrender);
        if (gs_texrender_begin(d->texrender, d->width, d->height)) {
            struct vec4 clear_color = { 0.0f, 0.0f, 0.0f, 0.0f };
            gs_clear(GS_CLEAR_COLOR, &clear_color, 1.0f, 0);
            gs_ortho(0.0f, (float)d->width,
                     0.0f, (float)d->height,
                     -100.0f, 100.0f);

            // Straight copy from the popup texture into the texrender
            // (which is pre-cleared to fully transparent). Using normal
            // SRCALPHA/INVSRCALPHA against a transparent dest would
            // square the alpha — dst.a = src.a*src.a — quietly making
            // the drop shadow ~3× too transparent after the final
            // composite. Replace blending (src=ONE, dst=ZERO) writes
            // the popup's RGBA verbatim, including alpha; GPU scissor
            // discards pixels translated outside (0,0)-(width,height).
            gs_blend_state_push();
            gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

            gs_matrix_push();
            gs_matrix_translate3f(0.0f, yOffset, 0.0f);

            gs_effect_t* eff   = obs_get_base_effect(OBS_EFFECT_DEFAULT);
            gs_eparam_t* param = gs_effect_get_param_by_name(eff, "image");
            gs_effect_set_texture(param, d->texture);
            while (gs_effect_loop(eff, "Draw")) {
                gs_draw_sprite(d->texture, 0, d->width, d->height);
            }

            gs_matrix_pop();
            gs_blend_state_pop();

            gs_texrender_end(d->texrender);
        }

        // Phase 2: composite the (clipped) intermediate texture into the
        // scene at the source's natural position — no translation here,
        // the slide offset is already baked into the texrender contents.
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

    // Fallback: texrender allocation failed during fcp_create. Direct
    // draw with translation — same behaviour as the 3d commit, popup
    // is visible outside its bbox during the slide. Better than not
    // rendering at all.
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

    gs_matrix_push();
    gs_matrix_translate3f(0.0f, yOffset, 0.0f);

    gs_effect_t* eff   = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t* param = gs_effect_get_param_by_name(eff, "image");
    gs_effect_set_texture(param, d->texture);
    while (gs_effect_loop(eff, "Draw")) {
        gs_draw_sprite(d->texture, 0, d->width, d->height);
    }

    gs_matrix_pop();
    gs_blend_state_pop();
}

static struct obs_source_info feeds_chat_popup_info = {};

} // namespace

namespace feeds {

void RegisterChatPopupSource() {
    feeds_chat_popup_info.id             = "feeds_chat_popup";
    feeds_chat_popup_info.type           = OBS_SOURCE_TYPE_INPUT;
    feeds_chat_popup_info.output_flags   = OBS_SOURCE_VIDEO |
                                           OBS_SOURCE_CUSTOM_DRAW;
    feeds_chat_popup_info.get_name       = fcp_get_name;
    feeds_chat_popup_info.create         = fcp_create;
    feeds_chat_popup_info.destroy        = fcp_destroy;
    feeds_chat_popup_info.get_width      = fcp_get_width;
    feeds_chat_popup_info.get_height     = fcp_get_height;
    feeds_chat_popup_info.get_properties = fcp_get_properties;
    feeds_chat_popup_info.video_render   = fcp_video_render;
    feeds_chat_popup_info.video_tick     = fcp_video_tick;
    feeds_chat_popup_info.icon_type      = OBS_ICON_TYPE_TEXT;
    obs_register_source(&feeds_chat_popup_info);
}

// Public API state machine. Four states (Hidden / AnimatingIn / Shown /
// AnimatingOut) plus a pending slot covers every click sequence the dock
// can produce. The lifecycle:
//
//   Hidden       —click→        AnimatingIn  (new content)
//   AnimatingIn  —elapsed→      Shown        (in-animation completes)
//   Shown        —same click→   AnimatingOut (dismiss)
//   Shown        —diff click→   AnimatingOut + pending=new content
//   AnimatingOut —elapsed→      Hidden        (if no pending)
//   AnimatingOut —elapsed→      AnimatingIn   (if pending → promote)
//
// During AnimatingIn, a click that matches the in-flight content reverses
// from the current visible position so the popup doesn't have to fully
// finish coming in before going back out — captured via
// ComputeCurrentFraction_locked() and used as the new `from`.
void ToggleChatPopup(unsigned int senderId,
                     const std::string& senderName,
                     const std::string& content) {
    bool needsRefresh = false;
    {
        std::lock_guard<std::mutex> lock(g_popupStateMutex);

        QString qName    = QString::fromStdString(senderName);
        QString qContent = QString::fromStdString(content);

        // "Same content" against whatever the popup is currently bound to —
        // valid in every non-Hidden state (visible stays true throughout
        // the AnimatingIn / Shown / AnimatingOut span).
        bool sameAsCurrent = g_popupVisible &&
                             g_popupSenderId == senderId &&
                             g_popupContent  == qContent;

        switch (g_popupAnimState) {
        case PopupAnimState::Hidden:
            g_popupVisible      = true;
            g_popupSenderId     = senderId;
            g_popupSenderName   = qName;
            g_popupContent      = qContent;
            g_popupAnimState    = PopupAnimState::AnimatingIn;
            g_popupAnimElapsed  = 0.0f;
            g_popupAnimFrom     = 1.0f;
            g_popupAnimTo       = 0.0f;
            needsRefresh = true;
            break;

        case PopupAnimState::Shown:
            g_popupAnimState   = PopupAnimState::AnimatingOut;
            g_popupAnimElapsed = 0.0f;
            g_popupAnimFrom    = 0.0f;
            g_popupAnimTo      = 1.0f;
            if (sameAsCurrent) {
                // Same-content click = dismiss. Clear any queued message
                // — the user changed their mind.
                g_popupPending = false;
                g_popupPendingSenderId = 0;
                g_popupPendingSenderName.clear();
                g_popupPendingContent.clear();
            } else {
                g_popupPending           = true;
                g_popupPendingSenderId   = senderId;
                g_popupPendingSenderName = qName;
                g_popupPendingContent    = qContent;
            }
            break;

        case PopupAnimState::AnimatingIn: {
            // Reverse from the current eased position so the popup doesn't
            // jump back to fully-in before sliding out.
            float cur          = ComputeCurrentFraction_locked();
            g_popupAnimState   = PopupAnimState::AnimatingOut;
            g_popupAnimElapsed = 0.0f;
            g_popupAnimFrom    = cur;
            g_popupAnimTo      = 1.0f;
            if (sameAsCurrent) {
                g_popupPending = false;
                g_popupPendingSenderId = 0;
                g_popupPendingSenderName.clear();
                g_popupPendingContent.clear();
            } else {
                g_popupPending           = true;
                g_popupPendingSenderId   = senderId;
                g_popupPendingSenderName = qName;
                g_popupPendingContent    = qContent;
            }
            break;
        }

        case PopupAnimState::AnimatingOut: {
            // Already on the way out. If the new click matches what's
            // already queued, no work to do; otherwise replace pending.
            bool sameAsPending = g_popupPending &&
                                 g_popupPendingSenderId == senderId &&
                                 g_popupPendingContent  == qContent;
            if (!sameAsPending) {
                g_popupPending           = true;
                g_popupPendingSenderId   = senderId;
                g_popupPendingSenderName = qName;
                g_popupPendingContent    = qContent;
            }
            break;
        }
        }
    }
    // Only the Hidden → AnimatingIn branch alters instance-visible state.
    // The other branches leave visibility/content unchanged for now —
    // AdvanceAnimation will fan out when AnimatingOut completes and
    // either promotes pending or flips visible to false.
    if (needsRefresh) UpdateAllInstances();
}

void ClearChatPopup() {
    std::lock_guard<std::mutex> lock(g_popupStateMutex);

    // ClearChatPopup always drops any queued message — the dock is
    // telling us the popup should go away, not that there's a new one
    // waiting.
    g_popupPending = false;
    g_popupPendingSenderId = 0;
    g_popupPendingSenderName.clear();
    g_popupPendingContent.clear();

    switch (g_popupAnimState) {
    case PopupAnimState::Hidden:
    case PopupAnimState::AnimatingOut:
        return;
    case PopupAnimState::Shown:
        g_popupAnimState   = PopupAnimState::AnimatingOut;
        g_popupAnimElapsed = 0.0f;
        g_popupAnimFrom    = 0.0f;
        g_popupAnimTo      = 1.0f;
        return;
    case PopupAnimState::AnimatingIn: {
        float cur          = ComputeCurrentFraction_locked();
        g_popupAnimState   = PopupAnimState::AnimatingOut;
        g_popupAnimElapsed = 0.0f;
        g_popupAnimFrom    = cur;
        g_popupAnimTo      = 1.0f;
        return;
    }
    }
}

void ReconcileChatPopupSources() {
    const bool shouldDisable = (g_currentTier < POPUP_MIN_TIER);
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    for (FeedsChatPopupData* d : g_instances) {
        if (!d) continue;
        if (d->tier_disabled != shouldDisable) {
            d->tier_disabled  = shouldDisable;
            // Mark dirty so any cached texture for the now-disabled
            // instance is recomputed (defensive — the render path will
            // short-circuit on tier_disabled anyway, but a re-enabled
            // instance needs to refresh whatever it was showing).
            d->texture_dirty  = true;
        }
    }
}

} // namespace feeds
