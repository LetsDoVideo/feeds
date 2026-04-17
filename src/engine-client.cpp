// engine-client.cpp — manages the FeedsEngine.exe subprocess and IPC with it.
//
// The plugin creates a named pipe server, launches FeedsEngine.exe into a
// Job Object (so it gets killed if OBS crashes), waits for the engine to
// connect to the pipe, then exchanges messages.

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <map>
#include <functional>
#include <mutex>
#include <obs-module.h>

namespace feeds {

static HANDLE g_jobObject = NULL;
static HANDLE g_engineProcess = NULL;
static HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;
static std::thread g_pipeReaderThread;
static std::atomic<bool> g_shutdownRequested{false};

static std::mutex g_handlersMutex;
static std::map<std::string, std::function<void(const std::string&)>> g_messageHandlers;

static std::mutex g_writeMutex;

static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\FeedsEngine";

// Forward declarations
static bool CreatePipeServer();
static bool CreateJobObject();
static bool LaunchEngineProcess();
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

    if (!CreatePipeServer()) {
        blog(LOG_ERROR, "[feeds] StartEngine: failed to create pipe server");
        return false;
    }

    if (!LaunchEngineProcess()) {
        blog(LOG_ERROR, "[feeds] StartEngine: failed to launch engine process");
        return false;
    }

    // Wait for engine to connect to pipe (blocking up to 10 seconds)
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    BOOL connected = ConnectNamedPipe(g_pipeHandle, &ov);
    DWORD err = GetLastError();

    if (!connected && err == ERROR_PIPE_CONNECTED) {
        connected = TRUE;
    } else if (!connected && err == ERROR_IO_PENDING) {
        DWORD waitResult = WaitForSingleObject(ov.hEvent, 10000);
        connected = (waitResult == WAIT_OBJECT_0);
    }

    CloseHandle(ov.hEvent);

    if (!connected) {
        blog(LOG_ERROR, "[feeds] StartEngine: engine did not connect to pipe within 10 seconds");
        return false;
    }

    blog(LOG_INFO, "[feeds] StartEngine: engine connected to pipe");

    g_shutdownRequested = false;
    g_pipeReaderThread = std::thread(PipeReaderThread);

    return true;
}

void StopEngine()
{
    blog(LOG_INFO, "[feeds] StopEngine: shutting down");

    g_shutdownRequested = true;

    if (g_pipeHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipeHandle);
        g_pipeHandle = INVALID_HANDLE_VALUE;
    }

    if (g_pipeReaderThread.joinable()) {
        g_pipeReaderThread.join();
    }

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

    if (g_pipeHandle == INVALID_HANDLE_VALUE) {
        blog(LOG_ERROR, "[feeds] SendToEngine: pipe not connected");
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(g_pipeHandle, jsonMessage.c_str(),
                        (DWORD)jsonMessage.size(), &written, NULL);
    if (!ok) {
        blog(LOG_ERROR, "[feeds] SendToEngine: WriteFile failed: %lu", GetLastError());
        return false;
    }

    blog(LOG_INFO, "[feeds] SendToEngine: sent %s", jsonMessage.c_str());
    return true;
}

void RegisterMessageHandler(const std::string& messageType,
                            std::function<void(const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(g_handlersMutex);
    g_messageHandlers[messageType] = handler;
}

// ---------------------------------------------------------------------------
// Internal: Job Object, pipe server, process launch
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

static bool CreatePipeServer()
{
    g_pipeHandle = CreateNamedPipeW(
        PIPE_NAME,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 4096, 4096, 0, NULL
    );

    if (g_pipeHandle == INVALID_HANDLE_VALUE) {
        blog(LOG_ERROR, "[feeds] CreateNamedPipe failed: %lu", GetLastError());
        return false;
    }

    return true;
}

static bool LaunchEngineProcess()
{
    HMODULE hModule = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&LaunchEngineProcess,
                       &hModule);

    wchar_t pluginPath[MAX_PATH];
    GetModuleFileNameW(hModule, pluginPath, MAX_PATH);

    wchar_t enginePath[MAX_PATH];
    wcscpy_s(enginePath, pluginPath);
    wchar_t* lastSlash = wcsrchr(enginePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    lastSlash = wcsrchr(enginePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    lastSlash = wcsrchr(enginePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    wcscat_s(enginePath, L"\\bin\\64bit\\FeedsEngine.exe");

    blog(LOG_INFO, "[feeds] LaunchEngineProcess: launching %ls", enginePath);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessW(
        enginePath, NULL, NULL, NULL, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW,
        NULL, NULL, &si, &pi
    );

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

// Minimal JSON string field extractor. Extracts the value of a string-typed
// field from a flat JSON object. Returns empty string on failure. This is
// intentionally naive — full JSON parsing happens on the plugin side only
// if/when we need it. For now, reading "type" is enough to dispatch messages.
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
        blog(LOG_WARNING, "[feeds] DispatchMessage: could not extract type from: %s", json.c_str());
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
        blog(LOG_INFO, "[feeds] DispatchMessage: no handler for type '%s' (message: %s)",
             type.c_str(), json.c_str());
    }
}

static void PipeReaderThread()
{
    blog(LOG_INFO, "[feeds] PipeReaderThread: started");

    char buffer[4096];
    while (!g_shutdownRequested) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(g_pipeHandle, buffer, sizeof(buffer) - 1, &bytesRead, NULL);

        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_BROKEN_PIPE && !g_shutdownRequested) {
                blog(LOG_ERROR, "[feeds] PipeReaderThread: ReadFile failed: %lu", err);
            }
            break;
        }

        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
            std::string json(buffer, bytesRead);
            blog(LOG_INFO, "[feeds] PipeReaderThread: received: %s", json.c_str());
            DispatchMessage(json);
        }
    }

    blog(LOG_INFO, "[feeds] PipeReaderThread: exiting");
}

} // namespace feeds
