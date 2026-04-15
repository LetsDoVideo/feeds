// libcurl_proxy.cpp
// x64-compatible proxy DLL for libcurl.
// Lazily loads libcurl_obs.dll (OBS curl) and libcurl_zoom.dll (Zoom curl)
// on first use rather than in DllMain, so a missing DLL never hard-crashes
// the host process.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

static HMODULE g_obs  = nullptr;
static HMODULE g_zoom = nullptr;
static bool    g_init = false;
static char    g_dir[MAX_PATH] = {};

static void EnsureLoaded() {
    if (g_init) return;
    g_init = true;

    // Find directory containing this DLL
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&EnsureLoaded, &hSelf);
    GetModuleFileNameA(hSelf, g_dir, MAX_PATH);
    char* slash = strrchr(g_dir, '\\');
    if (slash) *(slash + 1) = '\0';

    char obs_path[MAX_PATH], zoom_path[MAX_PATH];
    snprintf(obs_path,  MAX_PATH, "%slibcurl_obs.dll",  g_dir);
    snprintf(zoom_path, MAX_PATH, "%slibcurl_zoom.dll", g_dir);

    g_obs  = LoadLibraryA(obs_path);
    g_zoom = LoadLibraryA(zoom_path);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        if (g_obs)  { FreeLibrary(g_obs);  g_obs  = nullptr; }
        if (g_zoom) { FreeLibrary(g_zoom); g_zoom = nullptr; }
    }
    return TRUE;  // Always succeed — never hard-crash the host
}

typedef void* (*FnPtr)(...);

// ---------------------------------------------------------------------------
// Standard curl stubs -> libcurl_obs.dll
// ---------------------------------------------------------------------------

extern "C" void* curl_easy_cleanup(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_cleanup");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_duphandle(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_duphandle");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_escape(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_escape");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_getinfo(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_getinfo");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_header(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_header");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_init(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_init");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_nextheader(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_nextheader");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_option_by_id(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_option_by_id");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_option_by_name(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_option_by_name");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_option_next(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_option_next");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_pause(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_pause");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_perform(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_perform");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_recv(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_recv");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_reset(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_reset");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_send(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_send");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_setopt(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_setopt");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_ssls_export(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_ssls_export");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_ssls_import(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_ssls_import");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_strerror(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_strerror");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_unescape(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_unescape");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_easy_upkeep(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_easy_upkeep");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_escape(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_escape");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_formadd(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_formadd");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_formfree(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_formfree");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_formget(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_formget");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_free(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_free");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_getdate(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_getdate");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_getenv(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_getenv");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_global_cleanup(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_global_cleanup");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_global_init(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_global_init");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_global_init_mem(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_global_init_mem");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_global_sslset(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_global_sslset");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_global_trace(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_global_trace");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_maprintf(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_maprintf");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mfprintf(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mfprintf");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_addpart(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_addpart");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_data(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_data");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_data_cb(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_data_cb");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_encoder(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_encoder");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_filedata(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_filedata");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_filename(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_filename");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_free(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_free");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_headers(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_headers");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_init(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_init");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_name(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_name");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_subparts(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_subparts");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mime_type(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mime_type");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mprintf(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mprintf");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_msnprintf(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_msnprintf");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_msprintf(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_msprintf");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_add_handle(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_add_handle");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_assign(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_assign");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_cleanup(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_cleanup");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_fdset(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_fdset");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_get_handles(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_get_handles");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_info_read(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_info_read");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_init(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_init");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_perform(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_perform");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_poll(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_poll");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_remove_handle(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_remove_handle");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_setopt(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_setopt");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_socket(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_socket");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_socket_action(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_socket_action");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_socket_all(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_socket_all");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_strerror(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_strerror");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_timeout(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_timeout");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_wait(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_wait");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_waitfds(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_waitfds");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_wakeup(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_multi_wakeup");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mvaprintf(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mvaprintf");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mvfprintf(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mvfprintf");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mvprintf(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mvprintf");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mvsnprintf(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mvsnprintf");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_mvsprintf(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_mvsprintf");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_pushheader_byname(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_pushheader_byname");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_pushheader_bynum(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_pushheader_bynum");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_share_cleanup(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_share_cleanup");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_share_init(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_share_init");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_share_setopt(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_share_setopt");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_share_strerror(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_share_strerror");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_slist_append(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_slist_append");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_slist_free_all(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_slist_free_all");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_strequal(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_strequal");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_strnequal(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_strnequal");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_unescape(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_unescape");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_url(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_url");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_url_cleanup(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_url_cleanup");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_url_dup(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_url_dup");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_url_get(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_url_get");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_url_set(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_url_set");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_url_strerror(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_url_strerror");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_version(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_version");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_version_info(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_version_info");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_ws_meta(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_ws_meta");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_ws_recv(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_ws_recv");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_ws_send(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_obs) fp = (void*)GetProcAddress(g_obs, "curl_ws_send");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Zoom-specific stubs -> libcurl_zoom.dll
// ---------------------------------------------------------------------------

extern "C" void* Curl_hostcheck(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_zoom) fp = (void*)GetProcAddress(g_zoom, "Curl_hostcheck");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* Curl_ssl_init_certinfo(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_zoom) fp = (void*)GetProcAddress(g_zoom, "Curl_ssl_init_certinfo");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* Curl_ssl_push_certinfo_len(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_zoom) fp = (void*)GetProcAddress(g_zoom, "Curl_ssl_push_certinfo_len");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_global_init_for_zoom(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_zoom) fp = (void*)GetProcAddress(g_zoom, "curl_global_init_for_zoom");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_get_offt(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_zoom) fp = (void*)GetProcAddress(g_zoom, "curl_multi_get_offt");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_notify_disable(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_zoom) fp = (void*)GetProcAddress(g_zoom, "curl_multi_notify_disable");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_multi_notify_enable(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_zoom) fp = (void*)GetProcAddress(g_zoom, "curl_multi_notify_enable");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_websocket_conn_cleanup(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_zoom) fp = (void*)GetProcAddress(g_zoom, "curl_websocket_conn_cleanup");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}

extern "C" void* curl_ws_start_frame(...) {
    EnsureLoaded();
    static void* fp = nullptr;
    if (!fp && g_zoom) fp = (void*)GetProcAddress(g_zoom, "curl_ws_start_frame");
    if (fp) return reinterpret_cast<FnPtr>(fp)();
    return nullptr;
}
