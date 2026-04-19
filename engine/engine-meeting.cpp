// engine-meeting.cpp — Meeting service, listeners, and IPC handlers for
// join/leave/get_participants/logout.
//
// Ported behavior from plugin-main.cpp v1.0.0. All SDK listeners are stateless
// callback bridges that forward SDK events to the plugin as IPC messages.
// State (meeting status, active speaker, active sharer, raw-livestream grant)
// lives in engine-side globals here; the plugin receives snapshots via
// messages and keeps its own cache for UI purposes.

#include <windows.h>
#include <wincred.h>
#include <string>
#include <sstream>
#include <cstdio>

#include "zoom_sdk.h"
#include "zoom_sdk_def.h"
#include "meeting_service_interface.h"
#include "auth_service_interface.h"
#include "meeting_service_components/meeting_audio_interface.h"
#include "meeting_service_components/meeting_participants_ctrl_interface.h"
#include "meeting_service_components/meeting_live_stream_interface.h"
#include "meeting_service_components/meeting_sharing_interface.h"
#include "setting_service_interface.h"

// Defined elsewhere in the engine
extern void LogToFile(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {

// From engine-api.cpp
std::string       ZoomApiGet(const std::wstring& path);
std::string       FetchZak();
const std::string& GetUserDisplayName();
const std::string& GetUserPMI();

// ---------------------------------------------------------------------------
// State owned by this translation unit
// ---------------------------------------------------------------------------
static ZOOM_SDK_NAMESPACE::IMeetingService* g_meetingService = nullptr;

static bool         g_rawLiveStreamGranted = false;
static unsigned int g_activeSpeakerUserId  = 0;
static unsigned int g_activeSharerUserId   = 0;
static unsigned int g_activeShareSourceId  = 0;

// ---------------------------------------------------------------------------
// UTF-8 / wide-char helpers
// ---------------------------------------------------------------------------
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

static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1,
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return "";
    std::string result(needed - 1, '\0');  // drop null terminator
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1,
                        &result[0], needed, nullptr, nullptr);
    return result;
}

// Minimal JSON string-escape for participant names that might contain
// quotes, backslashes, or control characters.
static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// Minimal JSON field extractor
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
// Send the current participant list to the plugin. Called on explicit
// get_participants request, and after joining a meeting once
// raw-livestream privilege is granted (so the plugin gets an initial list
// without the user clicking refresh).
// ---------------------------------------------------------------------------
static void SendParticipantList() {
    if (!g_meetingService) {
        SendToPlugin("{\"type\":\"participant_list_changed\",\"my_user_id\":0,\"participants\":[]}");
        return;
    }

    ZOOM_SDK_NAMESPACE::IMeetingParticipantsController* pc =
        g_meetingService->GetMeetingParticipantsController();
    if (!pc) {
        SendToPlugin("{\"type\":\"participant_list_changed\",\"my_user_id\":0,\"participants\":[]}");
        return;
    }

    unsigned int myUserId = 0;
    ZOOM_SDK_NAMESPACE::IUserInfo* mySelf = pc->GetMySelfUser();
    if (mySelf) myUserId = mySelf->GetUserID();

    std::ostringstream msg;
    msg << "{\"type\":\"participant_list_changed\",\"my_user_id\":"
        << myUserId << ",\"participants\":[";

    ZOOM_SDK_NAMESPACE::IList<unsigned int>* userList = pc->GetParticipantsList();
    bool first = true;
    if (userList) {
        for (int i = 0; i < userList->GetCount(); i++) {
            unsigned int uid = userList->GetItem(i);
            ZOOM_SDK_NAMESPACE::IUserInfo* info = pc->GetUserByUserID(uid);
            if (!info) continue;

            std::string name = WideToUtf8(info->GetUserName());
            if (!first) msg << ",";
            msg << "{\"id\":" << uid
                << ",\"name\":\"" << JsonEscape(name) << "\"}";
            first = false;
        }
    }
    msg << "]}";

    SendToPlugin(msg.str());
}

// ---------------------------------------------------------------------------
// Audio listener — tracks active speaker so the plugin can use it for the
// [Active Speaker] participant option.
// ---------------------------------------------------------------------------
class ZoomAudioListener : public ZOOM_SDK_NAMESPACE::IMeetingAudioCtrlEvent {
public:
    virtual void onUserActiveAudioChange(
        ZOOM_SDK_NAMESPACE::IList<unsigned int>* plstActiveAudio) override {
        if (!plstActiveAudio || plstActiveAudio->GetCount() == 0) return;
        unsigned int newSpeaker = plstActiveAudio->GetItem(0);
        if (newSpeaker == g_activeSpeakerUserId) return;
        g_activeSpeakerUserId = newSpeaker;

        char buf[128];
        sprintf_s(buf, "{\"type\":\"active_speaker_changed\",\"participant_id\":%u}",
                  newSpeaker);
        SendToPlugin(buf);
    }
    virtual void onUserAudioStatusChange(
        ZOOM_SDK_NAMESPACE::IList<ZOOM_SDK_NAMESPACE::IUserAudioStatus*>*,
        const zchar_t* = nullptr) override {}
    virtual void onHostRequestStartAudio(
        ZOOM_SDK_NAMESPACE::IRequestStartAudioHandler*) override {}
    virtual void onJoin3rdPartyTelephonyAudio(const zchar_t*) override {}
    virtual void onMuteOnEntryStatusChange(bool) override {}
};
static ZoomAudioListener g_audioListener;

// ---------------------------------------------------------------------------
// Share listener — tracks the active sharer. Phase 5 only forwards status to
// the plugin (used in zs_properties for "Receiving screenshare" vs "Waiting").
// Phase 6 will use these IDs to subscribe the share renderer.
// ---------------------------------------------------------------------------
class ZoomShareListener : public ZOOM_SDK_NAMESPACE::IMeetingShareCtrlEvent {
public:
    virtual void onSharingStatus(
        ZOOM_SDK_NAMESPACE::ZoomSDKSharingSourceInfo shareInfo) override {
        bool changed = false;
        switch (shareInfo.status) {
            case ZOOM_SDK_NAMESPACE::Sharing_Other_Share_Begin:
            case ZOOM_SDK_NAMESPACE::Sharing_Self_Send_Begin:
                g_activeSharerUserId  = shareInfo.userid;
                g_activeShareSourceId = shareInfo.shareSourceID;
                changed = true;
                break;
            case ZOOM_SDK_NAMESPACE::Sharing_Other_Share_End:
            case ZOOM_SDK_NAMESPACE::Sharing_Self_Send_End:
                g_activeSharerUserId  = 0;
                g_activeShareSourceId = 0;
                changed = true;
                break;
            default: break;
        }

        if (changed) {
            char buf[128];
            sprintf_s(buf,
                "{\"type\":\"share_status_changed\",\"sharer_user_id\":%u}",
                g_activeSharerUserId);
            SendToPlugin(buf);
        }
    }
    virtual void onFailedToStartShare() override {}
    virtual void onLockShareStatus(bool) override {}
    virtual void onShareContentNotification(
        ZOOM_SDK_NAMESPACE::ZoomSDKSharingSourceInfo) override {}
    virtual void onMultiShareSwitchToSingleShareNeedConfirm(
        ZOOM_SDK_NAMESPACE::IShareSwitchMultiToSingleConfirmHandler*) override {}
    virtual void onShareSettingTypeChangedNotification(
        ZOOM_SDK_NAMESPACE::ShareSettingType) override {}
    virtual void onSharedVideoEnded() override {}
    virtual void onVideoFileSharePlayError(
        ZOOM_SDK_NAMESPACE::ZoomSDKVideoFileSharePlayError) override {}
    virtual void onOptimizingShareForVideoClipStatusChanged(
        ZOOM_SDK_NAMESPACE::ZoomSDKSharingSourceInfo) override {}
};
static ZoomShareListener g_shareListener;

// ---------------------------------------------------------------------------
// Live stream listener — the critical one. onRawLiveStreamPrivilegeChanged
// is where we get the green light to actually use the raw video stream.
// ---------------------------------------------------------------------------
class ZoomLiveStreamListener
    : public ZOOM_SDK_NAMESPACE::IMeetingLiveStreamCtrlEvent {
public:
    virtual void onRawLiveStreamPrivilegeChanged(bool bHasPrivilege) override {
        LogToFile(bHasPrivilege
            ? "Meeting: raw livestream privilege GRANTED"
            : "Meeting: raw livestream privilege DENIED");

        if (!bHasPrivilege) return;
        g_rawLiveStreamGranted = true;

        if (!g_meetingService) return;

        // Start the raw live stream. This is the step that actually unlocks
        // frame delivery (Phase 6). In Phase 5 we still call it so state on
        // the SDK side reaches the "ready to deliver frames" condition.
        ZOOM_SDK_NAMESPACE::IMeetingLiveStreamController* lsc =
            g_meetingService->GetMeetingLiveStreamController();
        if (lsc) {
            lsc->StartRawLiveStreaming(
                L"https://letsdovideo.com/feeds-support/", L"Feeds");
        }

        // Wire up the share listener now that the share controller is usable
        // and capture any in-progress share so the plugin's screenshare
        // source UI reflects reality on first open.
        ZOOM_SDK_NAMESPACE::IMeetingShareController* sc =
            g_meetingService->GetMeetingShareController();
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

        // Wire up audio (active-speaker) listener now that we're fully in.
        ZOOM_SDK_NAMESPACE::IMeetingAudioController* ac =
            g_meetingService->GetMeetingAudioController();
        if (ac) ac->SetEvent(&g_audioListener);

        // Tell the plugin the meeting is fully ready. This is the signal
        // that zp_properties / zs_properties should flip from the "click to
        // connect" prompt to the connected state.
        SendToPlugin("{\"type\":\"raw_livestream_granted\"}");

        // Send the initial participant list.
        SendParticipantList();

        // Report current share state if someone is already sharing.
        if (g_activeSharerUserId != 0) {
            char buf[128];
            sprintf_s(buf,
                "{\"type\":\"share_status_changed\",\"sharer_user_id\":%u}",
                g_activeSharerUserId);
            SendToPlugin(buf);
        }
    }

    virtual void onLiveStreamStatusChange(
        ZOOM_SDK_NAMESPACE::LiveStreamStatus) override {}
    virtual void onRawLiveStreamPrivilegeRequestTimeout() override {
        LogToFile("Meeting: raw livestream request TIMED OUT");
        SendToPlugin(
            "{\"type\":\"raw_livestream_timeout\"}");
    }
    virtual void onUserRawLiveStreamPrivilegeChanged(
        unsigned int, bool) override {}
    virtual void onRawLiveStreamPrivilegeRequested(
        ZOOM_SDK_NAMESPACE::IRequestRawLiveStreamPrivilegeHandler*) override {}
    virtual void onUserRawLiveStreamingStatusChanged(
        ZOOM_SDK_NAMESPACE::IList<
            ZOOM_SDK_NAMESPACE::RawLiveStreamInfo>*) override {}
    virtual void onLiveStreamReminderStatusChanged(bool) override {}
    virtual void onLiveStreamReminderStatusChangeFailed() override {}
    virtual void onUserThresholdReachedForLiveStream(int) override {}
};
static ZoomLiveStreamListener g_liveStreamListener;

// ---------------------------------------------------------------------------
// Meeting listener — status transitions.
// ---------------------------------------------------------------------------
class ZoomMeetingListener : public ZOOM_SDK_NAMESPACE::IMeetingServiceEvent {
public:
    virtual void onMeetingStatusChanged(
        ZOOM_SDK_NAMESPACE::MeetingStatus status, int iResult = 0) override {
        char msg[128];
        sprintf_s(msg, "Meeting: status changed to %d (result=%d)",
                  (int)status, iResult);
        LogToFile(msg);

        if (status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_INMEETING) {
            if (!g_meetingService) return;

            // Tell the plugin we're in. Meeting number is available from
            // GetMeetingInfo().
            ZOOM_SDK_NAMESPACE::IMeetingInfo* info =
                g_meetingService->GetMeetingInfo();
            unsigned long long meetingNumber = info ? info->GetMeetingNumber() : 0;
            char joinMsg[256];
            sprintf_s(joinMsg,
                "{\"type\":\"meeting_joined\",\"meeting_number\":\"%llu\"}",
                meetingNumber);
            SendToPlugin(joinMsg);

            // Attach the livestream listener. Depending on whether the
            // current user is the host (e.g. joining via PMI), privilege
            // may be immediately available, or we have to request it and
            // the host clicks "Allow" in the Zoom client.
            ZOOM_SDK_NAMESPACE::IMeetingLiveStreamController* lsc =
                g_meetingService->GetMeetingLiveStreamController();
            if (!lsc) return;
            lsc->SetEvent(&g_liveStreamListener);

            if (lsc->CanStartRawLiveStream() ==
                ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
                // Host: privilege available, simulate the "granted" event.
                g_liveStreamListener.onRawLiveStreamPrivilegeChanged(true);
            } else {
                LogToFile("Meeting: requesting raw livestream privilege from host");
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
            SendToPlugin("{\"type\":\"meeting_left\"}");
        }

        if (status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_FAILED) {
            // Map SDK error code to a user-visible message. We do this
            // engine-side because the SDK headers that define these
            // constants live here. Plugin just displays whatever we send.
            const char* errMsg = nullptr;
            switch (iResult) {
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_PASSWORD_ERR:
                    errMsg = "Incorrect meeting password. Please try again.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_MEETING_NOT_EXIST:
                    errMsg = "Meeting not found. Please check the meeting number or link.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_CONNECTION_ERR:
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_RECONNECT_ERR:
                    errMsg = "Connection error. Please check your internet connection and try again.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN:
                    errMsg = "The host has disabled external participants from joining this meeting.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING:
                    errMsg = "This app must be published on the Zoom Marketplace before joining external meetings.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING:
                    errMsg = "This meeting requires you to be logged in to Zoom. Please log in and try again.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_BLOCKED_BY_ACCOUNT_ADMIN:
                    errMsg = "Your Zoom account administrator has blocked this application.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_NEED_SIGN_IN_FOR_PRIVATE_MEETING:
                    errMsg = "This is a private meeting. Please log in to Zoom and try again.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_MEETING_OVER:
                    errMsg = "This meeting has already ended.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_MEETING_NOT_START:
                    errMsg = "This meeting has not started yet.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_MEETING_USER_FULL:
                    errMsg = "This meeting is at maximum capacity.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_MEETING_RESTRICTED:
                    errMsg = "This meeting is restricted.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_MEETING_RESTRICTED_JBH:
                    errMsg = "This meeting does not allow joining before the host.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_ENFORCE_LOGIN:
                    errMsg = "This meeting requires you to be logged in to Zoom.";
                    break;
                default: {
                    static char generic[128];
                    sprintf_s(generic, "Failed to join meeting. Error code: %d", iResult);
                    errMsg = generic;
                    break;
                }
            }

            // Escape the message for JSON. All our strings are plain ASCII
            // so we only need to escape quotes — but be defensive.
            std::string escaped;
            for (const char* p = errMsg; *p; ++p) {
                if (*p == '"')  escaped += "\\\"";
                else if (*p == '\\') escaped += "\\\\";
                else escaped += *p;
            }

            std::string failMsg = "{\"type\":\"meeting_failed\","
                                  "\"code\":" + std::to_string(iResult) + ","
                                  "\"message\":\"" + escaped + "\"}";
            SendToPlugin(failMsg);
        }
    }
    virtual void onMeetingStatisticsWarningNotification(
        ZOOM_SDK_NAMESPACE::StatisticsWarningType) override {}
    virtual void onMeetingParameterNotification(
        const ZOOM_SDK_NAMESPACE::MeetingParameter*) override {}
    virtual void onSuspendParticipantsActivities() override {}
    virtual void onAICompanionActiveChangeNotice(bool) override {}
    virtual void onMeetingTopicChanged(const zchar_t*) override {}
    virtual void onMeetingFullToWatchLiveStream(const zchar_t*) override {}
    virtual void onUserNetworkStatusChanged(
        ZOOM_SDK_NAMESPACE::MeetingComponentType,
        ZOOM_SDK_NAMESPACE::ConnectionQuality,
        unsigned int, bool) override {}
#if defined(WIN32)
    virtual void onAppSignalPanelUpdated(
        ZOOM_SDK_NAMESPACE::IMeetingAppSignalHandler*) override {}
#endif
};
static ZoomMeetingListener g_meetingListener;

// ---------------------------------------------------------------------------
// Configure default SDK settings. Called once right after the meeting
// service is created. The main thing we're fixing here is the auto-full-
// screen behavior: when our subprocess creates the meeting window in full
// screen, the title bar paints for the wrong size and stays broken until
// something forces a layout pass (like pressing Esc). Disabling auto-full-
// screen sidesteps the issue — the window opens in normal windowed mode
// and the user can go full-screen afterward if they want, which works
// correctly because the window is already fully initialized.
// ---------------------------------------------------------------------------
static void ApplyDefaultSettings() {
    ZOOM_SDK_NAMESPACE::ISettingService* settingService = nullptr;
    ZOOM_SDK_NAMESPACE::SDKError err =
        ZOOM_SDK_NAMESPACE::CreateSettingService(&settingService);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS || !settingService) {
        char buf[128];
        sprintf_s(buf, "Settings: CreateSettingService failed: %d", (int)err);
        LogToFile(buf);
        return;
    }

    ZOOM_SDK_NAMESPACE::IGeneralSettingContext* general =
        settingService->GetGeneralSettings();
    if (!general) {
        LogToFile("Settings: GetGeneralSettings returned nullptr");
        return;
    }

    ZOOM_SDK_NAMESPACE::SDKError r =
        general->EnableAutoFullScreenVideoWhenJoinMeeting(false);
    if (r != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        char buf[128];
        sprintf_s(buf, "Settings: EnableAutoFullScreenVideoWhenJoinMeeting(false) failed: %d", (int)r);
        LogToFile(buf);
    } else {
        LogToFile("Settings: disabled auto-full-screen on meeting join");
    }
}

// ---------------------------------------------------------------------------
// Initialization — called from engine-sdk.cpp's onAuthenticationReturn
// after SDK auth succeeds. Creates the meeting service (singleton in the
// SDK, but we only need to do this once), registers the meeting listener,
// and applies default SDK settings.
// ---------------------------------------------------------------------------
bool InitializeMeetingSession() {
    if (g_meetingService) {
        LogToFile("Meeting: InitializeMeetingSession called but already initialized");
        return true;
    }

    ZOOM_SDK_NAMESPACE::SDKError err =
        ZOOM_SDK_NAMESPACE::CreateMeetingService(&g_meetingService);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS || !g_meetingService) {
        char buf[128];
        sprintf_s(buf, "Meeting: CreateMeetingService failed: %d", (int)err);
        LogToFile(buf);
        return false;
    }

    g_meetingService->SetEvent(&g_meetingListener);
    LogToFile("Meeting: service created and listener attached");

    ApplyDefaultSettings();

    return true;
}

// ---------------------------------------------------------------------------
// IPC handler: join_meeting
// Message shape (from plugin):
//   {"type":"join_meeting",
//    "input":"<raw user input or PMI number>",
//    "password":"<optional password or empty>",
//    "is_pmi":true|false}
//
// Plugin is responsible for the user dialog; it passes us the raw text from
// the input box plus whether this was the PMI choice. We parse URL forms
// here (matching v1.0.0 behavior exactly) and fetch a fresh ZAK.
// ---------------------------------------------------------------------------
void HandleJoinMeeting(const std::string& json) {
    LogToFile("Meeting: HandleJoinMeeting called");

    if (!g_meetingService) {
        LogToFile("Meeting: join requested but meeting service not initialized");
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-1,"
                     "\"message\":\"Zoom SDK is not ready. Please try logging in again.\"}");
        return;
    }

    std::string input    = JsonExtractString(json, "input");
    std::string password = JsonExtractString(json, "password");
    // is_pmi is informational for logging; parsing is the same either way
    (void)JsonExtractString(json, "is_pmi");

    if (input.empty()) {
        LogToFile("Meeting: join_meeting received with empty input");
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-2,"
                     "\"message\":\"No meeting number or link was provided.\"}");
        return;
    }

    // Fresh ZAK, and verify display name is cached (should be, from post-auth
    // prefetch, but defensive).
    std::string zak = FetchZak();
    const std::string& displayName = GetUserDisplayName();

    if (zak.empty() || displayName.empty()) {
        LogToFile("Meeting: could not retrieve ZAK or display name");
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-3,"
                     "\"message\":\"Could not retrieve your Zoom account details. "
                     "Please log out and log in again.\"}");
        return;
    }

    // Build join params. These storage lifetimes must outlive the Join()
    // call, so they're static — same pattern as v1.0.0.
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
    if (!password.empty()) {
        s_password = Utf8ToWide(password);
        param.psw  = s_password.c_str();
    } else {
        param.psw = nullptr;
    }

    static std::wstring s_vanityId;
    param.vanityID      = nullptr;
    param.meetingNumber = 0;

    // Parse the input — same rules as v1.0.0:
    //   - "zoom.us/my/<vanityid>" → vanity ID
    //   - "zoom.us/j/<number>"    → meeting number
    //   - anything else           → strip non-digits, treat as meeting number
    auto findSubstr = [](const std::string& s, const std::string& sub) {
        return s.find(sub);
    };

    size_t pos;
    if ((pos = findSubstr(input, "zoom.us/my/")) != std::string::npos) {
        std::string rest = input.substr(pos + 11);
        // Cut at first '?', space, or other URL delimiter
        size_t end = rest.find_first_of("?& \r\n");
        if (end != std::string::npos) rest = rest.substr(0, end);
        s_vanityId     = Utf8ToWide(rest);
        param.vanityID = s_vanityId.c_str();
        LogToFile("Meeting: parsed as vanity ID URL");
    } else if ((pos = findSubstr(input, "zoom.us/j/")) != std::string::npos) {
        std::string rest = input.substr(pos + 10);
        size_t end = rest.find_first_of("?& \r\n");
        if (end != std::string::npos) rest = rest.substr(0, end);
        // Strip anything non-digit
        std::string digits;
        for (char c : rest) if (c >= '0' && c <= '9') digits += c;
        if (digits.empty()) {
            SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-2,"
                         "\"message\":\"Could not parse meeting number from link.\"}");
            return;
        }
        param.meetingNumber = _strtoui64(digits.c_str(), nullptr, 10);
        LogToFile("Meeting: parsed as /j/ URL");
    } else {
        // Raw meeting number or PMI — strip whitespace/dashes/non-digits
        std::string digits;
        for (char c : input) if (c >= '0' && c <= '9') digits += c;
        if (digits.empty()) {
            SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-2,"
                         "\"message\":\"Could not parse a valid meeting number.\"}");
            return;
        }
        param.meetingNumber = _strtoui64(digits.c_str(), nullptr, 10);
        LogToFile("Meeting: parsed as raw meeting number");
    }

    ZOOM_SDK_NAMESPACE::SDKError joinErr = g_meetingService->Join(joinParam);
    if (joinErr != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        char buf[256];
        sprintf_s(buf, "Meeting: Join() call failed immediately: %d", (int)joinErr);
        LogToFile(buf);
        sprintf_s(buf,
            "{\"type\":\"meeting_failed\",\"code\":%d,"
            "\"message\":\"Could not start meeting join. SDK error: %d\"}",
            (int)joinErr, (int)joinErr);
        SendToPlugin(buf);
        return;
    }

    LogToFile("Meeting: Join() call returned SUCCESS, waiting for status events");
}

// ---------------------------------------------------------------------------
// IPC handler: leave_meeting
// ---------------------------------------------------------------------------
void HandleLeaveMeeting(const std::string& /*json*/) {
    LogToFile("Meeting: HandleLeaveMeeting called");
    if (!g_meetingService) return;

    ZOOM_SDK_NAMESPACE::MeetingStatus status = g_meetingService->GetMeetingStatus();
    if (status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_IDLE ||
        status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_ENDED ||
        status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_DISCONNECTING) {
        // Already out. Still send meeting_left so the plugin's UI resets
        // in case it's out of sync.
        SendToPlugin("{\"type\":\"meeting_left\"}");
        return;
    }

    g_meetingService->Leave(ZOOM_SDK_NAMESPACE::LEAVE_MEETING);
    // meeting_left will be sent from onMeetingStatusChanged when the SDK
    // reports DISCONNECTING/ENDED.
}

// ---------------------------------------------------------------------------
// IPC handler: get_participants
// ---------------------------------------------------------------------------
void HandleGetParticipants(const std::string& /*json*/) {
    LogToFile("Meeting: HandleGetParticipants called");
    SendParticipantList();
}

// ---------------------------------------------------------------------------
// IPC handler: logout
// Leaves any active meeting, logs out of the SDK, clears stored tokens,
// confirms with logout_complete.
// ---------------------------------------------------------------------------
void HandleLogout(const std::string& /*json*/);  // fwd, defined below

// The logout handler needs to touch both engine-api (to clear tokens) and
// the auth service (to log out of the SDK), and should leave any active
// meeting first. Keeping the whole thing here is tidier than spreading it
// across files, and nothing outside this TU needs it.
void ClearUserInfo();  // from engine-api.cpp

void HandleLogout(const std::string& /*json*/) {
    LogToFile("Meeting: HandleLogout called");

    // Leave meeting first if we're in one
    if (g_meetingService) {
        ZOOM_SDK_NAMESPACE::MeetingStatus status =
            g_meetingService->GetMeetingStatus();
        if (status != ZOOM_SDK_NAMESPACE::MEETING_STATUS_IDLE &&
            status != ZOOM_SDK_NAMESPACE::MEETING_STATUS_ENDED &&
            status != ZOOM_SDK_NAMESPACE::MEETING_STATUS_DISCONNECTING) {
            g_meetingService->Leave(ZOOM_SDK_NAMESPACE::LEAVE_MEETING);
        }
    }

    // Log out of the SDK
    ZOOM_SDK_NAMESPACE::IAuthService* auth_service = nullptr;
    if (ZOOM_SDK_NAMESPACE::CreateAuthService(&auth_service) ==
            ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS && auth_service) {
        auth_service->LogOut();
    }

    // Clear stored tokens + cached user info
    ClearUserInfo();
    CredDeleteA("Feeds_AccessToken",  CRED_TYPE_GENERIC, 0);
    CredDeleteA("Feeds_RefreshToken", CRED_TYPE_GENERIC, 0);

    SendToPlugin("{\"type\":\"logout_complete\"}");
}

} // namespace feeds_engine
