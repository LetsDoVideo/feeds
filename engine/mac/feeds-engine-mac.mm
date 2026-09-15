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
// ── INCREMENT 1a — WHAT THIS DOES AND DOES NOT DO ────────────────────────────
// Does: come up as a child process of the plugin, speak the IPC protocol over
// the inherited socket, initialize the Zoom SDK, authenticate it with the
// public app key, and report whether the account carries the raw-data license.
// All of it is reported over IPC as log lines, which is the verifiable
// milestone: the OBS log shows the handshake, the auth result and the license.
//
// Does NOT: log in to Zoom (OAuth), join meetings, or deliver media. Zoom login
// and join are increment 1b; raw video is increment 2. Nothing here pretends to
// succeed at work it has not done.
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
//     and the main thread stay line-atomic.
// Note that initSDKWithParams legitimately BLOCKS the main thread for many
// seconds. Nothing in this increment depends on main-thread responsiveness
// during that window; when a heartbeat is added (with the plugin-side watchdog
// in a later increment) it must be written from the main queue, or it would
// vouch for a main thread that is actually wedged.
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
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

#include "feeds-ipc-posix.h"
#include "feeds-json-lite.h"
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
// Raw-data license
//
// The account-entitlement question, answered as a side effect of bringing the
// SDK up: raw data (the participant video this product is built on) requires a
// licensed account, and without it every later subscription fails. Reported now,
// once, rather than being discovered as "the video never arrives" in increment 2.
// Requires an authenticated SDK, so it runs from the auth callback.
// ---------------------------------------------------------------------------
static void ReportRawDataLicense()
{
    ZoomSDKRawDataController* raw = [[ZoomSDK sharedSDK] getRawDataController];
    if (!raw) {
        LogWarn("Mac engine: raw-data controller unavailable; cannot check the "
                "raw-data license");
        return;
    }

    const ZoomSDKError err = [raw hasRawDataLicense];
    if (err == ZoomSDKError_Success) {
        LogInfo("Mac engine: raw-data license OK — this account may receive "
                "participant video");
    } else {
        LogError("Mac engine: raw-data license CHECK FAILED (code " +
                 std::to_string((int)err) +
                 "). Participant video will not be available on this account "
                 "until Zoom grants the raw-data (Meeting SDK) entitlement.");
    }
}

// ---------------------------------------------------------------------------
// Auth delegate
// ---------------------------------------------------------------------------
@interface FeedsAuthDelegate : NSObject <ZoomSDKAuthDelegate>
@end

@implementation FeedsAuthDelegate

- (void)onZoomSDKAuthReturn:(ZoomSDKAuthError)returnValue
{
    if (returnValue == ZoomSDKAuthError_Success) {
        LogInfo("Mac engine: SDK authentication SUCCEEDED");
        ReportRawDataLicense();
        return;
    }
    LogError(std::string("Mac engine: SDK authentication FAILED: ") +
             AuthErrorName(returnValue) + " (code " +
             std::to_string((int)returnValue) + ")");
}

// @required by the protocol.
- (void)onZoomAuthIdentityExpired
{
    LogWarn("Mac engine: Zoom auth identity expired");
}

@end

// The delegate property is `assign` (unowned), so this must outlive the auth
// service: a deliberately never-released global, as the SDK's own samples do.
static FeedsAuthDelegate* g_authDelegate = nil;

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
        return;
    }
    LogInfo("Mac engine: initSDKWithParams SUCCEEDED");

    ZoomSDKAuthService* authService = [[ZoomSDK sharedSDK] getAuthService];
    if (!authService) {
        LogError("Mac engine: getAuthService returned nil; cannot authenticate");
        return;
    }

    if (!g_authDelegate) g_authDelegate = [[FeedsAuthDelegate alloc] init];
    authService.delegate = g_authDelegate;

    // The same credential the Windows engine authenticates with: the public app
    // key (our Zoom OAuth client id), baked in at build time. NOT a JWT — Feeds
    // has no client secret to sign one with, which is the whole point of the
    // public-app-key flow.
    const std::string clientId = FEEDS_ZOOM_CLIENT_ID;
    if (clientId.empty()) {
        LogError("Mac engine: no public app key was compiled in "
                 "(FEEDS_ZOOM_CLIENT_ID); cannot authenticate the SDK");
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
        return;
    }
    LogInfo("Mac engine: sdkAuth accepted; waiting for the auth callback");
}

// ---------------------------------------------------------------------------
// Command handling
// ---------------------------------------------------------------------------
static void HandleCommand(const std::string& line)
{
    const std::string type = feeds::ExtractJsonString(line, "type");
    if (type.empty()) return;

    if (type == "shutdown") {
        LogInfo("Mac engine: shutdown requested");
        g_running.store(false);
        dispatch_async(dispatch_get_main_queue(), ^{
            [[ZoomSDK sharedSDK] unInitSDK];
            ::exit(0);
        });
        return;
    }

    // Everything else belongs to a later increment. Say so plainly rather than
    // dropping it silently, so a premature message is visible in the log.
    LogWarn("Mac engine: ignoring '" + type +
            "' — not implemented in this build (Zoom login, join and video are "
            "later increments)");
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

    // Bring the SDK up on the main queue, after the run loop starts.
    dispatch_async(dispatch_get_main_queue(), ^{ BringUpSdk(); });

    // Hand the main thread to Cocoa: the SDK is an AppKit client and needs a
    // real NSApplication, not a bare NSRunLoop, for its delegates to fire.
    //
    // Accessory, not Regular: this increment shows no SDK UI, and Accessory
    // keeps the engine out of the Dock and the app switcher. When joining lands
    // (increment 1b) and the SDK's meeting window appears, this likely has to
    // become Regular — an Accessory app has no menu bar and cannot make its
    // windows properly key.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp run];
    return 0;
}
