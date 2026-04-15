#include <windows.h>
#include <string>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2) return 1;

    std::wstring arg(argv[1]);
    LocalFree(argv);

    // Extract code from ldvfeeds://callback?code=XXX
    int len = WideCharToMultiByte(CP_UTF8, 0, arg.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string full(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, arg.c_str(), -1, &full[0], len, nullptr, nullptr);
    std::string code;
    size_t pos = full.find("code=");
    if (pos == std::string::npos) return 1;
    code = full.substr(pos + 5);
    pos = code.find('&');
    if (pos != std::string::npos) code = code.substr(0, pos);
    // Strip any trailing whitespace, CR, LF, or null characters
while (!code.empty() && (code.back() == '\r' || code.back() == '\n' 
       || code.back() == ' ' || code.back() == '\0' || code.back() == '#'))
    code.pop_back();

    // Send code to named pipe
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 10; i++) {
        pipe = CreateFileA("\\\\.\\pipe\\FeedsAuth",
            GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        Sleep(500);
    }
    if (pipe == INVALID_HANDLE_VALUE) return 1;

    DWORD written = 0;
    WriteFile(pipe, code.c_str(), (DWORD)code.size(), &written, nullptr);
    CloseHandle(pipe);
    return 0;
}
