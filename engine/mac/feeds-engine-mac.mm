// feeds-engine-mac.mm — FeedsEngine for macOS (Objective-C++).
//
// The Windows engine (engine/engine-*.cpp) is written against the Zoom Meeting
// SDK's C++ surface: zoom_sdk.h, the ZOOM_SDK_NAMESPACE, IAuthService,
// IMeetingService, InitSDK(InitParam), SDKAuth(AuthContext). The macOS Meeting
// SDK shares none of it — ZoomSDK.framework is a pure Objective-C framework
// (ZoomSDK, ZoomSDKAuthService, ZoomSDKMeetingService, ZoomSDKRawDataController
// and delegate protocols). So this is a rewrite against that API, not a port of
// those files. What IS shared is the contract: the same JSON messages, so
// PROTOCOL.md describes both platforms.
//
// ── WHAT LIVES WHERE ─────────────────────────────────────────────────────────
// This file: the process, the IPC, the SDK's bring-up and authentication, the
// meeting (join / leave / status), the roster, and the raw-livestream privilege
// request that un-greys the plugin's dock and source-properties dialog.
//
// feeds-mac-login.mm: the whole Zoom login path — OAuth, session restore, the
// ZAK fetch a join begins with.
//
// feeds-mac-video.mm: everything downstream of the privilege — starting raw
// livestreaming, the renderers, the shared-memory frame transport, the
// active-speaker target and screenshare. This file feeds it the SDK events it
// needs (privilege granted, meeting started/ended, active audio, camera state,
// departures) and hands it the plugin's source messages; it owns all of the
// state behind them.
//
// ── Threading: the constraint that shapes this file ──────────────────────────
// The macOS SDK is an AppKit client: it delivers every result through
// Objective-C delegate callbacks dispatched on the MAIN run loop. If the main
// thread is parked in a read() loop, no delegate can ever fire and auth hangs
// with no error at all. So:
//   * the main thread runs the Cocoa run loop and nothing else;
//   * the IPC reader runs on its own thread;
//   * every SDK call is hopped onto the main queue, and delegate callbacks
//     arrive there;
//   * IPC writes are serialized with a mutex, so writes from the reader thread
//     and the main thread stay line-atomic;
//   * everything that touches the network — the whole login path, and the ZAK
//     fetch a join begins with — runs on its own detached thread, because all
//     of it blocks (see feeds-mac-net.h).
// Note that initSDKWithParams legitimately BLOCKS the main thread for many
// seconds. Nothing in this increment depends on main-thread responsiveness
// during that window; when a heartbeat is added (with the plugin-side watchdog
// in a later increment) it must be written from the main queue, or it would
// vouch for a main thread that is actually wedged.
//
// ── Memory management: ARC ───────────────────────────────────────────────────
// The OBS template turns ARC on for the whole project (CLANG_ENABLE_OBJC_ARC in
// cmake/macos/xcode.cmake), so it is on here too and explicit retain/release is
// a compile error, not a style choice. One memory model across the engine: no
// per-file opt-out. What this file still has to be deliberate about is the
// delegates' LIFETIME, because the SDK's delegate properties are unowned — see
// the note at their definitions.
//
// ── The SDK runtime must be inside this app bundle ───────────────────────────
// ZoomSDK.framework is not self-contained: at auth time it loads sibling
// bundles through the MAIN BUNDLE's Frameworks directory, not via rpath. Run
// the engine as a loose executable and initSDK still reports success while
// sdkAuth then fails with no delegate callback at all. Hence FeedsEngine.app
// with the SDK in Contents/Frameworks, and the preflight check below that turns
// a missing runtime into one explicit message instead of a silent auth failure.

#import <Cocoa/Cocoa.h>
#import <ZoomSDK/ZoomSDK.h>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "feeds-ipc-posix.h"
#include "feeds-json-lite.h"
#include "feeds-mac-login.h"
#include "feeds-mac-video.h"
#include "feeds-version.h"

// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------
static std::mutex        g_writeMutex;
static std::atomic<bool> g_running{true};

static bool SendToPlugin(const std::string& json)
{
    std::lock_guard<std::mutex> lock(g_writeMutex);
    return feeds_ipc::WriteLine(feeds_ipc::kEngineIpcFd, json);
}

// The engine has no log file of its own: every line is forwarded to the plugin,
// which re-emits it into the OBS log at the matching level. Same contract as the
// Windows engine's LogInfo / LogWarn / LogError.
static void Log(const char* level, const std::string& message)
{
    SendToPlugin(std::string("{\"type\":\"log\",\"level\":\"") + level +
                 "\",\"message\":\"" + feeds::JsonEscape(message) + "\"}");
}
static void LogInfo(const std::string& m)  { Log("info", m); }
static void LogDebug(const std::string& m) { Log("debug", m); }
static void LogWarn(const std::string& m)  { Log("warning", m); }
static void LogError(const std::string& m) { Log("error", m); }

// The login module reports through the engine rather than reaching for the
// socket itself; these are the definitions its header declares.
namespace feeds_mac {
void EngineSend(const std::string& json)                      { SendToPlugin(json); }
void EngineLog(const char* level, const std::string& message) { Log(level, message); }
} // namespace feeds_mac

// ---------------------------------------------------------------------------
// Auth result naming
//
// The macOS ZoomSDKAuthError enum is a different type with different numeric
// values from the Windows AuthResult, so the number alone is not comparable
// across platforms. Log the name as well, in the same vocabulary the Windows
// engine reports, so one support bundle reads the same on either OS.
// ---------------------------------------------------------------------------
static const char* AuthErrorName(ZoomSDKAuthError ret)
{
    switch (ret) {
    case ZoomSDKAuthError_Success:                return "SUCCESS";
    case ZoomSDKAuthError_KeyOrSecretWrong:       return "KEY_OR_SECRET_WRONG";
    case ZoomSDKAuthError_AccountNotSupport:      return "ACCOUNT_NOT_SUPPORT";
    case ZoomSDKAuthError_AccountNotEnableSDK:    return "ACCOUNT_NOT_ENABLE_SDK";
    case ZoomSDKAuthError_Timeout:                return "TIMEOUT";
    case ZoomSDKAuthError_NetworkIssue:           return "NETWORK_ISSUE";
    case ZoomSDKAuthError_Client_Incompatible:    return "CLIENT_INCOMPATIBLE";
    case ZoomSDKAuthError_JwtTokenWrong:          return "JWT_TOKEN_WRONG";
    case ZoomSDKAuthError_KeyOrSecretEmpty:       return "KEY_OR_SECRET_EMPTY";
    case ZoomSDKAuthError_LimitExceededException: return "LIMIT_EXCEEDED";
    case ZoomSDKAuthError_Unknown:                return "UNKNOWN";
    default:                                      return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// Join failure text
//
// The plugin shows the `message` verbatim, so these are the strings the user
// reads. Word for word the same sentences the Windows engine sends for the same
// situation — the macOS enum is a different type with different numbers, but a
// wrong password has to read identically on both platforms or the same support
// answer stops fitting both.
// ---------------------------------------------------------------------------
static std::string MeetingErrorMessage(ZoomSDKMeetingError err)
{
    switch (err) {
    case ZoomSDKMeetingError_PasswordError:
        return "Incorrect meeting password. Please try again.";
    case ZoomSDKMeetingError_MeetingNotExist:
    case ZoomSDKMeetingError_vanityNotExist:
        return "Meeting not found. Please check the meeting number or link.";
    case ZoomSDKMeetingError_ConnectionError:
    case ZoomSDKMeetingError_ReconnectFailed:
        return "Connection error. Please check your internet connection and try again.";
    case ZoomSDKMeetingError_HostDisallowOutsideUserJoin:
        return "The host has disabled external participants from joining this meeting.";
    case ZoomSDKMeetingError_UnableToJoinExternalMeeting:
        return "This app must be published on the Zoom Marketplace before joining external meetings.";
    case ZoomSDKMeetingError_AppCanNotAnonymousJoinMeeting:
        return "This meeting requires you to be logged in to Zoom. Please log in and try again.";
    case ZoomSDKMeetingError_BlockedByAccountAdmin:
        return "Your Zoom account administrator has blocked this application.";
    case ZoomSDKMeetingError_NeedSigninForPrivateMeeting:
        return "This is a private meeting. Please log in to Zoom and try again.";
    case ZoomSDKMeetingError_MeetingOver:
        return "This meeting has already ended.";
    case ZoomSDKMeetingError_MeetingNotStart:
        return "This meeting has not started yet.";
    case ZoomSDKMeetingError_UserFull:
        return "This meeting is at maximum capacity.";
    case ZoomSDKMeetingError_MeetingRestricted:
        return "This meeting is restricted.";
    case ZoomSDKMeetingError_MeetingJBHRestricted:
        return "This meeting does not allow joining before the host.";
    case ZoomSDKMeetingError_RegisterWebinarEnforceLogin:
        return "This meeting requires you to be logged in to Zoom.";
    case ZoomSDKMeetingError_RegisterWebinarFull:
        return "This webinar has reached its registration limit and is not accepting new attendees.";
    case ZoomSDKMeetingError_RegisterWebinarHostRegister:
        return "This webinar requires the host to complete registration before joining.";
    case ZoomSDKMeetingError_RegisterWebinarPanelistRegister:
        return "Unable to join as panelist. The webinar host may need to add you as "
               "a panelist on Zoom's website first.";
    case ZoomSDKMeetingError_RegisterWebinarDeniedEmail:
        return "Your email address is not authorized to register for this webinar.";
    case ZoomSDKMeetingError_joinWebinarWithSameEmail:
        return "You are already in this webinar from another session. Please leave "
               "the other session and try again.";
    default:
        return "Failed to join meeting. Error code: " + std::to_string((int)err);
    }
}

// ---------------------------------------------------------------------------
// SDK state
//
// Lazy bring-up, exactly as on Windows (engine-sdk.cpp): the engine does NOT
// initialize the Zoom SDK at startup. A logged-in-but-not-connected Feeds holds
// no SDK and no Zoom session, so an idle OBS carries no Zoom cost at all. The
// SDK comes up the first time the user actually connects, and the join that
// triggered it is replayed once auth lands.
//
//   g_sdkAuthenticated     — onZoomSDKAuthReturn(Success) has fired and the
//                            meeting service exists; joins may proceed.
//   g_sdkBringupInProgress — between the first EnsureSdkUpThen and that
//                            callback; stops a second connect starting a
//                            second init.
//   g_pendingSdkActions    — joins queued during bring-up; drained on success,
//                            failed with meeting_failed on failure so the
//                            plugin's Connect button re-enables.
// All three are guarded by g_sdkStateMutex.
// ---------------------------------------------------------------------------
static std::mutex                         g_sdkStateMutex;
static bool                               g_sdkAuthenticated     = false;
static bool                               g_sdkBringupInProgress = false;
static std::vector<std::function<void()>> g_pendingSdkActions;

static ZoomSDKMeetingService* g_meetingService = nil;   // owned by the SDK

static void BringUpSdk();                    // main queue only
static void SendParticipantList();           // main queue only
static void RequestRawLiveStreamPrivilege(); // main queue only

// Auth succeeded: flip the ready flag and replay the queued join(s), which
// re-enter their handler and this time sail past EnsureSdkUpThen. On a detached
// thread because a join starts with a blocking ZAK fetch and the main thread
// owes the SDK its run loop.
static void DrainPendingSdkActions()
{
    std::vector<std::function<void()>> actions;
    {
        std::lock_guard<std::mutex> lock(g_sdkStateMutex);
        g_sdkAuthenticated     = true;
        g_sdkBringupInProgress = false;
        actions.swap(g_pendingSdkActions);
    }
    if (actions.empty()) return;

    LogInfo("Mac engine: SDK bring-up complete, draining " +
            std::to_string(actions.size()) + " queued join(s)");
    std::thread([actions]() {
        for (const auto& a : actions) a();
    }).detach();
}

// Bring-up failed. Every queued join gets its own meeting_failed — sdk_auth_failed
// alone does not clear the plugin's per-join "joining" state, so without this the
// Connect button stays disabled forever. The flags reset so a later connect retries.
static void FailPendingSdkActions(const std::string& reason)
{
    std::vector<std::function<void()>> actions;
    {
        std::lock_guard<std::mutex> lock(g_sdkStateMutex);
        g_sdkAuthenticated     = false;
        g_sdkBringupInProgress = false;
        actions.swap(g_pendingSdkActions);
    }
    for (size_t i = 0; i < actions.size(); ++i) {
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-1,\"message\":\"" +
                     feeds::JsonEscape(reason) + "\"}");
    }
}

// True when the action was queued and the caller must return; false when the SDK
// is already up and the caller proceeds inline.
static bool EnsureSdkUpThen(std::function<void()> action)
{
    {
        std::lock_guard<std::mutex> lock(g_sdkStateMutex);
        if (g_sdkAuthenticated) return false;

        g_pendingSdkActions.push_back(std::move(action));
        if (g_sdkBringupInProgress) return true;
        g_sdkBringupInProgress = true;
    }

    LogInfo("Mac engine: first connect — bringing up Zoom SDK (init + auth)");
    // init and auth must run on the main thread: the SDK is an AppKit client and
    // its delegate callbacks are dispatched on the main run loop.
    dispatch_async(dispatch_get_main_queue(), ^{ BringUpSdk(); });
    return true;
}

// ---------------------------------------------------------------------------
// Meeting-status delegate
// ---------------------------------------------------------------------------
@interface FeedsMeetingDelegate : NSObject <ZoomSDKMeetingServiceDelegate>
@end

@implementation FeedsMeetingDelegate

- (void)onMeetingStatusChange:(ZoomSDKMeetingStatus)state
                 meetingError:(ZoomSDKMeetingError)error
                    EndReason:(EndMeetingReason)reason
{
    LogInfo("Mac engine: meeting status " + std::to_string((int)state) +
            " (error " + std::to_string((int)error) + ", end reason " +
            std::to_string((int)reason) + ")");

    switch (state) {
    case ZoomSDKMeetingStatus_InMeeting: {
        unsigned long long meetingNumber = 0;
        if (g_meetingService) {
            NSString* number =
                [g_meetingService getMeetingProperty:MeetingPropertyCmd_MeetingNumber];
            if (number) meetingNumber = (unsigned long long)number.longLongValue;
        }
        SendToPlugin("{\"type\":\"meeting_joined\",\"meeting_number\":\"" +
                     std::to_string(meetingNumber) + "\"}");
        // Seed the dock immediately. onUserJoin keeps it current afterwards,
        // but the people already in the meeting generate no join event.
        SendParticipantList();
        // Arm the video module before asking for the privilege: the grant can
        // come back immediately when we are the host, and it drains a queue
        // this call is what creates.
        feeds_mac::VideoMeetingStarted();
        // The roster alone does not light the dock up: it stays greyed until
        // the raw-livestream privilege is reported. Ask for it now, exactly
        // where the Windows engine asks.
        RequestRawLiveStreamPrivilege();
        break;
    }

    case ZoomSDKMeetingStatus_Failed:
        feeds_mac::VideoMeetingEnded();
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":" +
                     std::to_string((int)error) + ",\"message\":\"" +
                     feeds::JsonEscape(MeetingErrorMessage(error)) + "\"}");
        break;

    case ZoomSDKMeetingStatus_Ended:
        // Renderers first: they hold SDK objects belonging to the meeting that
        // is going away, and the plugin's sources must be released before it is
        // told the meeting is over.
        feeds_mac::VideoMeetingEnded();
        SendToPlugin("{\"type\":\"meeting_left\"}");
        // The roster is gone with the meeting; say so rather than leaving the
        // dock showing people who are no longer reachable.
        SendToPlugin("{\"type\":\"participant_list_changed\",\"my_user_id\":0,"
                     "\"participants\":[]}");
        break;

    default:
        // Connecting / WaitingForHost / Reconnecting / audio-ready and the
        // webinar and breakout transitions: progress, not outcomes. The plugin
        // is already showing "connecting" and has nothing to do with these.
        break;
    }
}

@end

// ---------------------------------------------------------------------------
// Roster delegate
//
// ZoomSDKMeetingActionControllerDelegate declares no @optional section, so every
// one of its methods is @required and must be implemented or the class does not
// conform. Only the roster ones do anything in this increment; the rest are
// deliberate empty stubs, and several of them (active speaker, video status,
// chat) become real in later increments.
// ---------------------------------------------------------------------------
@interface FeedsActionDelegate : NSObject <ZoomSDKMeetingActionControllerDelegate>
@end

// ---------------------------------------------------------------------------
// Chat delegate
//
// Receives in-meeting chat from the SDK and forwards PUBLIC messages to the
// plugin as chat_message, the same field-for-field shape the Windows engine
// emits, because one plugin parses both.
//
// The privacy filter is the point of this class, not a detail of it. Anything
// that is not addressed to everyone — a DM, a panelist-only message, a waiting
// room message — is dropped HERE, at the engine boundary, and never crosses the
// IPC. The plugin cannot leak private chat to an overlay or a popup because it
// never receives any. Windows expresses the same rule as IsChatToAll(); the
// macOS SDK has no such helper, so the equivalent is an explicit test for
// To_All against the message type, which is the same set of messages.
//
// ZoomSDKMeetingChatControllerDelegate declares no @optional section, so every
// method is @required; the file-transfer ones are deliberate empty stubs.
// ---------------------------------------------------------------------------
@interface FeedsChatDelegate : NSObject <ZoomSDKMeetingChatControllerDelegate>
@end

@implementation FeedsActionDelegate

// Implementing a method the SDK has deprecated is not a call to it; the warning
// is about the declaration we are obliged to match, and we are obliged because
// it is still @required.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-implementations"
- (void)onUserInfoUpdate:(unsigned int)userID {}
#pragma clang diagnostic pop

// ── the roster ───────────────────────────────────────────────────────────────
- (void)onUserJoin:(NSArray*)array                      { SendParticipantList(); }
- (void)onUserNamesChanged:(NSArray<NSNumber*>*)userList { SendParticipantList(); }
- (void)onInMeetingUserAvatarPathUpdated:(unsigned int)userID { SendParticipantList(); }

- (void)onUserLeft:(NSArray*)array
{
    // Zoom fires no raw-data-off for a user who LEAVES, only for one whose
    // video stops while they stay, so a source bound to them would freeze on
    // their last frame. Tell the video module before the roster goes out.
    for (id entry in array) {
        if (![entry isKindOfClass:[NSNumber class]]) continue;
        feeds_mac::VideoOnUserLeft([(NSNumber*)entry unsignedIntValue]);
    }
    SendParticipantList();
}

// ── video ────────────────────────────────────────────────────────────────────
// A camera coming back on is how a source that was subscribed to a camera-off
// participant recovers; a camera going off changes who is displayable as the
// active speaker.
- (void)onVideoStatusChange:(ZoomSDKVideoStatus)videoStatus UserID:(unsigned int)userID
{
    feeds_mac::VideoOnUserVideoStatusChanged(
        userID, videoStatus == ZoomSDKVideoStatus_On);
}

// The active-speaker INPUT. Item 0 is the current talker; the video module
// stores it raw and derives the on-screen target from it, which is the same
// split the Windows engine makes for the same reason (a filter applied here
// would throw away a speaker who becomes displayable a moment later).
- (void)onUserActiveAudioChange:(NSArray*)useridArray
{
    for (id entry in useridArray) {
        if (![entry isKindOfClass:[NSNumber class]]) continue;
        feeds_mac::VideoOnActiveAudio([(NSNumber*)entry unsignedIntValue]);
        break;
    }
}

// ── @required, and nothing this engine acts on ───────────────────────────────
- (void)onUserAudioStatusChange:(NSArray*)userAudioStatusArray {}
- (void)onVirtualNameTagStatusChanged:(BOOL)bOn userID:(unsigned int)userID {}
- (void)onVirtualNameTagRosterInfoUpdated:(unsigned int)userID {}
- (void)onHostChange:(unsigned int)userID {}
- (void)onMeetingCoHostChanged:(unsigned int)userID isCoHost:(BOOL)isCoHost {}
- (void)onSpotlightVideoUserChange:(NSArray*_Nullable)spotlightedUserList {}
- (void)onLowOrRaiseHandStatusChange:(BOOL)raise UserID:(unsigned int)userID {}
- (void)onJoinMeetingResponse:(ZoomSDKJoinMeetingHelper*_Nullable)joinMeetingHelper {}
- (void)onMultiToSingleShareNeedConfirm:(ZoomSDKMultiToSingleShareConfirmHandler*_Nullable)confirmHandle {}
// Deliberately NOT the active-speaker input. These two report Zoom's own
// video-layout choices, which lag the talker and latch on a pinned or
// spotlighted user; the audio callback above is what tracks who is actually
// speaking, and is the same input the Windows engine uses.
- (void)onActiveVideoUserChanged:(unsigned int)userID {}
- (void)onActiveSpeakerVideoUserChanged:(unsigned int)userID {}
- (void)onHostAskUnmute {}
- (void)onHostAskStartVideo {}
- (void)onInvalidReclaimHostKey {}
- (void)onHostVideoOrderUpdated:(NSArray*)orderList {}
- (void)onLocalVideoOrderUpdated:(NSArray*)localOrderList {}
- (void)onFollowHostVideoOrderChanged:(BOOL)follow {}
- (void)onAllHandsLowered {}
- (void)onUserVideoQualityChanged:(ZoomSDKVideoQuality)quality userID:(unsigned int)userID {}
- (void)onChatMsgDeleteNotification:(NSString*)msgID
                  messageDeleteType:(ZoomSDKChatMessageDeleteType)deleteBy {}
- (void)onChatStatusChangedNotification:(ZoomSDKChatStatus*)chatStatus {}
- (void)onShareMeetingChatStatusChanged:(BOOL)isStart {}
- (void)onSuspendParticipantsActivities {}
- (void)onAllowParticipantsStartVideoNotification:(BOOL)allow {}
- (void)onAllowParticipantsRenameNotification:(BOOL)allow {}
- (void)onAllowParticipantsUnmuteSelfNotification:(BOOL)allow {}
- (void)onAllowParticipantsShareWhiteBoardNotification:(BOOL)allow {}
- (void)onMeetingLockStatus:(BOOL)isLock {}
- (void)onRequestLocalRecordingPrivilegeChanged:(ZoomSDKLocalRecordingRequestPrivilegeStatus)status {}
- (void)onAllowParticipantsRequestCloudRecording:(BOOL)allow {}
- (void)onAICompanionActiveChangeNotice:(BOOL)active {}
- (void)onParticipantProfilePictureStatusChange:(BOOL)hidden {}
- (void)onVideoAlphaChannelStatusChanged:(BOOL)isAlphaModeOn {}
- (void)onFocusModeStateChanged:(BOOL)on {}
- (void)onFocusModeShareTypeChanged:(ZoomSDKFocusModeShareType)shareType {}
- (void)onMeetingQAStatusChanged:(BOOL)isMeetingQAFeatureOn {}
- (void)onCameraControlRequestReceived:(unsigned int)userId
                           requestType:(ZoomSDKCameraControlRequestType)requestType
                         actionApprove:(nullable ZoomSDKError(^)(void))actionApprove
                         actionDecline:(nullable ZoomSDKError(^)(void))actionDecline {}
- (void)onCameraControlRequestResult:(unsigned int)userId
                          resultType:(ZoomSDKCameraControlRequestResult)resultType {}
- (void)onMuteOnEntryStatusChange:(BOOL)enable {}
- (void)onMeetingTopicChanged:(NSString*)topic {}
- (void)onBotAuthorizerRelationChanged:(unsigned int)authorizeUserID {}
- (void)onCreateCompanionRelation:(unsigned int)parentUserID
                      childUserID:(unsigned int)childUserID {}
- (void)onRemoveCompanionRelation:(unsigned int)childUserID {}
- (void)onGrantCoOwnerPrivilegeChanged:(BOOL)canGrantOther {}

@end

@implementation FeedsChatDelegate

- (void)onChatMessageNotification:(ZoomSDKChatInfo*)chatInfo
{
    if (!chatInfo) {
        LogWarn("Chat: notification with no message");
        return;
    }

    // THE PRIVACY FILTER. Everything that is not To_All stops here: DMs
    // (To_Individual), panelist traffic (To_All_Panelist,
    // To_Individual_Panelist) and waiting-room messages (To_WaitingRoomUsers).
    // The type is the only thing logged about a filtered message — never its
    // content or its sender — so the engine log cannot leak a DM either.
    const ZoomSDKChatMessageType type = [chatInfo getChatMessageType];
    if (type != ZoomSDKChatMessageType_To_All) {
        LogDebug("Chat: filtered non-public message (type=" +
                 std::to_string((int)type) + ")");
        return;
    }

    @autoreleasepool {
        NSString* messageId  = [chatInfo getMessageID];
        NSString* senderName = [chatInfo getSenderDisplayName];
        NSString* content    = [chatInfo getMsgContent];
        const unsigned int senderId = [chatInfo getSenderUserID];
        const time_t timestamp      = [chatInfo getTimeStamp];

        // The avatar travels as a path, not as bytes: the SDK writes profile
        // pictures into its own data directory and both processes can read it.
        // Same arrangement the roster uses. Empty when the sender has no
        // picture, and the plugin falls back to the bundled Feeds logo.
        NSString* avatar = nil;
        ZoomSDKMeetingActionController* action =
            g_meetingService ? [g_meetingService getMeetingActionController] : nil;
        if (action) {
            ZoomSDKUserInfo* info = [action getUserByUserID:senderId];
            if (info) avatar = [info getAvatarPath];
        }

        auto utf8 = [](NSString* s) -> std::string {
            return (s && s.UTF8String) ? std::string(s.UTF8String) : std::string();
        };

        const std::string contentStr = utf8(content);
        const std::string nameStr    = utf8(senderName);

        SendToPlugin(
            std::string("{\"type\":\"chat_message\",\"message_id\":\"") +
            feeds::JsonEscape(utf8(messageId)) +
            "\",\"sender_id\":" + std::to_string(senderId) +
            ",\"sender_name\":\"" + feeds::JsonEscape(nameStr) +
            "\",\"content\":\"" + feeds::JsonEscape(contentStr) +
            "\",\"avatar_path\":\"" + feeds::JsonEscape(utf8(avatar)) +
            "\",\"timestamp\":" + std::to_string((long long)timestamp) + "}");

        // Visibility only; the message itself already went over the IPC. The
        // preview is truncated so one long paste cannot flood the log.
        const std::string preview = contentStr.size() > 40
            ? contentStr.substr(0, 40) + "..."
            : contentStr;
        LogDebug("Chat: public message from " + nameStr + ": " + preview);
    }
}

- (void)onChatMessageEditNotification:(ZoomSDKChatInfo*)chatInfo
{
    // Not surfaced. The plugin's chat model has no edit path on either
    // platform, and an edited message arriving as a new one would duplicate it
    // on every overlay. Windows logs and drops this too.
    (void)chatInfo;
}

// File transfer is not a Feeds feature on either platform. Required by the
// protocol, which declares no @optional section.
- (void)onFileSendStart:(ZoomSDKFileSender*)sender          { (void)sender; }
- (void)onFileReceived:(ZoomSDKFileReceiver*)receiver       { (void)receiver; }
- (void)onFileTransferProgress:(ZoomSDKFileTransferInfo*)info { (void)info; }

@end

// ---------------------------------------------------------------------------
// Live-stream delegate — the raw-livestream PRIVILEGE, and only the privilege
//
// This is what unlocks the plugin's UI. The dock and the source-properties
// dialog both gate on it: until raw_livestream_granted arrives they render
// "Waiting for participants…" and offer no participant picker, however full the
// roster is, because a picker that cannot produce video would be a lie.
//
// The privilege is separable from USING it. Granting it moves no pixels: the
// call that turns raw-data delivery on is startRawLiveStreaming, which the
// granted path below hands to the video module, exactly where the Windows
// engine calls it.
//
// RECORDING IS NOT THE MECHANISM. Raw data is unlocked by the raw-LIVESTREAM
// privilege, the same one the Windows engine requests — not by local recording
// and not by the deprecated account-level raw-data license.
// ---------------------------------------------------------------------------

// The broadcast URL and name the host sees when they are asked to allow this.
// Same two strings the Windows engine sends, so one meeting host sees one
// consistent request whichever platform the user is on.
static NSString* const kRawBroadcastUrl  = @"https://letsdovideo.com/feeds-support/";
static NSString* const kRawBroadcastName = @"Feeds";

@interface FeedsLiveStreamDelegate : NSObject <ZoomSDKLiveStreamHelperDelegate>
@end

@implementation FeedsLiveStreamDelegate

- (void)onRawLiveStreamPrivilegeChanged:(BOOL)bHasPrivilege
{
    if (!bHasPrivilege) {
        // The host declined, or revoked a privilege we already had. Either way
        // frames stop, so the plugin swaps its "waiting" messaging for denied.
        LogWarn("Mac engine: raw livestream privilege DENIED");
        SendToPlugin("{\"type\":\"raw_livestream_denied\"}");
        return;
    }

    LogInfo("Mac engine: raw livestream privilege GRANTED");

    // Start raw livestreaming BEFORE telling the plugin. This is the call that
    // actually unlocks frame delivery, and raw_livestream_granted is what makes
    // the plugin start sending subscribes — announcing first would invite a
    // burst of requests at an SDK that is not yet delivering anything.
    feeds_mac::VideoStartRawLiveStream();

    // The signal the plugin's dock and properties dialog are waiting on.
    SendToPlugin("{\"type\":\"raw_livestream_granted\"}");
}

- (void)onRawLiveStreamPrivilegeRequestTimeout
{
    // The host never answered the prompt. Distinct from a denial: nothing was
    // refused, so the plugin offers a retry rather than an explanation.
    LogWarn("Mac engine: raw livestream privilege request TIMED OUT");
    SendToPlugin("{\"type\":\"raw_livestream_timeout\"}");
}

// ── @required, and nothing to do with this increment ─────────────────────────
- (void)onLiveStreamStatusChange:(LiveStreamStatus)status {}
- (void)onUserRawLiveStreamPrivilegeChanged:(unsigned int)userID
                               hasPrivilege:(BOOL)bHasPrivilege {}
// Fires on the HOST's side when someone else asks for the privilege. Feeds
// never runs as the approver, so there is nothing to answer here.
- (void)onRawLiveStreamPrivilegeRequested:
    (ZoomSDKRequestRawLiveStreamPrivilegeHandler*_Nullable)handler {}

// Our own user appearing in this list is the SDK's only reliable "the raw-data
// renderer subsystem is up" signal. createRenderer before it returns a
// transient not-ready error, so the video module holds subscribe requests
// queued until this arrives.
- (void)onUserRawLiveStreamingStatusChanged:
    (NSArray<ZoomSDKRawLiveStreamInfo*>*_Nullable)liveStreamList
{
    for (ZoomSDKRawLiveStreamInfo* info in liveStreamList) {
        if (![info isKindOfClass:[ZoomSDKRawLiveStreamInfo class]]) continue;
        feeds_mac::VideoNotifyRawRenderReady(info.userID);
    }
}
- (void)onLiveStreamReminderStatusChanged:(BOOL)enable {}
- (void)onLiveStreamReminderStatusChangeFailed {}
- (void)onUserThresholdReachedForLiveStream:(int)percent {}

@end

// ---------------------------------------------------------------------------
// Auth delegate
// ---------------------------------------------------------------------------
@class FeedsAuthDelegate;
@class FeedsChatDelegate;

// All three delegate properties the SDK exposes are `assign` (unowned, and
// under ARC that means __unsafe_unretained): handing one an object nothing else
// holds would leave a dangling pointer the first time a callback fires, with no
// weak zeroing to catch it. These file-scope statics are strong references that
// live for the process, which is the simplest lifetime that outlives the
// services — the engine owns exactly one of each and never replaces it.
static FeedsAuthDelegate*       g_authDelegate       = nil;
static FeedsMeetingDelegate*    g_meetingDelegate    = nil;
static FeedsActionDelegate*     g_actionDelegate     = nil;
static FeedsChatDelegate*       g_chatDelegate       = nil;
static FeedsLiveStreamDelegate* g_liveStreamDelegate = nil;

@interface FeedsAuthDelegate : NSObject <ZoomSDKAuthDelegate>
@end

@implementation FeedsAuthDelegate

- (void)onZoomSDKAuthReturn:(ZoomSDKAuthError)returnValue
{
    if (returnValue != ZoomSDKAuthError_Success) {
        LogError(std::string("Mac engine: SDK authentication FAILED: ") +
                 AuthErrorName(returnValue) + " (code " +
                 std::to_string((int)returnValue) + ")");
        SendToPlugin("{\"type\":\"sdk_auth_failed\",\"code\":" +
                     std::to_string((int)returnValue) + "}");
        FailPendingSdkActions("Zoom SDK authentication failed.");
        return;
    }

    LogInfo("Mac engine: SDK authentication SUCCEEDED");

    // The meeting service exists only after a successful auth, and the delegates
    // have to be attached before the join — a status change that arrives with no
    // delegate is simply lost, and the join would hang with nothing reported.
    g_meetingService = [[ZoomSDK sharedSDK] getMeetingService];
    if (!g_meetingService) {
        LogError("Mac engine: getMeetingService returned nil after a successful "
                 "auth; joins cannot proceed");
        FailPendingSdkActions("Zoom SDK is not ready. Please try logging in again.");
        return;
    }

    g_meetingService.delegate = g_meetingDelegate;

    ZoomSDKMeetingActionController* action =
        [g_meetingService getMeetingActionController];
    if (action) {
        action.delegate = g_actionDelegate;
    } else {
        // Not fatal: the meeting still joins, the dock just stays empty. Say so
        // now rather than leaving an empty participant list to be read as "the
        // meeting has nobody in it".
        LogWarn("Mac engine: meeting action controller unavailable; the "
                "participant list will not update");
    }

    ZoomSDKMeetingChatController* chat =
        [g_meetingService getMeetingChatController];
    if (chat) {
        chat.delegate = g_chatDelegate;
    } else {
        // Fail soft, exactly as on Windows: the meeting is still perfectly
        // usable, the chat dock simply stays empty. Worth one line so an empty
        // dock is not mistaken for a meeting where nobody is talking.
        LogWarn("Mac engine: meeting chat controller unavailable; in-meeting "
                "chat will not be received or sent");
    }

    DrainPendingSdkActions();
}

// @required by the protocol.
- (void)onZoomAuthIdentityExpired
{
    LogWarn("Mac engine: Zoom auth identity expired");
}

@end

// ---------------------------------------------------------------------------
// Participant list (main queue only — every call below is an SDK getter)
//
// Same message shape as the Windows engine's SendParticipantList, field for
// field, because the plugin's dock parses one format for both platforms.
// ---------------------------------------------------------------------------
static void SendParticipantList()
{
    ZoomSDKMeetingActionController* action =
        g_meetingService ? [g_meetingService getMeetingActionController] : nil;
    if (!action) {
        SendToPlugin("{\"type\":\"participant_list_changed\",\"my_user_id\":0,"
                     "\"participants\":[]}");
        return;
    }

    @autoreleasepool {
        unsigned int     myUserId = 0;
        ZoomSDKUserInfo* mySelf   = [action getMyself];
        if (mySelf) myUserId = [mySelf getUserID];

        std::string msg = "{\"type\":\"participant_list_changed\",\"my_user_id\":" +
                          std::to_string(myUserId) + ",\"participants\":[";

        NSArray* users = [action getParticipantsList];
        bool     first = true;
        for (id entry in users) {
            if (![entry isKindOfClass:[NSNumber class]]) continue;
            const unsigned int uid = [(NSNumber*)entry unsignedIntValue];
            ZoomSDKUserInfo* info  = [action getUserByUserID:uid];
            if (!info) continue;

            NSString* name = [info getUserName];
            // The avatar is a path, not bytes: the SDK writes profile pictures
            // into its own data directory, which both processes can read, so the
            // path travels and the image does not. Empty when the user has no
            // picture; the plugin falls back to the bundled Feeds logo.
            NSString* avatar = [info getAvatarPath];

            const ZoomSDKAudioStatus audio = [info getAudioStatus];
            const bool muted = (audio == ZoomSDKAudioStatus_Muted ||
                                audio == ZoomSDKAudioStatus_MutedByHost ||
                                audio == ZoomSDKAudioStatus_MutedAllByHost);

            if (!first) msg += ",";
            msg += "{\"id\":" + std::to_string(uid) +
                   ",\"name\":\"" +
                   feeds::JsonEscape(name && name.UTF8String ? name.UTF8String : "") +
                   "\",\"avatar_path\":\"" +
                   feeds::JsonEscape(avatar && avatar.UTF8String ? avatar.UTF8String : "") +
                   "\",\"muted\":" + (muted ? "1" : "0") + "}";
            first = false;
        }

        msg += "]}";
        SendToPlugin(msg);
    }
}

// ---------------------------------------------------------------------------
// Ask for the raw-livestream privilege (main queue only)
//
// Called once, from the InMeeting status change — the same point the Windows
// engine asks. Two paths, and the difference is who we are in the meeting:
//
//   host / already privileged   canStartRawLiveStream reports success and the
//                               SDK fires onRawLiveStreamPrivilegeChanged on
//                               its own; we just wait for it.
//   everyone else               request it, which puts a prompt in front of the
//                               HOST, and tell the plugin we are waiting so it
//                               can say "waiting for host" instead of showing a
//                               picker that does not work yet.
//
// Either way we wait for the real callback rather than assuming the answer.
// Windows learned that the hard way: firing the granted path manually let the
// plugin subscribe before the SDK's own state was ready, and createRenderer
// then failed with a permission error.
// ---------------------------------------------------------------------------
static void RequestRawLiveStreamPrivilege()
{
    if (!g_meetingService) return;

    ZoomSDKLiveStreamHelper* helper = [g_meetingService getLiveStreamHelper];
    if (!helper) {
        // Nothing is sent to the plugin here, deliberately, and the same goes
        // for the rejected-request path below. Neither existing message fits:
        // raw_livestream_denied renders as "Host denied use of Feeds", which
        // would blame a host who was never asked. The engine log carries the
        // real reason, which is where a support bundle looks — and this matches
        // what the Windows engine does with the same failure.
        LogError("Mac engine: live-stream helper unavailable; the raw-livestream "
                 "privilege cannot be requested, so participant video will not "
                 "become available in this meeting");
        return;
    }

    if (!g_liveStreamDelegate)
        g_liveStreamDelegate = [[FeedsLiveStreamDelegate alloc] init];
    helper.delegate = g_liveStreamDelegate;

    if ([helper canStartRawLiveStream] == ZoomSDKError_Success) {
        LogInfo("Mac engine: raw livestream privilege already available; "
                "waiting for the SDK to confirm it");
        return;
    }

    LogInfo("Mac engine: requesting raw livestream privilege from the host");
    const ZoomSDKError err = [helper requestRawLiveStreaming:kRawBroadcastUrl
                                               broadcastName:kRawBroadcastName];
    if (err != ZoomSDKError_Success) {
        // A synchronous rejection means no callback will follow. Reported to
        // the log only, for the reason given above: the host declined nothing.
        LogError("Mac engine: requestRawLiveStreaming was rejected (code " +
                 std::to_string((int)err) + "); no privilege callback will arrive");
        return;
    }

    // The host now has a prompt in front of them. This is what turns the
    // plugin's messaging into "waiting for host".
    SendToPlugin("{\"type\":\"raw_livestream_pending\"}");
}

// ---------------------------------------------------------------------------
// SDK bring-up (main queue only)
// ---------------------------------------------------------------------------

// See the header note: the SDK finds its runtime bundles through the main
// bundle's Frameworks directory, and their absence shows up only as a
// synchronous auth failure with no callback. Check it first so the cause is
// stated rather than inferred.
static bool PreflightSdkRuntime(std::string& frameworksDir)
{
    NSString* frameworks = [[NSBundle mainBundle] privateFrameworksPath];
    if (!frameworks) {
        frameworksDir = "(no main bundle — the engine is not running from a .app)";
        return false;
    }
    frameworksDir = frameworks.UTF8String ? frameworks.UTF8String : "";
    NSString* sdk = [frameworks stringByAppendingPathComponent:@"ZoomSDK.framework"];
    return [[NSFileManager defaultManager] fileExistsAtPath:sdk];
}

static void BringUpSdk()
{
    std::string frameworksDir;
    if (!PreflightSdkRuntime(frameworksDir)) {
        LogError("Mac engine: ZoomSDK.framework is not in the engine app's "
                 "Frameworks directory, so the Zoom SDK cannot authenticate. "
                 "Looked in: " + frameworksDir);
        SendToPlugin("{\"type\":\"sdk_auth_failed\",\"code\":-1}");
        FailPendingSdkActions("Could not initialize the Zoom SDK.");
        return;
    }
    LogInfo("Mac engine: SDK runtime found in " + frameworksDir);

    ZoomSDKInitParams* params = [[ZoomSDKInitParams alloc] init];
    // Matches the Windows engine, which leaves InitParam's customized-UI flag at
    // its default and so runs with the SDK's own UI available.
    params.needCustomizedUI = NO;
    params.enableLog        = YES;
    params.zoomDomain       = @"https://zoom.us";

    // Raw-data memory mode is configured on the ZoomSDK SINGLETON here, not on
    // the init params — unlike Windows, where it lives inside InitParam. It has
    // to be set before initSDKWithParams or it is ignored. Heap mode matches
    // what the Windows engine asks for, so the frame buffers handed to the
    // shared-memory writer have the same lifetime rules on both platforms:
    // valid for the duration of the callback and freed after it unless
    // explicitly retained.
    [ZoomSDK sharedSDK].videoRawDataMode = ZoomSDKRawDataMemoryMode_Heap;
    [ZoomSDK sharedSDK].shareRawDataMode = ZoomSDKRawDataMemoryMode_Heap;

    LogInfo("Mac engine: calling initSDKWithParams (this blocks for several "
            "seconds by design)");
    const ZoomSDKError initErr = [[ZoomSDK sharedSDK] initSDKWithParams:params];
    if (initErr != ZoomSDKError_Success) {
        LogError("Mac engine: initSDKWithParams FAILED (code " +
                 std::to_string((int)initErr) + ")");
        SendToPlugin("{\"type\":\"sdk_auth_failed\",\"code\":-1}");
        FailPendingSdkActions("Could not initialize the Zoom SDK.");
        return;
    }
    LogInfo("Mac engine: initSDKWithParams SUCCEEDED");

    ZoomSDKAuthService* authService = [[ZoomSDK sharedSDK] getAuthService];
    if (!authService) {
        LogError("Mac engine: getAuthService returned nil; cannot authenticate");
        SendToPlugin("{\"type\":\"sdk_auth_failed\",\"code\":-1}");
        FailPendingSdkActions("Could not start Zoom SDK authentication.");
        return;
    }

    if (!g_authDelegate)    g_authDelegate    = [[FeedsAuthDelegate alloc] init];
    if (!g_meetingDelegate) g_meetingDelegate = [[FeedsMeetingDelegate alloc] init];
    if (!g_actionDelegate)  g_actionDelegate  = [[FeedsActionDelegate alloc] init];
    if (!g_chatDelegate)   g_chatDelegate   = [[FeedsChatDelegate alloc] init];
    authService.delegate = g_authDelegate;

    // The same credential the Windows engine authenticates with: the public app
    // key (our Zoom OAuth client id), baked in at build time. NOT a JWT — Feeds
    // has no client secret to sign one with, which is the whole point of the
    // public-app-key flow.
    const std::string clientId = FEEDS_ZOOM_CLIENT_ID;
    if (clientId.empty()) {
        LogError("Mac engine: no public app key was compiled in "
                 "(FEEDS_ZOOM_CLIENT_ID); cannot authenticate the SDK");
        SendToPlugin("{\"type\":\"sdk_auth_failed\",\"code\":-1}");
        FailPendingSdkActions("Could not start Zoom SDK authentication.");
        return;
    }

    ZoomSDKAuthContext* ctx = [[ZoomSDKAuthContext alloc] init];
    ctx.publicAppKey = [NSString stringWithUTF8String:clientId.c_str()];
    ctx.jwtToken     = nil;

    const ZoomSDKError authErr = [authService sdkAuth:ctx];
    if (authErr != ZoomSDKError_Success) {
        // A synchronous rejection means onZoomSDKAuthReturn will never fire, so
        // report here or nothing would ever be said about it.
        LogError("Mac engine: sdkAuth was rejected synchronously (code " +
                 std::to_string((int)authErr) +
                 "); no auth callback will arrive");
        SendToPlugin("{\"type\":\"sdk_auth_failed\",\"code\":-1}");
        FailPendingSdkActions("Could not start Zoom SDK authentication.");
        return;
    }
    LogInfo("Mac engine: sdkAuth accepted; waiting for the auth callback");
}

// ---------------------------------------------------------------------------
// join_meeting
//
// Runs on a BACKGROUND thread: it begins with a blocking ZAK fetch. Only the
// joinMeeting: call itself is hopped back to the main queue.
// ---------------------------------------------------------------------------

// The input box accepts what a user can paste. Same rules as the Windows engine:
//   zoom.us/my/<vanity>  → vanity ID
//   zoom.us/j/<number>   → meeting number
//   zoom.us/w/<number>   → webinar number
//   anything else        → strip non-digits and treat as a meeting number
// Returns false when nothing usable could be read out, with `error` set to what
// the user should be told.
static bool ParseMeetingInput(const std::string& input,
                              long long&         outNumber,
                              std::string&       outVanity,
                              std::string&       outError)
{
    outNumber = 0;
    outVanity.clear();

    auto trimUrlTail = [](std::string s) {
        const size_t end = s.find_first_of("?& \t\r\n/");
        return end == std::string::npos ? s : s.substr(0, end);
    };
    auto digitsOf = [](const std::string& s) {
        std::string out;
        for (char c : s) if (c >= '0' && c <= '9') out += c;
        return out;
    };

    size_t pos;
    if ((pos = input.find("zoom.us/my/")) != std::string::npos) {
        outVanity = trimUrlTail(input.substr(pos + 11));
        if (outVanity.empty()) {
            outError = "Could not parse a personal meeting link.";
            return false;
        }
        LogInfo("Mac engine: join input parsed as a vanity ID URL");
        return true;
    }

    const char* label = nullptr;
    size_t      skip  = 0;
    if ((pos = input.find("zoom.us/j/")) != std::string::npos) {
        label = "meeting number"; skip = 10;
    } else if ((pos = input.find("zoom.us/w/")) != std::string::npos) {
        label = "webinar number"; skip = 10;
    }

    if (label) {
        const std::string digits = digitsOf(trimUrlTail(input.substr(pos + skip)));
        if (digits.empty()) {
            outError = std::string("Could not parse ") + label + " from link.";
            return false;
        }
        outNumber = std::strtoll(digits.c_str(), nullptr, 10);
        LogInfo("Mac engine: join input parsed as a Zoom meeting URL");
        return true;
    }

    const std::string digits = digitsOf(input);
    if (digits.empty()) {
        outError = "Could not parse a valid meeting number.";
        return false;
    }
    outNumber = std::strtoll(digits.c_str(), nullptr, 10);
    LogInfo("Mac engine: join input parsed as a raw meeting number");
    return true;
}

static void HandleJoinMeeting(const std::string& json)
{
    // Lazy SDK bring-up: the first connect queues this call and starts init+auth,
    // and the queued copy re-enters here once auth lands, this time falling
    // straight through.
    if (EnsureSdkUpThen([json]() { HandleJoinMeeting(json); }))
        return;

    if (!g_meetingService) {
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-1,\"message\":\"Zoom SDK "
                     "is not ready. Please try logging in again.\"}");
        return;
    }

    const std::string input        = feeds::ExtractJsonString(json, "input");
    const std::string password     = feeds::ExtractJsonString(json, "password");
    const std::string customName   = feeds::ExtractJsonString(json, "display_name");
    const std::string webinarToken = feeds::ExtractJsonString(json, "webinar_token");

    if (input.empty()) {
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-2,\"message\":\"No "
                     "meeting number or link was provided.\"}");
        return;
    }

    // Zoom Events Hub sessions cannot be joined by pasting their link: the
    // event-join token in the URL path has nowhere to go in the join elements,
    // so the digits would be scraped out into a meaningless meeting number and
    // fail as "Meeting not found". Reject it while we can still say why.
    std::string inputLower = input;
    for (char& c : inputLower) if (c >= 'A' && c <= 'Z') c += 32;
    if (inputLower.find("events.zoom.us") != std::string::npos) {
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-4,\"message\":\"This "
                     "looks like a Zoom Events link, which can't be joined by "
                     "pasting the URL.\"}");
        return;
    }

    long long   meetingNumber = 0;
    std::string vanityId;
    std::string parseError;
    if (!ParseMeetingInput(input, meetingNumber, vanityId, parseError)) {
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-2,\"message\":\"" +
                     feeds::JsonEscape(parseError) + "\"}");
        return;
    }

    // Blocking REST, which is why this handler is not on the main thread. A ZAK
    // is short-lived, so it is fetched per join rather than cached.
    const std::string zak         = feeds_mac::FetchZak();
    const std::string displayName = feeds_mac::UserDisplayName();
    if (zak.empty() || displayName.empty()) {
        LogWarn("Mac engine: could not retrieve the ZAK or the account display name");
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-3,\"message\":\"Could not "
                     "retrieve your Zoom account details. Please log out and log in "
                     "again.\"}");
        return;
    }

    // A custom display name overrides the account name for this session only.
    // The account-name check above still stands either way: an empty account
    // name means the auth state is wrong, whatever name the user typed.
    const std::string& effectiveName = customName.empty() ? displayName : customName;

    ZoomSDKJoinMeetingElements* elements =
        [[ZoomSDKJoinMeetingElements alloc] init];
    elements.userType      = ZoomSDKUserType_WithoutLogin;
    elements.zak           = [NSString stringWithUTF8String:zak.c_str()];
    elements.displayName   = [NSString stringWithUTF8String:effectiveName.c_str()];
    elements.meetingNumber = meetingNumber;
    elements.isNoVideo     = YES;
    elements.isNoAudio     = YES;
    if (!vanityId.empty())
        elements.vanityID = [NSString stringWithUTF8String:vanityId.c_str()];
    if (!password.empty())
        elements.password = [NSString stringWithUTF8String:password.c_str()];
    if (!webinarToken.empty())
        elements.webinarToken = [NSString stringWithUTF8String:webinarToken.c_str()];

    LogInfo("Mac engine: joining (webinar token " +
            std::string(webinarToken.empty() ? "absent" : "present") + ")");

    dispatch_async(dispatch_get_main_queue(), ^{
        if (!g_meetingService) return;
        const ZoomSDKError err = [g_meetingService joinMeeting:elements];
        if (err != ZoomSDKError_Success) {
            // A synchronous rejection means no status change will follow, so
            // this is the only chance to report it.
            LogError("Mac engine: joinMeeting was rejected immediately (code " +
                     std::to_string((int)err) + ")");
            SendToPlugin("{\"type\":\"meeting_failed\",\"code\":" +
                         std::to_string((int)err) +
                         ",\"message\":\"Could not start meeting join. SDK error: " +
                         std::to_string((int)err) + "\"}");
            return;
        }
        LogInfo("Mac engine: joinMeeting accepted; waiting for status events");
    });
}

// ---------------------------------------------------------------------------
// leave_meeting / logout (main queue — SDK calls)
// ---------------------------------------------------------------------------
static void HandleLeaveMeeting()
{
    if (!g_meetingService) {
        SendToPlugin("{\"type\":\"meeting_left\"}");
        return;
    }

    const ZoomSDKMeetingStatus status = [g_meetingService getMeetingStatus];
    if (status == ZoomSDKMeetingStatus_Idle ||
        status == ZoomSDKMeetingStatus_Ended ||
        status == ZoomSDKMeetingStatus_Disconnecting) {
        // Already out. Send meeting_left anyway so a plugin whose UI drifted out
        // of sync still resets.
        SendToPlugin("{\"type\":\"meeting_left\"}");
        return;
    }

    [g_meetingService leaveMeetingWithCmd:LeaveMeetingCmd_Leave];
    // meeting_left follows from onMeetingStatusChange when the SDK reports Ended.
}

static void HandleLogout()
{
    // Renderers and regions belong to the meeting, and the meeting is about to
    // end without necessarily producing a status change we would see.
    feeds_mac::VideoMeetingEnded();

    if (g_meetingService) {
        const ZoomSDKMeetingStatus status = [g_meetingService getMeetingStatus];
        if (status != ZoomSDKMeetingStatus_Idle &&
            status != ZoomSDKMeetingStatus_Ended)
            [g_meetingService leaveMeetingWithCmd:LeaveMeetingCmd_Leave];
    }

    ZoomSDKAuthService* authService = [[ZoomSDK sharedSDK] getAuthService];
    if (authService) [authService logout];

    feeds_mac::ClearStoredCredentials();

    // The SDK's prior sdkAuth no longer counts as "ready to connect" once it has
    // logged out, so reset the lazy bring-up: the next connect must re-run
    // init+auth rather than try to join on a logged-out SDK.
    {
        std::lock_guard<std::mutex> lock(g_sdkStateMutex);
        g_sdkAuthenticated     = false;
        g_sdkBringupInProgress = false;
        g_pendingSdkActions.clear();
    }

    SendToPlugin("{\"type\":\"logout_complete\"}");
}

// ---------------------------------------------------------------------------
// Send one public chat message.
//
// Replies with chat_send_result either way. The plugin shows the error string
// verbatim, so the failure text here is written to be read by a user rather
// than by us.
//
// Threading is the one place this is simpler than Windows. There, the chat send
// has to be marshalled to the thread that owns the SDK window: sending from the
// pipe-reader thread returns success, echoes locally, and never reaches anyone
// else. Here HandleCommand already runs on the main queue, which is the queue
// the macOS SDK dispatches on, so the send happens inline and needs no hop.
// ---------------------------------------------------------------------------
static void ReplyChatSendResult(bool ok, const std::string& error)
{
    if (ok) {
        SendToPlugin("{\"type\":\"chat_send_result\",\"success\":true}");
    } else {
        SendToPlugin("{\"type\":\"chat_send_result\",\"success\":false,\"error\":\"" +
                     feeds::JsonEscape(error) + "\"}");
    }
}

static void HandleSendChatMessage(const std::string& line)
{
    const std::string content = feeds::ExtractJsonString(line, "content");
    if (content.empty()) {
        ReplyChatSendResult(false, "Nothing to send");
        return;
    }

    if (!g_meetingService ||
        [g_meetingService getMeetingStatus] != ZoomSDKMeetingStatus_InMeeting) {
        LogWarn("Chat: send requested while not in a meeting");
        ReplyChatSendResult(false, "Not in a meeting");
        return;
    }

    @autoreleasepool {
        ZoomSDKMeetingChatController* chat =
            [g_meetingService getMeetingChatController];
        if (!chat) {
            LogWarn("Chat: send requested but the chat controller is unavailable");
            ReplyChatSendResult(false, "Chat is unavailable in this meeting");
            return;
        }

        ZoomSDKChatMsgInfoBuilder* builder =
            [[ZoomSDKChatMsgInfoBuilder alloc] init];
        if (!builder) {
            LogWarn("Chat: could not create a message builder");
            ReplyChatSendResult(false, "Chat is unavailable in this meeting");
            return;
        }

        NSString* text = [NSString stringWithUTF8String:content.c_str()];
        if (!text) {
            ReplyChatSendResult(false, "Message could not be encoded");
            return;
        }

        // Receiver 0 AND To_All. Both are required: the type says what kind of
        // message this is, and a zero receiver is what the SDK reads as
        // "everyone". Same pair the Windows engine sets.
        [builder setContent:text];
        [builder setReceiver:0];
        [builder setMessageType:ZoomSDKChatMessageType_To_All];

        ZoomSDKChatInfo* message = [builder build];
        if (!message) {
            LogWarn("Chat: builder produced no message");
            ReplyChatSendResult(false, "Failed to build chat message");
            return;
        }

        const ZoomSDKError err = [chat sendChatMsgTo:message];
        if (err != ZoomSDKError_Success) {
            LogWarn("Chat: sendChatMsgTo failed (code " +
                    std::to_string((int)err) + ")");
            ReplyChatSendResult(
                false, "Failed to send (SDK error " + std::to_string((int)err) + ")");
            return;
        }

        LogDebug("Chat: message sent");
        ReplyChatSendResult(true, "");
    }
}

// ---------------------------------------------------------------------------
// Command handling (main queue — see the reader thread in main)
// ---------------------------------------------------------------------------
static void HandleCommand(const std::string& line)
{
    const std::string type = feeds::ExtractJsonString(line, "type");
    if (type.empty()) return;

    if (type == "shutdown") {
        LogInfo("Mac engine: shutdown requested");
        g_running.store(false);
        [[ZoomSDK sharedSDK] unInitSDK];
        ::exit(0);
        return;
    }

    if (type == "login_start") {
        if (!feeds_mac::StartLoginFlow())
            LogWarn("Mac engine: a login is already in progress");
        return;
    }
    if (type == "login_cancel") { feeds_mac::CancelLoginFlow(); return; }
    if (type == "logout")       { HandleLogout();               return; }

    if (type == "join_meeting") {
        // Off the main thread: the join blocks on a ZAK fetch before it can
        // build its join elements.
        std::thread([line]() { HandleJoinMeeting(line); }).detach();
        return;
    }
    if (type == "leave_meeting")   { HandleLeaveMeeting();  return; }
    if (type == "get_participants") { SendParticipantList(); return; }
    if (type == "send_chat_message") { HandleSendChatMessage(line); return; }

    // Video. A subscribe re-points an existing renderer where it can; a
    // recreate always rebuilds one, because a kept renderer loses SDK delivery
    // across a participant's drop and rejoin.
    if (type == "participant_source_subscribe") {
        feeds_mac::VideoHandleSubscribe(line, /*recreate=*/false);
        return;
    }
    if (type == "participant_source_recreate") {
        feeds_mac::VideoHandleSubscribe(line, /*recreate=*/true);
        return;
    }
    if (type == "participant_source_unsubscribe") {
        feeds_mac::VideoHandleUnsubscribe(line);
        return;
    }

    // Everything else belongs to a later increment. Say so plainly rather than
    // dropping it silently, so a premature message is visible in the log.
    LogWarn("Mac engine: ignoring '" + type +
            "' — not implemented in this build");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int /*argc*/, const char* /*argv*/[])
{
    // First, before any write: a plugin that dies must not kill this process
    // through SIGPIPE, and neither may the reverse (see feeds-ipc-posix.h).
    feeds_ipc::IgnoreSigPipe();
    feeds_ipc::SuppressSocketSigPipe(feeds_ipc::kEngineIpcFd);

    // Announce ourselves exactly as the Windows engine does: same message, same
    // fields, so the plugin's existing handler logs it unchanged.
    SendToPlugin(std::string("{\"type\":\"engine_ready\",\"version\":\"") +
                 feeds_shared::VERSION + "\",\"pid\":" +
                 std::to_string((long)getpid()) + "}");
    LogInfo("Mac engine: IPC connected");

    // The reader owns the socket; SDK work is hopped to the main queue.
    std::thread reader([]() {
        feeds_ipc::LineReader lineReader(feeds_ipc::kEngineIpcFd);
        std::string line;
        while (g_running.load() && lineReader.ReadLine(line)) {
            if (line.empty()) continue;
            const std::string copy = line;
            dispatch_async(dispatch_get_main_queue(), ^{ HandleCommand(copy); });
        }
        // EOF: the plugin closed the socket or OBS died. Nothing can be
        // reported any more, so leave from the main thread, which owns the run
        // loop. This is what stops the engine outliving OBS — macOS has no Job
        // Object to do it for us.
        dispatch_async(dispatch_get_main_queue(), ^{
            g_running.store(false);
            [[ZoomSDK sharedSDK] unInitSDK];
            ::exit(0);
        });
    });
    reader.detach();

    // Restore the logged-in appearance over REST if a token is stored — but do
    // NOT initialize the Zoom SDK here. An idle, logged-in-but-not-connected
    // Feeds runs no SDK and holds no Zoom session, so it adds no steady load;
    // the SDK comes up on the first connect, through EnsureSdkUpThen. Same
    // startup order as the Windows engine.
    feeds_mac::RestoreSessionFromStoredToken();

    // Hand the main thread to Cocoa: the SDK is an AppKit client and needs a
    // real NSApplication, not a bare NSRunLoop, for its delegates to fire.
    //
    // Accessory, not Regular: Feeds drives the SDK headlessly and shows no Zoom
    // UI, and Accessory keeps the engine out of the Dock and the app switcher.
    // Joining with isNoVideo/isNoAudio and no meeting window keeps that true
    // through this increment.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp run];
    return 0;
}
