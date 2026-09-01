#pragma once

#include <string>

#include <QImage>

// Public interface for the feeds_lower_third OBS source — a per-participant
// nameplate (avatar + name + editable title) drawn as a card over that
// participant's video.
//
// It is a SEPARATE source that sits above the participant source in the scene,
// not something composited into the participant's frames. That is the whole
// point: the participant's video is engine-rendered (I420 over shared memory)
// and users routinely put an OBS filter on it (NVIDIA background removal being
// the common one). A sibling scene item drawn after the participant is
// composited only once the participant's own filter chain has already run, so
// the nameplate is cleanly isolated from those filters.
//
// Lifecycle is driven entirely from the Feeds Controls dock (see plugin-main's
// lower-third orchestration): created on demand from the participant's row,
// never auto-created, and never deleted — only shown and hidden. Creating and
// destroying sources mid-stream risks OBS instability; toggling a card's
// visibility does not.
//
// Rendering reuses the chat popup's card pipeline via feeds-card-render.h. The
// state model deliberately does NOT: the popup is a singleton (one message
// fanned out to every instance, one global animation clock), whereas every
// lower third is independent, so each instance owns its own content and its
// own slide.

namespace feeds {

void RegisterLowerThirdSource();

// Push resolved display content into every lower-third instance bound to the
// given participant source UUID. Like the popup, the source is cache-agnostic:
// plugin-main resolves the name (Zoom roster name, falling back to the OBS
// source name), the title (from the participant's settings — the single source
// of truth) and the avatar (from g_avatarCache), and hands them over. A null
// QImage renders the neutral placeholder circle.
//
// Safe to call from any thread; instances are marked dirty and re-render on
// the next frame.
void UpdateLowerThirdContent(const std::string& participantUuid,
                             const std::string& name,
                             const std::string& title,
                             const QImage&      avatar);

// Re-evaluate every lower-third instance's tier_disabled flag against the
// current g_currentTier. The lower third is a Basic-tier feature, gated at
// >= 1 (the chat popup and overlay are gated at >= 2). Called from
// plugin-main's ReconcileSourcesToTier on login_succeeded, mirroring
// ReconcileChatPopupSources / ReconcileChatOverlaySources.
void ReconcileLowerThirdSources();

// Implemented by plugin-main, called by the lower third's own properties panel
// when the user edits the title there. The lower third writes the new value
// through to the participant's settings itself (that is the source of truth);
// this hook lets plugin-main rebuild the dock row and re-push content so the
// other two edit surfaces catch up. Declared here so both sides agree on the
// signature.
void NotifyLowerThirdTitleChanged(const std::string& participantUuid);

}  // namespace feeds
