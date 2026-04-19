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


static const wchar_t* P2E_PIPE_NAME = L"\\\\.\\pipe\\FeedsEngine_P2E";
static const wchar_t* E2P_PIPE_NAME = L"\\\\.\\pipe\\FeedsEngine_E2P";

static HANDLE g_readPipe  = INVALID_HANDLE_VALUE;  // engine reads from P2E
static HANDLE g_writePipe = INVALID_HANDLE_VALUE;  // engine writes to E2P
static std::map<std::string, std::function<void(const std::string&)>> g_messageHandlers;

namespace feeds_engine { 
    bool InitializeSDK();
    bool AuthenticateSDK();
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void LogToFile(const char* msg)
{
    wchar_t logPath[MAX_PATH];
    GetModuleFileNameW(NULL, logPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(logPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcscat_s(logPath, L"FeedsEngine.log");

    FILE* f = nullptr;
    _wfopen_s(&f, logPath, L"a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        fclose(f);
    }
}

// ---------------------------------------------------------------------------
// Pipe send/receive
// ---------------------------------------------------------------------------

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
    DWORD written = 0;
    BOOL ok = WriteFile(g_writePipe, json.c_str(), (DWORD)json.size(), &written, NULL);
    if (!ok) {
        char msg[256];
        sprintf_s(msg, "WriteFile failed: %lu", GetLastError());
        LogToFile(msg);
        return false;
    }

    char msg[4200];
    sprintf_s(msg, "Sent: %s", json.c_str());
    LogToFile(msg);
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
        char msg[4200];
        sprintf_s(msg, "Could not extract type from: %s", json.c_str());
        LogToFile(msg);
        return;
    }

    auto it = g_messageHandlers.find(type);
    if (it != g_messageHandlers.end()) {
        it->second(json);
    } else {
        char msg[4200];
        sprintf_s(msg, "No handler for type '%s' (message: %s)", type.c_str(), json.c_str());
        LogToFile(msg);
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
            std::string json(buffer, bytesRead);

            char msg[4200];
            sprintf_s(msg, "Received: %s", json.c_str());
            LogToFile(msg);

            DispatchIpcMessage(json);
        }
    }
}

// ---------------------------------------------------------------------------
// Message handlers (stubs for now — will be filled in as phases progress)
// ---------------------------------------------------------------------------

// Forward declaration — implemented in engine-oauth.cpp
namespace feeds_engine { bool StartLoginFlow(); }

static void HandleLoginStart(const std::string& json)
{
    LogToFile("HandleLoginStart: starting OAuth flow");
    feeds_engine::StartLoginFlow();
}
// Forward decls — defined in engine-meeting.cpp
namespace feeds_engine {
    void HandleJoinMeeting(const std::string&);
    void HandleLeaveMeeting(const std::string&);
    void HandleGetParticipants(const std::string&);
    void HandleLogout(const std::string&);
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
// Dummy window proc for the message-only window
static LRESULT CALLBACK EngineWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
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

    if (!ConnectToPipes()) {
        LogToFile("Could not connect to pipes, exiting");
        return 1;
    }

    // Register handlers for messages we expect from the plugin
    RegisterHandler("login_start",                    HandleLoginStart);
    RegisterHandler("logout",                         feeds_engine::HandleLogout);
    RegisterHandler("join_meeting",                   feeds_engine::HandleJoinMeeting);
    RegisterHandler("leave_meeting",                  feeds_engine::HandleLeaveMeeting);
    RegisterHandler("get_participants",               feeds_engine::HandleGetParticipants);
    RegisterHandler("participant_source_subscribe",   feeds_engine::HandleParticipantSourceSubscribe);
    RegisterHandler("participant_source_unsubscribe", feeds_engine::HandleParticipantSourceUnsubscribe);
    RegisterHandler("shutdown",                       HandleShutdown);

    // Announce we're ready. Include our PID so the plugin can construct
    // the shared-memory region names used for video frames.
    char readyMsg[128];
    sprintf_s(readyMsg,
        "{\"type\":\"engine_ready\",\"version\":\"1.0.0\",\"pid\":%lu}",
        GetCurrentProcessId());
    if (!SendToPlugin(readyMsg)) {
        LogToFile("Failed to send engine_ready");
        return 1;
    }

    // Initialize the Zoom SDK. Must happen on the thread that runs the message
    // pump and created the window.
    feeds_engine::InitializeSDK();

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

    DestroyWindow(hwnd);
    LogToFile("FeedsEngine.exe exiting normally");
    return 0;
}
