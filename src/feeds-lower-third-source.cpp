// feeds-lower-third-source.cpp — Feeds Lower Third OBS source.
//
// A per-participant nameplate: circular avatar + an orange name pill + an
// editable title line, on the same dark rounded card the chat popup draws. The
// card is painted with QPainter into a QImage on the CPU, uploaded to a
// gs_texture_t when content changes, and composited each frame — CPU work is
// amortised across frames, only the GPU blit happens per-frame in steady
// state.
//
// Shares its drawing with the chat popup through feeds-card-render.h (card
// body + shadow, avatar circle + ring, name pill, texture upload, the
// texrender slide clip and its two blend setups, ease-out cubic).
//
// It does NOT share the popup's state model. The popup is a singleton — one
// message at a time, fanned out to every instance from file-scope globals,
// with one global animation clock. Every lower third is independent: its own
// participant, its own name/title/avatar, its own show/hide. So content and
// animation live per instance here, which also makes the state machine
// simpler: there is no "replace the in-flight message" case, hence no pending
// slot, just Hidden -> AnimatingIn -> Shown -> AnimatingOut -> Hidden.
//
// The slide differs from the popup on purpose: the lower third comes in from
// the LEFT (the traditional lower-third entrance) rather than up from below,
// which also keeps it visually distinct from a chat popup on the same stream.
// Same ease-out cubic and same 0.5s duration.

#include "feeds-lower-third-source.h"
#include "feeds-card-render.h"
#include "feeds-version.h"

#include <obs-module.h>
#include <graphics/graphics.h>

#include <QColor>
#include <QDesktopServices>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

// Tier state from plugin-main.cpp. The lower third is gated at Basic (>= 1),
// enforced by ReconcileLowerThirdSources via the tier_disabled flag. Unlike
// the chat popup/overlay (Streamer, >= 2), this is Basic's marquee feature.
extern int g_currentTier;

// Tier threshold for the lower third. Basic (1), Streamer (2) and
// Broadcaster (3) get it; Free (0) does not.
static constexpr int LOWER_THIRD_MIN_TIER = 1;

// obs_data keys on the lower third's own settings.
//   participant_uuid — back-pointer to the participant source this nameplate
//                      belongs to. Durable: libobs persists a source's UUID in
//                      the scene-collection JSON, so the link survives
//                      save/load and an OBS restart (a name would not — the
//                      dock lets users rename sources).
//   shown            — the dock's three-state button writes this. Persisted,
//                      so a hidden nameplate stays hidden across a restart.
//   title            — a DISPLAY MIRROR of the participant's
//                      feeds_lower_third_title, which is the single source of
//                      truth. Present only so this source's properties panel
//                      has something to bind a text field to; edits are
//                      written straight through to the participant. The render
//                      path never trusts it while the participant resolves —
//                      see ResolveDisplayTitle, which is what enforces that
//                      precedence on every settings update, not just on the
//                      properties panel's own path.
static constexpr const char* kLtParticipantUuidKey = "participant_uuid";
static constexpr const char* kLtShownKey           = "shown";
static constexpr const char* kLtTitleKey           = "title";

// The participant-side key holding the one true title string. Mirrored in
// plugin-main.cpp (kLowerThirdTitleKey) — both sides read and write this
// exact key on the PARTICIPANT source's settings.
static constexpr const char* kParticipantTitleKey = "feeds_lower_third_title";

namespace {

// ---------------------------------------------------------------------------
// Canvas-relative layout. Reference design is a 1920x1080 canvas, on which the
// card is 768px wide (40% of canvas) with the avatar/pill/font sizes below
// captured as literals. Every pixel dimension is multiplied by
// `scale = cardWidth / 768.0` so the card looks proportionally identical at
// 720p, 1080p, 1440p, 4K. Width comes from GetCardWidth(); height is FIXED
// (unlike the popup, whose height grows with wrapped message text) — a
// nameplate anchored to the bottom of a video rect must not grow vertically,
// so both text lines elide instead of wrapping.
//
// The scene-item is then scaled to fit the participant's on-canvas rect (see
// plugin-main's PlaceLowerThirdItem). Rendering at a canvas-relative natural
// size rather than at one specific target width is what lets a SINGLE lower
// third source serve N scene-items across N scenes, each sitting over a
// differently-sized copy of the participant.
// ---------------------------------------------------------------------------

static constexpr float kCardCanvasFraction = 0.40f;
static constexpr float kReferenceCardWidth = 768.0f;   // 40% of 1920

// Layout literals at scale 1.0.
static constexpr int L_CARD_LEFT_MARGIN   = 10;
static constexpr int L_CARD_RIGHT_MARGIN  = 10;
static constexpr int L_CARD_TOP_OFFSET    = 4;
static constexpr int L_CARD_HEIGHT        = 120;
static constexpr int L_BOTTOM_MARGIN      = 14;   // room for the drop shadow
static constexpr int L_CARD_CORNER_RADIUS = 4;
static constexpr int L_SHADOW_OFFSET      = 5;
static constexpr int L_AVATAR_INSET       = 12;   // from the card's edges
static constexpr int L_AVATAR_SIZE        = 96;
static constexpr int L_AVATAR_BORDER      = 3;
static constexpr int L_TEXT_GAP           = 16;   // avatar right edge -> text
static constexpr int L_TEXT_RIGHT_PAD     = 18;
static constexpr int L_PILL_TOP           = 16;   // from the card's top edge
static constexpr int L_PILL_H_PAD         = 10;
static constexpr int L_PILL_V_PAD         = 6;
static constexpr int L_PILL_CORNER_RADIUS = 6;
static constexpr int L_NAME_FONT          = 30;
static constexpr int L_TITLE_FONT         = 24;
static constexpr int L_TITLE_TOP_GAP      = 8;    // below the pill

// 40% of the active OBS canvas width, queried per content change so the card
// is the same proportion of the screen at any canvas resolution. Falls back to
// the 1080p reference if obs_get_video_info fails — shouldn't happen during
// active rendering, but defensive against being called before OBS finishes
// initialising.
static uint32_t GetCardWidth() {
    struct obs_video_info ovi;
    if (obs_get_video_info(&ovi)) {
        return (uint32_t)((float)ovi.base_width * kCardCanvasFraction);
    }
    return (uint32_t)kReferenceCardWidth;
}

// The card's pixel footprint for the current canvas. Deterministic — it needs
// no content, only the canvas — so lt_create can report correct dimensions
// before the first frame, which matters because the placement code reads
// obs_source_get_width/height to compute the scene-item scale.
static void ComputeCardSize(uint32_t& outWidth, uint32_t& outHeight) {
    const uint32_t w     = GetCardWidth();
    const float    scale = (float)w / kReferenceCardWidth;
    outWidth  = w;
    outHeight = (uint32_t)((L_CARD_TOP_OFFSET + L_CARD_HEIGHT +
                            L_BOTTOM_MARGIN) * scale);
}

enum class LtAnimState { Hidden, AnimatingIn, Shown, AnimatingOut };

// ---------------------------------------------------------------------------
// Per-instance state. Content (name/title/avatar), visibility and animation
// are all per instance — see the file header for why this deliberately does
// not follow the popup's global model. The content/animation fields are
// guarded by g_ltStateMutex (written from the UI or IPC thread via
// UpdateLowerThirdContent / lt_update, read on the graphics thread in
// lt_video_render). rendered_image and texture are touched only on the
// graphics thread and need no extra locking.
// ---------------------------------------------------------------------------
struct FeedsLowerThirdData {
    obs_source_t* source = nullptr;

    QImage        rendered_image;
    gs_texture_t* texture       = nullptr;
    bool          texture_dirty = true;

    // Intermediate render target for slide clipping — see DrawClippedSlide.
    gs_texrender_t* texrender = nullptr;

    uint32_t      width  = (uint32_t)kReferenceCardWidth;
    uint32_t      height = 138;   // reference layout height at scale 1.0

    // True iff this instance is currently locked out by tier (logged in below
    // LOWER_THIRD_MIN_TIER). Set by ReconcileLowerThirdSources. When true,
    // lt_video_render no-ops and lt_get_properties returns an upgrade message.
    bool          tier_disabled = false;

    std::string   participant_uuid;

    QString       name;
    QString       title;
    QImage        avatar;    // resolved by plugin-main; null -> neutral circle

    bool          shown        = false;
    LtAnimState   anim         = LtAnimState::Hidden;
    float         anim_elapsed = 0.0f;
    float         anim_from    = 1.0f;
    float         anim_to      = 1.0f;
};

// Instance registry. UpdateLowerThirdContent / ReconcileLowerThirdSources walk
// this to reach every instance. Modified on lt_create / lt_destroy.
// Lock ordering is instances (outer) -> state (inner), never reversed.
std::mutex                          g_ltInstancesMutex;
std::vector<FeedsLowerThirdData*>   g_ltInstances;

// Guards the content/visibility/animation fields of every instance.
std::mutex                          g_ltStateMutex;

// ---------------------------------------------------------------------------
// Paint the card into d->rendered_image. name/title/avatar are passed in as a
// snapshot so the caller can take them under the state mutex and release it
// before the slow paint.
// ---------------------------------------------------------------------------
static void RenderLowerThirdToImage(FeedsLowerThirdData* d,
                                    const QString& name,
                                    const QString& title,
                                    const QImage&  avatar) {
    const int   cardWidth = (int)GetCardWidth();
    const float scale     = (float)cardWidth / kReferenceCardWidth;

    const int CARD_LEFT_MARGIN   = (int)(L_CARD_LEFT_MARGIN   * scale);
    const int CARD_RIGHT_MARGIN  = (int)(L_CARD_RIGHT_MARGIN  * scale);
    const int CARD_TOP_OFFSET    = (int)(L_CARD_TOP_OFFSET    * scale);
    const int CARD_HEIGHT        = (int)(L_CARD_HEIGHT        * scale);
    const int BOTTOM_MARGIN      = (int)(L_BOTTOM_MARGIN      * scale);
    const int CARD_CORNER_RADIUS = (int)(L_CARD_CORNER_RADIUS * scale);
    const int SHADOW_OFFSET      = (int)(L_SHADOW_OFFSET      * scale);
    const int AVATAR_INSET       = (int)(L_AVATAR_INSET       * scale);
    const int AVATAR_SIZE        = (int)(L_AVATAR_SIZE        * scale);
    const int AVATAR_BORDER      = std::max(1, (int)(L_AVATAR_BORDER * scale));
    const int TEXT_GAP           = (int)(L_TEXT_GAP           * scale);
    const int TEXT_RIGHT_PAD     = (int)(L_TEXT_RIGHT_PAD     * scale);
    const int PILL_TOP           = (int)(L_PILL_TOP           * scale);
    const int PILL_H_PAD         = (int)(L_PILL_H_PAD         * scale);
    const int PILL_V_PAD         = (int)(L_PILL_V_PAD         * scale);
    const int PILL_CORNER_RADIUS = (int)(L_PILL_CORNER_RADIUS * scale);
    const int NAME_FONT          = (int)(L_NAME_FONT          * scale);
    const int TITLE_FONT         = (int)(L_TITLE_FONT         * scale);
    const int TITLE_TOP_GAP      = (int)(L_TITLE_TOP_GAP      * scale);

    // Fixed height — see the layout note above. Written before the QImage is
    // sized so OBS's next get_width/get_height query sees the current canvas.
    d->width  = (uint32_t)cardWidth;
    d->height = (uint32_t)(CARD_TOP_OFFSET + CARD_HEIGHT + BOTTOM_MARGIN);

    if (d->rendered_image.size() != QSize((int)d->width, (int)d->height) ||
        d->rendered_image.format() != QImage::Format_RGBA8888) {
        d->rendered_image = QImage((int)d->width, (int)d->height,
                                   QImage::Format_RGBA8888);
    }
    d->rendered_image.fill(Qt::transparent);

    QPainter p(&d->rendered_image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect cardRect(
        CARD_LEFT_MARGIN,
        CARD_TOP_OFFSET,
        cardWidth - CARD_LEFT_MARGIN - CARD_RIGHT_MARGIN,
        CARD_HEIGHT);

    feeds::card::DrawCardBody(p, cardRect, CARD_CORNER_RADIUS, SHADOW_OFFSET);

    const QRect avatarRect(cardRect.left() + AVATAR_INSET,
                           cardRect.top()  + AVATAR_INSET,
                           AVATAR_SIZE, AVATAR_SIZE);

    const int textLeft  = avatarRect.right() + TEXT_GAP;
    const int textRight = cardRect.right() - TEXT_RIGHT_PAD;
    const int textWidth = std::max(0, textRight - textLeft);

    // Name pill. The pill sizes itself to its text, so the NAME is elided
    // first — against the width the pill's own padding leaves — or a long
    // display name would push the pill past the card's right edge. DrawPill
    // derives its font from the painter's current font, so set the family/
    // weight baseline here.
    QFont baseFont;
    baseFont.setBold(true);
    baseFont.setPixelSize(NAME_FONT);
    p.setFont(baseFont);

    QFontMetrics nameFm(baseFont);
    const int nameAvail = std::max(0, textWidth - 2 * PILL_H_PAD);
    const QString elidedName =
        nameFm.elidedText(name, Qt::ElideRight, nameAvail);

    // An empty name would draw a bare stub of a pill. That only happens in the
    // frame or two between a card being created/restored and plugin-main
    // pushing its content, so skip the pill and reserve its height instead —
    // the title line still lands where it will once the name arrives.
    const QPoint pillOrigin(textLeft, cardRect.top() + PILL_TOP);
    const QRect  pillRect =
        elidedName.isEmpty()
            ? QRect(pillOrigin,
                    QSize(0, nameFm.height() + 2 * PILL_V_PAD))
            : feeds::card::DrawPill(p, pillOrigin, elidedName, NAME_FONT,
                                    PILL_H_PAD, PILL_V_PAD,
                                    PILL_CORNER_RADIUS);

    // Title line, below the pill, white on the card body. Single line and
    // elided — a nameplate has a fixed height, so long titles truncate rather
    // than reflow. An empty title simply leaves the row blank (the card is
    // still meaningful as an avatar + name plate).
    if (!title.isEmpty()) {
        QFont titleFont;
        titleFont.setBold(false);
        titleFont.setPixelSize(TITLE_FONT);
        p.setFont(titleFont);
        p.setPen(Qt::white);

        const int titleTop = pillRect.bottom() + TITLE_TOP_GAP;
        const QRect titleRect(textLeft, titleTop, textWidth,
                              std::max(0, cardRect.bottom() - titleTop));

        QFontMetrics titleFm(titleFont);
        p.drawText(titleRect,
                   Qt::AlignLeft | Qt::AlignTop,
                   titleFm.elidedText(title, Qt::ElideRight, textWidth));
    }

    // Avatar last, matching the popup's draw order (the ring is the only thing
    // that may overlap the card edge).
    feeds::card::DrawAvatarCircle(p, avatarRect, avatar, AVATAR_BORDER);

    p.end();
}

// Repaint + re-upload. Called from video_render on the graphics thread, so the
// graphics context is already active — no obs_enter_graphics needed.
static void RegenerateTexture(FeedsLowerThirdData* d,
                              const QString& name,
                              const QString& title,
                              const QImage&  avatar) {
    RenderLowerThirdToImage(d, name, title, avatar);
    feeds::card::UploadImageToTexture(d->rendered_image, d->width, d->height,
                                      d->texture);
    d->texture_dirty = false;
}

// Current X-offset fraction: 0 = card in place, 1 = card translated fully off
// the left edge of its box. Caller must hold g_ltStateMutex.
static float ComputeFraction_locked(const FeedsLowerThirdData* d) {
    switch (d->anim) {
        case LtAnimState::Hidden: return 1.0f;
        case LtAnimState::Shown:  return 0.0f;
        case LtAnimState::AnimatingIn:
        case LtAnimState::AnimatingOut: {
            float t = d->anim_elapsed / feeds::card::kSlideDurationSeconds;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            const float e = feeds::card::EaseOutCubic(t);
            return d->anim_from + (d->anim_to - d->anim_from) * e;
        }
    }
    return 0.0f;
}

// Begin a slide toward `show`, starting from wherever the card currently sits
// so a rapid show/hide/show reverses smoothly instead of jumping. Caller must
// hold g_ltStateMutex.
static void StartSlide_locked(FeedsLowerThirdData* d, bool show) {
    const float cur = ComputeFraction_locked(d);
    d->anim         = show ? LtAnimState::AnimatingIn : LtAnimState::AnimatingOut;
    d->anim_elapsed = 0.0f;
    d->anim_from    = cur;
    d->anim_to      = show ? 0.0f : 1.0f;
}

// ---------------------------------------------------------------------------
// Resolve the participant source this instance points at, as an owned ref the
// caller must release (or null). Used by the properties panel to read and
// write the canonical title.
// ---------------------------------------------------------------------------
static obs_source_t* ResolveParticipant(const std::string& uuid) {
    if (uuid.empty()) return nullptr;
    return obs_get_source_by_uuid(uuid.c_str());
}

// Reads the canonical title into `out` and reports whether the participant
// actually resolved. The bool matters: an EMPTY canonical title is a
// legitimate value (the user cleared the field), so callers must be able to
// tell "no title" from "no participant" rather than treating both as "" and
// falling back to a stale mirror.
static bool ReadParticipantTitle(const std::string& participantUuid,
                                 std::string& out) {
    obs_source_t* p = ResolveParticipant(participantUuid);
    if (!p) return false;
    out.clear();
    if (obs_data_t* s = obs_source_get_settings(p)) {
        const char* t = obs_data_get_string(s, kParticipantTitleKey);
        if (t) out = t;
        obs_data_release(s);
    }
    obs_source_release(p);
    return true;
}

// The title this instance should DISPLAY: the participant's setting whenever
// the participant resolves, and only otherwise the mirror off our own
// settings. Must be called WITHOUT g_ltStateMutex held — it walks libobs'
// source list, and the lock ordering here keeps Feeds mutexes innermost.
static QString ResolveDisplayTitle(const std::string& participantUuid,
                                   const char*        mirror) {
    std::string truth;
    if (ReadParticipantTitle(participantUuid, truth))
        return QString::fromStdString(truth);
    return QString::fromUtf8(mirror ? mirror : "");
}

// ---------------------------------------------------------------------------
// OBS source callbacks
// ---------------------------------------------------------------------------
static const char* lt_get_name(void*) {
    // Display name only — the source ID ("feeds_lower_third") is the stable
    // key OBS uses in saved scene collections and must never change. Instances
    // are renamed to "Lower Third — <participant>" by the dock when created.
    return "Feeds Lower Third";
}

static void lt_get_defaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, kLtParticipantUuidKey, "");
    obs_data_set_default_string(settings, kLtTitleKey, "");
    obs_data_set_default_bool(settings, kLtShownKey, false);
}

static void* lt_create(obs_data_t* settings, obs_source_t* source) {
    // Always create, never return nullptr on a policy block. Returning nullptr
    // from a create callback makes OBS keep an invalid husk that the next
    // auto-save bakes into the scene JSON as a permanent loss — the same trap
    // the participant and chat popup sources document. A tier-blocked or
    // unbound lower third is created dormant instead: it renders nothing and
    // its properties panel explains why.
    FeedsLowerThirdData* d = new FeedsLowerThirdData();
    d->source = source;

    // Report correct dimensions immediately: the placement code reads
    // obs_source_get_width/height to compute the scene-item scale, and it runs
    // right after the source is added. The card's size depends only on the
    // canvas, so it is known before any content arrives.
    ComputeCardSize(d->width, d->height);

    if (settings) {
        const char* pu = obs_data_get_string(settings, kLtParticipantUuidKey);
        if (pu) d->participant_uuid = pu;
        d->shown = obs_data_get_bool(settings, kLtShownKey);
        // A lower third restored from a saved scene collection comes back in
        // whatever state it was left in — shown means shown, with no slide
        // (there is nothing to animate into on a scene load).
        d->anim = d->shown ? LtAnimState::Shown : LtAnimState::Hidden;
        // Mirror only, deliberately: during a scene-collection load the
        // participant we point at may not have been created yet, so the truth
        // is unreadable here. lt_load re-reads it once the whole collection
        // exists — see the info.load contract.
        const char* t = obs_data_get_string(settings, kLtTitleKey);
        if (t) d->title = QString::fromUtf8(t);
    }

    // Allocate the slide-clip texrender up front. gs_texrender_create requires
    // the graphics context; create is called from the Qt main thread, so we
    // enter/leave manually. If allocation fails (rare, graphics-memory
    // pressure), DrawClippedSlide falls back to a direct draw — the card still
    // works, just without slide-edge clipping.
    obs_enter_graphics();
    d->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    obs_leave_graphics();

    {
        std::lock_guard<std::mutex> lock(g_ltInstancesMutex);
        g_ltInstances.push_back(d);
    }

    return d;
}

static void lt_destroy(void* data) {
    FeedsLowerThirdData* d = static_cast<FeedsLowerThirdData*>(data);
    if (!d) return;

    {
        std::lock_guard<std::mutex> lock(g_ltInstancesMutex);
        g_ltInstances.erase(
            std::remove(g_ltInstances.begin(), g_ltInstances.end(), d),
            g_ltInstances.end());
    }

    // Both teardowns need the graphics context, and destroy is not guaranteed
    // to run on the graphics thread. One enter/leave amortises the switch.
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

static uint32_t lt_get_width(void* data) {
    FeedsLowerThirdData* d = static_cast<FeedsLowerThirdData*>(data);
    return d ? d->width : 0;
}

static uint32_t lt_get_height(void* data) {
    FeedsLowerThirdData* d = static_cast<FeedsLowerThirdData*>(data);
    return d ? d->height : 0;
}

static void lt_update(void* data, obs_data_t* settings) {
    FeedsLowerThirdData* d = static_cast<FeedsLowerThirdData*>(data);
    if (!d || !settings) return;

    const char* pu    = obs_data_get_string(settings, kLtParticipantUuidKey);
    const bool  shown = obs_data_get_bool(settings, kLtShownKey);
    const char* mirror = obs_data_get_string(settings, kLtTitleKey);

    // Resolve the title BEFORE taking the state lock (ResolveDisplayTitle
    // walks libobs' source list).
    //
    // Truth-first, not mirror-first, and that ordering is load-bearing: EVERY
    // caller of obs_source_update hands lt_update the WHOLE settings object,
    // including a title mirror it may never have written. The dock's show/hide
    // is exactly that caller — it writes `shown` and nothing else — so a
    // mirror-wins assignment here overwrote the title that the dock's
    // UpdateLowerThirdContent push had just delivered, and the re-shown card
    // came up blank until the user nudged the title field and re-triggered the
    // push. Re-reading the participant makes any settings update, show
    // included, re-apply the stored title instead of clobbering it.
    const QString title = ResolveDisplayTitle(pu ? pu : "", mirror);

    std::lock_guard<std::mutex> lock(g_ltStateMutex);

    if (pu && d->participant_uuid != pu) {
        d->participant_uuid = pu;
        d->texture_dirty    = true;
    }

    if (d->title != title) {
        d->title         = title;
        d->texture_dirty = true;
    }

    if (shown != d->shown) {
        d->shown = shown;
        StartSlide_locked(d, shown);
    }
}

// libobs calls this once the ENTIRE scene collection has been created —
// info.load is documented as running "after all the loading sources have
// actually been created because sometimes there are sources that depend on
// each other", and this source is precisely that case: at lt_create time the
// participant holding our title may not exist yet.
//
// Re-running lt_update against the same settings is all that is needed: it
// re-reads the title from the participant (which now resolves) and leaves
// `shown` alone, since lt_create already applied the same value, so there is
// no spurious slide on load. This is what makes a nameplate restored from a
// saved scene collection come up showing its stored title with no dock
// interaction at all.
static void lt_load(void* data, obs_data_t* settings) {
    lt_update(data, settings);
}

static obs_properties_t* lt_get_properties(void* data) {
    FeedsLowerThirdData* d = static_cast<FeedsLowerThirdData*>(data);

    // Tier-locked branch: replace the normal panel with an upgrade message +
    // button, mirroring zp_properties / fcp_get_properties.
    if (d && d->tier_disabled) {
        obs_properties_t* props = obs_properties_create();
        std::string verLabel = std::string("Feeds (v") +
                               feeds_shared::VERSION + ")";
        obs_properties_add_text(props, "ver_label", verLabel.c_str(),
                                OBS_TEXT_INFO);
        obs_properties_add_text(props, "tier_disabled_msg",
            "Feeds Lower Third is a Basic-tier feature. "
            "Your current tier is Free.",
            OBS_TEXT_INFO);
        obs_properties_add_button(props, "upgrade_btn",
            "Upgrade your plan to enable Feeds Lower Third",
            [](obs_properties_t*, obs_property_t*, void*) -> bool {
                QDesktopServices::openUrl(
                    QUrl("https://letsdovideo.com/feeds-upgrade"));
                return true;
            });
        return props;
    }

    obs_properties_t* props = obs_properties_create();

    // Seed the title mirror from the participant (the source of truth) so the
    // field opens showing the real value, whichever surface last edited it.
    // Written into the live settings object WITHOUT obs_source_update: the
    // properties view reads settings after this call returns, and calling
    // update from inside get_properties invites re-entrancy.
    if (d && !d->participant_uuid.empty()) {
        std::string truth;
        if (ReadParticipantTitle(d->participant_uuid, truth)) {
            if (obs_data_t* s = obs_source_get_settings(d->source)) {
                obs_data_set_string(s, kLtTitleKey, truth.c_str());
                obs_data_release(s);
            }
        }
    }

    obs_property_t* titleProp = obs_properties_add_text(
        props, kLtTitleKey, "Lower Third Title", OBS_TEXT_DEFAULT);

    // Write-through. This field is the one edit surface that cannot bind
    // directly to the canonical value (an OBS property can only bind to its
    // OWN source's settings), so an edit here is pushed to the participant's
    // settings — the single source of truth — and plugin-main is told, so the
    // dock row and the participant's own properties panel catch up.
    obs_property_set_modified_callback2(titleProp,
        [](void* priv, obs_properties_t*, obs_property_t*,
           obs_data_t* settings) -> bool {
            FeedsLowerThirdData* d = static_cast<FeedsLowerThirdData*>(priv);
            if (!d || d->participant_uuid.empty() || !settings) return false;

            const char* v = obs_data_get_string(settings, kLtTitleKey);
            const std::string newTitle = v ? v : "";

            obs_source_t* p = ResolveParticipant(d->participant_uuid);
            if (!p) return false;
            if (obs_data_t* ps = obs_source_get_settings(p)) {
                obs_data_set_string(ps, kParticipantTitleKey,
                                    newTitle.c_str());
                obs_source_update(p, ps);
                obs_data_release(ps);
            }
            obs_source_release(p);

            feeds::NotifyLowerThirdTitleChanged(d->participant_uuid);
            return false;   // value persists; no property layout change
        }, data);

    if (!d || d->participant_uuid.empty()) {
        obs_properties_add_text(props, "orphan_msg",
            "This lower third is not linked to a participant source. "
            "Create lower thirds from the participant's row in the Feeds "
            "Controls dock.",
            OBS_TEXT_INFO);
        return props;
    }

    obs_properties_add_text(props, "managed_msg",
        "This source is managed from the Feeds Controls dock — use the "
        "Show / Hide button on the participant's row. Showing it "
        "re-positions the card over the participant's current video.",
        OBS_TEXT_INFO);

    return props;
}

// Advance this instance's own animation. Unlike the popup — whose single
// global clock forces a "am I the first instance" gate — every lower third
// animates independently, so each one just steps its own counter.
static void lt_video_tick(void* data, float seconds) {
    FeedsLowerThirdData* d = static_cast<FeedsLowerThirdData*>(data);
    if (!d) return;

    std::lock_guard<std::mutex> lock(g_ltStateMutex);
    if (d->anim != LtAnimState::AnimatingIn &&
        d->anim != LtAnimState::AnimatingOut) {
        return;
    }
    d->anim_elapsed += seconds;
    if (d->anim_elapsed < feeds::card::kSlideDurationSeconds) return;

    d->anim = (d->anim == LtAnimState::AnimatingIn) ? LtAnimState::Shown
                                                    : LtAnimState::Hidden;
    d->anim_elapsed = 0.0f;
    d->anim_from    = (d->anim == LtAnimState::Shown) ? 0.0f : 1.0f;
    d->anim_to      = d->anim_from;
}

static void lt_video_render(void* data, gs_effect_t* /*effect*/) {
    FeedsLowerThirdData* d = static_cast<FeedsLowerThirdData*>(data);
    if (!d) return;

    // Tier-locked: render nothing so the stream stays clean. The properties
    // panel still tells the user why (see lt_get_properties).
    if (d->tier_disabled) return;

    // Snapshot under lock so a concurrent UpdateLowerThirdContent on the UI or
    // IPC thread can't tear the QStrings mid-render. Clear texture_dirty under
    // the same lock — a push landing after we release sets it again and the
    // next frame catches up.
    bool    dirty;
    QString name;
    QString title;
    QImage  avatar;
    float   fraction;
    {
        std::lock_guard<std::mutex> lock(g_ltStateMutex);

        // Fully hidden — nothing to draw at all. (Mid-slide states fall
        // through: the card is partially on screen.)
        //
        // Bail BEFORE touching texture_dirty. Consuming the flag here would
        // throw away a content change that arrived while the card was hidden:
        // no texture is regenerated on a hidden frame, so the next show slides
        // in the stale one. Nothing re-dirties it either — the show path now
        // resolves the same title the push already delivered, so it has no
        // difference to notice. Leaving the flag set is what makes an edit made
        // while hidden appear on the next show.
        if (d->anim == LtAnimState::Hidden) return;

        dirty    = d->texture_dirty;
        name     = d->name;
        title    = d->title;
        avatar   = d->avatar;
        if (dirty) d->texture_dirty = false;
        fraction = ComputeFraction_locked(d);
    }

    if (dirty || !d->texture) {
        RegenerateTexture(d, name, title, avatar);
    }
    if (!d->texture) return;

    // Slide in from the LEFT: fraction 1 translates the card one full box
    // width to the left (entirely outside the clip box), fraction 0 leaves it
    // in place. The popup's equivalent is a positive Y offset.
    const float xOffset = -fraction * (float)d->width;

    feeds::card::DrawClippedSlide(d->texrender, d->texture,
                                  d->width, d->height, xOffset, 0.0f);
}

static struct obs_source_info feeds_lower_third_info = {};

}  // namespace

namespace feeds {

void RegisterLowerThirdSource() {
    feeds_lower_third_info.id             = "feeds_lower_third";
    feeds_lower_third_info.type           = OBS_SOURCE_TYPE_INPUT;
    feeds_lower_third_info.output_flags   = OBS_SOURCE_VIDEO |
                                            OBS_SOURCE_CUSTOM_DRAW;
    feeds_lower_third_info.get_name       = lt_get_name;
    feeds_lower_third_info.create         = lt_create;
    feeds_lower_third_info.destroy        = lt_destroy;
    feeds_lower_third_info.get_width      = lt_get_width;
    feeds_lower_third_info.get_height     = lt_get_height;
    feeds_lower_third_info.get_defaults   = lt_get_defaults;
    feeds_lower_third_info.get_properties = lt_get_properties;
    feeds_lower_third_info.update         = lt_update;
    feeds_lower_third_info.load           = lt_load;
    feeds_lower_third_info.video_render   = lt_video_render;
    feeds_lower_third_info.video_tick     = lt_video_tick;
    feeds_lower_third_info.icon_type      = OBS_ICON_TYPE_TEXT;
    obs_register_source(&feeds_lower_third_info);
}

void UpdateLowerThirdContent(const std::string& participantUuid,
                             const std::string& name,
                             const std::string& title,
                             const QImage&      avatar) {
    if (participantUuid.empty()) return;

    const QString qName  = QString::fromStdString(name);
    const QString qTitle = QString::fromStdString(title);

    std::lock_guard<std::mutex> instLock(g_ltInstancesMutex);
    std::lock_guard<std::mutex> stateLock(g_ltStateMutex);
    for (FeedsLowerThirdData* d : g_ltInstances) {
        if (!d || d->participant_uuid != participantUuid) continue;
        if (d->name != qName || d->title != qTitle) {
            d->name          = qName;
            d->title         = qTitle;
            d->texture_dirty = true;
        }
        // Avatar assigned unconditionally (cheap COW). It isn't part of the
        // change test above because a late-arriving avatar for an unchanged
        // name would otherwise never repaint, so mark dirty when it differs by
        // identity — QImage comparison by cacheKey is cheap and exact.
        if (d->avatar.cacheKey() != avatar.cacheKey()) {
            d->avatar        = avatar;
            d->texture_dirty = true;
        }
    }
}

void ReconcileLowerThirdSources() {
    const bool shouldDisable = (g_currentTier < LOWER_THIRD_MIN_TIER);
    std::lock_guard<std::mutex> lock(g_ltInstancesMutex);
    for (FeedsLowerThirdData* d : g_ltInstances) {
        if (!d) continue;
        if (d->tier_disabled != shouldDisable) {
            d->tier_disabled = shouldDisable;
            // Mark dirty so a re-enabled instance refreshes whatever it was
            // showing. The render path short-circuits on tier_disabled anyway.
            d->texture_dirty = true;
        }
    }
}

}  // namespace feeds
