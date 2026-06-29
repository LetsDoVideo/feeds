// FeedsEngine.exe — subprocess that hosts the Zoom Meeting SDK.
//
// Launched by feeds.dll. Connects to the named pipe created by the plugin,
// sends engine_ready, then processes incoming command messages.

#include <windows.h>
#include <string>
#include <cstdio>
#include <map>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

#include "feeds-version.h"
#include "engine-shared.h"

// ===========================================================================
// TEMPORARY — freeze-investigation heartbeat (remove after the hard-hang
// cause is found). Everything for this diagnostic is gated on FEEDS_HEARTBEAT
// and tagged "[feeds-hb]" in the log; grep FEEDS_HEARTBEAT to pull it out.
// See the HeartbeatLoop block lower in this file for what each tick records.
// ===========================================================================
#define FEEDS_HEARTBEAT 1
#if FEEDS_HEARTBEAT
#define PSAPI_VERSION 2          // route GetProcessMemoryInfo to kernel32 (K32*),
#include <psapi.h>               // so no psapi.lib link is needed
#include <tlhelp32.h>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstdlib>

// Total protocol messages over IPC since start. Incremented in SendToPlugin
// (outbound) and PipeReaderLoop (inbound). Relaxed atomics — diagnostic only,
// behavior-neutral. Log lines bypass SendToPlugin, so they are NOT counted
// here; this counts real protocol messages only.
static std::atomic<uint64_t> g_hbIpcSent{0};
static std::atomic<uint64_t> g_hbIpcRecv{0};
#endif // FEEDS_HEARTBEAT

// Defined here, declared extern in engine-shared.h so engine-meeting.cpp
// can post WM_FEEDS_SEND_CHAT to it from the pipe thread. Set inside
// WinMain right after CreateWindowExW succeeds.
HWND g_anchorWnd = nullptr;

static const wchar_t* P2E_PIPE_NAME = L"\\\\.\\pipe\\FeedsEngine_P2E";
static const wchar_t* E2P_PIPE_NAME = L"\\\\.\\pipe\\FeedsEngine_E2P";

static HANDLE g_readPipe  = INVALID_HANDLE_VALUE;  // engine reads from P2E
static HANDLE g_writePipe = INVALID_HANDLE_VALUE;  // engine writes to E2P
static std::map<std::string, std::function<void(const std::string&)>> g_messageHandlers;

namespace feeds_engine {
    // REST-only session restore, run at startup in place of the old
    // InitializeSDK() call — restores the logged-in appearance from a stored
    // token without bringing up the Zoom SDK. The SDK comes up lazily on the
    // first connect (see engine-sdk.cpp). BringUpSdkOnMainThread (the
    // WM_FEEDS_INIT_SDK target) is declared in engine-shared.h.
    void RestoreSessionFromStoredToken();
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

// Forward decls — defined further down in this file.
static std::string ExtractJsonStringField(const std::string& json,
                                          const std::string& field);
static bool WriteToPipeRaw(const std::string& json, DWORD* outErr);

// Minimal JSON string-escape so log text containing quotes, backslashes, or
// control characters round-trips intact inside the IPC message. Mirrors the
// JsonEscape in engine-meeting.cpp and the plugin so the plugin's
// escape-aware extractor (ExtractJsonStringEscaped) can decode it.
static std::string JsonEscapeLog(const char* s)
{
    std::string out;
    for (const char* p = s ? s : ""; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// The engine forwards every log line to the plugin over the E2P pipe as a
// {"type":"log","level":"...","message":"..."} message; the plugin re-emits it
// into the OBS log via blog() at the matching level.
//
// Phase 2 assigns levels. The legacy LogToFile forwards at "debug" — so every
// call site left untouched becomes a DEBUG trace — while LogInfo / LogWarn /
// LogError forward their respective levels. Only the specific lines that belong
// in the default OBS log (the session narrative, degraded-but-handled
// warnings, and hard failures) were switched to those helpers.
//
// Lines emitted before the E2P pipe is connected (engine start, pipe-connect
// attempts) are dropped on purpose — we deliberately do NOT buffer. The plugin
// tracks engine connection state from its own side, so a not-yet-connected
// engine is not a total blind spot.
static void ForwardLog(const char* level, const char* msg)
{
    if (g_writePipe == INVALID_HANDLE_VALUE)
        return;  // pipe not up yet — drop, no buffer.

    std::string out = "{\"type\":\"log\",\"level\":\"";
    out += level;
    out += "\",\"message\":\"";
    out += JsonEscapeLog(msg);
    out += "\"}";
    WriteToPipeRaw(out, nullptr);
}

// LogToFile keeps its name so the many existing call sites (and their extern
// decls in the other TUs) need no change; it now forwards at DEBUG. Use
// LogInfo / LogWarn / LogError for lines that belong in the default OBS log.
void LogToFile(const char* msg) { ForwardLog("debug",   msg); }
void LogInfo  (const char* msg) { ForwardLog("info",    msg); }
void LogWarn  (const char* msg) { ForwardLog("warning", msg); }
void LogError (const char* msg) { ForwardLog("error",   msg); }

// ---------------------------------------------------------------------------
// Pipe send/receive
// ---------------------------------------------------------------------------

// Serializes all writes to the E2P pipe. The engine sends — and now logs —
// from multiple threads (pipe-reader, main message pump, video/scaler
// callbacks). Every log line is also an E2P write, so without this lock
// concurrent writes could interleave and corrupt messages on the pipe. This
// is the main correctness risk of routing logs through the pipe.
static std::mutex g_writePipeMutex;

// Low-level serialized write to the E2P pipe. Returns false if the pipe isn't
// connected or WriteFile fails; on WriteFile failure the OS error is reported
// via outErr (when non-null), captured while the lock is still held. Holds
// g_writePipeMutex only around the WriteFile so callers can log before/after
// without re-entering the (non-recursive) lock. Deliberately does NOT call
// LogToFile — LogToFile calls this, and recursion would self-deadlock.
static bool WriteToPipeRaw(const std::string& json, DWORD* outErr)
{
    if (g_writePipe == INVALID_HANDLE_VALUE) {
        if (outErr) *outErr = ERROR_PIPE_NOT_CONNECTED;
        return false;
    }

    std::lock_guard<std::mutex> lock(g_writePipeMutex);
    DWORD written = 0;
    BOOL ok = WriteFile(g_writePipe, json.c_str(), (DWORD)json.size(), &written, NULL);
    if (!ok && outErr) *outErr = GetLastError();
    return ok != FALSE;
}

static bool ConnectToPipes()
{
    // Connect to P2E (plugin-to-engine: engine reads)
    for (int i = 0; i < 20; i++) {
        g_readPipe = CreateFileW(
            P2E_PIPE_NAME,
            GENERIC_READ,
            0, NULL, OPEN_EXISTING, 0, NULL);

        if (g_readPipe != INVALID_HANDLE_VALUE) {
            LogToFile("Connected to P2E pipe");
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(g_readPipe, &mode, NULL, NULL);
            break;
        }

        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(P2E_PIPE_NAME, 500);
        } else {
            Sleep(250);
        }
    }

    if (g_readPipe == INVALID_HANDLE_VALUE) {
        LogToFile("Failed to connect to P2E pipe");
        return false;
    }

    // Connect to E2P (engine-to-plugin: engine writes)
    for (int i = 0; i < 20; i++) {
        g_writePipe = CreateFileW(
            E2P_PIPE_NAME,
            GENERIC_WRITE,
            0, NULL, OPEN_EXISTING, 0, NULL);

        if (g_writePipe != INVALID_HANDLE_VALUE) {
            LogToFile("Connected to E2P pipe");
            return true;
        }

        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(E2P_PIPE_NAME, 500);
        } else {
            Sleep(250);
        }
    }

    LogToFile("Failed to connect to E2P pipe");
    return false;
}

bool SendToPlugin(const std::string& json)
{
    DWORD err = 0;
    if (!WriteToPipeRaw(json, &err)) {
        char msg[256];
        sprintf_s(msg, "WriteFile failed: %lu", err);
        LogToFile(msg);
        return false;
    }

    // Redaction: log only the message type, never the payload. Outgoing
    // messages can carry meeting password, webinar_token, ZAK, and join_token
    // — none of which may reach the shared OBS log.
    std::string type = ExtractJsonStringField(json, "type");
    if (type.empty()) type = "(unknown)";
    LogToFile(("Sent: " + type).c_str());
#if FEEDS_HEARTBEAT
    g_hbIpcSent.fetch_add(1, std::memory_order_relaxed);  // TEMPORARY (heartbeat)
#endif
    return true;
}

// ---------------------------------------------------------------------------
// JSON helpers and message dispatch
// ---------------------------------------------------------------------------

static std::string ExtractJsonStringField(const std::string& json, const std::string& field)
{
    std::string key = "\"" + field + "\"";
    size_t keyPos = json.find(key);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos + key.size());
    if (colonPos == std::string::npos) return "";

    size_t quoteStart = json.find('"', colonPos + 1);
    if (quoteStart == std::string::npos) return "";

    size_t quoteEnd = json.find('"', quoteStart + 1);
    if (quoteEnd == std::string::npos) return "";

    return json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
}

static void RegisterHandler(const std::string& type,
                            std::function<void(const std::string&)> handler)
{
    g_messageHandlers[type] = handler;
}

static void DispatchIpcMessage(const std::string& json)
{
    std::string type = ExtractJsonStringField(json, "type");
    if (type.empty()) {
        // Redaction: never log the raw payload — incoming messages can carry
        // secrets (join_meeting: password, ZAK, join_token, webinar_token).
        LogToFile("Could not extract type from incoming message");
        return;
    }

    auto it = g_messageHandlers.find(type);
    if (it != g_messageHandlers.end()) {
        it->second(json);
    } else {
        // Redaction: log only the type, never the payload.
        LogToFile(("No handler for type '" + type + "'").c_str());
    }
}

static void PipeReaderLoop()
{
    char buffer[4096];
    while (true) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(g_readPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL);

        if (!ok) {
            DWORD err = GetLastError();
            char msg[256];
            sprintf_s(msg, "ReadFile failed: %lu (pipe probably closed)", err);
            LogToFile(msg);
            break;
        }

        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
#if FEEDS_HEARTBEAT
            g_hbIpcRecv.fetch_add(1, std::memory_order_relaxed);  // TEMPORARY (heartbeat)
#endif
            std::string json(buffer, bytesRead);

            // Redaction: log only the message type, never the payload.
            // Incoming join_meeting carries password, ZAK, join_token, and
            // webinar_token — none of which may reach the shared OBS log.
            std::string type = ExtractJsonStringField(json, "type");
            if (type.empty()) type = "(unknown)";
            LogToFile(("Received: " + type).c_str());

            DispatchIpcMessage(json);
        }
    }
}

// ---------------------------------------------------------------------------
// Message handlers (stubs for now — will be filled in as phases progress)
// ---------------------------------------------------------------------------

// Forward declarations — implemented in engine-oauth.cpp
namespace feeds_engine {
    bool StartLoginFlow();
    void CancelLoginFlow();
}

static void HandleLoginStart(const std::string& json)
{
    LogToFile("HandleLoginStart: starting OAuth flow");
    feeds_engine::StartLoginFlow();
}

static void HandleLoginCancel(const std::string& json)
{
    LogToFile("HandleLoginCancel: cancelling in-flight OAuth flow");
    feeds_engine::CancelLoginFlow();
}
// Forward decls — defined in engine-meeting.cpp
namespace feeds_engine {
    void HandleJoinMeeting(const std::string&);
    void HandleSendChatMessage(const std::string&);
    void HandleCreateInstantMeeting(const std::string&);
    void HandleLeaveMeeting(const std::string&);
    void HandleGetParticipants(const std::string&);
    void HandleLogout(const std::string&);
    void HandleRequestEvents(const std::string&);
    void HandleRequestSessions(const std::string&);
    void HandleJoinEventSession(const std::string&);
}
// Forward decls — defined in engine-video.cpp
namespace feeds_engine {
    void HandleParticipantSourceSubscribe(const std::string&);
    void HandleParticipantSourceUnsubscribe(const std::string&);
}

static void HandleShutdown(const std::string& json)
{
    LogToFile("HandleShutdown: received shutdown message");

    // If we're currently in a Zoom meeting, leave it cleanly before the
    // process exits. Without this, the Zoom meeting lingers after OBS
    // closes — the SDK won't signal the server that we left, so other
    // participants see our camera stay on as a ghost. Matches v1.0.0
    // behavior where the SDK ran in-process and naturally cleaned up
    // when OBS exited.
    //
    // HandleLeaveMeeting is a no-op if we're not in a meeting, so it's
    // safe to call unconditionally.
    feeds_engine::HandleLeaveMeeting(json);

    // Give the SDK a brief window to deliver the "leaving" message to
    // Zoom's servers before we exit. The SDK's Leave() call is
    // asynchronous — it doesn't block until delivery completes. Without
    // this wait, the process exits before the network packet goes out.
    // 500ms is a compromise between "always works" and "not annoyingly
    // slow to close OBS." Picked based on empirical testing.
    Sleep(500);

    SendToPlugin("{\"type\":\"shutdown_complete\"}");
    // Pipe will close when plugin closes its handle; reader loop will exit.
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static std::atomic<bool> g_engineShuttingDown{false};
// Window proc for the anchor window. Mostly a passthrough, but handles
// WM_FEEDS_SEND_CHAT — our cross-thread marshalling vehicle for SDK
// chat sends, which must run on the main thread (this thread). See
// engine-shared.h for the protocol.
static LRESULT CALLBACK EngineWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_FEEDS_SEND_CHAT) {
        // lp is a heap-allocated std::string* the pipe handler created.
        // Take ownership and free unconditionally.
        std::string* content = reinterpret_cast<std::string*>(lp);
        if (content) {
            feeds_engine::SendChatMessageOnMainThread(*content);
            delete content;
        }
        return 0;
    }
    if (msg == WM_FEEDS_INIT_SDK) {
        // Lazy SDK bring-up, marshaled here from the pipe thread by
        // EnsureSdkUpThen. InitSDK/SDKAuth must run on this (pump) thread.
        feeds_engine::BringUpSdkOnMainThread();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

#if FEEDS_HEARTBEAT
// ===========================================================================
// TEMPORARY — freeze-investigation heartbeat thread.
//
// Wakes on a fixed interval (default 30s; set env FEEDS_HB_INTERVAL_MS, e.g.
// 10000 for a denser curve, or edit kHbDefaultIntervalMs) and writes ONE compact
// "[feeds-hb]" line per tick at INFO level so it lands in the default OBS log.
// Runs independently of SDK/login state — started right after the pipes
// connect and never gated on InitializeSDK — so it covers the idle,
// logged-in-but-not-connected state we're chasing.
//
// Each line is a time series row: monotonic seconds-since-start (t=) and
// wall-clock HH:MM:SS (wall=) for row-for-row alignment with a Performance
// Monitor capture, then the richest per-process snapshot obtainable in-proc:
//   OS counters  : handles, threads, working-set MB, private-bytes MB,
//                  GDI objects, USER objects
//   Feeds-internal: live source subscriptions (subs=), and total protocol
//                  IPC messages out/in since start (ipcout=/ipcin=)
// (There is no IPC queue and no log buffer in this engine — writes are
// synchronous and ForwardLog never buffers — so there is nothing of that kind
// to report; subs= is the only container that grows over a session.)
//
// Allocation-free per tick: fixed stack buffers, atomics, and a Toolhelp
// snapshot that is a kernel object freed each tick. The line is flushed out of
// the engine immediately — the last line before a hang is the whole point.
// ===========================================================================
namespace feeds_engine { size_t GetActiveSubscriptionCount(); }

static const DWORD          kHbDefaultIntervalMs = 30000;  // edit or set FEEDS_HB_INTERVAL_MS
static std::atomic<bool>    g_hbStop{false};
static std::mutex           g_hbMutex;
static std::condition_variable g_hbCv;
static std::thread          g_hbThread;

// Thread count for our own process. No direct Win32 API for this, so snapshot
// the process table and read our entry's cntThreads. Walks all processes —
// the one mildly-costly call here, but at a 10–30s cadence it's negligible.
static unsigned HbSelfThreadCount()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    DWORD me = GetCurrentProcessId();
    unsigned count = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == me) { count = pe.cntThreads; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}

// Push whatever is already in the pipe out of the engine now. WriteFile in
// WriteToPipeRaw already delivers synchronously, so the line is "out" before
// this returns regardless; this is belt-and-braces per the flush requirement.
static void HbFlush()
{
    if (g_writePipe != INVALID_HANDLE_VALUE) FlushFileBuffers(g_writePipe);
}

static void HeartbeatLoop()
{
    DWORD intervalMs = kHbDefaultIntervalMs;
    char env[32];
    if (GetEnvironmentVariableA("FEEDS_HB_INTERVAL_MS", env, sizeof(env)) > 0) {
        int v = atoi(env);
        if (v >= 1000) intervalMs = (DWORD)v;  // floor at 1s; ignore garbage
    }

    const ULONGLONG startTick = GetTickCount64();
    const HANDLE     proc      = GetCurrentProcess();

    {
        char banner[160];
        sprintf_s(banner,
            "[feeds-hb] heartbeat started, interval=%lums (TEMPORARY freeze diagnostic)",
            intervalMs);
        LogInfo(banner);
        HbFlush();
    }

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_hbMutex);
            g_hbCv.wait_for(lk, std::chrono::milliseconds(intervalMs),
                            [] { return g_hbStop.load(); });
            if (g_hbStop.load()) break;
        }

        const ULONGLONG nowTick   = GetTickCount64();
        const unsigned long long uptimeSec = (nowTick - startTick) / 1000ULL;

        SYSTEMTIME lt; GetLocalTime(&lt);

        DWORD handles = 0; GetProcessHandleCount(proc, &handles);
        unsigned threads = HbSelfThreadCount();

        PROCESS_MEMORY_COUNTERS_EX pmc; ZeroMemory(&pmc, sizeof(pmc));
        pmc.cb = sizeof(pmc);
        double wsMB = 0.0, privMB = 0.0;
        if (GetProcessMemoryInfo(proc, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            wsMB   = pmc.WorkingSetSize / (1024.0 * 1024.0);
            privMB = pmc.PrivateUsage  / (1024.0 * 1024.0);
        }

        DWORD gdi  = GetGuiResources(proc, GR_GDIOBJECTS);
        DWORD user = GetGuiResources(proc, GR_USEROBJECTS);

        size_t             subs   = feeds_engine::GetActiveSubscriptionCount();
        unsigned long long ipcOut = g_hbIpcSent.load(std::memory_order_relaxed);
        unsigned long long ipcIn  = g_hbIpcRecv.load(std::memory_order_relaxed);

        char line[512];
        sprintf_s(line,
            "[feeds-hb] t=%06llu wall=%02d:%02d:%02d handles=%lu threads=%u "
            "ws=%.1fM priv=%.1fM gdi=%lu user=%lu subs=%zu ipcout=%llu ipcin=%llu",
            uptimeSec, lt.wHour, lt.wMinute, lt.wSecond,
            (unsigned long)handles, threads, wsMB, privMB,
            (unsigned long)gdi, (unsigned long)user, subs, ipcOut, ipcIn);
        LogInfo(line);
        HbFlush();
    }

    LogInfo("[feeds-hb] heartbeat stopped");
    HbFlush();
}
#endif // FEEDS_HEARTBEAT

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // --- TEMPORARY MIGRATION (remove in a few releases) --------------------
    // Older versions wrote engine logs to FeedsEngine.log next to the exe.
    // Engine logs now go to the OBS log via the plugin, so delete any stale
    // file left over from a previous install. New installs never create one.
    {
        wchar_t oldLog[MAX_PATH];
        GetModuleFileNameW(NULL, oldLog, MAX_PATH);
        wchar_t* slash = wcsrchr(oldLog, L'\\');
        if (slash) *(slash + 1) = L'\0';
        wcscat_s(oldLog, L"FeedsEngine.log");
        DeleteFileW(oldLog);  // ignore result — an absent file is the norm
    }
    // --- END TEMPORARY MIGRATION -------------------------------------------

    // NOTE: these two lines run before the E2P pipe is connected, so they are
    // dropped (LogToFile no longer buffers). Kept for symmetry once connected.
    LogToFile("========================================");
    LogToFile("FeedsEngine.exe starting");

    // Create a real (but invisible) top-level window. The Zoom SDK requires
    // a real HWND on the main thread for its async callbacks to fire, AND
    // it looks for a top-level window in the process to anchor its own
    // meeting UI to. A message-only window (HWND_MESSAGE parent) is not a
    // true top-level window — the Zoom meeting window ends up in a
    // confused initial state with a transparent top strip until the user
    // presses Esc. A normal hidden popup window acts as the anchor the SDK
    // wants without appearing in the taskbar or becoming visible.
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = EngineWndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"FeedsEngineWindow";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,           // no taskbar entry
        L"FeedsEngineWindow", L"FeedsEngine",
        WS_POPUP,                   // top-level, not a child
        0, 0, 1, 1,                 // 1x1 at origin; never shown
        NULL, NULL, hInstance, NULL);

    if (!hwnd) {
        LogToFile("Failed to create anchor window");
        return 1;
    }
    // Note: intentionally do NOT call ShowWindow. The window stays hidden.
    LogToFile("Created anchor window");
    // Publish the anchor handle so engine-meeting.cpp's chat send path
    // can post WM_FEEDS_SEND_CHAT to it from the pipe-reader thread.
    g_anchorWnd = hwnd;
    {
        char buf[96];
        sprintf_s(buf, "Engine: main thread id = %lu",
                  (unsigned long)GetCurrentThreadId());
        LogToFile(buf);
    }

    if (!ConnectToPipes()) {
        LogToFile("Could not connect to pipes, exiting");
        return 1;
    }

    // Register handlers for messages we expect from the plugin
    RegisterHandler("login_start",                    HandleLoginStart);
    RegisterHandler("login_cancel",                   HandleLoginCancel);
    RegisterHandler("logout",                         feeds_engine::HandleLogout);
    RegisterHandler("join_meeting",                   feeds_engine::HandleJoinMeeting);
    RegisterHandler("create_instant_meeting",         feeds_engine::HandleCreateInstantMeeting);
    RegisterHandler("leave_meeting",                  feeds_engine::HandleLeaveMeeting);
    RegisterHandler("get_participants",               feeds_engine::HandleGetParticipants);
    RegisterHandler("send_chat_message",              feeds_engine::HandleSendChatMessage);
    RegisterHandler("request_events",                 feeds_engine::HandleRequestEvents);
    RegisterHandler("request_sessions",               feeds_engine::HandleRequestSessions);
    RegisterHandler("join_event_session",             feeds_engine::HandleJoinEventSession);
    RegisterHandler("participant_source_subscribe",   feeds_engine::HandleParticipantSourceSubscribe);
    RegisterHandler("participant_source_unsubscribe", feeds_engine::HandleParticipantSourceUnsubscribe);
    RegisterHandler("shutdown",                       HandleShutdown);

    // Announce we're ready. Include our PID so the plugin can construct
    // the shared-memory region names used for video frames.
    char readyMsg[128];
    sprintf_s(readyMsg,
        "{\"type\":\"engine_ready\",\"version\":\"%s\",\"pid\":%lu}",
        feeds_shared::VERSION, GetCurrentProcessId());
    if (!SendToPlugin(readyMsg)) {
        LogToFile("Failed to send engine_ready");
        return 1;
    }

#if FEEDS_HEARTBEAT
    // TEMPORARY (freeze diagnostic): start the heartbeat now — after the pipes
    // are up so its lines reach the OBS log, and before InitializeSDK so it
    // covers the idle, logged-in-but-not-connected state regardless of SDK
    // or login state. Stopped and joined during shutdown below.
    g_hbThread = std::thread(HeartbeatLoop);
#endif

    // Restore the logged-in appearance over REST if a token is stored — but
    // do NOT initialize the Zoom SDK here. An idle, logged-in-but-not-connected
    // Feeds now runs no SDK and holds no Zoom authenticated session, so it adds
    // no steady load. The SDK is brought up lazily on the first connect, on
    // this same (pump) thread via WM_FEEDS_INIT_SDK. See engine-sdk.cpp.
    feeds_engine::RestoreSessionFromStoredToken();

    // Pipe reading runs on a background thread so the main thread is free
    // to pump Windows messages (required for Zoom SDK async callbacks).
    std::thread pipeThread([]() {
        PipeReaderLoop();
        g_engineShuttingDown = true;
        PostQuitMessage(0);
    });

    LogToFile("Entering main thread message pump");
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    LogToFile("Exited main thread message pump");

    if (pipeThread.joinable()) {
        pipeThread.join();
    }

#if FEEDS_HEARTBEAT
    // TEMPORARY (freeze diagnostic): stop the heartbeat and join cleanly so it
    // can't touch the pipe after teardown.
    {
        std::lock_guard<std::mutex> lk(g_hbMutex);
        g_hbStop = true;
    }
    g_hbCv.notify_all();
    if (g_hbThread.joinable()) g_hbThread.join();
#endif

    DestroyWindow(hwnd);
    LogToFile("FeedsEngine.exe exiting normally");
    return 0;
}
