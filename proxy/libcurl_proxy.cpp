// libcurl_proxy.cpp
// x64-compatible proxy DLL.
// Loads libcurl_obs.dll and libcurl_zoom.dll at startup, resolves all
// function pointers, and exports them via libcurl_proxy.def.
// The linker uses the import libs to satisfy the .def exports at link time;
// at runtime the actual calls go through these stubs to the real DLLs.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

static HMODULE g_obs  = nullptr;
static HMODULE g_zoom = nullptr;

static void* fp_curl_easy_cleanup = nullptr;
static void* fp_curl_easy_duphandle = nullptr;
static void* fp_curl_easy_escape = nullptr;
static void* fp_curl_easy_getinfo = nullptr;
static void* fp_curl_easy_header = nullptr;
static void* fp_curl_easy_init = nullptr;
static void* fp_curl_easy_nextheader = nullptr;
static void* fp_curl_easy_option_by_id = nullptr;
static void* fp_curl_easy_option_by_name = nullptr;
static void* fp_curl_easy_option_next = nullptr;
static void* fp_curl_easy_pause = nullptr;
static void* fp_curl_easy_perform = nullptr;
static void* fp_curl_easy_recv = nullptr;
static void* fp_curl_easy_reset = nullptr;
static void* fp_curl_easy_send = nullptr;
static void* fp_curl_easy_setopt = nullptr;
static void* fp_curl_easy_ssls_export = nullptr;
static void* fp_curl_easy_ssls_import = nullptr;
static void* fp_curl_easy_strerror = nullptr;
static void* fp_curl_easy_unescape = nullptr;
static void* fp_curl_easy_upkeep = nullptr;
static void* fp_curl_escape = nullptr;
static void* fp_curl_formadd = nullptr;
static void* fp_curl_formfree = nullptr;
static void* fp_curl_formget = nullptr;
static void* fp_curl_free = nullptr;
static void* fp_curl_getdate = nullptr;
static void* fp_curl_getenv = nullptr;
static void* fp_curl_global_cleanup = nullptr;
static void* fp_curl_global_init = nullptr;
static void* fp_curl_global_init_mem = nullptr;
static void* fp_curl_global_sslset = nullptr;
static void* fp_curl_global_trace = nullptr;
static void* fp_curl_maprintf = nullptr;
static void* fp_curl_mfprintf = nullptr;
static void* fp_curl_mime_addpart = nullptr;
static void* fp_curl_mime_data = nullptr;
static void* fp_curl_mime_data_cb = nullptr;
static void* fp_curl_mime_encoder = nullptr;
static void* fp_curl_mime_filedata = nullptr;
static void* fp_curl_mime_filename = nullptr;
static void* fp_curl_mime_free = nullptr;
static void* fp_curl_mime_headers = nullptr;
static void* fp_curl_mime_init = nullptr;
static void* fp_curl_mime_name = nullptr;
static void* fp_curl_mime_subparts = nullptr;
static void* fp_curl_mime_type = nullptr;
static void* fp_curl_mprintf = nullptr;
static void* fp_curl_msnprintf = nullptr;
static void* fp_curl_msprintf = nullptr;
static void* fp_curl_multi_add_handle = nullptr;
static void* fp_curl_multi_assign = nullptr;
static void* fp_curl_multi_cleanup = nullptr;
static void* fp_curl_multi_fdset = nullptr;
static void* fp_curl_multi_get_handles = nullptr;
static void* fp_curl_multi_info_read = nullptr;
static void* fp_curl_multi_init = nullptr;
static void* fp_curl_multi_perform = nullptr;
static void* fp_curl_multi_poll = nullptr;
static void* fp_curl_multi_remove_handle = nullptr;
static void* fp_curl_multi_setopt = nullptr;
static void* fp_curl_multi_socket = nullptr;
static void* fp_curl_multi_socket_action = nullptr;
static void* fp_curl_multi_socket_all = nullptr;
static void* fp_curl_multi_strerror = nullptr;
static void* fp_curl_multi_timeout = nullptr;
static void* fp_curl_multi_wait = nullptr;
static void* fp_curl_multi_waitfds = nullptr;
static void* fp_curl_multi_wakeup = nullptr;
static void* fp_curl_mvaprintf = nullptr;
static void* fp_curl_mvfprintf = nullptr;
static void* fp_curl_mvprintf = nullptr;
static void* fp_curl_mvsnprintf = nullptr;
static void* fp_curl_mvsprintf = nullptr;
static void* fp_curl_pushheader_byname = nullptr;
static void* fp_curl_pushheader_bynum = nullptr;
static void* fp_curl_share_cleanup = nullptr;
static void* fp_curl_share_init = nullptr;
static void* fp_curl_share_setopt = nullptr;
static void* fp_curl_share_strerror = nullptr;
static void* fp_curl_slist_append = nullptr;
static void* fp_curl_slist_free_all = nullptr;
static void* fp_curl_strequal = nullptr;
static void* fp_curl_strnequal = nullptr;
static void* fp_curl_unescape = nullptr;
static void* fp_curl_url = nullptr;
static void* fp_curl_url_cleanup = nullptr;
static void* fp_curl_url_dup = nullptr;
static void* fp_curl_url_get = nullptr;
static void* fp_curl_url_set = nullptr;
static void* fp_curl_url_strerror = nullptr;
static void* fp_curl_version = nullptr;
static void* fp_curl_version_info = nullptr;
static void* fp_curl_ws_meta = nullptr;
static void* fp_curl_ws_recv = nullptr;
static void* fp_curl_ws_send = nullptr;
static void* fp_Curl_hostcheck = nullptr;
static void* fp_Curl_ssl_init_certinfo = nullptr;
static void* fp_Curl_ssl_push_certinfo_len = nullptr;
static void* fp_curl_global_init_for_zoom = nullptr;
static void* fp_curl_multi_get_offt = nullptr;
static void* fp_curl_multi_notify_disable = nullptr;
static void* fp_curl_multi_notify_enable = nullptr;
static void* fp_curl_websocket_conn_cleanup = nullptr;
static void* fp_curl_ws_start_frame = nullptr;

static void GetDir(char* dir, DWORD size) {
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&GetDir, &hSelf);
    GetModuleFileNameA(hSelf, dir, size);
    char* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = '\0';
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        char dir[MAX_PATH] = {};
        GetDir(dir, MAX_PATH);

        char obs_path[MAX_PATH], zoom_path[MAX_PATH];
        snprintf(obs_path,  MAX_PATH, "%slibcurl_obs.dll",  dir);
        snprintf(zoom_path, MAX_PATH, "%slibcurl_zoom.dll", dir);

        g_obs  = LoadLibraryA(obs_path);
        g_zoom = LoadLibraryA(zoom_path);

        if (!g_obs || !g_zoom) return FALSE;

        fp_curl_easy_cleanup = GetProcAddress(g_obs,  "curl_easy_cleanup");
        fp_curl_easy_duphandle = GetProcAddress(g_obs,  "curl_easy_duphandle");
        fp_curl_easy_escape = GetProcAddress(g_obs,  "curl_easy_escape");
        fp_curl_easy_getinfo = GetProcAddress(g_obs,  "curl_easy_getinfo");
        fp_curl_easy_header = GetProcAddress(g_obs,  "curl_easy_header");
        fp_curl_easy_init = GetProcAddress(g_obs,  "curl_easy_init");
        fp_curl_easy_nextheader = GetProcAddress(g_obs,  "curl_easy_nextheader");
        fp_curl_easy_option_by_id = GetProcAddress(g_obs,  "curl_easy_option_by_id");
        fp_curl_easy_option_by_name = GetProcAddress(g_obs,  "curl_easy_option_by_name");
        fp_curl_easy_option_next = GetProcAddress(g_obs,  "curl_easy_option_next");
        fp_curl_easy_pause = GetProcAddress(g_obs,  "curl_easy_pause");
        fp_curl_easy_perform = GetProcAddress(g_obs,  "curl_easy_perform");
        fp_curl_easy_recv = GetProcAddress(g_obs,  "curl_easy_recv");
        fp_curl_easy_reset = GetProcAddress(g_obs,  "curl_easy_reset");
        fp_curl_easy_send = GetProcAddress(g_obs,  "curl_easy_send");
        fp_curl_easy_setopt = GetProcAddress(g_obs,  "curl_easy_setopt");
        fp_curl_easy_ssls_export = GetProcAddress(g_obs,  "curl_easy_ssls_export");
        fp_curl_easy_ssls_import = GetProcAddress(g_obs,  "curl_easy_ssls_import");
        fp_curl_easy_strerror = GetProcAddress(g_obs,  "curl_easy_strerror");
        fp_curl_easy_unescape = GetProcAddress(g_obs,  "curl_easy_unescape");
        fp_curl_easy_upkeep = GetProcAddress(g_obs,  "curl_easy_upkeep");
        fp_curl_escape = GetProcAddress(g_obs,  "curl_escape");
        fp_curl_formadd = GetProcAddress(g_obs,  "curl_formadd");
        fp_curl_formfree = GetProcAddress(g_obs,  "curl_formfree");
        fp_curl_formget = GetProcAddress(g_obs,  "curl_formget");
        fp_curl_free = GetProcAddress(g_obs,  "curl_free");
        fp_curl_getdate = GetProcAddress(g_obs,  "curl_getdate");
        fp_curl_getenv = GetProcAddress(g_obs,  "curl_getenv");
        fp_curl_global_cleanup = GetProcAddress(g_obs,  "curl_global_cleanup");
        fp_curl_global_init = GetProcAddress(g_obs,  "curl_global_init");
        fp_curl_global_init_mem = GetProcAddress(g_obs,  "curl_global_init_mem");
        fp_curl_global_sslset = GetProcAddress(g_obs,  "curl_global_sslset");
        fp_curl_global_trace = GetProcAddress(g_obs,  "curl_global_trace");
        fp_curl_maprintf = GetProcAddress(g_obs,  "curl_maprintf");
        fp_curl_mfprintf = GetProcAddress(g_obs,  "curl_mfprintf");
        fp_curl_mime_addpart = GetProcAddress(g_obs,  "curl_mime_addpart");
        fp_curl_mime_data = GetProcAddress(g_obs,  "curl_mime_data");
        fp_curl_mime_data_cb = GetProcAddress(g_obs,  "curl_mime_data_cb");
        fp_curl_mime_encoder = GetProcAddress(g_obs,  "curl_mime_encoder");
        fp_curl_mime_filedata = GetProcAddress(g_obs,  "curl_mime_filedata");
        fp_curl_mime_filename = GetProcAddress(g_obs,  "curl_mime_filename");
        fp_curl_mime_free = GetProcAddress(g_obs,  "curl_mime_free");
        fp_curl_mime_headers = GetProcAddress(g_obs,  "curl_mime_headers");
        fp_curl_mime_init = GetProcAddress(g_obs,  "curl_mime_init");
        fp_curl_mime_name = GetProcAddress(g_obs,  "curl_mime_name");
        fp_curl_mime_subparts = GetProcAddress(g_obs,  "curl_mime_subparts");
        fp_curl_mime_type = GetProcAddress(g_obs,  "curl_mime_type");
        fp_curl_mprintf = GetProcAddress(g_obs,  "curl_mprintf");
        fp_curl_msnprintf = GetProcAddress(g_obs,  "curl_msnprintf");
        fp_curl_msprintf = GetProcAddress(g_obs,  "curl_msprintf");
        fp_curl_multi_add_handle = GetProcAddress(g_obs,  "curl_multi_add_handle");
        fp_curl_multi_assign = GetProcAddress(g_obs,  "curl_multi_assign");
        fp_curl_multi_cleanup = GetProcAddress(g_obs,  "curl_multi_cleanup");
        fp_curl_multi_fdset = GetProcAddress(g_obs,  "curl_multi_fdset");
        fp_curl_multi_get_handles = GetProcAddress(g_obs,  "curl_multi_get_handles");
        fp_curl_multi_info_read = GetProcAddress(g_obs,  "curl_multi_info_read");
        fp_curl_multi_init = GetProcAddress(g_obs,  "curl_multi_init");
        fp_curl_multi_perform = GetProcAddress(g_obs,  "curl_multi_perform");
        fp_curl_multi_poll = GetProcAddress(g_obs,  "curl_multi_poll");
        fp_curl_multi_remove_handle = GetProcAddress(g_obs,  "curl_multi_remove_handle");
        fp_curl_multi_setopt = GetProcAddress(g_obs,  "curl_multi_setopt");
        fp_curl_multi_socket = GetProcAddress(g_obs,  "curl_multi_socket");
        fp_curl_multi_socket_action = GetProcAddress(g_obs,  "curl_multi_socket_action");
        fp_curl_multi_socket_all = GetProcAddress(g_obs,  "curl_multi_socket_all");
        fp_curl_multi_strerror = GetProcAddress(g_obs,  "curl_multi_strerror");
        fp_curl_multi_timeout = GetProcAddress(g_obs,  "curl_multi_timeout");
        fp_curl_multi_wait = GetProcAddress(g_obs,  "curl_multi_wait");
        fp_curl_multi_waitfds = GetProcAddress(g_obs,  "curl_multi_waitfds");
        fp_curl_multi_wakeup = GetProcAddress(g_obs,  "curl_multi_wakeup");
        fp_curl_mvaprintf = GetProcAddress(g_obs,  "curl_mvaprintf");
        fp_curl_mvfprintf = GetProcAddress(g_obs,  "curl_mvfprintf");
        fp_curl_mvprintf = GetProcAddress(g_obs,  "curl_mvprintf");
        fp_curl_mvsnprintf = GetProcAddress(g_obs,  "curl_mvsnprintf");
        fp_curl_mvsprintf = GetProcAddress(g_obs,  "curl_mvsprintf");
        fp_curl_pushheader_byname = GetProcAddress(g_obs,  "curl_pushheader_byname");
        fp_curl_pushheader_bynum = GetProcAddress(g_obs,  "curl_pushheader_bynum");
        fp_curl_share_cleanup = GetProcAddress(g_obs,  "curl_share_cleanup");
        fp_curl_share_init = GetProcAddress(g_obs,  "curl_share_init");
        fp_curl_share_setopt = GetProcAddress(g_obs,  "curl_share_setopt");
        fp_curl_share_strerror = GetProcAddress(g_obs,  "curl_share_strerror");
        fp_curl_slist_append = GetProcAddress(g_obs,  "curl_slist_append");
        fp_curl_slist_free_all = GetProcAddress(g_obs,  "curl_slist_free_all");
        fp_curl_strequal = GetProcAddress(g_obs,  "curl_strequal");
        fp_curl_strnequal = GetProcAddress(g_obs,  "curl_strnequal");
        fp_curl_unescape = GetProcAddress(g_obs,  "curl_unescape");
        fp_curl_url = GetProcAddress(g_obs,  "curl_url");
        fp_curl_url_cleanup = GetProcAddress(g_obs,  "curl_url_cleanup");
        fp_curl_url_dup = GetProcAddress(g_obs,  "curl_url_dup");
        fp_curl_url_get = GetProcAddress(g_obs,  "curl_url_get");
        fp_curl_url_set = GetProcAddress(g_obs,  "curl_url_set");
        fp_curl_url_strerror = GetProcAddress(g_obs,  "curl_url_strerror");
        fp_curl_version = GetProcAddress(g_obs,  "curl_version");
        fp_curl_version_info = GetProcAddress(g_obs,  "curl_version_info");
        fp_curl_ws_meta = GetProcAddress(g_obs,  "curl_ws_meta");
        fp_curl_ws_recv = GetProcAddress(g_obs,  "curl_ws_recv");
        fp_curl_ws_send = GetProcAddress(g_obs,  "curl_ws_send");
        fp_Curl_hostcheck = GetProcAddress(g_zoom, "Curl_hostcheck");
        fp_Curl_ssl_init_certinfo = GetProcAddress(g_zoom, "Curl_ssl_init_certinfo");
        fp_Curl_ssl_push_certinfo_len = GetProcAddress(g_zoom, "Curl_ssl_push_certinfo_len");
        fp_curl_global_init_for_zoom = GetProcAddress(g_zoom, "curl_global_init_for_zoom");
        fp_curl_multi_get_offt = GetProcAddress(g_zoom, "curl_multi_get_offt");
        fp_curl_multi_notify_disable = GetProcAddress(g_zoom, "curl_multi_notify_disable");
        fp_curl_multi_notify_enable = GetProcAddress(g_zoom, "curl_multi_notify_enable");
        fp_curl_websocket_conn_cleanup = GetProcAddress(g_zoom, "curl_websocket_conn_cleanup");
        fp_curl_ws_start_frame = GetProcAddress(g_zoom, "curl_ws_start_frame");
    }
    if (reason == DLL_PROCESS_DETACH) {
        if (g_obs)  { FreeLibrary(g_obs);  g_obs  = nullptr; }
        if (g_zoom) { FreeLibrary(g_zoom); g_zoom = nullptr; }
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Export stubs — each function resolves its pointer on first call and
// forwards. Using typedef void* avoids needing real curl signatures.
// The .def file ensures these are exported with the correct names.
// ---------------------------------------------------------------------------
typedef void* (*FnPtr)(...);

#define STUB(fp) \
    if (fp) return reinterpret_cast<FnPtr>(fp)(); \
    return nullptr;

// Standard curl -> libcurl_obs.dll
extern "C" void* curl_easy_cleanup(...) { STUB(fp_curl_easy_cleanup) }
extern "C" void* curl_easy_duphandle(...) { STUB(fp_curl_easy_duphandle) }
extern "C" void* curl_easy_escape(...) { STUB(fp_curl_easy_escape) }
extern "C" void* curl_easy_getinfo(...) { STUB(fp_curl_easy_getinfo) }
extern "C" void* curl_easy_header(...) { STUB(fp_curl_easy_header) }
extern "C" void* curl_easy_init(...) { STUB(fp_curl_easy_init) }
extern "C" void* curl_easy_nextheader(...) { STUB(fp_curl_easy_nextheader) }
extern "C" void* curl_easy_option_by_id(...) { STUB(fp_curl_easy_option_by_id) }
extern "C" void* curl_easy_option_by_name(...) { STUB(fp_curl_easy_option_by_name) }
extern "C" void* curl_easy_option_next(...) { STUB(fp_curl_easy_option_next) }
extern "C" void* curl_easy_pause(...) { STUB(fp_curl_easy_pause) }
extern "C" void* curl_easy_perform(...) { STUB(fp_curl_easy_perform) }
extern "C" void* curl_easy_recv(...) { STUB(fp_curl_easy_recv) }
extern "C" void* curl_easy_reset(...) { STUB(fp_curl_easy_reset) }
extern "C" void* curl_easy_send(...) { STUB(fp_curl_easy_send) }
extern "C" void* curl_easy_setopt(...) { STUB(fp_curl_easy_setopt) }
extern "C" void* curl_easy_ssls_export(...) { STUB(fp_curl_easy_ssls_export) }
extern "C" void* curl_easy_ssls_import(...) { STUB(fp_curl_easy_ssls_import) }
extern "C" void* curl_easy_strerror(...) { STUB(fp_curl_easy_strerror) }
extern "C" void* curl_easy_unescape(...) { STUB(fp_curl_easy_unescape) }
extern "C" void* curl_easy_upkeep(...) { STUB(fp_curl_easy_upkeep) }
extern "C" void* curl_escape(...) { STUB(fp_curl_escape) }
extern "C" void* curl_formadd(...) { STUB(fp_curl_formadd) }
extern "C" void* curl_formfree(...) { STUB(fp_curl_formfree) }
extern "C" void* curl_formget(...) { STUB(fp_curl_formget) }
extern "C" void* curl_free(...) { STUB(fp_curl_free) }
extern "C" void* curl_getdate(...) { STUB(fp_curl_getdate) }
extern "C" void* curl_getenv(...) { STUB(fp_curl_getenv) }
extern "C" void* curl_global_cleanup(...) { STUB(fp_curl_global_cleanup) }
extern "C" void* curl_global_init(...) { STUB(fp_curl_global_init) }
extern "C" void* curl_global_init_mem(...) { STUB(fp_curl_global_init_mem) }
extern "C" void* curl_global_sslset(...) { STUB(fp_curl_global_sslset) }
extern "C" void* curl_global_trace(...) { STUB(fp_curl_global_trace) }
extern "C" void* curl_maprintf(...) { STUB(fp_curl_maprintf) }
extern "C" void* curl_mfprintf(...) { STUB(fp_curl_mfprintf) }
extern "C" void* curl_mime_addpart(...) { STUB(fp_curl_mime_addpart) }
extern "C" void* curl_mime_data(...) { STUB(fp_curl_mime_data) }
extern "C" void* curl_mime_data_cb(...) { STUB(fp_curl_mime_data_cb) }
extern "C" void* curl_mime_encoder(...) { STUB(fp_curl_mime_encoder) }
extern "C" void* curl_mime_filedata(...) { STUB(fp_curl_mime_filedata) }
extern "C" void* curl_mime_filename(...) { STUB(fp_curl_mime_filename) }
extern "C" void* curl_mime_free(...) { STUB(fp_curl_mime_free) }
extern "C" void* curl_mime_headers(...) { STUB(fp_curl_mime_headers) }
extern "C" void* curl_mime_init(...) { STUB(fp_curl_mime_init) }
extern "C" void* curl_mime_name(...) { STUB(fp_curl_mime_name) }
extern "C" void* curl_mime_subparts(...) { STUB(fp_curl_mime_subparts) }
extern "C" void* curl_mime_type(...) { STUB(fp_curl_mime_type) }
extern "C" void* curl_mprintf(...) { STUB(fp_curl_mprintf) }
extern "C" void* curl_msnprintf(...) { STUB(fp_curl_msnprintf) }
extern "C" void* curl_msprintf(...) { STUB(fp_curl_msprintf) }
extern "C" void* curl_multi_add_handle(...) { STUB(fp_curl_multi_add_handle) }
extern "C" void* curl_multi_assign(...) { STUB(fp_curl_multi_assign) }
extern "C" void* curl_multi_cleanup(...) { STUB(fp_curl_multi_cleanup) }
extern "C" void* curl_multi_fdset(...) { STUB(fp_curl_multi_fdset) }
extern "C" void* curl_multi_get_handles(...) { STUB(fp_curl_multi_get_handles) }
extern "C" void* curl_multi_info_read(...) { STUB(fp_curl_multi_info_read) }
extern "C" void* curl_multi_init(...) { STUB(fp_curl_multi_init) }
extern "C" void* curl_multi_perform(...) { STUB(fp_curl_multi_perform) }
extern "C" void* curl_multi_poll(...) { STUB(fp_curl_multi_poll) }
extern "C" void* curl_multi_remove_handle(...) { STUB(fp_curl_multi_remove_handle) }
extern "C" void* curl_multi_setopt(...) { STUB(fp_curl_multi_setopt) }
extern "C" void* curl_multi_socket(...) { STUB(fp_curl_multi_socket) }
extern "C" void* curl_multi_socket_action(...) { STUB(fp_curl_multi_socket_action) }
extern "C" void* curl_multi_socket_all(...) { STUB(fp_curl_multi_socket_all) }
extern "C" void* curl_multi_strerror(...) { STUB(fp_curl_multi_strerror) }
extern "C" void* curl_multi_timeout(...) { STUB(fp_curl_multi_timeout) }
extern "C" void* curl_multi_wait(...) { STUB(fp_curl_multi_wait) }
extern "C" void* curl_multi_waitfds(...) { STUB(fp_curl_multi_waitfds) }
extern "C" void* curl_multi_wakeup(...) { STUB(fp_curl_multi_wakeup) }
extern "C" void* curl_mvaprintf(...) { STUB(fp_curl_mvaprintf) }
extern "C" void* curl_mvfprintf(...) { STUB(fp_curl_mvfprintf) }
extern "C" void* curl_mvprintf(...) { STUB(fp_curl_mvprintf) }
extern "C" void* curl_mvsnprintf(...) { STUB(fp_curl_mvsnprintf) }
extern "C" void* curl_mvsprintf(...) { STUB(fp_curl_mvsprintf) }
extern "C" void* curl_pushheader_byname(...) { STUB(fp_curl_pushheader_byname) }
extern "C" void* curl_pushheader_bynum(...) { STUB(fp_curl_pushheader_bynum) }
extern "C" void* curl_share_cleanup(...) { STUB(fp_curl_share_cleanup) }
extern "C" void* curl_share_init(...) { STUB(fp_curl_share_init) }
extern "C" void* curl_share_setopt(...) { STUB(fp_curl_share_setopt) }
extern "C" void* curl_share_strerror(...) { STUB(fp_curl_share_strerror) }
extern "C" void* curl_slist_append(...) { STUB(fp_curl_slist_append) }
extern "C" void* curl_slist_free_all(...) { STUB(fp_curl_slist_free_all) }
extern "C" void* curl_strequal(...) { STUB(fp_curl_strequal) }
extern "C" void* curl_strnequal(...) { STUB(fp_curl_strnequal) }
extern "C" void* curl_unescape(...) { STUB(fp_curl_unescape) }
extern "C" void* curl_url(...) { STUB(fp_curl_url) }
extern "C" void* curl_url_cleanup(...) { STUB(fp_curl_url_cleanup) }
extern "C" void* curl_url_dup(...) { STUB(fp_curl_url_dup) }
extern "C" void* curl_url_get(...) { STUB(fp_curl_url_get) }
extern "C" void* curl_url_set(...) { STUB(fp_curl_url_set) }
extern "C" void* curl_url_strerror(...) { STUB(fp_curl_url_strerror) }
extern "C" void* curl_version(...) { STUB(fp_curl_version) }
extern "C" void* curl_version_info(...) { STUB(fp_curl_version_info) }
extern "C" void* curl_ws_meta(...) { STUB(fp_curl_ws_meta) }
extern "C" void* curl_ws_recv(...) { STUB(fp_curl_ws_recv) }
extern "C" void* curl_ws_send(...) { STUB(fp_curl_ws_send) }

// Zoom-specific -> libcurl_zoom.dll
extern "C" void* Curl_hostcheck(...) { STUB(fp_Curl_hostcheck) }
extern "C" void* Curl_ssl_init_certinfo(...) { STUB(fp_Curl_ssl_init_certinfo) }
extern "C" void* Curl_ssl_push_certinfo_len(...) { STUB(fp_Curl_ssl_push_certinfo_len) }
extern "C" void* curl_global_init_for_zoom(...) { STUB(fp_curl_global_init_for_zoom) }
extern "C" void* curl_multi_get_offt(...) { STUB(fp_curl_multi_get_offt) }
extern "C" void* curl_multi_notify_disable(...) { STUB(fp_curl_multi_notify_disable) }
extern "C" void* curl_multi_notify_enable(...) { STUB(fp_curl_multi_notify_enable) }
extern "C" void* curl_websocket_conn_cleanup(...) { STUB(fp_curl_websocket_conn_cleanup) }
extern "C" void* curl_ws_start_frame(...) { STUB(fp_curl_ws_start_frame) }
