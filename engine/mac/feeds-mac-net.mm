// feeds-mac-net.mm — macOS implementations of the primitives in feeds-mac-net.h.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <CommonCrypto/CommonDigest.h>

#include <cstdio>

#include "feeds-mac-net.h"

namespace feeds_mac {

namespace {

// The Keychain service every Feeds item lives under. The account names match
// the Windows credential target names ("Feeds_AccessToken" and friends) so the
// two platforms describe the same three secrets in the same words.
NSString* const kKeychainService = @"Feeds (Zoom login)";

NSString* NsFrom(const std::string& s)
{
    NSString* out = [NSString stringWithUTF8String:s.c_str()];
    return out ? out : @"";
}

std::string StdFrom(NSString* s)
{
    if (!s) return {};
    const char* c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

// One synchronous request. NSURLSession is asynchronous by design, so the
// completion handler signals a semaphore this thread waits on. Safe only
// because every caller is already on a background thread (see the header).
HttpResponse PerformRequest(NSMutableURLRequest* request, int timeoutSeconds)
{
    HttpResponse out;
    if (!request) {
        out.error = "could not build the request";
        return out;
    }

    request.timeoutInterval = (NSTimeInterval)timeoutSeconds;

    // An ephemeral configuration keeps cookies and caches out of the picture:
    // every one of these calls is an API request with an explicit credential,
    // and a cached 401 or a stale cookie would be its own class of bug.
    NSURLSessionConfiguration* config =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    config.timeoutIntervalForRequest  = (NSTimeInterval)timeoutSeconds;
    config.timeoutIntervalForResource = (NSTimeInterval)timeoutSeconds;
    NSURLSession* session = [NSURLSession sessionWithConfiguration:config];

    // Under ARC a __block object pointer is a strong reference, so simply
    // assigning inside the handler keeps the result alive until this function
    // returns; the handler's own parameters die with it either way.
    __block NSData*        blockData     = nil;
    __block NSURLResponse* blockResponse = nil;
    __block NSError*       blockError    = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);

    NSURLSessionDataTask* task = [session
        dataTaskWithRequest:request
          completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
              blockData     = data;
              blockResponse = response;
              blockError    = error;
              dispatch_semaphore_signal(done);
          }];
    [task resume];

    // Bounded wait: the completion handler above is the only signaller, and the
    // session's own timeouts guarantee it fires. The extra few seconds are so a
    // wait can never outlive the transport's own deadline by much.
    const dispatch_time_t deadline =
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeoutSeconds + 5) * NSEC_PER_SEC);
    if (dispatch_semaphore_wait(done, deadline) != 0) {
        [task cancel];
        [session invalidateAndCancel];
        out.error = "request timed out";
        return out;
    }

    if (blockError) {
        out.error = StdFrom(blockError.localizedDescription);
    } else if ([blockResponse isKindOfClass:[NSHTTPURLResponse class]]) {
        out.reached = true;
        out.status  = (int)((NSHTTPURLResponse*)blockResponse).statusCode;
        if (blockData.length > 0) {
            out.body.assign((const char*)blockData.bytes, blockData.length);
        }
    } else {
        out.error = "no HTTP response";
    }

    [session finishTasksAndInvalidate];
    return out;
}

NSMutableURLRequest* BuildRequest(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    NSURL* nsurl = [NSURL URLWithString:NsFrom(url)];
    if (!nsurl) return nil;
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:nsurl];
    for (const auto& h : headers) {
        [request setValue:NsFrom(h.second) forHTTPHeaderField:NsFrom(h.first)];
    }
    return request;
}

} // namespace

HttpResponse HttpGet(const std::string& url,
                     const std::vector<std::pair<std::string, std::string>>& headers,
                     int timeoutSeconds)
{
    @autoreleasepool {
        NSMutableURLRequest* request = BuildRequest(url, headers);
        request.HTTPMethod = @"GET";
        return PerformRequest(request, timeoutSeconds);
    }
}

HttpResponse HttpPostForm(const std::string& url, const std::string& body,
                          const std::vector<std::pair<std::string, std::string>>& headers,
                          int timeoutSeconds)
{
    @autoreleasepool {
        NSMutableURLRequest* request = BuildRequest(url, headers);
        request.HTTPMethod = @"POST";
        if (![request valueForHTTPHeaderField:@"Content-Type"]) {
            [request setValue:@"application/x-www-form-urlencoded"
                 forHTTPHeaderField:@"Content-Type"];
        }
        request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
        return PerformRequest(request, timeoutSeconds);
    }
}

// ---------------------------------------------------------------------------
// PKCE / encoding
// ---------------------------------------------------------------------------

std::string UrlEncode(const std::string& s)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out += (char)c;
        } else {
            out += '%';
            out += kHex[(c >> 4) & 0xF];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

namespace {

std::string Base64Url(const unsigned char* data, size_t len)
{
    @autoreleasepool {
        NSData* raw = [NSData dataWithBytes:data length:len];
        NSString* b64 = [raw base64EncodedStringWithOptions:0];
        std::string out = StdFrom(b64);
        for (char& c : out) {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }
        while (!out.empty() && out.back() == '=') out.pop_back();
        return out;
    }
}

} // namespace

std::string RandomUrlSafeToken()
{
    unsigned char buf[32];
    if (SecRandomCopyBytes(kSecRandomDefault, sizeof(buf), buf) != errSecSuccess) {
        // Never silently fall back to a weaker source: the PKCE verifier and
        // the OAuth state both depend on this being unguessable. An empty
        // return aborts the login, which is the safe failure.
        return {};
    }
    return Base64Url(buf, sizeof(buf));
}

std::string Sha256Base64Url(const std::string& input)
{
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(input.data(), (CC_LONG)input.size(), digest);
    return Base64Url(digest, sizeof(digest));
}

// ---------------------------------------------------------------------------
// Keychain
// ---------------------------------------------------------------------------

// Defined in feeds-engine-mac.mm and declared in feeds-mac-login.h, which this
// file deliberately does not include: the net layer sits under the login
// module, not beside it. One declaration is cheaper than the dependency.
//
// NOTE the placement: this has to sit in namespace feeds_mac, NOT in the
// anonymous namespace below, or it would declare a different symbol with
// internal linkage and nothing would define it.
void EngineLog(const char* level, const std::string& message);

namespace {

// EVERY Keychain touch is logged, with the item and the operation.
//
// This exists because a prompt is not observable from inside the process: macOS
// shows it, the user answers it, and SecItem* simply returns. The only way to
// know how many prompts a session really costs is to count the accesses that
// can cause one, so the log does the counting. A previous attempt at reducing
// these prompts was reasoned about rather than measured, and reduced the wrong
// ones; this makes the next run answer the question directly.
void LogKeychain(const char* op, const std::string& account, OSStatus st)
{
    EngineLog("debug", std::string("Keychain: ") + op + " '" + account +
                       "' -> " + std::to_string((int)st));
}

// The kSec* constants are CFStringRefs, so every use as a dictionary key or
// value crosses the CoreFoundation/Objective-C line. __bridge is the right
// annotation throughout: these are immortal process-lifetime constants, nothing
// is being handed to or taken from ARC, and a transferring cast would hand ARC
// a release it does not own.
NSMutableDictionary* KeychainQuery(const std::string& account)
{
    NSMutableDictionary* query = [NSMutableDictionary dictionary];
    query[(__bridge id)kSecClass]       = (__bridge id)kSecClassGenericPassword;
    query[(__bridge id)kSecAttrService] = kKeychainService;
    query[(__bridge id)kSecAttrAccount] = NsFrom(account);
    return query;
}

} // namespace

bool KeychainSet(const std::string& account, const std::string& value)
{
    @autoreleasepool {
        NSMutableDictionary* query = KeychainQuery(account);
        NSData* data = [NSData dataWithBytes:value.data() length:value.size()];

        // Update an existing item in place when there is one: deleting and
        // re-adding would drop the user's "always allow" decision and prompt
        // them again on every token refresh.
        NSDictionary* update = @{ (__bridge id)kSecValueData : data };
        OSStatus st = SecItemUpdate((__bridge CFDictionaryRef)query,
                                    (__bridge CFDictionaryRef)update);
        if (st == errSecItemNotFound) {
            NSMutableDictionary* add = [query mutableCopy];
            add[(__bridge id)kSecValueData] = data;
            // After first unlock, so a refresh can run before the user has
            // touched the machine, but never synced to iCloud or another Mac:
            // these are this device's tokens.
            add[(__bridge id)kSecAttrAccessible] =
                (__bridge id)kSecAttrAccessibleAfterFirstUnlock;
            st = SecItemAdd((__bridge CFDictionaryRef)add, NULL);
            LogKeychain("add", account, st);
        } else {
            LogKeychain("update", account, st);
        }
        return st == errSecSuccess;
    }
}

std::string KeychainGet(const std::string& account)
{
    @autoreleasepool {
        NSMutableDictionary* query = KeychainQuery(account);
        query[(__bridge id)kSecReturnData] = @YES;
        query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;

        CFTypeRef result = NULL;
        const OSStatus st = SecItemCopyMatching((__bridge CFDictionaryRef)query,
                                                &result);

        // SecItemCopyMatching follows the Copy rule: it hands back a +1
        // reference. __bridge_transfer moves that straight into ARC, before the
        // checks below, so no early return can leak it and nothing has to
        // remember a matching CFRelease — which under ARC would be a double
        // release, not a fix.
        id item = (__bridge_transfer id)result;
        LogKeychain("read", account, st);
        if (st != errSecSuccess || ![item isKindOfClass:[NSData class]]) return {};

        NSData* data = (NSData*)item;
        std::string out;
        if (data.length > 0) out.assign((const char*)data.bytes, data.length);
        return out;
    }
}

void KeychainDelete(const std::string& account)
{
    @autoreleasepool {
        const OSStatus st =
            SecItemDelete((__bridge CFDictionaryRef)KeychainQuery(account));
        LogKeychain("delete", account, st);
    }
}

// ---------------------------------------------------------------------------
// Preferences (see the header for why these are not Keychain items)
// ---------------------------------------------------------------------------
namespace {
NSString* PrefsKey(const std::string& key)
{
    return [NSString stringWithFormat:@"com.letsdovideo.feeds.%s", key.c_str()];
}
}  // namespace

void PrefsSetInt(const std::string& key, int value)
{
    @autoreleasepool {
        [[NSUserDefaults standardUserDefaults] setInteger:value
                                                   forKey:PrefsKey(key)];
    }
}

int PrefsGetInt(const std::string& key, int fallback)
{
    @autoreleasepool {
        NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
        NSString* k = PrefsKey(key);
        // objectForKey distinguishes "never set" from "set to 0"; integerForKey
        // alone would collapse the two, and a cached tier of 0 (Free) is a real
        // answer that must not look like "never cached".
        if ([defaults objectForKey:k] == nil) return fallback;
        return (int)[defaults integerForKey:k];
    }
}

void PrefsRemove(const std::string& key)
{
    @autoreleasepool {
        [[NSUserDefaults standardUserDefaults] removeObjectForKey:PrefsKey(key)];
    }
}

// ---------------------------------------------------------------------------
// Browser
// ---------------------------------------------------------------------------

bool OpenInBrowser(const std::string& url)
{
    @autoreleasepool {
        NSURL* nsurl = [NSURL URLWithString:NsFrom(url)];
        if (!nsurl) return false;
        return [[NSWorkspace sharedWorkspace] openURL:nsurl] ? true : false;
    }
}

} // namespace feeds_mac
