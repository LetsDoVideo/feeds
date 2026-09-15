// engine-client-posix.cpp — the macOS half of engine-client.cpp.
//
// Same four entry points and the same JSON messages as the Windows client; only
// the transport and the process plumbing differ:
//
//   Windows                             macOS
//   two named pipes, MESSAGE mode       one socketpair, newline-framed
//   CreateProcess + Job Object          posix_spawn + explicit kill on stop
//   ReadFile / ERROR_BROKEN_PIPE        read() / EOF
//
// The engine inherits its end of the socketpair as fd 3 (feeds_ipc::kEngineIpcFd),
// so there is no socket path, no connect/accept race, and no stale file to clean
// up — and the socket reports EOF the instant either process dies. See
// common/feeds-ipc-posix.h for the framing and the SIGPIPE rules.
//
// INCREMENT 1a SCOPE. This launches the engine, carries messages both ways, and
// notices cleanly when the engine dies (logged; the plugin keeps running with no
// engine). It deliberately does NOT restart it — auto-restart and the session
// recovery that has to go with it are a later increment. On Windows the Job
// Object guarantees the engine dies with OBS; macOS has no such thing, so
// StopEngine ends the process itself: graceful shutdown message, then SIGTERM,
// then SIGKILL, each with a bounded wait, so OBS never waits on a wedged engine
// and never leaves an orphan holding the Zoom SDK.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <obs-module.h>

#include "feeds-ipc-posix.h"
#include "feeds-json-lite.h"

#if defined(__APPLE__)
// A plugin is a bundle, not the main executable, and `environ` is not exported
// to one: referencing it directly fails to link. Apple's supported accessor is
// _NSGetEnviron(), which is exactly what this is for.
#include <crt_externs.h>
#define FEEDS_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define FEEDS_ENVIRON environ
#endif

namespace feeds {

static std::mutex        g_stateMutex;      // guards the fd and pid below
static int               g_sock  = -1;      // our end of the socketpair
static pid_t             g_pid   = -1;
static std::thread       g_readerThread;
static std::atomic<bool> g_shutdownRequested{false};
static std::atomic<bool> g_engineAlive{false};

static std::mutex g_handlersMutex;
static std::map<std::string, std::function<void(const std::string&)>> g_messageHandlers;

// ---------------------------------------------------------------------------
// Engine location
//
// The engine ships as FeedsEngine.app inside the plugin bundle, beside the
// loaded module: <bundle>/Contents/MacOS/FeedsEngine.app/Contents/MacOS/FeedsEngine.
//
// It must be an .app, not a loose executable. The macOS Zoom SDK resolves its
// runtime bundles through the MAIN BUNDLE's Frameworks directory, so a bare
// binary authenticates nothing: initSDK reports success and sdkAuth then fails
// with no callback at all. The engine's own preflight check reports that case
// explicitly rather than leaving it to be re-diagnosed.
// ---------------------------------------------------------------------------
static std::string EngineExecutablePath()
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&EngineExecutablePath), &info) &&
        info.dli_fname) {
        const std::string modulePath(info.dli_fname);
        const size_t slash = modulePath.find_last_of('/');
        if (slash != std::string::npos) {
            const std::string dir = modulePath.substr(0, slash + 1);
            const std::string candidate =
                dir + "FeedsEngine.app/Contents/MacOS/FeedsEngine";
            if (access(candidate.c_str(), X_OK) == 0)
                return candidate;
        }
    }
    // No fallback to a bare name on PATH: the only supported layout is the one
    // above, and a PATH hit would be some other machine's build.
    return std::string();
}

static void DispatchMessage(const std::string& json)
{
    const std::string type = ExtractJsonString(json, "type");
    if (type.empty()) return;

    std::function<void(const std::string&)> handler;
    {
        std::lock_guard<std::mutex> lock(g_handlersMutex);
        auto it = g_messageHandlers.find(type);
        if (it != g_messageHandlers.end()) handler = it->second;
    }
    if (handler) handler(json);
}

// Reader thread. Ends on EOF (engine exited or was killed), a read error, or
// our own shutdown. Reaps the engine so it cannot linger as a zombie, and logs
// the exit status — the one place a crash becomes visible in this increment.
static void ReaderThread()
{
    blog(LOG_INFO, "[feeds] engine reader: started");

    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        fd = g_sock;
    }
    if (fd < 0) return;

    feeds_ipc::LineReader reader(fd);
    std::string line;
    while (reader.ReadLine(line)) {
        if (line.empty()) continue;
        // Redaction, as on Windows: log the type only. A login_succeeded
        // carries the user's name and PMI, and forwarded engine "log" lines are
        // re-emitted by their own handler.
        const std::string type = ExtractJsonString(line, "type");
        blog(LOG_DEBUG, "[feeds] engine reader: received %s",
             type.empty() ? "(unknown)" : type.c_str());
        DispatchMessage(line);
    }

    g_engineAlive.store(false);

    if (!g_shutdownRequested.load()) {
        // The engine went away on its own: a crash, a fatal SDK error, or an
        // exit we did not ask for. Report it and reap it. No restart in this
        // increment — the plugin simply has no engine until OBS restarts.
        pid_t pid = -1;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            pid = g_pid;
        }
        int status = 0;
        if (pid > 0 && waitpid(pid, &status, 0) == pid) {
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_pid = -1;
            }
            if (WIFEXITED(status)) {
                blog(LOG_ERROR,
                     "[feeds] engine exited unexpectedly (code %d). Zoom features "
                     "are unavailable until OBS is restarted.",
                     WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                blog(LOG_ERROR,
                     "[feeds] engine was killed by signal %d. Zoom features are "
                     "unavailable until OBS is restarted.",
                     WTERMSIG(status));
            }
        } else {
            blog(LOG_ERROR, "[feeds] engine connection lost");
        }
    }

    blog(LOG_INFO, "[feeds] engine reader: exiting");
}

// ---------------------------------------------------------------------------
// Public API (mirrors engine-client.cpp)
// ---------------------------------------------------------------------------

bool StartEngine()
{
    // Before anything can write to the socket. A dead engine must never be able
    // to kill OBS with SIGPIPE — see common/feeds-ipc-posix.h.
    feeds_ipc::IgnoreSigPipe();

    const std::string enginePath = EngineExecutablePath();
    if (enginePath.empty()) {
        blog(LOG_WARNING,
             "[feeds] StartEngine: no FeedsEngine.app beside the plugin — "
             "Zoom features are unavailable in this build");
        return false;
    }

    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        blog(LOG_ERROR, "[feeds] StartEngine: socketpair failed: %s",
             strerror(errno));
        return false;
    }
    feeds_ipc::SuppressSocketSigPipe(sv[0]);

    // Move the child's end out of the way if the kernel happened to hand us fd
    // 3 itself. posix_spawn's dup2 action with equal descriptors is a special
    // case (it only clears FD_CLOEXEC), and depending on that is a trap worth
    // one line to avoid.
    if (sv[1] == feeds_ipc::kEngineIpcFd) {
        const int moved = fcntl(sv[1], F_DUPFD, feeds_ipc::kEngineIpcFd + 1);
        if (moved >= 0) { close(sv[1]); sv[1] = moved; }
    }

    // Hand the child its end as fd 3 and close ours in the child. dup2 clears
    // FD_CLOEXEC on the new descriptor, so fd 3 survives the exec.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, sv[0]);
    posix_spawn_file_actions_adddup2(&actions, sv[1], feeds_ipc::kEngineIpcFd);

    char* const argv[] = {const_cast<char*>(enginePath.c_str()), nullptr};
    pid_t pid = -1;
    // posix_spawn returns the error number directly; it does not set errno.
    const int rc = posix_spawn(&pid, enginePath.c_str(), &actions, nullptr,
                               argv, FEEDS_ENVIRON);
    posix_spawn_file_actions_destroy(&actions);
    close(sv[1]);   // the child owns that end now

    if (rc != 0) {
        close(sv[0]);
        blog(LOG_ERROR, "[feeds] StartEngine: could not launch '%s': %s",
             enginePath.c_str(), strerror(rc));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_sock = sv[0];
        g_pid  = pid;
    }
    g_shutdownRequested.store(false);
    g_engineAlive.store(true);

    blog(LOG_INFO, "[feeds] StartEngine: launched %s (pid %d)",
         enginePath.c_str(), (int)pid);

    g_readerThread = std::thread(ReaderThread);
    return true;
}

void StopEngine()
{
    blog(LOG_INFO, "[feeds] StopEngine: shutting down");
    g_shutdownRequested.store(true);

    pid_t pid = -1;
    int   fd  = -1;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        pid = g_pid;
        fd  = g_sock;
    }

    // Ask nicely first; the engine leaves any meeting and exits on this.
    if (fd >= 0 && g_engineAlive.load())
        feeds_ipc::WriteLine(fd, "{\"type\":\"shutdown\"}");

    // Then unblock the reader: shutdown() makes the blocking read return 0 even
    // though the fd is still open, so the thread can join without waiting on
    // an engine that may never write again.
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    if (g_readerThread.joinable()) g_readerThread.join();

    // Bounded escalation. macOS has no Job Object, so nothing else guarantees
    // the engine dies with OBS: an engine left holding the Zoom SDK would block
    // the next one and keep a meeting alive with nobody watching.
    if (pid > 0) {
        bool exited = false;
        for (int i = 0; i < 40 && !exited; ++i) {   // up to ~2 s
            int status = 0;
            const pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid || (r < 0 && errno == ECHILD)) { exited = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!exited) {
            blog(LOG_WARNING, "[feeds] StopEngine: engine still running; SIGTERM");
            kill(pid, SIGTERM);
            for (int i = 0; i < 20 && !exited; ++i) {   // up to ~1 s
                int status = 0;
                const pid_t r = waitpid(pid, &status, WNOHANG);
                if (r == pid || (r < 0 && errno == ECHILD)) { exited = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (!exited) {
            blog(LOG_WARNING, "[feeds] StopEngine: engine ignored SIGTERM; SIGKILL");
            kill(pid, SIGKILL);
            int status = 0;
            waitpid(pid, &status, 0);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_sock >= 0) { close(g_sock); g_sock = -1; }
        g_pid = -1;
    }
    g_engineAlive.store(false);
    blog(LOG_INFO, "[feeds] StopEngine: shutdown complete");
}

bool SendToEngine(const std::string& jsonMessage)
{
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        fd = g_sock;
    }
    if (fd < 0 || !g_engineAlive.load()) {
        blog(LOG_DEBUG, "[feeds] SendToEngine: no engine; message dropped");
        return false;
    }

    if (!feeds_ipc::WriteLine(fd, jsonMessage)) {
        // EPIPE rather than a fatal signal, because SIGPIPE is ignored.
        g_engineAlive.store(false);
        blog(LOG_ERROR, "[feeds] SendToEngine: write failed (%s); engine link lost",
             strerror(errno));
        return false;
    }

    // Redaction: type only, never the payload — an outbound join_meeting carries
    // the meeting password.
    std::string type = ExtractJsonString(jsonMessage, "type");
    if (type.empty()) type = "(unknown)";
    blog(LOG_DEBUG, "[feeds] SendToEngine: sent %s", type.c_str());
    return true;
}

void RegisterMessageHandler(const std::string& messageType,
                            std::function<void(const std::string&)> handler)
{
    std::lock_guard<std::mutex> lock(g_handlersMutex);
    g_messageHandlers[messageType] = std::move(handler);
}

} // namespace feeds
