// feeds-mac-video.h — participant video and screenshare for the macOS engine.
//
// The macOS counterpart of engine-video.cpp + engine-screenshare.cpp +
// engine-speaker.cpp on Windows. The SDK surface is completely different
// (ZoomSDKRenderer / ZoomSDKRendererDelegate rather than IZoomSDKRenderer /
// IZoomSDKRendererDelegate) but the CONTRACT is identical: the same JSON
// messages in and out, and the same shared-memory frame layout, so the plugin
// reads a macOS engine's frames with exactly the code that reads a Windows
// engine's.
//
// Everything declared here runs on the MAIN queue. The registry of live
// subscriptions is main-queue-confined and therefore holds no lock of its own;
// the one piece of state the SDK's frame-callback thread touches is the shared
// memory writer, which carries its own mutex. See the threading note in the
// .mm for why that split is what keeps renderer teardown off a deadlock.

#pragma once

#include <string>

namespace feeds_mac {

// The raw-livestream privilege has been granted: start raw livestreaming, which
// is what actually turns raw-data delivery on, and open the renderer gate.
// Called from the live-stream delegate's granted callback, in the same place
// the Windows engine calls StartRawLiveStreaming.
void VideoStartRawLiveStream();

// Our own user has appeared in the SDK's raw-live-streaming list. That is the
// reliable "the raw-data renderer subsystem is up" signal — createRenderer
// before it returns a transient not-ready error — so it opens the gate that
// queued subscribes drain through.
void VideoNotifyRawRenderReady(unsigned int userId);

// participant_source_subscribe (recreate = false) and
// participant_source_recreate (recreate = true). A subscribe re-points an
// existing renderer in place where it can; a recreate always destroys and
// rebuilds through the gate, because a kept renderer loses SDK delivery across
// a participant's drop and rejoin.
void VideoHandleSubscribe(const std::string& json, bool recreate);

// participant_source_unsubscribe: tear the renderer down and release the
// region.
void VideoHandleUnsubscribe(const std::string& json);

// Meeting lifecycle. Started arms the gate/heartbeat tick; Ended tears every
// renderer and region down and disarms it.
void VideoMeetingStarted();
void VideoMeetingEnded();

// Roster and speaker inputs, all from the meeting action delegate.
void VideoOnActiveAudio(unsigned int rawUserId);
void VideoOnUserVideoStatusChanged(unsigned int userId, bool videoOn);
void VideoOnUserLeft(unsigned int userId);

// Screenshare: re-derive what is viewable from the share controller and make
// the share renderer match it.
//
// Deliberately takes no arguments. The share-status callbacks report a
// transition and an id, but the transitions are not a reliable state machine to
// drive from — the initialisation value arrives as a status, a content-type
// change re-reports an id that may not have settled, and a share that began
// before we joined produces no transition at all. Asking the controller what is
// viewable NOW answers all three the same way.
void VideoRefreshShareSubscription();

} // namespace feeds_mac
