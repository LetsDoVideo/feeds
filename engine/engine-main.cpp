#include <windows.h>

// FeedsEngine.exe — subprocess that will eventually host the Zoom Meeting SDK.
// For Phase 1, this is just a shell to verify the build pipeline produces a
// second signed executable alongside feeds.dll.

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    MessageBoxA(NULL,
                "FeedsEngine.exe started.\n\nThis is a Phase 1 placeholder.",
                "Feeds Engine",
                MB_OK);
    return 0;
}
