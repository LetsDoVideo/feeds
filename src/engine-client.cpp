// engine-client.cpp — manages the FeedsEngine.exe subprocess and IPC with it.
//
// The plugin creates a named pipe server, launches FeedsEngine.exe into a
// Job Object (so it gets killed if OBS crashes), waits for the engine to
// connect to the pipe, then exchanges messages.

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <obs-module.h>

namespace feeds {

static HANDLE g_jobObject = NULL;
static HANDLE g_engineProcess = NULL;
static HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;
static std::thread g_pipeReaderThread;
static std::atomic<bool> g_shutdownRequested{false};

static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\FeedsEngine";

// Forward declarations
static bool CreatePipeServer();
static bool CreateJobObject();
static bool LaunchEngineProcess();
static void PipeReaderThread();

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
        // Already connected - that's fine
        connected = TRUE;
    } else if (!connected && err == ERROR_IO_PENDING) {
        // Async connect pending - wait for it
        DWORD waitResult = WaitForSingleObject(ov.hEvent, 10000);
        connected = (waitResult == WAIT_OBJECT_0);
    }

    CloseHandle(ov.hEvent);

    if (!connected) {
        blog(LOG_ERROR, "[feeds] StartEngine: engine did not connect to pipe within 10 seconds");
        return false;
    }

    blog(LOG_INFO, "[feeds] StartEngine: engine connected to pipe");

    // Start background thread to read messages from engine
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

    // Closing the Job Object handle kills the engine process
    // (because of JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
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
        1,          // Max instances
        4096,       // Out buffer size
        4096,       // In buffer size
        0,          // Default timeout
        NULL        // Default security
    );

    if (g_pipeHandle == INVALID_HANDLE_VALUE) {
        blog(LOG_ERROR, "[feeds] CreateNamedPipe failed: %lu", GetLastError());
        return false;
    }

    return true;
}

static bool LaunchEngineProcess()
{
    // Find FeedsEngine.exe relative to the plugin DLL
    HMODULE hModule = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&LaunchEngineProcess,
                       &hModule);

    wchar_t pluginPath[MAX_PATH];
    GetModuleFileNameW(hModule, pluginPath, MAX_PATH);

    // Plugin is at: <obs>\obs-plugins\64bit\feeds.dll
    // Engine is at: <obs>\bin\64bit\FeedsEngine.exe
    // So we need to go up two dirs from the plugin, then into bin\64bit
    wchar_t enginePath[MAX_PATH];
    wcscpy_s(enginePath, pluginPath);
    // Strip feeds.dll
    wchar_t* lastSlash = wcsrchr(enginePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    // Strip 64bit
    lastSlash = wcsrchr(enginePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    // Strip obs-plugins
    lastSlash = wcsrchr(enginePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    // Append bin\64bit\FeedsEngine.exe
    wcscat_s(enginePath, L"\\bin\\64bit\\FeedsEngine.exe");

    blog(LOG_INFO, "[feeds] LaunchEngineProcess: launching %ls", enginePath);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Launch suspended so we can assign to Job Object before it runs
    BOOL ok = CreateProcessW(
        enginePath, NULL, NULL, NULL, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW,
        NULL, NULL, &si, &pi
    );

    if (!ok) {
        blog(LOG_ERROR, "[feeds] CreateProcess failed: %lu", GetLastError());
        return false;
    }

    // Assign to Job Object (so it dies if we die)
    if (!AssignProcessToJobObject(g_jobObject, pi.hProcess)) {
        blog(LOG_ERROR, "[feeds] AssignProcessToJobObject failed: %lu", GetLastError());
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return false;
    }

    // Now resume the process
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    g_engineProcess = pi.hProcess;
    blog(LOG_INFO, "[feeds] LaunchEngineProcess: engine launched, pid=%lu", pi.dwProcessId);

    return true;
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
            blog(LOG_INFO, "[feeds] PipeReaderThread: received message: %s", buffer);
        }
    }

    blog(LOG_INFO, "[feeds] PipeReaderThread: exiting");
}

} // namespace feeds
