#include <winsock2.h>
#include <ws2tcpip.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <string>
#include <vector>
#include <windows.h>
#include <shellapi.h>

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
// 0 = Free (1 feed), 1 = Basic (3 feeds), 2 = Streamer (5 feeds), 3 = Broadcaster (10 feeds)
// ---------------------------------------------------------------------------
static int g_currentTier = 0;
static int g_activeParticipantSources = 0;

static int GetMaxFeedsForTier() {
    switch (g_currentTier) {
        case 1:  return 3;
        case 2:  return 5;
        case 3:  return 10;
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
static bool g_rawLiveStreamGranted = false;
static QAction* g_connectAction = nullptr;
static unsigned int g_activeSharerUserId = 0;
static unsigned int g_activeShareSourceId = 0;
static unsigned int g_activeSpeakerUserId = 0;

// ---------------------------------------------------------------------------
// PER-SOURCE VIDEO CATCHER
// ---------------------------------------------------------------------------
class ZoomVideoCatcher : public ZOOM_SDK_NAMESPACE::IZoomSDKRendererDelegate {
public:
    obs_source_t* target = nullptr;
    uint64_t next_timestamp = 0;

    virtual void onRawDataFrameReceived(YUVRawDataI420* data) override {
        if (!target) return;
        unsigned int width = data->GetStreamWidth();
        unsigned int height = data->GetStreamHeight();

        struct obs_source_frame obs_frame = {};
        obs_frame.format = VIDEO_FORMAT_I420;
        obs_frame.width = width;
        obs_frame.height = height;
        obs_frame.data[0] = (uint8_t*)data->GetYBuffer();
        obs_frame.data[1] = (uint8_t*)data->GetUBuffer();
        obs_frame.data[2] = (uint8_t*)data->GetVBuffer();
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
static obs_source_t* g_screenshareSource = nullptr;
static uint64_t g_screenshare_timestamp = 0;

class ZoomShareCatcher : public ZOOM_SDK_NAMESPACE::IZoomSDKRendererDelegate {
public:
    virtual void onRawDataFrameReceived(YUVRawDataI420* data) override {
        if (!g_screenshareSource) return;
        unsigned int width = data->GetStreamWidth();
        unsigned int height = data->GetStreamHeight();

        struct obs_source_frame obs_frame = {};
        obs_frame.format = VIDEO_FORMAT_I420;
        obs_frame.width = width;
        obs_frame.height = height;
        obs_frame.data[0] = (uint8_t*)data->GetYBuffer();
        obs_frame.data[1] = (uint8_t*)data->GetUBuffer();
        obs_frame.data[2] = (uint8_t*)data->GetVBuffer();
        obs_frame.linesize[0] = width;
        obs_frame.linesize[1] = width / 2;
        obs_frame.linesize[2] = width / 2;

        video_format_get_parameters(VIDEO_CS_DEFAULT, VIDEO_RANGE_PARTIAL,
                                    obs_frame.color_matrix,
                                    obs_frame.color_range_min,
                                    obs_frame.color_range_max);

        uint64_t now = os_gettime_ns();
        if (g_screenshare_timestamp == 0 || g_screenshare_timestamp < now - 100000000ULL)
            g_screenshare_timestamp = now;
        obs_frame.timestamp = g_screenshare_timestamp;
        g_screenshare_timestamp += 33333333ULL;
        obs_source_output_video(g_screenshareSource, &obs_frame);
    }
    virtual void onRawDataStatusChanged(RawDataStatus status) override {}
    virtual void onRendererBeDestroyed() override {}
};

static ZoomShareCatcher g_shareCatcher;
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
    obs_source_t* source = nullptr;
    unsigned int current_user_id = 0;
    ZoomVideoCatcher videoCatcher;
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
                int index = (int)std::distance(g_allParticipantSources.begin(), it);
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
        unsigned int selected_id = (unsigned int)obs_data_get_int(settings,
                                                                   "participant_id");
        current_user_id = selected_id;
        if (!videoRenderer && g_rawLiveStreamGranted)
            initRenderer();
        if (videoRenderer) {
            videoRenderer->unSubscribe();
            unsigned int effectiveId = (current_user_id == 1)
                ? g_activeSpeakerUserId : current_user_id;
            if (effectiveId != 0)
                videoRenderer->subscribe(effectiveId,
                                         ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_VIDEO);
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
    virtual void onJoin3rdPartyTelephonyAudio(const zchar_t* audioInfo) override {}
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
                g_activeSharerUserId = shareInfo.userid;
                g_activeShareSourceId = shareInfo.shareSourceID;
                UpdateShareSubscription();
                break;
            case ZOOM_SDK_NAMESPACE::Sharing_Other_Share_End:
            case ZOOM_SDK_NAMESPACE::Sharing_Self_Send_End:
                g_activeSharerUserId = 0;
                g_activeShareSourceId = 0;
                UpdateShareSubscription();
                break;
            default:
                break;
        }
    }
    virtual void onFailedToStartShare() override {}
    virtual void onLockShareStatus(bool bLocked) override {}
    virtual void onShareContentNotification(
        ZOOM_SDK_NAMESPACE::ZoomSDKSharingSourceInfo shareInfo) override {}
    virtual void onMultiShareSwitchToSingleShareNeedConfirm(
        ZOOM_SDK_NAMESPACE::IShareSwitchMultiToSingleConfirmHandler* handler_) override {}
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
class ZoomLiveStreamListener : public ZOOM_SDK_NAMESPACE::IMeetingLiveStreamCtrlEvent {
public:
    virtual void onRawLiveStreamPrivilegeChanged(bool bHasPrivilege) override {
        if (!bHasPrivilege) return;

        g_rawLiveStreamGranted = true;

        if (g_connectAction)
            g_connectAction->setEnabled(false);

        ZOOM_SDK_NAMESPACE::IMeetingService* ms = nullptr;
        ZOOM_SDK_NAMESPACE::CreateMeetingService(&ms);
        if (!ms) return;

        ZOOM_SDK_NAMESPACE::IMeetingLiveStreamController* lsc =
            ms->GetMeetingLiveStreamController();
        if (!lsc) return;

        lsc->StartRawLiveStreaming(
            L"https://letsdovideo.com/feeds-support/",
            L"Feeds");

        ZOOM_SDK_NAMESPACE::IMeetingShareController* sc =
            ms->GetMeetingShareController();
        if (sc) {
            sc->SetEvent(&g_shareListener);
            ZOOM_SDK_NAMESPACE::IList<unsigned int>* sharingUsers =
                sc->GetViewableSharingUserList();
            if (sharingUsers && sharingUsers->GetCount() > 0) {
                unsigned int sharingUserId = sharingUsers->GetItem(0);
                g_activeSharerUserId = sharingUserId;
                ZOOM_SDK_NAMESPACE::IList<ZOOM_SDK_NAMESPACE::ZoomSDKSharingSourceInfo>* sourceList =
                    sc->GetSharingSourceInfoList(sharingUserId);
                if (sourceList && sourceList->GetCount() > 0)
                    g_activeShareSourceId = sourceList->GetItem(0).shareSourceID;
            }
        }

        ZOOM_SDK_NAMESPACE::IMeetingAudioController* ac =
            ms->GetMeetingAudioController();
        if (ac)
            ac->SetEvent(&g_audioListener);

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
    virtual void onUserRawLiveStreamPrivilegeChanged(unsigned int userid,
                                                      bool bHasPrivilege) override {}
    virtual void onRawLiveStreamPrivilegeRequested(
        ZOOM_SDK_NAMESPACE::IRequestRawLiveStreamPrivilegeHandler* handler) override {}
    virtual void onUserRawLiveStreamingStatusChanged(
        ZOOM_SDK_NAMESPACE::IList<ZOOM_SDK_NAMESPACE::RawLiveStreamInfo>* liveStreamList) override {}
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
    virtual void onMeetingStatusChanged(ZOOM_SDK_NAMESPACE::MeetingStatus status,
                                         int iResult = 0) override {
        if (status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_INMEETING) {
            ZOOM_SDK_NAMESPACE::IMeetingService* ms = nullptr;
            ZOOM_SDK_NAMESPACE::CreateMeetingService(&ms);
            if (!ms) return;

            ZOOM_SDK_NAMESPACE::IMeetingLiveStreamController* lsc =
                ms->GetMeetingLiveStreamController();
            if (!lsc) return;

            lsc->SetEvent(&g_liveStreamListener);

            if (lsc->CanStartRawLiveStream() == ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
                g_liveStreamListener.onRawLiveStreamPrivilegeChanged(true);
            } else {
                lsc->RequestRawLiveStreaming(
                    L"https://letsdovideo.com/feeds-support/",
                    L"Feeds");
            }
        }

        if (status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_ENDED ||
            status == ZOOM_SDK_NAMESPACE::MEETING_STATUS_DISCONNECTING) {
            g_rawLiveStreamGranted = false;
            g_activeSharerUserId = 0;
            g_activeShareSourceId = 0;
            g_activeSpeakerUserId = 0;
            if (g_connectAction)
                g_connectAction->setEnabled(true);
            if (g_shareRenderer) {
                g_shareRenderer->unSubscribe();
                ZOOM_SDK_NAMESPACE::destroyRenderer(g_shareRenderer);
                g_shareRenderer = nullptr;
            }
        }
    }
    virtual void onMeetingStatisticsWarningNotification(
        ZOOM_SDK_NAMESPACE::StatisticsWarningType type) override {}
    virtual void onMeetingParameterNotification(
        const ZOOM_SDK_NAMESPACE::MeetingParameter* meeting_param) override {}
    virtual void onSuspendParticipantsActivities() override {}
    virtual void onAICompanionActiveChangeNotice(bool bActive) override {}
    virtual void onMeetingTopicChanged(const zchar_t* sTopic) override {}
    virtual void onMeetingFullToWatchLiveStream(const zchar_t* sLiveStreamUrl) override {}
    virtual void onUserNetworkStatusChanged(ZOOM_SDK_NAMESPACE::MeetingComponentType type,
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
    virtual void onAuthenticationReturn(ZOOM_SDK_NAMESPACE::AuthResult ret) override {
        if (ret != ZOOM_SDK_NAMESPACE::AUTHRET_SUCCESS) return;

        ZOOM_SDK_NAMESPACE::IMeetingService* ms = nullptr;
        ZOOM_SDK_NAMESPACE::CreateMeetingService(&ms);
        if (!ms) return;
        ms->SetEvent(&g_meetingListener);

        QMainWindow* mainWindow = (QMainWindow*)obs_frontend_get_main_window();

        bool ok = false;
        QString input = QInputDialog::getText(
            mainWindow,
            "Join Zoom Meeting",
            "Enter your Zoom Meeting number or link:",
            QLineEdit::Normal,
            "",
            &ok);
        if (!ok || input.trimmed().isEmpty()) return;

        bool okPwd = false;
        QString password = QInputDialog::getText(
            mainWindow,
            "Meeting Password",
            "Enter meeting password (leave blank if none):",
            QLineEdit::Normal,
            "",
            &okPwd);
        if (!okPwd) return;

        input = input.trimmed();

        ZOOM_SDK_NAMESPACE::JoinParam joinParam;
        joinParam.userType = ZOOM_SDK_NAMESPACE::SDK_UT_WITHOUT_LOGIN;
        ZOOM_SDK_NAMESPACE::JoinParam4WithoutLogin& param = joinParam.param.withoutloginuserJoin;
        param.isAudioOff = true;
        param.isVideoOff = true;
        param.userName = L"Feeds";

        static std::wstring s_password;
        if (!password.trimmed().isEmpty()) {
            s_password = password.trimmed().toStdWString();
            param.psw = s_password.c_str();
        }

        static std::wstring s_vanityId;

        if (input.contains("zoom.us/my/")) {
            int start = input.indexOf("zoom.us/my/") + 11;
            QString vanityId = input.mid(start).split(
                QRegularExpression("[?\\s]")).first();
            s_vanityId = vanityId.toStdWString();
            param.vanityID = s_vanityId.c_str();
        } else if (input.contains("zoom.us/j/")) {
            int start = input.indexOf("zoom.us/j/") + 10;
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
    }

    virtual void onLoginReturnWithReason(ZOOM_SDK_NAMESPACE::LOGINSTATUS ret,
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
// CONNECT HELPER
// ---------------------------------------------------------------------------
void OnConnectClick() {
    ZOOM_SDK_NAMESPACE::IAuthService* auth_service = nullptr;
    if (ZOOM_SDK_NAMESPACE::CreateAuthService(&auth_service) != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS
        || !auth_service)
        return;
    auth_service->SetEvent(&g_authListener);
    ZOOM_SDK_NAMESPACE::AuthContext authContext;
    authContext.jwt_token = L"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJhcHBLZXkiOiJZNzNqelFSbVF4aWhoNFo3MnFSMnRnIiwiaWF0IjoxNzc0MDUwMDAwLCJleHAiOjQxMDI0NDQ4MDAsInRva2VuRXhwIjo0MTAyNDQ0ODAwLCJyb2xlIjoxLCJ1c2VyRW1haWwiOiJEYXZpZEBMZXRzRG9WaWRlby5jb20ifQ.8Hgz4-urKak3PlemhvlsysNFV6J2KmJgkP8Wn6MDn90";
    auth_service->SDKAuth(authContext);
    StartPostConnectRefreshTimer();
}

// ---------------------------------------------------------------------------
// MENU SETUP
// ---------------------------------------------------------------------------
void SetupPluginMenu(void) {
    QMainWindow* mainWindow = (QMainWindow*)obs_frontend_get_main_window();
    QMenuBar* menuBar = mainWindow->menuBar();
    QMenu* isoMenu = new QMenu("Feeds", menuBar);
    menuBar->addMenu(isoMenu);
    g_connectAction = isoMenu->addAction("Connect to Zoom...");
    QAction* aboutAction = isoMenu->addAction("About / Tier Status");
    QObject::connect(g_connectAction, &QAction::triggered, []() { OnConnectClick(); });
    QObject::connect(aboutAction, &QAction::triggered, []() {
        MessageBoxA(NULL, "Feeds v0.1\nTier: Free\nStatus: Active",
                    "About", MB_OK);
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
        obs_properties_add_button(props, "connect_btn",
            "Not connected to Zoom. Click to Connect...",
            [](obs_properties_t*, obs_property_t*, void*) -> bool {
                OnConnectClick();
                return true;
            });
    } else {
        std::string status_text = "Status: Connected";
        ZOOM_SDK_NAMESPACE::IMeetingInfo* info = ms->GetMeetingInfo();
        if (info)
            status_text = "Status: Connected to Meeting " +
                          std::to_string(info->GetMeetingNumber());
        obs_properties_add_text(props, "status_label", status_text.c_str(),
                                OBS_TEXT_INFO);
        obs_properties_add_button(props, "refresh_btn",
            "Refresh Participant List",
            [](obs_properties_t*, obs_property_t*, void*) -> bool {
                return true;
            });
    }

    ZoomParticipantSource* src = static_cast<ZoomParticipantSource*>(data);
    if (src && g_rawLiveStreamGranted && !src->videoRenderer)
        src->initRenderer();

    obs_property_t* list = obs_properties_add_list(props, "participant_id",
                                                    "Select Participant",
                                                    OBS_COMBO_TYPE_LIST,
                                                    OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(list, "--- Select Participant ---", 0);
    obs_property_list_add_int(list, "[Active Speaker]", 1);

    if (ms) {
        ZOOM_SDK_NAMESPACE::IMeetingParticipantsController* pc =
            ms->GetMeetingParticipantsController();
        if (pc) {
            ZOOM_SDK_NAMESPACE::IList<unsigned int>* userList = pc->GetParticipantsList();
            if (userList) {
                for (int i = 0; i < userList->GetCount(); i++) {
                    unsigned int uid = userList->GetItem(i);
                    ZOOM_SDK_NAMESPACE::IUserInfo* info = pc->GetUserByUserID(uid);
                    if (info) {
                        std::wstring wname = info->GetUserName();
                        if (wname == L"Feeds") continue;
                        int size_needed = WideCharToMultiByte(
                            CP_UTF8, 0, &wname[0], (int)wname.size(),
                            NULL, 0, NULL, NULL);
                        std::string name(size_needed, 0);
                        WideCharToMultiByte(CP_UTF8, 0, &wname[0], (int)wname.size(),
                                            &name[0], size_needed, NULL, NULL);
                        obs_property_list_add_int(list, name.c_str(), (long long)uid);
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
        obs_properties_add_button(props, "connect_btn",
            "Not connected to Zoom. Click to Connect...",
            [](obs_properties_t*, obs_property_t*, void*) -> bool {
                OnConnectClick();
                return true;
            });
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
OBS_MODULE_USE_DEFAULT_LOCALE("iso-for-obs", "en-US")

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
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
            SetupPluginMenu();
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
