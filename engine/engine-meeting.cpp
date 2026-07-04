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
#include <functional>
#include <cstdio>

#include "zoom_sdk.h"
#include "zoom_sdk_def.h"
#include "meeting_service_interface.h"
#include "auth_service_interface.h"
#include "meeting_service_components/meeting_audio_interface.h"
#include "meeting_service_components/meeting_chat_interface.h"
#include "meeting_service_components/meeting_configuration_interface.h"
#include "meeting_service_components/meeting_participants_ctrl_interface.h"
#include "meeting_service_components/meeting_video_interface.h"
#include "meeting_service_components/meeting_live_stream_interface.h"
#include "meeting_service_components/meeting_sharing_interface.h"
#include "setting_service_interface.h"

#include "engine-shared.h"

// Defined elsewhere in the engine
extern void LogToFile(const char* msg);  // forwards at DEBUG
extern void LogInfo(const char* msg);
extern void LogWarn(const char* msg);
extern void LogError(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {

// From engine-api.cpp
std::string       ZoomApiGet(const std::wstring& path);
std::string       FetchZak();
const std::string& GetUserDisplayName();
const std::string& GetUserPMI();
const std::string& GetUserEmail();
bool              CreateInstantMeeting(const std::string& topic,
                                       unsigned long long& outId,
                                       std::string& outPassword,
                                       std::string& outJoinUrl);
// Zoom Events API (parsing lives in engine-api.cpp).
std::string       FetchEventsArray(bool& authFailed);
std::string       FetchEventSessionsArray(const std::string& eventId);
bool              FetchEventJoinToken(const std::string& eventId,
                                      const std::string& sessionId,
                                      int& code, std::string& joinToken,
                                      std::string& errorMessage);

// From engine-video.cpp
void TearDownAllVideoSubscriptions();
void NotifyActiveSpeakerChanged(unsigned int newSpeakerId);
void BlankSubscriptionsForUser(unsigned int userId);

// From engine-screenshare.cpp
void UpdateShareSubscription();
void TearDownScreenShare();

// From engine-sdk.cpp — lazy SDK bring-up. EnsureSdkUpThen returns true when
// the SDK isn't up yet (it queued `action` and kicked off init+auth on the
// main thread; the action re-runs after auth) and false when the SDK is
// already authenticated (caller proceeds inline). ResetSdkBringupState clears
// the bring-up flags on logout so the next connect re-auths.
bool EnsureSdkUpThen(std::function<void()> action);
void ResetSdkBringupState();

// ---------------------------------------------------------------------------
// State owned by this translation unit
// ---------------------------------------------------------------------------
static ZOOM_SDK_NAMESPACE::IMeetingService* g_meetingService = nullptr;

static bool         g_rawLiveStreamGranted = false;
static unsigned int g_activeSpeakerUserId  = 0;
static unsigned int g_activeSharerUserId   = 0;
static unsigned int g_activeShareSourceId  = 0;

// Populated by HandleCreateInstantMeeting from the REST response and
// consumed (+ cleared) by onMeetingStatusChanged when MEETING_STATUS_INMEETING
// fires, so they're attached to the meeting_joined IPC for the plugin's
// share-this-meeting popup. Empty means "not an instant-meeting join"
// — the regular Join() path leaves these untouched and the plugin sees
// no extra fields in meeting_joined.
static std::string  g_pendingInstantJoinUrl;
static std::string  g_pendingInstantPassword;

// ---------------------------------------------------------------------------
// Accessors exposed to other engine translation units (engine-video.cpp
// needs the meeting service to check IsVideoOn() on a prospective active
// speaker, and needs the Feeds user's own ID to filter them out).
// ---------------------------------------------------------------------------
ZOOM_SDK_NAMESPACE::IMeetingService* GetMeetingService() {
    return g_meetingService;
}

unsigned int GetMySelfUserId() {
    if (!g_meetingService) return 0;
    auto* pc = g_meetingService->GetMeetingParticipantsController();
    if (!pc) return 0;
    auto* me = pc->GetMySelfUser();
    if (!me) return 0;
    return me->GetUserID();
}

// engine-screenshare.cpp reads the current share state via these.
unsigned int GetActiveSharerUserId()   { return g_activeSharerUserId;  }
unsigned int GetActiveShareSourceId()  { return g_activeShareSourceId; }

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

// Escape-aware JSON string extractor. JsonExtractString breaks at the
// first quote without honouring escapes, which truncates any field
// containing \" — fine for plain identifiers, broken for user-entered
// chat content. Mirrors the plugin's ExtractJsonStringEscaped.
static std::string JsonExtractStringEscaped(const std::string& json,
                                            const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();

    std::string out;
    out.reserve(json.size() - pos);
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') break;
        if (c == '\\' && pos + 1 < json.size()) {
            char n = json[pos + 1];
            switch (n) {
                case '"': case '\\': case '/': out += n;   pos += 2; break;
                case 'b': out += '\b'; pos += 2; break;
                case 'f': out += '\f'; pos += 2; break;
                case 'n': out += '\n'; pos += 2; break;
                case 'r': out += '\r'; pos += 2; break;
                case 't': out += '\t'; pos += 2; break;
                case 'u': pos += 6; break;
                default:  out += n; pos += 2; break;
            }
        } else {
            out += c;
            pos++;
        }
    }
    return out;
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

        // Let engine-video.cpp retarget any follow-active-speaker
        // subscriptions. The function filters out the Feeds user and
        // speakers with video off, so we just pass the raw ID.
        NotifyActiveSpeakerChanged(newSpeaker);
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
// Share listener — tracks the active sharer and drives the screenshare
// SDK subscription via UpdateShareSubscription() in engine-screenshare.cpp.
// Also forwards state to the plugin so zs_properties shows "Receiving"
// vs "Waiting" text.
// ---------------------------------------------------------------------------
class ZoomShareListener : public ZOOM_SDK_NAMESPACE::IMeetingShareCtrlEvent {
public:
    virtual void onSharingStatus(
        ZOOM_SDK_NAMESPACE::ZoomSDKSharingSourceInfo shareInfo) override {
        bool changed = false;
        switch (shareInfo.status) {
            case ZOOM_SDK_NAMESPACE::Sharing_Other_Share_Begin:
            case ZOOM_SDK_NAMESPACE::Sharing_Self_Send_Begin:
                // v1.0.0 choice: "most recent sharer wins" if multiple
                // people are sharing. We just overwrite the globals.
                g_activeSharerUserId  = shareInfo.userid;
                g_activeShareSourceId = shareInfo.shareSourceID;
                changed = true;
                break;
            case ZOOM_SDK_NAMESPACE::Sharing_Other_Share_End:
            case ZOOM_SDK_NAMESPACE::Sharing_Self_Send_End:
                // Only zero the globals if the user who stopped sharing is
                // the user we were currently tracking. When one sharer
                // replaces another, the SDK fires the new sharer's Begin
                // event before the old sharer's End event. Without this
                // check, the End event for the previous sharer would
                // incorrectly zero out the globals we just set for the
                // new sharer, causing the screenshare source to revert to
                // "Waiting for screenshare" 300ms after the swap.
                if (shareInfo.userid == g_activeSharerUserId) {
                    g_activeSharerUserId  = 0;
                    g_activeShareSourceId = 0;
                    changed = true;
                }
                break;
            default: break;
        }

        if (changed) {
            // Drive the screenshare renderer — subscribe, resubscribe, or
            // unsubscribe based on the new state.
            UpdateShareSubscription();

            // Tell the plugin so its zs_properties status text updates
            // and any ready-to-open screenshare sources can start their
            // pump threads.
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
// Participants listener — tracks meeting membership so the plugin's
// participant dropdown can auto-refresh on user join/leave/rename without
// the user clicking "Refresh Participant List". Independent of raw-
// livestream privilege: the participants controller is available from
// MEETING_STATUS_INMEETING because membership is meeting state, not
// stream data. Gating this on livestream-grant would create a window
// where joins/leaves go undetected.
// ---------------------------------------------------------------------------
class ZoomParticipantsListener
    : public ZOOM_SDK_NAMESPACE::IMeetingParticipantsCtrlEvent {
public:
    virtual void onUserJoin(
        ZOOM_SDK_NAMESPACE::IList<unsigned int>*,
        const zchar_t* = nullptr) override {
        SendParticipantList();
    }
    virtual void onUserLeft(
        ZOOM_SDK_NAMESPACE::IList<unsigned int>* lstUserID,
        const zchar_t* = nullptr) override {
        // Blank any subscriptions bound to the departing users. Zoom does
        // not fire RawData_Off on participant-leave (only on camera-off
        // while still in the meeting), so without this hook the plugin
        // would freeze on the last received frame.
        if (lstUserID) {
            for (int i = 0; i < lstUserID->GetCount(); ++i) {
                BlankSubscriptionsForUser(lstUserID->GetItem(i));
            }
        }
        SendParticipantList();
    }
    virtual void onUserNamesChanged(
        ZOOM_SDK_NAMESPACE::IList<unsigned int>*) override {
        SendParticipantList();
    }

    // Other IMeetingParticipantsCtrlEvent callbacks — required by the
    // pure-virtual interface but not currently used by Feeds.
    virtual void onHostChangeNotification(unsigned int) override {}
    virtual void onLowOrRaiseHandStatusChanged(bool, unsigned int) override {}
    virtual void onCoHostChangeNotification(unsigned int, bool) override {}
    virtual void onInvalidReclaimHostkey() override {}
    virtual void onAllHandsLowered() override {}
    virtual void onLocalRecordingStatusChanged(
        unsigned int, ZOOM_SDK_NAMESPACE::RecordingStatus) override {}
    virtual void onAllowParticipantsRenameNotification(bool) override {}
    virtual void onAllowParticipantsUnmuteSelfNotification(bool) override {}
    virtual void onAllowParticipantsStartVideoNotification(bool) override {}
    virtual void onAllowParticipantsShareWhiteBoardNotification(bool) override {}
    virtual void onRequestLocalRecordingPrivilegeChanged(
        ZOOM_SDK_NAMESPACE::LocalRecordingRequestPrivilegeStatus) override {}
    virtual void onAllowParticipantsRequestCloudRecording(bool) override {}
    virtual void onInMeetingUserAvatarPathUpdated(unsigned int) override {}
    virtual void onParticipantProfilePictureStatusChange(bool) override {}
    virtual void onFocusModeStateChanged(bool) override {}
    virtual void onFocusModeShareTypeChanged(
        ZOOM_SDK_NAMESPACE::FocusModeShareType) override {}
    virtual void onBotAuthorizerRelationChanged(unsigned int) override {}
    virtual void onVirtualNameTagStatusChanged(bool, unsigned int) override {}
    virtual void onVirtualNameTagRosterInfoUpdated(unsigned int) override {}
#if defined(WIN32)
    virtual void onCreateCompanionRelation(unsigned int, unsigned int) override {}
    virtual void onRemoveCompanionRelation(unsigned int) override {}
#endif
    virtual void onGrantCoOwnerPrivilegeChanged(bool) override {}
};
static ZoomParticipantsListener g_participantsListener;

// ---------------------------------------------------------------------------
// Video controller listener — drives camera-on re-establishment. When a
// participant turns their camera on, some of their same-userId renderers can be
// left created-but-unfed (the SDK resumes delivery to only some, and nothing
// re-establishes the rest); a camera toggle also changes no userId, so it fires
// no reconcile/recreate on its own. On Video_ON we re-run the delivery-gated
// recreate path for that participant's sources. The status log fires for EVERY
// transition — it's the confirmation instrument for whether this SDK event
// actually fires (esp. on the rejoin-camera-off -> on path).
// ---------------------------------------------------------------------------
class ZoomVideoListener
    : public ZOOM_SDK_NAMESPACE::IMeetingVideoCtrlEvent {
public:
    virtual void onUserVideoStatusChange(
        unsigned int userId,
        ZOOM_SDK_NAMESPACE::VideoStatus status) override {
        // Camera turned on: re-establish this participant's sources via the
        // existing delivery-gated recreate path (recovers renderers left
        // created-but-unfed while the camera was off). Hand off to the main
        // thread — this runs on an SDK thread, so no g_subs access / no lock
        // here (mirrors the frame-recv post). The main-thread handler debounces.
        if (status == ZOOM_SDK_NAMESPACE::Video_ON && g_anchorWnd) {
            PostMessageW(g_anchorWnd, WM_FEEDS_CAMERA_ON, (WPARAM)userId, 0);
        }
    }

    // Remaining IMeetingVideoCtrlEvent callbacks — required by the pure-virtual
    // interface, unused by Feeds.
    virtual void onSpotlightedUserListChangeNotification(
        ZOOM_SDK_NAMESPACE::IList<unsigned int>*) override {}
    virtual void onHostRequestStartVideo(
        ZOOM_SDK_NAMESPACE::IRequestStartVideoHandler*) override {}
    virtual void onActiveSpeakerVideoUserChanged(unsigned int) override {}
    virtual void onActiveVideoUserChanged(unsigned int) override {}
    virtual void onHostVideoOrderUpdated(
        ZOOM_SDK_NAMESPACE::IList<unsigned int>*) override {}
    virtual void onLocalVideoOrderUpdated(
        ZOOM_SDK_NAMESPACE::IList<unsigned int>*) override {}
    virtual void onFollowHostVideoOrderChanged(bool) override {}
    virtual void onUserVideoQualityChanged(
        ZOOM_SDK_NAMESPACE::VideoConnectionQuality, unsigned int) override {}
    virtual void onVideoAlphaChannelStatusChanged(bool) override {}
    virtual void onCameraControlRequestReceived(
        unsigned int, ZOOM_SDK_NAMESPACE::CameraControlRequestType,
        ZOOM_SDK_NAMESPACE::ICameraControlRequestHandler*) override {}
    virtual void onCameraControlRequestResult(
        unsigned int, ZOOM_SDK_NAMESPACE::CameraControlRequestResult) override {}
};
static ZoomVideoListener g_videoListener;

// ---------------------------------------------------------------------------
// Chat listener — receives messages from the SDK's chat controller and
// forwards public ("to everyone") messages to the plugin via the
// chat_message IPC. Non-public messages (DMs, panelist-only, waiting room)
// are filtered out here at the engine boundary and never reach the plugin.
// This is structural: the plugin literally cannot leak private chat
// because it never receives it. Filtering at the source is the correct
// security posture for this feature.
// ---------------------------------------------------------------------------
class ZoomChatListener : public ZOOM_SDK_NAMESPACE::IMeetingChatCtrlEvent {
public:
    virtual void onChatMsgNotification(
        ZOOM_SDK_NAMESPACE::IChatMsgInfo* chatMsg,
        const zchar_t* /*content*/ = nullptr) override {
        if (!chatMsg) {
            LogToFile("Chat: onChatMsgNotification with null chatMsg");
            return;
        }

        if (!chatMsg->IsChatToAll()) {
            // Privacy filter. Log only the chat-type enum — never include
            // the content or sender of a non-public message, even at debug
            // level, so engine logs cannot leak DMs.
            char buf[128];
            sprintf_s(buf, "Chat: filtered non-public message (type=%d)",
                      (int)chatMsg->GetChatMessageType());
            LogToFile(buf);
            return;
        }

        std::string messageId  = WideToUtf8(chatMsg->GetMessageID());
        unsigned int senderId  = chatMsg->GetSenderUserId();
        std::string senderName = WideToUtf8(chatMsg->GetSenderDisplayName());
        std::string content    = WideToUtf8(chatMsg->GetContent());
        time_t timestamp       = chatMsg->GetTimeStamp();

        // SDK writes avatars to %APPDATA%\ZoomSDK\data\ConfAvatar\ —
        // a per-user roaming location accessible to both engine and
        // plugin processes — so we can pass the path itself. Empty
        // string when the user has no profile picture; plugin falls
        // back to the bundled Feeds logo. JsonEscape handles the
        // backslashes for us.
        ZOOM_SDK_NAMESPACE::IMeetingParticipantsController* pc =
            g_meetingService
                ? g_meetingService->GetMeetingParticipantsController()
                : nullptr;
        ZOOM_SDK_NAMESPACE::IUserInfo* info =
            pc ? pc->GetUserByUserID(senderId) : nullptr;
        const zchar_t* avatarPathRaw = info ? info->GetAvatarPath() : nullptr;
        std::string avatarPath =
            avatarPathRaw ? WideToUtf8(avatarPathRaw) : "";

        std::ostringstream out;
        out << "{\"type\":\"chat_message\","
            << "\"message_id\":\"" << JsonEscape(messageId) << "\","
            << "\"sender_id\":" << senderId << ","
            << "\"sender_name\":\"" << JsonEscape(senderName) << "\","
            << "\"content\":\"" << JsonEscape(content) << "\","
            << "\"avatar_path\":\"" << JsonEscape(avatarPath) << "\","
            << "\"timestamp\":" << (long long)timestamp << "}";
        SendToPlugin(out.str());

        // Debug-visibility log only — the full content lives in the IPC.
        // Truncate the preview so log lines stay tidy.
        std::string preview = content.size() > 40
            ? content.substr(0, 40) + "..."
            : content;
        char buf[512];
        sprintf_s(buf, "Chat: public message from %s: %s",
                  senderName.c_str(), preview.c_str());
        LogToFile(buf);
    }

    virtual void onChatStatusChangedNotification(
        ZOOM_SDK_NAMESPACE::ChatStatus*) override {
        LogToFile("Chat: onChatStatusChangedNotification fired");
    }

    virtual void onChatMsgDeleteNotification(
        const zchar_t* msgID,
        ZOOM_SDK_NAMESPACE::SDKChatMessageDeleteType deleteBy) override {
        std::string id = WideToUtf8(msgID);
        char buf[256];
        sprintf_s(buf, "Chat: onChatMsgDeleteNotification id=%s deleteBy=%d",
                  id.c_str(), (int)deleteBy);
        LogToFile(buf);
    }

    virtual void onChatMessageEditNotification(
        ZOOM_SDK_NAMESPACE::IChatMsgInfo* chatMsg) override {
        std::string id = chatMsg ? WideToUtf8(chatMsg->GetMessageID()) : "";
        char buf[256];
        sprintf_s(buf, "Chat: onChatMessageEditNotification id=%s",
                  id.c_str());
        LogToFile(buf);
    }

    virtual void onShareMeetingChatStatusChanged(bool) override {}
    virtual void onFileSendStart(
        ZOOM_SDK_NAMESPACE::ISDKFileSender*) override {}
    virtual void onFileReceived(
        ZOOM_SDK_NAMESPACE::ISDKFileReceiver*) override {}
    virtual void onFileTransferProgress(
        ZOOM_SDK_NAMESPACE::SDKFileTransferInfo*) override {}
};
static ZoomChatListener g_chatListener;

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

        if (!bHasPrivilege) {
            // Tell the plugin the host declined (or revoked) so the
            // source properties panel can swap from "waiting" to the
            // denied messaging. Also fires when the host removes our
            // already-granted privilege mid-meeting (acceptable
            // side-effect coverage — frame delivery stops either way).
            g_rawLiveStreamGranted = false;
            SendToPlugin("{\"type\":\"raw_livestream_denied\"}");
            return;
        }
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

        // (Initial participant list and listener are wired at meeting-join
        // in onMeetingStatusChanged — they don't depend on livestream
        // privilege, only on meeting membership being available.)

        // Report current share state if someone is already sharing.
        if (g_activeSharerUserId != 0) {
            // Drive the screenshare renderer to subscribe to the in-
            // progress share. Same call the listener makes when new
            // shares start — safe to call unconditionally since it
            // handles the "no sharer" case as a no-op.
            UpdateShareSubscription();

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
        // The SDK has been observed firing this 10–25 s after a
        // successful grant — its internal request-timeout timer doesn't
        // get cancelled when the host approves. If we already have
        // privilege, the timeout is spurious; forwarding it would
        // overwrite the plugin's Granted state with TimedOut. Suppress
        // here; the plugin has a matching defensive check.
        if (g_rawLiveStreamGranted) {
            LogToFile("Meeting: ignoring raw livestream timeout — "
                      "privilege already granted (spurious SDK callback)");
            return;
        }
        LogToFile("Meeting: raw livestream request TIMED OUT");
        SendToPlugin("{\"type\":\"raw_livestream_timeout\"}");
    }
    virtual void onUserRawLiveStreamPrivilegeChanged(
        unsigned int, bool) override {}
    virtual void onRawLiveStreamPrivilegeRequested(
        ZOOM_SDK_NAMESPACE::IRequestRawLiveStreamPrivilegeHandler*) override {}
    virtual void onUserRawLiveStreamingStatusChanged(
        ZOOM_SDK_NAMESPACE::IList<
            ZOOM_SDK_NAMESPACE::RawLiveStreamInfo>* liveStreamList) override {
        // Self present in the raw-live-streaming list == the raw-data renderer
        // subsystem is ready. Open the readiness gate so engine-video drains any
        // subscribes queued at the grant instant (the Option C fix).
        unsigned int mySelf = GetMySelfUserId();
        int count = liveStreamList ? liveStreamList->GetCount() : 0;
        bool selfPresent = false;
        for (int i = 0; i < count; ++i) {
            ZOOM_SDK_NAMESPACE::RawLiveStreamInfo info = liveStreamList->GetItem(i);
            if (mySelf != 0 && info.userId == mySelf) { selfPresent = true; break; }
        }
        if (selfPresent) NotifyRawRenderReady();
    }
    virtual void onLiveStreamReminderStatusChanged(bool) override {}
    virtual void onLiveStreamReminderStatusChangeFailed() override {}
    virtual void onUserThresholdReachedForLiveStream(int) override {}
};
static ZoomLiveStreamListener g_liveStreamListener;

// ---------------------------------------------------------------------------
// Configuration listener — required for webinar joins. Without a subscribed
// IMeetingConfigurationEvent, the SDK's webinar code path falls through to
// MEETING_FAIL_UNKNOWN (65535) because callbacks like
// onWebinarNeedRegisterNotification have nowhere to deliver.
// ---------------------------------------------------------------------------
class ZoomConfigurationListener
    : public ZOOM_SDK_NAMESPACE::IMeetingConfigurationEvent {
public:
    // Set by HandleJoinMeeting before each Join() so the effective per-session
    // name (possibly user-overridden) is what callbacks respond with.
    void SetEffectiveDisplayName(const std::wstring& name) {
        m_displayName = name;
    }

    virtual void onWebinarNeedRegisterNotification(
        ZOOM_SDK_NAMESPACE::IWebinarNeedRegisterHandler* handler) override {
        if (!handler) {
            LogToFile("Config: onWebinarNeedRegisterNotification handler null");
            return;
        }
        auto regType = handler->GetWebinarNeedRegisterType();
        char buf[256];
        sprintf_s(buf, "Config: onWebinarNeedRegisterNotification type=%d",
                  (int)regType);
        LogToFile(buf);

        if (regType == ZOOM_SDK_NAMESPACE::IWebinarNeedRegisterHandler::
                       WebinarReg_By_Email_and_DisplayName) {
            auto* emailHandler = dynamic_cast<
                ZOOM_SDK_NAMESPACE::IWebinarNeedRegisterHandlerByEmail*>(
                    handler);
            if (!emailHandler) {
                LogToFile("Config: cast to IWebinarNeedRegisterHandlerByEmail failed");
                return;
            }
            const std::string& email = GetUserEmail();
            std::wstring wEmail = Utf8ToWide(email);
            std::wstring wName  = EffectiveName();
            ZOOM_SDK_NAMESPACE::SDKError err =
                emailHandler->InputWebinarRegisterEmailAndScreenName(
                    wEmail.c_str(), wName.c_str());
            sprintf_s(buf,
                "Config: InputWebinarRegisterEmailAndScreenName returned %d "
                "(email_present=%d)", (int)err, email.empty() ? 0 : 1);
            LogToFile(buf);
        } else if (regType == ZOOM_SDK_NAMESPACE::IWebinarNeedRegisterHandler::
                              WebinarReg_By_Register_Url) {
            auto* urlHandler = dynamic_cast<
                ZOOM_SDK_NAMESPACE::IWebinarNeedRegisterHandlerByUrl*>(handler);
            if (urlHandler) {
                LogToFile("Config: webinar requires browser-based registration; "
                          "releasing handler");
                urlHandler->Release();
            } else {
                LogToFile("Config: cast to IWebinarNeedRegisterHandlerByUrl failed");
            }
            SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-5,"
                         "\"message\":\"This webinar requires you to register "
                         "in your browser before joining. Please complete "
                         "registration on Zoom's website first.\"}");
        } else {
            LogToFile("Config: WebinarReg_NONE or unknown register type, ignoring");
        }
    }

    virtual void onWebinarNeedInputScreenName(
        ZOOM_SDK_NAMESPACE::IWebinarInputScreenNameHandler* handler) override {
        if (!handler) {
            LogToFile("Config: onWebinarNeedInputScreenName handler null");
            return;
        }
        LogToFile("Config: onWebinarNeedInputScreenName fired");
        std::wstring wName = EffectiveName();
        ZOOM_SDK_NAMESPACE::SDKError err = handler->InputName(wName.c_str());
        char buf[128];
        sprintf_s(buf, "Config: InputName returned %d", (int)err);
        LogToFile(buf);
    }

    virtual void onJoinMeetingNeedUserInfo(
        ZOOM_SDK_NAMESPACE::IMeetingInputUserInfoHandler* handler) override {
        if (!handler) {
            LogToFile("Config: onJoinMeetingNeedUserInfo handler null");
            return;
        }
        LogToFile("Config: onJoinMeetingNeedUserInfo fired");
        const std::string& email = GetUserEmail();
        std::wstring wEmail = Utf8ToWide(email);
        std::wstring wName  = EffectiveName();
        ZOOM_SDK_NAMESPACE::SDKError err =
            handler->InputUserInfo(wName.c_str(), wEmail.c_str());
        char buf[128];
        sprintf_s(buf, "Config: InputUserInfo returned %d (email_present=%d)",
                  (int)err, email.empty() ? 0 : 1);
        LogToFile(buf);
    }

    // Remaining IMeetingConfigurationEvent callbacks — their corresponding
    // RedirectXxx(true) toggles aren't enabled, so these shouldn't fire. Log
    // if they do; that's a signal something unexpected is happening.
    virtual void onInputMeetingPasswordAndScreenNameNotification(
        ZOOM_SDK_NAMESPACE::IMeetingPasswordAndScreenNameHandler*) override {
        LogToFile("Config: onInputMeetingPasswordAndScreenName fired (unexpected)");
    }
    virtual void onEndOtherMeetingToJoinMeetingNotification(
        ZOOM_SDK_NAMESPACE::IEndOtherMeetingToJoinMeetingHandler*) override {
        LogToFile("Config: onEndOtherMeetingToJoinMeeting fired (unexpected)");
    }
    virtual void onUserConfirmToStartArchive(
        ZOOM_SDK_NAMESPACE::IMeetingArchiveConfirmHandler*) override {
        LogToFile("Config: onUserConfirmToStartArchive fired (unexpected)");
    }
    virtual void onUserConfirmRecoverMeeting(
        ZOOM_SDK_NAMESPACE::IMeetingConfirmRecoverHandler*) override {
        LogToFile("Config: onUserConfirmRecoverMeeting fired (unexpected)");
    }

    // IMeetingConfigurationFreeMeetingEvent — free-tier callbacks, not
    // relevant to Feeds. Empty bodies satisfy the pure-virtual contract.
    virtual void onFreeMeetingRemainTime(unsigned int) override {}
    virtual void onFreeMeetingRemainTimeStopCountDown() override {}
    virtual void onFreeMeetingNeedToUpgrade(
        FreeMeetingNeedUpgradeType, const zchar_t*) override {}
    virtual void onFreeMeetingUpgradeToGiftFreeTrialStart() override {}
    virtual void onFreeMeetingUpgradeToGiftFreeTrialStop() override {}
    virtual void onFreeMeetingUpgradeToProMeeting() override {}

private:
    std::wstring EffectiveName() const {
        return m_displayName.empty()
            ? Utf8ToWide(GetUserDisplayName())
            : m_displayName;
    }

    std::wstring m_displayName;
};
static ZoomConfigurationListener g_configurationListener;

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
            // GetMeetingInfo(). For an instant-meeting Start() flow, the
            // REST response's join_url + password are attached so the
            // plugin can render its share-this-meeting popup. Regular
            // Join() paths leave those globals empty and the fields are
            // omitted from the payload.
            ZOOM_SDK_NAMESPACE::IMeetingInfo* info =
                g_meetingService->GetMeetingInfo();
            unsigned long long meetingNumber = info ? info->GetMeetingNumber() : 0;

            std::string joinMsg = "{\"type\":\"meeting_joined\","
                                  "\"meeting_number\":\"" +
                                  std::to_string(meetingNumber) + "\"";
            if (!g_pendingInstantJoinUrl.empty()) {
                joinMsg += ",\"join_url\":\"" +
                           JsonEscape(g_pendingInstantJoinUrl) + "\"";
            }
            if (!g_pendingInstantPassword.empty()) {
                joinMsg += ",\"password\":\"" +
                           JsonEscape(g_pendingInstantPassword) + "\"";
            }
            joinMsg += "}";
            SendToPlugin(joinMsg);
            // Consumed — clear so a subsequent non-instant Join doesn't
            // see leftover fields.
            g_pendingInstantJoinUrl.clear();
            g_pendingInstantPassword.clear();

            // Diagnostic logs: meeting type and local user role. Useful
            // for confirming webinar joins succeed and identifying the
            // role we got. Fire for every meeting, regardless of type.
            if (info) {
                char buf[128];
                sprintf_s(buf, "Meeting: type=%d (1=normal, 2=webinar, 3=breakout)",
                          (int)info->GetMeetingType());
                LogInfo(buf);
            }

            // Send the initial participant list and wire up the
            // participants listener so the plugin's dropdown can refresh
            // live on user join/leave/rename. Done here at meeting-join
            // (not at livestream-grant) so we don't miss membership
            // changes during the privilege-request window.
            ZOOM_SDK_NAMESPACE::IMeetingParticipantsController* pc =
                g_meetingService->GetMeetingParticipantsController();
            if (pc) {
                auto* me = pc->GetMySelfUser();
                if (me) {
                    char buf[128];
                    sprintf_s(buf, "Meeting: local user role=%d "
                                   "(1=host, 2=cohost, 3=panelist, 5=attendee)",
                              (int)me->GetUserRole());
                    LogInfo(buf);
                }
                SendParticipantList();
                pc->SetEvent(&g_participantsListener);
            } else {
                LogToFile("Meeting: participants controller unavailable at join");
            }

            // Attach the chat listener. Independent of livestream
            // privilege — chat is meeting state, not stream data, so
            // it's available from MEETING_STATUS_INMEETING. Gating on
            // livestream-grant would miss messages sent during the
            // privilege-request window.
            ZOOM_SDK_NAMESPACE::IMeetingChatController* cc =
                g_meetingService->GetMeetingChatController();
            if (cc) {
                cc->SetEvent(&g_chatListener);
                LogToFile("Meeting: chat controller listener attached");
            } else {
                LogToFile("Meeting: chat controller unavailable at join");
            }

            // Attach the video listener so a participant turning their camera ON
            // re-runs the delivery-gated recreate path for their sources
            // (recovers renderers left created-but-unfed while the camera was
            // off — the camera-toggle and rejoin-camera-off cases). Same
            // register-at-join lifecycle as the other controllers; the SDK tears
            // the controller down on meeting end.
            ZOOM_SDK_NAMESPACE::IMeetingVideoController* vc =
                g_meetingService->GetMeetingVideoController();
            if (vc) {
                vc->SetEvent(&g_videoListener);
                LogToFile("Meeting: video controller listener attached");
            } else {
                LogToFile("Meeting: video controller unavailable at join");
            }

            // Attach the livestream listener. Depending on whether the
            // current user is the host (e.g. joining via PMI), privilege
            // may be immediately available, or we have to request it and
            // the host clicks "Allow" in the Zoom client.
            //
            // Either way, we wait for the real onRawLiveStreamPrivilegeChanged
            // callback rather than firing it manually. Firing it manually
            // caused a race: the plugin would send subscribe before the
            // SDK's internal state was ready, and createRenderer would
            // return SDKERR_NO_PERMISSION (12).
            ZOOM_SDK_NAMESPACE::IMeetingLiveStreamController* lsc =
                g_meetingService->GetMeetingLiveStreamController();
            if (!lsc) return;
            lsc->SetEvent(&g_liveStreamListener);

            if (lsc->CanStartRawLiveStream() !=
                ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
                LogToFile("Meeting: requesting raw livestream privilege from host");
                lsc->RequestRawLiveStreaming(
                    L"https://letsdovideo.com/feeds-support/", L"Feeds");
                // Tell the plugin we're waiting on the host so the
                // source properties panel can show "Waiting for host"
                // instead of the participant dropdown. The host path
                // below skips this — they get privilege automatically
                // and the granted/denied callback drives the UI.
                SendToPlugin("{\"type\":\"raw_livestream_pending\"}");
            }
            // Otherwise: we're the host, privilege is already available.
            // The SDK will fire onRawLiveStreamPrivilegeChanged(true) on
            // its own schedule. Waiting for that real event prevents the
            // race that causes createRenderer to fail with NO_PERMISSION.
        }

        if (status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_ENDED ||
            status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_DISCONNECTING) {
            g_rawLiveStreamGranted = false;
            g_activeSharerUserId   = 0;
            g_activeShareSourceId  = 0;
            g_activeSpeakerUserId  = 0;
            TearDownAllVideoSubscriptions();
            TearDownScreenShare();
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
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_REGISTERWEBINAR_FULL:
                    errMsg = "This webinar has reached its registration limit and is not accepting new attendees.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_REGISTERWEBINAR_HOSTREGISTER:
                    errMsg = "This webinar requires the host to complete registration before joining.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_REGISTERWEBINAR_PANELISTREGISTER:
                    errMsg = "Unable to join as panelist. The webinar host may need to add you as a panelist on Zoom's website first.";
                    break;
                case ZOOM_SDK_NAMESPACE::MEETING_FAIL_REGISTERWEBINAR_DENIED_EMAIL:
                    errMsg = "Your email address is not authorized to register for this webinar.";
                    break;
                case ZOOM_SDK_NAMESPACE::CONF_FAIL_JOIN_WEBINAR_WITHSAMEEMAIL:
                    errMsg = "You are already in this webinar from another session. Please leave the other session and try again.";
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

        if (status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_WEBINAR_PROMOTE) {
            LogInfo("Meeting: WEBINAR_PROMOTE — local user promoted to panelist");
            if (g_meetingService) {
                ZOOM_SDK_NAMESPACE::IMeetingParticipantsController* pc =
                    g_meetingService->GetMeetingParticipantsController();
                if (pc) {
                    auto* me = pc->GetMySelfUser();
                    if (me) {
                        char buf[128];
                        sprintf_s(buf, "Meeting: post-promote role=%d",
                                  (int)me->GetUserRole());
                        LogToFile(buf);
                    }
                    SendParticipantList();
                }
            }
        }

        if (status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_WEBINAR_DEPROMOTE) {
            LogInfo("Meeting: WEBINAR_DEPROMOTE — local user demoted to attendee");
            if (g_meetingService) {
                ZOOM_SDK_NAMESPACE::IMeetingParticipantsController* pc =
                    g_meetingService->GetMeetingParticipantsController();
                if (pc) SendParticipantList();
            }
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
        LogError(buf);
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
        LogError(buf);
        return false;
    }

    g_meetingService->SetEvent(&g_meetingListener);
    LogToFile("Meeting: service created and listener attached");

    ZOOM_SDK_NAMESPACE::IMeetingConfiguration* config =
        g_meetingService->GetMeetingConfiguration();
    if (config) {
        config->SetEvent(&g_configurationListener);
        config->RedirectWebinarNeedRegister(true);
        config->RedirectWebinarNameInputDialog(true);
        config->RedirectMeetingInputUserInfoDialog(true);
        LogToFile("Meeting: configuration listener attached, webinar redirects enabled");
    } else {
        LogWarn("Meeting: GetMeetingConfiguration returned null — webinar joins may fail");
    }

    ApplyDefaultSettings();

    return true;
}

// ---------------------------------------------------------------------------
// IPC handler: join_meeting
// Message shape (from plugin):
//   {"type":"join_meeting",
//    "input":"<raw user input or PMI number>",
//    "password":"<optional password or empty>",
//    "display_name":"<optional session-only name override or empty>",
//    "is_pmi":true|false}
//
// Plugin is responsible for the user dialog; it passes us the raw text from
// the input box plus whether this was the PMI choice. We parse URL forms
// here (matching v1.0.0 behavior exactly) and fetch a fresh ZAK.
//
// display_name, when non-empty, overrides the Zoom-account display name for
// this session only — used by producers who want to appear as "LDV Studio"
// or similar instead of their personal name. Empty falls back to the
// account name fetched at login.
// ---------------------------------------------------------------------------
void HandleJoinMeeting(const std::string& json) {
    LogToFile("Meeting: HandleJoinMeeting called");

    // Lazy SDK bring-up: if the Zoom SDK isn't up yet (idle, logged-in-but-
    // not-connected state), queue this join and trigger init+auth. This
    // handler re-runs once auth lands; the second time through EnsureSdkUpThen
    // returns false and we fall past it into the real join.
    if (EnsureSdkUpThen([json]() { HandleJoinMeeting(json); }))
        return;

    if (!g_meetingService) {
        LogToFile("Meeting: join requested but meeting service not initialized");
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-1,"
                     "\"message\":\"Zoom SDK is not ready. Please try logging in again.\"}");
        return;
    }

    std::string input        = JsonExtractString(json, "input");
    std::string password     = JsonExtractString(json, "password");
    std::string customName   = JsonExtractString(json, "display_name");
    std::string webinarToken = JsonExtractString(json, "webinar_token");
    // is_pmi is informational for logging; parsing is the same either way
    (void)JsonExtractString(json, "is_pmi");

    LogToFile(webinarToken.empty()
        ? "Meeting: join request received with webinar_token=empty"
        : "Meeting: join request received with webinar_token=present");

    if (input.empty()) {
        LogToFile("Meeting: join_meeting received with empty input");
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-2,"
                     "\"message\":\"No meeting number or link was provided.\"}");
        return;
    }

    // Zoom Events Hub webinars can't be joined via the Meeting SDK — the
    // event-join token in the URL path can't be carried by JoinParam, so a
    // pasted event link would otherwise get digit-scraped into a garbage
    // meeting number and fail with a misleading "Meeting not found". Reject
    // it up front (lowercase a copy since the parser's find calls below are
    // case-sensitive and this needs to match the link as typed).
    std::string inputLower = input;
    for (char& c : inputLower) if (c >= 'A' && c <= 'Z') c += 32;
    if (inputLower.find("events.zoom.us") != std::string::npos) {
        LogToFile("Meeting: rejected Zoom Events link — use the Zoom Events flow");
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-4,"
                     "\"message\":\"This looks like a Zoom Events link, which can't "
                     "be joined by pasting the URL. Use the \\\"Zoom Events\\\" button "
                     "on the Connect screen instead — it lists your events and joins "
                     "the session directly.\"}");
        return;
    }

    // Fresh ZAK, and verify display name is cached (should be, from post-auth
    // prefetch, but defensive).
    std::string zak = FetchZak();
    const std::string& displayName = GetUserDisplayName();

    if (zak.empty() || displayName.empty()) {
        LogWarn("Meeting: could not retrieve ZAK or display name");
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

    // Custom display name overrides the account name for this session.
    // Account-name guard above still applies — we don't relax it when a
    // custom name is provided, since the prefetch always completes after
    // login and a missing account name signals a real auth-state problem.
    const std::string& effectiveName =
        customName.empty() ? displayName : customName;
    if (!customName.empty()) {
        LogToFile("Meeting: using custom display name override");
    }
    static std::wstring s_userName;
    s_userName     = Utf8ToWide(effectiveName);
    param.userName = s_userName.c_str();

    // Webinar registration callbacks (if they fire) respond with this name.
    g_configurationListener.SetEffectiveDisplayName(s_userName);

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

    static std::wstring s_webinarToken;
    if (!webinarToken.empty()) {
        s_webinarToken     = Utf8ToWide(webinarToken);
        param.webinarToken = s_webinarToken.c_str();
    } else {
        param.webinarToken = nullptr;
    }

    static std::wstring s_vanityId;
    param.vanityID      = nullptr;
    param.meetingNumber = 0;

    // Parse the input — same rules as v1.0.0:
    //   - "zoom.us/my/<vanityid>" → vanity ID
    //   - "zoom.us/j/<number>"    → meeting number
    //   - "zoom.us/w/<number>"    → webinar number
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
    } else if ((pos = findSubstr(input, "zoom.us/w/")) != std::string::npos) {
        std::string rest = input.substr(pos + 10);
        size_t end = rest.find_first_of("?& \r\n");
        if (end != std::string::npos) rest = rest.substr(0, end);
        std::string digits;
        for (char c : rest) if (c >= '0' && c <= '9') digits += c;
        if (digits.empty()) {
            SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-2,"
                         "\"message\":\"Could not parse webinar number from link.\"}");
            return;
        }
        param.meetingNumber = _strtoui64(digits.c_str(), nullptr, 10);
        LogToFile("Meeting: parsed as /w/ URL");
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
        LogError(buf);
        sprintf_s(buf,
            "{\"type\":\"meeting_failed\",\"code\":%d,"
            "\"message\":\"Could not start meeting join. SDK error: %d\"}",
            (int)joinErr, (int)joinErr);
        SendToPlugin(buf);
        return;
    }

    LogInfo("Meeting: Join() call returned SUCCESS, waiting for status events");
}

// ---------------------------------------------------------------------------
// IPC handlers: Zoom Events
//   request_events       → events_list / events_auth_required
//   request_sessions     → sessions_list
//   join_event_session   → fetch JIT join token, then SDK join-by-token
// The join itself reuses the without-login JoinParam path, but token-only:
// no meeting number, no password — eventId/sessionId are a separate ID space.
// ---------------------------------------------------------------------------
void HandleRequestEvents(const std::string& /*json*/) {
    LogToFile("Events: HandleRequestEvents called");
    bool authFailed = false;
    std::string events = FetchEventsArray(authFailed);
    if (authFailed) {
        LogWarn("Events: events list returned 401/403 — Events scope not granted");
        SendToPlugin("{\"type\":\"events_auth_required\"}");
        return;
    }
    SendToPlugin("{\"type\":\"events_list\",\"events\":" + events + "}");
}

void HandleRequestSessions(const std::string& json) {
    LogToFile("Events: HandleRequestSessions called");
    std::string eventId = JsonExtractString(json, "event_id");
    if (eventId.empty()) {
        SendToPlugin("{\"type\":\"sessions_list\",\"event_id\":\"\",\"sessions\":[]}");
        return;
    }
    std::string sessions = FetchEventSessionsArray(eventId);
    SendToPlugin("{\"type\":\"sessions_list\",\"event_id\":\"" +
                 JsonEscape(eventId) + "\",\"sessions\":" + sessions + "}");
}

void HandleJoinEventSession(const std::string& json) {
    LogToFile("Events: HandleJoinEventSession called");

    // Lazy SDK bring-up — see HandleJoinMeeting. Queue and re-run after auth.
    if (EnsureSdkUpThen([json]() { HandleJoinEventSession(json); }))
        return;

    if (!g_meetingService) {
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-1,"
                     "\"message\":\"Zoom SDK is not ready. Please try logging in again.\"}");
        return;
    }

    std::string eventId    = JsonExtractString(json, "event_id");
    std::string sessionId  = JsonExtractString(json, "session_id");
    std::string customName = JsonExtractString(json, "display_name");
    if (eventId.empty() || sessionId.empty()) {
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-2,"
                     "\"message\":\"No Zoom Events session was selected.\"}");
        return;
    }

    // Just-in-time join token — fetched immediately before joining, never
    // cached (no documented TTL).
    int code = -1;
    std::string joinToken, errorMessage;
    if (!FetchEventJoinToken(eventId, sessionId, code, joinToken, errorMessage)) {
        // Map Zoom's documented join-token error codes to friendly text. We
        // never surface Zoom's raw error_message to the user — codes like 1140
        // come back as unhelpful strings like "system busy" — so the default
        // shows a generic message and the raw code/message go to the log only.
        std::string friendly;
        switch (code) {
            case 1120:
                friendly = "Zoom couldn't process the request to join this "
                           "session.";
                break;
            case 1130:
                friendly = "You don't have a valid ticket for this Zoom Events "
                           "session, so it can't be joined.";
                break;
            case 1140:
                friendly = "This Zoom Events session can't be joined — it may "
                           "have already ended, or Zoom is temporarily busy. "
                           "Try again, or pick a session that's currently live "
                           "or upcoming.";
                break;
            case 1150:
                friendly = "Your ticket for this Zoom Events session has been "
                           "revoked, so it can't be joined.";
                break;
            default:
                friendly = "This Zoom Events session couldn't be joined. "
                           "Please try again, or choose a different session.";
                break;
        }
        char logBuf[512];
        sprintf_s(logBuf,
                  "Events: join-token fetch failed, code=%d, message=%s",
                  code, errorMessage.c_str());
        LogWarn(logBuf);
        char codeBuf[32];
        sprintf_s(codeBuf, "%d", code);
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":" + std::string(codeBuf) +
                     ",\"message\":\"" + JsonEscape(friendly) + "\"}");
        return;
    }

    std::string zak = FetchZak();
    const std::string& displayName = GetUserDisplayName();
    if (zak.empty() || displayName.empty()) {
        LogWarn("Meeting: could not retrieve ZAK or display name");
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-3,"
                     "\"message\":\"Could not retrieve your Zoom account details. "
                     "Please log out and log in again.\"}");
        return;
    }

    // Build a without-login join, token-only. tagJoinParam() zero-inits every
    // field, so the ones we leave unset (meetingNumber, psw, webinarToken,
    // vanityID, ...) are already empty/null — we set them explicitly here for
    // clarity since an Events join must carry no meeting number or password.
    ZOOM_SDK_NAMESPACE::JoinParam joinParam;
    joinParam.userType = ZOOM_SDK_NAMESPACE::SDK_UT_WITHOUT_LOGIN;
    ZOOM_SDK_NAMESPACE::JoinParam4WithoutLogin& param =
        joinParam.param.withoutloginuserJoin;
    param.isAudioOff = true;
    param.isVideoOff = true;

    const std::string& effectiveName =
        customName.empty() ? displayName : customName;
    static std::wstring s_evtUserName;
    s_evtUserName  = Utf8ToWide(effectiveName);
    param.userName = s_evtUserName.c_str();
    // Webinar registration callbacks (Events sessions can be webinars) respond
    // with this name.
    g_configurationListener.SetEffectiveDisplayName(s_evtUserName);

    static std::wstring s_evtZak;
    s_evtZak      = std::wstring(zak.begin(), zak.end());
    param.userZAK = s_evtZak.c_str();

    static std::wstring s_evtJoinToken;
    s_evtJoinToken   = std::wstring(joinToken.begin(), joinToken.end());
    param.join_token = s_evtJoinToken.c_str();

    param.meetingNumber = 0;        // token-only join: no number to pass
    param.vanityID      = nullptr;
    param.psw           = nullptr;
    param.webinarToken  = nullptr;

    LogInfo("Events: joining session via join_token (token-only, no meeting number)");
    ZOOM_SDK_NAMESPACE::SDKError joinErr = g_meetingService->Join(joinParam);
    if (joinErr != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        char buf[256];
        sprintf_s(buf, "Events: Join() call failed immediately: %d", (int)joinErr);
        LogError(buf);
        sprintf_s(buf,
            "{\"type\":\"meeting_failed\",\"code\":%d,"
            "\"message\":\"Could not start Events session join. SDK error: %d\"}",
            (int)joinErr, (int)joinErr);
        SendToPlugin(buf);
        return;
    }

    LogInfo("Events: Join() call returned SUCCESS, waiting for status events");
}

// ---------------------------------------------------------------------------
// IPC handler: create_instant_meeting
// Message shape (from plugin):
//   {"type":"create_instant_meeting",
//    "display_name":"<optional session-only name override or empty>"}
//
// Two-step flow:
//   1. CreateInstantMeeting() REST call provisions a new meeting on Zoom's
//      servers and returns the meeting id + password. Consumes the
//      meeting:write:meeting OAuth scope.
//   2. IMeetingService::Start() enters that meeting as host. Uses the
//      meeting id from step 1 + the user's ZAK. The user owns the meeting
//      (they just created it), so Zoom accepts the Start() call against
//      a real meeting number without needing meeting:write at the SDK
//      layer — that scope was already consumed in step 1.
//
// Why not Join()? A freshly-created meeting is in MEETING_STATUS_WAITINGFORHOST
// until Start() runs. Calling Join() would fail with MEETING_FAIL_MEETING_NOT_START
// (code 7), or with join_before_host enabled would put the user in as a
// regular participant rather than host. Start() is the correct entry.
// ---------------------------------------------------------------------------
void HandleCreateInstantMeeting(const std::string& json) {
    LogToFile("Meeting: HandleCreateInstantMeeting called");

    // Lazy SDK bring-up — see HandleJoinMeeting. Gate before the REST
    // provisioning below so the create call only runs once the SDK is up
    // (otherwise a queued-then-re-run pass would provision twice).
    if (EnsureSdkUpThen([json]() { HandleCreateInstantMeeting(json); }))
        return;

    if (!g_meetingService) {
        LogToFile("Meeting: create requested but meeting service not initialized");
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-1,"
                     "\"message\":\"Zoom SDK is not ready. Please try logging in again.\"}");
        return;
    }

    std::string customName = JsonExtractString(json, "display_name");

    std::string zak = FetchZak();
    const std::string& displayName = GetUserDisplayName();

    if (zak.empty() || displayName.empty()) {
        LogWarn("Meeting: could not retrieve ZAK or display name");
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-3,"
                     "\"message\":\"Could not retrieve your Zoom account details. "
                     "Please log out and log in again.\"}");
        return;
    }

    // Step 1: provision the meeting via REST.
    unsigned long long meetingId = 0;
    std::string meetingPassword;
    std::string joinUrl;
    if (!CreateInstantMeeting("Feeds Instant Meeting",
                              meetingId, meetingPassword, joinUrl)) {
        LogError("Meeting: CreateInstantMeeting REST call failed");
        SendToPlugin("{\"type\":\"meeting_failed\",\"code\":-4,"
                     "\"message\":\"Could not create instant meeting. "
                     "Check the engine log for details.\"}");
        return;
    }
    // Stash for the meeting_joined IPC payload — onMeetingStatusChanged
    // reads these when MEETING_STATUS_INMEETING fires and clears them.
    g_pendingInstantJoinUrl  = joinUrl;
    g_pendingInstantPassword = meetingPassword;

    // Step 2: enter the meeting as host via SDK Start(). Storage lifetimes
    // must outlive Start() — static, distinct names from the Join() statics
    // so the two paths don't share state if called in sequence.
    ZOOM_SDK_NAMESPACE::StartParam startParam;
    startParam.userType = ZOOM_SDK_NAMESPACE::SDK_UT_WITHOUT_LOGIN;
    ZOOM_SDK_NAMESPACE::StartParam4WithoutLogin& param =
        startParam.param.withoutloginStart;
    param.isAudioOff    = true;
    param.isVideoOff    = true;
    param.zoomuserType  = ZOOM_SDK_NAMESPACE::ZoomUserType_APIUSER;
    param.meetingNumber = meetingId;   // real id from REST, NOT 0
    param.vanityID      = nullptr;
    param.customer_key  = nullptr;

    const std::string& effectiveName =
        customName.empty() ? displayName : customName;
    if (!customName.empty()) {
        LogToFile("Meeting: instant meeting using custom display name");
    }
    static std::wstring s_startUserName;
    s_startUserName = Utf8ToWide(effectiveName);
    param.userName  = s_startUserName.c_str();

    static std::wstring s_startZak;
    s_startZak    = std::wstring(zak.begin(), zak.end());
    param.userZAK = s_startZak.c_str();

    ZOOM_SDK_NAMESPACE::SDKError startErr = g_meetingService->Start(startParam);
    if (startErr != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        // Clear the pending fields so they don't leak into a subsequent
        // non-instant Join's meeting_joined payload.
        g_pendingInstantJoinUrl.clear();
        g_pendingInstantPassword.clear();

        char buf[256];
        sprintf_s(buf, "Meeting: Start() failed: %d", (int)startErr);
        LogError(buf);
        sprintf_s(buf,
            "{\"type\":\"meeting_failed\",\"code\":%d,"
            "\"message\":\"Could not start instant meeting. SDK error: %d\"}",
            (int)startErr, (int)startErr);
        SendToPlugin(buf);
        return;
    }

    LogToFile("Meeting: Start() returned SUCCESS, waiting for status events");
}

// ---------------------------------------------------------------------------
// IPC handler: leave_meeting
// ---------------------------------------------------------------------------
void HandleLeaveMeeting(const std::string& /*json*/) {
    LogInfo("Meeting: HandleLeaveMeeting called");
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
// IPC handler: send_chat_message
// Sends a public ("to all") chat message via the SDK and replies with a
// chat_send_result. Failure reasons are written as short, user-facing
// strings — the plugin shows them verbatim in a MessageBox.
// ---------------------------------------------------------------------------
// Shared reply helper used by both the pipe-thread error paths and the
// main-thread send. Posting an empty `error` with success=true is the
// success reply.
static void ReplyChatSendResult(bool success, const std::string& error) {
    std::string out;
    if (success) {
        out = "{\"type\":\"chat_send_result\",\"success\":true}";
    } else {
        out = "{\"type\":\"chat_send_result\",\"success\":false,"
              "\"error\":\"" + JsonEscape(error) + "\"}";
    }
    SendToPlugin(out);
}

// IPC handler — runs on the pipe-reader thread. Does the lightweight
// JSON parse and posts the content to the main thread via the anchor
// window. The actual SDK send happens in SendChatMessageOnMainThread.
//
// Why: the Zoom Meeting SDK requires chat sends to run on the thread
// that owns the SDK window and pumps Windows messages. Sending from
// this background thread returns SDKERR_SUCCESS and echoes locally,
// but the message never propagates to other participants. Confirmed
// empirically v1.2.4 across multiple test sessions; matches the
// pattern documented on the Zoom dev forum and by Recall.ai.
void HandleSendChatMessage(const std::string& json) {
    std::string content = JsonExtractStringEscaped(json, "content");

    if (!g_anchorWnd) {
        LogToFile("Chat: send_chat_message — no anchor window to marshal to");
        ReplyChatSendResult(false, "Internal error (no main thread)");
        return;
    }

    // Heap-allocate; window proc takes ownership and frees. PostMessage
    // returns immediately; the actual send + chat_send_result reply
    // happen on the main thread.
    std::string* heapContent = new std::string(content);
    if (!PostMessageW(g_anchorWnd, WM_FEEDS_SEND_CHAT, 0,
                      reinterpret_cast<LPARAM>(heapContent))) {
        DWORD lastErr = GetLastError();
        char buf[128];
        sprintf_s(buf, "Chat: send_chat_message — PostMessage failed err=%lu",
                  lastErr);
        LogToFile(buf);
        delete heapContent;
        ReplyChatSendResult(false, "Internal error (post failed)");
        return;
    }
}

// Main-thread send — invoked by EngineWndProc on receipt of
// WM_FEEDS_SEND_CHAT. Contains everything that touches the SDK.
// Sends the chat_send_result reply directly so the plugin gets a
// single reply regardless of which thread did the work.
void SendChatMessageOnMainThread(const std::string& content) {
    if (!g_meetingService) {
        LogToFile("Chat: send_chat_message — no meeting service");
        ReplyChatSendResult(false, "Not in a meeting");
        return;
    }
    if (g_meetingService->GetMeetingStatus() !=
        ZOOM_SDK_NAMESPACE::MEETING_STATUS_INMEETING) {
        LogToFile("Chat: send_chat_message — not in meeting");
        ReplyChatSendResult(false, "Not in a meeting");
        return;
    }

    ZOOM_SDK_NAMESPACE::IMeetingChatController* cc =
        g_meetingService->GetMeetingChatController();
    if (!cc) {
        LogToFile("Chat: send_chat_message — chat controller unavailable");
        ReplyChatSendResult(false, "Chat is unavailable in this meeting");
        return;
    }

    ZOOM_SDK_NAMESPACE::IChatMsgInfoBuilder* builder =
        cc->GetChatMessageBuilder();
    if (!builder) {
        LogToFile("Chat: send_chat_message — chat builder unavailable");
        ReplyChatSendResult(false, "Chat is unavailable in this meeting");
        return;
    }

    std::wstring wContent = Utf8ToWide(content);
    builder->SetContent(wContent.c_str());
    // Required for To_All — per Zoom SDK docs and dev forum, the message
    // is sent to all participants only when receiver is explicitly set
    // to 0. The actual cause of v1.2.4's "FDC messages don't propagate"
    // bug was the threading (this function now runs on the main thread
    // via WM_FEEDS_SEND_CHAT marshalling), but SetReceiver(0) is still
    // required per spec.
    builder->SetReceiver(0);
    builder->SetMessageType(ZOOM_SDK_NAMESPACE::SDKChatMessageType_To_All);
    ZOOM_SDK_NAMESPACE::IChatMsgInfo* msg = builder->Build();
    if (!msg) {
        LogToFile("Chat: send_chat_message — Build returned null");
        ReplyChatSendResult(false, "Failed to build chat message");
        return;
    }

    ZOOM_SDK_NAMESPACE::SDKError err = cc->SendChatMsgTo(msg);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        char buf[128];
        sprintf_s(buf, "Chat: SendChatMsgTo returned %d", (int)err);
        LogToFile(buf);
        char errBuf[96];
        sprintf_s(errBuf, "Failed to send (SDK error %d)", (int)err);
        ReplyChatSendResult(false, errBuf);
        return;
    }

    LogToFile("Chat: send_chat_message sent successfully");
    ReplyChatSendResult(true, "");
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
    LogInfo("Meeting: HandleLogout called");

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

    // The SDK just logged out, so its prior SDKAuth no longer counts as
    // "ready to connect." Reset the lazy bring-up so a subsequent login +
    // connect re-runs init+auth instead of trying to Join on a logged-out SDK.
    ResetSdkBringupState();

    SendToPlugin("{\"type\":\"logout_complete\"}");
}

} // namespace feeds_engine
