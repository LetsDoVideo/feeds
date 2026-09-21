// engine-audio.h — per-participant (isolated) raw audio from the Zoom SDK.
//
// One global subscription to the SDK's raw audio. Each participant's one-way
// stream is routed by Zoom user id into the shared-memory audio ring
// (common/shared-audio.h) of every participant subscription currently showing
// that user. The meeting mix is never used: an ISO recording must carry only
// its own participant.

#pragma once

#include <cstdint>
#include <string>

namespace feeds_engine {

// ---------------------------------------------------------------------------
// Sinks — one per participant subscription (engine-video.cpp). Open returns a
// token (never 0) that names this sink for SetUser/Close; a stale token is a
// no-op, so a late Close from a replaced subscription can't tear down its
// successor's sink. Callable from any thread except the SDK audio callback.
// ---------------------------------------------------------------------------
uint64_t OpenIsolatedAudioSink(const std::string& sourceUuid, unsigned int userId);
void     SetIsolatedAudioSinkUser(uint64_t token, unsigned int userId);
void     CloseIsolatedAudioSink(uint64_t token);

// ---------------------------------------------------------------------------
// Subscription lifecycle
// ---------------------------------------------------------------------------

// Any thread. Posts WM_FEEDS_AUDIO_SUBSCRIBE; called once raw data is live.
void RequestIsolatedAudioSubscribe();

// Main (pump) thread, via WM_FEEDS_AUDIO_SUBSCRIBE. Subscribes once per
// meeting; a no-op when already subscribed or when the subscribe failed for a
// reason a retry can't fix.
void IsolatedAudioSubscribeOnMainThread();

// SDK audio-status thread. If the last subscribe failed only because this
// client had not joined computer audio yet, re-requests it.
void IsolatedAudioOnAudioStatusChanged();

// Main thread, at meeting end. Unsubscribes and resets for the next meeting.
void IsolatedAudioMeetingEnded();

} // namespace feeds_engine
