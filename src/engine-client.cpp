// engine-client.cpp — manages the FeedsEngine.exe subprocess and IPC with it.
//
// Uses TWO unidirectional named pipes to avoid any read/write serialization
// issues on a single duplex pipe:
//   \\.\pipe\FeedsEngine_P2E  — plugin writes, engine reads
//   \\.\pipe\FeedsEngine_E2P  — engine writes, plugin reads

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <map>
#include <functional>
#include <mutex>
#include <future>
#include <chrono>
#include <obs-module.h>

#include "feeds-ipc-reassembly.h"

namespace feeds {

static HANDLE g_jobObject = NULL;
static HANDLE g_engineProcess = NULL;
static HANDLE g_p2ePipe = INVALID_HANDLE_VALUE;  // plugin writes to this
static HANDLE g_e2pPipe = INVALID_HANDLE_VALUE;  // plugin reads from this
static std::thread g_pipeReaderThread;
static std::atomic<bool> g_shutdownRequested{false};

static std::mutex g_handlersMutex;
static std::map<std::string, std::function<void(const std::string&)>> g_messageHandlers;

static std::mutex g_writeMutex;

static const wchar_t* P2E_PIPE_NAME = L"\\\\.\\pipe\\FeedsEngine_P2E";
static const wchar_t* E2P_PIPE_NAME = L"\\\\.\\pipe\\FeedsEngine_E2P";

// Forward declarations
static bool CreatePipeServers();
static bool CreateJobObject();
static bool LaunchEngineProcess();
static bool WaitForEngineConnections();
static void PipeReaderThread();
static std::string ExtractJsonStringField(const std::string& json, const std::string& field);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool StartEngine()
{
    blog(LOG_INFO, "[feeds] StartEngine: beginning engine startup sequence");

    if (!CreateJobObject()) {
        blog(LOG_ERROR, "[feeds] StartEngine: failed to create Job Object");
        return false;
    }

    if (!CreatePipeServers()) {
        blog(LOG_ERROR, "[feeds] StartEngine: failed to create pipe servers");
        return false;
    }

    if (!LaunchEngineProcess()) {
        blog(LOG_ERROR, "[feeds] StartEngine: failed to launch engine process");
        return false;
    }

    if (!WaitForEngineConnections()) {
        blog(LOG_ERROR, "[feeds] StartEngine: engine did not connect to pipes");
        return false;
    }

    blog(LOG_INFO, "[feeds] StartEngine: engine connected to both pipes");

    g_shutdownRequested = false;
    g_pipeReaderThread = std::thread(PipeReaderThread);

    return true;
}

void StopEngine()
{
    blog(LOG_INFO, "[feeds] StopEngine: shutting down");

    g_shutdownRequested = true;

    // Tell the engine to clean up gracefully. The engine's shutdown handler
    // leaves any meeting, sends shutdown_complete, then the pipe closes.
    if (g_p2ePipe != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const char* shutdownMsg = "{\"type\":\"shutdown\"}";
        // Best-effort; don't block shutdown if this fails.
        WriteFile(g_p2ePipe, shutdownMsg, (DWORD)strlen(shutdownMsg),
                  &written, NULL);
    }

    // Unstick the pipe reader if it's blocked in ReadFile. Without this,
    // closing the handle from another thread does not reliably wake a
    // blocking ReadFile on Windows, and thread::join hangs forever. This
    // was the cause of OBS's "zombie process" problem where the main
    // executable lingered in Task Manager Details even after appearing
    // to close.
    if (g_e2pPipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(g_e2pPipe, NULL);
    }

    // Give the reader thread up to 2 seconds to exit cleanly. If it doesn't,
    // we still forcibly move on — OBS shutdown is more important than
    // perfect cleanup at this point.
    if (g_pipeReaderThread.joinable()) {
        auto future = std::async(std::launch::async, [](){
            if (g_pipeReaderThread.joinable())
                g_pipeReaderThread.join();
        });
        if (future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
            blog(LOG_WARNING, "[feeds] StopEngine: pipe reader did not exit, detaching");
            g_pipeReaderThread.detach();
        }
    }

    if (g_p2ePipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_p2ePipe);
        g_p2ePipe = INVALID_HANDLE_VALUE;
    }
    if (g_e2pPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_e2pPipe);
        g_e2pPipe = INVALID_HANDLE_VALUE;
    }

    // Give the engine a moment to exit gracefully before we kill it via
    // Job Object. The engine typically exits within ~200ms of receiving
    // the shutdown message.
    if (g_engineProcess) {
        WaitForSingleObject(g_engineProcess, 500);
    }

    // Closing the Job Object handle fires KILL_ON_JOB_CLOSE, which
    // terminates the engine subprocess if it hasn't exited on its own.
    if (g_jobObject != NULL) {
        CloseHandle(g_jobObject);
        g_jobObject = NULL;
    }

    if (g_engineProcess != NULL) {
        CloseHandle(g_engineProcess);
        g_engineProcess = NULL;
    }

    blog(LOG_INFO, "[feeds] StopEngine: shutdown complete");
}

bool SendToEngine(const std::string& jsonMessage)
{
    std::lock_guard<std::mutex> lock(g_writeMutex);

    if (g_p2ePipe == INVALID_HANDLE_VALUE) {
        blog(LOG_ERROR, "[feeds] SendToEngine: P2E pipe not connected");
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(g_p2ePipe, jsonMessage.c_str(),
                        (DWORD)jsonMessage.size(), &written, NULL);
    if (!ok) {
        blog(LOG_ERROR, "[feeds] SendToEngine: WriteFile failed: %lu", GetLastError());
        return false;
    }

    // Redaction: log only the message type, never the payload. Outbound
    // join_meeting carries the meeting password and webinar_token. At DEBUG
    // so the normal OBS log isn't flooded with per-message traces.
    std::string type = ExtractJsonStringField(jsonMessage, "type");
    if (type.empty()) type = "(unknown)";
    blog(LOG_DEBUG, "[feeds] SendToEngine: sent %s", type.c_str());
    return true;
}

void RegisterMessageHandler(const std::string& messageType,
                            std::function<void(const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(g_handlersMutex);
    g_messageHandlers[messageType] = handler;
}

// ---------------------------------------------------------------------------
// Internal: Job Object, pipe servers, process launch
// ---------------------------------------------------------------------------

static bool CreateJobObject()
{
    g_jobObject = ::CreateJobObject(NULL, NULL);
    if (g_jobObject == NULL) {
        blog(LOG_ERROR, "[feeds] CreateJobObject failed: %lu", GetLastError());
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!SetInformationJobObject(g_jobObject, JobObjectExtendedLimitInformation,
                                  &info, sizeof(info))) {
        blog(LOG_ERROR, "[feeds] SetInformationJobObject failed: %lu", GetLastError());
        CloseHandle(g_jobObject);
        g_jobObject = NULL;
        return false;
    }

    return true;
}

static bool CreatePipeServers()
{
    // P2E: plugin writes, engine reads -> plugin side is OUTBOUND only
    g_p2ePipe = CreateNamedPipeW(
        P2E_PIPE_NAME,
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 4096, 4096, 0, NULL);

    if (g_p2ePipe == INVALID_HANDLE_VALUE) {
        blog(LOG_ERROR, "[feeds] CreateNamedPipe (P2E) failed: %lu", GetLastError());
        return false;
    }

    // E2P: engine writes, plugin reads -> plugin side is INBOUND only
    // FILE_FLAG_OVERLAPPED so we can cancel pending reads on shutdown.
    // Even though we use synchronous ReadFile, the overlapped flag allows
    // CancelIoEx to properly interrupt a blocked read on close.
    g_e2pPipe = CreateNamedPipeW(
        E2P_PIPE_NAME,
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 4096, 4096, 0, NULL);

    if (g_e2pPipe == INVALID_HANDLE_VALUE) {
        blog(LOG_ERROR, "[feeds] CreateNamedPipe (E2P) failed: %lu", GetLastError());
        CloseHandle(g_p2ePipe);
        g_p2ePipe = INVALID_HANDLE_VALUE;
        return false;
    }

    return true;
}

static bool WaitForEngineConnections()
{
    // Engine will connect to both pipes. Order matters: engine connects P2E first,
    // then E2P. We accept them in the same order here.
    BOOL ok = ConnectNamedPipe(g_p2ePipe, NULL);
    if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
        blog(LOG_ERROR, "[feeds] ConnectNamedPipe (P2E) failed: %lu", GetLastError());
        return false;
    }

    ok = ConnectNamedPipe(g_e2pPipe, NULL);
    if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
        blog(LOG_ERROR, "[feeds] ConnectNamedPipe (E2P) failed: %lu", GetLastError());
        return false;
    }

    return true;
}

// Full path of the module containing this code (feeds.dll), or empty on
// failure. The buffer grows until the name fits, so a long install path is
// never silently truncated (GetModuleFileNameW truncates and returns the
// buffer size when it is too small).
static std::wstring GetPluginModulePath()
{
    HMODULE hModule = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&GetPluginModulePath,
                            &hModule)) {
        return std::wstring();
    }

    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(hModule, &path[0], (DWORD)path.size());
        if (len == 0) return std::wstring();
        if (len < path.size()) {
            path.resize(len);
            return path;
        }
        // Truncated. 32767 is the longest path Windows supports.
        if (path.size() >= 32768) return std::wstring();
        path.resize(path.size() * 2);
    }
}

// Everything before the last backslash, or empty if there is none.
static std::wstring ParentDir(const std::wstring& path)
{
    size_t pos = path.find_last_of(L'\\');
    return (pos == std::wstring::npos) ? std::wstring() : path.substr(0, pos);
}

static bool IsExistingFile(const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool LaunchEngineProcess()
{
    std::wstring pluginPath = GetPluginModulePath();
    if (pluginPath.empty()) {
        blog(LOG_ERROR, "[feeds] LaunchEngineProcess: could not determine feeds.dll path: %lu",
             GetLastError());
        return false;
    }
    blog(LOG_INFO, "[feeds] LaunchEngineProcess: plugin at %ls", pluginPath.c_str());

    // FeedsEngine.exe is looked up relative to feeds.dll's own folder (D), so
    // the plugin finds its engine whichever layout it was installed in. The
    // parent folders are taken by trimming path components rather than by
    // appending "..", which a \\?\ path would not collapse. First match wins.
    const std::wstring dllDir = ParentDir(pluginPath);
    const std::wstring upTwo  = ParentDir(ParentDir(dllDir));
    struct Candidate {
        const wchar_t* label;
        const std::wstring& base;
        const wchar_t* tail;
    };
    const Candidate candidates[] = {
        // Engine beside the DLL: OBS 33 layout (<plugins>\feeds\feeds.dll).
        {L"D\\zoom-sdk",                     dllDir, L"\\zoom-sdk\\FeedsEngine.exe"},
        // DLL in <plugins>\feeds\bin\64bit (OBS 33 legacy ProgramData layout),
        // engine at the plugin folder's root.
        {L"D\\..\\..\\zoom-sdk",             upTwo,  L"\\zoom-sdk\\FeedsEngine.exe"},
        // Today's layout: DLL in <OBS>\obs-plugins\64bit, engine in
        // <OBS>\bin\64bit\zoom-sdk.
        {L"D\\..\\..\\bin\\64bit\\zoom-sdk", upTwo,  L"\\bin\\64bit\\zoom-sdk\\FeedsEngine.exe"},
    };

    std::wstring enginePath;
    int index = 0;
    for (const Candidate& c : candidates) {
        ++index;
        if (c.base.empty()) {
            blog(LOG_INFO, "[feeds] LaunchEngineProcess: candidate %d (%ls): skipped, no such parent folder",
                 index, c.label);
            continue;
        }
        std::wstring path = c.base + c.tail;
        bool found = IsExistingFile(path);
        blog(LOG_INFO, "[feeds] LaunchEngineProcess: candidate %d (%ls): %ls: %s",
             index, c.label, path.c_str(), found ? "found" : "not found");
        if (found) {
            enginePath = path;
            break;
        }
    }

    if (enginePath.empty()) {
        blog(LOG_ERROR, "[feeds] LaunchEngineProcess: FeedsEngine.exe not found in any candidate location");
        return false;
    }

    blog(LOG_INFO, "[feeds] LaunchEngineProcess: launching %ls", enginePath.c_str());

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessW(
        enginePath.c_str(), NULL, NULL, NULL, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW,
        NULL, NULL, &si, &pi);

    if (!ok) {
        blog(LOG_ERROR, "[feeds] CreateProcess failed: %lu", GetLastError());
        return false;
    }

    if (!AssignProcessToJobObject(g_jobObject, pi.hProcess)) {
        blog(LOG_ERROR, "[feeds] AssignProcessToJobObject failed: %lu", GetLastError());
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return false;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    g_engineProcess = pi.hProcess;
    blog(LOG_INFO, "[feeds] LaunchEngineProcess: engine launched, pid=%lu", pi.dwProcessId);

    return true;
}

// ---------------------------------------------------------------------------
// Internal: pipe reader and message dispatch
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

static void DispatchMessage(const std::string& json)
{
    std::string type = ExtractJsonStringField(json, "type");
    if (type.empty()) {
        // Redaction: never log the raw payload — a malformed message can
        // still carry secrets.
        blog(LOG_WARNING, "[feeds] DispatchMessage: could not extract type from message");
        return;
    }

    std::function<void(const std::string&)> handler;
    {
        std::lock_guard<std::mutex> lock(g_handlersMutex);
        auto it = g_messageHandlers.find(type);
        if (it != g_messageHandlers.end()) {
            handler = it->second;
        }
    }

    if (handler) {
        handler(json);
    } else {
        // Redaction: log only the type, never the payload.
        blog(LOG_INFO, "[feeds] DispatchMessage: no handler for type '%s'",
             type.c_str());
    }
}

static void PipeReaderThread()
{
    blog(LOG_INFO, "[feeds] PipeReaderThread: started");

    char buffer[4096];
    // Accumulates one message across reads. The E2P pipe is message-mode, so a
    // message larger than `buffer` comes back as one or more ReadFile failures
    // with GetLastError() == ERROR_MORE_DATA (each filling `buffer` with the next
    // slice), terminated by a successful read carrying the final slice. Those
    // partials must be concatenated, not treated as fatal — otherwise a large
    // roster (50+ participant webinar) or a large Events payload would kill the
    // reader and silently stop ALL engine->plugin traffic for the session. A
    // message that fits in one read leaves this empty until that read completes.
    //
    // The concatenation itself is AccumulateE2PMessageSlice (feeds-ipc-reassembly.h)
    // — a pure function unit-tested against fabricated slices and a real named-pipe
    // loopback in tests/test_e2p_reader.cpp. Both ReadFile branches feed it: the
    // ERROR_MORE_DATA branch with moreData=true (partial), the success branch with
    // moreData=false (completing). `completed` receives a whole message on the
    // completing read.
    std::string message;
    while (!g_shutdownRequested) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(g_e2pPipe, buffer, sizeof(buffer), &bytesRead, NULL);

        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_MORE_DATA) {
                // Partial message: this read filled `buffer`. Accumulate and loop;
                // the message completes on the next read that returns TRUE. Handles
                // a message several buffers long (multiple ERROR_MORE_DATA reads).
                std::string unused;
                AccumulateE2PMessageSlice(message, buffer, bytesRead, true, unused);
                continue;
            }
            // ERROR_OPERATION_ABORTED (995) is expected during shutdown,
            // triggered by CancelIoEx. ERROR_BROKEN_PIPE (109) is expected
            // when the engine exits. Anything else is a real error.
            if (err != ERROR_BROKEN_PIPE &&
                err != ERROR_OPERATION_ABORTED &&
                !g_shutdownRequested) {
                blog(LOG_ERROR, "[feeds] PipeReaderThread: ReadFile failed: %lu", err);
            }
            break;
        }

        // Successful read: this slice completes the message (whole, if it fit in
        // one read; or the tail of an ERROR_MORE_DATA run).
        std::string completed;
        if (AccumulateE2PMessageSlice(message, buffer, bytesRead, false, completed)) {
            const std::string& json = completed;
            // Redaction: log only the message type, never the payload.
            // Inbound login_succeeded carries the user's name and PMI, and
            // forwarded engine "log" messages are re-emitted by their own
            // handler — so this raw line would both leak and duplicate. At
            // DEBUG so the normal OBS log isn't flooded.
            std::string rtype = ExtractJsonStringField(json, "type");
            if (rtype.empty()) rtype = "(unknown)";
            blog(LOG_DEBUG, "[feeds] PipeReaderThread: received: %s", rtype.c_str());
            DispatchMessage(json);
        }
    }

    blog(LOG_INFO, "[feeds] PipeReaderThread: exiting");
}

} // namespace feeds
