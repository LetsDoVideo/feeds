// feeds-chat-popup-source.cpp — Feeds Chat Popup OBS source.
//
// Renders a chat popup (circular avatar + yellow username pill + dark
// message bubble with drop shadow) modelled on SocialStreamNinja. The
// popup is painted with QPainter into a QImage on the CPU, uploaded
// to a gs_texture_t when content changes, and drawn each frame via
// gs_draw_sprite. CPU work is amortised across frames — only the GPU
// blit happens per-frame in steady state.
//
// Commit 3b (this commit) renders a hardcoded test message — enough
// to validate registration, the QPainter pipeline, and texture upload.
// Commit 3c wires the dock's send/receive path into the source so it
// shows real chat. Commit 3d adds the slide-in / fade-out animation.

#include <obs-module.h>
#include <graphics/graphics.h>

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>
#include <QString>

#include <map>
#include <mutex>
#include <vector>

// Avatar cache lives in plugin-main.cpp — declared with external linkage
// there specifically so this TU can read it. The popup is a pure consumer:
// the chat IPC handler populates the cache on receive, and we look up by
// sender_id when rendering. For 3b we just use the fallback.
extern std::mutex                       g_avatarCacheMutex;
extern std::map<unsigned int, QImage>   g_avatarCache;
extern QImage                           g_fallbackAvatar;

namespace {

struct FeedsChatPopupData {
    obs_source_t* source = nullptr;

    QImage        rendered_image;
    gs_texture_t* texture       = nullptr;
    bool          texture_dirty = true;

    uint32_t      width  = 600;
    uint32_t      height = 200;
};

// ---------------------------------------------------------------------------
// QPainter rendering — paints the popup into d->rendered_image. Called only
// when content changes (texture_dirty is set), not per frame.
//
// Layout (canvas 600 × 200, anchored top-left):
//   Avatar:        128×128 circle at (15, 15) with 3px yellow ring
//   Username pill: rounded rect starting at x=148, y=20, height ~36, width
//                  text-driven. Overlaps the top of the bubble.
//   Bubble:        rounded rect (10, 60, 580×130), #222 fill, hard offset
//                  shadow (0,0,0,80) at +5,+5.
//   Message text:  inside bubble, 145px left padding to clear the avatar,
//                  35px right padding, 20px top/bottom, white bold word-wrap.
// ---------------------------------------------------------------------------
static void RenderPopupToImage(FeedsChatPopupData* d,
                               const QString& senderName,
                               const QString& content,
                               const QImage&  avatar) {
    if (d->rendered_image.size() != QSize((int)d->width, (int)d->height) ||
        d->rendered_image.format() != QImage::Format_RGBA8888) {
        d->rendered_image = QImage((int)d->width, (int)d->height,
                                   QImage::Format_RGBA8888);
    }
    d->rendered_image.fill(Qt::transparent);

    QPainter p(&d->rendered_image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect bubbleRect(10, 60, 580, 130);

    // Drop shadow first so the bubble draws over it. Hard offset shadow
    // (not a real gaussian blur) — cheap, correct-enough for v1.2.0.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 80));
    p.drawRoundedRect(bubbleRect.translated(5, 5), 4, 4);

    // Bubble body
    p.setBrush(QColor("#222222"));
    p.drawRoundedRect(bubbleRect, 4, 4);

    // Message text inside the bubble, padded left to clear avatar
    QFont msgFont = p.font();
    msgFont.setBold(true);
    msgFont.setPixelSize(32);
    p.setFont(msgFont);
    p.setPen(Qt::white);
    QRect textRect = bubbleRect.adjusted(145, 20, -35, -20);
    p.drawText(textRect,
               Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
               content);

    // Username pill — yellow rounded rect, dark text, sized to fit
    QFont pillFont = p.font();
    pillFont.setBold(true);
    pillFont.setPixelSize(24);
    p.setFont(pillFont);
    QFontMetrics fm(pillFont);
    int textWidth  = fm.horizontalAdvance(senderName);
    int textHeight = fm.height();
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
// Upload the current QImage to GPU. Called from video_render when
// texture_dirty is set. video_render runs on the graphics thread so the
// graphics context is already active — no obs_enter_graphics needed.
// ---------------------------------------------------------------------------
static void RegenerateTexture(FeedsChatPopupData* d) {
    // 3b: hardcoded test content. 3c replaces with real data signalled
    // from the dock. For the avatar, just use the fallback — exercises
    // the load/scale/clip path without needing a real meeting.
    QImage avatar;
    {
        std::lock_guard<std::mutex> lock(g_avatarCacheMutex);
        avatar = g_fallbackAvatar;
    }

    const QString senderName = "Test User";
    const QString content =
        "This is what a Feeds chat popup looks like. Long messages will "
        "wrap to multiple lines just like the dock does, using QPainter's "
        "word-wrap rendering.";

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
// OBS source callbacks
// ---------------------------------------------------------------------------
static const char* fcp_get_name(void*) {
    return "Feeds Chat Popup";
}

static void* fcp_create(obs_data_t* /*settings*/, obs_source_t* source) {
    FeedsChatPopupData* d = new FeedsChatPopupData();
    d->source = source;
    return d;
}

static void fcp_destroy(void* data) {
    FeedsChatPopupData* d = static_cast<FeedsChatPopupData*>(data);
    if (!d) return;
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
    // 3b: no user-tunable settings yet. 3c+ will add layout / timeout /
    // position controls as needed.
    return obs_properties_create();
}

static void fcp_video_render(void* data, gs_effect_t* /*effect*/) {
    FeedsChatPopupData* d = static_cast<FeedsChatPopupData*>(data);
    if (!d) return;

    if (d->texture_dirty || !d->texture) {
        RegenerateTexture(d);
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

} // namespace feeds
