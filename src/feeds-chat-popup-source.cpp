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

#include <obs-module.h>
#include <graphics/graphics.h>

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>
#include <QString>

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

namespace {

// ---------------------------------------------------------------------------
// Canvas layout constants. Width is fixed (chat popups shouldn't span the
// full stream); height grows downward to fit wrapped text. Short messages
// get short popups, long messages get tall popups.
// ---------------------------------------------------------------------------
static constexpr int CANVAS_WIDTH            = 800;
static constexpr int BUBBLE_LEFT_MARGIN      = 10;
static constexpr int BUBBLE_RIGHT_MARGIN     = 10;
static constexpr int BUBBLE_TEXT_LEFT_PAD    = 145;  // clears the avatar
static constexpr int BUBBLE_TEXT_RIGHT_PAD   = 35;
static constexpr int BUBBLE_TEXT_TOP_PAD     = 20;
static constexpr int BUBBLE_TEXT_BOTTOM_PAD  = 20;
static constexpr int MIN_BUBBLE_HEIGHT       = 130;  // never shorter than avatar
static constexpr int BUBBLE_TOP_OFFSET       = 60;
static constexpr int BOTTOM_MARGIN           = 15;   // room for shadow + breathing
static constexpr int MESSAGE_FONT_PIXEL_SIZE = 32;

// ---------------------------------------------------------------------------
// Per-instance state. visible/sender_id/sender_name/content/texture_dirty
// are mirrored into each instance from the file-scope globals below via
// UpdateInstanceFromGlobal — protected by g_popupStateMutex on both
// read and write paths. rendered_image/texture are touched only on the
// graphics thread from fcp_video_render and need no extra locking.
// ---------------------------------------------------------------------------
struct FeedsChatPopupData {
    obs_source_t* source = nullptr;

    QImage        rendered_image;
    gs_texture_t* texture       = nullptr;
    bool          texture_dirty = true;

    uint32_t      width  = CANVAS_WIDTH;
    uint32_t      height = BUBBLE_TOP_OFFSET + MIN_BUBBLE_HEIGHT + BOTTOM_MARGIN;

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

static void RenderPopupToImage(FeedsChatPopupData* d,
                               const QString& senderName,
                               const QString& content,
                               const QImage&  avatar) {
    // Measure wrapped text first so we know how tall the canvas needs
    // to be. Width is fixed; height is content-driven.
    QFont msgFont;
    msgFont.setBold(true);
    msgFont.setPixelSize(MESSAGE_FONT_PIXEL_SIZE);
    QFontMetrics fm(msgFont);
    const int textAreaWidth = CANVAS_WIDTH
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
    d->width  = CANVAS_WIDTH;
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
        CANVAS_WIDTH - BUBBLE_LEFT_MARGIN - BUBBLE_RIGHT_MARGIN,
        bubbleHeight);

    // Drop shadow first so the bubble draws over it. Hard offset shadow
    // (not a real gaussian blur) — cheap, correct-enough for v1.2.0.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 80));
    p.drawRoundedRect(bubbleRect.translated(5, 5), 4, 4);

    // Bubble body
    p.setBrush(QColor("#222222"));
    p.drawRoundedRect(bubbleRect, 4, 4);

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
    pillFont.setPixelSize(24);
    p.setFont(pillFont);
    QFontMetrics pillFm(pillFont);
    int textWidth  = pillFm.horizontalAdvance(senderName);
    int textHeight = pillFm.height();
    QRect pillRect(148, 20, textWidth + 20, textHeight + 12);
    p.setBrush(QColor("#FFA500"));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(pillRect, 6, 6);
    p.setPen(QColor("#222222"));
    p.drawText(pillRect, Qt::AlignCenter, senderName);

    // Avatar — circular clip, then a 3px yellow ring on top
    const QRect avatarRect(15, 15, 128, 128);
    p.save();
    QPainterPath circle;
    circle.addEllipse(avatarRect);
    p.setClipPath(circle);
    if (!avatar.isNull()) {
        p.drawImage(avatarRect,
                    avatar.scaled(128, 128,
                                  Qt::KeepAspectRatioByExpanding,
                                  Qt::SmoothTransformation));
    } else {
        // Null fallback — flat neutral grey circle.
        p.fillRect(avatarRect, QColor(120, 120, 120));
    }
    p.restore();

    QPen ring(QColor("#FFA500"));
    ring.setWidth(3);
    p.setPen(ring);
    p.setBrush(Qt::NoBrush);
    // Inset by 1px so the 3px ring sits visually around the 128px circle.
    p.drawEllipse(avatarRect.adjusted(1, 1, -1, -1));

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
    FeedsChatPopupData* d = new FeedsChatPopupData();
    d->source = source;

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

    if (d->texture) {
        // gs_texture_destroy needs the graphics context — destroy is not
        // guaranteed to be on the graphics thread.
        obs_enter_graphics();
        gs_texture_destroy(d->texture);
        obs_leave_graphics();
        d->texture = nullptr;
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

static obs_properties_t* fcp_get_properties(void* /*data*/) {
    // No user-tunable settings yet — leave the properties panel empty
    // rather than inventing knobs we don't know we need.
    return obs_properties_create();
}

static void fcp_video_render(void* data, gs_effect_t* /*effect*/) {
    FeedsChatPopupData* d = static_cast<FeedsChatPopupData*>(data);
    if (!d) return;

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
    {
        std::lock_guard<std::mutex> lock(g_popupStateMutex);
        visible    = d->visible;
        dirty      = d->texture_dirty;
        senderId   = d->sender_id;
        senderName = d->sender_name;
        content    = d->content;
        if (dirty) d->texture_dirty = false;
    }

    if (!visible) return;

    if (dirty || !d->texture) {
        RegenerateTexture(d, senderId, senderName, content);
    }
    if (!d->texture) return;

    // Standard custom-draw sprite blit. Push the blend state so our
    // alpha-blended popup composites correctly over scene content.
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

    gs_effect_t* eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t* param = gs_effect_get_param_by_name(eff, "image");
    gs_effect_set_texture(param, d->texture);
    while (gs_effect_loop(eff, "Draw")) {
        gs_draw_sprite(d->texture, 0, d->width, d->height);
    }

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
    feeds_chat_popup_info.icon_type      = OBS_ICON_TYPE_TEXT;
    obs_register_source(&feeds_chat_popup_info);
}

void ToggleChatPopup(unsigned int senderId,
                     const std::string& senderName,
                     const std::string& content) {
    {
        std::lock_guard<std::mutex> lock(g_popupStateMutex);

        // "Same message" = currently showing this same sender_id + content.
        // Clicking it again dismisses; clicking a different one replaces.
        bool sameMessage = g_popupVisible &&
                           g_popupSenderId == senderId &&
                           g_popupContent.toStdString() == content;

        if (sameMessage) {
            g_popupVisible = false;
            g_popupSenderId = 0;
            g_popupSenderName.clear();
            g_popupContent.clear();
        } else {
            g_popupVisible    = true;
            g_popupSenderId   = senderId;
            g_popupSenderName = QString::fromStdString(senderName);
            g_popupContent    = QString::fromStdString(content);
        }
    }
    UpdateAllInstances();
}

void ClearChatPopup() {
    {
        std::lock_guard<std::mutex> lock(g_popupStateMutex);
        g_popupVisible = false;
        g_popupSenderId = 0;
        g_popupSenderName.clear();
        g_popupContent.clear();
    }
    UpdateAllInstances();
}

} // namespace feeds
