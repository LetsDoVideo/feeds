// feeds-speaker-smoothing.h — production-style switching for [Active Speaker]
// sources. Shared by the Windows engine (engine-speaker.cpp) and the macOS
// engine (feeds-mac-video.mm), so both switch identically and log identically.
//
// Zoom's active-speaker signal is built for meetings: it moves the moment
// someone else makes a sound, so a one-word "yeah" or a burst of crosstalk cuts
// a show's speaker source away and straight back. This step sits between each
// engine's existing resolution ("who could be on screen as the active speaker
// right now") and the target the follow sources actually show:
//   * WAIT: a new speaker must stay the active speaker for kSpeakerWaitMs before
//     the cut. A different speaker restarts the wait; the on-screen person
//     becoming active again cancels it.
//   * HOLD: after any cut, stay at least kSpeakerHoldMs. A pending candidate
//     waits the hold out and still needs its full wait.
//   * IMMEDIATE: nobody on screen yet (first speaker), or the on-screen person
//     left or turned their camera off -> cut to the displayable speaker now.
// Zoom's signal is sticky (it names the last person who spoke), so a candidate
// is never re-checked for "still speaking": it stays the input until someone
// else speaks.
//
// Pure: no clock, no SDK, no locks. The caller passes the time, applies the
// returned target, logs the returned line and re-evaluates at dueMs.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace feeds_shared {

static constexpr uint64_t kSpeakerWaitMs = 700;
static constexpr uint64_t kSpeakerHoldMs = 3000;

struct SpeakerSmoother {
    unsigned int candidate        = 0;   // waiting to go on screen, 0 = none
    uint64_t     candidateSinceMs = 0;   // when the current wait started
    uint64_t     lastCutMs        = 0;   // when the on-screen person was cut to
};

struct SpeakerStepResult {
    unsigned int target = 0;   // who follow sources should show, 0 = no one yet
    uint64_t     dueMs  = 0;   // re-evaluate at this time, 0 = nothing pending
    std::string  log;          // one "Speaker:" INFO line, empty if nothing happened
};

// displayable: the active speaker if they can go on screen right now, else 0.
// onScreen: the current target, 0 = no one yet.
// immediateReason: non-null when the on-screen person left or turned their
// camera off; a displayable speaker then goes on screen with no wait or hold.
inline SpeakerStepResult SpeakerSmoothingStep(SpeakerSmoother& s,
                                              unsigned int displayable,
                                              unsigned int onScreen,
                                              const char* immediateReason,
                                              uint64_t nowMs)
{
    SpeakerStepResult r;
    r.target = onScreen;
    char buf[224];

    // No one new to cut to: keep what is on screen and drop any pending wait.
    if (displayable == 0 || displayable == onScreen) {
        if (s.candidate != 0) {
            if (displayable != 0) {
                std::snprintf(buf, sizeof(buf),
                    "Speaker: wait cancelled for %u, %u (on screen) is active again",
                    s.candidate, onScreen);
            } else {
                std::snprintf(buf, sizeof(buf),
                    "Speaker: wait cancelled for %u, no displayable active speaker",
                    s.candidate);
            }
            r.log = buf;
            s.candidate = 0;
        }
        return r;
    }

    if (onScreen == 0 || immediateReason) {
        const char* reason = immediateReason ? immediateReason : "first_speaker";
        std::snprintf(buf, sizeof(buf),
            "Speaker: immediate switch %u -> %u (%s), no wait or hold",
            onScreen, displayable, reason);
        r.log       = buf;
        r.target    = displayable;
        s.candidate = 0;
        s.lastCutMs = nowMs;
        return r;
    }

    const uint64_t holdDue = s.lastCutMs + kSpeakerHoldMs;
    if (s.candidate != displayable) {
        if (s.candidate != 0) {
            std::snprintf(buf, sizeof(buf),
                "Speaker: wait restarted for %u (was candidate %u), %llu ms",
                displayable, s.candidate, (unsigned long long)kSpeakerWaitMs);
        } else {
            std::snprintf(buf, sizeof(buf),
                "Speaker: candidate %u, waiting %llu ms to cut from %u "
                "(hold remaining %llu ms)",
                displayable, (unsigned long long)kSpeakerWaitMs, onScreen,
                (unsigned long long)(holdDue > nowMs ? holdDue - nowMs : 0));
        }
        r.log              = buf;
        s.candidate        = displayable;
        s.candidateSinceMs = nowMs;
    }

    const uint64_t waitDue = s.candidateSinceMs + kSpeakerWaitMs;
    const uint64_t due     = waitDue > holdDue ? waitDue : holdDue;
    if (nowMs < due) {
        r.dueMs = due;
        return r;
    }

    std::snprintf(buf, sizeof(buf),
        "Speaker: cut %u -> %u after waiting %llu ms (%u was on screen %llu ms)",
        onScreen, displayable,
        (unsigned long long)(nowMs - s.candidateSinceMs), onScreen,
        (unsigned long long)(nowMs - s.lastCutMs));
    r.log       = buf;
    r.target    = displayable;
    s.candidate = 0;
    s.lastCutMs = nowMs;
    return r;
}

} // namespace feeds_shared
