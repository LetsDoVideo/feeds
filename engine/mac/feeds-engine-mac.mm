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
// ── INCREMENT 1b — WHAT THIS DOES AND DOES NOT DO ────────────────────────────
// Does: everything up to a joined meeting with a populated roster. Zoom login
// over OAuth (feeds-mac-login.mm), session restore at startup, lazy SDK
// bring-up on the first connect, join by meeting number or link, and the
// participant list — sent on join and kept current as people come and go.
//
// Does NOT: deliver any video. No raw-livestream privilege request, no renderer
// subscription, no shared-memory frame transport; those are increment 2. A
// source placed in OBS on macOS will show nothing yet, by design.
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

static void BringUpSdk();          // main queue only
static void SendParticipantList(); // main queue only

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
        break;
    }

    case ZoomSDKMeetingStatus_Failed:
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":" +
                     std::to_string((int)error) + ",\"message\":\"" +
                     feeds::JsonEscape(MeetingErrorMessage(error)) + "\"}");
        break;

    case ZoomSDKMeetingStatus_Ended:
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
- (void)onUserLeft:(NSArray*)array                      { SendParticipantList(); }
- (void)onUserNamesChanged:(NSArray<NSNumber*>*)userList { SendParticipantList(); }
- (void)onInMeetingUserAvatarPathUpdated:(unsigned int)userID { SendParticipantList(); }

// ── @required, and nothing to do with this increment ─────────────────────────
- (void)onUserAudioStatusChange:(NSArray*)userAudioStatusArray {}
- (void)onVirtualNameTagStatusChanged:(BOOL)bOn userID:(unsigned int)userID {}
- (void)onVirtualNameTagRosterInfoUpdated:(unsigned int)userID {}
- (void)onHostChange:(unsigned int)userID {}
- (void)onMeetingCoHostChanged:(unsigned int)userID isCoHost:(BOOL)isCoHost {}
- (void)onSpotlightVideoUserChange:(NSArray*_Nullable)spotlightedUserList {}
- (void)onVideoStatusChange:(ZoomSDKVideoStatus)videoStatus UserID:(unsigned int)userID {}
- (void)onLowOrRaiseHandStatusChange:(BOOL)raise UserID:(unsigned int)userID {}
- (void)onJoinMeetingResponse:(ZoomSDKJoinMeetingHelper*_Nullable)joinMeetingHelper {}
- (void)onMultiToSingleShareNeedConfirm:(ZoomSDKMultiToSingleShareConfirmHandler*_Nullable)confirmHandle {}
- (void)onActiveVideoUserChanged:(unsigned int)userID {}
- (void)onActiveSpeakerVideoUserChanged:(unsigned int)userID {}
- (void)onHostAskUnmute {}
- (void)onHostAskStartVideo {}
- (void)onUserActiveAudioChange:(NSArray*)useridArray {}
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

// ---------------------------------------------------------------------------
// Auth delegate
// ---------------------------------------------------------------------------
@class FeedsAuthDelegate;

// All three delegate properties the SDK exposes are `assign` (unowned, and
// under ARC that means __unsafe_unretained): handing one an object nothing else
// holds would leave a dangling pointer the first time a callback fires, with no
// weak zeroing to catch it. These file-scope statics are strong references that
// live for the process, which is the simplest lifetime that outlives the
// services — the engine owns exactly one of each and never replaces it.
static FeedsAuthDelegate*    g_authDelegate    = nil;
static FeedsMeetingDelegate* g_meetingDelegate = nil;
static FeedsActionDelegate*  g_actionDelegate  = nil;

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

    // Everything else belongs to a later increment. Say so plainly rather than
    // dropping it silently, so a premature message is visible in the log.
    LogWarn("Mac engine: ignoring '" + type +
            "' — not implemented in this build (participant video is a later "
            "increment)");
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
