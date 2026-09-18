// feeds-mac-http.mm — the macOS implementation of the WinHTTP surface declared
// in common/feeds-win32-stub.h. NON-WINDOWS BUILDS ONLY.
//
// WHY THIS SHAPE
//
// plugin-main.cpp is ~10k lines written against WinHTTP: the update check, the
// YouTube live-chat poller, the Twitch IRC-over-WebSocket reader and both
// avatar fetchers all call WinHttpOpen / WinHttpConnect / WinHttpOpenRequest /
// WinHttpSendRequest / WinHttpReadData directly. Two ways to give macOS real
// networking: move those readers into the engine process and invent IPC for
// them, or implement the handful of WinHTTP calls they use natively and leave
// the callers alone. This is the second. It keeps plugin-main.cpp byte-for-byte
// identical across platforms, which means the Windows chat readers stay the
// single source of truth for behaviour and there is no second implementation of
// the YouTube or Twitch protocol logic to drift.
//
// Only the subset the plugin actually calls is implemented. This is not a
// general WinHTTP emulation and should not grow into one: if a caller needs
// something new, add exactly that.
//
// SYNCHRONOUS OVER ASYNCHRONOUS
//
// WinHTTP's model is blocking; NSURLSession's is callbacks. Every call here
// bridges the two with a semaphore. That is safe because every caller is a
// dedicated background thread that already expects to block:
//
//   * YtPollerLoop / TwChatLoop / the avatar workers — their own std::threads
//   * CheckForUpdateAsync — its own std::thread
//
// Nothing on this path runs on the OBS UI thread, and nothing here may ever be
// called from it. A blocking call on the main queue would deadlock against
// NSURLSession's own delegate dispatch.
//
// HANDLE LIFETIMES
//
// WinHTTP handles are opaque void*. The session, connection and request handles
// are heap objects carrying a magic word as their first member, the same
// discriminator trick the event and mapping handles in feeds-win32-stub.h use,
// so WinHttpCloseHandle can tell them apart and refuse anything that is not
// ours. Each is closed exactly once, by the thread that opened it.
//
// WebSocket handles are the exception, and get a different scheme, because the
// Twitch reader deliberately closes its socket from a SECOND thread to abort a
// blocked receive. A magic word cannot survive that: the check would be reading
// memory the closing thread may already have freed. Those handles are keys into
// a registry instead. See MacWebSocket below.

#ifdef _WIN32
#error "feeds-mac-http.mm is for the macOS plugin build only"
#endif

#import <Foundation/Foundation.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "feeds-win32-stub.h"

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// The plugin builds its URLs and headers as wide strings because that is what
// WinHTTP takes. Everything it passes is ASCII (hosts, paths, header lines), so
// a narrowing copy is exact rather than lossy. Message CONTENT never comes
// through here: it arrives as UTF-8 in a response body.
std::string Narrow(const wchar_t* w)
{
    if (!w) return {};
    std::string out;
    for (const wchar_t* p = w; *p; ++p)
        out += (*p > 0 && *p < 0x80) ? (char)*p : '?';
    return out;
}

NSString* NsFrom(const std::string& s)
{
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

constexpr uint32_t kSessionMagic = 0x4D485331u;  // 'MHS1'
constexpr uint32_t kConnectMagic = 0x4D484332u;  // 'MHC2'
constexpr uint32_t kRequestMagic = 0x4D485233u;  // 'MHR3'

struct MacHttpSession {
    uint32_t    magic = kSessionMagic;
    std::string userAgent;
    // WinHTTP's four timeouts in milliseconds. Only send/receive map onto
    // anything NSURLSession exposes, and they collapse into one request
    // timeout; resolve/connect are accepted and ignored rather than rejected,
    // because the callers set all four together.
    int         sendTimeoutMs    = 30000;
    int         receiveTimeoutMs = 30000;
};

struct MacHttpConnect {
    uint32_t        magic = kConnectMagic;
    MacHttpSession* session = nullptr;
    std::string     host;
    INTERNET_PORT   port = 443;
    bool            secure = true;
};

struct MacHttpRequest {
    uint32_t        magic = kRequestMagic;
    MacHttpConnect* connect = nullptr;
    std::string     verb = "GET";
    std::string     path = "/";
    std::vector<std::pair<std::string, std::string>> headers;

    // Set by WinHttpSetOption(WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET). When true
    // the request is not sent as HTTP at all: SendRequest and ReceiveResponse
    // become no-ops that succeed, and the work happens in
    // WinHttpWebSocketCompleteUpgrade, which is where the caller expects a
    // socket to appear.
    bool            websocketUpgrade = false;

    // Filled by ReceiveResponse. The whole body is buffered there rather than
    // streamed, because WinHttpReadData's contract (repeated reads until a zero
    // read) is trivial to serve from a buffer and the largest thing the plugin
    // fetches is a chat page measured in hundreds of kilobytes.
    DWORD               status = 0;
    std::vector<uint8_t> body;
    size_t               readOffset = 0;
    bool                 responded = false;
};

// ---------------------------------------------------------------------------
// WebSocket
//
// NSURLSessionWebSocketTask delivers one message per receive callback and
// requires the next receive to be issued from the previous completion. This
// object keeps exactly one receive outstanding and parks whole messages in a
// queue; WinHttpWebSocketReceive drains that queue, blocking when it is empty.
//
// Cancellation is the part that matters. The Twitch reader blocks in
// WinHttpWebSocketReceive indefinitely on an idle channel and is woken by
// another thread calling WinHttpCloseHandle on the socket. So Close sets the
// closed flag, cancels the task and broadcasts: any thread parked in Receive
// wakes, sees closed, and returns an error, which is exactly what the reader
// treats as "reconnect".
//
// That cross-thread close is also why these are owned by shared_ptr through a
// registry rather than being raw heap objects like the HTTP handles. Cancelling
// the task makes the outstanding receive complete LATER, on a URL-session
// queue; if close had simply deleted the object, that completion would run
// against freed memory. Instead the completion block holds a weak_ptr and
// promotes it, close drops the registry's strong reference, and the object dies
// once no callback is still inside it. The HINTERNET stays the raw pointer,
// which is only ever used as a registry key and never dereferenced blind.
// ---------------------------------------------------------------------------
struct MacWebSocket {
    // No magic word, unlike the handles above: this one is identified by being
    // in the registry, which is the only check that is safe across the
    // cross-thread close.
    std::mutex              mutex;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> inbox;

    // A message larger than the caller's buffer is handed over across several
    // Receive calls. The front of the inbox is the message being drained and
    // this is how far into it we have got.
    size_t  frontOffset = 0;

    bool    closed = false;   // set by Close, or by a receive error
    bool    receiveInFlight = false;

    NSURLSession*               session = nil;
    NSURLSessionWebSocketTask*  task = nil;

    void Fail()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true;
        }
        cv.notify_all();
    }
};

// The live sockets. The registry owns the only strong reference; everything
// else holds a weak one, so a socket outlives its close exactly as long as some
// callback is still running inside it.
std::mutex& SocketRegistryMutex()
{
    static std::mutex m;
    return m;
}
std::map<MacWebSocket*, std::shared_ptr<MacWebSocket>>& SocketRegistry()
{
    static std::map<MacWebSocket*, std::shared_ptr<MacWebSocket>> r;
    return r;
}

// Issue the next receive. Called once after the upgrade and then from each
// completion handler, so exactly one is ever outstanding.
//
// The block captures a weak_ptr, never the object. If the socket has been
// closed and released by the time the URL session fires the completion, the
// promotion fails and the handler does nothing at all rather than writing into
// freed memory.
void PumpReceive(const std::shared_ptr<MacWebSocket>& sp)
{
    if (!sp) return;
    MacWebSocket* ws = sp.get();
    {
        std::lock_guard<std::mutex> lock(ws->mutex);
        if (ws->closed || ws->receiveInFlight) return;
        ws->receiveInFlight = true;
    }

    std::weak_ptr<MacWebSocket> weak = sp;
    [ws->task receiveMessageWithCompletionHandler:
        ^(NSURLSessionWebSocketMessage* message, NSError* error) {
            std::shared_ptr<MacWebSocket> self = weak.lock();
            if (!self) return;   // closed and gone; nothing to do

            bool deliver = false;
            std::vector<uint8_t> bytes;

            if (error || !message) {
                self->Fail();
            } else if (message.type == NSURLSessionWebSocketMessageTypeString) {
                const char* utf8 = message.string.UTF8String;
                if (utf8) {
                    bytes.assign(utf8, utf8 + strlen(utf8));
                    deliver = true;
                }
            } else if (message.data) {
                const uint8_t* p = (const uint8_t*)message.data.bytes;
                bytes.assign(p, p + message.data.length);
                deliver = true;
            }

            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->receiveInFlight = false;
                if (deliver && !self->closed)
                    self->inbox.push_back(std::move(bytes));
            }
            self->cv.notify_all();
            PumpReceive(self);
        }];
}


// ---------------------------------------------------------------------------
// Handle discrimination. Each cast checks the magic word, so a handle of the
// wrong kind (or a stale one) is rejected rather than reinterpreted.
// ---------------------------------------------------------------------------
template <typename T, uint32_t Magic>
T* As(HINTERNET h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return nullptr;
    T* p = static_cast<T*>(h);
    return (p->magic == Magic) ? p : nullptr;
}

MacHttpSession* AsSession(HINTERNET h) { return As<MacHttpSession, kSessionMagic>(h); }
MacHttpConnect* AsConnect(HINTERNET h) { return As<MacHttpConnect, kConnectMagic>(h); }
MacHttpRequest* AsRequest(HINTERNET h) { return As<MacHttpRequest, kRequestMagic>(h); }
// Sockets are looked up, never cast. The pointer is only a key here: a handle
// closed by another thread is simply absent from the registry, which is a miss
// rather than a read of freed memory.
std::shared_ptr<MacWebSocket> LookupSocket(HINTERNET h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return nullptr;
    std::lock_guard<std::mutex> lock(SocketRegistryMutex());
    auto it = SocketRegistry().find(static_cast<MacWebSocket*>(h));
    return (it == SocketRegistry().end()) ? nullptr : it->second;
}

// One shared session for every plain HTTP request. NSURLSession is designed to
// be reused and doing so keeps connection pooling and TLS session resumption,
// which matters for the YouTube poller's steady request cadence.
NSURLSession* SharedHttpSession()
{
    static NSURLSession* shared = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSURLSessionConfiguration* cfg =
            [NSURLSessionConfiguration ephemeralSessionConfiguration];
        // The plugin drives its own polling cadence and caches nothing through
        // this layer; a URL cache here would serve stale chat pages.
        cfg.URLCache = nil;
        cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        shared = [NSURLSession sessionWithConfiguration:cfg];
    });
    return shared;
}

std::string BuildUrl(const MacHttpRequest* req)
{
    const MacHttpConnect* c = req->connect;
    std::string url = (c->secure ? "https://" : "http://") + c->host;
    const bool defaultPort =
        (c->secure && c->port == 443) || (!c->secure && c->port == 80);
    if (!defaultPort) url += ":" + std::to_string((unsigned)c->port);
    if (req->path.empty() || req->path.front() != '/') url += "/";
    url += req->path;
    return url;
}

// WinHttpAddRequestHeaders takes whole "Name: value\r\n" lines, sometimes
// several at once. Split them back into pairs so NSMutableURLRequest can take
// them, since it sets headers by name.
void AppendHeaderLines(MacHttpRequest* req, const std::string& block)
{
    size_t pos = 0;
    while (pos < block.size()) {
        size_t end = block.find("\r\n", pos);
        if (end == std::string::npos) end = block.size();
        std::string line = block.substr(pos, end - pos);
        pos = (end == block.size()) ? end : end + 2;

        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        std::string name  = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        const size_t first = value.find_first_not_of(" \t");
        value = (first == std::string::npos) ? "" : value.substr(first);
        req->headers.emplace_back(std::move(name), std::move(value));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Session / connection / request handles
// ---------------------------------------------------------------------------

HINTERNET WinHttpOpen(const wchar_t* userAgent, DWORD /*accessType*/,
                      const wchar_t* /*proxy*/, const wchar_t* /*proxyBypass*/,
                      DWORD /*flags*/)
{
    // Proxy arguments are ignored on purpose. feeds-http.h resolves DIRECT here
    // (its proxy lookups are the one part of this surface left unimplemented),
    // and NSURLSession applies the system proxy configuration itself, so the
    // user's proxy is honoured without this layer knowing about it.
    auto* s = new MacHttpSession();
    s->userAgent = Narrow(userAgent);
    return s;
}

BOOL WinHttpSetTimeouts(HINTERNET h, int /*resolve*/, int /*connect*/,
                        int send, int receive)
{
    MacHttpSession* s = AsSession(h);
    if (!s) return FALSE;
    s->sendTimeoutMs    = send;
    s->receiveTimeoutMs = receive;
    return TRUE;
}

BOOL WinHttpSetOption(HINTERNET h, DWORD option, LPVOID /*value*/, DWORD /*length*/)
{
    // The upgrade flag is the only option with a meaning here, and it must
    // report success: the Twitch connect path tests it inside a boolean chain
    // and abandons the connection if it fails.
    if (option == WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET) {
        MacHttpRequest* r = AsRequest(h);
        if (!r) return FALSE;
        r->websocketUpgrade = true;
        return TRUE;
    }

    // Everything else the plugin sets is a Windows-specific knob with no
    // counterpart: TLS protocol floors (NSURLSession negotiates modern TLS on
    // its own) and the proxy auto-logon policy. Accepted so callers proceed.
    return TRUE;
}

HINTERNET WinHttpConnect(HINTERNET h, const wchar_t* host,
                         INTERNET_PORT port, DWORD /*reserved*/)
{
    MacHttpSession* s = AsSession(h);
    if (!s || !host) return nullptr;
    auto* c = new MacHttpConnect();
    c->session = s;
    c->host    = Narrow(host);
    c->port    = port;
    return c;
}

HINTERNET WinHttpOpenRequest(HINTERNET h, const wchar_t* verb,
                             const wchar_t* objectName, const wchar_t* /*version*/,
                             const wchar_t* /*referrer*/, const wchar_t** /*acceptTypes*/,
                             DWORD flags)
{
    MacHttpConnect* c = AsConnect(h);
    if (!c) return nullptr;
    auto* r = new MacHttpRequest();
    r->connect = c;
    r->verb    = verb ? Narrow(verb) : "GET";
    r->path    = objectName ? Narrow(objectName) : "/";
    c->secure  = (flags & WINHTTP_FLAG_SECURE) != 0;
    return r;
}

BOOL WinHttpAddRequestHeaders(HINTERNET h, const wchar_t* headers,
                              DWORD /*length*/, DWORD /*modifiers*/)
{
    MacHttpRequest* r = AsRequest(h);
    if (!r || !headers) return FALSE;
    AppendHeaderLines(r, Narrow(headers));
    return TRUE;
}

// ---------------------------------------------------------------------------
// The request itself
//
// WinHTTP splits sending from reading the response; NSURLSession does not. The
// whole exchange therefore happens in SendRequest, and ReceiveResponse just
// reports whether it worked. That ordering is invisible to the callers, which
// always call the two in sequence and check both.
// ---------------------------------------------------------------------------
BOOL WinHttpSendRequest(HINTERNET h, const wchar_t* extraHeaders,
                        DWORD /*headersLength*/, LPVOID optional,
                        DWORD optionalLength, DWORD /*totalLength*/,
                        DWORD_PTR /*context*/)
{
    MacHttpRequest* r = AsRequest(h);
    if (!r || !r->connect || !r->connect->session) return FALSE;

    // A WebSocket upgrade sends nothing here. The socket is opened in
    // WinHttpWebSocketCompleteUpgrade, so this and ReceiveResponse simply
    // succeed and let the caller reach it.
    if (r->websocketUpgrade) return TRUE;

    if (extraHeaders) AppendHeaderLines(r, Narrow(extraHeaders));

    @autoreleasepool {
        NSString* urlString = NsFrom(BuildUrl(r));
        NSURL* url = [NSURL URLWithString:urlString];
        if (!url) return FALSE;

        NSMutableURLRequest* request =
            [NSMutableURLRequest requestWithURL:url];
        request.HTTPMethod = NsFrom(r->verb);

        const int timeoutMs = r->connect->session->receiveTimeoutMs > 0
            ? r->connect->session->receiveTimeoutMs
            : 30000;
        request.timeoutInterval = (NSTimeInterval)timeoutMs / 1000.0;

        if (!r->connect->session->userAgent.empty()) {
            [request setValue:NsFrom(r->connect->session->userAgent)
                 forHTTPHeaderField:@"User-Agent"];
        }
        for (const auto& kv : r->headers) {
            [request setValue:NsFrom(kv.second)
                 forHTTPHeaderField:NsFrom(kv.first)];
        }
        if (optional && optionalLength > 0) {
            request.HTTPBody = [NSData dataWithBytes:optional
                                              length:optionalLength];
        }

        __block NSData*          outData   = nil;
        __block NSHTTPURLResponse* outResp = nil;
        __block BOOL             ok        = NO;

        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        NSURLSessionDataTask* task = [SharedHttpSession()
            dataTaskWithRequest:request
              completionHandler:^(NSData* data, NSURLResponse* response,
                                  NSError* error) {
                  if (!error && [response isKindOfClass:[NSHTTPURLResponse class]]) {
                      outData = data;
                      outResp = (NSHTTPURLResponse*)response;
                      ok = YES;
                  }
                  dispatch_semaphore_signal(done);
              }];
        [task resume];

        // Bounded wait rather than DISPATCH_TIME_FOREVER: timeoutInterval
        // normally ends the task on its own, but a hung task must not pin a
        // plugin thread for the life of the process. The margin is generous so
        // the URL loading system, not this, is what normally times out.
        const int64_t waitNs =
            (int64_t)(timeoutMs + 15000) * NSEC_PER_MSEC;
        if (dispatch_semaphore_wait(done,
                dispatch_time(DISPATCH_TIME_NOW, waitNs)) != 0) {
            [task cancel];
            return FALSE;
        }
        if (!ok) return FALSE;

        r->status = (DWORD)outResp.statusCode;
        if (outData.length > 0) {
            const uint8_t* p = (const uint8_t*)outData.bytes;
            r->body.assign(p, p + outData.length);
        }
        r->readOffset = 0;
        r->responded  = true;
        return TRUE;
    }
}

BOOL WinHttpReceiveResponse(HINTERNET h, LPVOID /*reserved*/)
{
    MacHttpRequest* r = AsRequest(h);
    if (!r) return FALSE;
    if (r->websocketUpgrade) return TRUE;
    return r->responded ? TRUE : FALSE;
}

BOOL WinHttpQueryHeaders(HINTERNET h, DWORD infoLevel, const wchar_t* /*name*/,
                         LPVOID buffer, LPDWORD bufferLength, LPDWORD /*index*/)
{
    MacHttpRequest* r = AsRequest(h);
    if (!r || !r->responded) return FALSE;

    // The plugin asks for exactly one thing: the status code as a number.
    if (infoLevel == (WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER)) {
        if (!buffer || !bufferLength || *bufferLength < sizeof(DWORD)) return FALSE;
        *(DWORD*)buffer = r->status;
        *bufferLength = sizeof(DWORD);
        return TRUE;
    }
    return FALSE;
}

// Drains the buffered body. A zero read with a TRUE return is the end of the
// response, which is the loop condition every caller uses.
BOOL WinHttpReadData(HINTERNET h, LPVOID buffer, DWORD toRead, LPDWORD read)
{
    if (read) *read = 0;
    MacHttpRequest* r = AsRequest(h);
    if (!r || !r->responded || !buffer) return FALSE;

    const size_t remaining = r->body.size() - r->readOffset;
    const size_t n = remaining < (size_t)toRead ? remaining : (size_t)toRead;
    if (n > 0) {
        memcpy(buffer, r->body.data() + r->readOffset, n);
        r->readOffset += n;
    }
    if (read) *read = (DWORD)n;
    return TRUE;
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------

HINTERNET WinHttpWebSocketCompleteUpgrade(HINTERNET h, DWORD_PTR /*context*/)
{
    MacHttpRequest* r = AsRequest(h);
    if (!r || !r->websocketUpgrade || !r->connect || !r->connect->session) {
        return nullptr;
    }

    @autoreleasepool {
        // wss:// rather than https:// — NSURLSession selects the WebSocket task
        // from the scheme.
        std::string url = BuildUrl(r);
        if (url.compare(0, 8, "https://") == 0) url = "wss://" + url.substr(8);
        else if (url.compare(0, 7, "http://") == 0) url = "ws://" + url.substr(7);

        NSURL* nsurl = [NSURL URLWithString:NsFrom(url)];
        if (!nsurl) return nullptr;

        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:nsurl];
        if (!r->connect->session->userAgent.empty()) {
            [request setValue:NsFrom(r->connect->session->userAgent)
                 forHTTPHeaderField:@"User-Agent"];
        }
        for (const auto& kv : r->headers) {
            [request setValue:NsFrom(kv.second)
                 forHTTPHeaderField:NsFrom(kv.first)];
        }

        auto ws = std::make_shared<MacWebSocket>();

        // Its own session, not the shared one: the socket is long-lived and is
        // torn down by invalidating this session, which must not disturb the
        // request pool the pollers are using.
        NSURLSessionConfiguration* cfg =
            [NSURLSessionConfiguration ephemeralSessionConfiguration];
        ws->session = [NSURLSession sessionWithConfiguration:cfg];
        ws->task = [ws->session webSocketTaskWithRequest:request];
        if (!ws->task) {
            [ws->session invalidateAndCancel];
            return nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(SocketRegistryMutex());
            SocketRegistry()[ws.get()] = ws;
        }

        [ws->task resume];
        PumpReceive(ws);
        return ws.get();
    }
}

DWORD WinHttpWebSocketSend(HINTERNET h, WINHTTP_WEB_SOCKET_BUFFER_TYPE type,
                           PVOID buffer, DWORD length)
{
    std::shared_ptr<MacWebSocket> ws = LookupSocket(h);
    if (!ws || !buffer) return ERROR_NOT_SUPPORTED;
    {
        std::lock_guard<std::mutex> lock(ws->mutex);
        if (ws->closed) return ERROR_NOT_SUPPORTED;
    }

    @autoreleasepool {
        NSURLSessionWebSocketMessage* message = nil;
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            NSString* text =
                [[NSString alloc] initWithBytes:buffer
                                         length:length
                                       encoding:NSUTF8StringEncoding];
            if (!text) return ERROR_NOT_SUPPORTED;
            message = [[NSURLSessionWebSocketMessage alloc] initWithString:text];
        } else {
            NSData* data = [NSData dataWithBytes:buffer length:length];
            message = [[NSURLSessionWebSocketMessage alloc] initWithData:data];
        }

        // Waited on rather than fired and forgotten, so the caller's "did the
        // send land" contract still means something. The IRC handshake sends
        // four lines back to back and a silent failure there would look like a
        // channel that simply never produces messages.
        __block BOOL ok = NO;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [ws->task sendMessage:message completionHandler:^(NSError* error) {
            ok = (error == nil);
            dispatch_semaphore_signal(done);
        }];
        const int64_t waitNs = (int64_t)15000 * NSEC_PER_MSEC;
        if (dispatch_semaphore_wait(done,
                dispatch_time(DISPATCH_TIME_NOW, waitNs)) != 0) {
            return ERROR_NOT_SUPPORTED;
        }
        return ok ? NO_ERROR : ERROR_NOT_SUPPORTED;
    }
}

DWORD WinHttpWebSocketReceive(HINTERNET h, PVOID buffer, DWORD length,
                              LPDWORD read, WINHTTP_WEB_SOCKET_BUFFER_TYPE* type)
{
    if (read) *read = 0;
    std::shared_ptr<MacWebSocket> ws = LookupSocket(h);
    if (!ws || !buffer || length == 0) return ERROR_NOT_SUPPORTED;

    std::unique_lock<std::mutex> lock(ws->mutex);
    MacWebSocket* raw = ws.get();
    ws->cv.wait(lock, [raw] { return raw->closed || !raw->inbox.empty(); });

    // Closed wins only when there is nothing left to hand over, so a message
    // that arrived just before the close is still delivered.
    if (ws->inbox.empty()) return ERROR_NOT_SUPPORTED;

    std::vector<uint8_t>& front = ws->inbox.front();
    const size_t remaining = front.size() - ws->frontOffset;
    const size_t n = remaining < (size_t)length ? remaining : (size_t)length;
    memcpy(buffer, front.data() + ws->frontOffset, n);
    ws->frontOffset += n;

    const bool complete = (ws->frontOffset >= front.size());
    if (complete) {
        ws->inbox.pop_front();
        ws->frontOffset = 0;
    }

    if (read) *read = (DWORD)n;
    if (type) {
        // The caller only distinguishes a close frame from everything else, but
        // report the fragment type honestly anyway: a message too large for the
        // caller's buffer is delivered across several calls.
        *type = complete ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
                         : WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
    }
    return NO_ERROR;
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------
BOOL WinHttpCloseHandle(HINTERNET h)
{
    if (std::shared_ptr<MacWebSocket> ws = LookupSocket(h)) {
        // Drop the registry's reference first, so a second close of the same
        // handle finds nothing and does nothing. The Twitch reader closes this
        // handle from two places by design, settling the race in TwCloseSocket;
        // this is the backstop for it.
        {
            std::lock_guard<std::mutex> lock(SocketRegistryMutex());
            SocketRegistry().erase(ws.get());
        }
        // Wake anything parked in Receive BEFORE tearing the task down, so the
        // reader's cross-thread abort returns promptly rather than waiting on a
        // message that will never arrive.
        ws->Fail();
        @autoreleasepool {
            [ws->task cancel];
            [ws->session invalidateAndCancel];
        }
        // No delete: the local shared_ptr is the last strong reference unless a
        // receive completion is still running, and whichever finishes last
        // destroys it.
        return TRUE;
    }
    if (MacHttpRequest* r = AsRequest(h)) { r->magic = 0; delete r; return TRUE; }
    if (MacHttpConnect* c = AsConnect(h)) { c->magic = 0; delete c; return TRUE; }
    if (MacHttpSession* s = AsSession(h)) { s->magic = 0; delete s; return TRUE; }
    return TRUE;
}
