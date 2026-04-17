// FeedsEngine.exe — subprocess that hosts the Zoom Meeting SDK.
//
// Launched by feeds.dll. Connects to the named pipe created by the plugin,
// sends engine_ready, then processes incoming command messages.

#include <windows.h>
#include <string>
#include <cstdio>
#include <map>
#include <functional>

static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\FeedsEngine";

static HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;
static std::map<std::string, std::function<void(const std::string&)>> g_messageHandlers;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static void LogToFile(const char* msg)
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

static bool ConnectToPipe()
{
    for (int i = 0; i < 20; i++) {
        g_pipeHandle = CreateFileW(
            PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING, 0, NULL
        );

        if (g_pipeHandle != INVALID_HANDLE_VALUE) {
            LogToFile("Connected to pipe");
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(g_pipeHandle, &mode, NULL, NULL);
            return true;
        }

        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(PIPE_NAME, 500);
        } else {
            char msg[256];
            sprintf_s(msg, "CreateFile on pipe failed: %lu", err);
            LogToFile(msg);
            Sleep(250);
        }
    }

    LogToFile("Failed to connect to pipe after 20 attempts");
    return false;
}

static bool SendToPlugin(const std::string& json)
{
    DWORD written = 0;
    BOOL ok = WriteFile(g_pipeHandle, json.c_str(), (DWORD)json.size(), &written, NULL);
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

static void DispatchMessage(const std::string& json)
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
        BOOL ok = ReadFile(g_pipeHandle, buffer, sizeof(buffer) - 1, &bytesRead, NULL);

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

            DispatchMessage(json);
        }
    }
}

// ---------------------------------------------------------------------------
// Message handlers (stubs for now — will be filled in as phases progress)
// ---------------------------------------------------------------------------

static void HandleLoginStart(const std::string& json)
{
    LogToFile("HandleLoginStart: received login_start message (stub)");
    // Phase 3 step 3 will do the actual OAuth here.
    // For now, just acknowledge to prove the round-trip works.
    SendToPlugin("{\"type\":\"login_failed\",\"error\":\"not_implemented_yet\"}");
}

static void HandleShutdown(const std::string& json)
{
    LogToFile("HandleShutdown: received shutdown message");
    SendToPlugin("{\"type\":\"shutdown_complete\"}");
    // Pipe will close when plugin closes its handle; reader loop will exit.
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    LogToFile("========================================");
    LogToFile("FeedsEngine.exe starting");

    if (!ConnectToPipe()) {
        LogToFile("Could not connect to pipe, exiting");
        return 1;
    }

    // Register handlers for messages we expect from the plugin
    RegisterHandler("login_start", HandleLoginStart);
    RegisterHandler("shutdown", HandleShutdown);

    // Announce we're ready
    if (!SendToPlugin("{\"type\":\"engine_ready\",\"version\":\"1.0.0\"}")) {
        LogToFile("Failed to send engine_ready");
        return 1;
    }

    // Process messages until pipe closes
    PipeReaderLoop();

    LogToFile("FeedsEngine.exe exiting normally");
    return 0;
}
