// engine-speaker.cpp — the single source of truth for "who is the active
// speaker", and the only place that decides who an [Active Speaker] source
// shows.
//
// WHY THIS FILE EXISTS
// Through 1.7.0 there were three independent copies of the active speaker: one
// in engine-meeting.cpp's audio listener, one in engine-video.cpp, and one in
// the plugin — written on three threads with no ordering between them. The
// video copy applied its filters (skip the Feeds user, skip camera-off) as early
// returns that threw the new speaker away, while the meeting copy had already
// recorded that speaker and deduplicated every later event for them. So a
// single stale IsVideoOn() read pinned an [Active Speaker] source to the
// PREVIOUS speaker — typically the person who had just stopped talking and
// muted — and nothing short of leaving the meeting cleared it.
//
// THE MODEL
//   * Store the INPUT, not a decision. g_rawSpeakerId is what the SDK reported,
//     written on every event with no dedup and no filtering, and stamped with a
//     monotonic generation.
//   * DERIVE the decision on the pump thread, fresh on every evaluation: on each
//     raw update, on each follow-source subscribe/recreate, and on a 1 s tick.
//     The filters run here, at resolve time, and never alter the stored input —
//     so a speaker who is not displayable now is simply reconsidered next time.
//   * APPLY level-triggered: every evaluation re-points any follow source that
//     is not on the resolved target, rather than acting only on a change edge.
//   * The plugin receives the RESOLVED id, so the dock row and nameplate name
//     the person actually on screen instead of keeping a copy of their own.
//
// RESOLUTION RULE (the old filters' intent, minus the latch)
//   raw speaker is displayable (not the Feeds user, camera not reported off)
//       -> show them.
//   otherwise -> keep the current target ("keep the last valid speaker on
//       screen", as before), dropping it only if it is the Feeds user.
//   The difference from before is that the raw speaker is still stored, so the
//   moment they become displayable — camera on, or a stale read corrected — the
//   next evaluation cuts to them.
//
// THREADS AND LOCKS
//   g_speakerMutex guards the state block below and is held only for a copy or
//   a commit: never across an SDK getter (the SDK can fire callbacks re-entrantly
//   from inside its getters) and never while taking g_subsMutex. The lock order
//   is one-way, g_subsMutex -> g_speakerMutex: engine-video.cpp may read the
//   resolved target while holding g_subsMutex, and nothing here takes
//   g_subsMutex except through RetargetFollowSources(), called with no lock held.
//
// GENERATION GUARD
//   An evaluation snapshots (raw, generation), derives with no lock held, and
//   commits only if the generation is still current. If a newer input landed
//   mid-derive the result is discarded — that input has already queued its own
//   evaluation — so an older derivation can never overwrite a newer one.
//
// LOGGING — INFO, so it reaches a customer's normal OBS log
//   * "Speaker: state"     whenever raw, resolved or the decision changes;
//   * "Speaker: heartbeat" unconditionally every 5 s while in a meeting, with
//     the follow sources' actual bound user ids, so one line shows whether what
//     is on screen matches what was resolved.

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "zoom_sdk.h"
#include "meeting_service_interface.h"
#include "meeting_service_components/meeting_participants_ctrl_interface.h"

#include "engine-shared.h"
#include "engine-speaker.h"

// Defined in engine-main.cpp
extern void LogInfo(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {

// From engine-meeting.cpp.
ZOOM_SDK_NAMESPACE::IMeetingService* GetMeetingService();
unsigned int GetMySelfUserId();

// The sentinel user ID the plugin sends for "[Active Speaker]". Matches the
// sentinel in engine-video.cpp and plugin-main.cpp.
static constexpr unsigned int ACTIVE_SPEAKER_SENTINEL = 1;
static constexpr unsigned int kNoSpeaker              = 0;

static const UINT kSpeakerTickMs       = 1000;
static const int  kHeartbeatEveryTicks = 5;   // heartbeat every 5 s

// --- Guarded by g_speakerMutex ----------------------------------------------
static std::mutex   g_speakerMutex;
static unsigned int g_rawSpeakerId      = kNoSpeaker;  // SDK input, unfiltered
static uint64_t     g_rawGeneration     = 0;           // bumps on every input
static ULONGLONG    g_rawTick           = 0;           // GetTickCount64 of last input
static unsigned int g_resolvedSpeakerId = kNoSpeaker;  // derived on-screen target

// --- Cross-thread flags ------------------------------------------------------
static std::atomic<bool> g_speakerInMeeting{false};
static std::atomic<bool> g_speakerEvalPosted{false};

// --- Pump thread only --------------------------------------------------------
static bool         g_speakerTimerActive  = false;
static int          g_ticksSinceHeartbeat = 0;
static unsigned int g_loggedRaw           = kNoSpeaker;
static unsigned int g_loggedResolved      = kNoSpeaker;
static const char*  g_loggedDecision      = "";

void RequestSpeakerEvaluation() {
    if (g_speakerEvalPosted.exchange(true)) return;   // one is already queued
    if (!g_anchorWnd ||
        !PostMessageW(g_anchorWnd, WM_FEEDS_SPEAKER_EVAL, 0, 0)) {
        g_speakerEvalPosted.store(false);   // nothing queued; let a later request try
    }
}

void SpeakerOnActiveAudio(unsigned int rawUserId) {
    if (rawUserId == kNoSpeaker) return;   // not a user id
    {
        std::lock_guard<std::mutex> lock(g_speakerMutex);
        g_rawSpeakerId = rawUserId;
        ++g_rawGeneration;
        g_rawTick = GetTickCount64();
    }
    RequestSpeakerEvaluation();
}

void SpeakerMeetingStarted() {
    g_speakerInMeeting.store(true);
    LogInfo("Speaker: meeting started — active-speaker evaluation and "
            "heartbeat armed");
    RequestSpeakerEvaluation();
}

void SpeakerMeetingEnded() {
    {
        std::lock_guard<std::mutex> lock(g_speakerMutex);
        g_rawSpeakerId      = kNoSpeaker;
        g_resolvedSpeakerId = kNoSpeaker;
        g_rawTick           = 0;
        ++g_rawGeneration;   // reject any evaluation already in flight
    }
    g_speakerInMeeting.store(false);
    RequestSpeakerEvaluation();   // the pump thread stops the tick
}

unsigned int GetResolvedActiveSpeakerTarget() {
    std::lock_guard<std::mutex> lock(g_speakerMutex);
    return (g_resolvedSpeakerId != kNoSpeaker) ? g_resolvedSpeakerId
                                               : ACTIVE_SPEAKER_SENTINEL;
}

// Derive the on-screen target from the raw input and the currently committed
// target. Called with NO lock held — it calls SDK getters.
static unsigned int DeriveTarget(unsigned int raw, unsigned int held,
                                 unsigned int me, const char*& decision) {
    // The Feeds user is never a target: OBS is likely feeding them back into
    // Zoom as a virtual camera, so subscribing would loop. This applies to the
    // held target too — one accepted before our own id was known must not
    // survive once it is.
    if (me != 0 && held == me) held = kNoSpeaker;

    if (raw == kNoSpeaker)       { decision = "no_speaker_yet"; return held; }
    if (me != 0 && raw == me)    { decision = "hold_self";      return held; }

    // Camera reported off -> not displayable right now; keep the current target.
    // A missing user record is NOT treated as camera-off (same as before): the
    // participants controller can lag a fresh join, and the next evaluation
    // re-checks anyway.
    if (ZOOM_SDK_NAMESPACE::IMeetingService* ms = GetMeetingService()) {
        if (auto* pc = ms->GetMeetingParticipantsController()) {
            if (auto* ui = pc->GetUserByUserID(raw)) {
                if (!ui->IsVideoOn()) { decision = "hold_video_off"; return held; }
            }
        }
    }

    decision = "accept";
    return raw;
}

static void EvaluateAndApply(bool heartbeat) {
    unsigned int raw     = kNoSpeaker;
    unsigned int held    = kNoSpeaker;
    uint64_t     gen     = 0;
    ULONGLONG    rawTick = 0;
    {
        std::lock_guard<std::mutex> lock(g_speakerMutex);
        raw     = g_rawSpeakerId;
        held    = g_resolvedSpeakerId;
        gen     = g_rawGeneration;
        rawTick = g_rawTick;
    }

    const unsigned int me       = GetMySelfUserId();
    const char*        decision = "";
    const unsigned int resolved = DeriveTarget(raw, held, me, decision);

    bool committed = false;
    {
        std::lock_guard<std::mutex> lock(g_speakerMutex);
        if (g_rawGeneration == gen) {
            g_resolvedSpeakerId = resolved;
            committed = true;
        }
    }
    if (!committed) {
        char msg[192];
        sprintf_s(msg,
            "Speaker: discarded stale evaluation gen=%llu (raw=%u resolved=%u) "
            "— a newer input superseded it",
            (unsigned long long)gen, raw, resolved);
        LogInfo(msg);
        RequestSpeakerEvaluation();   // normally already queued by that input
        return;
    }

    if (raw != g_loggedRaw || resolved != g_loggedResolved ||
        std::strcmp(decision, g_loggedDecision) != 0) {
        char msg[256];
        sprintf_s(msg,
            "Speaker: state raw=%u resolved=%u gen=%llu decision=%s "
            "(was raw=%u resolved=%u)",
            raw, resolved, (unsigned long long)gen, decision,
            g_loggedRaw, g_loggedResolved);
        LogInfo(msg);
        g_loggedRaw      = raw;
        g_loggedResolved = resolved;
        g_loggedDecision = decision;
    }

    // The meeting ended after our commit (SpeakerMeetingEnded can run on another
    // thread): don't re-point or re-announce a target from the old meeting. The
    // evaluation it queued stops the tick.
    if (!g_speakerInMeeting.load()) return;

    // Level-triggered apply. No g_speakerMutex held here (lock order).
    const FollowRetargetResult r = RetargetFollowSources();

    // The plugin mirrors the RESOLVED target (0 = no one on screen yet): the
    // person an [Active Speaker] source is actually showing, which is who its
    // dock row and nameplate must name. Sent on change, and re-sent on every
    // heartbeat so the plugin's mirror can never stay stale either.
    if (resolved != held || heartbeat) {
        char buf[96];
        sprintf_s(buf,
            "{\"type\":\"active_speaker_changed\",\"participant_id\":%u}",
            resolved);
        SendToPlugin(buf);
    }

    if (heartbeat) {
        const ULONGLONG now = GetTickCount64();
        char head[320];
        sprintf_s(head,
            "Speaker: heartbeat raw=%u resolved=%u gen=%llu decision=%s me=%u "
            "raw_age_ms=%lld follow_sources=%d target=%u bound=[",
            raw, resolved, (unsigned long long)gen, decision, me,
            rawTick ? (long long)(now - rawTick) : -1LL, r.follow, r.target);
        std::string line = head;
        line += r.bound;
        line += "]";
        LogInfo(line.c_str());
    }
}

// Out of a meeting: stop the tick and forget the pump-thread log state, so the
// next meeting's first "Speaker: state" line doesn't cite this one's ids.
static void StopSpeakerTick() {
    if (g_speakerTimerActive) {
        if (g_anchorWnd) KillTimer(g_anchorWnd, kSpeakerTimerId);
        g_speakerTimerActive = false;
        LogInfo("Speaker: meeting ended — evaluation and heartbeat stopped");
    }
    g_ticksSinceHeartbeat = 0;
    g_loggedRaw      = kNoSpeaker;
    g_loggedResolved = kNoSpeaker;
    g_loggedDecision = "";
}

void SpeakerEvaluateOnMainThread() {
    // Clear first: any input arriving from here on must queue a fresh evaluation.
    g_speakerEvalPosted.store(false);

    if (!g_speakerInMeeting.load()) {
        StopSpeakerTick();
        return;
    }

    if (!g_speakerTimerActive && g_anchorWnd &&
        SetTimer(g_anchorWnd, kSpeakerTimerId, kSpeakerTickMs, nullptr)) {
        g_speakerTimerActive = true;
    }
    EvaluateAndApply(false);
}

void SpeakerOnTimer() {
    if (!g_speakerInMeeting.load()) {
        StopSpeakerTick();
        return;
    }
    const bool heartbeat = (++g_ticksSinceHeartbeat >= kHeartbeatEveryTicks);
    if (heartbeat) g_ticksSinceHeartbeat = 0;
    EvaluateAndApply(heartbeat);
}

} // namespace feeds_engine
