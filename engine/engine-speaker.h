// engine-speaker.h — the active-speaker state layer. See engine-speaker.cpp for
// the model; this header is the whole contract other translation units use.
//
// LOCK ORDER (one-way, load-bearing): g_subsMutex -> g_speakerMutex. Code
// holding g_subsMutex may call GetResolvedActiveSpeakerTarget(). Nothing in
// engine-speaker.cpp ever holds g_speakerMutex while taking g_subsMutex, and
// RetargetFollowSources() must never be called with g_speakerMutex held.

#pragma once

#include <string>

namespace feeds_engine {

// SDK callback thread (any thread). Records the raw active-speaker id from
// onUserActiveAudioChange — every event, unconditionally, with a new generation
// — and asks the pump thread to re-derive. Takes no g_subsMutex and calls no
// SDK getters.
void SpeakerOnActiveAudio(unsigned int rawUserId);

// Any thread. Meeting lifecycle: Started arms the pump-thread re-evaluation and
// heartbeat; Ended clears the state and stops them.
void SpeakerMeetingStarted();
void SpeakerMeetingEnded();

// Any thread. Asks the pump thread to re-derive the target now. Coalesced: a
// burst of requests produces one evaluation.
void RequestSpeakerEvaluation();

// Pump thread only. EngineWndProc calls these on WM_FEEDS_SPEAKER_EVAL and on
// the kSpeakerTimerId WM_TIMER respectively.
void SpeakerEvaluateOnMainThread();
void SpeakerOnTimer();

// Any thread; safe to call while holding g_subsMutex. The on-screen target an
// [Active Speaker] source should show right now, as last derived on the pump
// thread, or ACTIVE_SPEAKER_SENTINEL (1) when there is no one to show yet.
unsigned int GetResolvedActiveSpeakerTarget();

// Defined in engine-video.cpp. Pump thread. Re-points every follow-active-
// speaker subscription whose current user differs from the resolved target.
// Takes g_subsMutex; never touches a specific-participant subscription.
struct FollowRetargetResult {
    unsigned int target    = 0;   // resolved target applied (1 = sentinel)
    int          follow    = 0;   // follow-active-speaker subscriptions seen
    int          repointed = 0;   // of those, re-pointed this pass
    std::string  bound;           // "uuid=userId,..." after the pass
};
FollowRetargetResult RetargetFollowSources();

} // namespace feeds_engine
