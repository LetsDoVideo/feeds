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
//   * Shared memory (OpenFileMappingA / MapViewOfFile / UnmapViewOfFile):
//     REAL, implemented with POSIX shm_open + mmap. The engine writes frames
//     into a named shm object and the plugin's pump threads read them, so a
//     stub here means a permanently black participant source.
//   * Memory barrier (MemoryBarrier): REAL. It orders the pump thread's read
//     of the ring's write_index against its read of the frame slot, and on
//     arm64 that ordering is not free.
//   * Events (CreateEventW / SetEvent / WaitForSingleObject / CloseHandle):
//     REAL, implemented with std::mutex + std::condition_variable. The chat and
//     pump threads block on these and are woken by SetEvent at shutdown; a
//     fake would either spin a CPU or hang OBS's exit.
//   * Time (Sleep / GetTickCount / GetTickCount64): real.
//
// This is a bridge, not the Mac port: each group is replaced with a native
// implementation as that part of the port lands (shared memory now; networking
// still to come) and this header shrinks away. The Windows build never
// includes it.

#pragma once

#ifdef _WIN32
#error "feeds-win32-stub.h is for non-Windows builds; include <windows.h> instead"
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

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
typedef unsigned char  BYTE;
typedef unsigned short INTERNET_PORT;

// BOOL is a platform type on Apple: <objc/objc.h> defines it (signed char on
// x86_64, bool on arm64), and OBS's own graphics/graphics.h already pulls that
// in through obs-module.h, so re-typedef'ing it collides. Include it explicitly
// so this header doesn't depend on include order. Everything here uses BOOL
// only as a boolean (FALSE / TRUE / != FALSE), which all three types handle.
// Elsewhere (no Objective-C runtime) there is no BOOL, so define the Win32 one.
#ifdef __APPLE__
#include <objc/objc.h>
#else
typedef int BOOL;
#endif

// TRUE / FALSE may already exist (e.g. <mach/boolean.h> on macOS, as 1 / 0).
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

// Callers log GetLastError() after a failed call, so it has to carry something
// worth logging. The shared-memory functions below record their errno here;
// everything else in this header fails for the one structural reason
// ERROR_NOT_SUPPORTED already states. Per-thread, like the Win32 original.
namespace feeds_win32_stub {
inline DWORD& LastErrorSlot()
{
    static thread_local DWORD value = ERROR_NOT_SUPPORTED;
    return value;
}
inline void SetLastErrorFromErrno() { LastErrorSlot() = (DWORD)errno; }
}  // namespace feeds_win32_stub

inline DWORD GetLastError() { return feeds_win32_stub::LastErrorSlot(); }

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

// Real, not a no-op. The frame ring's whole correctness argument is "the reader
// that sees the new write_index is guaranteed to see the new slot contents",
// and on arm64 — every Apple Silicon Mac — nothing enforces that ordering for
// free. An empty body here would compile and then tear frames under load.
inline void MemoryBarrier()
{
    std::atomic_thread_fence(std::memory_order_acq_rel);
}

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

// A HANDLE from this header is either an Event or a Mapping, and CloseHandle
// takes both, so each carries its own magic as the discriminator. The first
// word of both structs is the magic, which is what makes the probe below safe
// to run against either.
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

// ---------------------------------------------------------------------------
// Shared memory — POSIX shm, read side.
//
// The plugin only ever OPENS regions; the engine creates and sizes them. So
// these three are deliberately open-only: there is no CreateFileMapping here
// and nothing in this header calls ftruncate.
//
// Mapping the region read-WRITE when FILE_MAP_WRITE is asked for is not
// incidental. The participant pump thread writes last_read_index back into the
// shared header, so a read-only view would fault on the first frame it
// delivered. The screenshare source asks for FILE_MAP_READ only and gets a
// read-only view, matching what it does on Windows.
//
// UnmapViewOfFile takes only a pointer, while munmap also needs the length, so
// each live mapping's length is recorded when it is made. The map is keyed by
// the mapped address, which is unique for as long as the mapping exists.
// ---------------------------------------------------------------------------
namespace feeds_win32_stub {

constexpr uint32_t kMappingMagic = 0x4645454du;  // 'FEEM'

struct Mapping {
    uint32_t    magic = kMappingMagic;
    int         fd    = -1;
    std::string name;
};

inline Mapping* AsMapping(HANDLE h)
{
    Mapping* m = static_cast<Mapping*>(h);
    return (m && h != INVALID_HANDLE_VALUE && m->magic == kMappingMagic) ? m
                                                                        : nullptr;
}

inline std::mutex& ViewMutex()
{
    static std::mutex m;
    return m;
}
inline std::map<const void*, size_t>& ViewSizes()
{
    static std::map<const void*, size_t> sizes;
    return sizes;
}

}  // namespace feeds_win32_stub

inline HANDLE OpenFileMappingA(DWORD desiredAccess, BOOL /*inheritHandle*/,
                               const char* name)
{
    if (!name) return nullptr;

    const int flags = (desiredAccess & FILE_MAP_WRITE) ? O_RDWR : O_RDONLY;
    const int fd    = shm_open(name, flags, 0600);
    if (fd < 0) {
        feeds_win32_stub::SetLastErrorFromErrno();
        return nullptr;
    }

    auto* m = new feeds_win32_stub::Mapping();
    m->fd   = fd;
    m->name = name;
    return m;
}

inline LPVOID MapViewOfFile(HANDLE h, DWORD desiredAccess, DWORD /*offsetHigh*/,
                            DWORD /*offsetLow*/, size_t size)
{
    feeds_win32_stub::Mapping* m = feeds_win32_stub::AsMapping(h);
    if (!m || size == 0) return nullptr;

    const int prot = (desiredAccess & FILE_MAP_WRITE) ? (PROT_READ | PROT_WRITE)
                                                      : PROT_READ;
    void* p = mmap(nullptr, size, prot, MAP_SHARED, m->fd, 0);
    if (p == MAP_FAILED) {
        feeds_win32_stub::SetLastErrorFromErrno();
        return nullptr;
    }

    std::lock_guard<std::mutex> lk(feeds_win32_stub::ViewMutex());
    feeds_win32_stub::ViewSizes()[p] = size;
    return p;
}

inline BOOL UnmapViewOfFile(const void* p)
{
    if (!p) return FALSE;

    size_t size = 0;
    {
        std::lock_guard<std::mutex> lk(feeds_win32_stub::ViewMutex());
        auto& sizes = feeds_win32_stub::ViewSizes();
        auto  it    = sizes.find(p);
        if (it == sizes.end()) return FALSE;
        size = it->second;
        sizes.erase(it);
    }
    return munmap(const_cast<void*>(p), size) == 0 ? TRUE : FALSE;
}

// Closes either an event or a shared-memory handle. Callers join their waiting
// thread before closing an event (as on Windows), and unmap their view before
// closing a mapping — but closing the descriptor first would be harmless
// either way, because a POSIX mapping stays valid after its fd is closed.
//
// Note what is NOT here: shm_unlink. The plugin is the reader and does not own
// the name; unlinking it would pull the region out from under a second source
// reading the same share region, and out from under the engine's own writer.
inline BOOL CloseHandle(HANDLE h)
{
    if (feeds_win32_stub::Event* e = feeds_win32_stub::AsEvent(h)) {
        e->magic = 0;
        delete e;
        return TRUE;
    }
    if (feeds_win32_stub::Mapping* m = feeds_win32_stub::AsMapping(h)) {
        if (m->fd >= 0) close(m->fd);
        m->magic = 0;
        delete m;
        return TRUE;
    }
    return TRUE;
}

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
