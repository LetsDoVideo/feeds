// feeds-ipc-posix.h — the POSIX (macOS) half of the plugin <-> engine transport.
//
// Windows uses two unidirectional named pipes in MESSAGE mode, where one write
// is one message and the reader gets the boundaries for free (see
// engine-client.cpp and feeds-ipc-reassembly.h). A Unix socket is a byte
// STREAM: it has no message boundaries at all, so this transport frames each
// message with a trailing '\n'. The payload is the same JSON the Windows
// protocol uses, so PROTOCOL.md describes both platforms and only the
// transport differs.
//
// One socketpair, not two named sockets. The plugin creates it before spawning
// the engine and hands the engine its end as fd 3 (see engine-client-posix.cpp
// and the engine's main). That buys three things a filesystem socket would
// have to solve by hand:
//   * no path, so no stale socket file, no /tmp collision between two OBS
//     instances or two users, and no sun_path length limit;
//   * no connect/accept handshake and no retry loop;
//   * EOF on either end the moment the other process dies, which is how each
//     side detects the death of the other.
//
// SIGPIPE IS THE LOAD-BEARING DETAIL. Writing to a socket whose peer has died
// raises SIGPIPE, and the default disposition TERMINATES THE PROCESS. In the
// plugin that process is OBS: a crashed engine would take the whole host down
// with it and destroy the crash isolation the separate process exists for.
// Both sides therefore ignore SIGPIPE at startup (IgnoreSigPipe) and, where the
// platform offers it, also set SO_NOSIGPIPE on the socket itself. With those in
// place a doomed write returns EPIPE and the caller handles it as a lost link.

#pragma once

#ifdef _WIN32
#error "feeds-ipc-posix.h is POSIX-only; Windows uses named pipes (engine-client.cpp)"
#endif

#include <csignal>
#include <cstddef>
#include <cerrno>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace feeds_ipc {

// The descriptor the engine inherits. 0/1/2 stay as stdio so ordinary logging
// and crash output still work.
inline constexpr int kEngineIpcFd = 3;

// Ignore SIGPIPE process-wide. Call once, early, on BOTH sides. See the header
// note: without this a dead peer kills the host process on the next write.
inline void IgnoreSigPipe()
{
    std::signal(SIGPIPE, SIG_IGN);
}

// Belt and braces alongside IgnoreSigPipe: suppress SIGPIPE for this socket
// specifically, so the process-wide disposition is not the only thing standing
// between a dead engine and a dead OBS. Not available on every POSIX platform;
// absence is not an error.
inline void SuppressSocketSigPipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

// Write one message plus its '\n' terminator. Loops over short writes and
// retries EINTR. Returns false on a closed/broken socket (EPIPE) or any other
// error, which callers must treat as "the peer is gone", never as "sent".
inline bool WriteLine(int fd, const std::string& message)
{
    if (fd < 0) return false;
    const std::string framed = message + "\n";
    const char* p = framed.c_str();
    size_t remaining = framed.size();
    while (remaining > 0) {
        const ssize_t n = ::write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;   // EPIPE / EBADF / ECONNRESET: the link is gone
        }
        if (n == 0) return false;
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

// Buffered line reader. One instance per socket, owned by the reading thread.
//
// Reads in blocks rather than a byte at a time: a roster message for a large
// webinar is several KB, and a per-byte read() would be one syscall per byte.
// Partial lines are held across reads, so a message split over several reads is
// reassembled exactly like the Windows reader's ERROR_MORE_DATA path.
class LineReader {
public:
    explicit LineReader(int fd) : m_fd(fd) {}

    // Blocks until a whole line is available. Returns false on EOF (the peer
    // closed or died) or a read error — in both cases the caller stops reading.
    // The trailing '\n' is not included in `out`.
    bool ReadLine(std::string& out)
    {
        for (;;) {
            const size_t nl = m_buffer.find('\n');
            if (nl != std::string::npos) {
                out = m_buffer.substr(0, nl);
                m_buffer.erase(0, nl + 1);
                return true;
            }
            char chunk[8192];
            const ssize_t n = ::read(m_fd, chunk, sizeof(chunk));
            if (n > 0) {
                m_buffer.append(chunk, static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            return false;   // 0 = EOF (peer gone), <0 = error
        }
    }

private:
    int         m_fd;
    std::string m_buffer;
};

} // namespace feeds_ipc
