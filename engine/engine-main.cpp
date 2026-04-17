// FeedsEngine.exe — subprocess that hosts the Zoom Meeting SDK.
//
// Launched by feeds.dll. Connects to the named pipe created by the plugin,
// sends engine_ready, then waits for commands.

#include <windows.h>
#include <string>
#include <cstdio>

static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\FeedsEngine";

static HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;

// Simple log to a file next to the exe, so we can see what's happening
// even though we're a windowless subprocess.
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

static bool ConnectToPipe()
{
    // The plugin creates the pipe then launches us, so the pipe should
    // already exist. But add a short retry loop just in case of timing.
    for (int i = 0; i < 20; i++) {
        g_pipeHandle = CreateFileW(
            PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0, NULL,
            OPEN_EXISTING,
            0, NULL
        );

        if (g_pipeHandle != INVALID_HANDLE_VALUE) {
            LogToFile("Connected to pipe");
            
            // Set to message mode
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

static bool SendMessage(const char* json)
{
    DWORD written = 0;
    BOOL ok = WriteFile(g_pipeHandle, json, (DWORD)strlen(json), &written, NULL);
    if (!ok) {
        char msg[256];
        sprintf_s(msg, "WriteFile failed: %lu", GetLastError());
        LogToFile(msg);
        return false;
    }
    return true;
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
            char msg[4200];
            sprintf_s(msg, "Received message: %s", buffer);
            LogToFile(msg);
        }
    }
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    LogToFile("========================================");
    LogToFile("FeedsEngine.exe starting");

    if (!ConnectToPipe()) {
        LogToFile("Could not connect to pipe, exiting");
        return 1;
    }

    // Send engine_ready
    const char* readyMsg = "{\"type\":\"engine_ready\",\"version\":\"1.0.0\"}";
    if (!SendMessage(readyMsg)) {
        LogToFile("Failed to send engine_ready");
        return 1;
    }
    LogToFile("Sent engine_ready");

    // Now loop reading messages from plugin until pipe closes
    PipeReaderLoop();

    LogToFile("FeedsEngine.exe exiting normally");
    return 0;
}
