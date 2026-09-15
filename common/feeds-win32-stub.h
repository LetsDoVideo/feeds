// feeds-win32-stub.h — NON-WINDOWS builds only (the macOS plugin shell).
//
// The plugin was written against Win32: WinHTTP for the update check and the
// YouTube / Twitch chat readers, Win32 auto-reset events for thread wakeups,
// and named file mappings for the engine's shared-memory frames. Rather than
// fork ~10k lines of plugin-main.cpp with #ifdefs, a non-Windows build includes
// this header in place of <windows.h> / <winhttp.h>. It declares exactly the
// Win32 surface the plugin uses, with these behaviors:
//
//   * Networking (WinHttp*): every call FAILS (null handle / FALSE / an error
//     code). Each caller already treats that as "request failed", so the update
//     check finds nothing and the chat readers stay disconnected. Not available
//     on this platform yet.
//   * Shared memory (OpenFileMappingA / MapViewOfFile): always fails. There is
//     no engine on this platform, so no region ever exists to map.
//   * Events (CreateEventW / SetEvent / WaitForSingleObject / CloseHandle):
//     REAL, implemented with std::mutex + std::condition_variable. The chat and
//     pump threads block on these and are woken by SetEvent at shutdown; a
//     fake would either spin a CPU or hang OBS's exit.
//   * Time (Sleep / GetTickCount / GetTickCount64): real.
//
// This is a bridge for the shell milestone, not the Mac port. The port replaces
// each group with a native implementation (shared memory with POSIX shm,
// networking with a real HTTP/WebSocket client) and this header shrinks away.
// The Windows build never includes it.

#pragma once

#ifdef _WIN32
#error "feeds-win32-stub.h is for non-Windows builds; include <windows.h> instead"
#endif

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

// ---------------------------------------------------------------------------
// Types and constants
// ---------------------------------------------------------------------------
typedef void*          HANDLE;
typedef void*          HINTERNET;
typedef void*          LPVOID;
typedef void*          PVOID;
typedef unsigned long  DWORD;
typedef DWORD*         LPDWORD;
typedef uintptr_t      DWORD_PTR;
typedef int            BOOL;
typedef unsigned char  BYTE;
typedef unsigned short INTERNET_PORT;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define INFINITE             0xFFFFFFFFul
#define WAIT_OBJECT_0        0ul
#define WAIT_TIMEOUT         258ul
#define NO_ERROR             0ul
#define ERROR_NOT_SUPPORTED  50ul
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define CP_UTF8              65001u
#define FILE_MAP_WRITE       0x0002ul
#define FILE_MAP_READ        0x0004ul

// WinHTTP constants. The values only need to be distinct and type-correct:
// nothing reaches a real WinHTTP stack on this platform.
#define INTERNET_DEFAULT_HTTPS_PORT            ((INTERNET_PORT)443)
#define WINHTTP_ACCESS_TYPE_DEFAULT_PROXY      0ul
#define WINHTTP_ACCESS_TYPE_NO_PROXY           1ul
#define WINHTTP_ACCESS_TYPE_NAMED_PROXY        3ul
#define WINHTTP_NO_PROXY_NAME                  ((const wchar_t*)nullptr)
#define WINHTTP_NO_PROXY_BYPASS                ((const wchar_t*)nullptr)
#define WINHTTP_NO_REFERER                     ((const wchar_t*)nullptr)
#define WINHTTP_DEFAULT_ACCEPT_TYPES           ((const wchar_t**)nullptr)
#define WINHTTP_NO_ADDITIONAL_HEADERS          ((const wchar_t*)nullptr)
#define WINHTTP_NO_REQUEST_DATA                ((LPVOID)nullptr)
#define WINHTTP_HEADER_NAME_BY_INDEX           ((const wchar_t*)nullptr)
#define WINHTTP_NO_HEADER_INDEX                ((LPDWORD)nullptr)
#define WINHTTP_FLAG_SECURE                    0x00800000ul
#define WINHTTP_ADDREQ_FLAG_ADD                0x20000000ul
#define WINHTTP_QUERY_STATUS_CODE              19ul
#define WINHTTP_QUERY_FLAG_NUMBER              0x20000000ul
#define WINHTTP_OPTION_AUTOLOGON_POLICY        77ul
#define WINHTTP_OPTION_SECURE_PROTOCOLS        84ul
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET   114ul
#define WINHTTP_AUTOLOGON_SECURITY_LEVEL_LOW   0ul
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2    0x00000800ul
#define WINHTTP_AUTOPROXY_AUTO_DETECT          0x00000001ul
#define WINHTTP_AUTOPROXY_CONFIG_URL           0x00000002ul
#define WINHTTP_AUTO_DETECT_TYPE_DHCP          0x00000001ul
#define WINHTTP_AUTO_DETECT_TYPE_DNS_A         0x00000002ul
#define ERROR_WINHTTP_LOGIN_FAILURE            12015ul
#define ERROR_WINHTTP_AUTODETECTION_FAILED     12180ul

struct WINHTTP_CURRENT_USER_IE_PROXY_CONFIG {
    BOOL     fAutoDetect;
    wchar_t* lpszAutoConfigUrl;
    wchar_t* lpszProxy;
    wchar_t* lpszProxyBypass;
};

struct WINHTTP_AUTOPROXY_OPTIONS {
    DWORD          dwFlags;
    DWORD          dwAutoDetectFlags;
    const wchar_t* lpszAutoConfigUrl;
    LPVOID         lpvReserved;
    DWORD          dwReserved;
    BOOL           fAutoLogonIfChallenged;
};

struct WINHTTP_PROXY_INFO {
    DWORD    dwAccessType;
    wchar_t* lpszProxy;
    wchar_t* lpszProxyBypass;
};

enum WINHTTP_WEB_SOCKET_BUFFER_TYPE {
    WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE  = 0,
    WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE = 1,
    WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE    = 2,
    WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE   = 3,
    WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE           = 4
};

// ---------------------------------------------------------------------------
// Kernel: errors, time, memory barrier
// ---------------------------------------------------------------------------
inline DWORD GetLastError() { return ERROR_NOT_SUPPORTED; }

inline void Sleep(DWORD ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline uint64_t GetTickCount64()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline DWORD GetTickCount() { return (DWORD)(GetTickCount64() & 0xFFFFFFFFull); }

inline void MemoryBarrier() {}

// ---------------------------------------------------------------------------
// Events — a real implementation (see the header note for why).
// ---------------------------------------------------------------------------
namespace feeds_win32_stub {

constexpr uint32_t kEventMagic = 0x46454556u;  // 'FEEV'

struct Event {
    uint32_t                magic    = kEventMagic;
    bool                    manual   = false;
    bool                    signaled = false;
    std::mutex              m;
    std::condition_variable cv;
};

// Every non-null HANDLE this header can hand out is an Event (file mappings and
// WinHTTP always fail), so the magic check is a guard, not a type system.
inline Event* AsEvent(HANDLE h)
{
    Event* e = static_cast<Event*>(h);
    return (e && h != INVALID_HANDLE_VALUE && e->magic == kEventMagic) ? e : nullptr;
}

}  // namespace feeds_win32_stub

inline HANDLE CreateEventW(void* /*securityAttributes*/, BOOL manualReset,
                           BOOL initialState, const wchar_t* /*name*/)
{
    auto* e     = new feeds_win32_stub::Event();
    e->manual   = (manualReset != FALSE);
    e->signaled = (initialState != FALSE);
    return e;
}

inline BOOL SetEvent(HANDLE h)
{
    feeds_win32_stub::Event* e = feeds_win32_stub::AsEvent(h);
    if (!e) return FALSE;
    {
        std::lock_guard<std::mutex> lk(e->m);
        e->signaled = true;
    }
    if (e->manual) e->cv.notify_all();
    else           e->cv.notify_one();
    return TRUE;
}

inline DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    feeds_win32_stub::Event* e = feeds_win32_stub::AsEvent(h);
    if (!e) {
        // Not an event (never expected here). Never spin: bound the wait.
        Sleep(ms == INFINITE ? 1000ul : ms);
        return WAIT_TIMEOUT;
    }
    std::unique_lock<std::mutex> lk(e->m);
    auto signaled = [e] { return e->signaled; };
    if (ms == INFINITE) {
        e->cv.wait(lk, signaled);
    } else if (!e->cv.wait_for(lk, std::chrono::milliseconds(ms), signaled)) {
        return WAIT_TIMEOUT;
    }
    if (!e->manual) e->signaled = false;  // auto-reset
    return WAIT_OBJECT_0;
}

// Callers join their waiting thread before closing the event (as on Windows).
inline BOOL CloseHandle(HANDLE h)
{
    if (feeds_win32_stub::Event* e = feeds_win32_stub::AsEvent(h)) {
        e->magic = 0;
        delete e;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Shared memory — always unavailable (no engine on this platform).
// ---------------------------------------------------------------------------
inline HANDLE OpenFileMappingA(DWORD, BOOL, const char*) { return nullptr; }
inline LPVOID MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, size_t) { return nullptr; }
inline BOOL   UnmapViewOfFile(const void*) { return TRUE; }

// ---------------------------------------------------------------------------
// Strings / memory used by feeds-http.h
// ---------------------------------------------------------------------------
inline LPVOID GlobalFree(LPVOID) { return nullptr; }

// ASCII-only narrowing. feeds-http.h calls it only to log a resolved proxy
// name, and the stubbed resolver below never produces one.
inline int WideCharToMultiByte(unsigned, DWORD, const wchar_t* src, int srcLen,
                               char* dst, int dstLen, const char*, BOOL*)
{
    if (!src || srcLen <= 0) return 0;
    if (!dst || dstLen <= 0) return srcLen;
    int n = srcLen < dstLen ? srcLen : dstLen;
    for (int i = 0; i < n; ++i) dst[i] = (char)src[i];
    return n;
}

// ---------------------------------------------------------------------------
// WinHTTP — every operation fails.
// ---------------------------------------------------------------------------
inline HINTERNET WinHttpOpen(const wchar_t*, DWORD, const wchar_t*, const wchar_t*, DWORD) { return nullptr; }
inline BOOL WinHttpCloseHandle(HINTERNET) { return TRUE; }
inline BOOL WinHttpSetTimeouts(HINTERNET, int, int, int, int) { return FALSE; }
inline BOOL WinHttpSetOption(HINTERNET, DWORD, LPVOID, DWORD) { return FALSE; }
inline HINTERNET WinHttpConnect(HINTERNET, const wchar_t*, INTERNET_PORT, DWORD) { return nullptr; }
inline HINTERNET WinHttpOpenRequest(HINTERNET, const wchar_t*, const wchar_t*, const wchar_t*,
                                    const wchar_t*, const wchar_t**, DWORD) { return nullptr; }
inline BOOL WinHttpAddRequestHeaders(HINTERNET, const wchar_t*, DWORD, DWORD) { return FALSE; }
inline BOOL WinHttpSendRequest(HINTERNET, const wchar_t*, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR) { return FALSE; }
inline BOOL WinHttpReceiveResponse(HINTERNET, LPVOID) { return FALSE; }
inline BOOL WinHttpQueryHeaders(HINTERNET, DWORD, const wchar_t*, LPVOID, LPDWORD, LPDWORD) { return FALSE; }
inline BOOL WinHttpReadData(HINTERNET, LPVOID, DWORD, LPDWORD read)
{
    if (read) *read = 0;
    return FALSE;
}
inline BOOL WinHttpGetIEProxyConfigForCurrentUser(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG*) { return FALSE; }
inline BOOL WinHttpGetProxyForUrl(HINTERNET, const wchar_t*, WINHTTP_AUTOPROXY_OPTIONS*, WINHTTP_PROXY_INFO*) { return FALSE; }
inline HINTERNET WinHttpWebSocketCompleteUpgrade(HINTERNET, DWORD_PTR) { return nullptr; }
inline DWORD WinHttpWebSocketSend(HINTERNET, WINHTTP_WEB_SOCKET_BUFFER_TYPE, PVOID, DWORD) { return ERROR_NOT_SUPPORTED; }
inline DWORD WinHttpWebSocketReceive(HINTERNET, PVOID, DWORD, LPDWORD read, WINHTTP_WEB_SOCKET_BUFFER_TYPE*)
{
    if (read) *read = 0;
    return ERROR_NOT_SUPPORTED;
}
