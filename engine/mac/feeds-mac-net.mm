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

    __block NSData*       blockData     = nil;
    __block NSURLResponse* blockResponse = nil;
    __block NSError*      blockError    = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);

    NSURLSessionDataTask* task = [session
        dataTaskWithRequest:request
          completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
              blockData     = [data retain];
              blockResponse = [response retain];
              blockError    = [error retain];
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
        dispatch_release(done);
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

    [blockData release];
    [blockResponse release];
    [blockError release];
    dispatch_release(done);
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

namespace {

NSMutableDictionary* KeychainQuery(const std::string& account)
{
    NSMutableDictionary* query = [NSMutableDictionary dictionary];
    query[(id)kSecClass]       = (id)kSecClassGenericPassword;
    query[(id)kSecAttrService] = kKeychainService;
    query[(id)kSecAttrAccount] = NsFrom(account);
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
        NSDictionary* update = @{ (id)kSecValueData : data };
        OSStatus st = SecItemUpdate((CFDictionaryRef)query,
                                    (CFDictionaryRef)update);
        if (st == errSecItemNotFound) {
            NSMutableDictionary* add = [query mutableCopy];
            add[(id)kSecValueData] = data;
            // After first unlock, so a refresh can run before the user has
            // touched the machine, but never synced to iCloud or another Mac:
            // these are this device's tokens.
            add[(id)kSecAttrAccessible] =
                (id)kSecAttrAccessibleAfterFirstUnlock;
            st = SecItemAdd((CFDictionaryRef)add, NULL);
            [add release];
        }
        return st == errSecSuccess;
    }
}

std::string KeychainGet(const std::string& account)
{
    @autoreleasepool {
        NSMutableDictionary* query = KeychainQuery(account);
        query[(id)kSecReturnData]  = @YES;
        query[(id)kSecMatchLimit]  = (id)kSecMatchLimitOne;

        CFTypeRef result = NULL;
        const OSStatus st = SecItemCopyMatching((CFDictionaryRef)query,
                                                &result);
        if (st != errSecSuccess || !result) return {};

        NSData* data = (NSData*)result;
        std::string out;
        if (data.length > 0) out.assign((const char*)data.bytes, data.length);
        CFRelease(result);
        return out;
    }
}

void KeychainDelete(const std::string& account)
{
    @autoreleasepool {
        SecItemDelete((CFDictionaryRef)KeychainQuery(account));
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
