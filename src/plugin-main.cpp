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
#include <wincred.h>
#include <winhttp.h>
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "Advapi32.lib")

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
#include <delayimp.h>
#include <shlwapi.h>

// ---------------------------------------------------------------------------
// TIER GATING
// 0 = Free (1 feed), 1 = Basic (3 feeds), 2 = Streamer (5 feeds), 3 = Broadcaster (8 feeds)
// ---------------------------------------------------------------------------
static int g_currentTier = 0;
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
static bool     g_sdkInitialized       = false; // Set true after InitSDK succeeds
static bool     g_rawLiveStreamGranted = false;
static QAction* g_connectAction        = nullptr;
static QAction* g_loginAction          = nullptr;
static QAction* g_logoutAction         = nullptr;
static bool     g_isLoggedIn           = false;
static bool     g_pendingMeetingJoin   = false;

static unsigned int g_activeSharerUserId  = 0;
static unsigned int g_activeShareSourceId = 0;
static unsigned int g_activeSpeakerUserId = 0;

// PKCE / user state
static std::string g_pkceVerifier;
static std::string g_accessToken;
static std::string g_refreshToken;
static std::string g_userDisplayName;
static std::string g_userPMI;

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
        "&redirect_uri="  + UrlEncode("https://letsdovideo.com/loginsuccess") +
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

// Extract a numeric field from JSON (returned without quotes)
static std::string JsonExtractNumber(const std::string& json,
                                     const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find_first_of("0123456789", pos + search.size());
    if (pos == std::string::npos) return "";
    size_t end = json.find_first_not_of("0123456789", pos);
    return json.substr(pos, end == std::string::npos ? std::string::npos
                                                      : end - pos);
}

// Safe UTF-8 to wstring conversion (handles non-ASCII display names)
static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0,
                                     utf8.c_str(), (int)utf8.size(),
                                     nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring result(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
                        utf8.c_str(), (int)utf8.size(),
                        &result[0], needed);
    return result;
}

// ---------------------------------------------------------------------------
// FORWARD DECLARATIONS
// ---------------------------------------------------------------------------
void OnLoginClick();
void OnLogoutClick();
void OnConnectClick();
void DoSDKAuth();
static std::string ZoomApiGet(const std::wstring& path);
static void FetchAndApplyEntitlement();

// ---------------------------------------------------------------------------
// PER-SOURCE VIDEO CATCHER
// ---------------------------------------------------------------------------
class ZoomVideoCatcher : public ZOOM_SDK_NAMESPACE::IZoomSDKRendererDelegate {
public:
    obs_source_t* target = nullptr;

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

        // Use wall clock directly for each frame. Accumulating a fixed
        // 33ms increment causes forward drift vs. actual delivery rate,
        // which OBS buffers away as latency (~1s after ~30s of streaming).
        obs_frame.timestamp = os_gettime_ns();
        obs_source_output_video(target, &obs_frame);
    }
    virtual void onRawDataStatusChanged(RawDataStatus status) override {}
    virtual void onRendererBeDestroyed() override { target = nullptr; }
};

// ---------------------------------------------------------------------------
// SCREENSHARE GLOBALS
// ---------------------------------------------------------------------------
static obs_source_t* g_screenshareSource = nullptr;

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

        // Use wall clock directly — same drift fix as ZoomVideoCatcher.
        obs_frame.timestamp = os_gettime_ns();
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
    obs_source_t* source          = nullptr;
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
            // Guard against SDK being partially shut down during OBS exit.
            // The SDK may have already freed internal state by the time
            // zp_destroy is called, causing an access violation if we call
            // through to it unguarded.
            try {
                videoRenderer->unSubscribe();
                ZOOM_SDK_NAMESPACE::destroyRenderer(videoRenderer);
            } catch (...) {}
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
            if (g_connectAction && g_isLoggedIn)
                g_connectAction->setEnabled(true);
            MessageBoxA(NULL, msg.c_str(), "Feeds - Join Failed",
                        MB_OK | MB_ICONERROR);
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

        // Pre-fetch user info (display name + PMI) so it is ready before
        // the user clicks Connect for the first time. ZAK is fetched fresh
        // at connect time since ZAK tokens are short-lived.
        std::thread([]() {
            std::string userResponse = ZoomApiGet(L"/v2/users/me");
            std::string displayName = JsonExtractString(userResponse, "display_name");
            if (displayName.empty())
                displayName = JsonExtractString(userResponse, "first_name");
            g_userDisplayName = displayName;
            g_userPMI         = JsonExtractNumber(userResponse, "pmi");

            // Also apply entitlement tier at login time
            FetchAndApplyEntitlement();
        }).detach();

        // Refresh all open source properties panels
        for (ZoomParticipantSource* src : g_allParticipantSources) {
            if (src && src->source)
                obs_source_update_properties(src->source);
        }
        if (g_screenshareSource)
            obs_source_update_properties(g_screenshareSource);

        if (g_pendingMeetingJoin) {
            g_pendingMeetingJoin = false;
            QTimer::singleShot(500, []() { OnConnectClick(); });
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
// TOKEN PERSISTENCE via Windows Credential Manager (DPAPI-protected)
// Tokens are stored as generic credentials under the target name "Feeds".
// Only the same Windows user account can read them back — DPAPI ensures
// the data is encrypted at rest and tied to the user's login credentials.
// ---------------------------------------------------------------------------
static void SaveTokensToRegistry() {
    // Store access token
    {
        std::string targetAccess = "Feeds_AccessToken";
        CREDENTIALA cred = {};
        cred.Type                 = CRED_TYPE_GENERIC;
        cred.TargetName           = (LPSTR)targetAccess.c_str();
        cred.CredentialBlobSize   = (DWORD)g_accessToken.size();
        cred.CredentialBlob       = (LPBYTE)g_accessToken.data();
        cred.Persist              = CRED_PERSIST_LOCAL_MACHINE;
        CredWriteA(&cred, 0);
    }
    // Store refresh token
    {
        std::string targetRefresh = "Feeds_RefreshToken";
        CREDENTIALA cred = {};
        cred.Type                 = CRED_TYPE_GENERIC;
        cred.TargetName           = (LPSTR)targetRefresh.c_str();
        cred.CredentialBlobSize   = (DWORD)g_refreshToken.size();
        cred.CredentialBlob       = (LPBYTE)g_refreshToken.data();
        cred.Persist              = CRED_PERSIST_LOCAL_MACHINE;
        CredWriteA(&cred, 0);
    }
}

static bool LoadTokensFromRegistry() {
    bool loaded = false;

    // Load access token
    {
        PCREDENTIALA pCred = nullptr;
        if (CredReadA("Feeds_AccessToken", CRED_TYPE_GENERIC, 0, &pCred)) {
            g_accessToken = std::string(
                (char*)pCred->CredentialBlob,
                pCred->CredentialBlobSize);
            CredFree(pCred);
            loaded = true;
        }
    }
    // Load refresh token
    {
        PCREDENTIALA pCred = nullptr;
        if (CredReadA("Feeds_RefreshToken", CRED_TYPE_GENERIC, 0, &pCred)) {
            g_refreshToken = std::string(
                (char*)pCred->CredentialBlob,
                pCred->CredentialBlobSize);
            CredFree(pCred);
        }
    }

    return loaded && !g_accessToken.empty();
}

static void ClearTokensFromRegistry() {
    CredDeleteA("Feeds_AccessToken",  CRED_TYPE_GENERIC, 0);
    CredDeleteA("Feeds_RefreshToken", CRED_TYPE_GENERIC, 0);
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
// NAMED PIPE LISTENER
// Runs on a background thread. Waits for the feeds:// protocol handler
// to deliver the auth code via a named pipe, then completes token exchange.
// ---------------------------------------------------------------------------
static void RunNamedPipeListener(std::string verifier) {
SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE pipe = CreateNamedPipeA(
        "\\\\.\\pipe\\FeedsAuth",
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        256,
        256,
        300000,
        &sa
    );
if (pipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::string errMsg = "Could not create auth pipe for Zoom login.\nError code: " + 
                             std::to_string(err) + "\nPlease try again.";
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), [errMsg]() {
            MessageBoxA(NULL, errMsg.c_str(),
                "Feeds - Login Error", MB_OK | MB_ICONERROR);
            if (g_loginAction) g_loginAction->setEnabled(true);
        });
        return;
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            MessageBoxA(NULL, "Could not create auth pipe for Zoom login.\nPlease try again.",
                "Feeds - Login Error", MB_OK | MB_ICONERROR);
            if (g_loginAction) g_loginAction->setEnabled(true);
        });
        return;
    }

    // Wait for protocol handler to connect (5 min timeout via pipe settings)
    BOOL connected = ConnectNamedPipe(pipe, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(pipe);
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            if (g_loginAction) g_loginAction->setEnabled(true);
        });
        return;
    }

    char buf[512] = {};
    DWORD bytesRead = 0;
    ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr);
    CloseHandle(pipe);

    std::string code(buf, bytesRead);
   
    if (code.empty()) {
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            MessageBoxA(NULL, "Login was cancelled or the authorization code was missing.\nPlease try again.",
                "Feeds - Login", MB_OK | MB_ICONWARNING);
            if (g_loginAction) g_loginAction->setEnabled(true);
        });
        return;
    }

    std::string tokenResponse = ExchangeCodeForToken(code, verifier);
    std::string accessToken   = JsonExtractString(tokenResponse, "access_token");
    std::string refreshToken  = JsonExtractString(tokenResponse, "refresh_token");

    if (accessToken.empty()) {
        std::string errMsg = "Token exchange failed.\n\nServer response:\n" +
                             tokenResponse.substr(0, 300);
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), [errMsg]() {
            MessageBoxA(NULL, errMsg.c_str(), "Feeds - Login Error", MB_OK | MB_ICONERROR);
            if (g_loginAction) g_loginAction->setEnabled(true);
        });
        return;
    }

    g_accessToken  = accessToken;
    g_refreshToken = refreshToken;
    SaveTokensToRegistry();

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

    if (g_loginAction) g_loginAction->setEnabled(false);

    g_pkceVerifier        = GenerateCodeVerifier();
    std::string challenge = DeriveCodeChallenge(g_pkceVerifier);

    std::string authUrl =
        std::string("https://zoom.us/oauth/authorize") +
        "?response_type=code" +
        "&client_id=JlP6KfRqTt6r0t67FcDuqQ" +
        "&redirect_uri="          + UrlEncode("https://letsdovideo.com/loginsuccess") +
        "&code_challenge="        + challenge +
        "&code_challenge_method=S256" +
        "&prompt=consent";

    ShellExecuteA(NULL, "open", authUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);

    std::string verifier = g_pkceVerifier;
    std::thread([verifier]() {
    RunNamedPipeListener(verifier);
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
    g_userDisplayName.clear();
    g_userPMI.clear();
    ClearTokensFromRegistry();

    if (g_loginAction)   g_loginAction->setEnabled(true);
    if (g_logoutAction)  g_logoutAction->setEnabled(false);
    if (g_connectAction) g_connectAction->setEnabled(false);

    MessageBoxA(NULL, "You have been logged out of Zoom.",
                "Feeds - Logout", MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// REFRESH ACCESS TOKEN using stored refresh token
// ---------------------------------------------------------------------------
static bool RefreshAccessToken() {
    if (g_refreshToken.empty()) return false;

    std::string body =
        std::string("grant_type=refresh_token") +
        "&refresh_token=" + UrlEncode(g_refreshToken) +
        "&client_id=JlP6KfRqTt6r0t67FcDuqQ";

    HINTERNET hSession = WinHttpOpen(L"Feeds/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    HINTERNET hConnect = WinHttpConnect(hSession, L"zoom.us",
                                         INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/oauth/token",
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
    WinHttpAddRequestHeaders(hRequest,
        L"Content-Type: application/x-www-form-urlencoded",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                       (LPVOID)body.c_str(), (DWORD)body.size(),
                       (DWORD)body.size(), 0);
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

    std::string newAccess  = JsonExtractString(response, "access_token");
    std::string newRefresh = JsonExtractString(response, "refresh_token");

    if (newAccess.empty()) return false;

    g_accessToken = newAccess;
    if (!newRefresh.empty())
        g_refreshToken = newRefresh;
    SaveTokensToRegistry();
    return true;
}

// ---------------------------------------------------------------------------
// INTERNAL HELPER: single authenticated GET to api.zoom.us
// ---------------------------------------------------------------------------
static std::string ZoomApiGet(const std::wstring& path) {
    auto doRequest = [&]() -> std::string {
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

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);

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

        return std::to_string(statusCode) + "|" + response;
    };

    std::string result = doRequest();
    std::string status = result.substr(0, result.find('|'));
    std::string body   = result.substr(result.find('|') + 1);

    if (status == "401") {
        if (RefreshAccessToken()) {
            result = doRequest();
            body   = result.substr(result.find('|') + 1);
        } else {
            QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
                g_isLoggedIn = false;
                ClearTokensFromRegistry();
                if (g_loginAction)   g_loginAction->setEnabled(true);
                if (g_logoutAction)  g_logoutAction->setEnabled(false);
                if (g_connectAction) g_connectAction->setEnabled(false);
                MessageBoxA(NULL,
                    "Your Zoom login has expired and could not be renewed.\n\n"
                    "Please log in again.",
                    "Feeds - Session Expired", MB_OK | MB_ICONWARNING);
            });
            return "";
        }
    }

    return body;
}

// ---------------------------------------------------------------------------
// FETCH AND APPLY MARKETPLACE ENTITLEMENT
// Sets g_currentTier based on what the user has purchased.
// Defaults to Free (0) if the app is not yet on the Marketplace or the
// user has no paid entitlement.
// ---------------------------------------------------------------------------
static void FetchAndApplyEntitlement() {
    std::string response = ZoomApiGet(
        L"/v2/marketplace/users/me/entitlements"
        L"?app_id=JlP6KfRqTt6r0t67FcDuqQ");

    // Default to Free tier
    g_currentTier = 0;

    if (response.find("Broadcaster") != std::string::npos)
        g_currentTier = 3;
    else if (response.find("Streamer") != std::string::npos)
        g_currentTier = 2;
    else if (response.find("Basic") != std::string::npos)
        g_currentTier = 1;
}

// ---------------------------------------------------------------------------
// FETCH USER INFO (ZAK + display name + PMI) via Zoom REST API
// Always fetches a fresh ZAK (short-lived token).
// Skips the /v2/users/me call if display name and PMI are already cached
// from the post-login pre-fetch, saving an API round-trip.
// ---------------------------------------------------------------------------
static bool FetchUserInfo(std::string& zak, std::string& displayName) {
    // Only call /v2/users/me if we don't already have the cached values
    if (g_userDisplayName.empty() || g_userPMI.empty()) {
        std::string userResponse = ZoomApiGet(L"/v2/users/me");
        std::string fetchedName = JsonExtractString(userResponse, "display_name");
        if (fetchedName.empty())
            fetchedName = JsonExtractString(userResponse, "first_name");
        g_userDisplayName = fetchedName;
        g_userPMI         = JsonExtractNumber(userResponse, "pmi");
    }
    displayName = g_userDisplayName;

    // Always fetch a fresh ZAK — these are short-lived
    std::string zakResponse = ZoomApiGet(L"/v2/users/me/zak");
    zak = JsonExtractString(zakResponse, "token");

    if (zak.empty() || displayName.empty()) {
        MessageBoxA(NULL,
            "Could not retrieve your Zoom account details.\n\n"
            "Please log out and log in again.",
            "Feeds - Error", MB_OK | MB_ICONERROR);
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

    // Show join options dialog
    QStringList options;
    QString pmiOption = "My Personal Meeting Room (PMI)";
    if (!g_userPMI.empty())
        pmiOption += " - " + QString::fromStdString(g_userPMI);
    options << pmiOption
            << "Join by Meeting Number or Link";

    bool ok = false;
    QString choice = QInputDialog::getItem(
        mainWindow, "Join Zoom Meeting",
        "How would you like to join?",
        options, 0, false, &ok);
    if (!ok) return;

    QString input;
    QString password;

    if (choice.startsWith("My Personal")) {
        if (g_userPMI.empty()) {
            MessageBoxA(NULL,
                "Could not retrieve your Personal Meeting Room ID.\n"
                "Please use Join by Meeting Number instead.",
                "Feeds", MB_OK | MB_ICONWARNING);
            return;
        }
        input = QString::fromStdString(g_userPMI);
        bool okPwd = false;
        password = QInputDialog::getText(
            mainWindow, "Meeting Password",
            "Enter your PMI password (leave blank if none):",
            QLineEdit::Normal, "", &okPwd);
        if (!okPwd) return;
    } else {
        bool okInput = false;
        input = QInputDialog::getText(
            mainWindow, "Join Zoom Meeting",
            "Enter your Zoom Meeting number or link:",
            QLineEdit::Normal, "", &okInput);
        if (!okInput || input.trimmed().isEmpty()) return;
        input = input.trimmed();

        bool okPwd = false;
        password = QInputDialog::getText(
            mainWindow, "Meeting Password",
            "Enter meeting password (leave blank if none):",
            QLineEdit::Normal, "", &okPwd);
        if (!okPwd) return;
    }

    // Fetch a fresh ZAK token (and display name / PMI if not yet cached)
    std::string zak, displayName;
    if (!FetchUserInfo(zak, displayName))
        return;

    ZOOM_SDK_NAMESPACE::JoinParam joinParam;
    joinParam.userType = ZOOM_SDK_NAMESPACE::SDK_UT_WITHOUT_LOGIN;
    ZOOM_SDK_NAMESPACE::JoinParam4WithoutLogin& param =
        joinParam.param.withoutloginuserJoin;
    param.isAudioOff = true;
    param.isVideoOff = true;

    static std::wstring s_userName;
    s_userName     = Utf8ToWide(displayName);
    param.userName = s_userName.c_str();

    static std::wstring s_zak;
    s_zak         = std::wstring(zak.begin(), zak.end());
    param.userZAK = s_zak.c_str();

    static std::wstring s_password;
    if (!password.trimmed().isEmpty()) {
        s_password = password.trimmed().toStdWString();
        param.psw  = s_password.c_str();
    }

    static std::wstring s_vanityId;

    if (input.contains("zoom.us/my/")) {
        int start = input.indexOf("zoom.us/my/") + 11;
        QString vanityId = input.mid(start).split(
            QRegularExpression("[?\\s]")).first();
        s_vanityId     = vanityId.toStdWString();
        param.vanityID = s_vanityId.c_str();
    } else if (input.contains("zoom.us/j/")) {
        int start = input.indexOf("zoom.us/j/") + 10;
        QString numStr = input.mid(start).split(
            QRegularExpression("[?\\s]")).first();
        numStr = numStr.replace(QRegularExpression("[^0-9]"), "");
        if (numStr.isEmpty()) return;
        param.meetingNumber = numStr.toULongLong();
    } else {
        // Treat as raw meeting number or PMI number
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

    g_logoutAction->setEnabled(false);
    g_connectAction->setEnabled(false);

    QObject::connect(g_loginAction,   &QAction::triggered,
                     []() { OnLoginClick(); });
    QObject::connect(g_logoutAction,  &QAction::triggered,
                     []() { OnLogoutClick(); });
    QObject::connect(g_connectAction, &QAction::triggered,
                     []() { OnConnectClick(); });
    QObject::connect(aboutAction, &QAction::triggered, []() {
        std::string tierName;
        switch (g_currentTier) {
            case 1:  tierName = "Basic";       break;
            case 2:  tierName = "Streamer";    break;
            case 3:  tierName = "Broadcaster"; break;
            default: tierName = "Free";        break;
        }
        std::string aboutText = "Feeds v1.0\n";
        if (!g_userDisplayName.empty())
            aboutText += "Logged in as: " + g_userDisplayName + "\n";
        aboutText += "Tier: " + tierName;
        MessageBoxA(NULL, aboutText.c_str(), "About Feeds", MB_OK);
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
    obs_properties_add_text(props, "ver_label", "Feeds (v1.0)",
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
                    obs_source_update_properties(src->source);
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
            // Get our own user ID to exclude from the participant list
            unsigned int myUserId = 0;
            ZOOM_SDK_NAMESPACE::IUserInfo* mySelf = pc->GetMySelfUser();
            if (mySelf) myUserId = mySelf->GetUserID();

            ZOOM_SDK_NAMESPACE::IList<unsigned int>* userList =
                pc->GetParticipantsList();
            if (userList) {
                for (int i = 0; i < userList->GetCount(); i++) {
                    unsigned int uid = userList->GetItem(i);
                    // Skip ourselves — filter by ID, not name
                    if (myUserId != 0 && uid == myUserId) continue;
                    ZOOM_SDK_NAMESPACE::IUserInfo* info =
                        pc->GetUserByUserID(uid);
                    if (info) {
                        std::wstring wname = info->GetUserName();
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
                            "Feeds - Screenshare (v1.0)", OBS_TEXT_INFO);

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
    // Guard against source creation before the Zoom SDK has finished
    // initializing. On slower machines the SDK can take 2+ seconds to init,
    // and if the user adds a source during that window the SDK call inside
    // ZoomParticipantSource's constructor can crash.
    if (!g_sdkInitialized) {
        MessageBoxA(NULL,
            "The Zoom SDK is still initializing. Please wait a moment and try again.",
            "Feeds - Not Ready", MB_OK | MB_ICONINFORMATION);
        return nullptr;
    }

    if (g_activeParticipantSources >= GetMaxFeedsForTier()) {
        std::string msg = "Your current tier allows a maximum of " +
                          std::to_string(GetMaxFeedsForTier()) +
                          " participant feed(s).\n\nUpgrade your plan at:\n"
                          "https://marketplace.zoom.us";
        MessageBoxA(NULL, msg.c_str(), "Feeds - Upgrade Required",
                    MB_OK | MB_ICONINFORMATION);
        return nullptr;
    }
    obs_source_set_async_unbuffered(source, true);
    return new ZoomParticipantSource(source);
}

void zp_destroy(void* data) {
    if (data) delete static_cast<ZoomParticipantSource*>(data);
}

void zp_update(void* data, obs_data_t* settings) {
    if (data) static_cast<ZoomParticipantSource*>(data)->update(settings);
}

void* zs_create(obs_data_t* settings, obs_source_t* source) {
    if (!g_sdkInitialized)
        return nullptr;
    obs_source_set_async_unbuffered(source, true);
    return new ZoomScreenshareSource(source);
}

void zs_destroy(void* data) {
    if (data) delete static_cast<ZoomScreenshareSource*>(data);
}

// ---------------------------------------------------------------------------
// ZOOM SDK DELAY-LOAD HOOK
// Intercepts sdk.dll resolution and loads it from zoom-sdk/ subfolder.
// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR ensures sdk.dll's own directory is
// searched for its dependencies (zcurl.dll, etc.)
// ---------------------------------------------------------------------------
static HMODULE g_hZoomSDK = nullptr;

static FARPROC WINAPI ZoomDelayLoadHook(unsigned dliNotify, PDelayLoadInfo pdli) {
    if (dliNotify == dliNotePreLoadLibrary) {
        if (_stricmp(pdli->szDll, "sdk.dll") == 0) {
            if (!g_hZoomSDK) {
                HMODULE hSelf = nullptr;
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCWSTR)&ZoomDelayLoadHook,
                    &hSelf);

                wchar_t path[MAX_PATH];
                GetModuleFileNameW(hSelf, path, MAX_PATH);
                PathRemoveFileSpecW(path);
                wcscat_s(path, MAX_PATH, L"\\..\\..\\bin\\64bit\\zoom-sdk\\sdk.dll");

                g_hZoomSDK = LoadLibraryExW(path, NULL,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            }
            return (FARPROC)g_hZoomSDK;
        }
    }
    return NULL;
}

const PfnDliHook __pfnDliNotifyHook2 = &ZoomDelayLoadHook;

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
    zoom_participant_info.icon_type      = OBS_ICON_TYPE_CAMERA;
    obs_register_source(&zoom_participant_info);

    zoom_screenshare_info.id             = "zoom_screenshare_source";
    zoom_screenshare_info.type           = OBS_SOURCE_TYPE_INPUT;
    zoom_screenshare_info.output_flags   = OBS_SOURCE_ASYNC_VIDEO;
    zoom_screenshare_info.get_name       = [](void*) { return "Zoom Screenshare"; };
    zoom_screenshare_info.create         = zs_create;
    zoom_screenshare_info.destroy        = zs_destroy;
    zoom_screenshare_info.get_properties = zs_properties;
    zoom_screenshare_info.icon_type      = OBS_ICON_TYPE_DESKTOP_CAPTURE;
    obs_register_source(&zoom_screenshare_info);

    ZOOM_SDK_NAMESPACE::InitParam initParam;
    initParam.strWebDomain = L"https://zoom.us";
    if (ZOOM_SDK_NAMESPACE::InitSDK(initParam) ==
            ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        g_sdkInitialized = true;
    char pluginPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, pluginPath, MAX_PATH);
    std::string obsPath(pluginPath);
    size_t binPos = obsPath.rfind("obs64.exe");
    std::string helperExe = obsPath.substr(0, binPos) + "FeedsLogin.exe";
    std::string command = "\"" + helperExe + "\" \"%1\"";

    HKEY hKey;
    RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Classes\\ldvfeeds",
        0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    RegSetValueExA(hKey, "", 0, REG_SZ,
        (BYTE*)"URL:Feeds Protocol", 19);
    RegSetValueExA(hKey, "URL Protocol", 0, REG_SZ,
        (BYTE*)"", 1);
    RegCloseKey(hKey);

    RegCreateKeyExA(HKEY_CURRENT_USER,
        "Software\\Classes\\ldvfeeds\\shell\\open\\command",
        0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    RegSetValueExA(hKey, "", 0, REG_SZ,
        (BYTE*)command.c_str(), (DWORD)command.size() + 1);
    RegCloseKey(hKey);
    }

    obs_frontend_add_event_callback([](enum obs_frontend_event event, void*) {
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
            SetupPluginMenu();
            if (LoadTokensFromRegistry()) {
                DoSDKAuth();
            }
        }
    }, nullptr);

    return true;
}

void obs_module_unload(void) {
    g_sdkInitialized = false;
    if (g_shareRenderer) {
        try {
            g_shareRenderer->unSubscribe();
            ZOOM_SDK_NAMESPACE::destroyRenderer(g_shareRenderer);
        } catch (...) {}
        g_shareRenderer = nullptr;
    }
}
