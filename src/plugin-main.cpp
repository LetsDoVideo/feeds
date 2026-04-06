#include <winsock2.h>
#include <ws2tcpip.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <string>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include <thread>
#include <sstream>
#include <iomanip>
#include <random>

// Crypto for SHA-256 (Windows native, no OpenSSL needed)
#include <wincrypt.h>
#include <winhttp.h>
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

// Qt Headers
#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QInputDialog>
#include <QRegularExpression>
#include <QTimer>

// 1. Master Dictionaries
#include "zoom_sdk_def.h"
#include "zoom_sdk_raw_data_def.h"

// 2. Core Engine
#include "zoom_sdk.h"
#include "auth_service_interface.h"
#include "meeting_service_interface.h"

// 3. Meeting Components
#include "meeting_service_components/meeting_audio_interface.h"
#include "meeting_service_components/meeting_participants_ctrl_interface.h"
#include "meeting_service_components/meeting_recording_interface.h"
#include "meeting_service_components/meeting_live_stream_interface.h"
#include "meeting_service_components/meeting_sharing_interface.h"

// 4. Raw Data
#include "rawdata/rawdata_renderer_interface.h"
#include "rawdata/zoom_rawdata_api.h"

// ---------------------------------------------------------------------------
// TIER GATING
// 0 = Free (1 feed), 1 = Basic (3 feeds), 2 = Streamer (5 feeds), 3 = Broadcaster (8 feeds)
// ---------------------------------------------------------------------------
static int g_currentTier = 1;
static int g_activeParticipantSources = 0;

static int GetMaxFeedsForTier() {
    switch (g_currentTier) {
        case 1:  return 3;
        case 2:  return 5;
        case 3:  return 8;
        default: return 1;
    }
}

static ZOOM_SDK_NAMESPACE::ZoomSDKResolution GetResolutionForTier() {
    return (g_currentTier >= 1) ? ZOOM_SDK_NAMESPACE::ZoomSDKResolution_1080P
                                : ZOOM_SDK_NAMESPACE::ZoomSDKResolution_720P;
}

// ---------------------------------------------------------------------------
// GLOBALS
// ---------------------------------------------------------------------------
static bool     g_rawLiveStreamGranted = false;
static QAction* g_connectAction        = nullptr;
static QAction* g_loginAction          = nullptr;
static QAction* g_logoutAction         = nullptr;
static bool     g_isLoggedIn           = false;
static bool     g_pendingMeetingJoin   = false;

static unsigned int g_activeSharerUserId  = 0;
static unsigned int g_activeShareSourceId = 0;
static unsigned int g_activeSpeakerUserId = 0;

// PKCE state
static std::string g_pkceVerifier;
static std::string g_accessToken;
static std::string g_refreshToken;
static std::string g_userDisplayName;

// ---------------------------------------------------------------------------
// PKCE HELPERS
// ---------------------------------------------------------------------------

// URL-safe base64 (no padding, + -> -, / -> _)
static std::string Base64UrlEncode(const unsigned char* data, size_t len) {
    DWORD encoded_len = 0;
    CryptBinaryToStringA(data, (DWORD)len,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         nullptr, &encoded_len);
    std::string encoded(encoded_len, '\0');
    CryptBinaryToStringA(data, (DWORD)len,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         &encoded[0], &encoded_len);
    while (!encoded.empty() && encoded.back() == '\0')
        encoded.pop_back();
    for (char& c : encoded) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!encoded.empty() && encoded.back() == '=')
        encoded.pop_back();
    return encoded;
}

// Generate a cryptographically random code_verifier
static std::string GenerateCodeVerifier() {
    unsigned char buf[32];
    HCRYPTPROV hProv = 0;
    CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL,
                        CRYPT_VERIFYCONTEXT);
    CryptGenRandom(hProv, sizeof(buf), buf);
    CryptReleaseContext(hProv, 0);
    return Base64UrlEncode(buf, sizeof(buf));
}

// SHA-256 hash using Windows native crypto
static std::vector<unsigned char> SHA256Hash(const std::string& input) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::vector<unsigned char> result(32);
    CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES,
                        CRYPT_VERIFYCONTEXT);
    CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash);
    CryptHashData(hHash, (const BYTE*)input.data(), (DWORD)input.size(), 0);
    DWORD hashLen = 32;
    CryptGetHashParam(hHash, HP_HASHVAL, result.data(), &hashLen, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return result;
}

// Derive code_challenge = BASE64URL(SHA256(verifier))
static std::string DeriveCodeChallenge(const std::string& verifier) {
    auto hash = SHA256Hash(verifier);
    return Base64UrlEncode(hash.data(), hash.size());
}

// URL-encode a string for use in POST bodies
static std::string UrlEncode(const std::string& s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::uppercase << std::hex
                << std::setw(2) << std::setfill('0') << (int)c;
        }
    }
    return out.str();
}

// Extract a query parameter value from a URL/request string
static std::string ExtractQueryParam(const std::string& text,
                                     const std::string& param) {
    std::string key = param + "=";
    size_t pos = text.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    size_t end = text.find_first_of("& \r\n", pos);
    return text.substr(pos, end == std::string::npos ? std::string::npos
                                                      : end - pos);
}

// Token exchange via WinHTTP (handles TLS to zoom.us natively)
static std::string ExchangeCodeForToken(const std::string& code,
                                        const std::string& verifier) {
    std::string body =
        std::string("grant_type=authorization_code") +
        "&code="          + UrlEncode(code) +
        "&client_id=JlP6KfRqTt6r0t67FcDuqQ" +
        "&redirect_uri="  + UrlEncode("http://localhost:9847/callback") +
        "&code_verifier=" + UrlEncode(verifier);

    HINTERNET hSession = WinHttpOpen(L"Feeds/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(hSession, L"zoom.us",
                                         INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/oauth/token",
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);

    WinHttpAddRequestHeaders(hRequest,
        L"Content-Type: application/x-www-form-urlencoded",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    // Basic auth with client_id and empty secret for public client
    std::string credStr = "JlP6KfRqTt6r0t67FcDuqQ:";
    DWORD b64Len = 0;
    CryptBinaryToStringA((const BYTE*)credStr.data(), (DWORD)credStr.size(),
                          CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                          nullptr, &b64Len);
    std::string b64(b64Len, '\0');
    CryptBinaryToStringA((const BYTE*)credStr.data(), (DWORD)credStr.size(),
                          CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                          &b64[0], &b64Len);
    while (!b64.empty() && (b64.back() == '\0' || b64.back() == '\n'))
        b64.pop_back();
    std::wstring basicAuth = L"Authorization: Basic " +
                              std::wstring(b64.begin(), b64.end());
    WinHttpAddRequestHeaders(hRequest, basicAuth.c_str(),
                             (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                       (LPVOID)body.c_str(), (DWORD)body.size(),
                       (DWORD)body.size(), 0);
    WinHttpReceiveResponse(hRequest, nullptr);

    std::string response;
    char buf[4096];
    DWORD bytesRead = 0;
    while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead) &&
           bytesRead > 0) {
        buf[bytesRead] = '\0';
        response += buf;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

// Minimal JSON string field extractor
static std::string JsonExtractString(const std::string& json,
                                     const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + search.size() + 1);
    if (pos == std::string::npos) return "";
    pos++;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// ---------------------------------------------------------------------------
// FORWARD DECLARATIONS
// ---------------------------------------------------------------------------
void OnLoginClick();
void OnLogoutClick();
void OnConnectClick();
void DoSDKAuth();

// ---------------------------------------------------------------------------
// PER-SOURCE VIDEO CATCHER
// ---------------------------------------------------------------------------
class ZoomVideoCatcher : public ZOOM_SDK_NAMESPACE::IZoomSDKRendererDelegate {
public:
    obs_source_t* target = nullptr;
    uint64_t next_timestamp = 0;

    virtual void onRawDataFrameReceived(YUVRawDataI420* data) override {
        if (!target) return;
        unsigned int width  = data->GetStreamWidth();
        unsigned int height = data->GetStreamHeight();

        struct obs_source_frame obs_frame = {};
        obs_frame.format      = VIDEO_FORMAT_I420;
        obs_frame.width       = width;
        obs_frame.height      = height;
        obs_frame.data[0]     = (uint8_t*)data->GetYBuffer();
        obs_frame.data[1]     = (uint8_t*)data->GetUBuffer();
        obs_frame.data[2]     = (uint8_t*)data->GetVBuffer();
        obs_frame.linesize[0] = width;
        obs_frame.linesize[1] = width / 2;
        obs_frame.linesize[2] = width / 2;

        video_format_get_parameters(VIDEO_CS_DEFAULT, VIDEO_RANGE_PARTIAL,
                                    obs_frame.color_matrix,
                                    obs_frame.color_range_min,
                                    obs_frame.color_range_max);

        uint64_t now = os_gettime_ns();
        if (next_timestamp == 0 || next_timestamp < now - 100000000ULL)
            next_timestamp = now;
        obs_frame.timestamp = next_timestamp;
        next_timestamp += 33333333ULL;
        obs_source_output_video(target, &obs_frame);
    }
    virtual void onRawDataStatusChanged(RawDataStatus status) override {}
    virtual void onRendererBeDestroyed() override { target = nullptr; }
};

// ---------------------------------------------------------------------------
// SCREENSHARE GLOBALS
// ---------------------------------------------------------------------------
static obs_source_t* g_screenshareSource     = nullptr;
static uint64_t      g_screenshare_timestamp = 0;

class ZoomShareCatcher : public ZOOM_SDK_NAMESPACE::IZoomSDKRendererDelegate {
public:
    virtual void onRawDataFrameReceived(YUVRawDataI420* data) override {
        if (!g_screenshareSource) return;
        unsigned int width  = data->GetStreamWidth();
        unsigned int height = data->GetStreamHeight();

        struct obs_source_frame obs_frame = {};
        obs_frame.format      = VIDEO_FORMAT_I420;
        obs_frame.width       = width;
        obs_frame.height      = height;
        obs_frame.data[0]     = (uint8_t*)data->GetYBuffer();
        obs_frame.data[1]     = (uint8_t*)data->GetUBuffer();
        obs_frame.data[2]     = (uint8_t*)data->GetVBuffer();
        obs_frame.linesize[0] = width;
        obs_frame.linesize[1] = width / 2;
        obs_frame.linesize[2] = width / 2;

        video_format_get_parameters(VIDEO_CS_DEFAULT, VIDEO_RANGE_PARTIAL,
                                    obs_frame.color_matrix,
                                    obs_frame.color_range_min,
                                    obs_frame.color_range_max);

        uint64_t now = os_gettime_ns();
        if (g_screenshare_timestamp == 0 ||
            g_screenshare_timestamp < now - 100000000ULL)
            g_screenshare_timestamp = now;
        obs_frame.timestamp = g_screenshare_timestamp;
        g_screenshare_timestamp += 33333333ULL;
        obs_source_output_video(g_screenshareSource, &obs_frame);
    }
    virtual void onRawDataStatusChanged(RawDataStatus status) override {}
    virtual void onRendererBeDestroyed() override {}
};

static ZoomShareCatcher                      g_shareCatcher;
static ZOOM_SDK_NAMESPACE::IZoomSDKRenderer* g_shareRenderer = nullptr;

// ---------------------------------------------------------------------------
// FORWARD DECLARATION
// ---------------------------------------------------------------------------
class ZoomParticipantSource;
static std::vector<ZoomParticipantSource*> g_allParticipantSources;

// ---------------------------------------------------------------------------
// SCREENSHARE SUBSCRIPTION HELPER
// ---------------------------------------------------------------------------
static void UpdateShareSubscription() {
    if (!g_shareRenderer && g_rawLiveStreamGranted) {
        ZOOM_SDK_NAMESPACE::createRenderer(&g_shareRenderer, &g_shareCatcher);
        if (g_shareRenderer)
            g_shareRenderer->setRawDataResolution(GetResolutionForTier());
    }
    if (!g_shareRenderer) return;
    g_shareRenderer->unSubscribe();
    if (g_activeShareSourceId != 0) {
        g_shareRenderer->subscribe(g_activeShareSourceId,
                                   ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_SHARE);
    } else if (g_activeSharerUserId != 0) {
        g_shareRenderer->subscribe(g_activeSharerUserId,
                                   ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_SHARE);
    }
}

// ---------------------------------------------------------------------------
// PARTICIPANT SOURCE
// ---------------------------------------------------------------------------
class ZoomParticipantSource {
public:
    obs_source_t* source        = nullptr;
    unsigned int  current_user_id = 0;
    ZoomVideoCatcher                      videoCatcher;
    ZOOM_SDK_NAMESPACE::IZoomSDKRenderer* videoRenderer = nullptr;

    ZoomParticipantSource(obs_source_t* src) : source(src) {
        videoCatcher.target = src;
        g_activeParticipantSources++;
        g_allParticipantSources.push_back(this);
        if (g_rawLiveStreamGranted)
            initRenderer();
    }

    ~ZoomParticipantSource() {
        auto it = std::find(g_allParticipantSources.begin(),
                            g_allParticipantSources.end(), this);
        if (it != g_allParticipantSources.end())
            g_allParticipantSources.erase(it);
        if (videoRenderer) {
            videoRenderer->unSubscribe();
            ZOOM_SDK_NAMESPACE::destroyRenderer(videoRenderer);
            videoRenderer = nullptr;
        }
        videoCatcher.target = nullptr;
        g_activeParticipantSources--;
    }

    void initRenderer() {
        if (!videoRenderer) {
            ZOOM_SDK_NAMESPACE::createRenderer(&videoRenderer, &videoCatcher);
            if (videoRenderer) {
                auto it = std::find(g_allParticipantSources.begin(),
                                    g_allParticipantSources.end(), this);
                int index = (int)std::distance(
                    g_allParticipantSources.begin(), it);
                ZOOM_SDK_NAMESPACE::ZoomSDKResolution res = (index == 0)
                    ? GetResolutionForTier()
                    : ZOOM_SDK_NAMESPACE::ZoomSDKResolution_360P;
                videoRenderer->setRawDataResolution(res);
            }
        }
        videoCatcher.next_timestamp = 0;
        if (videoRenderer) {
            videoRenderer->unSubscribe();
            unsigned int effectiveId = (current_user_id == 1)
                ? g_activeSpeakerUserId : current_user_id;
            if (effectiveId != 0)
                videoRenderer->subscribe(effectiveId,
                                         ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_VIDEO);
        }
    }

    void update(obs_data_t* settings) {
        unsigned int selected_id =
            (unsigned int)obs_data_get_int(settings, "participant_id");
        current_user_id = selected_id;
        if (!videoRenderer && g_rawLiveStreamGranted)
            initRenderer();
        if (videoRenderer) {
            videoRenderer->unSubscribe();
            unsigned int effectiveId = (current_user_id == 1)
                ? g_activeSpeakerUserId : current_user_id;
            if (effectiveId != 0) {
                videoRenderer->subscribe(effectiveId,
                                         ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_VIDEO);
                QTimer::singleShot(600, [this, effectiveId]() {
                    if (videoRenderer && current_user_id != 0) {
                        videoRenderer->unSubscribe();
                        videoCatcher.next_timestamp = 0;
                        videoRenderer->subscribe(
                            effectiveId,
                            ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_VIDEO);
                    }
                });
            }
        }
    }

    void onActiveSpeakerChanged() {
        if (current_user_id != 1) return;
        if (!videoRenderer) return;
        videoRenderer->unSubscribe();
        if (g_activeSpeakerUserId != 0)
            videoRenderer->subscribe(g_activeSpeakerUserId,
                                     ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_VIDEO);
    }
};

// ---------------------------------------------------------------------------
// AUDIO LISTENER
// ---------------------------------------------------------------------------
class ZoomAudioListener : public ZOOM_SDK_NAMESPACE::IMeetingAudioCtrlEvent {
public:
    virtual void onUserActiveAudioChange(
        ZOOM_SDK_NAMESPACE::IList<unsigned int>* plstActiveAudio) override {
        if (!plstActiveAudio || plstActiveAudio->GetCount() == 0) return;
        unsigned int newSpeaker = plstActiveAudio->GetItem(0);
        if (newSpeaker == g_activeSpeakerUserId) return;
        g_activeSpeakerUserId = newSpeaker;
        for (ZoomParticipantSource* src : g_allParticipantSources)
            src->onActiveSpeakerChanged();
    }
    virtual void onUserAudioStatusChange(
        ZOOM_SDK_NAMESPACE::IList<ZOOM_SDK_NAMESPACE::IUserAudioStatus*>* lst,
        const zchar_t* strList = nullptr) override {}
    virtual void onHostRequestStartAudio(
        ZOOM_SDK_NAMESPACE::IRequestStartAudioHandler* handler_) override {}
    virtual void onJoin3rdPartyTelephonyAudio(
        const zchar_t* audioInfo) override {}
    virtual void onMuteOnEntryStatusChange(bool bEnabled) override {}
};
static ZoomAudioListener g_audioListener;

// ---------------------------------------------------------------------------
// SHARE LISTENER
// ---------------------------------------------------------------------------
class ZoomShareListener : public ZOOM_SDK_NAMESPACE::IMeetingShareCtrlEvent {
public:
    virtual void onSharingStatus(
        ZOOM_SDK_NAMESPACE::ZoomSDKSharingSourceInfo shareInfo) override {
        switch (shareInfo.status) {
            case ZOOM_SDK_NAMESPACE::Sharing_Other_Share_Begin:
            case ZOOM_SDK_NAMESPACE::Sharing_Self_Send_Begin:
                g_activeSharerUserId  = shareInfo.userid;
                g_activeShareSourceId = shareInfo.shareSourceID;
                UpdateShareSubscription();
                break;
            case ZOOM_SDK_NAMESPACE::Sharing_Other_Share_End:
            case ZOOM_SDK_NAMESPACE::Sharing_Self_Send_End:
                g_activeSharerUserId  = 0;
                g_activeShareSourceId = 0;
                UpdateShareSubscription();
                break;
            default: break;
        }
    }
    virtual void onFailedToStartShare() override {}
    virtual void onLockShareStatus(bool bLocked) override {}
    virtual void onShareContentNotification(
        ZOOM_SDK_NAMESPACE::ZoomSDKSharingSourceInfo shareInfo) override {}
    virtual void onMultiShareSwitchToSingleShareNeedConfirm(
        ZOOM_SDK_NAMESPACE::IShareSwitchMultiToSingleConfirmHandler* h) override {}
    virtual void onShareSettingTypeChangedNotification(
        ZOOM_SDK_NAMESPACE::ShareSettingType type) override {}
    virtual void onSharedVideoEnded() override {}
    virtual void onVideoFileSharePlayError(
        ZOOM_SDK_NAMESPACE::ZoomSDKVideoFileSharePlayError error) override {}
    virtual void onOptimizingShareForVideoClipStatusChanged(
        ZOOM_SDK_NAMESPACE::ZoomSDKSharingSourceInfo shareInfo) override {}
};
static ZoomShareListener g_shareListener;

// ---------------------------------------------------------------------------
// LIVE STREAM LISTENER
// ---------------------------------------------------------------------------
class ZoomLiveStreamListener
    : public ZOOM_SDK_NAMESPACE::IMeetingLiveStreamCtrlEvent {
public:
    virtual void onRawLiveStreamPrivilegeChanged(bool bHasPrivilege) override {
        if (!bHasPrivilege) return;

        g_rawLiveStreamGranted = true;
        if (g_connectAction) g_connectAction->setEnabled(false);

        ZOOM_SDK_NAMESPACE::IMeetingService* ms = nullptr;
        ZOOM_SDK_NAMESPACE::CreateMeetingService(&ms);
        if (!ms) return;

        ZOOM_SDK_NAMESPACE::IMeetingLiveStreamController* lsc =
            ms->GetMeetingLiveStreamController();
        if (!lsc) return;

        lsc->StartRawLiveStreaming(
            L"https://letsdovideo.com/feeds-support/", L"Feeds");

        ZOOM_SDK_NAMESPACE::IMeetingShareController* sc =
            ms->GetMeetingShareController();
        if (sc) {
            sc->SetEvent(&g_shareListener);
            ZOOM_SDK_NAMESPACE::IList<unsigned int>* sharingUsers =
                sc->GetViewableSharingUserList();
            if (sharingUsers && sharingUsers->GetCount() > 0) {
                unsigned int sharingUserId = sharingUsers->GetItem(0);
                g_activeSharerUserId = sharingUserId;
                ZOOM_SDK_NAMESPACE::IList<
                    ZOOM_SDK_NAMESPACE::ZoomSDKSharingSourceInfo>* sourceList =
                    sc->GetSharingSourceInfoList(sharingUserId);
                if (sourceList && sourceList->GetCount() > 0)
                    g_activeShareSourceId =
                        sourceList->GetItem(0).shareSourceID;
            }
        }

        ZOOM_SDK_NAMESPACE::IMeetingAudioController* ac =
            ms->GetMeetingAudioController();
        if (ac) ac->SetEvent(&g_audioListener);

        Sleep(500);

        for (ZoomParticipantSource* src : g_allParticipantSources)
            src->initRenderer();

        if (!g_shareRenderer) {
            ZOOM_SDK_NAMESPACE::createRenderer(&g_shareRenderer, &g_shareCatcher);
            if (g_shareRenderer)
                g_shareRenderer->setRawDataResolution(GetResolutionForTier());
        }
        UpdateShareSubscription();

        for (ZoomParticipantSource* src : g_allParticipantSources) {
            if (src && src->source)
                obs_source_update_properties(src->source);
        }
    }

    virtual void onLiveStreamStatusChange(
        ZOOM_SDK_NAMESPACE::LiveStreamStatus status) override {}
    virtual void onRawLiveStreamPrivilegeRequestTimeout() override {}
    virtual void onUserRawLiveStreamPrivilegeChanged(
        unsigned int userid, bool bHasPrivilege) override {}
    virtual void onRawLiveStreamPrivilegeRequested(
        ZOOM_SDK_NAMESPACE::IRequestRawLiveStreamPrivilegeHandler* h) override {}
    virtual void onUserRawLiveStreamingStatusChanged(
        ZOOM_SDK_NAMESPACE::IList<
            ZOOM_SDK_NAMESPACE::RawLiveStreamInfo>* list) override {}
    virtual void onLiveStreamReminderStatusChanged(bool enable) override {}
    virtual void onLiveStreamReminderStatusChangeFailed() override {}
    virtual void onUserThresholdReachedForLiveStream(int percent) override {}
};
static ZoomLiveStreamListener g_liveStreamListener;

// ---------------------------------------------------------------------------
// MEETING LISTENER
// ---------------------------------------------------------------------------
class ZoomMeetingListener : public ZOOM_SDK_NAMESPACE::IMeetingServiceEvent {
public:
    virtual void onMeetingStatusChanged(
        ZOOM_SDK_NAMESPACE::MeetingStatus status, int iResult = 0) override {
        if (status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_INMEETING) {
            ZOOM_SDK_NAMESPACE::IMeetingService* ms = nullptr;
            ZOOM_SDK_NAMESPACE::CreateMeetingService(&ms);
            if (!ms) return;

            ZOOM_SDK_NAMESPACE::IMeetingLiveStreamController* lsc =
                ms->GetMeetingLiveStreamController();
            if (!lsc) return;
            lsc->SetEvent(&g_liveStreamListener);

            if (lsc->CanStartRawLiveStream() ==
                ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
                g_liveStreamListener.onRawLiveStreamPrivilegeChanged(true);
            } else {
                lsc->RequestRawLiveStreaming(
                    L"https://letsdovideo.com/feeds-support/", L"Feeds");
            }
        }

        if (status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_ENDED ||
            status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_DISCONNECTING) {
            g_rawLiveStreamGranted = false;
            g_activeSharerUserId   = 0;
            g_activeShareSourceId  = 0;
            g_activeSpeakerUserId  = 0;
            if (g_connectAction && g_isLoggedIn)
                g_connectAction->setEnabled(true);
            if (g_shareRenderer) {
                g_shareRenderer->unSubscribe();
                ZOOM_SDK_NAMESPACE::destroyRenderer(g_shareRenderer);
                g_shareRenderer = nullptr;
            }
        }
        if (status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_FAILED) {
            std::string msg;
            switch (iResult) {
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_PASSWORD_ERR:
                    msg = "Incorrect meeting password. Please try again.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_MEETING_NOT_EXIST:
                    msg = "Meeting not found. Please check the meeting number or link.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_CONNECTION_ERR:
                    msg = "Connection error. Please check your internet connection and try again.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN:
                    msg = "The host has disabled external participants from joining this meeting.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING:
                    msg = "This app must be published on the Zoom Marketplace before joining external meetings.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING:
                    msg = "This meeting requires you to be logged in to Zoom. Please log in and try again.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_BLOCKED_BY_ACCOUNT_ADMIN:
                    msg = "Your Zoom account administrator has blocked this application.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_NEED_SIGN_IN_FOR_PRIVATE_MEETING:
                    msg = "This is a private meeting. Please log in to Zoom and try again.";
                    break;
                default:
                    msg = "Failed to join meeting. Error code: " + std::to_string(iResult);
                    break;
            }
            // Re-enable connect button and show error
            if (g_connectAction && g_isLoggedIn)
                g_connectAction->setEnabled(true);
            std::string fullMsg = "Feeds - Could not join meeting\n\n" + msg;
            MessageBoxA(NULL, fullMsg.c_str(),
                        "Feeds - Join Failed", MB_OK | MB_ICONERROR);
        }
    }
    virtual void onMeetingStatisticsWarningNotification(
        ZOOM_SDK_NAMESPACE::StatisticsWarningType type) override {}
    virtual void onMeetingParameterNotification(
        const ZOOM_SDK_NAMESPACE::MeetingParameter* meeting_param) override {}
    virtual void onSuspendParticipantsActivities() override {}
    virtual void onAICompanionActiveChangeNotice(bool bActive) override {}
    virtual void onMeetingTopicChanged(const zchar_t* sTopic) override {}
    virtual void onMeetingFullToWatchLiveStream(
        const zchar_t* sLiveStreamUrl) override {}
    virtual void onUserNetworkStatusChanged(
        ZOOM_SDK_NAMESPACE::MeetingComponentType type,
        ZOOM_SDK_NAMESPACE::ConnectionQuality level,
        unsigned int userId, bool uplink) override {}
#if defined(WIN32)
    virtual void onAppSignalPanelUpdated(
        ZOOM_SDK_NAMESPACE::IMeetingAppSignalHandler* pHandler) override {}
#endif
};
static ZoomMeetingListener g_meetingListener;

// ---------------------------------------------------------------------------
// AUTH LISTENER
// ---------------------------------------------------------------------------
class ZoomAuthListener : public ZOOM_SDK_NAMESPACE::IAuthServiceEvent {
public:
    virtual void onAuthenticationReturn(
        ZOOM_SDK_NAMESPACE::AuthResult ret) override {
        if (ret != ZOOM_SDK_NAMESPACE::AUTHRET_SUCCESS) {
            MessageBoxA(NULL,
                "Zoom authentication failed. Please try logging in again.",
                "Feeds - Auth Failed", MB_OK | MB_ICONERROR);
            if (g_loginAction) g_loginAction->setEnabled(true);
            return;
        }

        g_isLoggedIn = true;
        if (g_loginAction)   g_loginAction->setEnabled(false);
        if (g_logoutAction)  g_logoutAction->setEnabled(true);
        if (g_connectAction) g_connectAction->setEnabled(true);

        // Refresh all open source properties panels
        for (ZoomParticipantSource* src : g_allParticipantSources) {
            if (src && src->source)
                obs_source_update_properties(src->source);
        }
        if (g_screenshareSource)
            obs_source_update_properties(g_screenshareSource);

        if (g_pendingMeetingJoin) {
            g_pendingMeetingJoin = false;
            QTimer::singleShot(200, []() { OnConnectClick(); });
        }
    }

    virtual void onLoginReturnWithReason(
        ZOOM_SDK_NAMESPACE::LOGINSTATUS ret,
        ZOOM_SDK_NAMESPACE::IAccountInfo* pAccountInfo,
        ZOOM_SDK_NAMESPACE::LoginFailReason reason) override {}
    virtual void onLogout() override {}
    virtual void onZoomIdentityExpired() override {}
    virtual void onZoomAuthIdentityExpired() override {}
#if defined(WIN32)
    virtual void onNotificationServiceStatus(
        ZOOM_SDK_NAMESPACE::SDKNotificationServiceStatus status,
        ZOOM_SDK_NAMESPACE::SDKNotificationServiceError error) override {}
#endif
};
static ZoomAuthListener g_authListener;

// ---------------------------------------------------------------------------
// AUTO-REFRESH: Poll until in meeting then refresh all participant sources
// ---------------------------------------------------------------------------
static void StartPostConnectRefreshTimer() {
    QTimer* timer = new QTimer((QObject*)obs_frontend_get_main_window());
    timer->setInterval(1000);
    QObject::connect(timer, &QTimer::timeout, [timer]() {
        ZOOM_SDK_NAMESPACE::IMeetingService* ms = nullptr;
        ZOOM_SDK_NAMESPACE::CreateMeetingService(&ms);
        if (ms && ms->GetMeetingStatus() ==
            ZOOM_SDK_NAMESPACE::MEETING_STATUS_INMEETING) {
            for (ZoomParticipantSource* src : g_allParticipantSources) {
                if (src && src->source)
                    obs_source_update_properties(src->source);
            }
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start();
}
// ---------------------------------------------------------------------------
// TOKEN PERSISTENCE via Windows Registry
// ---------------------------------------------------------------------------
static void SaveTokensToRegistry() {
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Feeds", 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                        &hKey, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExA(hKey, "access_token", 0, REG_SZ,
                   (const BYTE*)g_accessToken.c_str(),
                   (DWORD)g_accessToken.size() + 1);
    RegSetValueExA(hKey, "refresh_token", 0, REG_SZ,
                   (const BYTE*)g_refreshToken.c_str(),
                   (DWORD)g_refreshToken.size() + 1);
    RegCloseKey(hKey);
}

static bool LoadTokensFromRegistry() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Feeds", 0,
                      KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    char buf[4096];
    DWORD size = sizeof(buf);
    DWORD type = REG_SZ;
    if (RegQueryValueExA(hKey, "access_token", nullptr, &type,
                         (BYTE*)buf, &size) == ERROR_SUCCESS)
        g_accessToken = std::string(buf, size - 1);
    size = sizeof(buf);
    if (RegQueryValueExA(hKey, "refresh_token", nullptr, &type,
                         (BYTE*)buf, &size) == ERROR_SUCCESS)
        g_refreshToken = std::string(buf, size - 1);
    RegCloseKey(hKey);
    return !g_accessToken.empty();
}

static void ClearTokensFromRegistry() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Feeds", 0,
                      KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return;
    RegDeleteValueA(hKey, "access_token");
    RegDeleteValueA(hKey, "refresh_token");
    RegCloseKey(hKey);
}

// ---------------------------------------------------------------------------
// SDK AUTH — called on Qt main thread after token exchange succeeds
// ---------------------------------------------------------------------------
void DoSDKAuth() {
    ZOOM_SDK_NAMESPACE::IAuthService* auth_service = nullptr;
    if (ZOOM_SDK_NAMESPACE::CreateAuthService(&auth_service) !=
            ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS || !auth_service)
        return;
    auth_service->SetEvent(&g_authListener);

    ZOOM_SDK_NAMESPACE::AuthContext authContext;
    static std::wstring s_clientId = L"JlP6KfRqTt6r0t67FcDuqQ";
    authContext.publicAppKey = s_clientId.c_str();
    auth_service->SDKAuth(authContext);
}

// ---------------------------------------------------------------------------
// PKCE LOCALHOST LISTENER
// Runs on a background thread. Waits for Zoom to redirect to
// http://localhost:9847/callback?code=... then extracts the code,
// exchanges it for a token, and fires DoSDKAuth() on the Qt main thread.
// ---------------------------------------------------------------------------
static void RunPKCEListener(std::string verifier) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) { WSACleanup(); return; }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(9847);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(listenSock, 1) != 0) {
        closesocket(listenSock);
        WSACleanup();
        // Re-enable login button so user can retry
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            MessageBoxA(NULL,
                "Could not open local port 9847 for Zoom login.\n"
                "Another application may be using it. Please try again.",
                "Feeds - Login Error", MB_OK | MB_ICONERROR);
            if (g_loginAction) g_loginAction->setEnabled(true);
        });
        return;
    }

    // 5-minute timeout — if user abandons the browser, thread exits cleanly
    DWORD timeout = 300000;
    setsockopt(listenSock, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&timeout, sizeof(timeout));

    SOCKET clientSock = accept(listenSock, nullptr, nullptr);
    closesocket(listenSock);

    if (clientSock == INVALID_SOCKET) {
        WSACleanup();
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            if (g_loginAction) g_loginAction->setEnabled(true);
        });
        return;
    }

    // Read the HTTP request line
    char buf[4096] = {};
    recv(clientSock, buf, sizeof(buf) - 1, 0);

    // Send a friendly success page so the browser tab doesn't show an error
    const char* httpResponse =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<html><body style='font-family:sans-serif;text-align:center;"
        "margin-top:80px'>"
        "<h2>&#10003; Successfully connected to Zoom!</h2>"
        "<p>Your Zoom account is now linked to Feeds. "
        "You can close this tab and return to OBS.</p>"
        "</body></html>";
    send(clientSock, httpResponse, (int)strlen(httpResponse), 0);
    closesocket(clientSock);
    WSACleanup();

    // Parse the auth code out of the GET request line:
    // "GET /callback?code=XXXX HTTP/1.1"
    std::string request(buf);
    std::string code = ExtractQueryParam(request, "code");

    if (code.empty()) {
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            MessageBoxA(NULL,
                "Login was cancelled or the authorization code was missing.\n"
                "Please try again.",
                "Feeds - Login", MB_OK | MB_ICONWARNING);
            if (g_loginAction) g_loginAction->setEnabled(true);
        });
        return;
    }

    // Exchange code + verifier for access token (blocking HTTPS call)
    std::string tokenResponse = ExchangeCodeForToken(code, verifier);
    std::string accessToken   = JsonExtractString(tokenResponse, "access_token");
    std::string refreshToken  = JsonExtractString(tokenResponse, "refresh_token");

    if (accessToken.empty()) {
        // Log the raw response to help debug if something goes wrong
        std::string errMsg = "Token exchange failed.\n\nServer response:\n" +
                             tokenResponse.substr(0, 300);
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
                           [errMsg]() {
            MessageBoxA(NULL, errMsg.c_str(),
                        "Feeds - Login Error", MB_OK | MB_ICONERROR);
            if (g_loginAction) g_loginAction->setEnabled(true);
        });
        return;
    }

    g_accessToken  = accessToken;
    g_refreshToken = refreshToken;
    SaveTokensToRegistry();

    // Fire SDK auth on the Qt main thread
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
                       []() { DoSDKAuth(); });
}

// ---------------------------------------------------------------------------
// LOGIN HELPER — full PKCE flow
// ---------------------------------------------------------------------------
void OnLoginClick() {
    if (g_isLoggedIn) {
        MessageBoxA(NULL, "You are already logged in to Zoom.",
                    "Feeds - Login", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Disable login button while flow is in progress to prevent double-clicks
    if (g_loginAction) g_loginAction->setEnabled(false);

    // Generate PKCE values
    g_pkceVerifier        = GenerateCodeVerifier();
    std::string challenge = DeriveCodeChallenge(g_pkceVerifier);

    // Build the Zoom authorization URL
    // prompt=login forces Zoom to show the account chooser even if
    // the user is already logged in — important for multi-account users
    std::string authUrl =
        "https://zoom.us/oauth/authorize"
        "?response_type=code"
        "&client_id=JlP6KfRqTt6r0t67FcDuqQ"
        "&redirect_uri="          + UrlEncode("http://localhost:9847/callback") +
        "&code_challenge="        + challenge +
        "&code_challenge_method=S256"
        "&prompt=login";

    // Open the system browser to the Zoom login page
    ShellExecuteA(NULL, "open", authUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);

    // Start background thread to listen for the redirect
    std::string verifier = g_pkceVerifier;
    std::thread([verifier]() {
        RunPKCEListener(verifier);
    }).detach();
}

// ---------------------------------------------------------------------------
// LOGOUT HELPER
// ---------------------------------------------------------------------------
void OnLogoutClick() {
    if (!g_isLoggedIn) {
        MessageBoxA(NULL, "You are not currently logged in to Zoom.",
                    "Feeds - Logout", MB_OK | MB_ICONINFORMATION);
        return;
    }

    ZOOM_SDK_NAMESPACE::IAuthService* auth_service = nullptr;
    if (ZOOM_SDK_NAMESPACE::CreateAuthService(&auth_service) ==
            ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS && auth_service)
        auth_service->LogOut();

    g_isLoggedIn         = false;
    g_pendingMeetingJoin = false;
    g_accessToken.clear();
    g_refreshToken.clear();
    g_pkceVerifier.clear();
    ClearTokensFromRegistry();

    if (g_loginAction)   g_loginAction->setEnabled(true);
    if (g_logoutAction)  g_logoutAction->setEnabled(false);
    if (g_connectAction) g_connectAction->setEnabled(false);

    MessageBoxA(NULL, "You have been logged out of Zoom.",
                "Feeds - Logout", MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// INTERNAL HELPER: single authenticated GET to api.zoom.us
// ---------------------------------------------------------------------------
static std::string ZoomApiGet(const std::wstring& path) {
    HINTERNET hSession = WinHttpOpen(L"Feeds/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";
    HINTERNET hConnect = WinHttpConnect(hSession, L"api.zoom.us",
                                         INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
    std::wstring authHeader = L"Authorization: Bearer " +
        std::wstring(g_accessToken.begin(), g_accessToken.end());
    WinHttpAddRequestHeaders(hRequest, authHeader.c_str(),
                             (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                       nullptr, 0, 0, 0);
    WinHttpReceiveResponse(hRequest, nullptr);

    std::string response;
    char buf[4096];
    DWORD bytesRead = 0;
    while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead)
           && bytesRead > 0) {
        buf[bytesRead] = '\0';
        response += buf;
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

// ---------------------------------------------------------------------------
// FETCH USER INFO (ZAK + display name) via Zoom REST API
// ---------------------------------------------------------------------------
static bool FetchUserInfo(std::string& zak, std::string& displayName) {

    // Get display name from /v2/users/me  (requires user:read:user scope)
    std::string userResponse = ZoomApiGet(L"/v2/users/me");
    displayName = JsonExtractString(userResponse, "display_name");
    if (displayName.empty())
        displayName = JsonExtractString(userResponse, "first_name");
    g_userDisplayName = displayName; 

    // Get ZAK from /v2/users/me/zak  (requires user:read:zak scope)
    std::string zakResponse = ZoomApiGet(L"/v2/users/me/zak");
    zak = JsonExtractString(zakResponse, "token");

    if (zak.empty() || displayName.empty()) {
        // Show raw API responses to help diagnose scope or token problems
        std::string errMsg =
            "Could not retrieve your Zoom account details.\n\n"
            "/users/me response:\n" +
            userResponse.substr(0, 300) + "\n\n"
            "/users/me/zak response:\n" +
            zakResponse.substr(0, 300);
        MessageBoxA(NULL, errMsg.c_str(),
                    "Feeds - API Debug", MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// CONNECT TO MEETING HELPER
// ---------------------------------------------------------------------------
void OnConnectClick() {
    if (!g_isLoggedIn) {
        g_pendingMeetingJoin = true;
        MessageBoxA(NULL,
            "You need to log in to Zoom first.\n\n"
            "Please log in and then try Connect to Zoom Meeting again.",
            "Feeds - Login Required", MB_OK | MB_ICONINFORMATION);
        OnLoginClick();
        return;
    }

    ZOOM_SDK_NAMESPACE::IMeetingService* ms = nullptr;
    ZOOM_SDK_NAMESPACE::CreateMeetingService(&ms);
    if (!ms) return;
    ms->SetEvent(&g_meetingListener);

    QMainWindow* mainWindow = (QMainWindow*)obs_frontend_get_main_window();

    bool ok = false;
    QString input = QInputDialog::getText(
        mainWindow, "Join Zoom Meeting",
        "Enter your Zoom Meeting number or link:",
        QLineEdit::Normal, "", &ok);
    if (!ok || input.trimmed().isEmpty()) return;

    bool okPwd = false;
    QString password = QInputDialog::getText(
        mainWindow, "Meeting Password",
        "Enter meeting password (leave blank if none):",
        QLineEdit::Normal, "", &okPwd);
    if (!okPwd) return;

    input = input.trimmed();

    // Fetch ZAK and display name using our OAuth access token.
    // ZAK identifies the user to Zoom: grants host privileges,
    // bypasses waiting room for own meetings, and satisfies the
    // March 2026 external-meeting authorization requirement.
    std::string zak, displayName;
    if (!FetchUserInfo(zak, displayName)) {
        // FetchUserInfo already showed a diagnostic message box
        return;
    }

    ZOOM_SDK_NAMESPACE::JoinParam joinParam;
    joinParam.userType = ZOOM_SDK_NAMESPACE::SDK_UT_WITHOUT_LOGIN;
    ZOOM_SDK_NAMESPACE::JoinParam4WithoutLogin& param =
        joinParam.param.withoutloginuserJoin;
    param.isAudioOff = true;
    param.isVideoOff = true;

    // Join as the authenticated user, not as anonymous "Feeds"
    static std::wstring s_userName;
    s_userName     = std::wstring(displayName.begin(), displayName.end());
    param.userName = s_userName.c_str();

    // Pass ZAK so Zoom recognises the user
    static std::wstring s_zak;
    s_zak        = std::wstring(zak.begin(), zak.end());
    param.userZAK = s_zak.c_str();

    static std::wstring s_password;
    if (!password.trimmed().isEmpty()) {
        s_password = password.trimmed().toStdWString();
        param.psw  = s_password.c_str();
    }

    static std::wstring s_vanityId;

    if (input.contains("zoom.us/my/")) {
        int start    = input.indexOf("zoom.us/my/") + 11;
        QString vanityId = input.mid(start).split(
            QRegularExpression("[?\\s]")).first();
        s_vanityId     = vanityId.toStdWString();
        param.vanityID = s_vanityId.c_str();
    } else if (input.contains("zoom.us/j/")) {
        int start  = input.indexOf("zoom.us/j/") + 10;
        QString numStr = input.mid(start).split(
            QRegularExpression("[?\\s]")).first();
        numStr = numStr.replace(QRegularExpression("[^0-9]"), "");
        if (numStr.isEmpty()) return;
        param.meetingNumber = numStr.toULongLong();
    } else {
        QString numStr = input.replace(QRegularExpression("[\\s\\-]"), "");
        numStr = numStr.replace(QRegularExpression("[^0-9]"), "");
        if (numStr.isEmpty()) return;
        param.meetingNumber = numStr.toULongLong();
    }

    ms->Join(joinParam);
    StartPostConnectRefreshTimer();
}

// ---------------------------------------------------------------------------
// MENU SETUP
// ---------------------------------------------------------------------------
void SetupPluginMenu(void) {
    QMainWindow* mainWindow = (QMainWindow*)obs_frontend_get_main_window();
    QMenuBar*    menuBar    = mainWindow->menuBar();
    QMenu*       feedsMenu  = new QMenu("Feeds", menuBar);
    menuBar->addMenu(feedsMenu);

    g_loginAction   = feedsMenu->addAction("Login to Zoom");
    g_logoutAction  = feedsMenu->addAction("Logout of Zoom");
    feedsMenu->addSeparator();
    g_connectAction = feedsMenu->addAction("Connect to Zoom Meeting...");
    feedsMenu->addSeparator();
    QAction* aboutAction = feedsMenu->addAction("About / Tier Status");

    // Initial state: not logged in
    g_logoutAction->setEnabled(false);
    g_connectAction->setEnabled(false);

    QObject::connect(g_loginAction,   &QAction::triggered,
                     []() { OnLoginClick(); });
    QObject::connect(g_logoutAction,  &QAction::triggered,
                     []() { OnLogoutClick(); });
    QObject::connect(g_connectAction, &QAction::triggered,
                     []() { OnConnectClick(); });
    QObject::connect(aboutAction, &QAction::triggered, []() {
        std::string info = "Feeds v0.1\n";
        if (g_isLoggedIn && !g_userDisplayName.empty())
            info += "Logged in as: " + g_userDisplayName + "\n";
        else
            info += "Not logged in\n";
        info += "Tier: Basic\nStatus: Active";
        MessageBoxA(NULL, info.c_str(), "About Feeds", MB_OK);
    });
}

// ---------------------------------------------------------------------------
// SCREENSHARE SOURCE
// ---------------------------------------------------------------------------
class ZoomScreenshareSource {
public:
    obs_source_t* source = nullptr;
    ZoomScreenshareSource(obs_source_t* src) : source(src) {
        g_screenshareSource = src;
        if (g_rawLiveStreamGranted && g_shareRenderer)
            UpdateShareSubscription();
    }
    ~ZoomScreenshareSource() {
        if (g_screenshareSource == source)
            g_screenshareSource = nullptr;
    }
};

// ---------------------------------------------------------------------------
// PROPERTIES - PARTICIPANT SOURCE
// ---------------------------------------------------------------------------
static obs_properties_t* zp_properties(void* data) {
    obs_properties_t* props = obs_properties_create();
    obs_properties_add_text(props, "ver_label", "Feeds (v0.1 Alpha)",
                            OBS_TEXT_INFO);

    ZOOM_SDK_NAMESPACE::IMeetingService* ms = nullptr;
    ZOOM_SDK_NAMESPACE::CreateMeetingService(&ms);
    bool inMeeting = ms && ms->GetMeetingStatus() ==
                     ZOOM_SDK_NAMESPACE::MEETING_STATUS_INMEETING;

    if (!inMeeting) {
        if (!g_isLoggedIn) {
            obs_properties_add_button(props, "login_btn",
                "Not logged in to Zoom. Click to Login...",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnLoginClick();
                    return true;
                });
        } else {
            obs_properties_add_button(props, "connect_btn",
                "Logged in. Click to Connect to Zoom Meeting...",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnConnectClick();
                    return true;
                });
        }
    } else {
        std::string status_text = "Status: Connected";
        ZOOM_SDK_NAMESPACE::IMeetingInfo* info = ms->GetMeetingInfo();
        if (info)
            status_text = "Status: Connected to Meeting " +
                          std::to_string(info->GetMeetingNumber());
        obs_properties_add_text(props, "status_label", status_text.c_str(),
                                OBS_TEXT_INFO);

        ZoomParticipantSource* src = static_cast<ZoomParticipantSource*>(data);
        obs_properties_add_button2(props, "refresh_btn",
            "Refresh Participant List",
            [](obs_properties_t*, obs_property_t*, void* data) -> bool {
                ZoomParticipantSource* src =
                    static_cast<ZoomParticipantSource*>(data);
                if (src) {
                    obs_data_t* settings =
                        obs_source_get_settings(src->source);
                    if (settings) {
                        src->update(settings);
                        obs_data_release(settings);
                    }
                }
                return true;
            }, src);
    }

    ZoomParticipantSource* src = static_cast<ZoomParticipantSource*>(data);
    if (src && g_rawLiveStreamGranted && !src->videoRenderer)
        src->initRenderer();

    obs_property_t* list = obs_properties_add_list(
        props, "participant_id", "Select Participant",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(list, "--- Select Participant ---", 0);
    obs_property_list_add_int(list, "[Active Speaker]", 1);

    if (ms) {
        ZOOM_SDK_NAMESPACE::IMeetingParticipantsController* pc =
            ms->GetMeetingParticipantsController();
        if (pc) {
            ZOOM_SDK_NAMESPACE::IList<unsigned int>* userList =
                pc->GetParticipantsList();
            if (userList) {
                for (int i = 0; i < userList->GetCount(); i++) {
                    unsigned int uid = userList->GetItem(i);
                    ZOOM_SDK_NAMESPACE::IUserInfo* info =
                        pc->GetUserByUserID(uid);
                    if (info) {
                        std::wstring wname = info->GetUserName();
                        if (wname == L"Feeds") continue;
                        int size_needed = WideCharToMultiByte(
                            CP_UTF8, 0, &wname[0], (int)wname.size(),
                            NULL, 0, NULL, NULL);
                        std::string name(size_needed, 0);
                        WideCharToMultiByte(CP_UTF8, 0, &wname[0],
                                            (int)wname.size(), &name[0],
                                            size_needed, NULL, NULL);
                        obs_property_list_add_int(list, name.c_str(),
                                                  (long long)uid);
                    }
                }
            }
        }
    }
    return props;
}

// ---------------------------------------------------------------------------
// PROPERTIES - SCREENSHARE SOURCE
// ---------------------------------------------------------------------------
static obs_properties_t* zs_properties(void* data) {
    (void)data;
    obs_properties_t* props = obs_properties_create();
    obs_properties_add_text(props, "ver_label",
                            "Feeds - Screenshare (v0.1 Alpha)", OBS_TEXT_INFO);

    ZOOM_SDK_NAMESPACE::IMeetingService* ms = nullptr;
    ZOOM_SDK_NAMESPACE::CreateMeetingService(&ms);
    bool inMeeting = ms && ms->GetMeetingStatus() ==
                     ZOOM_SDK_NAMESPACE::MEETING_STATUS_INMEETING;

    if (!inMeeting) {
        if (!g_isLoggedIn) {
            obs_properties_add_button(props, "login_btn",
                "Not logged in to Zoom. Click to Login...",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnLoginClick();
                    return true;
                });
        } else {
            obs_properties_add_button(props, "connect_btn",
                "Logged in. Click to Connect to Zoom Meeting...",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnConnectClick();
                    return true;
                });
        }
    } else {
        std::string status_text = (g_activeSharerUserId != 0)
            ? "Status: Receiving screenshare"
            : "Status: Connected - waiting for screenshare";
        obs_properties_add_text(props, "status_label", status_text.c_str(),
                                OBS_TEXT_INFO);
    }

    return props;
}

// ---------------------------------------------------------------------------
// SOURCE CALLBACKS
// ---------------------------------------------------------------------------
void* zp_create(obs_data_t* settings, obs_source_t* source) {
    if (g_activeParticipantSources >= GetMaxFeedsForTier()) {
        std::string msg = "Your current tier allows a maximum of " +
                          std::to_string(GetMaxFeedsForTier()) +
                          " participant feed(s).\n\nUpgrade your plan at:\n"
                          "https://marketplace.zoom.us";
        MessageBoxA(NULL, msg.c_str(), "Feeds - Upgrade Required",
                    MB_OK | MB_ICONINFORMATION);
        return nullptr;
    }
    return new ZoomParticipantSource(source);
}

void zp_destroy(void* data) {
    if (data) delete static_cast<ZoomParticipantSource*>(data);
}

void zp_update(void* data, obs_data_t* settings) {
    if (data) static_cast<ZoomParticipantSource*>(data)->update(settings);
}

void* zs_create(obs_data_t* settings, obs_source_t* source) {
    return new ZoomScreenshareSource(source);
}

void zs_destroy(void* data) {
    if (data) delete static_cast<ZoomScreenshareSource*>(data);
}

// ---------------------------------------------------------------------------
// SOURCE INFO STRUCTS
// ---------------------------------------------------------------------------
struct obs_source_info zoom_participant_info = {};
struct obs_source_info zoom_screenshare_info = {};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("feeds", "en-US")

bool obs_module_load(void) {
    zoom_participant_info.id             = "zoom_participant_source";
    zoom_participant_info.type           = OBS_SOURCE_TYPE_INPUT;
    zoom_participant_info.output_flags   = OBS_SOURCE_ASYNC_VIDEO;
    zoom_participant_info.get_name       = [](void*) { return "Zoom Participant"; };
    zoom_participant_info.create         = zp_create;
    zoom_participant_info.destroy        = zp_destroy;
    zoom_participant_info.get_properties = zp_properties;
    zoom_participant_info.update         = zp_update;
    zoom_participant_info.icon_type      = OBS_ICON_TYPE_CUSTOM;
    obs_register_source(&zoom_participant_info);

    zoom_screenshare_info.id             = "zoom_screenshare_source";
    zoom_screenshare_info.type           = OBS_SOURCE_TYPE_INPUT;
    zoom_screenshare_info.output_flags   = OBS_SOURCE_ASYNC_VIDEO;
    zoom_screenshare_info.get_name       = [](void*) { return "Zoom Screenshare"; };
    zoom_screenshare_info.create         = zs_create;
    zoom_screenshare_info.destroy        = zs_destroy;
    zoom_screenshare_info.get_properties = zs_properties;
    zoom_screenshare_info.icon_type      = OBS_ICON_TYPE_CUSTOM;
    obs_register_source(&zoom_screenshare_info);

    ZOOM_SDK_NAMESPACE::InitParam initParam;
    initParam.strWebDomain = L"https://zoom.us";
    ZOOM_SDK_NAMESPACE::InitSDK(initParam);

    obs_frontend_add_event_callback([](enum obs_frontend_event event, void*) {
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
            SetupPluginMenu();
            // Auto-login if we have stored tokens
            if (LoadTokensFromRegistry()) {
                DoSDKAuth();
            }
        }
    }, nullptr);

    return true;
}

void obs_module_unload(void) {
    if (g_shareRenderer) {
        g_shareRenderer->unSubscribe();
        ZOOM_SDK_NAMESPACE::destroyRenderer(g_shareRenderer);
        g_shareRenderer = nullptr;
    }
}
